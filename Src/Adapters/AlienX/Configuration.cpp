#include "Configuration.h"
#include "Compatibility.h"
#include "plugin_helpers.h"

#include <cstring>
#include <limits>
#include <utility>

namespace MiniMap::Adapters::AlienX
{
    namespace S = Services;

    S::Status Configuration::Availability() const
    {
        auto* self = GetSelf();
        if (!self || !self->config || !self->hooks || !self->config->ReadString ||
            !self->config->InitializeFromSchema || !self->config->ValidateConfig)
            return {S::Error::Unavailable, "MiniMap: configuration interface unavailable"};
#if defined(MODLOADER_CLIENT_BUILD)
        if (!self->hooks->UI || !self->hooks->UI->RegisterOnConfigChanged ||
            !self->hooks->UI->UnregisterOnConfigChanged)
            return {S::Error::Unavailable, "MiniMap: client configuration notification interface unavailable"};
#endif
        // Public table availability does not certify the private manager's
        // initialization. Its real return values still govern startup admission.
        return {};
    }

    S::Result<std::string> Configuration::ReadRaw(S::SettingName name, std::string_view missingDefault)
    {
        const auto available = Availability();
        if (available.error != S::Error::None) return {{}, available};
        try
        {
            const std::string section(name.section), key(name.key), initial(missingDefault);
            // Match the former domain read capacity. Every accepted <=127-byte
            // value survives intact; longer loader values remain overlong.
            char raw[4096]{};
            auto* self = GetSelf();
            if (!self->config->ReadString(self, section.c_str(), key.c_str(), raw,
                int(sizeof(raw)), initial.c_str())) return {{}, {S::Error::BackendFailure, {}}};
            const auto length = strnlen_s(raw, sizeof(raw));
            if (length == sizeof(raw)) return {{}, {S::Error::InvalidArgument, {}}};
            return {std::string(raw, length), {}};
        }
        catch (...) { return {{}, {S::Error::BackendFailure, {}}}; }
    }

    S::Status Configuration::PublishSchema(std::span<const S::SettingSpec> settings)
    {
        const auto available = Availability();
        if (available.error != S::Error::None) return available;
        if (settings.empty() || settings.size() > std::size_t((std::numeric_limits<int>::max)()))
            return {S::Error::InvalidArgument, {}};
        try
        {
            if (!Schema)
            {
                auto storage = std::make_unique<SchemaStorage>();
                storage->Strings.reserve(settings.size());
                storage->Entries.reserve(settings.size());
                for (const auto& setting : settings)
                    storage->Strings.push_back({std::string(setting.section), std::string(setting.key),
                        std::string(setting.defaultText), std::string(setting.description)});
                for (std::size_t i = 0; i < settings.size(); ++i)
                {
                    ConfigValueType type;
                    switch (settings[i].kind)
                    {
                    case S::SettingKind::Boolean: type = ConfigValueType::Boolean; break;
                    case S::SettingKind::Float: type = ConfigValueType::Float; break;
                    case S::SettingKind::KeyBinding: type = ConfigValueType::Keybind; break;
                    default: return {S::Error::InvalidArgument, {}};
                    }
                    const auto& strings = storage->Strings[i];
                    storage->Entries.push_back({strings.Section.c_str(), strings.Key.c_str(), type,
                        strings.Default.c_str(), strings.Description.c_str(),
                        settings[i].editorMinimum.value_or(0.0f), settings[i].editorMaximum.value_or(0.0f)});
                }
                storage->Schema = {storage->Entries.data(), int(storage->Entries.size())};
                Schema = std::move(storage); // Retain before crossing a mutating host call.
            }
            else
            {
                // Reinitialization may reuse the same descriptors; never free a
                // schema the host can still borrow or replace it with new policy.
                if (settings.size() != Schema->Entries.size()) return {S::Error::InvalidArgument, {}};
                for (std::size_t i = 0; i < settings.size(); ++i)
                {
                    const auto& native = Schema->Entries[i];
                    const auto& setting = settings[i];
                    const auto kind = native.type == ConfigValueType::Boolean ? S::SettingKind::Boolean :
                        native.type == ConfigValueType::Float ? S::SettingKind::Float : S::SettingKind::KeyBinding;
                    if (setting.section != native.section || setting.key != native.key ||
                        setting.defaultText != native.defaultValue || setting.description != native.description ||
                        setting.kind != kind || setting.editorMinimum.value_or(0.0f) != native.rangeMin ||
                        setting.editorMaximum.value_or(0.0f) != native.rangeMax)
                        return {S::Error::InvalidArgument, {}};
                }
            }
            auto* self = GetSelf();
            SchemaInitialized = false;
            if (!self->config->InitializeFromSchema(self, &Schema->Schema)) return {S::Error::BackendFailure, {}};
            SchemaInitialized = true;
            return {};
        }
        catch (...) { return {S::Error::BackendFailure, {}}; }
    }

