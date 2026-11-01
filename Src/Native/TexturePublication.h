#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <array>
#include <cstdint>
#include <optional>
#include <vector>
namespace SDK { class UTexture2D; }

namespace MiniMapTexturePublication
{
    // Linear associated R,G,B and transmittance T, little-endian half words.
    inline constexpr std::array<uint8_t, 8> ClearPixel{0, 0, 0, 0, 0, 0, 0, 0x3c};
    struct Generation;
    class Image
    {
        int32_t Width = 0, Height = 0;
        std::vector<uint8_t> Pixels;
        friend uint64_t Prepare(Image&&, Generation);
    public:
        Image() = default;
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
        Image(Image&&) noexcept = default;
        Image& operator=(Image&&) noexcept = default;
        // Pure CPU operation, permitted on workers. Invalid input is rejected,
        // never clamped. Ownership transfers; callers retain no mutable aliases.
        static std::optional<Image> FromCanonical(int32_t width, int32_t height, std::vector<uint8_t>&& bytes);
    };
    struct Generation
    {
        uint64_t Epoch = 0, Request = 0, Revision = 0;
        bool operator==(const Generation&) const = default;
    };
    enum class State : uint8_t { Empty, Prepared, ResourceInitializing, Ready, Retiring, Failed };
    struct Status
    {
        State Phase = State::Empty;
        bool OwnershipUncertain = false, Failed = false;
        uint64_t ReservedCpuBytes = 0, ReservedGpuBytes = 0;
    };
    bool Initialize() noexcept;
    // All functions below are GT-only. Prepare/Ready require the existing
    // admitted BuildingComplex service scope. Zero means no capacity/admission.
    uint64_t Prepare(Image&&, Generation);
    SDK::UTexture2D* Ready(uint64_t owner, Generation current);
    Status Inspect(uint64_t owner);
    // The future consumer must detach all issued uses before Retire. A ready
    // identity is immutable and remains rooted until this acknowledgement.
    bool Retire(uint64_t owner, bool detached);
    // Called outside lifecycle locks by the existing plugin callback. Polls at
    // a bounded set of simultaneous owners; never waits, flushes, or does worker GPU IO.
    void Service();
    void CancelWorld() noexcept; // Withdraw readiness; retain issued uses.
    void Shutdown() noexcept; // No native work without safe GT/GC admission.
}
#endif
