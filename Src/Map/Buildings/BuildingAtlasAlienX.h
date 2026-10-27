#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "plugin_interface.h"
#include "Map/Buildings/SpriteStatus.h"
#include <array>
#include <cstdint>
#include <span>

namespace MiniMapBuildingAtlasAlienX
{
    enum class TransferResult { Complete, Rejected, Unavailable, OwnershipUncertain };
    enum class ReleaseResult { Empty, Released, OwnershipUncertain };
    struct Point { float X{}, Y{}; };
    using UV = MiniMapSpriteStatus::AtlasUV;

    class Backend;
    class Texture
    {
    public:
        Texture() = default;
        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;
        Texture(Texture&& other) noexcept;
        Texture& operator=(Texture&&) = delete; // Never overwrite an unreleased reference.
        // Explicit lifetime: Release before destruction and before loader teardown.
        // No loader calls from destructors, including static/global destruction.
        ~Texture() = default;
        void Swap(Texture& other) noexcept; // Transfers obligations; releases neither reference.
        bool HasOwnership() const noexcept { return State != Ownership::Empty; }
        bool IsValid() const noexcept { return State == Ownership::Validated; }
        bool ReleaseUncertain() const noexcept { return State == Ownership::ReleaseUncertain; }
        // Borrowed opaque loader binding; never free it independently of this owner.
        PluginTextureHandle Binding() const noexcept { return IsValid() ? Handle : nullptr; }
        std::uint32_t Width() const noexcept { return IsValid() ? ImageWidth : 0; }
        std::uint32_t Height() const noexcept { return IsValid() ? ImageHeight : 0; }

    private:
        friend class Backend;
        enum class Ownership { Empty, Acquired, Validated, ReleaseUncertain };
        Ownership State = Ownership::Empty;
        PluginTextureHandle Handle{};
        void (*Free)(PluginTextureHandle){};
        void (*Size)(PluginTextureHandle, int*, int*){};
        std::uint32_t ImageWidth{}, ImageHeight{};
    };

    class Backend
    {
    public:
        Backend() = default;
        Backend(const Backend&) = delete;
        Backend& operator=(const Backend&) = delete;
        Backend(Backend&&) = delete;
        Backend& operator=(Backend&&) = delete;
        // Borrow v70 API until all textures are explicitly released. Game-thread
        // initialization only; no GPU operation. Reinitialization never resets names.
        bool Initialize(IPluginImGuiTextures* textures) noexcept;
        bool HasCapacity() const noexcept; // Advisory, not a slot reservation.
        // Caller: documented game-thread callback, outside lifecycle lock and UI
        // render callback. SDK has no public thread-check API; caller owns affinity.
        // Output must be empty. Failure never overwrites an existing owner.
        // OwnershipUncertain can leave a non-drawable release record in output;
        // preserve it for diagnosis, and never blindly retry its FreeTexture.
        TransferResult Create(std::span<const std::uint8_t> rgba,
            std::uint32_t width, std::uint32_t height, Texture& output) noexcept;
        // Any thread per v70, but serialize access to the C++ Texture record.
        // Withdraw published binding first. Uncertain releases must not be retried.
        ReleaseResult Release(Texture& texture) const noexcept;
        // Active UI callback only. Caller keeps the binding valid for submission.
        // Corners/UVs are forwarded in supplied order; no clipping-state changes.
        bool SubmitQuad(IModLoaderImGui* ui, PluginDrawList list, const Texture& texture,
            const std::array<Point, 4>& positions, const std::array<UV, 4>& uvs,
            std::uint32_t packedColor) const noexcept;
        // Publication-validated UVs only. All dynamic/position guards remain live.
        bool SubmitPreparedQuad(IModLoaderImGui* ui, PluginDrawList list, const Texture& texture,
            const std::array<Point, 4>& positions, const MiniMapSpriteStatus::PreparedAtlasUV& uvs,
            std::uint32_t packedColor) const noexcept;
        bool UploadOwnershipUncertain() const noexcept { return UploadUncertain; }

    private:
        bool Available() const noexcept;
        IPluginImGuiTextures* Textures{};
        std::array<char, 33> InstanceToken{}; // 128-bit GUID as 32 uppercase hex bytes.
        std::uint64_t Serial{}; // Never reset; Create/Initialize serialized on game thread.
        bool UploadUncertain = false;
    };
}

#endif
