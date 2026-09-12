#include "MSDFManager.h"
#include "MSDFCache.h"

#pragma comment(lib, "onecore.lib")

MSDFManager::MSDFManager() {
    GetSystemInfo(&s_si);
}

MSDFManager::~MSDFManager() {
    FlushAll();
}

void MSDFManager::MappedBlock::Close() {
    view.Close();
    mapping.Close();
    file.Close();
    header = nullptr;
    entries = nullptr;
    hashTable = nullptr;
    payload = nullptr;
    entryCount = 0;
}

MSDFManager::ArenaState::ArenaState() {
    // No IS_WIN10 gate and no allocation here on purpose. This constructor runs
    // during the DLL's static initialisation, where the order against MSDF.h's
    // inline `IS_WIN10` is unspecified across translation units; the check now sits
    // in ReserveSlot, which cannot run before DllMain. The arrays are filled
    // unconditionally - the old early return left slotAddresses uninitialised and
    // GetFreeSlot then handed out garbage pointers on anything below Windows 10.
    constexpr uint32_t maxGlyphDim = MSDF::SDF_RENDER_SIZE + 2 * MSDF::SDF_SPREAD;
    constexpr uint32_t maxPixelsPerGlyph = maxGlyphDim * maxGlyphDim;
    constexpr uint32_t maxBytesPerGlyph = maxPixelsPerGlyph * 4;

    constexpr size_t maxPayload = MSDFCache::BLOCK_SIZE * maxBytesPerGlyph;
    constexpr size_t maxEntries = MSDFCache::BLOCK_SIZE * sizeof(MSDFCache::GlyphEntry);
    constexpr size_t maxHashTable = MSDFCache::BLOCK_SIZE * sizeof(uint32_t);
    constexpr size_t maxBlockSize = sizeof(MSDFCache::BlockFileHeader) + maxEntries + maxHashTable + maxPayload;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t gran = si.dwAllocationGranularity;
    effectiveSlotSize = ((maxBlockSize + gran - 1) / gran) * gran;

    slotAddresses.fill(nullptr);
    slotReservedSize.fill(0);
    slotToBlockIndex.fill(0xFFFFFFFF);
}

MSDFManager::ArenaState::~ArenaState() {
    for (size_t i = 0; i < MAX_ARENA_SLOTS; ++i) {
        if (slotAddresses[i]) {
            VirtualFreeEx(GetCurrentProcess(), slotAddresses[i], 0, MEM_RELEASE);
            slotAddresses[i] = nullptr;
            slotReservedSize[i] = 0;
        }
    }
}

bool MSDFManager::ArenaState::ClaimSlot(uint32_t blockIndex, uint32_t& outSlotIndex) {
    if (freeMask == 0) return false;
    uint32_t slotIdx = static_cast<uint32_t>(std::countr_zero(freeMask));
    freeMask &= ~(1ULL << slotIdx);
    slotToBlockIndex[slotIdx] = blockIndex;
    outSlotIndex = slotIdx;
    return true;
}

// `bytes` is the block file's size rounded up to the allocation granularity, so
// the placeholder is exactly the size of the view that replaces it - no split
// with MEM_PRESERVE_PLACEHOLDER is needed any more, and no tail is left reserved.
void* MSDFManager::ArenaState::ReserveSlot(uint32_t slotIndex, size_t bytes) {
    if (!MSDF::IS_WIN10) return nullptr;
    if (slotIndex >= MAX_ARENA_SLOTS || bytes == 0 || bytes > effectiveSlotSize) return nullptr;
    if (slotAddresses[slotIndex]) return nullptr;

    void* addr = VirtualAlloc2(GetCurrentProcess(), nullptr, bytes,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
        PAGE_NOACCESS, nullptr, 0);
    if (!addr) return nullptr;

    slotAddresses[slotIndex] = addr;
    slotReservedSize[slotIndex] = bytes;
    return addr;
}

