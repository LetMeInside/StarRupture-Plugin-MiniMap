#pragma once
#include "Services/Viewport.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "plugin_interface.h"
#include <condition_variable>
#include <list>
#include <mutex>

namespace MiniMap::Adapters::AlienX
{
    class Viewport final : public Services::IViewport
    {
        struct Entry
        {
            Services::Widget Token;
            std::string Name;
            PluginWindowHints Hints{};
            PluginWidgetDesc Description{};
            Services::RenderFn Render = nullptr;
            void* User = nullptr;
            WidgetHandle Handle = nullptr;
            void (*Detach)(WidgetHandle) = nullptr;
            void (*Visible)(WidgetHandle, bool) = nullptr;
            bool Admitting = true, RemovalRequested = false;
            std::size_t InFlight = 0;
            Services::RegistryRemoval Registry = Services::RegistryRemoval::NotRequested;
            Services::Error RemovalError = Services::Error::None;
        };
        std::list<Entry> Entries; // Stable endpoint/descriptor storage through late host calls.
        Entry* Current = nullptr; // The existing single MiniMap widget endpoint.
        std::uint64_t NextToken = 1;
        std::mutex Mutex;
        std::condition_variable Drained;
        static void RenderThunk(IModLoaderImGui*) noexcept;
        void Dispatch(IModLoaderImGui*) noexcept;
    public:
        Services::RegistrationReceipt<Services::Widget> CreateOverlay(const Services::OverlaySpec&,
            Services::RenderFn, void* user) override;
        Services::Status SetVisible(Services::Widget, bool) override;
        Services::Removal Remove(Services::Widget) override;
    };
    Viewport& GetViewport() noexcept;

    // Stage 2.7 only: existing legacy texture owners borrow one binding for an
    // immediate Image/ImageQuad call. No acquisition, release, registry or cache.
    // Neither this guard nor its token may outlive that call/current frame.
    class BorrowedTexture final
    {
        Services::ICanvas& Frame;
        Services::Texture Token;
    public:
        BorrowedTexture(Services::ICanvas&, PluginTextureHandle);
        ~BorrowedTexture();
        BorrowedTexture(const BorrowedTexture&) = delete;
        BorrowedTexture& operator=(const BorrowedTexture&) = delete;
        Services::Texture Value() const noexcept { return Token; }
    };
}
#endif
