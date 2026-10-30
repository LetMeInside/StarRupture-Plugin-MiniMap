#include "Input.h"
#include "Compatibility.h"
#include "plugin_helpers.h"
#include <utility>

// Preserve the former input ABI/vocabulary review checks inside the adapter.
static_assert(PLUGIN_INTERFACE_VERSION == 70, "Review the wheel ABI when updating the SDK");
static_assert(std::size_t(EModKey::Unknown) == 101);

namespace MiniMap::Adapters::AlienX
{
    namespace S = Services;

    // Transitional admission checks at the original consumer initialization
    // positions. Availability is not proof that the host dispatch route is live.
    bool KeyInputAvailable() noexcept
    {
        auto* self = GetSelf();
        return self && self->hooks && self->hooks->Input &&
            self->hooks->Input->RegisterKeybindByName && self->hooks->Input->UnregisterKeybindByName;
    }
    bool WheelInputAvailable() noexcept
    {
        auto* self = GetSelf();
        return self && self->hooks && self->hooks->Input &&
            self->hooks->Input->RegisterMouseWheel && self->hooks->Input->UnregisterMouseWheel;
    }

    template<std::size_t Index> void Input::KeyThunk(EModKey, EModKeyEvent) noexcept
    {
        GetInput().DispatchKey(Index);
    }
    PluginKeybindCallback Input::NativeKey(Kind kind) noexcept
    {
        switch (kind)
        {
        case Kind::Toggle: return &KeyThunk<0>;
        case Kind::ZoomIn: return &KeyThunk<1>;
        case Kind::ZoomOut: return &KeyThunk<2>;
        case Kind::Diagnostic: return &KeyThunk<3>;
        default: return nullptr;
        }
    }

    S::RegistrationReceipt<S::Registration> Input::BindAction(const S::ActionSpec& spec, S::ActionFn action, void* user)
    {
        if (spec.preference.section == "General" && spec.preference.key == "ToggleMiniMap")
            return BindKey(Kind::Toggle, spec.initialCombo, action, user);
        if (spec.preference.section == "Zoom" && spec.preference.key == "ZoomInKey")
            return BindKey(Kind::ZoomIn, spec.initialCombo, action, user);
        if (spec.preference.section == "Zoom" && spec.preference.key == "ZoomOutKey")
            return BindKey(Kind::ZoomOut, spec.initialCombo, action, user);
        return {{}, S::Acceptance::Rejected, {S::Error::InvalidArgument, {}}};
    }
    S::RegistrationReceipt<S::Registration> Input::BindDiagnostic(std::string_view combo, S::ActionFn action, void* user)
    {
        return BindKey(Kind::Diagnostic, combo, action, user);
    }

    S::RegistrationReceipt<S::Registration> Input::BindKey(Kind kind, std::string_view combo, S::ActionFn action, void* user)
    {
        if (!KeyInputAvailable()) return {{}, S::Acceptance::Rejected, {S::Error::Unavailable, {}}};
        if (!action || combo.empty() || combo.find('\0') != std::string_view::npos)
            return {{}, S::Acceptance::Rejected, {S::Error::InvalidArgument, {}}};
        Entry* entry;
        try
        {
            // Prepare owned identity before exposing an endpoint to the loader.
            std::string original(combo);
            std::lock_guard lock(Mutex);
            auto*& current = Keys[std::size_t(kind)];
            if (current && (current->Admitting || current->InFlight ||
                current->Registry != S::RegistryRemoval::RequestReturned))
                return {{}, S::Acceptance::Rejected, {S::Error::Unavailable, {}}};
            if (!NextToken) return {{}, S::Acceptance::Rejected, {S::Error::Capacity, {}}};
            Entries.push_back({{NextToken}, kind, std::move(original), action, nullptr, user});
            ++NextToken;
            current = entry = &Entries.back();
        }
        catch (...) { return {{}, S::Acceptance::Rejected, {S::Error::BackendFailure, {}}}; }
        try
        {
            GetSelf()->hooks->Input->RegisterKeybindByName(entry->OriginalCombo.c_str(), EModKeyEvent::Pressed, NativeKey(kind));
            return {entry->Token, S::Acceptance::AcceptedUnverified, {}};
        }
        catch (...)
        {
            std::lock_guard lock(Mutex);
            entry->Admitting = false;
            entry->Registry = S::RegistryRemoval::Unknown;
            return {entry->Token, S::Acceptance::BackendUnknown, {S::Error::BackendFailure, {}}};
        }
    }