void MSDFManager::ArenaState::FreeSlot(uint32_t slotIndex) {
    if (slotIndex >= MAX_ARENA_SLOTS || !IsSlotOccupied(slotIndex)) return;

    // A slot can be occupied without holding a reservation: ClaimSlot takes the
    // index, and LoadMappedBlock can fail (missing file, bad header) before
    // ReserveSlot ever runs.
    if (void* slotAddr = slotAddresses[slotIndex]) {
        uintptr_t currentAddr = reinterpret_cast<uintptr_t>(slotAddr);
        const uintptr_t endAddr = currentAddr + slotReservedSize[slotIndex];

        while (currentAddr < endAddr) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(reinterpret_cast<void*>(currentAddr), &mbi, sizeof(mbi)) == 0) break;
            if (reinterpret_cast<uintptr_t>(mbi.BaseAddress) >= endAddr) break;

            if (mbi.State != MEM_FREE) {
                if (mbi.Type == MEM_MAPPED) {
                    UnmapViewOfFile2(GetCurrentProcess(), mbi.BaseAddress, 0);
                }
                else {
                    VirtualFree(mbi.BaseAddress, 0, MEM_RELEASE);
                }
            }
            currentAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        }

        // The address space goes back to the OS instead of being re-reserved as a
        // placeholder - that is the whole point of the lazy arena.
        slotAddresses[slotIndex] = nullptr;
        slotReservedSize[slotIndex] = 0;
    }

    freeMask |= (1ULL << slotIndex);

    slotToBlockIndex[slotIndex] = 0xFFFFFFFF;
}

void MSDFManager::ArenaState::FlushAll() {
    for (uint32_t i = 0; i < MAX_ARENA_SLOTS; ++i) {
        if (IsSlotOccupied(i)) FreeSlot(i);
    }
}

void MSDFManager::FreeBlock(uint32_t blockIndex) {
    if (blockIndex >= MAX_ARENA_SLOTS) return;

    MappedBlock& block = s_mappedBlocks[blockIndex];
    MSDFCache::BlockKey keyToErase = block.key;

    if (block.slotIndex != 0xFFFFFFFF) {
        s_arena.FreeSlot(block.slotIndex);
    }
    block.Reset();
    s_blockCache.erase(keyToErase);

    if (s_lastBlockIndex == blockIndex) {
        s_lastBlockIndex = 0xFFFFFFFF;
        s_lastBlockKey = {};
    }
}

void MSDFManager::FreeBlockByKey(MSDFCache::BlockKey key) {
    auto it = s_blockCache.find(key);
    if (it == s_blockCache.end()) return;
    FreeBlock(it->second);
}

void MSDFManager::FlushAll() {
    for (auto& block : s_mappedBlocks) {
        block.Reset();
    }
    s_blockCache.clear();
    s_arena.FlushAll();

    s_lastBlockIndex = 0xFFFFFFFF;
    s_lastBlockKey = {};
}

uint32_t MSDFManager::RegisterFont(FontHash hash) {
    auto it = s_fontHashToId.find(hash);
    if (it != s_fontHashToId.end()) {
        return it->second;
    }

    uint32_t fontId = s_nextFontId++;
    s_fontHashToId.emplace(hash, fontId);
    s_fontIdToHash.emplace(fontId, hash);
    return fontId;
}

FontHash MSDFManager::GetFontHash(uint32_t fontId) {
    auto it = s_fontIdToHash.find(fontId);
    return (it != s_fontIdToHash.end()) ? it->second : 0;
}

