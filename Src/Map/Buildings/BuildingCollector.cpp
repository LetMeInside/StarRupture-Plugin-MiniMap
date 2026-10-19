#include "CallbackTiming.h"
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingCollector.h"
#include "BuildingGeometry.h"
#include "BuildingInventory.h"
#include "BuildingVisuals.h"
#include "BuildingBlueprintIdentity.h"
#include "Native/ResidentMetadata.h"
#include "DebugText.h"
#include "Map/Map.h"
#include "Native/NativeApi.h"
#include "plugin_helpers.h"
#include "SDK/Chimera_classes.hpp"

#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <string_view>
#if MINIMAP_DEBUG_UI
#include <iomanip>
#include <sstream>
#endif

namespace
{
    template<class... Args> void SpriteDiagnostic(const char* format, Args... args) noexcept
    {
        try { if (auto* self=GetSelf(); self && self->logger && self->logger->Warn) self->logger->Warn(self,format,args...); }
        catch (...) {} // Diagnostics never participate in collector admission.
    }
    using namespace MiniMapBuildingCollector;
    using Clock = std::chrono::steady_clock;
    using Id = MiniMapNative::MassEntityHandle;
    using Native = MiniMapNative::NativeApi;
    constexpr double kOriginPadding = MiniMapBuildingGeometry::kOrdinarySafetyAllowance;
    constexpr double kSplinePadding = MiniMapBuildingGeometry::kSplineSafetyAllowance;
    constexpr double kMovementPadding = 5000.0;
    constexpr double kFallbackVerticalExtent = 100000.0; // Original policy if optional XY traversal is unavailable.
    constexpr auto kLocalCadence = std::chrono::milliseconds(200);
    constexpr auto kIdentityCadence = std::chrono::seconds(1);
    constexpr auto kDistantCadence = std::chrono::seconds(2);
    constexpr auto kLogCadence = std::chrono::seconds(5);
    constexpr double kRefreshBudgetMs = 2.0; // Soft budget, checked after each entity.
    constexpr int kMaximumHandles = 1000000;

    struct Types
    {
        SDK::UScriptStruct *Building=nullptr, *Grid=nullptr, *Transform=nullptr,
            *Parameters=nullptr, *Spline=nullptr, *Tint=nullptr, *TargetBox=nullptr;
        SDK::UClass *Placement=nullptr, *BuildingData=nullptr, *GridClass=nullptr;
        SDK::UClass *Config=nullptr, *TargetTrait=nullptr, *VisualTrait=nullptr,
            *CosmeticTrait=nullptr, *DroneRail=nullptr, *Walkway=nullptr, *StaticMesh=nullptr,
            *DronesSettings=nullptr;
    };
    struct Query
    {
        alignas(16) std::array<std::byte, 0x350> Storage = {};
        bool Constructed = false;
    };
    struct Entry
    {
        Record Data;
        uint64_t Epoch = 0;
        Clock::time_point Due = {};
        bool Read = false;
        uint8_t PreviousSources = 0;
    };
    struct Definition
    {
        uint32_t Id; uint8_t Category; int32_t NameIndex; uint32_t NameNumber;
        std::shared_ptr<const DefinitionGeometry> Geometry;
        std::shared_ptr<const std::string> CanonicalBlueprint;
        int32_t ObjectSerial=0; // Observed UObject lifetime, not a session generation or pointer.
        uint64_t IdentityObservation=0, GeometryObservation=0;
        bool IdentityInterrupted=false, GeometryHasSpline=false;
        uint64_t IdentityDiagnostics=0; // Once per issue for this existing definition owner; no identity registry.
        bool InteriorTagChecked=false, InteriorOnly=false;
    };
    struct Metrics
    {
        double GridQuery=0, GridExtract=0, OffQuery=0, SplineQuery=0, Reconcile=0,
            Dynamic=0, BoundsTime=0, OffDynamic=0, OffBounds=0, CacheService=0,
            Filter=0, SnapshotTime=0, Total=0, MaxTick=0, GridMax=0,
            DefinitionTime=0, OrdinaryTime=0, SplineTime=0;
        double SnapshotBuild=0;
        size_t Added=0, Removed=0, OffAdded=0, OffRemoved=0, DefinitionMiss=0, Stale=0, Refreshed=0, Polls=0, IdentityPolls=0, GridCalls=0;
        size_t NewAttempts=0, RetryAttempts=0, ResolvedNow=0, CachedReuse=0, DynamicReads=0,
            TransformReads=0, MembershipTransforms=0, MembershipTags=0, PlacementReads=0, Conversions=0, ConversionSuccess=0,
            InteriorReads=0, OrdinaryGeometry=0, OrdinaryDefinitions=0, SnapshotsBuilt=0, SnapshotsReused=0;
    };
    IPluginSelf* g_collectorSelf=nullptr;
    bool g_initialized=false, g_ready=false, g_typesReady=false, g_unavailableLogged=false;
    uint32_t g_definitionDiagnostics=0; // Four pre-definition categories per collector lifetime/generation.
    bool g_gridSucceeded=false, g_identitySucceeded=false;
    SDK::UWorld* g_world=nullptr;
    const void* g_manager=nullptr; // Game-thread query owner identity only; never published.
    Types g_types;
    Query g_offQuery, g_splineQuery;
    uint64_t g_generation=0, g_epoch=0;
    std::unordered_map<uint64_t,Entry> g_cache;
    std::unordered_map<uint64_t,Record> g_indexedCopies; // Last copied observations; membership follows successful grid enumerations.
    uint64_t g_observation=0;
    std::unordered_map<uint64_t,Definition> g_definitions;
    std::vector<uint64_t> g_keys, g_near, g_pending;
    size_t g_cursor=0, g_nearCursor=0;
    Bounds g_region, g_queryRegion;
    double g_queryZ=0, g_lastRadius=0;
    double g_ordinaryQueryPadding=kOriginPadding;
    std::mutex g_mutex;
    double g_viewRadius=0;
    std::shared_ptr<const Snapshot> g_snapshot;
    Clock::time_point g_nextLocal={}, g_nextIdentity={}, g_nextLog={}, g_nextWarning={};
    Metrics g_metrics;
    bool g_snapshotDirty=true, g_publishedGrid=false, g_publishedIdentity=false;
#if MINIMAP_DEBUG_UI
    struct DebugOracle
    {
        bool Valid=false;
        size_t Missing=0, Repeated=0, Stale=0, Duplicate=0;
        size_t Expected=0, Present=0, UnresolvedCached=0, UnresolvedWorldwide=0;
    };
    DebugOracle g_debugOracle;
    double g_debugLastGridMs=0;
#endif
    SplineGeometry g_curveScratch; // Game-thread scratch retains capacity; unchanged curves reuse ownership.
    size_t g_indexed=0, g_gridResults=0, g_duplicates=0;
    std::vector<Id> g_indexedHandles; // Retains plugin-owned capacity; no grid shared-pointer members.
    size_t g_gridVisited=0;
    bool g_xyQuery=false;
    unsigned g_initialDiscoveryReports=0;

