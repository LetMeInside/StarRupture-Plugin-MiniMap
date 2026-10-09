#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingCollector.h"
#include "BuildingGeometry.h"
#include "BuildingInventory.h"
#include "Map.h"
#include "../Native/NativeApi.h"
#include "../plugin_helpers.h"
#include "SDK/Chimera_classes.hpp"

#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using namespace MiniMapBuildingCollector;
    using Clock = std::chrono::steady_clock;
    using Id = MiniMapNative::MassEntityHandle;
    using Native = MiniMapNative::NativeApi;
    constexpr double kOriginPadding = 5000.0; // 50m admission padding; NOT measured mesh bounds.
    constexpr double kSplinePadding = 1000.0; // Curve hull + 10m provisional width allowance.
    constexpr double kMovementPadding = 5000.0;
    constexpr double kFallbackVerticalExtent = 100000.0; // Original policy if optional XY traversal is unavailable.
    constexpr auto kLocalCadence = std::chrono::milliseconds(200);
    constexpr auto kIdentityCadence = std::chrono::seconds(1);
    constexpr auto kDistantCadence = std::chrono::seconds(2);
    constexpr auto kLogCadence = std::chrono::seconds(5);
    constexpr double kRefreshBudgetMs = 2.0; // Soft budget, checked after each entity.
    constexpr size_t kServiceVisitsPerTick = 512;
    constexpr int kMaximumHandles = 1000000;

    struct Types
    {
        SDK::UScriptStruct *Building=nullptr, *Grid=nullptr, *Transform=nullptr,
            *Parameters=nullptr, *Spline=nullptr, *Tint=nullptr;
        SDK::UClass *Placement=nullptr, *BuildingData=nullptr, *GridClass=nullptr;
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
    struct Definition { uint32_t Id; uint8_t Category; int32_t NameIndex; uint32_t NameNumber; };
    struct Metrics
    {
        double GridQuery=0, GridExtract=0, OffQuery=0, SplineQuery=0, Reconcile=0,
            Dynamic=0, BoundsTime=0, OffDynamic=0, OffBounds=0, CacheService=0,
            Filter=0, SnapshotTime=0, Total=0, MaxTick=0, GridMax=0;
        size_t Added=0, Removed=0, OffAdded=0, OffRemoved=0, DefinitionMiss=0, Stale=0, Refreshed=0, Polls=0, IdentityPolls=0, GridCalls=0;
    };
    IPluginSelf* g_collectorSelf=nullptr;
    bool g_initialized=false, g_ready=false, g_typesReady=false, g_unavailableLogged=false;
    bool g_gridSucceeded=false, g_identitySucceeded=false;
    SDK::UWorld* g_world=nullptr;
    const void* g_manager=nullptr; // Game-thread query owner identity only; never published.
    Types g_types;
    Query g_offQuery, g_splineQuery;
    uint64_t g_generation=0, g_epoch=0;
    std::unordered_map<uint64_t,Entry> g_cache;
    std::unordered_map<uint64_t,Definition> g_definitions;
    std::vector<uint64_t> g_keys, g_near, g_pending;
    size_t g_cursor=0, g_nearCursor=0;
    Bounds g_region, g_queryRegion;
    double g_queryZ=0, g_lastRadius=0;
    std::mutex g_mutex;
    double g_viewRadius=0;
    std::shared_ptr<const Snapshot> g_snapshot;
    Clock::time_point g_nextLocal={}, g_nextIdentity={}, g_nextLog={}, g_nextWarning={}, g_nextCompare={};
    Metrics g_metrics;
    size_t g_indexed=0, g_gridResults=0, g_duplicates=0;
    std::vector<Id> g_indexedHandles; // Retains plugin-owned capacity; no grid shared-pointer members.
    size_t g_gridVisited=0;
    bool g_xyQuery=false;

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
    bool IsClass(const SDK::UObject* object,const SDK::UClass* base)
    {
        if(!object || !base) return false;
        const SDK::UStruct* type=object->Class;
        for(unsigned depth=0;type && depth<64;++depth,type=type->SuperStruct)
            if(type==base) return true;
        return false;
    }
    SDK::UObject* Metadata(const char* name,const char* kind,size_t size)
    {
        PluginObjectInfo info={};
        if(g_collectorSelf->hooks->ObjectWalker->FindObjectsByNameInto(name,PluginObjectLookup_Both,&info,1)!=1 ||
            std::strcmp(info.className,kind)!=0) return nullptr;
        auto* type=static_cast<SDK::UStruct*>(info.object);
        return type->Size==int32_t(size)?type:nullptr;
    }
    bool ResolveTypes()
    {
        if(g_typesReady) return true;
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
        g_typesReady=g_types.Building && g_types.Grid && g_types.Transform && g_types.Parameters &&
            g_types.Spline && g_types.Placement && g_types.BuildingData && g_types.GridClass;
        return g_typesReady;
    }
    const void* Fragment(const Native& n,Id id,const SDK::UScriptStruct* type)
    { return type?n.mass.getFragmentDataPtr(g_manager,id,type):nullptr; }
    const SDK::FCrBuildingParameters* Parameters(const Native& n,Id id)
    {
        const auto* wrapper=n.mass.getConstSharedFragmentPtr(g_manager,id,g_types.Parameters);
        const uint8_t* memory=nullptr;
        if(wrapper) std::memcpy(&memory,wrapper,sizeof(memory));
        const auto alignment=g_types.Parameters->MinAlignment;
        if(!memory || alignment<=0 || (alignment&(alignment-1))) return nullptr;
        const uintptr_t base=reinterpret_cast<uintptr_t>(memory+sizeof(void*));
        return reinterpret_cast<const SDK::FCrBuildingParameters*>((base+alignment-1)&~uintptr_t(alignment-1));
    }
    bool Read(const Native& n,Record& r)
    {
        const Id id{r.Index,r.Serial};
        if(!Live(n,id)) { ++g_metrics.Stale; return false; }
        const auto start=Clock::now();
        r.TransformValid=false; r.DefinitionValid=false; r.Extent={}; r.BoundsKind=Coverage::Unresolved;
        if(const auto* t=static_cast<const SDK::FTransform*>(Fragment(n,id,g_types.Transform)))
        {
            r.Position={t->Translation.X,t->Translation.Y,t->Translation.Z};
            r.Rotation={t->Rotation.X,t->Rotation.Y,t->Rotation.Z,t->Rotation.W};
            r.Scale={t->Scale3D.X,t->Scale3D.Y,t->Scale3D.Z};
            auto finite=[](double v){return std::isfinite(v);};
            double norm=0; for(double q:r.Rotation) norm+=q*q;
            r.TransformValid=std::all_of(r.Position.begin(),r.Position.end(),finite) &&
                std::all_of(r.Rotation.begin(),r.Rotation.end(),finite) &&
                std::all_of(r.Scale.begin(),r.Scale.end(),finite) && std::isfinite(norm) && norm>0;
        }
        const auto* parameters=Parameters(n,id);
        const auto* placement=parameters?parameters->PlacementData:nullptr;
        if((reinterpret_cast<uintptr_t>(placement)&7)==0 && IsClass(placement,g_types.Placement))
        {
            r.DefinitionIndex=placement->Name.ComparisonIndex; r.DefinitionNumber=placement->Name.Number;
            r.DefinitionObjectIndex=placement->Index;
            // Short FNames alone can alias assets in different packages.
            const auto key=Key(placement->Index,int32_t(placement->BuildingID));
            auto it=g_definitions.find(key);
            if(it==g_definitions.end() || it->second.NameIndex!=r.DefinitionIndex || it->second.NameNumber!=r.DefinitionNumber)
            {
                uint8_t category=0xFF;
                if(IsClass(placement,g_types.BuildingData)) category=uint8_t(static_cast<const SDK::UCrBuildingData*>(placement)->Type);
                it=g_definitions.insert_or_assign(key,Definition{uint32_t(placement->BuildingID),category,r.DefinitionIndex,r.DefinitionNumber}).first;
                ++g_metrics.DefinitionMiss;
            }
            r.BuildingId=it->second.Id; r.Category=it->second.Category; r.DefinitionValid=true;
        }
        r.Tint={1,1,1,1};
        if(const auto* tint=static_cast<const SDK::FCrBuildingVisualCustomizationFragment*>(Fragment(n,id,g_types.Tint)))
            r.Tint={tint->ColorTint.R,tint->ColorTint.G,tint->ColorTint.B,tint->ColorTint.A};
        const auto* spline=static_cast<const SDK::FAuSplineConnectionFragment*>(Fragment(n,id,g_types.Spline));
        r.HasSpline=spline!=nullptr;
        const double dynamic=Ms(start);
        g_metrics.Dynamic+=dynamic;
        if(r.Sources&OffGrid) g_metrics.OffDynamic+=dynamic;
        const auto boundsStart=Clock::now();
        if(spline)
        {
            r.Extent=Pad(MiniMapBuildingGeometry::SplineBounds(*spline),kSplinePadding);
            if(r.Extent.Valid) r.BoundsKind=Coverage::SplineHull;
        }
        else if(r.TransformValid)
        { r.Extent=Box(r.Position[0],r.Position[1],kOriginPadding); r.BoundsKind=Coverage::OriginPadding; }
        const double boundsTime=Ms(boundsStart);
        g_metrics.BoundsTime+=boundsTime;
        if(r.Sources&OffGrid) g_metrics.OffBounds+=boundsTime;
        r.RefreshedAtSeconds=Seconds();
        ++g_metrics.Refreshed;
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
        Destroy();
        g_cache.clear(); g_definitions.clear(); g_keys.clear(); g_near.clear(); g_pending.clear();
        g_cursor=g_nearCursor=0; g_epoch=0; g_types={}; g_typesReady=false;
        g_region={}; g_queryRegion={}; g_lastRadius=0;
        g_nextLocal=g_nextIdentity=g_nextLog=g_nextWarning=g_nextCompare={}; g_metrics={};
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
                    entry.Data.Generation=g_generation; entry.Data.Index=id.Index; entry.Data.Serial=id.SerialNumber;
                    g_pending.push_back(Key(id));
                    ++g_metrics.Added;
                }
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
            if(!current) { it=g_cache.erase(it); ++g_metrics.Removed; } else ++it;
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
            auto& e=it->second; if(now<e.Due) return;
            e.Read=Read(n,e.Data);
            e.Due=now+(MiniMapBuildingGeometry::Intersects(e.Data.Extent,g_region)?kLocalCadence:kDistantCadence);
        };
        // Newly discovered identities get a bounded priority slice. Initial
        // warm-up also proceeds through the fair sweep, without a load-time burst.
        size_t pendingVisits=0;
        while(!g_pending.empty() && pendingVisits<128 && Ms(start)<kRefreshBudgetMs*0.25)
        { const auto key=g_pending.back(); g_pending.pop_back(); service(key); ++pendingVisits; }
        // Local candidates have priority, but reserve half the budget for
        // discovery/distant validation so a dense factory cannot starve warm-up.
        size_t visited=0;
        while(visited<g_near.size() && visited<kServiceVisitsPerTick && Ms(start)<kRefreshBudgetMs*0.5)
        {
            if(g_nearCursor>=g_near.size()) g_nearCursor=0;
            service(g_near[g_nearCursor++]); ++visited;
        }
        visited=0;
        while(visited<g_keys.size() && visited<kServiceVisitsPerTick && Ms(start)<kRefreshBudgetMs)
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
        if(!Live(n,handle.Entity)) return true;
        const auto* transform=static_cast<const SDK::FTransform*>(Fragment(n,handle.Entity,g_types.Transform));
        if(!transform) return true;
        const auto& p=transform->Translation;
        // Reproduce FindEntitiesInBox's STRICT XY origin tests, with no Z test.
        if(!(p.X>context.Region.MinX && p.X<context.Region.MaxX &&
             p.Y>context.Region.MinY && p.Y<context.Region.MaxY)) return true;
        alignas(8) std::array<std::byte,0x28> view={};
        n.gridDiagnostic.viewConstruct(view.data(),g_manager,handle.Entity);
        // Required BuildingTag filter, same HasTag operation as the native box
        // callback. No unfiltered grid population enters extraction.
        if(!n.gridDiagnostic.viewHasTag(view.data(),g_types.Building)) return true;
        if(g_indexedHandles.size()>=kMaximumHandles) { context.Overflow=true; return false; }
        g_indexedHandles.push_back(handle.Entity); // Copy 8-byte identity only.
        return true; // Native traversal interprets false as early termination.
    }
    void MeasureGridCall(Clock::time_point start)
    {
        const double elapsed=Ms(start);
        g_metrics.GridQuery+=elapsed;
        g_metrics.GridMax=(std::max)(g_metrics.GridMax,elapsed);
        ++g_metrics.GridCalls;
    }
    std::vector<Record> IndexedRecords(const Native& n,const MiniMapMap::PlayerPose& pose)
    {
        std::vector<Record> records;
        g_gridSucceeded=false; g_gridResults=0;
        void* grid=n.gridDiagnostic.getSubsystem(g_world);
        if(!IsClass(static_cast<SDK::UObject*>(grid),g_types.GridClass)) return records;
        const void* manager=nullptr;
        std::memcpy(&manager,static_cast<const uint8_t*>(grid)+0xF0,sizeof(manager));
        if(manager!=g_manager) return records;
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
        records.reserve(g_indexedHandles.size());
        for(const auto id:g_indexedHandles)
        {
            Record r; r.Generation=g_generation; r.Index=id.Index;
            r.Serial=id.SerialNumber; r.Sources=Indexed;
            if(Read(n,r)) records.push_back(r);
        }
        g_metrics.GridExtract+=Ms(extraction);
        return records;
    }
    void Publish(std::vector<Record> indexed,const Native& n)
    {
        const auto start=Clock::now();
        auto result=std::make_shared<Snapshot>(); result->Generation=g_generation; result->Region=g_region;
        result->Complete=g_gridSucceeded && g_identitySucceeded;
        result->Records.reserve(indexed.size()+g_near.size());
        g_near.clear(); g_duplicates=0;
        auto add=[&](const Record& r)
        {
            if(!r.TransformValid || !r.DefinitionValid || !r.Extent.Valid) { result->Complete=false; return; }
            if(!MiniMapBuildingGeometry::Intersects(r.Extent,g_region)) return;
            result->Records.push_back(r);
        };
        for(const auto& r:indexed) add(r); // Fresh local data wins deterministic dedupe.
        for(const auto& [key,e]:g_cache)
        {
            if(!e.Read || !e.Data.Extent.Valid || !e.Data.TransformValid || !e.Data.DefinitionValid) { result->Complete=false; continue; }
            if(!MiniMapBuildingGeometry::Intersects(e.Data.Extent,g_region)) continue;
            g_near.push_back(key);
            if(!Live(n,{e.Data.Index,e.Data.Serial})) { ++g_metrics.Stale; continue; }
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
        { std::scoped_lock lock(g_mutex); g_snapshot=std::move(result); }
        g_metrics.SnapshotTime+=Ms(publication);
    }
    void OnTick(float)
    {
        if(!g_ready || !MiniMapMap::HasWorld()) return;
        const auto start=Clock::now(); const auto* n=MiniMapNative::Get();
        if(!n || !n->gridDiagnostic.IsAvailable() || !n->texture.memoryFree || !n->mass.getConstSharedFragmentPtr ||
            !n->mass.isEntityValid || !n->mass.isEntityBuilt || !n->mass.addSplineRequirement)
        {
            if(!g_unavailableLogged) LOG_WARN("MiniMap: BuildingCollector: optional native prerequisites unavailable; collector disabled");
            g_unavailableLogged=true; return;
        }
        if(!Ensure(*n)) return;
        double radius=0; { std::scoped_lock lock(g_mutex); radius=g_viewRadius; }
        MiniMapMap::PlayerPose pose;
        if(radius<=0 || !MiniMapMap::TryGetPlayerPose(pose)) return;
        const auto now=Clock::now(); g_region=Box(pose.WorldX,pose.WorldY,radius);
        const bool relocate=!Contains(g_queryRegion,Pad(g_region,kOriginPadding)) || radius!=g_lastRadius || std::abs(pose.WorldZ-g_queryZ)>kMovementPadding;
        if(relocate)
        {
            g_queryRegion=Pad(g_region,kOriginPadding+kMovementPadding); g_queryZ=pose.WorldZ; g_lastRadius=radius;
            g_nextLocal={};
            // Newly relevant cached hulls get refreshed promptly, independent of
            // rotation. Unknown bounds still use the fair discovery sweep.
            g_near.clear();
            for(auto& [key,e]:g_cache) if(MiniMapBuildingGeometry::Intersects(e.Data.Extent,g_region))
            { e.Due={}; g_near.push_back(key); }
        }
        if(now>=g_nextIdentity) { Reconcile(*n); g_nextIdentity=now+kIdentityCadence; }
        Refresh(*n);
        if(now>=g_nextLocal)
        {
            g_nextLocal=now+kLocalCadence;
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
            size_t off=0,tagged=0,pending=0,splines=0;
            double maxAge=0,localAge=0;
            for(const auto& [key,e]:g_cache)
            {
                off+=(e.Data.Sources&OffGrid)!=0; tagged+=(e.Data.Sources&IndexedSpline)!=0;
                pending+=!e.Read || !e.Data.Extent.Valid || !e.Data.TransformValid || !e.Data.DefinitionValid;
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
            g_metrics={}; g_nextLog=now+kLogCadence;
        }
    }

    // Stage A validation sink: never feeds records or metadata into acquisition.
    std::shared_ptr<const Snapshot> g_comparison;
    std::unordered_set<uint64_t> g_remaining, g_previousMissing, g_missing;
    size_t g_expected=0,g_missingCount=0,g_persistent=0,g_boundsCases=0,g_unresolved=0,g_outside=0,g_missingOff=0,g_missingGrid=0,g_stale=0;
    size_t g_missingWarmup=0,g_missingUnresolved=0,g_verticalRejected=0,g_snapshotDuplicates=0;
    std::array<size_t,6> g_heightBuckets={},g_indexedHeightBuckets={},g_slabMisses={};
    size_t g_heightIndependent=0,g_heightIndexed=0,g_heightUnresolved=0,g_threeSlabMiss=0,g_xyExcluded=0;
    double g_maxHeight=0,g_maxIndexedHeight=0;
    constexpr std::array<double,5> kHeightBucketsMeters={25,50,100,200,500};
    constexpr std::array<double,6> kTestHalfSlabsMeters={25,50,100,200,500,1000};
    Clock::time_point g_compareStart;
    void ClearComparison()
    {
        g_comparison.reset(); g_remaining.clear(); g_previousMissing.clear(); g_missing.clear();
        g_heightBuckets={}; g_indexedHeightBuckets={}; g_slabMisses={};
        g_heightIndependent=g_heightIndexed=g_heightUnresolved=g_threeSlabMiss=g_xyExcluded=0;
        g_maxHeight=g_maxIndexedHeight=0;
    }
    bool SlabCaptures(const MiniMapBuildingInventory::Record& r,double halfHeight,double shift=0)
    {
        // Diagnostic simulation only: reproduce the box wrapper's float sphere
        // radius and IsInRadius's float squared-distance comparison, then XY.
        if(!(r.Position[0]>g_queryRegion.MinX && r.Position[0]<g_queryRegion.MaxX &&
             r.Position[1]>g_queryRegion.MinY && r.Position[1]<g_queryRegion.MaxY)) return false;
        const double width=g_queryRegion.MaxX-g_queryRegion.MinX;
        const double height=g_queryRegion.MaxY-g_queryRegion.MinY;
        const double depth=halfHeight*2;
        const float radius=static_cast<float>(std::sqrt((width*width+height*height+depth*depth)*0.25));
        const double dx=r.Position[0]-(g_queryRegion.MinX+width*0.5);
        const double dy=r.Position[1]-(g_queryRegion.MinY+height*0.5);
        const double dz=r.Position[2]-(g_queryZ+shift);
        return static_cast<float>(dx*dx+dy*dy+dz*dz)<=radius*radius;
    }
    void ObserveHeight(const MiniMapBuildingInventory::Record& r,const Native& n)
    {
        const double dz=std::abs(r.Position[2]-g_queryZ)/100.0;
        size_t bucket=0;
        while(bucket<kHeightBucketsMeters.size() && dz>kHeightBucketsMeters[bucket]) ++bucket;
        ++g_heightBuckets[bucket]; g_maxHeight=(std::max)(g_maxHeight,dz);
        // Spline geometry may cross at another height than its origin. Neither
        // spline complement depends on a local origin-Z query: classify bypass,
        // never use origin height to exclude its geometry from the oracle.
        if(r.HasSpline) { ++g_heightIndependent; return; }
        const Id id{r.Index,r.SerialNumber};
        if(!Live(n,id)) { ++g_heightUnresolved; return; }
        alignas(8) std::array<std::byte,0x28> view={};
        n.gridDiagnostic.viewConstruct(view.data(),g_manager,id);
        if(!n.gridDiagnostic.viewHasTag(view.data(),g_types.Grid)) { ++g_heightIndependent; return; }
        ++g_heightIndexed; ++g_indexedHeightBuckets[bucket];
        g_maxIndexedHeight=(std::max)(g_maxIndexedHeight,dz);
        for(size_t i=0;i<kTestHalfSlabsMeters.size();++i)
            g_slabMisses[i]+=!SlabCaptures(r,kTestHalfSlabsMeters[i]*100);
        g_threeSlabMiss+=!(SlabCaptures(r,10000,-20000) || SlabCaptures(r,10000) || SlabCaptures(r,10000,20000));
        g_xyExcluded+=!(r.Position[0]>g_queryRegion.MinX && r.Position[0]<g_queryRegion.MaxX &&
                       r.Position[1]>g_queryRegion.MinY && r.Position[1]<g_queryRegion.MaxY);
    }
}

namespace MiniMapBuildingCollector
{
    bool Initialize(IPluginSelf* self)
    {
        if(g_initialized) return true;
        if(!self || !self->hooks || !self->hooks->Engine || !self->hooks->ObjectWalker ||
            !self->hooks->ObjectWalker->IsReady || !self->hooks->ObjectWalker->FindObjectsByNameInto) return false;
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
    void Shutdown()
    {
        if(!g_initialized) return;
        g_collectorSelf->hooks->Engine->UnregisterOnTick(&OnTick); Reset(); g_collectorSelf=nullptr; g_initialized=false;
    }
    bool BeginComparison(const SDK::UWorld* world)
    {
        if(world!=g_world || Clock::now()<g_nextCompare) return false;
        g_comparison=GetSnapshot(); if(!g_comparison) return false;
        g_nextCompare=Clock::now()+std::chrono::seconds(10); g_compareStart=Clock::now();
        g_remaining.clear(); g_missing.clear();
        g_expected=g_missingCount=g_persistent=g_boundsCases=g_unresolved=g_outside=g_missingOff=g_missingGrid=g_stale=0;
        g_missingWarmup=g_missingUnresolved=g_verticalRejected=g_snapshotDuplicates=0;
        g_heightBuckets={}; g_indexedHeightBuckets={}; g_slabMisses={};
        g_heightIndependent=g_heightIndexed=g_heightUnresolved=g_threeSlabMiss=g_xyExcluded=0;
        g_maxHeight=g_maxIndexedHeight=0;
        const auto* n=MiniMapNative::Get();
        for(const auto& r:g_comparison->Records)
        {
            g_snapshotDuplicates+=!g_remaining.insert(Key(r.Index,r.Serial)).second;
            if(!Live(*n,{r.Index,r.Serial})) ++g_stale;
        }
        return true;
    }
    void ObserveBaseline(const MiniMapBuildingInventory::Record& r)
    {
        if(!g_comparison) return;
        if(!r.HasTransform || !r.HasPlacement) { ++g_unresolved; return; }
        Bounds bounds;
        const auto* n=MiniMapNative::Get(); const Id id{r.Index,r.SerialNumber};
        // Diagnostic oracle uses every baseline spline, with no length heuristic.
        // Its native reads and traversal time are excluded from collector timing.
        if(r.HasSpline && Live(*n,id))
        {
            const auto* spline=static_cast<const SDK::FAuSplineConnectionFragment*>(Fragment(*n,id,g_types.Spline));
            if(spline) bounds=Pad(MiniMapBuildingGeometry::SplineBounds(*spline),kSplinePadding);
        }
        else if(!r.HasSpline && r.HasTransform) bounds=Box(r.Position[0],r.Position[1],kOriginPadding);
        if(!bounds.Valid) { ++g_unresolved; return; }
        if(!MiniMapBuildingGeometry::Intersects(bounds,g_comparison->Region)) { ++g_outside; return; }
        ++g_expected;
        ObserveHeight(r,*n);
        if(!Contains(g_comparison->Region,Box(r.Position[0],r.Position[1],0))) ++g_boundsCases;
        const auto key=Key(id);
        if(g_remaining.erase(key)) return;
        ++g_missingCount; g_missing.insert(key); g_persistent+=g_previousMissing.contains(key);
        const auto cached=g_cache.find(key);
        if(cached!=g_cache.end())
        {
            g_missingWarmup+=!cached->second.Read;
            g_missingUnresolved+=cached->second.Read && (!cached->second.Data.Extent.Valid ||
                !cached->second.Data.TransformValid || !cached->second.Data.DefinitionValid);
        }
        if(Live(*n,id))
        {
            alignas(8) std::array<std::byte,0x28> view={};
            n->gridDiagnostic.viewConstruct(view.data(),g_manager,id);
            if(n->gridDiagnostic.viewHasTag(view.data(),g_types.Grid))
            {
                ++g_missingGrid;
                if(!g_xyQuery && !r.HasSpline && !SlabCaptures(r,kFallbackVerticalExtent)) ++g_verticalRejected;
            }
            else ++g_missingOff;
        }
        if(g_missingCount<=4) LOG_DEBUG("MiniMap: BuildingCollector: comparison missing identity=%d:%d buildingID=%u spline=%d cached=%d",id.Index,id.SerialNumber,r.BuildingId,r.HasSpline,int(g_cache.contains(key)));
    }
    void EndComparison()
    {
        if(!g_comparison) return;
        LOG_INFO("MiniMap: BuildingCollector: oracle expected=%zu present=%zu missing=%zu repeatedMissing=%zu missingPath(off/grid)=%zu/%zu warmup=%zu unresolvedCached=%zu sphereRejected=%zu extraOrTemporal=%zu mergedInputs=%zu duplicateSnapshot=%zu stale=%zu boundsAdmission=%zu unresolvedWorldwide=%zu outside=%zu cacheComplete=%d snapshotAge=%.3fs validation=%.3fms",
            g_expected,g_expected-g_missingCount,g_missingCount,g_persistent,g_missingOff,g_missingGrid,g_missingWarmup,g_missingUnresolved,g_verticalRejected,g_remaining.size(),g_duplicates,g_snapshotDuplicates,g_stale,g_boundsCases,g_unresolved,g_outside,int(g_comparison->Complete),Seconds()-g_comparison->PublishedAtSeconds,Ms(g_compareStart));
        LOG_INFO("MiniMap: BuildingCollector: verticalOracle originAbsDZ(m) buckets=[0,25]/(25,50]/(50,100]/(100,200]/(200,500]/>500 all=%zu/%zu/%zu/%zu/%zu/%zu indexedNonSpline=%zu/%zu/%zu/%zu/%zu/%zu max(all/indexed)=%.1f/%.1fm indexed=%zu complementBypass=%zu unresolved=%zu XYexcluded=%zu simulatedSingleSlabMiss(25/50/100/200/500/1000m)=%zu/%zu/%zu/%zu/%zu/%zu threeSlab100mMiss=%zu mode=%s",
            g_heightBuckets[0],g_heightBuckets[1],g_heightBuckets[2],g_heightBuckets[3],g_heightBuckets[4],g_heightBuckets[5],
            g_indexedHeightBuckets[0],g_indexedHeightBuckets[1],g_indexedHeightBuckets[2],g_indexedHeightBuckets[3],g_indexedHeightBuckets[4],g_indexedHeightBuckets[5],
            g_maxHeight,g_maxIndexedHeight,g_heightIndexed,g_heightIndependent,g_heightUnresolved,g_xyExcluded,
            g_slabMisses[0],g_slabMisses[1],g_slabMisses[2],g_slabMisses[3],g_slabMisses[4],g_slabMisses[5],g_threeSlabMiss,g_xyQuery?"XY/all-Z":"fallback-sphere");
        g_previousMissing.swap(g_missing); g_comparison.reset();
    }
}
#endif
