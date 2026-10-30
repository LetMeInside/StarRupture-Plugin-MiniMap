#pragma once

#include "Configuration.h"

namespace MiniMap::Services
{
    enum class WheelModifier : std::uint32_t
    {
        LeftControl = 1u, RightControl = 2u, LeftShift = 4u,
        RightShift = 8u, LeftAlt = 16u, RightAlt = 32u
    };
    struct WheelEvent
    {
        std::int32_t verticalRaw, horizontalRaw;
        std::uint32_t sidedModifiers;
        bool exclusiveUiCapture;
    };
    using WheelFn = bool (*)(void* user, const WheelEvent&);
    using ActionFn = void (*)(void* user);
    struct ActionSpec
    {
        SettingName preference;
        std::string_view initialCombo;
    };
    class IInput
    {
    public:
        virtual ~IInput() = default;
        virtual RegistrationReceipt<Registration> BindAction(const ActionSpec&, ActionFn, void* user) = 0;
        virtual RegistrationReceipt<Registration> BindDiagnostic(std::string_view combo, ActionFn, void* user) = 0;
        virtual RegistrationReceipt<Registration> OnWheel(WheelFn, void* user) = 0;
        virtual Removal Remove(Registration) = 0;
    };
}
