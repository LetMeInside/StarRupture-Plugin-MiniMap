#include "CaptureTiming.h"
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingAtlasAlienX.h"
#include <objbase.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#pragma comment(lib, "ole32.lib")

namespace MiniMapBuildingAtlasAlienX
{
    Texture::Texture(Texture&& other) noexcept { Swap(other); }

    void Texture::Swap(Texture& other) noexcept
    {
        std::swap(State, other.State);
        std::swap(Handle, other.Handle);
        std::swap(Free, other.Free);
        std::swap(Size, other.Size);
        std::swap(ImageWidth, other.ImageWidth);
        std::swap(ImageHeight, other.ImageHeight);
    }

    bool Backend::Available() const noexcept
    {
        return Textures && Textures->LoadFromRGBA && Textures->FreeTexture &&
            Textures->GetSize && Textures->GetFreeSlotCount && !UploadUncertain;
    }

    bool Backend::Initialize(IPluginImGuiTextures* textures) noexcept
    {
        if (Textures) return textures == Textures && Available();
        if (!textures || !textures->LoadFromRGBA || !textures->FreeTexture ||
            !textures->GetSize || !textures->GetFreeSlotCount) return false;
        GUID token{};
        if (FAILED(CoCreateGuid(&token))) return false;
        static_assert(sizeof(token) == 16);
        constexpr char hex[] = "0123456789ABCDEF";
        const auto* bytes = reinterpret_cast<const unsigned char*>(&token);
        for (std::size_t i = 0; i < sizeof(token); ++i)
        {
            InstanceToken[i * 2] = hex[bytes[i] >> 4];
            InstanceToken[i * 2 + 1] = hex[bytes[i] & 15];
        }
        Textures = textures;
        return true;
    }

    bool Backend::HasCapacity() const noexcept
    {
        if (!Available() || Serial == (std::numeric_limits<std::uint64_t>::max)()) return false;
        try { return Textures->GetFreeSlotCount() > 0; }
        catch (...) { return false; } // Observational call; acquires no reference.
    }

    TransferResult Backend::Create(std::span<const std::uint8_t> rgba,
        std::uint32_t width, std::uint32_t height, Texture& output) noexcept
    {
        if (output.HasOwnership()) return TransferResult::Rejected;
        if (UploadUncertain) return TransferResult::OwnershipUncertain;
        if (!Available()) return TransferResult::Unavailable;
        if (!((width == 512 && height == 512) || (width == 1024 && height == 1024)))
            return TransferResult::Rejected;
        const auto maximum = (std::numeric_limits<std::size_t>::max)();
        if (std::size_t(width) > maximum / height || std::size_t(width) * height > maximum / 4)
            return TransferResult::Rejected;
        const std::size_t bytes = std::size_t(width) * height * 4;
        if (rgba.size() != bytes || !rgba.data()) return TransferResult::Rejected;
        if (Serial == (std::numeric_limits<std::uint64_t>::max)()) return TransferResult::Rejected;
        try { if (Textures->GetFreeSlotCount() <= 0) return TransferResult::Rejected; }
        catch (...) { return TransferResult::Unavailable; }

        // 5 prefix + 32 GUID + 1 separator + 16 serial + NUL = 55, within
        // the inspected v70 record's name[64]. No allocation or truncation.
        std::array<char, 55> name{};
        static_assert(sizeof(name) <= 64);
        std::memcpy(name.data(), "MMBA:", 5);
        std::memcpy(name.data() + 5, InstanceToken.data(), 32);
        name[37] = ':';
        constexpr char hex[] = "0123456789ABCDEF";
        auto number = ++Serial; // Consume even failed uploads; never reuse names.
        for (std::size_t i = 0; i < 16; ++i)
        {
            name[53 - i] = hex[number & 15];
            number >>= 4;
        }
        PluginTextureHandle handle{};
        try { handle = MiniMapCaptureTiming::Measure(MiniMapCaptureTiming::Stage::Upload,[&]{return Textures->LoadFromRGBA(rgba.data(), int(width), int(height), name.data());}); }
        catch (const std::out_of_range&) { MiniMapCaptureTiming::UploadOutcome(3);return TransferResult::Rejected; } // Verified pre-acquisition slot failure.
        catch (...)
        {
            // No handle was returned, so no reference can safely be identified or
            // freed. Preserve uncertainty and stop uploads, rather than retrying.
            MiniMapCaptureTiming::UploadOutcome(3);
            UploadUncertain = true;
            return TransferResult::OwnershipUncertain;
        }
        MiniMapCaptureTiming::UploadOutcome(handle?2:1);
        if (!handle) return TransferResult::Unavailable; // API cannot distinguish not-ready from upload failure.
        // Record the successful-load obligation BEFORE any validation call.
        output.Handle = handle;
        output.Free = Textures->FreeTexture;
        output.Size = Textures->GetSize;
        output.ImageWidth = width;
        output.ImageHeight = height;
        output.State = Texture::Ownership::Acquired;
        int actualWidth = 0, actualHeight = 0;
        bool valid = false;
        try
        {
            output.Size(handle, &actualWidth, &actualHeight);
            valid = actualWidth == int(width) && actualHeight == int(height);
        }
        catch (...) {} // The known acquired reference still requires cleanup below.
        if (!valid)
            return Release(output) == ReleaseResult::OwnershipUncertain
                ? TransferResult::OwnershipUncertain : TransferResult::Rejected;
        output.State = Texture::Ownership::Validated;
        return TransferResult::Complete;
    }

