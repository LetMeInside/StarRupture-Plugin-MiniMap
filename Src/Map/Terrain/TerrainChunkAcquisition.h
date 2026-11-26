// Shared synchronous BC1/CPU image acquisition, cache dispatch and RGBA texture upload.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainSource.h"
#include "Services/Textures.h"
#include "Native/TextureAccess.h"
#include <vector>

namespace MiniMapTerrainSource
{
    /** Borrows native data only during Load; the caller owns the final receipt slot. */
    class TerrainChunkAcquisition
    {
    public:
        /** Uploads one 256-square chunk from native BC1 or owned CPU pixels into the caller's receipt. */
        static MiniMap::Services::Texture Load(const SourceTile& tile, int x, int y,
            MiniMap::Services::OwnedTexture& owner, bool& uncertain, bool& cached);
    private:
        /** Waits for native readiness and re-resolves registered textures after reentrancy. */
        static bool ResolveReadyTexture(const SourceTile& tile, SDK::UTexture2D*& texture);
        /** Queries and validates BC1 platform/mip-zero metadata without retaining native pointers. */
        static bool ReadMetadata(const SourceTile& tile,
            MiniMapNative::TextureAccess::PlatformData& platform,
            MiniMapNative::TextureAccess::MipData& mip);
        /** Enforces existing full 2048-square BC1 platform requirements plus registration metadata. */
        static bool ValidPlatform(const SourceTile& tile,
            const MiniMapNative::TextureAccess::PlatformData& platform);
        /** Reads legacy cache or copies/extracts native BC1 bytes; registered images bypass disk. */
        static bool ReadCompressed(const SourceTile& tile, int x, int y,
            std::vector<uint8_t>& compressed, bool& cached);
        /** Extracts one owned RGBA/BGRA image chunk and pads incomplete edges by clamping pixels. */
        static bool ExtractCpuChunk(const RegisteredTerrainImage& image, int x, int y,
            std::vector<uint8_t>& rgba);
        /** Copies native BulkData and frees it through the paired allocator even on exceptions. */
        static bool CopyChunk(const SourceTile& tile, int x, int y,
            const MiniMapNative::TextureAccess::PlatformData& platform,
            const MiniMapNative::TextureAccess::MipData& mip, std::vector<uint8_t>& compressed);
        /** Generates the unchanged legacy sharing name; registered uploads request fresh names. */
        static std::string SharingName(const SourceTile& tile, int x, int y);
        /** Validates the upload receipt while preserving all uncertain/retained outcomes. */
        static MiniMap::Services::Texture FinishUpload(MiniMap::Services::UploadState result,
            MiniMap::Services::OwnedTexture& owner, bool& uncertain);
    };
}
#endif
