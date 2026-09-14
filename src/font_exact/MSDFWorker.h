#pragma once
#include "MSDF.h"
#include "MSDFUtils.h"
#include <memory>
#include <vector>

// [1.12] Glyph generation off the rendering thread.
//
// Generating one MSDF glyph at SDF_RENDER_SIZE costs 7-11 ms for a Latin glyph
// and ~52 ms (up to 130 ms) for a CJK one - measured on the x86 build of these
// libraries, see docs/chat-stutter.md. MSDFFont::GetGlyph used to do it inline, on
// the client's rendering thread, the first time a glyph was drawn: every chat line
// with characters the cache had not seen froze the game for tens to hundreds of
// milliseconds. The client now gets nothing for such a glyph until a worker has
// made it (the quad is hidden and the string is laid out again afterwards).

// Font bytes owned by Lexara. A job can outlive the client's FT_Face - the client
// recreates its faces in bursts - and the buffer the client handed to
// FT_New_Memory_Face is freed together with the face, so a worker never reads it.
struct FontBlob {
    FontHash hash = 0;
    std::vector<uint8_t> bytes;
};

namespace MSDFWorker {
    struct Result {
        FontHash hash = 0;
        GlyphMetricsToStore glyph;
    };

    // Rendering thread only. Queues the glyph unless the same codepoint of the same
    // file is already queued or being generated.
    void Request(const std::shared_ptr<const FontBlob>& font, uint32_t codepoint);

    // Rendering thread only. Moves every finished glyph into `out` (which must be
    // empty); returns false when there was none.
    bool Drain(std::vector<Result>& out);

    // Lock-free: something is waiting to be drained.
    bool HasResults();

    // Nothing queued, nothing being generated, nothing waiting to be drained.
    bool Idle();
}