    S::Status Configuration::ValidateStoredSchema()
    {
        const auto available = Availability();
        if (available.error != S::Error::None) return available;
        if (!SchemaInitialized) return {S::Error::Unavailable, {}};
        try
        {
            auto* self = GetSelf();
            self->config->ValidateConfig(self, &Schema->Schema);
            return {}; // Void host request returned; not persistence/readiness certification.
        }
        catch (...) { return {S::Error::BackendFailure, {}}; }
    }

    S::RegistrationReceipt<S::Registration> Configuration::Observe(S::ConfigChanged callback, void* user)
    {
        const auto available = Availability();
        if (available.error != S::Error::None) return {{}, S::Acceptance::Rejected, available};
#if defined(MODLOADER_CLIENT_BUILD)
        if (!callback) return {{}, S::Acceptance::Rejected, {S::Error::InvalidArgument, {}}};
        S::Registration token;
        {
            std::lock_guard lock(ObserverMutex);
            if (Admitting || InFlight || Registry == S::RegistryRemoval::Unknown)
                return {{}, S::Acceptance::Rejected, {S::Error::Unavailable, {}}};
            if (!NextObserver) return {{}, S::Acceptance::Rejected, {S::Error::Capacity, {}}};
            Observer = token = {NextObserver++};
            Callback = callback;
            User = user;
            Registry = S::RegistryRemoval::NotRequested;
            DetachRequested = false;
            RemovalError = S::Error::None;
            Admitting = true; // Fully prepared before the host can publish the thunk.
        }
        try
        {
            auto* self = GetSelf();
            self->hooks->UI->RegisterOnConfigChanged(self, &OnChanged);
            return {token, S::Acceptance::AcceptedUnverified, {}};
        }
        catch (...)
        {
            std::lock_guard lock(ObserverMutex);
            Admitting = false;
            Registry = S::RegistryRemoval::Unknown;
            return {token, S::Acceptance::BackendUnknown, {S::Error::BackendFailure, {}}};
        }
#else
        (void)callback;
        (void)user;
        return {{}, S::Acceptance::Rejected, {S::Error::Unavailable, {}}};
#endif
    }

    S::Removal Configuration::Remove(S::Registration token)
    {
        {
            std::lock_guard lock(ObserverMutex);
            if (!token.id) return {true, S::RegistryRemoval::NotRequested, S::CoreDrain::Quiescent, {}};
            // A new observation is allowed only after the prior detach returned
            // and its admitted bodies drained. Old tokens cannot close it.
            if (token.id < Observer.id)
                return {true, S::RegistryRemoval::RequestReturned, S::CoreDrain::Quiescent, {}};
            if (token.id != Observer.id)
                return {false, S::RegistryRemoval::NotRequested, S::CoreDrain::Unknown, {S::Error::InvalidArgument, {}}};
            Admitting = false;
            if (DetachRequested)
                return {true, Registry, InFlight ? S::CoreDrain::Pending : S::CoreDrain::Quiescent, {RemovalError, {}}};
            // Reserve the single detach attempt before releasing our lock.
            DetachRequested = true;
            RemovalError = S::Error::OwnershipUncertain;
            Registry = S::RegistryRemoval::Unknown;
        }
        S::Status status;
#if defined(MODLOADER_CLIENT_BUILD)
        try
        {
            auto* self = GetSelf();
            if (self && self->hooks && self->hooks->UI && self->hooks->UI->UnregisterOnConfigChanged)
            {
                self->hooks->UI->UnregisterOnConfigChanged(self, &OnChanged);
                std::lock_guard lock(ObserverMutex);
                Registry = S::RegistryRemoval::RequestReturned;
            }
            else status.error = S::Error::Unavailable;
        }
        catch (...) { status.error = S::Error::BackendFailure; }
#else
        status.error = S::Error::Unavailable;
#endif
        std::lock_guard lock(ObserverMutex);
        RemovalError = status.error;
        return {true, Registry, InFlight ? S::CoreDrain::Pending : S::CoreDrain::Quiescent, std::move(status)};
    }

    void Configuration::OnChanged(const char* section, const char* key, const char* value) noexcept
    {
        (void)GetConfiguration().Notify(section, key, value);
    }

    S::EditDecision Configuration::Notify(const char* section, const char* key, const char* value) noexcept
    {
        S::ConfigChanged callback;
        void* user;
        {
            std::lock_guard lock(ObserverMutex);
            if (!Admitting) return S::EditDecision::Ignored;
            ++InFlight;
            callback = Callback;
            user = User;
        }
        S::EditDecision decision = S::EditDecision::Rejected;
        try
        {
            decision = callback(user, {section ? std::string_view(section) : std::string_view{},
                key ? std::string_view(key) : std::string_view{}},
                value ? std::string_view(value) : std::string_view{});
        }
        catch (...) { LOG_ERROR("MiniMap: configuration notification exception"); }
        {
            std::lock_guard lock(ObserverMutex);
            --InFlight;
        }
        // AlienX's void callback cannot veto its later Commit or key relocation.
        return decision;
    }
}
