#pragma once
#include "Services/Textures.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "Services/Viewport.h"
#include "plugin_interface.h"
#include <array>
#include <condition_variable>
#include <list>
#include <mutex>

namespace MiniMap::Adapters::AlienX
{
    // Marks the existing synchronous Canvas callback; never schedules work.
    class TextureRenderScope final
    {
        static thread_local unsigned Depth;
    public:
        TextureRenderScope() noexcept { ++Depth; }
        ~TextureRenderScope() { --Depth; }
        TextureRenderScope(const TextureRenderScope&) = delete;
        TextureRenderScope& operator=(const TextureRenderScope&) = delete;
        static bool Active() noexcept { return Depth != 0; }
    };
    class Textures final : public Services::ITextures
    {
        enum class State { Reserved, Acquired, Validated, Withdrawing, Released, ReleaseUncertain, UploadUncertain };
        enum class Evidence { None, SlotRejected, NullReturn, LoadException, SizeException, InvalidSize,
            RequiredSizeMismatch, InterfaceUnavailable, FreeException };
        struct Backend
        {
            decltype(IPluginImGuiTextures::LoadFromRGBA) rgba = nullptr;
            decltype(IPluginImGuiTextures::LoadFromUTexture2D) copy = nullptr;
            decltype(IPluginImGuiTextures::GetSize) size = nullptr;
            decltype(IPluginImGuiTextures::FreeTexture) free = nullptr;
            decltype(IPluginImGuiTextures::GetFreeSlotCount) slots = nullptr;
        };
        struct Record
        {
            ReceiptLink Receipt;
            std::string Name;
            std::optional<Services::TextureSize> Required;
            Services::TextureSize Input{}, ValidatedSize{};
            int ObservedWidth = 0, ObservedHeight = 0;
            Backend Functions;
            PluginTextureHandle Handle = nullptr;
            State Ownership = State::Reserved;
            Evidence Failure = Evidence::None, CleanupFailure = Evidence::None;
            bool EngineCopy = false, Busy = true;
            bool ReleaseAttempted = false, ReleaseThrew = false;
        };
        enum class Kind { Acquire, Release, Draw };
        class Operation
        {
            Textures& Owner;
            Operation* Previous = nullptr;
            static thread_local Operation* Current;
        public:
            Backend Functions;
            bool Admitted = false;
            Operation(Textures&, Kind) noexcept;
            ~Operation();
            Operation(const Operation&) = delete;
            Operation& operator=(const Operation&) = delete;
            static bool Contains(const Textures&) noexcept;
        };
        mutable std::mutex Gate;
        std::condition_variable Drained;
        std::size_t InFlight = 0;
        bool Initialized = false, AcquisitionsOpen = false, OperationsOpen = false, BackendOpen = false;
        mutable bool BackendLost = false;
        IPluginImGuiTextures* Native = nullptr;
        Backend Functions;
        std::array<char, 32> InstanceName{};
        bool NamesReady = false;
        std::atomic<std::uint64_t> NextIdentity{1}; // Never reset, including Initialize/Stop.
        mutable std::mutex LedgerMutex;
        std::list<Record> Ledger; // Stable nodes, retained even when receipts are lost.
        bool Connected() const noexcept; // Gate held; observation only, no loader call.
        void WithdrawAll() const noexcept;
        Record* Find(std::uint64_t) noexcept; // LedgerMutex held.
        Services::UploadState Acquire(std::span<const std::uint8_t>, Services::TextureSize,
            SDK::UTexture2D*, const Services::TextureRequest&, Services::OwnedTexture&) noexcept;
        Services::ReleaseState ReturnRecord(Record&, Services::OwnedTexture&) noexcept;
        std::uint64_t IssueIdentity() noexcept;
    public:
        // Bootstrap-only lifetime. Initialization never uploads or frees.
        void Initialize(IPluginSelf*) noexcept;
        bool CloseAcquisitions() noexcept; // False on same-operation teardown.
        bool Stop() noexcept; // No sweep; false means retain adapter/runtime.
        bool Available() const noexcept override;
        Services::FreeSlots ObserveFreeSlots() noexcept override;
        Services::UploadState UploadRGBA(std::span<const std::uint8_t>, Services::TextureSize,
            const Services::TextureRequest&, Services::OwnedTexture&) noexcept override;
        Services::UploadState CopyTexture2D(SDK::UTexture2D&,
            const Services::TextureRequest&, Services::OwnedTexture&) noexcept override;
        Services::ReleaseState Release(Services::OwnedTexture&) noexcept override;
        // Adapter-only immediate forwarding: never exposes a resolved binding.
        void Image(IModLoaderImGui&, PluginDrawList, Services::Texture, Services::Vec2, Services::Vec2,
            Services::Vec2, Services::Vec2, Services::Color);
        void ImageQuad(IModLoaderImGui&, PluginDrawList, Services::Texture,
            const Services::Quad&, const Services::Quad&, Services::Color);
    };
    Textures& GetTextures() noexcept;
}
#endif
