#include "MSDFWorker.h"
#include "MSDFFont.h"
#include "../Logger.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {
    struct GlyphKey {
        FontHash hash;
        uint32_t codepoint;
        bool operator==(const GlyphKey& other) const { return hash == other.hash && codepoint == other.codepoint; }
    };

    struct GlyphKeyHash {
        using is_avalanching = void;
        uint64_t operator()(const GlyphKey& k) const noexcept {
            // splitmix64 finaliser over the two fields
            uint64_t x = k.hash ^ (static_cast<uint64_t>(k.codepoint) * 0x9E3779B97F4A7C15ULL);
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
            return x ^ (x >> 31);
        }
    };

    struct Job {
        std::shared_ptr<const FontBlob> font;
        uint32_t codepoint = 0;
        bool refine = false;
    };

    // Codepoints end at 0x10FFFF, so the top bit is free to tell a refinement apart.
    constexpr uint32_t REFINE_BIT = 0x80000000u;

    GlyphKey KeyOf(FontHash hash, uint32_t codepoint, bool refine) {
        return { hash, codepoint | (refine ? REFINE_BIT : 0u) };
    }

    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<Job> queue;         // SDF glyphs - what a hidden quad is waiting for
        std::deque<Job> refineQueue;   // MSDF refinements, taken only when `queue` is empty
        // Queued, being generated, or finished and not drained yet. It used to be
        // cleared when a worker finished, and a string laid out again before the
        // rendering thread drained the result asked for the glyph a second time.
        ankerl::unordered_dense::set<GlyphKey, GlyphKeyHash> inFlight;
        std::vector<MSDFWorker::Result> done;
        std::atomic<uint32_t> outstanding{ 0 };                           // requested and not yet drained
        std::atomic<bool> hasResults{ false };
        bool started = false;
    };

    // Deliberately never destroyed. At process exit the workers are killed wherever
    // they happen to be, possibly inside the mutex, and a static destructor running
    // after that would touch it.
    State& S() {
        static State* state = new State();
        return *state;
    }

    struct LoadedFont {
        std::shared_ptr<const FontBlob> font;
        FT_Face face = nullptr;
        msdfgen::FontHandle* handle = nullptr;
    };

    constexpr size_t MAX_LOADED_FONTS = 4;

    void Unload(LoadedFont& f) {
        if (f.handle) msdfgen::destroyFont(f.handle);
        if (f.face) FT_Done_Face(f.face);
        f = {};
    }

    void WorkerMain() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        // A library of its own: FreeType objects must not be shared between threads,
        // and the client's library and faces belong to the rendering thread.
        FT_Library library = nullptr;
        if (FT_Init_FreeType(&library) != 0) {
            library = nullptr;
            Log("[MSDF] worker: FT_Init_FreeType failed - its jobs come back as empty glyphs");
        }

        std::vector<LoadedFont> loaded;
        State& s = S();

        for (;;) {
            Job job;
            {
                std::unique_lock lock(s.mutex);
                if (s.queue.empty() && s.refineQueue.empty()) {
                    // Idle: drop the faces of files nobody but this worker keeps alive.
                    lock.unlock();
                    for (auto it = loaded.begin(); it != loaded.end();) {
                        if (it->font.use_count() == 1) { Unload(*it); it = loaded.erase(it); }
                        else ++it;
                    }
                    lock.lock();
                    s.wake.wait(lock, [&s] { return !s.queue.empty() || !s.refineQueue.empty(); });
                }
                std::deque<Job>& from = !s.queue.empty() ? s.queue : s.refineQueue;
                job = std::move(from.front());
                from.pop_front();
            }

            LoadedFont* font = nullptr;
            for (LoadedFont& f : loaded) {
                if (f.font->hash == job.font->hash) { font = &f; break; }
            }
            if (!font) {
                if (loaded.size() >= MAX_LOADED_FONTS) {
                    Unload(loaded.front());
                    loaded.erase(loaded.begin());
                }
                LoadedFont f;
                f.font = job.font;
                if (library && FT_New_Memory_Face(library, f.font->bytes.data(),
                        static_cast<FT_Long>(f.font->bytes.size()), 0, &f.face) == 0) {
                    f.handle = msdfgen::adoptFreetypeFont(f.face);
                }
                else {
                    f.face = nullptr;
                }
                loaded.push_back(std::move(f));
                font = &loaded.back();
            }

            MSDFWorker::Result result;
            result.hash = job.font->hash;
            result.refine = job.refine;
            result.glyph.codepoint = job.codepoint;
            // An empty result is still a result: it is cached as an empty glyph, so
            // a glyph that cannot be made is not asked for again and again.
            if (font->face && font->handle) {
                MSDFFont::BuildGlyph(font->face, font->handle, job.codepoint, result.glyph, job.refine);
            }
            job.font.reset();

            {
                std::lock_guard lock(s.mutex);
                s.done.push_back(std::move(result));
                s.hasResults.store(true, std::memory_order_relaxed);
            }
        }
    }

    void StartWorkers() {
        const unsigned hw = std::thread::hardware_concurrency();
        // Below normal priority, and half the hardware threads minus two for the
        // client's own thread, DXVK's and the driver's: 8 threads -> 2 workers,
        // 12 -> 4, 16 and up -> 6. Until a worker finishes it a glyph is not drawn at
        // all; hw / 4 capped at three gave the CJK reporter's 16-thread machine three
        // workers for a chat full of new hanzi, back when each took ~52 ms as an MSDF.
        const unsigned count = std::clamp(hw > 4 ? hw / 2 - 2 : 1u, 1u, 6u);
        for (unsigned i = 0; i < count; ++i) std::thread(WorkerMain).detach();
        Log("[MSDF] glyph workers started: %u (hardware threads %u)", count, hw);
    }
}

void MSDFWorker::Request(const std::shared_ptr<const FontBlob>& font, uint32_t codepoint, bool refine) {
    if (!font) return;
    State& s = S();
    {
        std::lock_guard lock(s.mutex);
        if (!s.inFlight.emplace(KeyOf(font->hash, codepoint, refine)).second) return;
        (refine ? s.refineQueue : s.queue).push_back({ font, codepoint, refine });
        s.outstanding.fetch_add(1, std::memory_order_relaxed);
        if (!s.started) {
            s.started = true;
            StartWorkers();
        }
    }
    s.wake.notify_one();
}

bool MSDFWorker::Drain(std::vector<Result>& out) {
    State& s = S();
    if (!s.hasResults.load(std::memory_order_relaxed)) return false;
    {
        std::lock_guard lock(s.mutex);
        out.swap(s.done);
        s.hasResults.store(false, std::memory_order_relaxed);
        for (const Result& r : out) s.inFlight.erase(KeyOf(r.hash, r.glyph.codepoint, r.refine));
    }
    s.outstanding.fetch_sub(static_cast<uint32_t>(out.size()), std::memory_order_relaxed);
    return !out.empty();
}

bool MSDFWorker::HasResults() {
    return S().hasResults.load(std::memory_order_relaxed);
}

bool MSDFWorker::Idle() {
    return S().outstanding.load(std::memory_order_relaxed) == 0;
}
