#pragma once

#include "Services/Configuration.h"
#include "plugin_interface.h"
#include <memory>
#include <mutex>
#include <vector>

namespace MiniMap::Adapters::AlienX
{
    class Configuration final : public Services::IConfiguration
    {
        struct OwnedSetting
        {
            std::string Section, Key, Default, Description;
        };
        struct SchemaStorage
        {
            std::vector<OwnedSetting> Strings;
            std::vector<ConfigEntry> Entries;
            ConfigSchema Schema{};
        };
        // The host retains the schema pointer, even if initialization fails.
        // Keep one translation at a stable address for this module's lifetime.
        std::unique_ptr<SchemaStorage> Schema;
        bool SchemaInitialized = false; // Lifecycle only; not domain readiness.

        // One existing MiniMap observer, not a second notification registry.
        // Host snapshots can enter the thunk after native unregister returns.
        std::mutex ObserverMutex;
        Services::ConfigChanged Callback = nullptr;
        void* User = nullptr;
        Services::Registration Observer{};
        std::uint64_t NextObserver = 1;
        std::size_t InFlight = 0;
        bool Admitting = false;
        bool DetachRequested = false;
        Services::Error RemovalError = Services::Error::None;
        Services::RegistryRemoval Registry = Services::RegistryRemoval::NotRequested;

        Services::Status Availability() const;
        static void OnChanged(const char* section, const char* key, const char* value) noexcept;
        Services::EditDecision Notify(const char* section, const char* key, const char* value) noexcept;
    public:
        Services::Result<std::string> ReadRaw(Services::SettingName, std::string_view missingDefault) override;
        Services::Status PublishSchema(std::span<const Services::SettingSpec>) override;
        Services::Status ValidateStoredSchema() override;
        Services::RegistrationReceipt<Services::Registration> Observe(Services::ConfigChanged, void* user) override;
        Services::Removal Remove(Services::Registration) override;
    };

    // Selected bootstrap's fixed instance. No setter or replaceable service slot.
    Configuration& GetConfiguration() noexcept;
}
