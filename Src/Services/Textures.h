#pragma once
#include "ServiceTypes.h"
#include <atomic>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace SDK { class UTexture2D; }

namespace MiniMap::Services
{
    struct Texture { std::uint64_t id = 0; }; // Copyable, non-owning draw identity.
    struct TextureSize { std::uint32_t width, height; };
    struct TextureRequest
    {
        std::string_view sharingName; // Empty requests a fresh backend name.
        std::optional<TextureSize> requiredSize;
    };
    enum class UploadState { Complete, Rejected, Unavailable, OwnershipUncertain };
    enum class ReleaseState { Empty, Returned, UnavailableRetained, OwnershipUncertain };
    struct FreeSlots { Status status; std::uint32_t count = 0; };

    class ITextures;
    class OwnedTexture final
    {
        friend class ITextures;
        // Neutral receipt linkage, shared with the issuing service's ledger.
        // No native binding and no destructor-driven backend operation.
        struct Record
        {
            const ITextures* const issuer;
            const std::uint64_t key;
            std::atomic<std::uint64_t> view{0}, dimensions{0};
            Record(const ITextures* service, std::uint64_t id) noexcept : issuer(service), key(id) {}
        };
        std::shared_ptr<Record> Link;
    public:
        OwnedTexture() noexcept = default;
        OwnedTexture(OwnedTexture&& other) noexcept { Swap(other); }
        OwnedTexture(const OwnedTexture&) = delete;
        OwnedTexture& operator=(const OwnedTexture&) = delete;
        OwnedTexture& operator=(OwnedTexture&&) = delete;
        ~OwnedTexture() = default;
        void Swap(OwnedTexture& other) noexcept { Link.swap(other.Link); }
        bool HasRecord() const noexcept { return bool(Link); }
        bool IsValidated() const noexcept { return View().id != 0; }
        Texture View() const noexcept { return {Link ? Link->view.load(std::memory_order_acquire) : 0}; }
        std::optional<TextureSize> Size() const noexcept
        {
            const auto bits = Link ? Link->dimensions.load(std::memory_order_acquire) : 0;
            if (!bits) return std::nullopt;
            return TextureSize{std::uint32_t(bits >> 32), std::uint32_t(bits)};
        }
        // As with other owner values, moving/swapping/releasing the receipt
        // itself requires owner synchronization. Copied Texture values do not.
    };

    class ITextures
    {
    protected:
        using ReceiptRecord = OwnedTexture::Record;
        using ReceiptLink = std::shared_ptr<ReceiptRecord>;
        ReceiptLink PrepareReceipt(std::uint64_t id) { return std::make_shared<ReceiptRecord>(this, id); }
        static void Attach(OwnedTexture& output, const ReceiptLink& link) noexcept { output.Link = link; }
        static void Clear(OwnedTexture& output) noexcept { output.Link.reset(); }
        bool Issued(const OwnedTexture& owner) const noexcept { return owner.Link && owner.Link->issuer == this; }
        static std::uint64_t Key(const OwnedTexture& owner) noexcept { return owner.Link ? owner.Link->key : 0; }
        static void Admit(const ReceiptLink& link, TextureSize size) noexcept
        {
            link->dimensions.store((std::uint64_t(size.width) << 32) | size.height, std::memory_order_release);
            link->view.store(link->key, std::memory_order_release);
        }
        static void Withdraw(const ReceiptLink& link) noexcept { link->view.store(0, std::memory_order_release); }
    public:
        virtual ~ITextures() = default;
        // Passive interface/lifetime prerequisites, not GPU readiness.
        virtual bool Available() const noexcept = 0;
        // Observation and acquisition require a game-thread callback outside rendering.
        virtual FreeSlots ObserveFreeSlots() noexcept = 0;
        virtual UploadState UploadRGBA(std::span<const std::uint8_t>, TextureSize,
            const TextureRequest&, OwnedTexture& emptyOutput) noexcept = 0;
        virtual UploadState CopyTexture2D(SDK::UTexture2D& borrowedSource,
            const TextureRequest&, OwnedTexture& emptyOutput) noexcept = 0;
        // Returned reports the API return, never GPU retirement completion.
        virtual ReleaseState Release(OwnedTexture&) noexcept = 0;
    };
}