    uint64_t Key(int32_t index,int32_t serial)
    { return (uint64_t(uint32_t(serial))<<32)|uint32_t(index); }
    uint64_t Key(Id id) { return Key(id.Index,id.SerialNumber); }
    double Ms(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
    double Seconds() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
    void ClearComparison();
    Bounds Box(double x,double y,double r) { return {x-r,y-r,x+r,y+r,true}; }
    Bounds Pad(Bounds b,double r)
    { if(b.Valid) { b.MinX-=r; b.MinY-=r; b.MaxX+=r; b.MaxY+=r; } return b; }
    bool Contains(const Bounds& a,const Bounds& b)
    { return a.Valid && b.Valid && a.MinX<=b.MinX && a.MinY<=b.MinY && a.MaxX>=b.MaxX && a.MaxY>=b.MaxY; }
    bool Live(const Native& n,Id id)
    {
        // InternalGetFragmentDataPtr asserts on an unbuilt entity. IsEntityBuilt
        // itself asserts on invalid identity: the order of these checks matters.
        return n.mass.isEntityValid(g_manager,id) && n.mass.isEntityBuilt(g_manager,id);
    }
    enum class ClassObservation { Unavailable, Other, Matches };
    ClassObservation ObserveClass(const SDK::UObject* object,const SDK::UClass* base)
    {
        if(!object || !base || !MiniMapResidentMetadata::Available() ||
            !MiniMapResidentMetadata::Validate(reinterpret_cast<uintptr_t>(object),sizeof(SDK::UObject))) return ClassObservation::Unavailable;
        const SDK::UStruct* type=object->Class;
        if(!type)return ClassObservation::Unavailable;
        for(unsigned depth=0;type && depth<64;++depth,type=type->SuperStruct)
        {
            // base is acquired/rooted by ResolveTypes for this session. Equality
            // reads no memory through type; validate only before following an
            // otherwise unverified superclass pointer.
            if(type==base)
            {
#if !defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
                // The legacy metadata-walker configuration does not own roots.
                if(!MiniMapResidentMetadata::Validate(reinterpret_cast<uintptr_t>(type),sizeof(SDK::UStruct))) return ClassObservation::Unavailable;
#endif
                return ClassObservation::Matches;
            }
            if(!MiniMapResidentMetadata::Validate(reinterpret_cast<uintptr_t>(type),sizeof(SDK::UStruct))) return ClassObservation::Unavailable;
        }
        return type?ClassObservation::Unavailable:ClassObservation::Other;
    }
    bool IsClass(const SDK::UObject* object,const SDK::UClass* base)
    { return ObserveClass(object,base)==ClassObservation::Matches; }
    enum class InteriorTagResult : uint8_t { Unknown, Absent, Present };
    InteriorTagResult ReadInteriorTag(const SDK::UCrBuildingData* building,bool production=false)
    {
        if(production)++g_metrics.InteriorReads;
#if defined(MINIMAP_STAGE_R26B4_CLASSIFICATION)
        const auto* native=MiniMapNative::Get();
        if(!native || !native->asset.nameEqualsWide) return InteriorTagResult::Unknown;
        const auto* tags=&building->Tags;
        // Both callers have just validated sizeof(UCrBuildingData). Tags is
        // inline in that same range; only its separate allocation needs a query.
        const int32_t count=tags->Num(),capacity=tags->Max();
        const auto* data=tags->GetDataPtr();
        constexpr int32_t maximumTags=4096;
        if(count<0 || capacity<count || capacity>maximumTags || (capacity&&!data) || (!capacity&&data) ||
            (data&&(reinterpret_cast<uintptr_t>(data)&(alignof(SDK::FName)-1))))
            return InteriorTagResult::Unknown;
        const size_t bytes=size_t(count)*sizeof(SDK::FName);
        if(bytes&&!MiniMapNative::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(data),bytes,alignof(SDK::FName)))
            return InteriorTagResult::Unknown;
        bool invalidElement=false;
        for(int32_t i=0;i<count;++i)
        {
            const auto name=data[i];
            if(name.ComparisonIndex<0) { invalidElement=true; continue; }
            if(name.Number==0 && native->asset.nameEqualsWide(name,L"Interior"))
                return InteriorTagResult::Present;
        }
        return invalidElement?InteriorTagResult::Unknown:InteriorTagResult::Absent;
#else
        (void)building;
        return InteriorTagResult::Unknown;
#endif
    }
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
    std::array<uintptr_t,18> g_metadataRoots{};
    size_t g_metadataCount=0;
    bool ResolveTypes(bool splineMetadata=false)
    {
        MiniMapCallbackTiming::Phase timing(MiniMapCallbackTiming::Id::CollectorMetadata);
        if(g_typesReady && (!splineMetadata || (g_types.VisualTrait && g_types.CosmeticTrait && g_types.DroneRail && g_types.Walkway && g_types.StaticMesh && g_types.DronesSettings)))return true;
        if(!MiniMapResidentMetadata::Available())return false;
        // Resolve the finite metadata set while native admission permits it.
        // Rooted successes are reused; unavailable operations retry on acquisition.
        for(size_t step=0;step<(splineMetadata?g_metadataRoots.size():size_t(12));++step)
        {
            if(!MiniMapResidentMetadata::Available())return false;
            uintptr_t acquired=0;
            switch(step)
            {
            case 0: if(!g_types.Building){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Chimera.CrMassBuildingTag",true,sizeof(SDK::FCrMassBuildingTag));g_types.Building=reinterpret_cast<SDK::UScriptStruct*>(acquired);}break;
            case 1: if(!g_types.Grid){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Chimera.CrMassEntityGridTag",true,sizeof(SDK::FCrMassEntityGridTag));g_types.Grid=reinterpret_cast<SDK::UScriptStruct*>(acquired);}break;
            case 2: if(!g_types.Transform){acquired=MiniMapResidentMetadata::Acquire(L"/Script/MassCommon.TransformFragment",true,sizeof(SDK::FTransform));g_types.Transform=reinterpret_cast<SDK::UScriptStruct*>(acquired);}break;
            case 3: if(!g_types.Parameters){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Chimera.CrBuildingParameters",true,sizeof(SDK::FCrBuildingParameters));g_types.Parameters=reinterpret_cast<SDK::UScriptStruct*>(acquired);}break;
            case 4: if(!g_types.Spline){acquired=MiniMapResidentMetadata::Acquire(L"/Script/AuActorPlacement.AuSplineConnectionFragment",true,sizeof(SDK::FAuSplineConnectionFragment));g_types.Spline=reinterpret_cast<SDK::UScriptStruct*>(acquired);}break;
            case 5: if(!g_types.Tint){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Chimera.CrBuildingVisualCustomizationFragment",true,sizeof(SDK::FCrBuildingVisualCustomizationFragment));g_types.Tint=reinterpret_cast<SDK::UScriptStruct*>(acquired);}break;
            case 6: if(!g_types.Placement){acquired=MiniMapResidentMetadata::Acquire(L"/Script/AuActorPlacement.AuActorPlacementData",false,sizeof(SDK::UAuActorPlacementData));g_types.Placement=reinterpret_cast<SDK::UClass*>(acquired);}break;
            case 7: if(!g_types.BuildingData){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Chimera.CrBuildingData",false,sizeof(SDK::UCrBuildingData));g_types.BuildingData=reinterpret_cast<SDK::UClass*>(acquired);}break;
            case 8: if(!g_types.GridClass){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Chimera.CrEntityGridSubsystem",false,sizeof(SDK::UCrEntityGridSubsystem));g_types.GridClass=reinterpret_cast<SDK::UClass*>(acquired);}break;
            case 9: if(!g_types.TargetBox){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Chimera.CrBuildingAggroTargetDataFragment",true,sizeof(SDK::FCrBuildingAggroTargetDataFragment));g_types.TargetBox=reinterpret_cast<SDK::UScriptStruct*>(acquired);}break;
            case 10: if(!g_types.Config){acquired=MiniMapResidentMetadata::Acquire(L"/Script/MassSpawner.MassEntityConfigAsset",false,sizeof(SDK::UMassEntityConfigAsset));g_types.Config=reinterpret_cast<SDK::UClass*>(acquired);}break;
            case 11: if(!g_types.TargetTrait){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Chimera.CrBuildingAggroTargetDataTrait",false,sizeof(SDK::UCrBuildingAggroTargetDataTrait));g_types.TargetTrait=reinterpret_cast<SDK::UClass*>(acquired);}break;
            case 12: if(!g_types.VisualTrait){acquired=MiniMapResidentMetadata::Acquire(L"/Script/MassRepresentation.MassVisualizationTrait",false,sizeof(SDK::UMassVisualizationTrait));g_types.VisualTrait=reinterpret_cast<SDK::UClass*>(acquired);}break;
            case 13: if(!g_types.CosmeticTrait){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Chimera.CrMassRepVisCosmeticsTrait",false,sizeof(SDK::UCrMassRepVisCosmeticsTrait));g_types.CosmeticTrait=reinterpret_cast<SDK::UClass*>(acquired);}break;
            case 14: if(!g_types.DroneRail){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Chimera.CrDronePathPointConnection",false,sizeof(SDK::ACrDronePathPointConnection));g_types.DroneRail=reinterpret_cast<SDK::UClass*>(acquired);}break;
            case 15: if(!g_types.Walkway){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Chimera.CrModularRampWalkway",false,sizeof(SDK::ACrModularRampWalkway));g_types.Walkway=reinterpret_cast<SDK::UClass*>(acquired);}break;
            case 16: if(!g_types.StaticMesh){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Engine.StaticMesh",false,sizeof(SDK::UStaticMesh));g_types.StaticMesh=reinterpret_cast<SDK::UClass*>(acquired);}break;
            case 17: if(!g_types.DronesSettings){acquired=MiniMapResidentMetadata::Acquire(L"/Script/Chimera.DronesDeveloperSettings",false,sizeof(SDK::UDronesDeveloperSettings));g_types.DronesSettings=reinterpret_cast<SDK::UClass*>(acquired);}break;
            }
            if(acquired)g_metadataRoots[g_metadataCount++]=acquired;
        }
        g_typesReady=g_types.Building && g_types.Grid && g_types.Transform && g_types.Parameters &&
            g_types.Spline && g_types.Placement && g_types.BuildingData && g_types.GridClass;
        return g_typesReady;
    }
#else
    SDK::UObject* Metadata(const char* name,const char* kind,size_t size)
    {
        PluginObjectInfo info={};
        if(g_collectorSelf->hooks->ObjectWalker->FindObjectsByNameInto(name,PluginObjectLookup_Both,&info,1)!=1 ||
            std::strcmp(info.className,kind)!=0) return nullptr;
        auto* type=static_cast<SDK::UStruct*>(info.object);
        return type->Size==int32_t(size)?type:nullptr;
    }
    bool ResolveTypes(bool splineMetadata=false)
    {
        if(g_typesReady && (!splineMetadata || (g_types.VisualTrait && g_types.CosmeticTrait && g_types.DroneRail && g_types.Walkway && g_types.StaticMesh && g_types.DronesSettings))) return true;
        if(!g_collectorSelf->hooks->ObjectWalker->IsReady()) return false;
        g_types.Building=static_cast<SDK::UScriptStruct*>(Metadata("CrMassBuildingTag","ScriptStruct",sizeof(SDK::FCrMassBuildingTag)));
        g_types.Grid=static_cast<SDK::UScriptStruct*>(Metadata("CrMassEntityGridTag","ScriptStruct",sizeof(SDK::FCrMassEntityGridTag)));
        g_types.Transform=static_cast<SDK::UScriptStruct*>(Metadata("TransformFragment","ScriptStruct",sizeof(SDK::FTransform)));
        g_types.Parameters=static_cast<SDK::UScriptStruct*>(Metadata("CrBuildingParameters","ScriptStruct",sizeof(SDK::FCrBuildingParameters)));
        g_types.Spline=static_cast<SDK::UScriptStruct*>(Metadata("AuSplineConnectionFragment","ScriptStruct",sizeof(SDK::FAuSplineConnectionFragment)));
        g_types.Tint=static_cast<SDK::UScriptStruct*>(Metadata("CrBuildingVisualCustomizationFragment","ScriptStruct",sizeof(SDK::FCrBuildingVisualCustomizationFragment)));
        g_types.Placement=static_cast<SDK::UClass*>(Metadata("AuActorPlacementData","Class",sizeof(SDK::UAuActorPlacementData)));
        g_types.BuildingData=static_cast<SDK::UClass*>(Metadata("CrBuildingData","Class",sizeof(SDK::UCrBuildingData)));
        g_types.GridClass=static_cast<SDK::UClass*>(Metadata("CrEntityGridSubsystem","Class",sizeof(SDK::UCrEntityGridSubsystem)));
        // Optional geometry sources. Missing metadata never disables B.1 acquisition.
        g_types.TargetBox=static_cast<SDK::UScriptStruct*>(Metadata("CrBuildingAggroTargetDataFragment","ScriptStruct",sizeof(SDK::FCrBuildingAggroTargetDataFragment)));
        g_types.Config=static_cast<SDK::UClass*>(Metadata("MassEntityConfigAsset","Class",sizeof(SDK::UMassEntityConfigAsset)));
        g_types.TargetTrait=static_cast<SDK::UClass*>(Metadata("CrBuildingAggroTargetDataTrait","Class",sizeof(SDK::UCrBuildingAggroTargetDataTrait)));
        if(splineMetadata)
        {
            g_types.VisualTrait=static_cast<SDK::UClass*>(Metadata("MassVisualizationTrait","Class",sizeof(SDK::UMassVisualizationTrait)));
            g_types.CosmeticTrait=static_cast<SDK::UClass*>(Metadata("CrMassRepVisCosmeticsTrait","Class",sizeof(SDK::UCrMassRepVisCosmeticsTrait)));
            g_types.DroneRail=static_cast<SDK::UClass*>(Metadata("CrDronePathPointConnection","Class",sizeof(SDK::ACrDronePathPointConnection)));
            g_types.Walkway=static_cast<SDK::UClass*>(Metadata("CrModularRampWalkway","Class",sizeof(SDK::ACrModularRampWalkway)));
            g_types.StaticMesh=static_cast<SDK::UClass*>(Metadata("StaticMesh","Class",sizeof(SDK::UStaticMesh)));
            g_types.DronesSettings=static_cast<SDK::UClass*>(Metadata("DronesDeveloperSettings","Class",sizeof(SDK::UDronesDeveloperSettings)));
        }
        g_typesReady=g_types.Building && g_types.Grid && g_types.Transform && g_types.Parameters &&
            g_types.Spline && g_types.Placement && g_types.BuildingData && g_types.GridClass;
        return g_typesReady;
    }
#endif
    const void* Fragment(const Native& n,Id id,const SDK::UScriptStruct* type)
    { return type?n.mass.getFragmentDataPtr(g_manager,id,type):nullptr; }
    const SDK::FCrBuildingParameters* Parameters(const Native& n,Id id,bool production=false)
    {
        if(!MiniMapResidentMetadata::Available())return nullptr;
        if(production)++g_metrics.PlacementReads;
        const auto* wrapper=n.mass.getConstSharedFragmentPtr(g_manager,id,g_types.Parameters);
        const uint8_t* memory=nullptr;
        // The synchronous Mass API borrows manager-owned shared storage. The
        // verified native caller reads this wrapper and aligned payload directly
        // (Fingerprints.cpp InternalGetConstSharedFragmentPtr anchor). No yield,
        // entity mutation or retained pointer intervenes in this acquisition.
        if(wrapper && !(reinterpret_cast<uintptr_t>(wrapper)&(alignof(void*)-1)))
            std::memcpy(&memory,wrapper,sizeof(memory));
        const auto alignment=g_types.Parameters->MinAlignment;
        if(!memory || alignment<=0 || (alignment&(alignment-1))) return nullptr;
        const auto raw=reinterpret_cast<uintptr_t>(memory);
        if(raw>(std::numeric_limits<uintptr_t>::max)()-sizeof(void*)-uintptr_t(alignment-1))return nullptr;
        const uintptr_t base=raw+sizeof(void*);
        const auto address=(base+alignment-1)&~uintptr_t(alignment-1);
        if(address>(std::numeric_limits<uintptr_t>::max)()-sizeof(SDK::FCrBuildingParameters))return nullptr;
        return reinterpret_cast<const SDK::FCrBuildingParameters*>(address);
    }
    DefinitionGeometry ResolveGeometry(const SDK::UAuActorPlacementData* placement,bool spline)
    {
        DefinitionGeometry shape;
        if(spline)ResolveTypes(true); // Optional metadata is never an ordinary-building prerequisite.
        if(!placement || !g_types.Config || !MiniMapResidentMetadata::Available() ||
            !MiniMapResidentMetadata::Validate(reinterpret_cast<uintptr_t>(placement),sizeof(SDK::UAuActorPlacementData))) return shape;
        auto readTemplate=[&](const SDK::UClass* type)
        {
            // Do not call GetDefaultObject: that can create/load a CDO. Only
            // already resident, hard-referenced template defaults are inspected.
            const auto* object=type?type->ClassDefaultObject:nullptr;
            const SDK::UStaticMesh* mesh=nullptr;
            if(IsClass(object,g_types.DroneRail))
            {
                mesh=static_cast<const SDK::ACrDronePathPointConnection*>(object)->SplineMesh;
                const auto* settings=g_types.DronesSettings?g_types.DronesSettings->ClassDefaultObject:nullptr;
                if(IsClass(settings,g_types.DronesSettings))
                {
                    const double extension=static_cast<const SDK::UDronesDeveloperSettings*>(settings)->RailMeshVisualExtraLength;
                    if(std::isfinite(extension) && std::abs(extension)<=MiniMapBuildingGeometry::kMaximumGeometrySpan)
                    { shape.RailEndpointExtension=std::abs(extension); shape.HasRailExtension=true; }
                }
            }
            else if(IsClass(object,g_types.Walkway))
                mesh=static_cast<const SDK::ACrModularRampWalkway*>(object)->SplineMesh;
            if(!IsClass(mesh,g_types.StaticMesh)) return;
            // CL-127004 rail/walkway builders use forward axis X. ExtendedBounds
            // is mesh-local, not world space. Include origin offsets and Z so a
            // tilted/rolled cross section cannot exceed this radial allowance.
            const auto& b=mesh->ExtendedBounds;
            const Point extent{b.BoxExtent.X,b.BoxExtent.Y,b.BoxExtent.Z};
            const Point origin{b.Origin.X,b.Origin.Y,b.Origin.Z};
            if(!MiniMapBuildingGeometry::Finite(extent) || !MiniMapBuildingGeometry::Finite(origin) ||
                std::any_of(extent.begin(),extent.end(),[](double v){return v<0;}))
            { shape.Issue=GeometryIssue::NonFinite; return; }
            const double width=std::hypot(std::abs(origin[1])+extent[1],std::abs(origin[2])+extent[2]);
            if(!std::isfinite(width) || width<=0 || width>MiniMapBuildingGeometry::kMaximumGeometrySpan)
            { shape.Issue=GeometryIssue::Oversized; return; }
            if(!shape.HasSplineMesh || width>shape.SplineMeshCrossSection)
            {
                shape.SplineMeshCrossSection=width; shape.HasSplineMesh=true;
                shape.VisualClassObjectIndex=type->Index;
                shape.SplineMeshBounds.Valid=true;
                for(size_t axis=0;axis<3;++axis)
                { shape.SplineMeshBounds.Min[axis]=origin[axis]-extent[axis]; shape.SplineMeshBounds.Max[axis]=origin[axis]+extent[axis]; }
                shape.SplineMeshObjectIndex=mesh->Index;
                shape.SplineMeshNameIndex=mesh->Name.ComparisonIndex;
                shape.SplineMeshNameNumber=mesh->Name.Number;
            }
        };
        const auto* config=placement->EntityType.EntityConfigPtr;
        std::array<const SDK::UMassEntityConfigAsset*,32> visited={};
        for(size_t depth=0;config && depth<visited.size();++depth)
        {
            if((reinterpret_cast<uintptr_t>(config)&7) || !IsClass(config,g_types.Config) ||
                !MiniMapResidentMetadata::Validate(reinterpret_cast<uintptr_t>(config),sizeof(SDK::UMassEntityConfigAsset)) ||
                std::find(visited.begin(),visited.begin()+depth,config)!=visited.begin()+depth)
            { shape.Issue=GeometryIssue::InvalidArray; return shape; }
            visited[depth]=config;
            const auto& traits=config->Config.Traits;
            if(traits.Num()<0 || traits.Num()>256 || traits.Max()<traits.Num() || (traits.Num() && !traits.GetDataPtr()))
            { shape.Issue=GeometryIssue::InvalidArray; return shape; }
            if(traits.Num() && !MiniMapNative::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(traits.GetDataPtr()),
                size_t(traits.Num())*sizeof(void*),alignof(void*)))
            { shape.Issue=GeometryIssue::InvalidArray; return shape; }
            for(const auto* trait:traits)
            {
                if(reinterpret_cast<uintptr_t>(trait)&7) { shape.Issue=GeometryIssue::InvalidArray; return shape; }
                if(!shape.TargetingBox.Valid && IsClass(trait,g_types.TargetTrait) &&
                    MiniMapResidentMetadata::Validate(reinterpret_cast<uintptr_t>(trait),sizeof(SDK::UCrBuildingAggroTargetDataTrait)))
                    shape.TargetingBox=MiniMapBuildingGeometry::Copy(static_cast<const SDK::UCrBuildingAggroTargetDataTrait*>(trait)->BuildingBoundingBox);
                if(spline && IsClass(trait,g_types.CosmeticTrait))
                {
                    const auto* visual=static_cast<const SDK::UCrMassRepVisCosmeticsTrait*>(trait);
                    readTemplate(visual->HighResTemplateActor); readTemplate(visual->LowResTemplateActor);
                }
                else if(spline && IsClass(trait,g_types.VisualTrait))
                {
                    const auto* visual=static_cast<const SDK::UMassVisualizationTrait*>(trait);
                    readTemplate(visual->HighResTemplateActor); readTemplate(visual->LowResTemplateActor);
                }
            }
            config=config->Config.Parent;
        }
        // Partial resident sources are useful, but do not establish full visual
        // coverage. In particular Blueprint construction scripts, dynamic poles,
        // foundations and mesh/component variants may add geometry.
        return shape;
    }
    std::shared_ptr<const DefinitionGeometry> DefinitionShape(Definition& definition,const SDK::UAuActorPlacementData* placement,bool spline,bool measure=true)
    {
        // Ordinary definition geometry is copied once. The independent oracle
        // passes measure=false and still observes native geometry for comparison.
        if(measure && !spline && definition.Geometry)return definition.Geometry;
        if(definition.GeometryObservation!=g_observation || (spline && !definition.GeometryHasSpline))
        {
            definition.GeometryObservation=g_observation;
            definition.GeometryHasSpline=spline;
            const auto start=Clock::now();
            if(measure && !spline)++g_metrics.OrdinaryDefinitions;
            auto shape=ResolveGeometry(placement,spline);
            if(!definition.Geometry || !(*definition.Geometry==shape))
                definition.Geometry=std::make_shared<const DefinitionGeometry>(std::move(shape));
            if(measure) g_metrics.DefinitionTime+=Ms(start);
        }
        return definition.Geometry;
    }
    void DefinitionBlueprint(Definition& definition,const SDK::UAuActorPlacementData* placement,const Record& record)
    {
        if(definition.CanonicalBlueprint || definition.IdentityInterrupted || definition.IdentityObservation==g_observation) return;
        definition.IdentityObservation=g_observation; // Memoize this definition's observation, not a quota across types.
        ++g_metrics.Conversions; // Production resolver attempts; oracle calls are separate.
        const auto result=MiniMapBuildingBlueprintIdentity::Resolve(placement,g_types.Placement,
            {record.DefinitionObjectIndex,record.DefinitionIndex,record.DefinitionNumber,record.BuildingId,definition.ObjectSerial});
        if(result.Path)++g_metrics.ConversionSuccess;
        // An invalid/unavailable authored identity does not invalidate the building.
        // Resident metadata or authored data may settle; retry only this definition.
        definition.CanonicalBlueprint=result.Path;
        definition.IdentityInterrupted=result.State==MiniMapBuildingBlueprintIdentity::Status::Interrupted;
        if(!result.Path)
        {
            const auto bit=uint64_t(1)<<unsigned(result.Failure);
            if(!(definition.IdentityDiagnostics&bit))
            {
                definition.IdentityDiagnostics|=bit;
                const auto state=result.State==MiniMapBuildingBlueprintIdentity::Status::Invalid?"invalid":
                    result.State==MiniMapBuildingBlueprintIdentity::Status::Interrupted?"interrupted":"unavailable";
                SpriteDiagnostic("MiniMap: sprite identity unresolved: building=%u definition=%d name=%d:%u serial=%d stage=canonical resolution state=%s reason=%s policy=normal acquisition retry; uncertain native operations are not retried; path=unavailable",
                    record.BuildingId,record.DefinitionObjectIndex,record.DefinitionIndex,record.DefinitionNumber,
                    definition.ObjectSerial,state,result.Detail);
            }
        }
    }
    void Geometry(Record& r,const SDK::FAuSplineConnectionFragment* spline,const SDK::FBox* targetingBox,bool retain=true)
    {
        r.SourceFootprint={}; r.LocalFootprint={}; r.UnboundedGeometry=false;
        r.InvalidFootprintSource=false;
        r.SplineInflation=0; r.GeometryStatus=GeometryIssue::MissingSource;
        r.UsesSafetyAllowance=true;
        if(spline)
        {
            const auto start=Clock::now();
            MiniMapBuildingGeometry::CopySpline(*spline,g_curveScratch);
            if(retain && (!r.Curve || r.Curve->Issue!=g_curveScratch.Issue ||
                r.Curve->Reconstructed!=g_curveScratch.Reconstructed || r.Curve->Segments!=g_curveScratch.Segments))
                r.Curve=std::make_shared<const SplineGeometry>(g_curveScratch);
            const auto& curve=retain?*r.Curve:g_curveScratch;
            r.GeometryStatus=curve.Issue;
            const double scale=(std::max)({std::abs(r.Scale[0]),std::abs(r.Scale[1]),std::abs(r.Scale[2]),1.0});
            double sourced=0;
            if(r.DefinitionShape)
                sourced=r.DefinitionShape->SplineMeshCrossSection+r.DefinitionShape->RailEndpointExtension;
            r.SplineInflation=(std::max)(kSplinePadding,sourced*scale);
            if(std::isfinite(r.SplineInflation) && r.SplineInflation<=MiniMapBuildingGeometry::kMaximumGeometrySpan)
                r.Extent=Pad(curve.CenterlineBounds,r.SplineInflation);
            else { r.Extent={}; r.GeometryStatus=GeometryIssue::Oversized; }
            if(r.Extent.Valid) r.BoundsKind=Coverage::SplineHull;
            else
            {
                // No finite guessed box can guarantee an unsupported spline's
                // crossing coverage. Keep its identity explicitly admitted.
                r.UnboundedGeometry=true;
                r.Extent=Box(r.Position[0],r.Position[1],kOriginPadding);
                r.BoundsKind=Coverage::Unresolved;
            }
            if(retain) g_metrics.SplineTime+=Ms(start);
        }
        else if(r.TransformValid)
        {
            if(retain)++g_metrics.OrdinaryGeometry;
            const auto start=Clock::now(); r.Curve.reset();
            if(targetingBox) r.LocalFootprint=MiniMapBuildingGeometry::Copy(*targetingBox);
            else if(r.DefinitionShape) r.LocalFootprint=r.DefinitionShape->TargetingBox;
            r.InvalidFootprintSource=targetingBox && !r.LocalFootprint.Valid;
            r.SourceFootprint=MiniMapBuildingGeometry::Footprint(r.LocalFootprint,r.Position,r.Rotation,r.Scale);
            // Authored targeting bounds do NOT prove visual-mesh bounds. Retain
            // B.1 safety coverage and enlarge it if a known proxy extends farther.
            r.Extent=MiniMapBuildingGeometry::Union(Box(r.Position[0],r.Position[1],kOriginPadding),r.SourceFootprint);
            r.BoundsKind=r.SourceFootprint.Valid?Coverage::OrdinaryProxy:Coverage::OriginPadding;
            if(r.LocalFootprint.Valid && !r.SourceFootprint.Valid) r.GeometryStatus=GeometryIssue::InvalidTransform;
            if(retain) g_metrics.OrdinaryTime+=Ms(start);
        }
    }
    bool Read(const Native& n,Record& r)
    {
        // Defensive guard; membership callers also bypass Read for resolved data.
        if(r.StaticResolved()) {++g_metrics.CachedReuse;return true;}
        g_snapshotDirty=true; // Conservative for unresolved/dynamic observations.
        if(r.HasSpline || r.DynamicObserved)++g_metrics.DynamicReads;
        else if(r.AcquisitionAttempted)++g_metrics.RetryAttempts;
        else ++g_metrics.NewAttempts;
        r.AcquisitionAttempted=true;
        const Id id{r.Index,r.Serial};
        if(!n.mass.isEntityValid(g_manager,id)) { ++g_metrics.Stale; return false; }
        // Built is a precondition of fragment access, not copied-data eligibility.
        // An unavailable refresh does not erase a prior successful observation.
        if(!n.mass.isEntityBuilt(g_manager,id)) return r.TransformValid;
        const auto start=Clock::now();
        ++g_metrics.TransformReads;
        if(const auto* t=static_cast<const SDK::FTransform*>(Fragment(n,id,g_types.Transform)))
        {
            const Point position{t->Translation.X,t->Translation.Y,t->Translation.Z};
            const Point scale{t->Scale3D.X,t->Scale3D.Y,t->Scale3D.Z};
            const std::array<double,4> rotation{t->Rotation.X,t->Rotation.Y,t->Rotation.Z,t->Rotation.W};
            auto finite=[](double v){return std::isfinite(v);};
            double norm=0; for(double q:rotation) norm+=q*q;
            if(std::all_of(position.begin(),position.end(),finite) && std::all_of(rotation.begin(),rotation.end(),finite) &&
                std::all_of(scale.begin(),scale.end(),finite) && std::isfinite(norm) && norm>0)
            { r.Position=position; r.Scale=scale; r.Rotation=rotation; r.TransformValid=true; }
        }
        const auto* spline=static_cast<const SDK::FAuSplineConnectionFragment*>(Fragment(n,id,g_types.Spline));
        // Remember actual dynamic-source evidence for this lifetime. A missing
        // spline fragment during a later observation must not freeze the record.
        // HasSpline itself keeps its existing per-observation rendering semantics.
        r.DynamicObserved=r.DynamicObserved || r.HasSpline || spline!=nullptr;
        r.HasSpline=spline!=nullptr;
        // These are the ONLY UObject accesses needed for ordinary identity and
        // Interior facts. Failure leaves already copied facts intact. No raw
        // placement pointer survives this synchronous acquisition scope.
        if(MiniMapResidentMetadata::Available())
        {
            const auto* parameters=Parameters(n,id,true);
            const auto* placement=parameters?parameters->PlacementData:nullptr;
            const auto object=reinterpret_cast<uintptr_t>(placement);
            int32_t serial=0;
            if(MiniMapResidentMetadata::Serial(object,serial) &&
                MiniMapResidentMetadata::Validate(object,sizeof(SDK::UAuActorPlacementData)) && IsClass(placement,g_types.Placement))
            {
                const auto index=placement->Index, name=placement->Name.ComparisonIndex;
                const auto number=placement->Name.Number, building=uint32_t(placement->BuildingID);
                // A positively observed change invalidates only this instance's
                // old facts. A failed observation is not evidence of a change.
                if(r.DefinitionValid && (r.DefinitionObjectIndex!=index || r.DefinitionObjectSerial!=serial ||
                    r.DefinitionIndex!=name || r.DefinitionNumber!=number || r.BuildingId!=building))
                { r.CanonicalBlueprint.reset(); r.InteriorKnown=false; r.InteriorOnly=false; r.DefinitionShape.reset(); }
                r.DefinitionObjectIndex=index; r.DefinitionObjectSerial=serial;
                r.DefinitionIndex=name; r.DefinitionNumber=number; r.BuildingId=building; r.DefinitionValid=true;
                const auto key=Key(index,int32_t(building));
                auto it=g_definitions.find(key);
                if(it==g_definitions.end() || it->second.NameIndex!=name || it->second.NameNumber!=number || it->second.ObjectSerial!=serial)
                {
                    it=g_definitions.insert_or_assign(key,Definition{building,0xFF,name,number}).first;
                    it->second.ObjectSerial=serial; ++g_metrics.DefinitionMiss;
                }
                auto& definition=it->second;
                if(!definition.InteriorTagChecked)
                {
                    const auto kind=ObserveClass(placement,g_types.BuildingData);
                    const auto tag=kind==ClassObservation::Unavailable?InteriorTagResult::Unknown:
                        kind==ClassObservation::Matches
                        ?(MiniMapResidentMetadata::Validate(object,sizeof(SDK::UCrBuildingData))
                            ?ReadInteriorTag(static_cast<const SDK::UCrBuildingData*>(placement),true):InteriorTagResult::Unknown)
                        :InteriorTagResult::Absent;
                    if(tag!=InteriorTagResult::Unknown)
                    { definition.InteriorTagChecked=true; definition.InteriorOnly=tag==InteriorTagResult::Present; }
                }
                r.InteriorKnown=definition.InteriorTagChecked; r.InteriorOnly=definition.InteriorOnly;
                if(!r.HasSpline && r.InteriorKnown && !r.InteriorOnly)
                { DefinitionBlueprint(definition,placement,r); r.CanonicalBlueprint=definition.CanonicalBlueprint; }
                // Optional geometry is solely for the existing spatial envelope.
                // Ordinary instances never inspect mesh/template/render resources.
                if(!r.InteriorOnly)r.DefinitionShape=DefinitionShape(definition,placement,r.HasSpline);
            }
            else if(!r.CanonicalBlueprint && !r.HasSpline && !(g_definitionDiagnostics&1))
            {
                g_definitionDiagnostics|=1;
                SpriteDiagnostic("MiniMap: building acquisition unavailable: entity=%d:%d placement identity not safely readable; copied facts preserved; normal acquisition retries",r.Index,r.Serial);
            }
        }
        if(const auto* tint=static_cast<const SDK::FCrBuildingVisualCustomizationFragment*>(Fragment(n,id,g_types.Tint)))
            r.Tint={tint->ColorTint.R,tint->ColorTint.G,tint->ColorTint.B,tint->ColorTint.A};
        const double dynamic=Ms(start); g_metrics.Dynamic+=dynamic;
        if(r.Sources&OffGrid) g_metrics.OffDynamic+=dynamic;
        const auto boundsStart=Clock::now();
        const auto* targeting=static_cast<const SDK::FCrBuildingAggroTargetDataFragment*>(Fragment(n,id,g_types.TargetBox));
        Geometry(r,spline,targeting?&targeting->BuildingBoundingBox:nullptr);
        if(!r.HasSpline && r.SourceFootprint.Valid)
        {
            const double reach=(std::max)({std::abs(r.SourceFootprint.MinX-r.Position[0]),std::abs(r.SourceFootprint.MaxX-r.Position[0]),
                std::abs(r.SourceFootprint.MinY-r.Position[1]),std::abs(r.SourceFootprint.MaxY-r.Position[1])});
            g_ordinaryQueryPadding=(std::max)(g_ordinaryQueryPadding,reach);
        }
        const double boundsTime=Ms(boundsStart); g_metrics.BoundsTime+=boundsTime;
        if(r.Sources&OffGrid) g_metrics.OffBounds+=boundsTime;
        r.RefreshedAtSeconds=Seconds(); ++g_metrics.Refreshed;
        // Known Interior is terminal exclusion and needs no classname conversion.
        r.StaticAcquired=!r.HasSpline && r.TransformValid && r.InteriorKnown &&
            !r.DynamicObserved && (r.InteriorOnly || bool(r.CanonicalBlueprint));
        if(r.StaticAcquired)++g_metrics.ResolvedNow;
        return r.TransformValid;
    }
    void Destroy()
    {
        if(const auto* n=MiniMapNative::Get())
            for(auto* q:{&g_offQuery,&g_splineQuery})
                if(q->Constructed) n->mass.queryDestruct(q->Storage.data());
        g_offQuery={}; g_splineQuery={}; g_manager=nullptr; g_world=nullptr;
    }
    void Clear()
    {
        Destroy();
#if MINIMAP_DEBUG_UI
        MiniMapDebugText::Clear();
        g_debugOracle={}; g_debugLastGridMs=0;
#endif
        g_cache.clear(); g_indexedCopies.clear(); g_observation=0; g_definitions.clear(); g_keys.clear(); g_near.clear(); g_pending.clear();
        g_definitionDiagnostics=0;
        g_curveScratch={};
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
        size_t retained=0;
        for(size_t i=0;i<g_metadataCount;++i)
            if(!MiniMapResidentMetadata::Release(g_metadataRoots[i]))g_metadataRoots[retained++]=g_metadataRoots[i];
        // Failed checked releases remain owned; the native helper disables new acquisition.
        g_metadataCount=retained;
#endif
        g_cursor=g_nearCursor=0; g_epoch=0; g_types={}; g_typesReady=false;
        g_region={}; g_queryRegion={}; g_lastRadius=0;
        g_ordinaryQueryPadding=kOriginPadding;
        g_initialDiscoveryReports=0;
        g_nextLocal=g_nextIdentity=g_nextLog=g_nextWarning={}; g_metrics={};
        g_snapshotDirty=true; g_publishedGrid=g_publishedIdentity=false;
        g_indexed=g_gridResults=g_duplicates=g_gridVisited=0;
        g_indexedHandles.clear(); g_xyQuery=false;
        g_gridSucceeded=g_identitySucceeded=false;
        ClearComparison();
        { std::scoped_lock lock(g_mutex); ++g_generation; g_snapshot.reset(); }
        LOG_INFO("MiniMap: BuildingCollector: reset generation=%llu",static_cast<unsigned long long>(g_generation));
    }
    bool Ensure(const Native& n)
    {
        auto* world=MiniMapMap::GetWorld();
        auto* subsystem=world?n.mass.getMassEntitySubsystem(world):nullptr;
        const void* manager=nullptr;
        if(subsystem) std::memcpy(&manager,reinterpret_cast<const uint8_t*>(subsystem)+0x38,sizeof(manager));
        if(g_manager && (manager!=g_manager || world!=g_world)) Clear();
        if(!manager || !ResolveTypes()) return false;
        if(g_offQuery.Constructed) return true;
        g_manager=manager; g_world=world;
        for(auto* q:{&g_offQuery,&g_splineQuery})
        {
            n.mass.queryConstruct(q->Storage.data(),reinterpret_cast<const uint8_t*>(subsystem)+0x38);
            q->Constructed=true;
            n.mass.addTagRequirement(q->Storage.data(),g_types.Building,SDK::EMassFragmentPresence::All);
            n.mass.addTransformRequirement(q->Storage.data(),uint8_t(SDK::EMassFragmentAccess::ReadOnly),uint8_t(SDK::EMassFragmentPresence::Optional));
        }
        n.mass.addTagRequirement(g_offQuery.Storage.data(),g_types.Grid,SDK::EMassFragmentPresence::None);
        n.mass.addTagRequirement(g_splineQuery.Storage.data(),g_types.Grid,SDK::EMassFragmentPresence::All);
        n.mass.addSplineRequirement(g_splineQuery.Storage.data(),uint8_t(SDK::EMassFragmentAccess::ReadOnly),uint8_t(SDK::EMassFragmentPresence::All));
        LOG_INFO("MiniMap: BuildingCollector: ready local=200ms identities=1s distant=2s refreshBudget=2ms gridPolicy=%s",
            n.gridDiagnostic.forEachCellInRadius?"XY/all-Z":"fallback-sphere-Z+/-1000m");
        if(!n.gridDiagnostic.forEachCellInRadius)
            LOG_WARN("MiniMap: BuildingCollector: optional XY traversal unavailable; retaining original expensive box policy");
        return true;
    }
    struct Handles
    {
        MiniMapNative::MassEntityHandleArray Array;
        const Native& N;
        ~Handles() { if(Array.Data) N.texture.memoryFree(Array.Data); }
        bool Valid() const { return Array.Num>=0 && Array.Num<=Array.Max && Array.Max<=kMaximumHandles && (!Array.Num || Array.Data); }
    };
    void Reconcile(const Native& n)
    {
        ++g_metrics.IdentityPolls;
        g_identitySucceeded=false;
        Handles off{{},n}, splines{{},n};
        auto start=Clock::now(); n.mass.getMatchingEntityHandles(g_offQuery.Storage.data(),&off.Array);
        g_metrics.OffQuery+=Ms(start);
        start=Clock::now(); n.mass.getMatchingEntityHandles(g_splineQuery.Storage.data(),&splines.Array);
        g_metrics.SplineQuery+=Ms(start);
        if(!off.Valid() || !splines.Valid()) return; // Never infer removals from failed results.
        g_identitySucceeded=true;
        start=Clock::now(); ++g_epoch;
        auto merge=[&](const auto& array,uint8_t source)
        {
            for(int i=0;i<array.Num;++i)
            {
                const auto id=array.Data[i]; if(id.Index<=0 || !id.SerialNumber) continue;
                auto [it,added]=g_cache.try_emplace(Key(id));
                auto& entry=it->second;
                if(entry.Epoch!=g_epoch) { entry.PreviousSources=entry.Data.Sources; entry.Data.Sources=0; }
                entry.Epoch=g_epoch; entry.Data.Sources|=source;
                if(added)
                {
                    g_snapshotDirty=true;
                    if(const auto indexed=g_indexedCopies.find(Key(id));indexed!=g_indexedCopies.end())
                        entry.Data=indexed->second; // Same entity lifetime changing acquisition source.
                    entry.Data.Sources=source;
                    entry.Data.Generation=g_generation; entry.Data.Index=id.Index; entry.Data.Serial=id.SerialNumber;
                    entry.Read=entry.Data.StaticResolved();
                    g_pending.push_back(Key(id));
                    ++g_metrics.Added;
                }
                if(source&IndexedSpline)entry.Data.DynamicObserved=true;
            }
        };
        merge(off.Array,OffGrid); merge(splines.Array,IndexedSpline);
        for(auto it=g_cache.begin();it!=g_cache.end();)
        {
            const bool current=it->second.Epoch==g_epoch;
            const uint8_t before=current?it->second.PreviousSources:it->second.Data.Sources;
            const uint8_t after=current?it->second.Data.Sources:0;
            g_metrics.OffAdded+=!(before&OffGrid) && (after&OffGrid);
            g_metrics.OffRemoved+=(before&OffGrid) && !(after&OffGrid);
            if(before!=after)g_snapshotDirty=true;
            if(!current)
            {
                // Preserve the same lifetime's copied payload for an off-grid ->
                // indexed transition. Sources=0 is staging, never publication.
                if(n.mass.isEntityValid(g_manager,{it->second.Data.Index,it->second.Data.Serial}))
                {
                    auto [copy,added]=g_indexedCopies.try_emplace(it->first,it->second.Data);
                    if(added)copy->second.Sources=0;
                }
                it=g_cache.erase(it); ++g_metrics.Removed; g_snapshotDirty=true;
            }
            else ++it;
        }
        g_keys.clear(); g_keys.reserve(g_cache.size());
        for(const auto& [key,entry]:g_cache) g_keys.push_back(key);
        if(g_cursor>=g_keys.size()) g_cursor=0;
        g_metrics.Reconcile+=Ms(start);
    }
    void Refresh(const Native& n)
    {
        const auto start=Clock::now(); const auto now=start;
        auto service=[&](uint64_t key)
        {
            auto it=g_cache.find(key); if(it==g_cache.end()) return;
            auto& e=it->second;
            if(e.Data.StaticResolved()) {e.Read=true;++g_metrics.CachedReuse;return;}
            if(now<e.Due) return;
            e.Read=Read(n,e.Data);
            const bool unresolved=!e.Data.TransformValid || (!e.Data.HasSpline &&
                (!e.Data.InteriorKnown || (!e.Data.InteriorOnly && !e.Data.CanonicalBlueprint)));
            // Missing required facts retry on the normal next acquisition slice,
            // without the settled-record movement cadence becoming a cooldown.
            e.Due=unresolved?Clock::time_point{}:
                now+(MiniMapBuildingGeometry::Intersects(e.Data.Extent,g_region)?kLocalCadence:kDistantCadence);
        };
        // Measured startup refresh cost includes a 5.467ms single service slice.
        // Retain the existing soft CPU time budget for worldwide refresh work,
        // not a canonical-conversion quota or a publication delay.
        while(!g_pending.empty() && Ms(start)<kRefreshBudgetMs*0.25)
        { const auto key=g_pending.back(); g_pending.pop_back(); service(key); }
        // Local candidates have priority, but reserve half the budget for
        // discovery/distant validation so a dense factory cannot starve warm-up.
        size_t visited=0;
        while(visited<g_near.size() && Ms(start)<kRefreshBudgetMs*0.5)
        {
            if(g_nearCursor>=g_near.size()) g_nearCursor=0;
            service(g_near[g_nearCursor++]); ++visited;
        }
        visited=0;
        while(visited<g_keys.size() && Ms(start)<kRefreshBudgetMs)
        {
            if(g_cursor>=g_keys.size()) g_cursor=0;
            service(g_keys[g_cursor++]); ++visited;
        }
        g_metrics.CacheService+=Ms(start);
    }
    struct GridVisitContext
    {
        const Native& Api;
        const Bounds& Region;
        bool Overflow = false;
    };
    bool VisitIndexed(void* object,const MiniMapNative::GridEntityHandle& handle)
    {
        auto& context=*static_cast<GridVisitContext*>(object);
        ++g_gridVisited;
        const auto& n=context.Api;
        if(!n.mass.isEntityValid(g_manager,handle.Entity)) return true;
        const auto previous=g_indexedCopies.find(Key(handle.Entity));
        auto keepCopied=[&]()
        {
            if(previous!=g_indexedCopies.end() && previous->second.Position[0]>context.Region.MinX && previous->second.Position[0]<context.Region.MaxX &&
                previous->second.Position[1]>context.Region.MinY && previous->second.Position[1]<context.Region.MaxY)
            {
                if(g_indexedHandles.size()>=kMaximumHandles){context.Overflow=true;return false;}
                g_indexedHandles.push_back(handle.Entity);
            }
            return true;
        };
        if(!n.mass.isEntityBuilt(g_manager,handle.Entity))return keepCopied();
        // This is the existing source membership filter, not Interior
        // acquisition. Grid cells contain non-buildings too; keep this filter
        // even for copied ordinary instances so removed BuildingTag membership
        // cannot become a ghost. It never accesses placement or geometry.
        alignas(8) std::array<std::byte,0x28> view={};
        ++g_metrics.MembershipTags;
        n.gridDiagnostic.viewConstruct(view.data(),g_manager,handle.Entity);
        if(!n.gridDiagnostic.viewHasTag(view.data(),g_types.Building)) return true;
        if(previous!=g_indexedCopies.end() && previous->second.StaticResolved())return keepCopied();
        ++g_metrics.MembershipTransforms;
        const auto* transform=static_cast<const SDK::FTransform*>(Fragment(n,handle.Entity,g_types.Transform));
        if(!transform) return keepCopied();
        const auto& p=transform->Translation;
        if(!std::isfinite(p.X) || !std::isfinite(p.Y) || !std::isfinite(p.Z))return keepCopied();
        // Reproduce FindEntitiesInBox's STRICT XY origin tests, with no Z test.
        if(!(p.X>context.Region.MinX && p.X<context.Region.MaxX &&
             p.Y>context.Region.MinY && p.Y<context.Region.MaxY)) return true;
        if(g_indexedHandles.size()>=kMaximumHandles) { context.Overflow=true; return false; }
        g_indexedHandles.push_back(handle.Entity); // Copy 8-byte identity only.
        return true; // Native traversal interprets false as early termination.
    }
    void MeasureGridCall(Clock::time_point start)
    {
        const double elapsed=Ms(start);
#if MINIMAP_DEBUG_UI
        g_debugLastGridMs=elapsed;
#endif
        g_metrics.GridQuery+=elapsed;
        g_metrics.GridMax=(std::max)(g_metrics.GridMax,elapsed);
        ++g_metrics.GridCalls;
    }
    std::vector<Record> IndexedRecords(const Native& n,const MiniMapMap::PlayerPose& pose)
    {
        std::vector<Record> records;
        g_gridSucceeded=false; g_gridResults=0;
        auto copied=[&]()
        {
            for(auto it=g_indexedCopies.begin();it!=g_indexedCopies.end();)
                if(!n.mass.isEntityValid(g_manager,{it->second.Index,it->second.Serial}))
                {it=g_indexedCopies.erase(it);g_snapshotDirty=true;}
                else {if(it->second.Sources&Indexed)records.push_back(it->second);++it;}
            return records;
        };
        void* grid=n.gridDiagnostic.getSubsystem(g_world);
        if(!IsClass(static_cast<SDK::UObject*>(grid),g_types.GridClass) ||
            !MiniMapResidentMetadata::Validate(reinterpret_cast<uintptr_t>(grid),0xF0+sizeof(void*))) return copied();
        const void* manager=nullptr;
        std::memcpy(&manager,static_cast<const uint8_t*>(grid)+0xF0,sizeof(manager));
        if(manager!=g_manager) return copied();
        g_queryZ=pose.WorldZ;
        g_indexedHandles.clear(); g_gridVisited=0;
        g_xyQuery=n.gridDiagnostic.forEachCellInRadius!=nullptr;
        if(g_xyQuery)
        {
            const double hx=(g_queryRegion.MaxX-g_queryRegion.MinX)*0.5;
            const double hy=(g_queryRegion.MaxY-g_queryRegion.MinY)*0.5;
            const SDK::FVector center{g_queryRegion.MinX+hx,g_queryRegion.MinY+hy,pose.WorldZ};
            // Outward rounding prevents narrowing the XY cell window when the
            // native float radius is converted back to double for grid lookup.
            const float radius=std::nextafter(static_cast<float>(std::hypot(hx,hy)),std::numeric_limits<float>::infinity());
            GridVisitContext context{n,g_queryRegion};
            const MiniMapNative::GridDiagnosticApi::VisitorRef visitor{&VisitIndexed,&context};
            const auto start=Clock::now();
            n.gridDiagnostic.forEachCellInRadius(grid,center,radius,visitor,false);
            MeasureGridCall(start);
            g_gridSucceeded=!context.Overflow;
            g_gridResults=g_indexedHandles.size();
        }
        else
        {
            SDK::FBox box={};
            box.Min={g_queryRegion.MinX,g_queryRegion.MinY,pose.WorldZ-kFallbackVerticalExtent};
            box.Max={g_queryRegion.MaxX,g_queryRegion.MaxY,pose.WorldZ+kFallbackVerticalExtent}; box.IsValid=1;
            SDK::UScriptStruct* tag=g_types.Building;
            struct Tags { SDK::UScriptStruct** Data; int32_t Num,Max; } required{&tag,1,1};
            static_assert(sizeof(Tags)==0x10);
            MiniMapNative::GridResultArray result={};
            const auto start=Clock::now();
            n.gridDiagnostic.findInBox(grid,box,result,&required,nullptr);
            MeasureGridCall(start);
            const auto extraction=Clock::now();
            const bool valid=result.Num>=0 && result.Num<=result.Max && result.Max<=kMaximumHandles && (!result.Num || result.Data);
            if(valid)
            {
                g_gridSucceeded=true;
                g_gridResults=result.Num;
                g_indexedHandles.reserve(result.Num);
                for(int i=0;i<result.Num;++i)
                    g_indexedHandles.push_back(result.Data[i].Entity);
                // FCrGridEntityHandle contains a native TSharedPtr. Destruct
                // every constructed element before freeing its array storage.
                if(result.Num) n.gridDiagnostic.destructItems(result.Data,result.Num);
            }
            if(result.Data) n.texture.memoryFree(result.Data);
            g_metrics.GridExtract+=Ms(extraction);
        }
        const auto extraction=Clock::now();
        std::unordered_set<uint64_t> seen;
        seen.reserve(g_indexedHandles.size());
        for(const auto id:g_indexedHandles)
        {
            const auto key=Key(id); if(!seen.insert(key).second)continue;
            auto [it,added]=g_indexedCopies.try_emplace(key);
            auto& r=it->second;
            if(added)
            {
                g_snapshotDirty=true;
                if(const auto cached=g_cache.find(key);cached!=g_cache.end())r=cached->second.Data;
                r.Generation=g_generation;r.Index=id.Index;r.Serial=id.SerialNumber;
            }
            if(r.Sources!=Indexed)g_snapshotDirty=true;
            r.Sources=Indexed;
            if(r.StaticResolved())++g_metrics.CachedReuse;
            else Read(n,r); // Only newly discovered/unresolved or dynamic data.
        }
        records.reserve(g_indexedCopies.size());
        for(auto it=g_indexedCopies.begin();it!=g_indexedCopies.end();)
        {
            // Successful enumeration owns scope membership; a failed query does
            // not assert removal. Entity serial validity is the removal oracle.
            if(!n.mass.isEntityValid(g_manager,{it->second.Index,it->second.Serial}) ||
                (g_gridSucceeded && !seen.contains(it->first)))
            {it=g_indexedCopies.erase(it);g_snapshotDirty=true;}
            else {if(it->second.Sources&Indexed)records.push_back(it->second);++it;}
        }
        g_metrics.GridExtract+=Ms(extraction);
        return records;
    }
    void Publish(std::vector<Record> indexed,const Native& n)
    {
        const auto start=Clock::now();
        // Keep actual removal detection at publication cadence, including when
        // immutable observations let us reuse the previous snapshot wholesale.
        for(auto it=g_cache.begin();it!=g_cache.end();)
            if(!n.mass.isEntityValid(g_manager,{it->second.Data.Index,it->second.Data.Serial}))
            {it=g_cache.erase(it);g_snapshotDirty=true;++g_metrics.Removed;}
            else ++it;
        const auto previous=MiniMapBuildingCollector::GetSnapshot();
        const auto sameRegion=[&]
        {
            if(!previous)return false;
            const auto& r=previous->Region;
            return r.Valid==g_region.Valid && r.MinX==g_region.MinX && r.MinY==g_region.MinY &&
                r.MaxX==g_region.MaxX && r.MaxY==g_region.MaxY;
        };
        if(!g_snapshotDirty && previous && previous->Generation==g_generation && sameRegion() &&
            g_publishedGrid==g_gridSucceeded && g_publishedIdentity==g_identitySucceeded)
        {
            ++g_metrics.SnapshotsReused;
            g_metrics.Filter+=Ms(start);
            return; // Pointer identity lets BuildingVisuals skip preparation too.
        }
        ++g_metrics.SnapshotsBuilt;
        auto result=std::make_shared<Snapshot>(); result->Generation=g_generation; result->Region=g_region;
        result->Complete=g_gridSucceeded && g_identitySucceeded;
        result->Records.reserve(indexed.size()+g_near.size());
        g_near.clear(); g_duplicates=0;
        auto add=[&](const Record& r)
        {
            if(r.InteriorOnly) return;
            if(!r.TransformValid || !r.Extent.Valid) { result->Complete=false; return; }
            if(!r.UnboundedGeometry && !MiniMapBuildingGeometry::Intersects(r.Extent,g_region)) return;
            result->Records.push_back(r);
            // Publication consumes copied facts only, never reopens native
            // definition/entity provenance to draw an acquired sprite.
            if(!r.HasSpline && (!r.CanonicalBlueprint || !r.InteriorKnown)) result->Complete=false;
        };
        for(const auto& r:indexed) add(r); // Fresh local data wins deterministic dedupe.
        for(const auto& [key,e]:g_cache)
        {
            if(!e.Read || !e.Data.Extent.Valid || !e.Data.TransformValid) { result->Complete=false; continue; }
            if(!e.Data.UnboundedGeometry && !MiniMapBuildingGeometry::Intersects(e.Data.Extent,g_region)) continue;
            g_near.push_back(key);
            add(e.Data);
        }
        g_metrics.Filter+=Ms(start);
        const auto publication=Clock::now();
        result->PublishedAtSeconds=Seconds();
        std::sort(result->Records.begin(),result->Records.end(),[](const Record& a,const Record& b)
        {
            const auto ka=Key(a.Index,a.Serial),kb=Key(b.Index,b.Serial);
            if(ka!=kb) return ka<kb;
            if(bool(a.Sources&Indexed)!=bool(b.Sources&Indexed)) return bool(a.Sources&Indexed);
            return a.RefreshedAtSeconds>b.RefreshedAtSeconds;
        });
        size_t written=0;
        for(const auto& r:result->Records)
        {
            if(written && Key(result->Records[written-1].Index,result->Records[written-1].Serial)==Key(r.Index,r.Serial))
            { result->Records[written-1].Sources|=r.Sources; ++g_duplicates; }
            else { result->Records[written++]=r; }
        }
        result->Records.resize(written);
        if(g_initialDiscoveryReports<8)
        {
            ++g_initialDiscoveryReports;
            try
            {
                std::vector<std::string_view> canonical;
                size_t unresolved=0;
                for(const auto& r:result->Records)
                {
                    if(r.HasSpline)continue;
                    if(r.CanonicalBlueprint)canonical.emplace_back(*r.CanonicalBlueprint);
                    else ++unresolved;
                }
                std::sort(canonical.begin(),canonical.end());
                const auto count=std::unique(canonical.begin(),canonical.end())-canonical.begin();
                SpriteDiagnostic("MiniMap: initial discovery observation: generation=%llu sample=%u/8 boundary=available-scoped-pass acceptedRecords=%zu canonicalTypes=%zu unresolvedOrdinary=%zu snapshotComplete=%d gridQuery=%d identityQuery=%d pendingRefresh=%zu; all available definitions serviced without conversion quota",
                    static_cast<unsigned long long>(g_generation),g_initialDiscoveryReports,result->Records.size(),size_t(count),
                    unresolved,int(result->Complete),int(g_gridSucceeded),int(g_identitySucceeded),g_pending.size());
            }
            catch(...) {} // Diagnostics must not prevent snapshot publication.
        }
        { std::scoped_lock lock(g_mutex); g_snapshot=std::move(result); }
        g_snapshotDirty=false; g_publishedGrid=g_gridSucceeded; g_publishedIdentity=g_identitySucceeded;
        g_metrics.SnapshotTime+=Ms(publication);
        g_metrics.SnapshotBuild+=Ms(start);
    }
    void OnTick(float)
    {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::CollectorTick,true);
        if(!MiniMapMap::GameplayReady())return;
        if(!g_ready || !MiniMapMap::HasWorld()) return;
        const auto start=Clock::now(); const auto* n=MiniMapNative::Get();
        if(!n || !n->gridDiagnostic.IsAvailable() || !n->texture.memoryFree || !n->mass.getConstSharedFragmentPtr ||
            !n->mass.isEntityValid || !n->mass.isEntityBuilt || !n->mass.addSplineRequirement)
        {
            if(!g_unavailableLogged) LOG_WARN("MiniMap: BuildingCollector: optional native prerequisites unavailable; collector disabled");
            g_unavailableLogged=true; return;
        }
        {MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::CollectorEnsure);if(!Ensure(*n)) return;}
        if(++g_observation==0) {g_observation=1;for(auto& [key,d]:g_definitions){d.IdentityObservation=0;d.GeometryObservation=0;}}
        double radius=0; { std::scoped_lock lock(g_mutex); radius=g_viewRadius; }
        MiniMapMap::PlayerPose pose;
        if(radius<=0 || !MiniMapMap::TryGetPlayerPose(pose)) return;
        const auto now=Clock::now(); g_region=Box(pose.WorldX,pose.WorldY,radius);
        const bool relocate=!Contains(g_queryRegion,Pad(g_region,g_ordinaryQueryPadding)) || radius!=g_lastRadius || std::abs(pose.WorldZ-g_queryZ)>kMovementPadding;
        if(relocate)
        {
            g_queryRegion=Pad(g_region,g_ordinaryQueryPadding+kMovementPadding); g_queryZ=pose.WorldZ; g_lastRadius=radius;
            g_nextLocal={};
            // Newly relevant cached hulls get refreshed promptly, independent of
            // rotation. Unknown bounds still use the fair discovery sweep.
            g_near.clear();
            for(auto& [key,e]:g_cache) if(MiniMapBuildingGeometry::Intersects(e.Data.Extent,g_region))
            { e.Due={}; g_near.push_back(key); }
        }
        if(now>=g_nextIdentity) { MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::CollectorReconcile);Reconcile(*n); g_nextIdentity=now+kIdentityCadence; }
        {MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::CollectorRefresh);Refresh(*n);}
        if(now>=g_nextLocal)
        {
            g_nextLocal=now+kLocalCadence;
            MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::CollectorPublish);
            auto indexed=IndexedRecords(*n,pose); g_indexed=indexed.size();
            Publish(std::move(indexed),*n); ++g_metrics.Polls;
        }
        const double total=Ms(start); g_metrics.Total+=total; g_metrics.MaxTick=(std::max)(g_metrics.MaxTick,total);
        if(total>20 && now>=g_nextWarning)
        {
            LOG_WARN("MiniMap: BuildingCollector: slow callback=%.3fms indexed=%zu cached=%zu",total,g_indexed,g_cache.size());
            g_nextWarning=now+kLogCadence;
        }
        if(now>=g_nextLog)
        {
            MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::CollectorDiagnostics);
            size_t off=0,tagged=0,pending=0,splines=0;
            double maxAge=0,localAge=0;
            for(const auto& [key,e]:g_cache)
            {
                off+=(e.Data.Sources&OffGrid)!=0; tagged+=(e.Data.Sources&IndexedSpline)!=0;
                pending+=!e.Read || !e.Data.Extent.Valid || !e.Data.TransformValid || (!e.Data.HasSpline && (!e.Data.InteriorKnown || (!e.Data.InteriorOnly && !e.Data.CanonicalBlueprint)));
                if(e.Read) maxAge=(std::max)(maxAge,Seconds()-e.Data.RefreshedAtSeconds);
            }
            auto snapshot=MiniMapBuildingCollector::GetSnapshot();
            if(snapshot) for(const auto& r:snapshot->Records) { splines+=r.HasSpline; localAge=(std::max)(localAge,Seconds()-r.RefreshedAtSeconds); }
            LOG_INFO("MiniMap: BuildingCollector: gridResults=%zu indexed=%zu offGridCached=%zu taggedSplines=%zu candidates=%zu splines=%zu pending=%zu dedup=%zu added=%zu removed=%zu offDelta=+%zu/-%zu definitionMiss=%zu stale=%zu refreshed=%zu polls=%zu identityPolls=%zu ageMax(local/global)=%.3f/%.3fs intervalSums(ms): gridQuery=%.3f gridExtract=%.3f offGridQuery=%.3f splineQuery=%.3f reconcile=%.3f dynamicRefresh=%.3f bounds=%.3f offTransform=%.3f offBounds=%.3f cacheService=%.3f filter=%.3f snapshot=%.3f total=%.3f maxTick=%.3f",
                g_gridResults,g_indexed,off,tagged,snapshot?snapshot->Records.size():0,splines,pending,g_duplicates,g_metrics.Added,g_metrics.Removed,g_metrics.OffAdded,g_metrics.OffRemoved,g_metrics.DefinitionMiss,g_metrics.Stale,g_metrics.Refreshed,g_metrics.Polls,g_metrics.IdentityPolls,localAge,maxAge,
                g_metrics.GridQuery,g_metrics.GridExtract,g_metrics.OffQuery,g_metrics.SplineQuery,g_metrics.Reconcile,g_metrics.Dynamic,g_metrics.BoundsTime,g_metrics.OffDynamic,g_metrics.OffBounds,g_metrics.CacheService,g_metrics.Filter,g_metrics.SnapshotTime,g_metrics.Total,g_metrics.MaxTick);
            LOG_INFO("MiniMap: BuildingCollector: gridTiming mode=%s calls=%zu total=%.3fms mean=%.3fms maxCall=%.3fms visitedLast=%zu indexed=%zu slabs=0",
                g_xyQuery?"XY/all-Z":"fallback-sphere",g_metrics.GridCalls,g_metrics.GridQuery,
                g_metrics.GridCalls?g_metrics.GridQuery/g_metrics.GridCalls:0.0,g_metrics.GridMax,g_gridVisited,g_indexed);
            LOG_INFO("MiniMap: BuildingCollector acquisition (production interval, excludes oracle): newAttempts=%zu unresolvedRetries=%zu staticResolvedNow=%zu cachedReuseVisits=%zu dynamicReads=%zu transformFragments=%zu membershipTransforms=%zu membershipTags=%zu placementFragments=%zu canonicalAttempts=%zu canonicalResolved=%zu interiorReads=%zu ordinaryGeometry=%zu ordinaryDefinitionTraversals=%zu snapshotsBuilt=%zu snapshotsReused=%zu membershipMs=%.3f extractionMs=%.3f snapshotBuildMs=%.3f callbackTotalMs=%.3f callbackMaxMs=%.3f",
                g_metrics.NewAttempts,g_metrics.RetryAttempts,g_metrics.ResolvedNow,g_metrics.CachedReuse,g_metrics.DynamicReads,
                g_metrics.TransformReads,g_metrics.MembershipTransforms,g_metrics.MembershipTags,g_metrics.PlacementReads,
                g_metrics.Conversions,g_metrics.ConversionSuccess,g_metrics.InteriorReads,g_metrics.OrdinaryGeometry,g_metrics.OrdinaryDefinitions,
                g_metrics.SnapshotsBuilt,g_metrics.SnapshotsReused,g_metrics.GridQuery,g_metrics.GridExtract,
                g_metrics.SnapshotBuild,g_metrics.Total,g_metrics.MaxTick);
            size_t ordinary=0,proxy=0,meshWidth=0,railExtension=0,unresolvedSpline=0,reconstructed=0,
                ordinaryCross=0,splineCross=0,ordinarySourceOnly=0,segments=0,invalidFootprint=0;
            std::array<size_t,10> issues={};
            if(snapshot) for(const auto& r:snapshot->Records)
            {
                ++issues[size_t(r.GeometryStatus)];
                invalidFootprint+=r.InvalidFootprintSource;
                const bool originOutside=!Contains(snapshot->Region,Box(r.Position[0],r.Position[1],0));
                if(r.HasSpline)
                {
                    meshWidth+=r.DefinitionShape && r.DefinitionShape->HasSplineMesh;
                    railExtension+=r.DefinitionShape && r.DefinitionShape->HasRailExtension;
                    unresolvedSpline+=r.UnboundedGeometry;
                    splineCross+=originOutside && !r.UnboundedGeometry;
                    if(r.Curve) { reconstructed+=r.Curve->Reconstructed; segments+=r.Curve->Segments.size(); }
                }
                else
                {
                    ++ordinary; proxy+=r.SourceFootprint.Valid; ordinaryCross+=originOutside;
                    ordinarySourceOnly+=!MiniMapBuildingGeometry::Intersects(Box(r.Position[0],r.Position[1],kOriginPadding),snapshot->Region);
                }
            }
            LOG_INFO("MiniMap: BuildingCollector: geometry ordinary=%zu proxy=%zu completeVisual=0 ordinarySafety50m=%zu invalidFootprint=%zu splineMeshWidth=%zu railExtension=%zu splineCapSafety=%zu unresolvedSpline=%zu reconstructed=%zu segments=%zu originOutside(ordinary/spline)=%zu/%zu ordinaryBeyondSafety=%zu issue(none/missingSource/uninitialized/looped/array/curve/mode/nonfinite/oversized/transform)=%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu queryPadding=%.1fm intervalSums(ms): definition=%.3f ordinary=%.3f spline=%.3f",
                ordinary,proxy,ordinary,invalidFootprint,meshWidth,railExtension,splines,unresolvedSpline,reconstructed,segments,
                ordinaryCross,splineCross,ordinarySourceOnly,issues[0],issues[1],issues[2],issues[3],issues[4],issues[5],issues[6],issues[7],issues[8],issues[9],
                g_ordinaryQueryPadding/100.0,g_metrics.DefinitionTime,g_metrics.OrdinaryTime,g_metrics.SplineTime);
#if MINIMAP_DEBUG_UI
            // Reuse the existing diagnostic traversal; no render-time collection.
            std::ostringstream text;
            text << std::fixed << std::setprecision(2)
                << "BuildingCollector\nMode: " << (g_xyQuery?"XY/all-Z":"fallback-sphere")
                << "\nCache complete: " << (snapshot && snapshot->Complete?"yes":"no")
                << "\nPending: " << pending
                << "\nCandidates: " << (snapshot?snapshot->Records.size():0)
                << "\nIndexed: " << g_indexed
                << "\nOff-grid cached: " << off
                << "\nTagged splines: " << tagged;
            if(snapshot) text << "\nSnapshot age (sampled): " << Seconds()-snapshot->PublishedAtSeconds << " s";
            else text << "\nSnapshot: awaiting publication";
            if(g_debugOracle.Valid)
                text << "\nOracle (10s) expected/present: " << g_debugOracle.Expected << '/' << g_debugOracle.Present
                    << "\nOracle missing/repeated: " << g_debugOracle.Missing << '/' << g_debugOracle.Repeated
                    << "\nOracle stale/duplicate: " << g_debugOracle.Stale << '/' << g_debugOracle.Duplicate
                    << "\nOracle unresolved cache/world: " << g_debugOracle.UnresolvedCached << '/' << g_debugOracle.UnresolvedWorldwide;
            else text << "\nOracle: awaiting comparison";
            text << "\nLast local query: " << g_debugLastGridMs << " ms"
                << "\n\nGeometry"
                << "\nOrdinary proxy: " << proxy
                << "\nOrdinary fallback (50m): " << ordinary
                << "\nSpline resident-width: " << meshWidth
                << "\nSpline fallback-width: " << splines-meshWidth
                << "\nSpline cap safety: " << splines
                << "\nUnresolved spline geometry: " << unresolvedSpline
                << "\nUnproven ordinary visual bounds: " << ordinary
                << "\nOrigin-outside ordinary: " << ordinaryCross
                << "\nOrigin-outside spline: " << splineCross;
            text << MiniMapBuildingVisuals::GetDebugText();
            MiniMapDebugText::Publish(text.str());
#endif
            g_metrics={}; g_nextLog=now+kLogCadence;
        }
    }

    // Independent worldwide-reference sink. None of this state feeds acquisition.
    std::shared_ptr<const Snapshot> g_comparison;
    std::unordered_set<uint64_t> g_remaining, g_previousMissing, g_missing;
    size_t g_expected=0,g_missingCount=0,g_persistent=0,g_unresolved=0;
    size_t g_interiorExcluded=0,g_eligibilityUnknown=0;
    size_t g_missingOff=0,g_missingGrid=0,g_stale=0,g_unresolvedCached=0,g_snapshotDuplicates=0;
    std::unordered_map<uint64_t,Definition> g_oracleDefinitions; // Copied metadata, independent of production cache.
    uint64_t g_oraclePass=0;
    Clock::time_point g_compareStart;
    void ClearComparison()
    {
        g_comparison.reset(); g_remaining.clear(); g_previousMissing.clear(); g_missing.clear();
        g_oracleDefinitions.clear();
        g_oraclePass=0;
        g_expected=g_missingCount=g_persistent=g_unresolved=0;
        g_interiorExcluded=g_eligibilityUnknown=0;
        g_missingOff=g_missingGrid=g_stale=g_unresolvedCached=g_snapshotDuplicates=0;
    }
}
namespace MiniMapBuildingCollector
{
    bool Initialize(IPluginSelf* self)
    {
        if(g_initialized) return true;
        if(!self || !self->hooks || !self->hooks->Engine) return false;
#if !defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
        if(!self->hooks->ObjectWalker || !self->hooks->ObjectWalker->IsReady ||
            !self->hooks->ObjectWalker->FindObjectsByNameInto) return false;
#endif
        g_collectorSelf=self; Reset(); self->hooks->Engine->RegisterOnTick(&OnTick); g_initialized=true; return true;
    }
    void Reset()
    {
        g_ready=false; Clear(); g_unavailableLogged=false;
        g_comparison.reset(); g_remaining.clear(); g_previousMissing.clear(); g_missing.clear();
        std::scoped_lock lock(g_mutex); g_viewRadius=0;
    }
    void OnExperienceLoadComplete() { if(g_initialized) g_ready=true; }
    void SetViewport(const MiniMapMap::Transform& t)
    {
        double radius=0;
        if(t.Valid && t.Width>0 && t.Height>0 && std::isfinite(t.PixelsPerWorldUnit) && t.PixelsPerWorldUnit>0)
            radius=0.5*std::hypot(t.Width,t.Height)/t.PixelsPerWorldUnit;
        std::scoped_lock lock(g_mutex); g_viewRadius=std::isfinite(radius)?radius:0;
    }
    std::shared_ptr<const Snapshot> GetSnapshot() { std::scoped_lock lock(g_mutex); return g_snapshot; }
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
    DiagnosticVisitStatus VisitDiagnosticDefinition(const Record& r,bool(*visitor)(const SDK::UObject*,void*),void* context)
    {
        const auto* n=MiniMapNative::Get();
        if(!visitor||!n||!g_ready||!g_manager||!g_typesReady)return DiagnosticVisitStatus::CollectorUnavailable;
        if(r.Generation!=g_generation||g_world!=MiniMapMap::GetWorld())return DiagnosticVisitStatus::WorldGenerationChanged;
        if(!r.DefinitionValid)return DiagnosticVisitStatus::InvalidCopiedDefinition;
        if(!Live(*n,{r.Index,r.Serial}))return DiagnosticVisitStatus::StaleEntity;
        const auto* parameters=Parameters(*n,{r.Index,r.Serial});
        if(!parameters)return DiagnosticVisitStatus::ParametersUnavailable;
        // Identity/type/extent checks and strong acquisition happen in the
        // synchronous diagnostic visitor before any placement fields are read.
        return visitor(parameters->PlacementData,context)?DiagnosticVisitStatus::Visited:DiagnosticVisitStatus::VisitorRejected;
    }
#endif
    void Shutdown()
    {
        if(!g_initialized) return;
        g_collectorSelf->hooks->Engine->UnregisterOnTick(&OnTick); Reset(); g_collectorSelf=nullptr; g_initialized=false;
    }
    bool BeginComparison(const SDK::UWorld* world,const void* manager)
    {
        const auto* n=MiniMapNative::Get();
        if(!n || world!=g_world || manager!=g_manager) return false;
        g_comparison=GetSnapshot();
        if(!g_comparison || g_comparison->Generation!=g_generation) { g_comparison.reset(); return false; }
        g_compareStart=Clock::now();
        if(++g_oraclePass==0){g_oraclePass=1;for(auto& [key,d]:g_oracleDefinitions)d.IdentityObservation=0;}
        g_remaining.clear(); g_missing.clear();
        g_expected=g_missingCount=g_persistent=g_unresolved=0;
        g_interiorExcluded=g_eligibilityUnknown=0;
        g_missingOff=g_missingGrid=g_stale=g_unresolvedCached=g_snapshotDuplicates=0;
        for(const auto& r:g_comparison->Records)
        {
            g_snapshotDuplicates+=!g_remaining.insert(Key(r.Index,r.Serial)).second;
            if(!n->mass.isEntityValid(g_manager,{r.Index,r.Serial})) ++g_stale;
        }
        for(const auto& [key,e]:g_cache)
            g_unresolvedCached+=!e.Read || !e.Data.Extent.Valid || !e.Data.TransformValid || (!e.Data.HasSpline && (!e.Data.InteriorKnown || (!e.Data.InteriorOnly && !e.Data.CanonicalBlueprint)));
        return true;
    }
    void ObserveBaseline(const MiniMapBuildingInventory::Record& r)
    {
        if(!g_comparison) return;
        if(!r.HasTransform) { ++g_unresolved; return; }
        // Preserve the existing ordinary discovery envelope. This does not claim
        // worldwide completeness for unproven visual footprints beyond it.
        if(!r.HasSpline && !Contains(Pad(g_comparison->Region,g_ordinaryQueryPadding),Box(r.Position[0],r.Position[1],0)))
            return;
        const auto* n=MiniMapNative::Get(); const Id id{r.Index,r.SerialNumber};
        if(!Live(*n,id)) { ++g_unresolved; return; }
        const auto* spline=r.HasSpline?static_cast<const SDK::FAuSplineConnectionFragment*>(Fragment(*n,id,g_types.Spline)):nullptr;
        if(r.HasSpline && !spline) { ++g_unresolved; return; }

        // Re-read native geometry/definition data independently of production
        // records. All worldwide splines are examined, including origin-outside crossings.
        Record geometry; geometry.Position=r.Position; geometry.Rotation=r.Rotation; geometry.Scale=r.Scale;
        geometry.TransformValid=true; geometry.HasSpline=r.HasSpline;
        const auto* parameters=Parameters(*n,id);
        const auto* placement=parameters?parameters->PlacementData:nullptr;
        const auto object=reinterpret_cast<uintptr_t>(placement);
        int32_t serial=0;
        // Independent native definition/tag observation: never consult production
        // InteriorOnly, g_definitions or g_cache to decide baseline eligibility.
        if(!g_types.Placement || !g_types.BuildingData || (object&7)!=0 || !MiniMapResidentMetadata::Serial(object,serial) ||
            !MiniMapResidentMetadata::Validate(object,sizeof(SDK::UAuActorPlacementData)) ||
            !IsClass(placement,g_types.Placement))
        { ++g_unresolved; ++g_eligibilityUnknown; return; }
        const auto kind=ObserveClass(placement,g_types.BuildingData);
        auto tagResult=kind==ClassObservation::Unavailable?InteriorTagResult::Unknown:InteriorTagResult::Absent;
        if(kind==ClassObservation::Matches)
            tagResult=MiniMapResidentMetadata::Validate(object,sizeof(SDK::UCrBuildingData))
                ?ReadInteriorTag(static_cast<const SDK::UCrBuildingData*>(placement))
                :InteriorTagResult::Unknown;
        if(tagResult==InteriorTagResult::Unknown)
        { ++g_unresolved; ++g_eligibilityUnknown; return; }
        if(tagResult==InteriorTagResult::Present)
        { ++g_interiorExcluded; return; }
        {
            const auto key=Key(placement->Index,int32_t(placement->BuildingID));
            auto [entry,inserted]=g_oracleDefinitions.try_emplace(key,Definition{uint32_t(placement->BuildingID),0xFF,
                placement->Name.ComparisonIndex,placement->Name.Number});
            if(!inserted && (entry->second.NameIndex!=placement->Name.ComparisonIndex || entry->second.NameNumber!=placement->Name.Number ||
                entry->second.ObjectSerial!=serial))
                entry->second=Definition{uint32_t(placement->BuildingID),0xFF,placement->Name.ComparisonIndex,placement->Name.Number};
            entry->second.ObjectSerial=serial;
            // Independent canonical acquisition; no production identity cache or
            // eligibility helper supplies the oracle's expected membership.
            if(!r.HasSpline && !entry->second.CanonicalBlueprint && !entry->second.IdentityInterrupted &&
                entry->second.IdentityObservation!=g_oraclePass)
            {
                entry->second.IdentityObservation=g_oraclePass;
                const auto result=MiniMapBuildingBlueprintIdentity::Resolve(placement,g_types.Placement,
                    {placement->Index,placement->Name.ComparisonIndex,placement->Name.Number,uint32_t(placement->BuildingID),serial});
                entry->second.CanonicalBlueprint=result.Path;
                entry->second.IdentityInterrupted=result.State==MiniMapBuildingBlueprintIdentity::Status::Interrupted;
            }
            if(!r.HasSpline && !entry->second.CanonicalBlueprint) {++g_unresolved;++g_eligibilityUnknown;return;}
            geometry.DefinitionShape=DefinitionShape(entry->second,placement,r.HasSpline,false);
        }
        const auto* targeting=static_cast<const SDK::FCrBuildingAggroTargetDataFragment*>(Fragment(*n,id,g_types.TargetBox));
        Geometry(geometry,spline,targeting?&targeting->BuildingBoundingBox:nullptr,false);
        g_unresolved+=geometry.UnboundedGeometry;
        if(!geometry.UnboundedGeometry && !MiniMapBuildingGeometry::Intersects(geometry.Extent,g_comparison->Region)) return;

        ++g_expected;
        const auto key=Key(id);
        if(g_remaining.erase(key)) return;
        ++g_missingCount; g_missing.insert(key); g_persistent+=g_previousMissing.contains(key);
        alignas(8) std::array<std::byte,0x28> view={};
        n->gridDiagnostic.viewConstruct(view.data(),g_manager,id);
        if(n->gridDiagnostic.viewHasTag(view.data(),g_types.Grid)) ++g_missingGrid;
        else ++g_missingOff;
    }
    void EndComparison(size_t matching,double queryMs,double referenceMs)
    {
        if(!g_comparison) return;
#if MINIMAP_DEBUG_UI
        g_debugOracle={true,g_missingCount,g_persistent,g_stale,g_snapshotDuplicates,
            g_expected,g_expected-g_missingCount,g_unresolvedCached,g_unresolved};
#endif
        // Unknown eligibility contributes to unresolvedWorldwide, never to expected
        // or missing. cacheComplete remains the unchanged production snapshot bit.
        LOG_INFO("MiniMap: BuildingCollector: oracle independent reacquisition (excluded from production acquisition counters) cadence=10s matching=%zu expected=%zu present=%zu missing=%zu repeatedMissing=%zu missingPath(off/grid)=%zu/%zu unresolvedCached=%zu duplicateSnapshot=%zu stale=%zu unresolvedWorldwide=%zu cacheComplete=%d query=%.3fms reference=%.3fms validation=%.3fms interiorExcluded=%zu eligibilityUnknown=%zu",
            matching,g_expected,g_expected-g_missingCount,g_missingCount,g_persistent,g_missingOff,g_missingGrid,
            g_unresolvedCached,g_snapshotDuplicates,g_stale,g_unresolved,int(g_comparison->Complete),
            queryMs,referenceMs,queryMs+Ms(g_compareStart),g_interiorExcluded,g_eligibilityUnknown);
        g_previousMissing.swap(g_missing); g_comparison.reset();
    }
}
#endif
