#if defined(MODLOADER_CLIENT_BUILD)
#include "ExternalMapBuildingSessionAbi.h"
#include "ExternalMapBuildingSession.h"
#include "plugin_helpers.h"
#include <cstddef>
static_assert(sizeof(uint32_t)==4 && sizeof(uint64_t)==8 && sizeof(float)==4 && sizeof(double)==8);
static_assert(sizeof(MiniMapBuildingWorldRectangle)==32 && sizeof(MiniMapBuildingRenderRequest)==80);
static_assert(offsetof(MiniMapBuildingRenderRequest,Canvas)==56);
static_assert(sizeof(MiniMapBuildingRenderStatus)==24 && sizeof(MiniMapBuildingSessionApi)==24);
namespace
{
    // Retain the exact owner on rejected cleanup; no destructor after native shutdown.
    MiniMapBuildingCollector::ExternalMapBuildingSession& Session()
    { static auto* session=new MiniMapBuildingCollector::ExternalMapBuildingSession; return *session; }
    MiniMapBuildingAbiResult MINIMAP_BUILDING_ABI_CALL Render(
        const MiniMapBuildingRenderRequest* request,MiniMapBuildingRenderStatus* status)
    {
        if(!request || request->StructSize!=sizeof(*request) || !status || status->StructSize!=sizeof(*status))
            return MINIMAP_BUILDING_ABI_INVALID_ARGUMENT;
        *status={sizeof(*status),0,0,0};
        try { return Session().Render(*request,*status); }
        catch(...) { LOG_ERROR("MiniMap: external building render failed"); return MINIMAP_BUILDING_ABI_REJECTED; }
    }
    MiniMapBuildingAbiResult MINIMAP_BUILDING_ABI_CALL Close()
    { return Session().CloseSession()?MINIMAP_BUILDING_ABI_OK:MINIMAP_BUILDING_ABI_UNAVAILABLE; }
}
extern "C" MINIMAP_BUILDING_ABI_EXPORT MiniMapBuildingAbiResult MINIMAP_BUILDING_ABI_CALL
MiniMap_GetBuildingSessionApi(uint32_t requestedVersion,MiniMapBuildingSessionApi* api)
{
    if(!api || api->StructSize!=sizeof(*api))return MINIMAP_BUILDING_ABI_INVALID_ARGUMENT;
    if(requestedVersion!=MINIMAP_BUILDING_SESSION_ABI_VERSION)return MINIMAP_BUILDING_ABI_VERSION_MISMATCH;
    *api={sizeof(*api),MINIMAP_BUILDING_SESSION_ABI_VERSION,&Render,&Close};
    return MINIMAP_BUILDING_ABI_OK;
}
namespace MiniMapBuildingCollector
{
    void ShutdownExternalMapBuildingSessionAbi(){Session().Shutdown();}
    void InvalidateExternalMapBuildingSessionAbi(){Session().InvalidateOnWorldEnd();}
}
#endif
