#pragma once

#include "ServiceTypes.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace SDK
{
    class UObject;
    class UClass;
    class UFunction;
}

namespace MiniMap::Services
{
    struct ObjectRecord
    {
        SDK::UObject* object = nullptr;
        std::string objectName;
        std::string className;
    };

    struct ObjectMatches
    {
        std::size_t total = 0;
    };

    class IObjects
    {
    public:
        virtual ~IObjects() = default;
        virtual bool Ready() const noexcept = 0;
        virtual Result<SDK::UObject*> FindResident(SDK::UClass* type,
            SDK::UObject* outer, std::wstring_view path, bool exactClass) = 0;
        virtual Result<ObjectMatches> ClassDefaults(std::string_view className,
            std::span<ObjectRecord> output) = 0;
        virtual Status Invoke(SDK::UObject* object, SDK::UFunction* function,
            void* parameterBuffer) = 0;
        virtual Status FreeEngineAllocation(void* allocation) noexcept = 0;
    };
}
