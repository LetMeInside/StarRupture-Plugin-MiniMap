#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <cstdint>
namespace MiniMapBuildingComplexApiInternal
{
    bool ServiceCollectorForVisibleRequests(uint64_t session) noexcept;
    void ServiceRequests(uint64_t session) noexcept;
}
#endif