bool MSDFManager::LoadMappedBlock(const MSDFCache::BlockWrap& wrap, MappedBlock& outBlock, uint32_t slotIndex) {
    outBlock.file.handle = CreateFileW(wrap.path.native().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (outBlock.file.handle == INVALID_HANDLE_VALUE) {
        s_arena.FreeSlot(slotIndex);
        return false;
    }

    LARGE_INTEGER fileSizeLI;
    if (!GetFileSizeEx(outBlock.file.handle, &fileSizeLI)) {
        s_arena.FreeSlot(slotIndex);
        return false;
    }
    outBlock.fileSize = static_cast<uint64_t>(fileSizeLI.QuadPart);

    const size_t allocGran = s_si.dwAllocationGranularity;
    uint64_t splitSize = ((outBlock.fileSize + allocGran - 1) / allocGran) * allocGran;
    if (splitSize < allocGran) splitSize = allocGran;
    if (splitSize > s_arena.SlotSize()) {
        s_arena.FreeSlot(slotIndex);
        return false;
    }
    outBlock.slotIndex = slotIndex;

    // The reservation happens HERE, not in the arena's constructor, and only for
    // the bytes this block actually needs.
    void* slotAddr = s_arena.ReserveSlot(slotIndex, static_cast<size_t>(splitSize));
    if (!slotAddr) {
        s_arena.FreeSlot(slotIndex);
        return false;
    }

    DWORD sizeHigh = static_cast<DWORD>(splitSize >> 32);
    DWORD sizeLow = static_cast<DWORD>(splitSize & 0xFFFFFFFF);
    outBlock.mapping.handle = CreateFileMappingW(outBlock.file.handle, nullptr, PAGE_READONLY, sizeHigh, sizeLow, nullptr);
    if (!outBlock.mapping.handle) {
        s_arena.FreeSlot(slotIndex);
        return false;
    }

    outBlock.view.ptr = MapViewOfFile3(outBlock.mapping.handle, nullptr, slotAddr, 0, splitSize,
        MEM_REPLACE_PLACEHOLDER, PAGE_READONLY, nullptr, 0);
    if (!outBlock.view.ptr) {
        s_arena.FreeSlot(slotIndex);
        return false;
    }

    outBlock.header = static_cast<const MSDFCache::BlockFileHeader*>(outBlock.view.ptr);
    if (outBlock.header->magic != MSDFCache::BLOCK_MAGIC ||
        outBlock.header->version != MSDFCache::CACHE_VERSION ||
        outBlock.header->blockId != wrap.key.blockId ||
        outBlock.header->entryCount > MSDFCache::BLOCK_SIZE) {
        s_arena.FreeSlot(slotIndex);
        return false;
    }

    outBlock.entryCount = outBlock.header->entryCount;
    outBlock.entries = reinterpret_cast<const MSDFCache::GlyphEntry*>(
        static_cast<const uint8_t*>(outBlock.view.ptr) + sizeof(MSDFCache::BlockFileHeader));

    size_t hashTableOffset = sizeof(MSDFCache::BlockFileHeader) +
        static_cast<size_t>(outBlock.entryCount) * sizeof(MSDFCache::GlyphEntry);
    outBlock.hashTable = reinterpret_cast<const uint32_t*>(
        static_cast<const uint8_t*>(outBlock.view.ptr) + hashTableOffset);

    size_t payloadOffset = hashTableOffset + (MSDFCache::BLOCK_SIZE * sizeof(uint32_t));
    if (outBlock.fileSize < payloadOffset) {
        s_arena.FreeSlot(slotIndex);
        return false;
    }
    outBlock.payload = static_cast<const uint8_t*>(outBlock.view.ptr) + payloadOffset;

    size_t maxPayload = static_cast<size_t>(outBlock.fileSize - payloadOffset);
    for (uint32_t i = 0; i < outBlock.entryCount; ++i) {
        const MSDFCache::GlyphEntry& e = outBlock.entries[i];
        if (e.dataSize > 0) {
            if (e.dataOffset + e.dataSize > maxPayload) {
                s_arena.FreeSlot(slotIndex);
                return false;
            }
        }
    }
    outBlock.key = wrap.key;

    return true;
}

MSDFManager::MappedBlock* MSDFManager::GetOrLoadMappedBlock(const MSDFCache::BlockWrap& wrap) {
    if (s_lastBlockIndex != 0xFFFFFFFF && s_lastBlockKey == wrap.key) {
        return &s_mappedBlocks[s_lastBlockIndex];
    }

    auto it = s_blockCache.find(wrap.key);
    if (it != s_blockCache.end()) {
        s_lastBlockIndex = it->second;
        s_lastBlockKey = wrap.key;
        return &s_mappedBlocks[it->second];
    }
    if (s_arena.freeMask == 0) FlushAll();

    uint32_t slotIndex = 0;
    if (!s_arena.ClaimSlot(wrap.key.blockId, slotIndex)) return nullptr;

    MappedBlock& newBlock = s_mappedBlocks[slotIndex];
    if (!LoadMappedBlock(wrap, newBlock, slotIndex)) return nullptr;

    s_lastBlockIndex = slotIndex;
    s_lastBlockKey = wrap.key;

    s_blockCache[wrap.key] = slotIndex;
    return &newBlock;
}

bool MSDFManager::LoadGlyph(const MSDFCache::BlockWrap& wrap, uint32_t codepoint, GlyphMetrics& outMetrics) {
    MappedBlock* blockPtr = GetOrLoadMappedBlock(wrap);
    if (!blockPtr) return false;

    uint32_t entryIndex = blockPtr->hashTable[codepoint & (MSDFCache::BLOCK_SIZE - 1)];
    if (entryIndex == 0xFFFFFFFF || entryIndex >= blockPtr->entryCount) return false;

    const MSDFCache::GlyphEntry& ge = blockPtr->entries[entryIndex];
    if (ge.codepoint != codepoint) return false;

    outMetrics.width = ge.width;
    outMetrics.height = ge.height;
    outMetrics.bitmapTop = ge.bitmapTop;
    outMetrics.bitmapLeft = ge.bitmapLeft;
    outMetrics.pixelData = ge.dataSize > 0 ? blockPtr->payload + ge.dataOffset : nullptr;

    return true;
}