    S::RegistrationReceipt<S::Registration> Input::OnWheel(S::WheelFn callback, void* user)
    {
        if (!WheelInputAvailable()) return {{}, S::Acceptance::Rejected, {S::Error::Unavailable, {}}};
        if (!callback) return {{}, S::Acceptance::Rejected, {S::Error::InvalidArgument, {}}};
        Entry* entry;
        try
        {
            std::lock_guard lock(Mutex);
            if (Wheel && (Wheel->Admitting || Wheel->InFlight || Wheel->Registry != S::RegistryRemoval::RequestReturned))
                return {{}, S::Acceptance::Rejected, {S::Error::Unavailable, {}}};
            if (!NextToken) return {{}, S::Acceptance::Rejected, {S::Error::Capacity, {}}};
            Entries.push_back({{NextToken}, Kind::Wheel, {}, nullptr, callback, user});
            ++NextToken;
            Wheel = entry = &Entries.back();
        }
        catch (...) { return {{}, S::Acceptance::Rejected, {S::Error::BackendFailure, {}}}; }
        try
        {
            GetSelf()->hooks->Input->RegisterMouseWheel(&WheelThunk, entry);
            return {entry->Token, S::Acceptance::AcceptedUnverified, {}};
        }
        catch (...)
        {
            std::lock_guard lock(Mutex);
            entry->Admitting = false;
            entry->Registry = S::RegistryRemoval::Unknown;
            return {entry->Token, S::Acceptance::BackendUnknown, {S::Error::BackendFailure, {}}};
        }
    }

    S::Removal Input::Remove(S::Registration token)
    {
        Entry* entry = nullptr;
        {
            std::lock_guard lock(Mutex);
            if (!token.id) return {true, S::RegistryRemoval::NotRequested, S::CoreDrain::Quiescent, {}};
            for (auto& candidate : Entries) if (candidate.Token.id == token.id) { entry = &candidate; break; }
            if (!entry) return {false, S::RegistryRemoval::NotRequested, S::CoreDrain::Unknown, {S::Error::InvalidArgument, {}}};
            entry->Admitting = false;
            if (entry->DetachRequested)
                return {true, entry->Registry, entry->InFlight ? S::CoreDrain::Pending : S::CoreDrain::Quiescent, {entry->RemovalError, {}}};
            entry->DetachRequested = true;
            entry->Registry = S::RegistryRemoval::Unknown;
            entry->RemovalError = S::Error::OwnershipUncertain;
        }
        auto registry = S::RegistryRemoval::Unknown;
        auto error = S::Error::None;
        try
        {
            if (entry->Type == Kind::Wheel && WheelInputAvailable())
            {
                GetSelf()->hooks->Input->UnregisterMouseWheel(&WheelThunk, entry);
                registry = S::RegistryRemoval::RequestReturned;
            }
            else if (entry->Type != Kind::Wheel && KeyInputAvailable())
            {
                GetSelf()->hooks->Input->UnregisterKeybindByName(entry->OriginalCombo.c_str(), EModKeyEvent::Pressed, NativeKey(entry->Type));
                registry = S::RegistryRemoval::RequestReturned;
            }
            else error = S::Error::Unavailable;
        }
        catch (...) { error = S::Error::BackendFailure; }
        std::lock_guard lock(Mutex);
        entry->Registry = registry;
        entry->RemovalError = error;
        return {true, registry, entry->InFlight ? S::CoreDrain::Pending : S::CoreDrain::Quiescent, {error, {}}};
    }

    void Input::DispatchKey(std::size_t index) noexcept
    {
        Entry* entry;
        {
            std::lock_guard lock(Mutex);
            entry = Keys[index];
            if (!entry || !entry->Admitting) return;
            ++entry->InFlight;
        }
        try { entry->Action(entry->User); }
        catch (...) {} // The original key dispatcher also contains callback exceptions.
        std::lock_guard lock(Mutex);
        --entry->InFlight;
    }

    bool Input::WheelThunk(const PluginMouseWheelEvent* event, void* user) noexcept
    {
        if (!event || event->size < sizeof(PluginMouseWheelEvent) || !user) return false;
        return GetInput().DispatchWheel(*static_cast<Entry*>(user), *event);
    }
    bool Input::DispatchWheel(Entry& entry, const PluginMouseWheelEvent& event) noexcept
    {
        {
            std::lock_guard lock(Mutex);
            if (!entry.Admitting) return false;
            ++entry.InFlight;
        }
        std::uint32_t modifiers = 0;
        if (event.modifiers & PluginWheelMod_LeftCtrl) modifiers |= std::uint32_t(S::WheelModifier::LeftControl);
        if (event.modifiers & PluginWheelMod_RightCtrl) modifiers |= std::uint32_t(S::WheelModifier::RightControl);
        if (event.modifiers & PluginWheelMod_LeftShift) modifiers |= std::uint32_t(S::WheelModifier::LeftShift);
        if (event.modifiers & PluginWheelMod_RightShift) modifiers |= std::uint32_t(S::WheelModifier::RightShift);
        if (event.modifiers & PluginWheelMod_LeftAlt) modifiers |= std::uint32_t(S::WheelModifier::LeftAlt);
        if (event.modifiers & PluginWheelMod_RightAlt) modifiers |= std::uint32_t(S::WheelModifier::RightAlt);
        bool handled = false;
        try { handled = entry.Wheel(entry.User, {event.rawDelta, event.rawDeltaH, modifiers, event.uiCapturing}); }
        catch (...) { LOG_ERROR("MiniMap: wheel callback exception; treated as not consumed"); }
        std::lock_guard lock(Mutex);
        --entry.InFlight;
        return handled;
    }
}
