#pragma once

#include "Services/Input.h"
#include "plugin_interface.h"
#include <array>
#include <list>
#include <mutex>

namespace MiniMap::Adapters::AlienX
{
    class Input final : public Services::IInput
    {
        enum class Kind { Toggle, ZoomIn, ZoomOut, Diagnostic, Wheel };
        struct Entry
        {
            Services::Registration Token;
            Kind Type;
            std::string OriginalCombo;
            Services::ActionFn Action = nullptr;
            Services::WheelFn Wheel = nullptr;
            void* User = nullptr;
            bool Admitting = true, DetachRequested = false;
            std::size_t InFlight = 0;
            Services::RegistryRemoval Registry = Services::RegistryRemoval::NotRequested;
            Services::Error RemovalError = Services::Error::None;
        };
        // These are the four actual key consumers, each with a distinct native
        // callback identity. Wheel userdata can identify its own stable record.
        std::array<Entry*, 4> Keys{};
        Entry* Wheel = nullptr;
        std::list<Entry> Entries; // Closed records survive host callback snapshots.
        std::uint64_t NextToken = 1;
        std::mutex Mutex;

        Services::RegistrationReceipt<Services::Registration> BindKey(Kind, std::string_view,
            Services::ActionFn, void* user);
        static PluginKeybindCallback NativeKey(Kind) noexcept;
        template<std::size_t Index> static void KeyThunk(EModKey, EModKeyEvent) noexcept;
        static bool WheelThunk(const PluginMouseWheelEvent*, void* user) noexcept;
        void DispatchKey(std::size_t index) noexcept;
        bool DispatchWheel(Entry&, const PluginMouseWheelEvent&) noexcept;
    public:
        Services::RegistrationReceipt<Services::Registration> BindAction(const Services::ActionSpec&,
            Services::ActionFn, void* user) override;
        Services::RegistrationReceipt<Services::Registration> BindDiagnostic(std::string_view,
            Services::ActionFn, void* user) override;
        Services::RegistrationReceipt<Services::Registration> OnWheel(Services::WheelFn, void* user) override;
        Services::Removal Remove(Services::Registration) override;
    };

    Input& GetInput() noexcept; // Fixed bootstrap-owned instance; no setter.
}
