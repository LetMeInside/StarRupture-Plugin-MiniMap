#pragma once

#include "Services/Objects.h"

namespace MiniMap::Adapters::AlienX
{
    class Objects final : public Services::IObjects
    {
    public:
        bool Ready() const noexcept override;
        Services::Result<SDK::UObject*> FindResident(SDK::UClass* type,
            SDK::UObject* outer, std::wstring_view path, bool exactClass) override;
        Services::Result<Services::ObjectMatches> ClassDefaults(
            std::string_view className, std::span<Services::ObjectRecord> output) override;
        Services::Status Invoke(SDK::UObject* object, SDK::UFunction* function,
            void* parameterBuffer) override;
        Services::Status FreeEngineAllocation(void* allocation) noexcept override;
    };

    Objects& GetObjects() noexcept;
}