    ReleaseResult Backend::Release(Texture& texture) const noexcept
    {
        if (!texture.HasOwnership()) return ReleaseResult::Empty;
        if (texture.State == Texture::Ownership::ReleaseUncertain) return ReleaseResult::OwnershipUncertain;
        // Set before crossing the irreversible API boundary. If it throws after
        // decrementing a shared reference, repeating it could release another owner.
        texture.State = Texture::Ownership::ReleaseUncertain;
        if (!texture.Handle || !texture.Free) return ReleaseResult::OwnershipUncertain;
        try { MiniMapCaptureTiming::Measure(MiniMapCaptureTiming::Stage::Free,[&]{return texture.Free(texture.Handle);}); }
        catch (...) { return ReleaseResult::OwnershipUncertain; }
        texture.Handle = nullptr;
        texture.Free = nullptr;
        texture.Size = nullptr;
        texture.ImageWidth = texture.ImageHeight = 0;
        texture.State = Texture::Ownership::Empty;
        return ReleaseResult::Released;
    }

    bool Backend::SubmitQuad(IModLoaderImGui* ui, PluginDrawList list, const Texture& texture,
        const std::array<Point, 4>& p, const std::array<UV, 4>& uv, std::uint32_t color) const noexcept
    {
        if (!ui || !list || !ui->DL_AddImageQuad || !texture.IsValid() || !texture.Handle ||
            !texture.ImageWidth || !texture.ImageHeight) return false;
        for (std::size_t i = 0; i < 4; ++i)
            if (!std::isfinite(p[i].X) || !std::isfinite(p[i].Y) || !std::isfinite(uv[i].U) || !std::isfinite(uv[i].V)) return false;
        try
        {
            // Create already validated dimensions. The caller protects this
            // owned Texture from Release throughout submission; do not query
            // the texture-management API inside the lifecycle-locked draw pass.
            ui->DL_AddImageQuad(list, texture.Handle,
                p[0].X, p[0].Y, p[1].X, p[1].Y, p[2].X, p[2].Y, p[3].X, p[3].Y,
                uv[0].U, uv[0].V, uv[1].U, uv[1].V, uv[2].U, uv[2].V, uv[3].U, uv[3].V, color);
            return true; // Submission only, not GPU completion or visibility proof.
        }
        catch (...) { return false; } // No ownership change; never retry a possibly recorded draw here.
    }
}

#endif
