#pragma once

#include "ServiceTypes.h"
#include <span>
#include <string_view>

namespace MiniMap::Services
{
    enum class SettingKind { Boolean, Float, KeyBinding };
    struct SettingSpec
    {
        std::string_view section, key, defaultText, description;
        SettingKind kind;
        std::optional<float> editorMinimum, editorMaximum;

        // Keep domain schema declarations readable in section/key/type order.
        constexpr SettingSpec(std::string_view sectionName, std::string_view keyName,
            SettingKind type, std::string_view initial, std::string_view help,
            std::optional<float> minimum = {}, std::optional<float> maximum = {}) noexcept
            : section(sectionName), key(keyName), defaultText(initial), description(help),
              kind(type), editorMinimum(minimum), editorMaximum(maximum) {}
    };
    struct SettingName { std::string_view section, key; };
    enum class EditDecision { Accepted, Rejected, Ignored };
    using ConfigChanged = EditDecision (*)(void* user, SettingName name, std::string_view rawValue);

    class IConfiguration
    {
    public:
        virtual ~IConfiguration() = default;
        // Available before schema publication; an absent value is read failure,
        // not an invented default or a usable truncated prefix.
        virtual Result<std::string> ReadRaw(SettingName, std::string_view missingDefault) = 0;
        virtual Status PublishSchema(std::span<const SettingSpec>) = 0;
        virtual Status ValidateStoredSchema() = 0;
        // Callback views are borrowed until return. Acceptance is a request
        // receipt, not proof of editor readiness or durable persistence.
        virtual RegistrationReceipt<Registration> Observe(ConfigChanged, void* user) = 0;
        virtual Removal Remove(Registration) = 0;
    };
}
