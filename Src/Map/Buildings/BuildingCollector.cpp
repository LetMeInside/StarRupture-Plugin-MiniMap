#include "CallbackTiming.h"
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingCollector.h"
#include "BuildingGeometry.h"
#include "BuildingInventory.h"
#include "BuildingVisuals.h"
#include "BuildingBlueprintIdentity.h"
#include "BuildingSplineAppearance.h"
#include "Native/ResidentMetadata.h"
#include "Native/NativeVisualAccess.h"
#include "DebugText.h"
#include "Map/Map.h"
#include "Native/NativeApi.h"
#include "Native/DefinitionAcquisitionCore.h"
#include "Native/TextureAccess.h"
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
#include <iomanip>
#include <sstream>
#include <locale>
#include <type_traits>
#include <Windows.h>

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
    struct Acquisition
    {
        bool Complete = false, TintObserved = false;
    };
    struct Entry
    {
        Record Data;
        uint64_t Epoch = 0;
        uint64_t Observation = 0; // Eligible only in this collector invocation.
        Acquisition Acquired;
        Clock::time_point Due = {};
        bool Read = false;
        uint8_t PreviousSources = 0;
    };
    struct IndexedEntry
    {
        Record Data;
        uint64_t SeenQuery = 0;
        IndexedEntry() = default;
        explicit IndexedEntry(const Record& data) : Data(data) {} // try_emplace copies only on insertion.
    };
    struct Definition
    {
        uint32_t Id; uint8_t Category; int32_t NameIndex; uint32_t NameNumber;
        std::shared_ptr<const DefinitionGeometry> Geometry;
        std::shared_ptr<const std::string> CanonicalBlueprint;
        int32_t ObjectSerial=0; // Observed UObject lifetime, not a session generation or pointer.
        uint64_t IdentityObservation=0, GeometryObservation=0;
        bool IdentityInterrupted=false, GeometryHasSpline=false;
        MiniMapBuildingSplineProfiles::Availability ActorClassReference=MiniMapBuildingSplineProfiles::Availability::Unverified;
        uint64_t IdentityDiagnostics=0; // Once per issue for this existing definition owner; no identity registry.
        // Immutable diagnostic publication only. Kept OUTSIDE geometry/profile
        // equality, snapshot identity and every production readiness predicate.
        std::shared_ptr<const MiniMapBuildingSplineAppearance::Report> Appearance;
        bool AppearanceFailureLogged=false;
        bool InteriorTagChecked=false, InteriorOnly=false;
    };
    struct Metrics
    {
        double GridQuery=0, GridExtract=0, OffQuery=0, SplineQuery=0, Reconcile=0,
            Dynamic=0, BoundsTime=0, OffDynamic=0, OffBounds=0, CacheService=0,
            Filter=0, SnapshotTime=0, Total=0, MaxTick=0, GridMax=0,
            DefinitionTime=0, OrdinaryTime=0, SplineTime=0;
        double SnapshotBuild=0;
        size_t OverlapCandidates=0, OverlapComplete=0, OverlapReused=0, OverlapIncomplete=0, OverlapIneligible=0;
        size_t Added=0, Removed=0, OffAdded=0, OffRemoved=0, DefinitionMiss=0, Stale=0, Refreshed=0, Polls=0, IdentityPolls=0, GridCalls=0;
        size_t NewAttempts=0, RetryAttempts=0, ResolvedNow=0, CachedReuse=0, DynamicReads=0,
            TransformReads=0, MembershipTransforms=0, MembershipTags=0, PlacementReads=0, Conversions=0, ConversionSuccess=0,
            InteriorReads=0, OrdinaryGeometry=0, OrdinaryDefinitions=0, SnapshotsBuilt=0, SnapshotsReused=0;
    };
    bool g_initialized=false, g_ready=false, g_typesReady=false, g_unavailableLogged=false;
    uint32_t g_definitionDiagnostics=0; // Four pre-definition categories per collector lifetime/generation.
    bool g_gridSucceeded=false, g_identitySucceeded=false;
    SDK::UWorld* g_world=nullptr;
    const void* g_manager=nullptr; // Game-thread query owner identity only; never published.
    Types g_types;
    Query g_offQuery, g_splineQuery;
    uint64_t g_generation=0, g_epoch=0;
    std::unordered_map<uint64_t,Entry> g_cache;
    std::unordered_map<uint64_t,IndexedEntry> g_indexedCopies; // Copied payload plus current-query membership.
    uint64_t g_indexedQuery=0; // Independent of the acquisition callback observation.
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
        auto* self=GetSelf();
        if(!self || !self->hooks || !self->hooks->ObjectWalker ||
            !self->hooks->ObjectWalker->FindObjectsByNameInto)return nullptr;
        PluginObjectInfo info={};
        if(self->hooks->ObjectWalker->FindObjectsByNameInto(name,PluginObjectLookup_Both,&info,1)!=1 ||
            std::strcmp(info.className,kind)!=0) return nullptr;
        auto* type=static_cast<SDK::UStruct*>(info.object);
        return type->Size==int32_t(size)?type:nullptr;
    }
    bool ResolveTypes(bool splineMetadata=false)
    {
        if(g_typesReady && (!splineMetadata || (g_types.VisualTrait && g_types.CosmeticTrait && g_types.DroneRail && g_types.Walkway && g_types.StaticMesh && g_types.DronesSettings))) return true;
        auto* self=GetSelf();
        if(!self || !self->hooks || !self->hooks->ObjectWalker || !self->hooks->ObjectWalker->IsReady ||
            !self->hooks->ObjectWalker->FindObjectsByNameInto || !self->hooks->ObjectWalker->IsReady()) return false;
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
    namespace P=MiniMapBuildingSplineProfiles;
    void FinalizeProfiles(P::Set& profiles)
    {
        profiles.Acquisition.PlacementUsable=std::any_of(profiles.Profiles.begin(),profiles.Profiles.end(),[](const P::Profile& p)
            {return p.Provenance==P::Origin::PlacementActorClass && MiniMapBuildingSplineEnvelope::Eligibility(p)==0;});
        P::Finalize(profiles);
    }
    // Borrowed reads only inside the existing synchronous game-thread resolver.
    // Unlike ResidentMetadata::Validate/Serial these never probe memory pages.
    bool ProfileReadBytes(uintptr_t address,void* output,size_t size) noexcept
    {
        if(!address || address>UINTPTR_MAX-size)return false;
        __try {std::memcpy(output,reinterpret_cast<const void*>(address),size);return true;}
        __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
            ?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
    }
    struct ProfileReader
    {
        template<class T> bool Read(uintptr_t address,T& value)
        {return ProfileReadBytes(address,&value,sizeof(value));}
    };
    bool ProfileIdentity(uintptr_t object,P::Identity& identity,MiniMapDefinitionAcquisition::IdentityObservation* observation=nullptr)
    {
        const auto* n=MiniMapNative::Get();
        ProfileReader read;int32_t index=-1,serial=0;
        if(!MiniMapDefinitionAcquisition::ReadStableIdentity(read,n?reinterpret_cast<uintptr_t>(n->asset.objectArray):0,
            object,sizeof(SDK::UObject),index,serial,observation))return false;
        identity={index,serial};return true;
    }
    enum class ProfileCheck : uint8_t { Matches,NoMatch,Unverified,Invalid };
    bool ProfileTransient(uintptr_t object,size_t extent,MiniMapDefinitionAcquisition::IdentityObservation* observation,
        MiniMapDefinitionAcquisition::IdentityRole role,uint32_t depth,uintptr_t lookup,
        MiniMapDefinitionAcquisition::IdentityObservation* firstZero=nullptr)
    {
        namespace A=MiniMapDefinitionAcquisition;
        A::IdentityObservation local;auto* out=observation?observation:&local;
        const auto* n=MiniMapNative::Get();ProfileReader read;
        const auto valid=A::ValidateTransientObject(read,n?reinterpret_cast<uintptr_t>(n->asset.objectArray):0,object,extent,out);
        out->Role=role;out->Depth=depth;out->IsLookupResult=object==lookup;
        if(firstZero && firstZero->Predicate==A::IdentityPredicate::Unobserved &&
            out->SlotObserved && out->Serial==0)*firstZero=*out;
        return valid==A::TransientValidity::Valid;
    }
    ProfileCheck ProfileNativeClass(uintptr_t type,const wchar_t* name,const wchar_t* package=L"/Script/Engine",
        MiniMapDefinitionAcquisition::IdentityObservation* observation=nullptr,
        MiniMapDefinitionAcquisition::IdentityRole role=MiniMapDefinitionAcquisition::IdentityRole::ClassNode,
        uint32_t depth=0,uintptr_t lookup=0,MiniMapDefinitionAcquisition::IdentityObservation* firstZero=nullptr)
    {
        namespace A=MiniMapDefinitionAcquisition;
        const auto* n=MiniMapNative::Get();ProfileReader read;SDK::FName typeName{},outerName{};uintptr_t outer=0;
        auto fail=[&](A::IdentityPredicate p){if(observation)observation->Predicate=p;return ProfileCheck::Unverified;};
        if(!n || !n->asset.nameEqualsWide)return fail(A::IdentityPredicate::NameFacility);
        if(!ProfileTransient(type,sizeof(SDK::UObject),observation,role,depth,lookup,firstZero))return ProfileCheck::Unverified;
        if(!read.Read(type+0x18,typeName))return fail(A::IdentityPredicate::NameRead);
        if(!read.Read(type+0x20,outer))return fail(A::IdentityPredicate::OuterRead);
        const auto outerRole=role==A::IdentityRole::MetaclassNode?A::IdentityRole::MetaclassOuter:A::IdentityRole::ClassOuter;
        if(!ProfileTransient(outer,sizeof(SDK::UObject),observation,outerRole,depth,lookup,firstZero))return ProfileCheck::Unverified;
        if(!read.Read(outer+0x18,outerName))return fail(A::IdentityPredicate::PackageRead);
        return typeName.Number==0 && outerName.Number==0 && n->asset.nameEqualsWide(typeName,name) &&
            n->asset.nameEqualsWide(outerName,package)?ProfileCheck::Matches:ProfileCheck::NoMatch;
    }
    ProfileCheck ProfileDerives(uintptr_t object,const SDK::UClass* expected,const wchar_t* nativeName=nullptr,
        const wchar_t* package=L"/Script/Engine",MiniMapDefinitionAcquisition::IdentityObservation* observation=nullptr,
        uintptr_t lookup=0,MiniMapDefinitionAcquisition::IdentityObservation* firstZero=nullptr)
    {
        namespace A=MiniMapDefinitionAcquisition;
        ProfileReader read;uintptr_t type=0;
        if(!ProfileTransient(object,sizeof(SDK::UObject),observation,A::IdentityRole::LookupResult,0,lookup,firstZero))return ProfileCheck::Unverified;
        if(!read.Read(object+0x10,type)){if(observation)observation->Predicate=A::IdentityPredicate::MetaclassRead;return ProfileCheck::Unverified;}
        std::unordered_set<uintptr_t> seen;uint32_t depth=0;
        while(type)
        {
            if(!seen.insert(type).second)
            {
                if(observation){*observation={};observation->Predicate=A::IdentityPredicate::HierarchyCycle;
                    observation->Role=A::IdentityRole::MetaclassNode;observation->Depth=depth;observation->IsLookupResult=type==lookup;}
                return ProfileCheck::Invalid;
            }
            if(!ProfileTransient(type,sizeof(SDK::UClass),observation,A::IdentityRole::MetaclassNode,depth,lookup,firstZero))return ProfileCheck::Unverified;
            if(expected && type==reinterpret_cast<uintptr_t>(expected))return ProfileCheck::Matches;
            const auto nodeObservation=observation?*observation:A::IdentityObservation{};
            if(nativeName)
            {
                const auto native=ProfileNativeClass(type,nativeName,package,observation,A::IdentityRole::MetaclassNode,depth,lookup,firstZero);
                if(native!=ProfileCheck::NoMatch)return native;
            }
            if(observation)*observation=nodeObservation;
            if(!read.Read(type+offsetof(SDK::UStruct,SuperStruct),type)){if(observation)observation->Predicate=A::IdentityPredicate::SuperclassRead;return ProfileCheck::Unverified;}
            ++depth;
        }
        return ProfileCheck::NoMatch;
    }
    void ProfileMesh(uintptr_t mesh,P::Profile& profile)
    {
        namespace A=MiniMapDefinitionAcquisition;
        profile.Geometry.reset();profile.ObservedMeshSerial=P::MeshSerial::Unobserved;
        profile.MeshRead=P::MeshAcquisition::Null;
        if(!mesh)return;
        profile.MeshRead=P::MeshAcquisition::Interrupted;
        const auto world=MiniMapMap::GetWorld();const auto generation=g_generation;
        const auto context=[&](){return world && world==g_world && world==MiniMapMap::GetWorld() &&
            generation==g_generation && MiniMapResidentMetadata::Available();};
        if(!context())return;
        A::IdentityObservation observed;
        profile.MeshRead=P::MeshAcquisition::TransientInvalid;
        if(!ProfileTransient(mesh,sizeof(SDK::UStaticMesh),&observed,A::IdentityRole::LookupResult,0,mesh))return;
        profile.ObservedMeshSerial=observed.Serial==0?P::MeshSerial::Zero:
            observed.Serial>0?P::MeshSerial::Positive:P::MeshSerial::Negative;
        profile.MeshRead=P::MeshAcquisition::RetainFailed;
        if(!MiniMapResidentMetadata::Retain(mesh,sizeof(SDK::UStaticMesh)))return;
        struct RetainedMesh
        {
            uintptr_t Object;bool Held=true;
            bool Release(){Held=false;return MiniMapResidentMetadata::Release(Object);}
            ~RetainedMesh(){if(Held)MiniMapResidentMetadata::Release(Object);}
        } retained{mesh};
        // Only copied POD escapes this scope; neither the serial nor pointer is a cache key.
        P::Bounds bounds;bool copied=false;
        auto inspect=[&]()
        {
            if(!context()){profile.MeshRead=P::MeshAcquisition::Interrupted;return;}
            if(!g_types.StaticMesh){profile.MeshRead=P::MeshAcquisition::TransientInvalid;return;}
            const auto kind=ProfileDerives(mesh,g_types.StaticMesh);
            if(kind!=ProfileCheck::Matches)
            {profile.MeshRead=kind==ProfileCheck::NoMatch?P::MeshAcquisition::ClassMismatch:P::MeshAcquisition::TransientInvalid;return;}
            ProfileReader read;SDK::FBoxSphereBounds native{};
            static_assert(offsetof(SDK::UStaticMesh,ExtendedBounds)==0x240);
            if(!read.Read(mesh+offsetof(SDK::UStaticMesh,ExtendedBounds),native))
            {profile.MeshRead=P::MeshAcquisition::BoundsReadFailed;return;}
            bounds={{native.Origin.X,native.Origin.Y,native.Origin.Z},
                {native.BoxExtent.X,native.BoxExtent.Y,native.BoxExtent.Z},native.SphereRadius};
            if(!P::Valid(bounds)){profile.MeshRead=P::MeshAcquisition::InvalidBounds;return;}
            copied=true;
        };
        inspect(); // The guard releases exactly once on exceptions as well.
        if(!context()) {profile.MeshRead=P::MeshAcquisition::Interrupted;copied=false;}
        else if(!ProfileTransient(mesh,sizeof(SDK::UStaticMesh),nullptr,A::IdentityRole::LookupResult,0,mesh))
        {profile.MeshRead=P::MeshAcquisition::TransientInvalid;copied=false;}
        const bool released=retained.Release();
        if(!released){profile.MeshRead=P::MeshAcquisition::ReleaseFailed;return;}
        if(!context()){profile.MeshRead=P::MeshAcquisition::Interrupted;return;}
        if(copied)
        {
            profile.Geometry=std::make_shared<const P::Mesh>(P::Mesh{{bounds,P::Availability::Available}});
            profile.MeshRead=P::MeshAcquisition::Copied;
        }
    }
    void ProfileTemplate(uintptr_t component,const P::Profile& source,P::Set& profiles)
    {
        ProfileReader read;P::Profile result=source;P::Identity id;
        result.Source=P::Selection::Candidate;
        result.Construction={P::Mechanism::Unknown,P::Availability::Unverified};
        if(!ProfileIdentity(component,id))
        {result.Template.Status=P::Availability::Pending;profiles.Profiles.push_back(std::move(result));return;}
        result.Template={id,P::Availability::Available};
        // Attached static meshes can also extend the visual envelope. They are
        // still candidates: neither enumeration nor their type proves use.
        const auto splineKind=ProfileDerives(component,nullptr,L"SplineMeshComponent");
        const bool splineMesh=splineKind==ProfileCheck::Matches;
        const auto staticKind=splineMesh?ProfileCheck::NoMatch:
            splineKind==ProfileCheck::Unverified?ProfileCheck::Unverified:
            ProfileDerives(component,nullptr,L"StaticMeshComponent");
        if(!splineMesh && staticKind!=ProfileCheck::Matches)
        {
            if(splineKind==ProfileCheck::Unverified || staticKind==ProfileCheck::Unverified)
            {result.Construction.Status=P::Availability::Pending;result.Status=P::State::Pending;}
            else if(splineKind==ProfileCheck::Invalid || staticKind==ProfileCheck::Invalid)
            {result.Construction.Status=P::Availability::Invalid;result.Status=P::State::Partial;}
            else result.Status=P::State::Unsupported;
            profiles.Profiles.push_back(std::move(result));return;
        }
        SDK::FVector location{},scale{};SDK::FRotator rotation{};uintptr_t mesh=0;
        static_assert(offsetof(SDK::USceneComponent,RelativeLocation)==0x140 &&
            offsetof(SDK::USceneComponent,RelativeRotation)==0x158 && offsetof(SDK::USceneComponent,RelativeScale3D)==0x170 &&
            offsetof(SDK::UStaticMeshComponent,StaticMesh)==0x588);
        result.TemplateTransform.Status=P::Availability::Pending;
        if(read.Read(component+offsetof(SDK::USceneComponent,RelativeLocation),location) &&
            read.Read(component+offsetof(SDK::USceneComponent,RelativeRotation),rotation) &&
            read.Read(component+offsetof(SDK::USceneComponent,RelativeScale3D),scale))
        {
            P::RelativeTransform transform{{location.X,location.Y,location.Z},{rotation.Pitch,rotation.Yaw,rotation.Roll},{scale.X,scale.Y,scale.Z}};
            if(MiniMapVisualAccess::Finite(transform.Translation) && MiniMapVisualAccess::Finite(transform.RotationDegrees) &&
                MiniMapVisualAccess::Finite(transform.Scale))result.TemplateTransform={transform,P::Availability::Available};
            else result.TemplateTransform.Status=P::Availability::Invalid;
        }
        if(splineMesh)
        {
            SDK::FSplineMeshParams params{};SDK::FVector up{};uint8_t axis=0;
            if(read.Read(component+offsetof(SDK::USplineMeshComponent,ForwardAxis),axis))
                result.TemplateForwardAxis={axis,axis<=2?P::Availability::Available:P::Availability::Invalid};
            else result.TemplateForwardAxis.Status=P::Availability::Pending;
            result.TemplateCrossSection.Status=P::Availability::Pending;
            if(read.Read(component+offsetof(SDK::USplineMeshComponent,SplineParams),params) &&
                read.Read(component+offsetof(SDK::USplineMeshComponent,SplineUpDir),up))
            {
                P::CrossSection section{{params.StartScale.X,params.StartScale.Y},{params.EndScale.X,params.EndScale.Y},
                    {params.StartOffset.X,params.StartOffset.Y},{params.EndOffset.X,params.EndOffset.Y},params.StartRoll,params.EndRoll,{up.X,up.Y,up.Z}};
                bool valid=MiniMapVisualAccess::Finite(section.Up) && std::isfinite(section.StartRoll) && std::isfinite(section.EndRoll);
                for(const auto* pair:{&section.StartScale,&section.EndScale,&section.StartOffset,&section.EndOffset})
                    for(double value:*pair)valid=valid && std::isfinite(value);
                if(valid)result.TemplateCrossSection={section,P::Availability::Available};
                else result.TemplateCrossSection.Status=P::Availability::Invalid;
            }
        }
        else
        {
            result.TemplateForwardAxis.Status=P::Availability::Unsupported;
            result.TemplateCrossSection.Status=P::Availability::Unsupported;
        }
        result.SplineTemplate=splineMesh && source.TemplateOwner.Status==P::Availability::Available &&
            source.VisualClass.Status==P::Availability::Available && source.Defaults.Status==P::Availability::Available;
        result.MeshRead=P::MeshAcquisition::BoundsReadFailed;
        if(read.Read(component+offsetof(SDK::UStaticMeshComponent,StaticMesh),mesh))ProfileMesh(mesh,result);
        profiles.Profiles.push_back(std::move(result));
    }
    void ProfileClass(const SDK::UClass* visualClass,bool highResolution,P::Set& profiles,
        P::Origin provenance=P::Origin::MassRepresentation,bool* cdoAbsent=nullptr,bool includeNative=true)
    {
        if(cdoAbsent)*cdoAbsent=false;
        if(includeNative)++profiles.Acquisition.Classes;
        ProfileReader read;P::Profile source;P::Identity id;const uintptr_t type=reinterpret_cast<uintptr_t>(visualClass);
        source.HighResolution=highResolution;
        source.Provenance=provenance;
        source.VisualClass.Status=P::Availability::Pending;
        if(!ProfileIdentity(type,id))
        {source.Status=P::State::Pending;profiles.Inventory=P::Availability::Pending;profiles.Profiles.push_back(std::move(source));return;}
        source.VisualClass={id,P::Availability::Available};
        uintptr_t cdo=0;source.Defaults.Status=P::Availability::Pending;
        const bool cdoRead=read.Read(type+offsetof(SDK::UClass,ClassDefaultObject),cdo);
        if(cdoAbsent && cdoRead && !cdo)*cdoAbsent=true;
        if(cdoRead && cdo && ProfileIdentity(cdo,id))
        {
            uint32_t flags=0;
            if(read.Read(cdo+8,flags))
            {
                if(flags&0x10)
                {
                    uintptr_t defaultsClass=0;
                    const auto defaults=ProfileDerives(cdo,visualClass);
                    if(defaults==ProfileCheck::Matches && read.Read(cdo+0x10,defaultsClass) && defaultsClass==type)
                        source.Defaults={id,P::Availability::Available};
                    else if(defaults==ProfileCheck::Matches)source.Defaults.Status=P::Availability::Invalid;
                    else if(defaults==ProfileCheck::NoMatch || defaults==ProfileCheck::Invalid)
                        source.Defaults.Status=P::Availability::Invalid;
                }
                else source.Defaults.Status=P::Availability::Invalid;
            }
        }
        if(includeNative)
        {
            const SDK::UClass* builder=nullptr;size_t meshOffset=0;P::Mechanism mechanism=P::Mechanism::Unknown;
            profiles.Acquisition.Defaults+=source.Defaults.Status==P::Availability::Available;
            static_assert(offsetof(SDK::ACrDronePathPointConnection,SplineMesh)==0x708 &&
                offsetof(SDK::ACrModularRampWalkway,SplineMesh)==0x6f8);
            const auto droneBuilder=source.Defaults.Status==P::Availability::Available?
                ProfileDerives(cdo,g_types.DroneRail):ProfileCheck::Unverified;
            const auto walkwayBuilder=source.Defaults.Status==P::Availability::Available && droneBuilder==ProfileCheck::NoMatch?
                ProfileDerives(cdo,g_types.Walkway):ProfileCheck::Unverified;
            if(droneBuilder==ProfileCheck::Matches)
            {builder=g_types.DroneRail;meshOffset=offsetof(SDK::ACrDronePathPointConnection,SplineMesh);mechanism=P::Mechanism::DroneRail;}
            else if(walkwayBuilder==ProfileCheck::Matches)
            {builder=g_types.Walkway;meshOffset=offsetof(SDK::ACrModularRampWalkway,SplineMesh);mechanism=P::Mechanism::ModularWalkway;}
            if(builder)
            {
                ++profiles.Acquisition.NativeBuilders;
                P::Profile input=source;input.Source=P::Selection::NativeBuilderInput;
                input.Construction={mechanism,P::Availability::Available};
                input.ConstructionForwardAxis={0,P::Availability::Available};
                input.ConstructionTransform={{{},{},{1,1,1}},P::Availability::Available};
                input.ConstructionCrossSection={{{1,1},{1,1},{0,0},{0,0},0,0,{0,0,1}},P::Availability::Available};
                // Do not certify arbitrary Blueprint overrides/additional geometry.
                input.ActiveUse=provenance==P::Origin::MassRepresentation && visualClass==builder?
                    P::Availability::Available:P::Availability::Unverified;
                uintptr_t mesh=0;input.MeshRead=P::MeshAcquisition::BoundsReadFailed;
                if(read.Read(cdo+meshOffset,mesh))ProfileMesh(mesh,input);
                input.Status=input.ActiveUse==P::Availability::Available && input.Geometry &&
                    input.Geometry->LocalBounds.Status==P::Availability::Available?P::State::Complete:P::State::Partial;
                profiles.Profiles.push_back(std::move(input));
            }
            else
            {
                const bool known=source.Defaults.Status==P::Availability::Available && g_types.DroneRail && g_types.Walkway &&
                    droneBuilder==ProfileCheck::NoMatch && walkwayBuilder==ProfileCheck::NoMatch;
                const bool invalid=source.Defaults.Status==P::Availability::Invalid ||
                    droneBuilder==ProfileCheck::Invalid || walkwayBuilder==ProfileCheck::Invalid;
                source.Construction.Status=known?P::Availability::Unsupported:
                    invalid?P::Availability::Invalid:P::Availability::Pending;
                source.Status=known?P::State::Unsupported:invalid?P::State::Partial:P::State::Pending;
                profiles.Profiles.push_back(source);
            }
        }
        std::unordered_set<uintptr_t> seen;
        for(uintptr_t current=type;current && seen.insert(current).second;)
        {
            uintptr_t meta=0,parent=0;
            if(!ProfileIdentity(current,id) || !read.Read(current+0x10,meta))
            {profiles.Inventory=P::Availability::Pending;return;}
            const auto blueprintClass=ProfileNativeClass(meta,L"BlueprintGeneratedClass");
            if(blueprintClass==ProfileCheck::Unverified)
            {profiles.Inventory=P::Availability::Pending;return;}
            if(blueprintClass==ProfileCheck::Matches)
            {
                // Verified FindComponentTemplateByName body: this class's array
                // only. Inventory is copied as candidates, never as active use.
                struct Array {uintptr_t Data;int32_t Num,Max;};Array templates{};
                static_assert(offsetof(SDK::UBlueprintGeneratedClass,ComponentTemplates)==0x220);
                if(!read.Read(current+offsetof(SDK::UBlueprintGeneratedClass,ComponentTemplates),templates))
                {profiles.Inventory=P::Availability::Pending;return;}
                if(templates.Num<0 || templates.Max<templates.Num || (templates.Num && !templates.Data) ||
                    size_t(templates.Num)>SIZE_MAX/sizeof(uintptr_t) ||
                    templates.Data>UINTPTR_MAX-size_t(templates.Num)*sizeof(uintptr_t))
                {profiles.Inventory=P::Availability::Invalid;return;}
                P::Profile owner=source;owner.TemplateOwner={id,P::Availability::Available};
                for(int32_t i=0;i<templates.Num;++i)
                {
                    uintptr_t component=0;
                    if(!read.Read(templates.Data+size_t(i)*sizeof(uintptr_t),component))
                    {profiles.Inventory=P::Availability::Pending;continue;}
                    if(component){++profiles.Acquisition.Templates;ProfileTemplate(component,owner,profiles);}
                }
            }
            if(!read.Read(current+offsetof(SDK::UStruct,SuperStruct),parent))
            {profiles.Inventory=P::Availability::Pending;return;}
            current=parent;
            if(current && seen.contains(current)){profiles.Inventory=P::Availability::Invalid;return;}
        }
    }
    ProfileCheck ProfileActorClass(uintptr_t type,MiniMapDefinitionAcquisition::IdentityObservation* observation=nullptr,
        MiniMapDefinitionAcquisition::IdentityObservation* firstZero=nullptr)
    {
        namespace A=MiniMapDefinitionAcquisition;
        const auto lookup=type;
        // Certify the metaclass before interpreting the returned object as UStruct.
        const auto kind=ProfileDerives(type,nullptr,L"Class",L"/Script/CoreUObject",observation,lookup,firstZero);
        if(kind!=ProfileCheck::Matches){if(kind==ProfileCheck::NoMatch && observation)observation->Predicate=A::IdentityPredicate::MetaclassMismatch;return kind;}
        ProfileReader read;std::unordered_set<uintptr_t> seen;uint32_t depth=0;
        while(type)
        {
            if(!seen.insert(type).second)
            {
                if(observation){*observation={};observation->Predicate=A::IdentityPredicate::HierarchyCycle;
                    observation->Role=A::IdentityRole::ClassNode;observation->Depth=depth;observation->IsLookupResult=type==lookup;}
                return ProfileCheck::Invalid;
            }
            if(!ProfileTransient(type,sizeof(SDK::UClass),observation,A::IdentityRole::ClassNode,depth,lookup,firstZero))return ProfileCheck::Unverified;
            const auto nodeObservation=observation?*observation:A::IdentityObservation{};
            const auto actor=ProfileNativeClass(type,L"Actor",L"/Script/Engine",observation,A::IdentityRole::ClassNode,depth,lookup,firstZero);
            if(actor!=ProfileCheck::NoMatch)return actor;
            if(observation)*observation=nodeObservation;
            if(!read.Read(type+offsetof(SDK::UStruct,SuperStruct),type)){if(observation)observation->Predicate=A::IdentityPredicate::SuperclassRead;return ProfileCheck::Unverified;}
            ++depth;
        }
        if(observation)observation->Predicate=A::IdentityPredicate::ActorMismatch;
        return ProfileCheck::NoMatch;
    }
    struct ProfileRetention
    {
        uintptr_t Object=0; // Borrowed only inside this synchronous stack scope.
        int32_t Index=-1; // Transient slot correspondence, never a published identity.
        ProfileRetention()=default;
        explicit ProfileRetention(uintptr_t held):Object(held){}
        ProfileRetention(const ProfileRetention&)=delete;
        ProfileRetention& operator=(const ProfileRetention&)=delete;
        bool Retain(uintptr_t object,size_t extent)
        {
            if(!MiniMapResidentMetadata::Retain(object,extent))return false;
            Object=object;return true;
        }
        bool Release()
        {
            const auto object=Object;Object=0;
            return !object || MiniMapResidentMetadata::Release(object);
        }
        ~ProfileRetention(){if(Object)Release();}
    };
    bool ProfileCDORelationship(uintptr_t type,uintptr_t cdo,int32_t expectedIndex)
    {
        namespace A=MiniMapDefinitionAcquisition;
        ProfileReader read;uintptr_t current=0,actual=0;uint32_t flags=0;A::IdentityObservation observation;
        return ProfileTransient(cdo,sizeof(SDK::UObject),&observation,A::IdentityRole::LookupResult,0,cdo) &&
            observation.Index==expectedIndex && observation.Serial>=0 &&
            read.Read(cdo+8,flags) && (flags&0x10) && read.Read(cdo+0x10,actual) && actual==type &&
            read.Read(type+offsetof(SDK::UClass,ClassDefaultObject),current) && current==cdo &&
            ProfileDerives(cdo,reinterpret_cast<const SDK::UClass*>(type))==ProfileCheck::Matches;
    }
#include "BuildingSplineAppearance.inl"
    // The caller holds the class and owns the final publication transaction.
    // Leave the CDO held for final revalidation and any stable template inventory.
    template<class Context> bool ProfileNativeCopy(uintptr_t type,P::Set& profiles,P::Profile& copy,
        ProfileRetention& defaults,Context context,bool& nativeBuilder)
    {
        namespace A=MiniMapDefinitionAcquisition;
        copy.Ownership=P::Backing::NativeCopy;copy.Source=P::Selection::NativeBuilderInput;
        copy.Provenance=P::Origin::PlacementActorClass;copy.HighResolution=false;
        copy.Status=P::State::Partial;copy.ActiveUse=P::Availability::Unverified;
        P::Identity id;A::IdentityObservation observation;
        // Optional positive identities remain truthful; zero never becomes {Index,0}.
        if(ProfileIdentity(type,id,&observation))copy.VisualClass={id,P::Availability::Available};
        else if(observation.Predicate!=A::IdentityPredicate::ZeroSerial)return false;
        ProfileReader read;uintptr_t cdo=0;
        if(!context() || !read.Read(type+offsetof(SDK::UClass,ClassDefaultObject),cdo))return false;
        profiles.Acquisition.CdoAbsent=!cdo;
        if(!cdo || !ProfileTransient(cdo,sizeof(SDK::UObject),&observation,A::IdentityRole::LookupResult,0,cdo))return false;
        defaults.Index=observation.Index;
        if(!defaults.Retain(cdo,sizeof(SDK::UObject)))return false;
        if(!context() || !ProfileCDORelationship(type,cdo,defaults.Index))return false;
        if(ProfileIdentity(cdo,id,&observation))copy.Defaults={id,P::Availability::Available};
        else if(observation.Predicate!=A::IdentityPredicate::ZeroSerial)return false;
        profiles.Acquisition.CdoAvailability=copy.Defaults.Status;
        const auto drone=g_types.DroneRail?ProfileDerives(cdo,g_types.DroneRail):ProfileCheck::Unverified;
        const auto walkway=drone==ProfileCheck::NoMatch && g_types.Walkway?
            ProfileDerives(cdo,g_types.Walkway):ProfileCheck::Unverified;
        size_t meshOffset=0;
        static_assert(offsetof(SDK::ACrDronePathPointConnection,SplineMesh)==0x708 &&
            offsetof(SDK::ACrModularRampWalkway,SplineMesh)==0x6f8);
        if(drone==ProfileCheck::Matches)
        {copy.Construction={P::Mechanism::DroneRail,P::Availability::Available};meshOffset=offsetof(SDK::ACrDronePathPointConnection,SplineMesh);}
        else if(walkway==ProfileCheck::Matches)
        {copy.Construction={P::Mechanism::ModularWalkway,P::Availability::Available};meshOffset=offsetof(SDK::ACrModularRampWalkway,SplineMesh);}
        else return drone==ProfileCheck::NoMatch && walkway==ProfileCheck::NoMatch;
        nativeBuilder=true;
        ++profiles.Acquisition.Classes;++profiles.Acquisition.Defaults;++profiles.Acquisition.NativeBuilders;
        copy.ConstructionForwardAxis={0,P::Availability::Available};
        copy.ConstructionTransform={{{},{},{1,1,1}},P::Availability::Available};
        copy.ConstructionCrossSection={{{1,1},{1,1},{0,0},{0,0},0,0,{0,0,1}},P::Availability::Available};
        uintptr_t mesh=0;copy.MeshRead=P::MeshAcquisition::BoundsReadFailed;
        if(read.Read(cdo+meshOffset,mesh))ProfileMesh(mesh,copy);
        profiles.Acquisition.NativeCopyMesh=copy.MeshRead;
        return context() && ProfileCDORelationship(type,cdo,defaults.Index);
    }
    void PlacementProfiles(const SDK::UAuActorPlacementData* placement,Definition& definition,P::Set& profiles,
        const P::Set* previous)
    {
        namespace B=MiniMapBuildingBlueprintIdentity;
        auto& acquisition=profiles.Acquisition;
        // Same definition/resource owner as geometry; never an entity or actor cache.
        const auto previousAppearance=definition.Appearance;
        if(previous && previous->Generation==profiles.Generation && previous->Definition==profiles.Definition &&
            previous->Acquisition.PlacementInventory==P::Availability::Available && !previous->Acquisition.NativeCopyAttempted &&
            std::none_of(previous->Profiles.begin(),previous->Profiles.end(),[](const P::Profile& p)
                {return p.Provenance==P::Origin::PlacementActorClass && P::Pending(p);}))
        {
            acquisition.ActorClassReference=previous->Acquisition.ActorClassReference;
            acquisition.ResidentClass=previous->Acquisition.ResidentClass;
            acquisition.CanonicalClassPath=previous->Acquisition.CanonicalClassPath;
            acquisition.ResidentLookup=previous->Acquisition.ResidentLookup;
            acquisition.IdentityValidation=previous->Acquisition.IdentityValidation;
            acquisition.LookupValidation=previous->Acquisition.LookupValidation;
            acquisition.PublicationIdentity=previous->Acquisition.PublicationIdentity;
            acquisition.TraversalZero=previous->Acquisition.TraversalZero;
            acquisition.PersistentClassIdentity=previous->Acquisition.PersistentClassIdentity;
            acquisition.NativeNumericalCopy=previous->Acquisition.NativeNumericalCopy;
            acquisition.NativeCopyMesh=previous->Acquisition.NativeCopyMesh;
            acquisition.ResidentLookupExecuted=previous->Acquisition.ResidentLookupExecuted;
            acquisition.ResidentLookupCompleted=previous->Acquisition.ResidentLookupCompleted;
            acquisition.ResidentObjectFound=previous->Acquisition.ResidentObjectFound;
            acquisition.CdoAbsent=previous->Acquisition.CdoAbsent;
            acquisition.CdoAvailability=previous->Acquisition.CdoAvailability;
            for(const auto& profile:previous->Profiles)
                if(profile.Provenance==P::Origin::PlacementActorClass)profiles.Profiles.push_back(profile);
            return; // Other pending sources must not retraverse settled placement metadata.
        }
        if(definition.CanonicalBlueprint)definition.ActorClassReference=P::Availability::Available;
        acquisition.ActorClassReference=definition.ActorClassReference;
        acquisition.ResidentClass=P::Availability::Pending;
        acquisition.CanonicalClassPath=definition.CanonicalBlueprint;
        acquisition.ResidentLookup=P::ClassLookup::Unattempted;acquisition.IdentityValidation={};
        acquisition.LookupValidation={};acquisition.PublicationIdentity={};acquisition.TraversalZero={};
        acquisition.ResidentLookupExecuted=false;acquisition.ResidentLookupCompleted=false;acquisition.ResidentObjectFound=false;
        acquisition.CdoAbsent=false;acquisition.CdoAvailability=P::Availability::Unverified;
        const auto world=MiniMapMap::GetWorld();
        if(!world || world!=g_world)
        {profiles.Inventory=P::Availability::Pending;return;}
        if(!MiniMapResidentMetadata::Available())
        {acquisition.ResidentLookup=P::ClassLookup::FacilityUnavailable;profiles.Inventory=P::Availability::Pending;return;}
        if(!definition.CanonicalBlueprint && !definition.IdentityInterrupted &&
            definition.ActorClassReference!=P::Availability::Unsupported &&
            definition.ActorClassReference!=P::Availability::Invalid && definition.IdentityObservation!=g_observation)
        {
            definition.IdentityObservation=g_observation;++g_metrics.Conversions;
            const auto result=B::Resolve(placement,g_types.Placement,
                {placement->Index,definition.NameIndex,definition.NameNumber,definition.Id,definition.ObjectSerial});
            // Conversion failures also include malformed text. Only a copied
            // empty authored reference establishes Missing; do not label an
            // invalid nonempty class path as an absent property.
            SDK::FSoftObjectPtr reference{};ProfileReader read;
            static_assert(sizeof(reference)==0x28 && offsetof(SDK::UAuActorPlacementData,ActorClass)==0x38);
            const bool missing=result.State==B::Status::Invalid &&
                (result.Failure==B::Issue::ReturnedLength || result.Failure==B::Issue::CanonicalPath) &&
                read.Read(reinterpret_cast<uintptr_t>(placement)+offsetof(SDK::UAuActorPlacementData,ActorClass),reference) &&
                reference.ObjectID.AssetPath.PackageName.ComparisonIndex==0 && reference.ObjectID.AssetPath.PackageName.Number==0 &&
                reference.ObjectID.AssetPath.AssetName.ComparisonIndex==0 && reference.ObjectID.AssetPath.AssetName.Number==0 &&
                reference.ObjectID.SubPathString.Num()<=1;
            definition.CanonicalBlueprint=result.Path;
            definition.IdentityInterrupted=result.State==B::Status::Interrupted;
            definition.ActorClassReference=result.Path?P::Availability::Available:
                result.State==B::Status::Invalid?
                    (missing?P::Availability::Unsupported:P::Availability::Invalid):
                result.State==B::Status::Interrupted?P::Availability::Invalid:P::Availability::Pending;
            if(result.Path)++g_metrics.ConversionSuccess;
        }
        acquisition.ActorClassReference=definition.ActorClassReference;
        if(!definition.CanonicalBlueprint)
        {
            acquisition.CanonicalClassPath.reset();
            acquisition.ResidentLookup=definition.ActorClassReference==P::Availability::Invalid?
                P::ClassLookup::CanonicalInvalid:P::ClassLookup::CanonicalUnavailable;
            acquisition.ResidentClass=definition.ActorClassReference==P::Availability::Pending ||
                definition.ActorClassReference==P::Availability::Unverified?P::Availability::Pending:
                definition.ActorClassReference;
            if(acquisition.ResidentClass==P::Availability::Pending)profiles.Inventory=P::Availability::Pending;
            return;
        }
        const auto& path=*definition.CanonicalBlueprint;
        acquisition.CanonicalClassPath=definition.CanonicalBlueprint;
        const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.data(),int(path.size()),nullptr,0);
        if(length<=0){acquisition.ResidentLookup=P::ClassLookup::CanonicalInvalid;acquisition.ResidentClass=P::Availability::Invalid;return;}
        std::wstring wide(size_t(length),L'\0');
        if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.data(),int(path.size()),wide.data(),length)!=length)
        {acquisition.ResidentLookup=P::ClassLookup::CanonicalInvalid;acquisition.ResidentClass=P::Availability::Invalid;return;}
        const auto context=[&]()
        {
            P::Identity current;
            return world==MiniMapMap::GetWorld() && world==g_world && profiles.Generation==g_generation &&
                MiniMapResidentMetadata::Available() && profiles.Definition.Status==P::Availability::Available &&
                ProfileIdentity(reinterpret_cast<uintptr_t>(placement),current) && current==profiles.Definition.Value;
        };
        auto* hooks=GetHooks();auto* engine=hooks?hooks->Engine:nullptr;
        const auto find=engine && engine->GetStaticFindObjectByNameAddress?engine->GetStaticFindObjectByNameAddress():0;
        uintptr_t type=0;
        if(!find)
        {acquisition.ResidentLookup=P::ClassLookup::FacilityUnavailable;profiles.Inventory=P::Availability::Pending;return;}
        acquisition.ResidentLookupExecuted=true;
        acquisition.ResidentLookupCompleted=MiniMapResidentMetadata::FindResident(find,0,wide.c_str(),false,type);
        if(!acquisition.ResidentLookupCompleted)
        {acquisition.ResidentLookup=P::ClassLookup::Interrupted;profiles.Inventory=P::Availability::Pending;return;}
        if(!type)
        {acquisition.ResidentLookup=P::ClassLookup::ReturnedNull;profiles.Inventory=P::Availability::Pending;return;} // Resident-only miss, never load.
        acquisition.ResidentObjectFound=true;
        namespace A=MiniMapDefinitionAcquisition;
        if(!MiniMapResidentMetadata::Available() || world!=MiniMapMap::GetWorld() || world!=g_world || profiles.Generation!=g_generation)
        {acquisition.IdentityValidation.Predicate=A::IdentityPredicate::ContextChanged;profiles.Inventory=P::Availability::Pending;return;}
        if(!ProfileTransient(type,sizeof(SDK::UObject),&acquisition.IdentityValidation,A::IdentityRole::LookupResult,0,type))
        {acquisition.LookupValidation=acquisition.IdentityValidation;acquisition.ResidentLookup=P::ClassLookup::IdentityUnverified;profiles.Inventory=P::Availability::Pending;return;}
        acquisition.LookupValidation=acquisition.IdentityValidation;
        // Retain the eligible UObject before extended class inspection. Retain
        // itself rechecks quiet-game-thread admission and slot correspondence.
        if(!MiniMapResidentMetadata::Retain(type,sizeof(SDK::UObject)))
        {
            acquisition.IdentityValidation.Predicate=A::IdentityPredicate::RetainFailed;
            acquisition.ResidentLookup=P::ClassLookup::IdentityUnverified;profiles.Inventory=P::Availability::Pending;return;
        }
        ProfileRetention retained{type};
        ProfileRetention defaults;
        P::Profile copy;bool nativeBuilder=false,ownersValid=false;
        acquisition.NativeCopyAttempted=true;acquisition.NativeNumericalCopy=P::Availability::Pending;
        const auto first=profiles.Profiles.size();bool accepted=false;
        std::shared_ptr<const Appearance::Report> appearance;
        // Early inspection failures leave this lambda, not the retained scope.
        // No caller callback, loading, yielding or Blueprint execution occurs.
        auto inspect=[&]()
        {
            if(!MiniMapResidentMetadata::Available() || world!=MiniMapMap::GetWorld() || world!=g_world || profiles.Generation!=g_generation)
            {
                acquisition.IdentityValidation.Predicate=A::IdentityPredicate::ContextChanged;
                acquisition.ResidentLookup=P::ClassLookup::Interrupted;profiles.Inventory=P::Availability::Pending;return;
            }
            if(!ProfileTransient(type,sizeof(SDK::UObject),&acquisition.IdentityValidation,A::IdentityRole::LookupResult,0,type) ||
                acquisition.IdentityValidation.Index!=acquisition.LookupValidation.Index)
            {acquisition.ResidentLookup=P::ClassLookup::IdentityUnverified;profiles.Inventory=P::Availability::Pending;return;}
            const auto actor=ProfileActorClass(type,&acquisition.IdentityValidation,&acquisition.TraversalZero);
            if(actor==ProfileCheck::NoMatch)
            {acquisition.ResidentLookup=P::ClassLookup::TypeRejected;acquisition.ResidentClass=P::Availability::Invalid;return;}
            if(actor!=ProfileCheck::Matches)
            {
                acquisition.ResidentLookup=actor==ProfileCheck::Invalid?P::ClassLookup::IdentityRejected:P::ClassLookup::IdentityUnverified;
                acquisition.ResidentClass=actor==ProfileCheck::Unverified?P::Availability::Pending:P::Availability::Invalid;
                if(acquisition.ResidentClass==P::Availability::Pending)profiles.Inventory=P::Availability::Pending;
                return;
            }
            acquisition.ResidentLookup=P::ClassLookup::ClassValidated;
            P::Identity before,after;
            const bool publishable=ProfileIdentity(type,before,&acquisition.PublicationIdentity);
            acquisition.PublicationIdentity.Role=A::IdentityRole::PublicationOwner;
            acquisition.PublicationIdentity.IsLookupResult=true;
            acquisition.IdentityValidation=acquisition.PublicationIdentity;
            if(!publishable && acquisition.PublicationIdentity.Predicate!=A::IdentityPredicate::ZeroSerial)
            {acquisition.ResidentLookup=P::ClassLookup::IdentityUnverified;profiles.Inventory=P::Availability::Pending;return;}
            ownersValid=ProfileNativeCopy(type,profiles,copy,defaults,context,nativeBuilder);
            if(!ownersValid)
            {acquisition.ResidentLookup=P::ClassLookup::IdentityUnverified;profiles.Inventory=P::Availability::Pending;return;}
            // This diagnostic is deliberately independent of Mass actors and of
            // geometry eligibility. Failure cannot reject a geometry transaction.
            if(path=="/Game/Chimera/Buildings/Modular/Walkway/BP_Walkway.BP_Walkway_C")
            {
                try
                {
                    AppearanceOps ops;
                    appearance=Appearance::Acquire(ops,type,profiles.Generation,path,previousAppearance);
                }
                catch(...) {} // Diagnostic allocation/inspection failure has no production effect.
                if(!appearance && !definition.AppearanceFailureLogged)
                {
                    definition.AppearanceFailureLogged=true;
                    SpriteDiagnostic("MiniMap: authored spline appearance: definition=%d:%d class=%s unavailable: diagnostic inspection/allocation or scope validation/release failed; no current appearance observation; geometry admission unchanged",
                        profiles.Definition.Value.Index,profiles.Definition.Value.Serial,path.c_str());
                }
            }
            if(!nativeBuilder)
            {
                acquisition.NativeNumericalCopy=P::Availability::Unsupported;
                // Stable template-only observations retain their existing settled
                // reuse. Only native copies/transient owners require refresh.
                acquisition.NativeCopyAttempted=!publishable || copy.Defaults.Status!=P::Availability::Available;
            }
            // Templates still need stable class AND CDO owners. Never use the
            // numerical-copy contract as permission to publish template identities.
            if(publishable && copy.Defaults.Status==P::Availability::Available)
                ProfileClass(reinterpret_cast<const SDK::UClass*>(type),false,profiles,P::Origin::PlacementActorClass,
                    &acquisition.CdoAbsent,!nativeBuilder);
            P::Identity finalDefaults;A::IdentityObservation finalClass;
            const bool stableClass=!publishable || (ProfileIdentity(type,after) && before==after);
            const bool stableDefaults=copy.Defaults.Status!=P::Availability::Available ||
                (ProfileIdentity(defaults.Object,finalDefaults) && finalDefaults==copy.Defaults.Value);
            if(!context() || !ProfileTransient(type,sizeof(SDK::UClass),&finalClass,A::IdentityRole::LookupResult,0,type) ||
                finalClass.Index!=acquisition.LookupValidation.Index || finalClass.Serial<0 ||
                ProfileActorClass(type)!=ProfileCheck::Matches || !ProfileCDORelationship(type,defaults.Object,defaults.Index) ||
                !stableClass || !stableDefaults)
            {
                acquisition.IdentityValidation.Predicate=!stableClass || !stableDefaults?
                    A::IdentityPredicate::IdentityChanged:A::IdentityPredicate::ContextChanged;
                acquisition.ResidentLookup=P::ClassLookup::Interrupted;profiles.Inventory=P::Availability::Pending;return;
            }
            if(nativeBuilder && copy.MeshRead==P::MeshAcquisition::Copied && copy.Geometry)
            {
                // Allocate before releasing owners; failure is contained by the
                // transaction. No seal is exposed until both releases succeed.
                copy.CopyObservation=std::make_shared<const P::NativeCopyObservation>();
                profiles.Profiles.reserve(profiles.Profiles.size()+1);
            }
            accepted=true;
        };
        try{inspect();}
        catch(...)
        {
            profiles.Profiles.resize(first);accepted=false;
            acquisition.ResidentLookup=P::ClassLookup::Interrupted;
            acquisition.ResidentClass=P::Availability::Pending;profiles.Inventory=P::Availability::Pending;
        } // The common exit attempts each successful retention's release once.
        const bool contextValid=context();
        const bool defaultsReleased=defaults.Release();
        const bool released=retained.Release();
        if(!contextValid || !defaultsReleased || !released || !context())
        {
            acquisition.IdentityValidation.Predicate=!defaultsReleased || !released?A::IdentityPredicate::ReleaseFailed:A::IdentityPredicate::ContextChanged;
            acquisition.ResidentLookup=P::ClassLookup::Interrupted;acquisition.ResidentClass=P::Availability::Pending;
            profiles.Inventory=P::Availability::Pending;accepted=false;
        }
        if(!accepted){profiles.Profiles.resize(first);return;}
        if(nativeBuilder && copy.MeshRead==P::MeshAcquisition::Copied && copy.Geometry)
        {
            // Publish the complete observation only after all required releases.
            // Capacity was reserved above; Profile moves own no borrowed pointers.
            profiles.Profiles.insert(profiles.Profiles.begin()+first,std::move(copy));
            acquisition.NativeNumericalCopy=P::Availability::Available;
        }
        else if(nativeBuilder)profiles.Inventory=P::Availability::Pending;
        acquisition.PersistentClassIdentity=std::any_of(profiles.Profiles.begin()+first,profiles.Profiles.end(),[](const P::Profile& p)
            {return p.VisualClass.Status==P::Availability::Available;})?P::Availability::Available:P::Availability::Unverified;
        acquisition.ResidentClass=P::Availability::Available;
        if(appearance)
        {
            definition.Appearance=appearance;
            if(!previousAppearance || !appearance->SameAuthored(*previousAppearance))
            {
                SpriteDiagnostic("MiniMap: authored spline appearance: generation=%llu definition=%d:%d class=%s rows=%zu; authored only; identity_decode=unavailable: NativeApi exposes name equality, not FName/object-path conversion; numeric tokens preserved",
                    static_cast<unsigned long long>(profiles.Generation),profiles.Definition.Value.Index,
                    profiles.Definition.Value.Serial,path.c_str(),appearance->Rows.size());
                for(const auto& row:appearance->Rows)
                    SpriteDiagnostic("MiniMap: authored spline appearance: definition=%d:%d source=%s object={%s} %s",
                        profiles.Definition.Value.Index,profiles.Definition.Value.Serial,row.Source.c_str(),row.Object.c_str(),row.Detail.c_str());
            }
            // Always retain latest owned observations. Streaming changes never
            // republish the authored inventory, but are not frozen or hidden.
            if(!previousAppearance || appearance->Generation!=previousAppearance->Generation ||
                appearance->AuthoredClass!=previousAppearance->AuthoredClass || appearance->Residency!=previousAppearance->Residency)
                for(const auto& row:appearance->Residency)
                    if(!previousAppearance || appearance->Generation!=previousAppearance->Generation ||
                        appearance->AuthoredClass!=previousAppearance->AuthoredClass ||
                        std::find(previousAppearance->Residency.begin(),previousAppearance->Residency.end(),row)==previousAppearance->Residency.end())
                        SpriteDiagnostic("MiniMap: spline texture observation: generation=%llu definition=%d:%d source=%s object={%s} %s",
                            static_cast<unsigned long long>(profiles.Generation),profiles.Definition.Value.Index,
                            profiles.Definition.Value.Serial,row.Source.c_str(),row.Object.c_str(),row.Detail.c_str());
        }
    }
    DefinitionGeometry ResolveGeometry(const SDK::UAuActorPlacementData* placement,bool spline,Definition* definition=nullptr)
    {
        DefinitionGeometry shape;
        std::shared_ptr<P::Set> profiles;
        if(spline)
        {
            profiles=std::make_shared<P::Set>();profiles->Generation=g_generation;
            profiles->Definition.Status=P::Availability::Pending;
            profiles->Inventory=P::Availability::Pending;
            shape.Profiles=profiles;
        }
        if(spline)ResolveTypes(true); // Optional metadata is never an ordinary-building prerequisite.
        if(!placement || (!spline && !g_types.Config) || !MiniMapResidentMetadata::Available() ||
            !MiniMapResidentMetadata::Validate(reinterpret_cast<uintptr_t>(placement),sizeof(SDK::UAuActorPlacementData))) return shape;
        if(profiles)
        {
            profiles->Inventory=P::Availability::Available;
            profiles->Acquisition.Placement=P::Availability::Available;
            profiles->Acquisition.TraitTypesReady=g_types.CosmeticTrait && g_types.VisualTrait;
            P::Identity id;
            if(ProfileIdentity(reinterpret_cast<uintptr_t>(placement),id))profiles->Definition={id,P::Availability::Available};
            if(definition)PlacementProfiles(placement,*definition,*profiles,
                definition->Geometry && definition->Geometry->Profiles?definition->Geometry->Profiles.get():nullptr);
            else profiles->Inventory=P::Availability::Pending;
            profiles->Acquisition.PlacementInventory=profiles->Inventory;
            profiles->Inventory=profiles->Acquisition.TraitTypesReady?P::Availability::Available:P::Availability::Pending;
        }
        auto finish=[&]()
        {
            profiles->Acquisition.MassInventory=profiles->Inventory;
            if(profiles->Acquisition.PlacementInventory==P::Availability::Pending)
                profiles->Inventory=P::Availability::Pending;
            FinalizeProfiles(*profiles);
        };
        auto readTemplate=[&](const SDK::UClass* type,bool highResolution)
        {
            if(profiles)
            {
                ProfileClass(type,highResolution,*profiles);
            }
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
        if(profiles)
        {
            // This SDK field is a transient native cache, distinct from the
            // authored EntityConfig soft reference. Null means unexamined, not
            // an available, empty inventory. Do not load or guess its target.
            profiles->Acquisition.MassCacheAbsent=!config;
            profiles->Acquisition.EntityConfig=config?P::Availability::Available:P::Availability::Pending;
            if(!config)profiles->Inventory=P::Availability::Pending;
        }
        std::array<const SDK::UMassEntityConfigAsset*,32> visited={};
        for(size_t depth=0;config && depth<visited.size();++depth)
        {
            const auto configKind=ObserveClass(config,g_types.Config);
            const bool configReadable=configKind==ClassObservation::Matches &&
                MiniMapResidentMetadata::Validate(reinterpret_cast<uintptr_t>(config),sizeof(SDK::UMassEntityConfigAsset));
            if(profiles && (configKind==ClassObservation::Unavailable ||
                (configKind==ClassObservation::Matches && !configReadable)))
            {
                shape.Issue=GeometryIssue::InvalidArray; // Preserve the existing admission result.
                profiles->Acquisition.EntityConfig=P::Availability::Pending;
                profiles->Inventory=P::Availability::Pending;finish();
                return shape;
            }
            if((reinterpret_cast<uintptr_t>(config)&7) || !configReadable ||
                std::find(visited.begin(),visited.begin()+depth,config)!=visited.begin()+depth)
            {
                shape.Issue=GeometryIssue::InvalidArray;
                if(profiles){profiles->Acquisition.EntityConfig=P::Availability::Invalid;profiles->Inventory=P::Availability::Invalid;finish();}
                return shape;
            }
            visited[depth]=config;
            if(profiles)++profiles->Acquisition.Configs;
            if(profiles)
            {
                P::Identity id;
                if(ProfileIdentity(reinterpret_cast<uintptr_t>(config),id))profiles->Resources.push_back(id);
                else profiles->Inventory=P::Availability::Pending;
            }
            const auto& traits=config->Config.Traits;
            if(traits.Num()<0 || traits.Num()>256 || traits.Max()<traits.Num() || (traits.Num() && !traits.GetDataPtr()))
            {
                shape.Issue=GeometryIssue::InvalidArray;
                if(profiles){profiles->Inventory=P::Availability::Invalid;finish();}
                return shape;
            }
            if(traits.Num() && !MiniMapNative::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(traits.GetDataPtr()),
                size_t(traits.Num())*sizeof(void*),alignof(void*)))
            {
                shape.Issue=GeometryIssue::InvalidArray;
                if(profiles){profiles->Inventory=P::Availability::Invalid;finish();}
                return shape;
            }
            for(const auto* trait:traits)
            {
                if(profiles)++profiles->Acquisition.Traits;
                if(profiles)
                {
                    P::Identity id;
                    if(ProfileIdentity(reinterpret_cast<uintptr_t>(trait),id))profiles->Resources.push_back(id);
                    else profiles->Inventory=P::Availability::Pending;
                }
                if(reinterpret_cast<uintptr_t>(trait)&7)
                {
                    shape.Issue=GeometryIssue::InvalidArray;
                    if(profiles){profiles->Inventory=P::Availability::Invalid;finish();}
                    return shape;
                }
                if(!shape.TargetingBox.Valid && IsClass(trait,g_types.TargetTrait) &&
                    MiniMapResidentMetadata::Validate(reinterpret_cast<uintptr_t>(trait),sizeof(SDK::UCrBuildingAggroTargetDataTrait)))
                    shape.TargetingBox=MiniMapBuildingGeometry::Copy(static_cast<const SDK::UCrBuildingAggroTargetDataTrait*>(trait)->BuildingBoundingBox);
                const auto cosmetic=spline?ObserveClass(trait,g_types.CosmeticTrait):ClassObservation::Other;
                const auto visualKind=spline && cosmetic!=ClassObservation::Matches?
                    ObserveClass(trait,g_types.VisualTrait):ClassObservation::Other;
                if(profiles && (cosmetic==ClassObservation::Unavailable || visualKind==ClassObservation::Unavailable))
                    profiles->Inventory=P::Availability::Pending;
                if(spline && cosmetic==ClassObservation::Matches)
                {
                    ++profiles->Acquisition.VisualTraits;
                    const auto* visual=static_cast<const SDK::UCrMassRepVisCosmeticsTrait*>(trait);
                    readTemplate(visual->HighResTemplateActor,true); readTemplate(visual->LowResTemplateActor,false);
                }
                else if(spline && visualKind==ClassObservation::Matches)
                {
                    ++profiles->Acquisition.VisualTraits;
                    const auto* visual=static_cast<const SDK::UMassVisualizationTrait*>(trait);
                    readTemplate(visual->HighResTemplateActor,true); readTemplate(visual->LowResTemplateActor,false);
                }
            }
            config=config->Config.Parent;
        }
        // Partial resident sources are useful, but do not establish full visual
        // coverage. In particular Blueprint construction scripts, dynamic poles,
        // foundations and mesh/component variants may add geometry.
        if(profiles)
        {
            if(config)profiles->Inventory=P::Availability::Pending; // Parent traversal did not establish absence.
            finish();
        }
        return shape;
    }
    template<class T> void PreserveKnown(P::Field<T>& current,const P::Field<T>& previous)
    {
        if((current.Status==P::Availability::Pending || current.Status==P::Availability::Unverified) &&
            previous.Status!=P::Availability::Pending && previous.Status!=P::Availability::Unverified)
            current=previous;
    }
    bool Compatible(const P::Field<P::Identity>& current,const P::Field<P::Identity>& previous)
    {
        const bool currentKnown=current.Status==P::Availability::Available || current.Status==P::Availability::Invalid;
        const bool previousKnown=previous.Status==P::Availability::Available || previous.Status==P::Availability::Invalid;
        return !currentKnown || !previousKnown || current.Value==previous.Value;
    }
    bool Invalid(const P::Profile& profile)
    {
        return profile.VisualClass.Status==P::Availability::Invalid || profile.Defaults.Status==P::Availability::Invalid ||
            profile.TemplateOwner.Status==P::Availability::Invalid || profile.Template.Status==P::Availability::Invalid ||
            profile.Construction.Status==P::Availability::Invalid || profile.TemplateTransform.Status==P::Availability::Invalid ||
            profile.ConstructionTransform.Status==P::Availability::Invalid || profile.TemplateCrossSection.Status==P::Availability::Invalid ||
            profile.ConstructionCrossSection.Status==P::Availability::Invalid || profile.TemplateForwardAxis.Status==P::Availability::Invalid ||
            profile.ConstructionForwardAxis.Status==P::Availability::Invalid ||
            profile.MeshRead==P::MeshAcquisition::ClassMismatch || profile.MeshRead==P::MeshAcquisition::InvalidBounds ||
            (profile.Geometry && profile.Geometry->LocalBounds.Status==P::Availability::Invalid);
    }
    bool SameOwner(const P::Field<P::Identity>& a,const P::Field<P::Identity>& b)
    {
        return a.Status==P::Availability::Available && b.Status==P::Availability::Available &&
            a.Value.Index>=0 && a.Value.Serial>0 && a.Value==b.Value;
    }
    bool SameProfile(const P::Profile& current,const P::Profile& previous)
    {
        if(P::Copied(current) || P::Copied(previous))return false;
        return current.Provenance==previous.Provenance && current.HighResolution==previous.HighResolution && current.Source==previous.Source &&
            SameOwner(current.VisualClass,previous.VisualClass) && SameOwner(current.Defaults,previous.Defaults) &&
            (current.Source!=P::Selection::Candidate ||
                (SameOwner(current.TemplateOwner,previous.TemplateOwner) && SameOwner(current.Template,previous.Template))) &&
            (current.Construction.Status!=P::Availability::Available || previous.Construction.Status!=P::Availability::Available ||
                current.Construction.Value==previous.Construction.Value);
    }
    bool SameProfileGroup(const P::Profile& current,const P::Profile& previous)
    {
        if(P::Copied(current) || P::Copied(previous))return false;
        return current.Provenance==previous.Provenance && current.HighResolution==previous.HighResolution && current.Source==previous.Source &&
            SameOwner(current.VisualClass,previous.VisualClass) && SameOwner(current.Defaults,previous.Defaults) &&
            (current.Source!=P::Selection::Candidate || SameOwner(current.TemplateOwner,previous.TemplateOwner));
    }
    bool SameResources(const P::Set& current,const P::Set& previous)
    {
        if(current.Inventory!=P::Availability::Pending)return current.Resources==previous.Resources;
        return std::all_of(current.Resources.begin(),current.Resources.end(),[&](P::Identity id)
            {return std::find(previous.Resources.begin(),previous.Resources.end(),id)!=previous.Resources.end();});
    }
    bool SameResolvedProfileIdentities(const P::Set& current,const P::Set& previous)
    {
        auto changed=[](const auto& a,const auto& b)
        {
            const bool knownA=a.Status==P::Availability::Available || a.Status==P::Availability::Invalid;
            const bool knownB=b.Status==P::Availability::Available || b.Status==P::Availability::Invalid;
            return knownA && knownB && a.Value!=b.Value;
        };
        for(const auto& a:current.Profiles)
            for(const auto& b:previous.Profiles)
                if(!P::Copied(a) && !P::Copied(b) && a.Provenance==b.Provenance && a.HighResolution==b.HighResolution && a.Source==b.Source &&
                    (changed(a.VisualClass,b.VisualClass) || changed(a.TemplateOwner,b.TemplateOwner) ||
                        changed(a.Template,b.Template) || changed(a.Defaults,b.Defaults)))return false;
        return true;
    }
    bool SameDefinition(const P::Set& current,const P::Set& previous)
    {
        return current.Generation==previous.Generation && SameOwner(current.Definition,previous.Definition) &&
            SameResources(current,previous) && SameResolvedProfileIdentities(current,previous);
    }
    void PreserveProfiles(P::Set& current,const P::Set& previous)
    {
        if(!SameDefinition(current,previous))return;
        std::vector<bool> used(previous.Profiles.size());
        for(auto& profile:current.Profiles)
        {
            if(P::Copied(profile))continue; // Indivisible; never merge fields from another acquisition.
            size_t match=previous.Profiles.size(),matches=0;
            for(size_t i=0;i<previous.Profiles.size();++i)
                if(!used[i] && SameProfile(profile,previous.Profiles[i])){match=i;++matches;}
            if(matches!=1)continue;
            used[match]=true;const auto& old=previous.Profiles[match];
            PreserveKnown(profile.VisualClass,old.VisualClass);PreserveKnown(profile.Defaults,old.Defaults);
            PreserveKnown(profile.TemplateOwner,old.TemplateOwner);PreserveKnown(profile.Template,old.Template);
            PreserveKnown(profile.Construction,old.Construction);PreserveKnown(profile.TemplateTransform,old.TemplateTransform);
            PreserveKnown(profile.ConstructionTransform,old.ConstructionTransform);
            PreserveKnown(profile.TemplateCrossSection,old.TemplateCrossSection);
            PreserveKnown(profile.ConstructionCrossSection,old.ConstructionCrossSection);
            PreserveKnown(profile.TemplateForwardAxis,old.TemplateForwardAxis);
            PreserveKnown(profile.ConstructionForwardAxis,old.ConstructionForwardAxis);
            if((profile.ActiveUse==P::Availability::Pending || profile.ActiveUse==P::Availability::Unverified) &&
                old.ActiveUse!=P::Availability::Pending && old.ActiveUse!=P::Availability::Unverified)
                profile.ActiveUse=old.ActiveUse;
            // A new mesh observation must stand on its own. Without a persistent
            // mesh key, never restore old bounds over a null/failed/new observation.
            if(!Invalid(profile))
            {
                if(profile.Status!=P::State::Complete && old.Status==P::State::Complete)profile.Status=old.Status;
                else if(profile.Status==P::State::Pending && old.Status!=P::State::Pending)profile.Status=old.Status;
            }
        }
        if(current.Inventory==P::Availability::Pending)
            for(size_t i=0;i<previous.Profiles.size();++i)
            {
                if(used[i] || P::Copied(previous.Profiles[i]) || P::Pending(previous.Profiles[i]))continue;
                if(previous.Profiles[i].Provenance==P::Origin::PlacementActorClass &&
                    (current.Acquisition.NativeCopyAttempted || current.Acquisition.ResidentLookupExecuted))continue;
                const bool replaced=std::any_of(current.Profiles.begin(),current.Profiles.end(),[&](const P::Profile& profile)
                    {return SameProfileGroup(profile,previous.Profiles[i]) && !SameProfile(profile,previous.Profiles[i]);});
                if(!replaced)current.Profiles.push_back(previous.Profiles[i]);
            }
        PreserveKnown(current.Definition,previous.Definition);
        if(current.Inventory==P::Availability::Pending)current.Resources=previous.Resources;
        FinalizeProfiles(current);
    }
    std::shared_ptr<const DefinitionGeometry> DefinitionShape(Definition& definition,const SDK::UAuActorPlacementData* placement,bool spline,bool measure=true)
    {
        // Ordinary definition geometry is copied once. The independent oracle
        // passes measure=false and still observes native geometry for comparison.
        if(measure && !spline && definition.Geometry)return definition.Geometry;
        // Stable definition metadata is reusable; live spline geometry is copied below in Geometry().
        if(measure && spline && definition.Geometry && definition.Geometry->Profiles &&
            !P::NeedsRetry(*definition.Geometry->Profiles,g_generation))return definition.Geometry;
        if(definition.GeometryObservation!=g_observation || (spline && !definition.GeometryHasSpline))
        {
            definition.GeometryObservation=g_observation;
            definition.GeometryHasSpline=spline;
            const auto start=Clock::now();
            if(measure && !spline)++g_metrics.OrdinaryDefinitions;
            auto shape=ResolveGeometry(placement,spline,&definition);
            if(spline && shape.Profiles && definition.Geometry && definition.Geometry->Profiles &&
                P::NeedsRetry(*shape.Profiles,g_generation) &&
                SameDefinition(*shape.Profiles,*definition.Geometry->Profiles))
            {
                const auto& previous=*definition.Geometry;
                if(!shape.TargetingBox.Valid)shape.TargetingBox=previous.TargetingBox;
                const bool preserveNative=!shape.Profiles->Acquisition.NativeCopyAttempted &&
                    !previous.Profiles->Acquisition.NativeCopyAttempted;
                if(preserveNative && !shape.HasSplineMesh && previous.HasSplineMesh)
                {
                    shape.SplineMeshBounds=previous.SplineMeshBounds;
                    shape.SplineMeshCrossSection=previous.SplineMeshCrossSection;
                    shape.VisualClassObjectIndex=previous.VisualClassObjectIndex;
                    shape.SplineMeshObjectIndex=previous.SplineMeshObjectIndex;
                    shape.SplineMeshNameIndex=previous.SplineMeshNameIndex;
                    shape.SplineMeshNameNumber=previous.SplineMeshNameNumber;
                    shape.HasSplineMesh=true;
                }
                if(preserveNative && !shape.HasRailExtension && previous.HasRailExtension)
                {shape.RailEndpointExtension=previous.RailEndpointExtension;shape.HasRailExtension=true;}
            }
            if(spline && shape.Profiles && definition.Geometry && definition.Geometry->Profiles)
            {
                auto profiles=std::make_shared<P::Set>(*shape.Profiles);
                PreserveProfiles(*profiles,*definition.Geometry->Profiles);
                shape.Profiles=std::move(profiles);
            }
            if(shape.Profiles && definition.Geometry && definition.Geometry->Profiles &&
                *shape.Profiles==*definition.Geometry->Profiles)shape.Profiles=definition.Geometry->Profiles;
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
    bool Read(const Native& n,Record& r,Acquisition* acquired=nullptr)
    {
        if(acquired)*acquired={}; // A usable retained transform is not a complete acquisition.
        bool freshTransform=false, freshPlacement=false, tintObserved=false;
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
            { r.Position=position; r.Scale=scale; r.Rotation=rotation; r.TransformValid=true; freshTransform=true; }
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
                freshPlacement=true;
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
        { r.Tint={tint->ColorTint.R,tint->ColorTint.G,tint->ColorTint.B,tint->ColorTint.A}; tintObserved=true; }
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
        if(acquired)
        {
            // Missing optional tint/target bounds are legitimate. Invalid or
            // unresolved geometry keeps the original later acquisition path.
            acquired->TintObserved=tintObserved;
            acquired->Complete=freshTransform && freshPlacement && r.InteriorKnown && !r.InteriorOnly &&
                r.Extent.Valid && !r.UnboundedGeometry && !r.InvalidFootprintSource &&
                (r.HasSpline ? (r.Curve && r.GeometryStatus==GeometryIssue::None) :
                    (r.CanonicalBlueprint && r.GeometryStatus!=GeometryIssue::InvalidTransform));
        }
        return r.TransformValid;
    }
    bool ReuseAcquisition(const Native& n,Record& r,const Entry& entry)
    {
        const auto& sample=entry.Data;
        if(sample.Generation!=g_generation || r.Generation!=g_generation ||
            sample.Index!=r.Index || sample.Serial!=r.Serial)return false;
        // Another acquisition can enrich shared definition geometry in this pass.
        // If its copied metadata changed, retain the original indexed Read.
        const auto definition=g_definitions.find(Key(sample.DefinitionObjectIndex,int32_t(sample.BuildingId)));
        if(definition==g_definitions.end())return false;
        const auto& d=definition->second;
        if(d.ObjectSerial!=sample.DefinitionObjectSerial || d.NameIndex!=sample.DefinitionIndex ||
            d.NameNumber!=sample.DefinitionNumber || !d.InteriorTagChecked || d.InteriorOnly!=sample.InteriorOnly ||
            d.Geometry!=sample.DefinitionShape || (!sample.HasSpline && d.CanonicalBlueprint!=sample.CanonicalBlueprint))return false;
        if(!Live(n,{r.Index,r.Serial}))return false;
        // Only a complete same-invocation sample reaches here. Preserve fields
        // that Read would retain from the INDEXED prior record, not the global one.
        const auto sources=r.Sources, category=r.Category;
        const auto tint=entry.Acquired.TintObserved?sample.Tint:r.Tint;
        const bool dynamic=r.DynamicObserved || r.HasSpline || sample.HasSpline;
        const bool changed=r.DefinitionValid && (r.DefinitionObjectIndex!=sample.DefinitionObjectIndex ||
            r.DefinitionObjectSerial!=sample.DefinitionObjectSerial || r.DefinitionIndex!=sample.DefinitionIndex ||
            r.DefinitionNumber!=sample.DefinitionNumber || r.BuildingId!=sample.BuildingId);
        const auto canonical=sample.HasSpline?(changed?nullptr:r.CanonicalBlueprint):sample.CanonicalBlueprint;
        const auto curve=sample.HasSpline && r.Curve && r.Curve->Issue==sample.Curve->Issue &&
            r.Curve->Reconstructed==sample.Curve->Reconstructed && r.Curve->Segments==sample.Curve->Segments
            ?r.Curve:sample.Curve; // Geometry also retains an equal indexed curve owner.
        r=sample;
        r.Sources=sources; r.Category=category; r.Tint=tint; r.CanonicalBlueprint=canonical; r.Curve=curve;
        r.DynamicObserved=dynamic;
        r.StaticAcquired=!r.HasSpline && r.TransformValid && r.InteriorKnown && !dynamic &&
            (r.InteriorOnly || bool(r.CanonicalBlueprint));
        if(r.StaticAcquired)++g_metrics.ResolvedNow;
        ++g_metrics.Refreshed; // Logical record refresh; native acquisition counters exclude this reuse.
        g_snapshotDirty=true;
        return true;
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
        MiniMapNativeVisualAccess::ResetWorld();
        Destroy();
#if MINIMAP_DEBUG_UI
        MiniMapDebugText::Clear();
        g_debugOracle={}; g_debugLastGridMs=0;
#endif
        g_cache.clear(); g_indexedCopies.clear(); g_observation=0; g_definitions.clear(); g_keys.clear(); g_near.clear(); g_pending.clear();
        g_indexedQuery=0;
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
        MiniMapNativeVisualAccess::InitializeWorld(g_world,g_manager,g_generation);
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
                        entry.Data=indexed->second.Data; // Same entity lifetime changing acquisition source.
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
                    if(added)copy->second.Data.Sources=0;
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
            e.Observation=g_observation;
            e.Read=Read(n,e.Data,&e.Acquired);
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
            if(previous!=g_indexedCopies.end() && previous->second.Data.Position[0]>context.Region.MinX && previous->second.Data.Position[0]<context.Region.MaxX &&
                previous->second.Data.Position[1]>context.Region.MinY && previous->second.Data.Position[1]<context.Region.MaxY)
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
        if(previous!=g_indexedCopies.end() && previous->second.Data.StaticResolved())return keepCopied();
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
    size_t IndexedRecords(const Native& n,const MiniMapMap::PlayerPose& pose)
    {
        size_t indexedCount=0;
        if(++g_indexedQuery==0)
        {
            g_indexedQuery=1;
            for(auto& [key,entry]:g_indexedCopies)entry.SeenQuery=0;
        }
        g_gridSucceeded=false; g_gridResults=0;
        auto copied=[&]()
        {
            for(auto it=g_indexedCopies.begin();it!=g_indexedCopies.end();)
                if(!n.mass.isEntityValid(g_manager,{it->second.Data.Index,it->second.Data.Serial}))
                {it=g_indexedCopies.erase(it);g_snapshotDirty=true;}
                else {if(it->second.Data.Sources&Indexed)++indexedCount;++it;}
            return indexedCount;
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
        for(const auto id:g_indexedHandles)
        {
            const auto key=Key(id);
            auto [it,added]=g_indexedCopies.try_emplace(key);
            auto& entry=it->second;
            if(entry.SeenQuery==g_indexedQuery)continue;
            entry.SeenQuery=g_indexedQuery; // Mark before acquisition, including failed attempts.
            auto& r=entry.Data;
            const auto cached=(added || !r.StaticResolved())?g_cache.find(key):g_cache.end();
            if(added)
            {
                g_snapshotDirty=true;
                if(cached!=g_cache.end())r=cached->second.Data;
                r.Generation=g_generation;r.Index=id.Index;r.Serial=id.SerialNumber;
            }
            if(r.Sources!=Indexed)g_snapshotDirty=true;
            r.Sources=Indexed;
            if(r.StaticResolved())++g_metrics.CachedReuse;
            else
            {
                if(cached!=g_cache.end() && cached->second.Observation==g_observation)
                {
                    ++g_metrics.OverlapCandidates; // Would otherwise perform a second Read attempt.
                    if(!cached->second.Acquired.Complete)++g_metrics.OverlapIncomplete;
                    else
                    {
                        ++g_metrics.OverlapComplete;
                        if(ReuseAcquisition(n,r,cached->second)){++g_metrics.OverlapReused;continue;}
                        ++g_metrics.OverlapIneligible;
                    }
                }
                Read(n,r); // Original path for absent, partial, invalid or incompatible samples.
            }
        }
        for(auto it=g_indexedCopies.begin();it!=g_indexedCopies.end();)
        {
            // Successful enumeration owns scope membership; a failed query does
            // not assert removal. Entity serial validity is the removal oracle.
            if(!n.mass.isEntityValid(g_manager,{it->second.Data.Index,it->second.Data.Serial}) ||
                (g_gridSucceeded && it->second.SeenQuery!=g_indexedQuery))
            {it=g_indexedCopies.erase(it);g_snapshotDirty=true;}
            else {if(it->second.Data.Sources&Indexed)++indexedCount;++it;}
        }
        g_metrics.GridExtract+=Ms(extraction);
        return indexedCount;
    }
    void Publish(size_t indexedCount,const Native& n)
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
        struct Candidate
        {
            uint64_t Identity;
            const Record* Data; // Borrowed only until synchronous materialization below.
        };
        std::vector<Candidate> candidates;
        candidates.reserve(indexedCount+g_near.size());
        g_near.clear(); g_duplicates=0;
        auto add=[&](const Record& r)
        {
            if(r.InteriorOnly) return;
            if(!r.TransformValid || !r.Extent.Valid) { result->Complete=false; return; }
            if(!r.UnboundedGeometry && !MiniMapBuildingGeometry::Intersects(r.Extent,g_region)) return;
            candidates.push_back({Key(r.Index,r.Serial),&r});
            // Publication consumes copied facts only, never reopens native
            // definition/entity provenance to draw an acquired sprite.
            if(!r.HasSpline && (!r.CanonicalBlueprint || !r.InteriorKnown)) result->Complete=false;
        };
        // Pruning has finished; only owned snapshot copies escape this iteration.
        for(const auto& [key,entry]:g_indexedCopies)
            if(entry.Data.Sources&Indexed)add(entry.Data); // Fresh local data wins deterministic dedupe.
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
        std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b)
        {
            if(a.Identity!=b.Identity) return a.Identity<b.Identity;
            if(bool(a.Data->Sources&Indexed)!=bool(b.Data->Sources&Indexed)) return bool(a.Data->Sources&Indexed);
            return a.Data->RefreshedAtSeconds>b.Data->RefreshedAtSeconds;
        });
        result->Records.reserve(candidates.size()); // No relocation of materialized Records.
        for(size_t first=0;first<candidates.size();)
        {
            size_t next=first+1;
            auto sources=candidates[first].Data->Sources;
            while(next<candidates.size() && candidates[next].Identity==candidates[first].Identity)
            { sources|=candidates[next].Data->Sources; ++g_duplicates; ++next; }
            result->Records.push_back(*candidates[first].Data);
            result->Records.back().Sources=sources;
            first=next;
        }
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
        if(++g_observation==0)
        {
            g_observation=1;
            for(auto& [key,d]:g_definitions){d.IdentityObservation=0;d.GeometryObservation=0;}
            for(auto& [key,e]:g_cache)e.Observation=0;
        }
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
            g_indexed=IndexedRecords(*n,pose);
            Publish(g_indexed,*n); ++g_metrics.Polls;
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
            LOG_INFO("MiniMap: BuildingCollector overlap (production interval): candidates=%zu complete=%zu reused=%zu incomplete=%zu ineligible=%zu; reused counts eliminated full acquisitions, excludes membership reads",
                g_metrics.OverlapCandidates,g_metrics.OverlapComplete,g_metrics.OverlapReused,g_metrics.OverlapIncomplete,g_metrics.OverlapIneligible);
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
        Reset(); self->hooks->Engine->RegisterOnTick(&OnTick); g_initialized=true; return true;
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
    MiniMapVisualAccess::Observation ObserveNativeVisuals(const Record& record)
    {
        const MiniMapVisualAccess::Token token{record.Generation,{record.Index,record.Serial}};
        return MiniMapNativeVisualAccess::Observe(token,[](uint64_t generation,const SDK::UWorld* world,const void* manager)
        {
            return g_initialized && MiniMapMap::GameplayReady() && generation==g_generation &&
                world && world==g_world && world==MiniMapMap::GetWorld() && manager && manager==g_manager;
        });
    }
    void Shutdown()
    {
        if(!g_initialized) return;
        auto* self=GetSelf();
        self->hooks->Engine->UnregisterOnTick(&OnTick);
        Reset(); g_initialized=false;
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
