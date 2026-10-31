#if defined(MODLOADER_CLIENT_BUILD)
#include "ExternalMapBuildingSessionAbi.h"
#include "ExternalMapBuildingSession.h"
#include <cmath>

static_assert(sizeof(uint32_t)==4 && sizeof(uint64_t)==8 && sizeof(float)==4 && sizeof(double)==8);

namespace
{
    MiniMapBuildingCollector::ExternalMapBuildingSession& Session()
    {
        static MiniMapBuildingCollector::ExternalMapBuildingSession session;
        return session;
    }

    bool Convert(const MiniMapBuildingWorldRectangleV1* input,MiniMapBuildingCollector::Bounds& output)
    {
        if(!input || !std::isfinite(input->MinX) || !std::isfinite(input->MinY) ||
            !std::isfinite(input->MaxX) || !std::isfinite(input->MaxY) ||
            input->MinX>=input->MaxX || input->MinY>=input->MaxY)return false;
        output={input->MinX,input->MinY,input->MaxX,input->MaxY,true};
        return true;
    }

    MiniMapBuildingAbiResult MINIMAP_BUILDING_ABI_CALL Open(
        const MiniMapBuildingWorldRectangleV1* rectangle)
    {
        MiniMapBuildingCollector::Bounds bounds{};
        if(!Convert(rectangle,bounds))return MINIMAP_BUILDING_ABI_INVALID_ARGUMENT;
        return Session().OpenSession(bounds)?MINIMAP_BUILDING_ABI_OK:MINIMAP_BUILDING_ABI_REJECTED;
    }
    MiniMapBuildingAbiResult MINIMAP_BUILDING_ABI_CALL Update(
        const MiniMapBuildingWorldRectangleV1* rectangle)
    {
        MiniMapBuildingCollector::Bounds bounds{};
        if(!Convert(rectangle,bounds))return MINIMAP_BUILDING_ABI_INVALID_ARGUMENT;
        return Session().UpdateWorldRectangle(bounds)?MINIMAP_BUILDING_ABI_OK:MINIMAP_BUILDING_ABI_REJECTED;
    }
    MiniMapBuildingAbiResult MINIMAP_BUILDING_ABI_CALL Service(float deltaSeconds)
    {
        if(!std::isfinite(deltaSeconds) || deltaSeconds<0)return MINIMAP_BUILDING_ABI_INVALID_ARGUMENT;
        const auto before=Session().GetDiagnostics();
        if(!before.Active)return MINIMAP_BUILDING_ABI_NOT_ACTIVE;
        Session().Service(deltaSeconds);
        const auto after=Session().GetDiagnostics();
        return after.Active && after.Admitted?MINIMAP_BUILDING_ABI_OK:MINIMAP_BUILDING_ABI_UNAVAILABLE;
    }
    MiniMapBuildingAbiResult MINIMAP_BUILDING_ABI_CALL ReadStatus(MiniMapBuildingSessionStatusV1* status)
    {
        if(!status || status->StructSize<sizeof(MiniMapBuildingSessionStatusV1))
            return MINIMAP_BUILDING_ABI_INVALID_ARGUMENT;
        const auto diagnostics=Session().GetDiagnostics();
        MiniMapBuildingSessionStatusV1 result{};
        result.StructSize=sizeof(result);
        if(diagnostics.Active)result.Flags|=MINIMAP_BUILDING_STATUS_ACTIVE;
        if(diagnostics.Admitted)result.Flags|=MINIMAP_BUILDING_STATUS_ADMITTED;
        if(diagnostics.Collector.Ready)result.Flags|=MINIMAP_BUILDING_STATUS_COLLECTOR_READY;
        if(diagnostics.Collector.MembershipComplete)result.Flags|=MINIMAP_BUILDING_STATUS_MEMBERSHIP_COMPLETE;
        if(diagnostics.WorldInvalidated)result.Flags|=MINIMAP_BUILDING_STATUS_WORLD_INVALIDATED;
        result.SessionGeneration=diagnostics.SessionGeneration;
        result.CollectorPublicationGeneration=diagnostics.Collector.PublicationGeneration;
        result.AuthoritativeBuildingCount=diagnostics.Collector.AuthoritativeBuildingCount;
        result.CompletedBuildingCount=diagnostics.Collector.CompletedBuildingCount;
        result.PublishedInRegionCount=diagnostics.Collector.PublishedInRegionCount;
        result.PendingAcquisitionCount=diagnostics.Collector.PendingAcquisitionCount;
        *status=result;
        return MINIMAP_BUILDING_ABI_OK;
    }
    MiniMapBuildingAbiResult MINIMAP_BUILDING_ABI_CALL Close()
    {
        const auto diagnostics=Session().GetDiagnostics();
        if(diagnostics.Active && !diagnostics.Admitted)return MINIMAP_BUILDING_ABI_UNAVAILABLE;
        Session().CloseSession();
        return MINIMAP_BUILDING_ABI_OK;
    }
}

extern "C" MINIMAP_BUILDING_ABI_EXPORT MiniMapBuildingAbiResult MINIMAP_BUILDING_ABI_CALL
MiniMap_GetBuildingSessionApi(uint32_t requestedVersion,MiniMapBuildingSessionApiV1* api)
{
    if(!api || api->StructSize<sizeof(MiniMapBuildingSessionApiV1))
        return MINIMAP_BUILDING_ABI_INVALID_ARGUMENT;
    if(requestedVersion!=MINIMAP_BUILDING_SESSION_ABI_VERSION)
        return MINIMAP_BUILDING_ABI_VERSION_MISMATCH;
    MiniMapBuildingSessionApiV1 result{};
    result.StructSize=sizeof(result);
    result.Version=MINIMAP_BUILDING_SESSION_ABI_VERSION;
    result.Capabilities=MINIMAP_BUILDING_ABI_CAP_SESSION|MINIMAP_BUILDING_ABI_CAP_DIAGNOSTICS;
    result.OpenSession=&Open;
    result.UpdateWorldRectangle=&Update;
    result.Service=&Service;
    result.ReadStatus=&ReadStatus;
    result.CloseSession=&Close;
    *api=result;
    return MINIMAP_BUILDING_ABI_OK;
}

namespace MiniMapBuildingCollector
{
    void ShutdownExternalMapBuildingSessionAbi(){Session().Shutdown();}
}
#endif
