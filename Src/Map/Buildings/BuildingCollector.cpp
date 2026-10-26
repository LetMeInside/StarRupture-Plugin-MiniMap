#include "CallbackTiming.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingCollector.h"
#include "Config/Config.h"
#include "UI/MiniMapUI.h"
#include "BuildingBlueprintIdentity.h"
#include "BuildingGeometry.h"
#include "BuildingSplinePayload.h"
#include "BuildingSplinePolicy.h"
#include "BuildingSplineEnvelope.h"
#include "BuildingSpriteGeometry.h"
#include "BuildingAtlas.h"
#include "BuildingScheduler.h"
#include "Native/NativeApi.h"
#include "Map/Map.h"
#include "plugin_helpers.h"
#include <SDK/Chimera_classes.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using namespace MiniMapBuildingCollector;
    using Native=MiniMapNative::NativeApi;
    using Id=MiniMapNative::MassEntityHandle;
    using Clock=std::chrono::steady_clock;
    constexpr auto kMeasurementInterval=std::chrono::seconds(10);
    namespace P=MiniMapBuildingSplineProfiles;
    namespace G=MiniMapBuildingGeometry;
    struct Types
    {
        SDK::UScriptStruct *Building=nullptr,*Transform=nullptr,*Parameters=nullptr,*Spline=nullptr,*AP=nullptr;
        SDK::UClass *BuildingData=nullptr,*Visual=nullptr,*Cosmetic=nullptr,*Walkway=nullptr,*Blueprint=nullptr,*SplineMesh=nullptr;
    } types;
    struct Entry {Record Data;const SDK::UAuActorPlacementData* Definition=nullptr;bool Complete=false;uint64_t SeenScan=0;};
    struct Definition
    {
        // A strong engine reference makes this engine-facing lookup key stable
        // for the session. It is never exposed to snapshots or renderers.
        SDK::UAuActorPlacementData* Root=nullptr;
        std::shared_ptr<const std::string> Blueprint;
        std::shared_ptr<const DefinitionGeometry> Recipe;
        bool Interior=false,Prepared=false;
        Definition()=default;
        Definition(const Definition&)=delete;
        Definition& operator=(const Definition&)=delete;
        ~Definition(){if(Root)if(const auto* n=MiniMapNative::Get())n->asset.releaseObject(Root);}
    };
    std::unordered_map<uint64_t,Entry> entities;
    // Scheduling index only: complete index/serial keys of exactly the
    // incomplete authoritative entries in this session; no entity data.
    std::unordered_set<uint64_t> pending;
    std::unordered_map<const SDK::UAuActorPlacementData*,std::shared_ptr<Definition>> definitions;
    std::vector<SDK::UObject*> metadata;
    alignas(16) std::array<std::byte,0x350> query{};
    bool queryConstructed=false,initialized=false,ready=false,dirty=false,remoteTurn=false,membershipComplete=false;
    const void* manager=nullptr;
    SDK::UWorld* world=nullptr;
    uint64_t generation=0;
    uint64_t membershipScanEpoch=0;
    Clock::time_point nextMembership{};
    std::mutex publicationMutex;
    std::shared_ptr<const Snapshot> snapshot;
    double viewRadius=0;
    Bounds region;
    uint64_t atlasSession=0,atlasRevision=0;
    struct PhaseAggregate
    {
        uint64_t Samples=0;
        double TotalMs=0,MaxMs=0;
        void Add(double ms) noexcept {++Samples;TotalMs+=ms;MaxMs=(std::max)(MaxMs,ms);}
        double MeanMs() const noexcept {return Samples?TotalMs/double(Samples):0;}
    };
    struct Measurements
    {
        PhaseAggregate Membership,MembershipNative,MembershipBookkeeping,MembershipSweep,MembershipRelease,LocalGrid,PendingEnumeration,Acquisition,Maintenance;
        uint64_t MembershipPolls=0,MembershipChanges=0,GridVisitors=0,UniqueLocalKeys=0;
        uint64_t NativeHandles=0,MembershipAdded=0,MembershipRemoved=0,MembershipDuplicates=0,MembershipInvalid=0;
        uint64_t FailedScans=0,MalformedScans=0,AuthoritativeN=0,PendingCount=0;
        double WorstScanMs=0,WorstScanNativeMs=0,WorstScanBookkeepingMs=0,WorstScanSweepMs=0,WorstScanReleaseMs=0;
        double WorstScanBeginMonoMs=0,WorstScanCallbackMs=0;
        uint64_t WorstScanHandles=0,WorstScanN=0,WorstScanPending=0,WorstScanAdded=0,WorstScanRemoved=0,WorstScanGeneration=0,WorstScanSerial=0;
        uint64_t LocalAttempts=0,RemoteAttempts=0,NewlyCompleted=0;
        uint64_t DefinitionAttempts=0,DefinitionSuccesses=0,DefinitionFailures=0;
        uint64_t PendingN=0,PendingP=0,PendingZeroCallbacks=0,RemotePassesSkipped=0;
        uint64_t AtlasSessionChanges=0,AtlasRevisionChanges=0,AtlasBoundsRecalculations=0;
        uint64_t PublicationAttempts=0,Publications=0;
        uint64_t DirtyMembership=0,DirtyRegion=0,DirtyAtlas=0,DirtyAcquisition=0;
        void Reset() noexcept {*this={};}
    } measurements;
    Clock::time_point measurementIntervalStart{};
    uint64_t measurementGeneration=0;
    uint64_t membershipScanSerial=0,lastMembershipScanSerial=0;
    uint64_t Key(Id id){return uint64_t(uint32_t(id.Index))|(uint64_t(uint32_t(id.SerialNumber))<<32);}
    uint64_t NextMembershipScanEpoch()
    {
        if(membershipScanEpoch==(std::numeric_limits<uint64_t>::max)())
        {
            // Epoch 1 is reused only after every old mark is deterministically cleared.
            for(auto& item:entities)item.second.SeenScan=0;
            membershipScanEpoch=1;
        }
        else ++membershipScanEpoch; // Starts at 1; zero is never an active scan.
        return membershipScanEpoch;
    }
    bool Is(const SDK::UObject* object,const SDK::UClass* base)
    {
        if(!object || !base)return false;
        for(auto* type=object->Class;type;type=static_cast<SDK::UClass*>(type->SuperStruct))if(type==base)return true;
        return false;
    }
    // Called only at initialization/experience initialization, never in an
    // entity-dependent loop. Metadata is rooted until collector shutdown.
    bool InitializeMetadata()
    {
        if(types.Building)return true;
        auto* hooks=GetHooks();const auto* n=MiniMapNative::Get();
        if(!hooks || !hooks->Engine || !n)return false;
        const auto address=hooks->Engine->GetStaticFindObjectByNameAddress();if(!address)return false;
        using Find=SDK::UObject*(*)(SDK::UClass*,SDK::UObject*,const wchar_t*,bool);
        auto find=reinterpret_cast<Find>(address);
        // Roll back partial initialization without duplicating roots on retry.
        struct Roots
        {
            const Native& N;std::vector<SDK::UObject*> Values;
            ~Roots(){for(auto* p:Values)N.asset.releaseObject(p);}
        } roots{*n,{}};
        auto lookup=[&](const wchar_t* path)
        {
            auto* value=find(nullptr,nullptr,path,false);
            if(value){roots.Values.push_back(value);n->asset.retainObject(value);}return value;
        };
        Types candidate;
        candidate.Building=static_cast<SDK::UScriptStruct*>(lookup(L"/Script/Chimera.CrMassBuildingTag"));
        candidate.Transform=static_cast<SDK::UScriptStruct*>(lookup(L"/Script/MassCommon.TransformFragment"));
        candidate.Parameters=static_cast<SDK::UScriptStruct*>(lookup(L"/Script/Chimera.CrBuildingParameters"));
        candidate.Spline=static_cast<SDK::UScriptStruct*>(lookup(L"/Script/AuActorPlacement.AuSplineConnectionFragment"));
        candidate.AP=static_cast<SDK::UScriptStruct*>(lookup(L"/Script/AuActorPlacement.AuAPMassFragment"));
        candidate.BuildingData=static_cast<SDK::UClass*>(lookup(L"/Script/Chimera.CrBuildingData"));
        candidate.Visual=static_cast<SDK::UClass*>(lookup(L"/Script/MassRepresentation.MassVisualizationTrait"));
        candidate.Cosmetic=static_cast<SDK::UClass*>(lookup(L"/Script/Chimera.CrMassRepVisCosmeticsTrait"));
        candidate.Walkway=static_cast<SDK::UClass*>(lookup(L"/Script/Chimera.CrModularRampWalkway"));
        candidate.Blueprint=static_cast<SDK::UClass*>(lookup(L"/Script/Engine.BlueprintGeneratedClass"));
        candidate.SplineMesh=static_cast<SDK::UClass*>(lookup(L"/Script/Engine.SplineMeshComponent"));
        if(!candidate.Building || !candidate.Transform || !candidate.Parameters || !candidate.Spline || !candidate.AP ||
            !candidate.BuildingData || !candidate.Visual || !candidate.Cosmetic || !candidate.Walkway || !candidate.Blueprint || !candidate.SplineMesh ||
            !MiniMapBuildingBlueprintIdentity::Initialize())return false;
        metadata=std::move(roots.Values);types=candidate;return true;
    }
    const SDK::FCrBuildingParameters* Parameters(const Native& n,Id id)
    {
        const auto* wrapper=n.mass.getConstSharedFragmentPtr(manager,id,types.Parameters);
        const uint8_t* storage=nullptr;if(wrapper)std::memcpy(&storage,wrapper,sizeof(storage));
        if(!storage)return nullptr;
        const auto alignment=uintptr_t(types.Parameters->MinAlignment);
        if(!alignment || (alignment&(alignment-1)))return nullptr;
        const auto address=(reinterpret_cast<uintptr_t>(storage)+sizeof(void*)+alignment-1)&~(alignment-1);
        return reinterpret_cast<const SDK::FCrBuildingParameters*>(address);
    }
    void ClearSession()
    {
        if(const auto* n=MiniMapNative::Get())
        {
            if(queryConstructed)n->mass.queryDestruct(query.data());
        }
        queryConstructed=false;manager=nullptr;world=nullptr;region={};atlasSession=atlasRevision=0;
        pending.clear();entities.clear();definitions.clear();nextMembership={};membershipScanEpoch=0;dirty=true;remoteTurn=false;membershipComplete=false;
        {std::scoped_lock lock(publicationMutex);++generation;snapshot.reset();}
    }
    bool Ensure(const Native& n)
    {
        auto* current=MiniMapMap::GetWorld();
        auto* subsystem=current?n.mass.getMassEntitySubsystem(current):nullptr;
        const void* currentManager=nullptr;
        if(subsystem)std::memcpy(&currentManager,reinterpret_cast<const uint8_t*>(subsystem)+0x38,sizeof(currentManager));
        if(world && (world!=current || manager!=currentManager))ClearSession();
        if(!currentManager || !types.Building)return false;
        if(queryConstructed)return true;
        world=current;manager=currentManager;
        n.mass.queryConstruct(query.data(),reinterpret_cast<const uint8_t*>(subsystem)+0x38);queryConstructed=true;
        n.mass.addTagRequirement(query.data(),types.Building,SDK::EMassFragmentPresence::All);
        n.mass.addTransformRequirement(query.data(),uint8_t(SDK::EMassFragmentAccess::ReadOnly),uint8_t(SDK::EMassFragmentPresence::Optional));
        return true;
    }
    struct ReconcileTiming
    {
        Clock::time_point Started=Clock::now();
        double NativeMs=0,BookkeepingMs=0,SweepMs=0,ReleaseMs=0;
        uint64_t Handles=0,Added=0,Removed=0,Duplicates=0,Invalid=0,Epoch=0,Generation=0;
        bool Complete=false,Malformed=false;
        ~ReconcileTiming() noexcept
        {
            const double total=std::chrono::duration<double,std::milli>(Clock::now()-Started).count();
            ++membershipScanSerial;
            measurements.Membership.Add(total);measurements.MembershipNative.Add(NativeMs);
            measurements.MembershipBookkeeping.Add(BookkeepingMs);measurements.MembershipSweep.Add(SweepMs);measurements.MembershipRelease.Add(ReleaseMs);
            measurements.NativeHandles+=Handles;measurements.MembershipAdded+=Added;measurements.MembershipRemoved+=Removed;
            measurements.MembershipDuplicates+=Duplicates;measurements.MembershipInvalid+=Invalid;
            measurements.AuthoritativeN=entities.size();measurements.PendingCount=pending.size();
            if(!Complete)++measurements.FailedScans;
            if(Malformed)++measurements.MalformedScans;
            if(total>=measurements.WorstScanMs)
            {
                measurements.WorstScanMs=total;measurements.WorstScanNativeMs=NativeMs;
                measurements.WorstScanBookkeepingMs=BookkeepingMs;measurements.WorstScanSweepMs=SweepMs;
                measurements.WorstScanReleaseMs=ReleaseMs;measurements.WorstScanHandles=Handles;
                measurements.WorstScanN=entities.size();measurements.WorstScanPending=pending.size();
                measurements.WorstScanAdded=Added;measurements.WorstScanRemoved=Removed;measurements.WorstScanGeneration=Generation;
                measurements.WorstScanBeginMonoMs=std::chrono::duration<double,std::milli>(Started.time_since_epoch()).count();
                measurements.WorstScanSerial=membershipScanSerial;
            }
            lastMembershipScanSerial=membershipScanSerial;
        }
    };
    struct MembershipArrayOwner
    {
        const Native& N;MiniMapNative::MassEntityHandleArray& H;ReconcileTiming& Timing;
        ~MembershipArrayOwner() noexcept
        {
            if(H.Data){const auto begin=Clock::now();N.texture.memoryFree(H.Data);Timing.ReleaseMs=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();}
        }
    };
    bool CurrentScanContext(const Native& n,uint64_t capturedGeneration,SDK::UWorld* capturedWorld,const void* capturedManager)
    {
        if(!queryConstructed || generation!=capturedGeneration || world!=capturedWorld || manager!=capturedManager ||
            MiniMapMap::GetWorld()!=capturedWorld)return false;
        auto* subsystem=capturedWorld?n.mass.getMassEntitySubsystem(capturedWorld):nullptr;
        const void* currentManager=nullptr;
        if(subsystem)std::memcpy(&currentManager,reinterpret_cast<const uint8_t*>(subsystem)+0x38,sizeof(currentManager));
        return currentManager==capturedManager;
    }
    bool Reconcile(const Native& n,uint64_t& changes)
    {
        ReconcileTiming timing;timing.Generation=generation;timing.Epoch=NextMembershipScanEpoch();
        MiniMapNative::MassEntityHandleArray handles{};
        MembershipArrayOwner owner{n,handles,timing};
        const auto capturedGeneration=generation;auto* const capturedWorld=world;const void* const capturedManager=manager;
        const auto nativeBegin=Clock::now();
        MiniMapNative::MassEntityHandleArray* returned=nullptr;
        try {returned=n.mass.getMatchingEntityHandles(query.data(),&handles);}
        catch(...){timing.NativeMs=std::chrono::duration<double,std::milli>(Clock::now()-nativeBegin).count();throw;}
        timing.NativeMs=std::chrono::duration<double,std::milli>(Clock::now()-nativeBegin).count();
        if(returned!=&handles){timing.Malformed=true;return false;}
        if(handles.Num>0)timing.Handles=uint64_t(handles.Num);
        if(handles.Num<0 || handles.Max<handles.Num || (handles.Num && !handles.Data)){timing.Malformed=true;return false;}
        if(!CurrentScanContext(n,capturedGeneration,capturedWorld,capturedManager))return false;

        // A fresh epoch is consumed even when traversal later fails. Partial marks
        // can therefore never certify a future complete scan.
        const auto bookkeepingBegin=Clock::now();
        try
        {
            for(int i=0;i<handles.Num;++i)
            {
                const auto id=handles.Data[i];
                if(id.Index<0 || !id.SerialNumber){++timing.Invalid;continue;}
                const auto key=Key(id);
                auto [it,added]=entities.try_emplace(key);
                if(added)
                {
                    // Preserve the established rollback if pending-index insertion fails.
                    try {pending.insert(key);}
                    catch(...) {entities.erase(it);throw;}
                    auto& r=it->second.Data;r.Generation=capturedGeneration;r.Index=int32_t(uint32_t(key));r.Serial=int32_t(uint32_t(key>>32));
                    dirty=true;++timing.Added;++changes;
                }
                else if(it->second.SeenScan==timing.Epoch)++timing.Duplicates;
                it->second.SeenScan=timing.Epoch;
            }
        }
        catch(...){timing.BookkeepingMs=std::chrono::duration<double,std::milli>(Clock::now()-bookkeepingBegin).count();throw;}
        timing.BookkeepingMs=std::chrono::duration<double,std::milli>(Clock::now()-bookkeepingBegin).count();
        // Query execution may reenter lifecycle code; only the owning world,
        // manager, query and generation may authorize the removal sweep.
        if(!CurrentScanContext(n,capturedGeneration,capturedWorld,capturedManager))return false;

        const auto sweepBegin=Clock::now();
        for(auto it=entities.begin();it!=entities.end();)
            if(it->second.SeenScan!=timing.Epoch)
            {pending.erase(it->first);it=entities.erase(it);dirty=true;++timing.Removed;++changes;}
            else ++it;
        timing.SweepMs=std::chrono::duration<double,std::milli>(Clock::now()-sweepBegin).count();
        timing.Complete=true;
        return true;
    }
    struct Candidate
    {
        P::Profile Numbers;
        std::array<int64_t,12> Order{};
    };
    bool Mesh(SDK::UStaticMesh* mesh,P::Profile& profile)
    {
        if(!mesh)return false;const auto& b=mesh->ExtendedBounds;
        profile.LocalBounds={{b.Origin.X,b.Origin.Y,b.Origin.Z},{b.BoxExtent.X,b.BoxExtent.Y,b.BoxExtent.Z},b.SphereRadius};
        return P::Valid(profile.LocalBounds);
    }
    bool Profiles(SDK::UClass* type,int rank,std::vector<Candidate>& result)
    {
        if(!type || !type->ClassDefaultObject)return false;
        const auto* cdo=type->ClassDefaultObject;
        // The old selector's exact-class preference is preserved numerically.
        if(Is(cdo,types.Walkway))
        {
            Candidate p;p.Order={rank,0,type==types.Walkway?0:1,type->Index,0,0,0,0,0,cdo->Index,0,int64_t(result.size())};
            if(Mesh(static_cast<const SDK::ACrModularRampWalkway*>(cdo)->SplineMesh,p.Numbers))result.push_back(std::move(p));
        }
        for(auto* current=type;current;current=static_cast<SDK::UClass*>(current->SuperStruct))
        {
            if(!Is(current,types.Blueprint))continue;
            const auto& components=static_cast<SDK::UBlueprintGeneratedClass*>(current)->ComponentTemplates;
            for(auto* component:components)
            {
                if(!Is(component,types.SplineMesh))continue;
                const auto* spline=static_cast<const SDK::USplineMeshComponent*>(component);
                Candidate p;p.Order={rank,1,1,type->Index,0,current->Index,0,component->Index,0,cdo->Index,0,int64_t(result.size())};
                auto& v=p.Numbers;
                if(!Mesh(spline->StaticMesh,v))continue;
                v.Transform={{spline->RelativeLocation.X,spline->RelativeLocation.Y,spline->RelativeLocation.Z},
                    {spline->RelativeRotation.Pitch,spline->RelativeRotation.Yaw,spline->RelativeRotation.Roll},
                    {spline->RelativeScale3D.X,spline->RelativeScale3D.Y,spline->RelativeScale3D.Z}};
                const auto& params=spline->SplineParams;
                v.Section={{params.StartScale.X,params.StartScale.Y},{params.EndScale.X,params.EndScale.Y},
                    {params.StartOffset.X,params.StartOffset.Y},{params.EndOffset.X,params.EndOffset.Y},
                    params.StartRoll,params.EndRoll,{spline->SplineUpDir.X,spline->SplineUpDir.Y,spline->SplineUpDir.Z}};
                v.ForwardAxis=uint8_t(spline->ForwardAxis);result.push_back(std::move(p));
            }
        }
        return true;
    }
    // No Mass fragment borrow is live here. The definition root owns its
    // properties; synchronous loading may run engine code before copying POD.
    bool PrepareDefinition(const Native& n,Definition& d,bool spline,uint32_t building)
    {
        if(d.Prepared)return true;
        auto* placement=d.Root;
        d.Interior=false;
        if(Is(placement,types.BuildingData))
            for(const auto tag:static_cast<const SDK::UCrBuildingData*>(placement)->Tags)
                if(tag.Number==0 && n.asset.nameEqualsWide(tag,L"Interior"))d.Interior=true;
        if(d.Interior){d.Prepared=true;return true;}
        if(!d.Blueprint)d.Blueprint=MiniMapBuildingBlueprintIdentity::Copy(*placement);
        if(!d.Blueprint)return false;
        if(!spline || MiniMapBuildingSplinePolicy::IsDroneRailDefinition(true,building))
        {d.Prepared=true;return true;}
        std::vector<Candidate> candidates;
        auto* config=placement->EntityType.EntityConfigPtr;
        if(!config)config=static_cast<SDK::UMassEntityConfigAsset*>(n.asset.loadSynchronous(&placement->EntityType.EntityConfig));
        if(!config && placement->EntityType.EntityConfig.ObjectID.AssetPath.PackageName.ComparisonIndex!=0)return false;
        std::unordered_set<const SDK::UMassEntityConfigAsset*> visited;
        for(;config;config=config->Config.Parent)
        {
            if(!visited.insert(config).second)return false;
            for(auto* trait:config->Config.Traits)
            {
                SDK::UClass *high=nullptr,*low=nullptr;
                if(Is(trait,types.Cosmetic))
                {const auto* v=static_cast<const SDK::UCrMassRepVisCosmeticsTrait*>(trait);high=v->HighResTemplateActor;low=v->LowResTemplateActor;}
                else if(Is(trait,types.Visual))
                {const auto* v=static_cast<const SDK::UMassVisualizationTrait*>(trait);high=v->HighResTemplateActor;low=v->LowResTemplateActor;}
                if(high && !Profiles(high,0,candidates))return false;
                if(low && !Profiles(low,1,candidates))return false;
            }
        }
        auto* authored=static_cast<SDK::UClass*>(n.asset.loadSynchronous(&placement->ActorClass));
        if(!authored || !Profiles(authored,2,candidates))return false;
        std::stable_sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.Order<b.Order;});
        auto profiles=std::make_shared<P::Set>();
        for(auto& candidate:candidates)profiles->Profiles.push_back(std::move(candidate.Numbers));
        if(profiles->Profiles.empty())return false;
        auto recipe=std::make_shared<DefinitionGeometry>();recipe->Profiles=std::move(profiles);d.Recipe=std::move(recipe);d.Prepared=true;return true;
    }
    // This function never loads assets, yields, or retains a fragment/payload.
    bool CopyInitial(const Native& n,Entry& entry,const MiniMapConfig::BuildingRenderSettings& settings)
    {
        auto& r=entry.Data;
        if(r.DefinitionValid && r.TransformValid && (!r.HasSpline || r.Curve))return true;
        const Id id{r.Index,r.Serial};
        if(!r.DefinitionValid)
        {
            const auto* parameters=Parameters(n,id);auto* placement=parameters?parameters->PlacementData:nullptr;
            if(!placement)return false;
            // Exact validated definition admission precedes rooting, transform
            // and spline/payload copying. Leave the identity pending on exclusion.
            if(!settings.RenderDroneRails && MiniMapBuildingSplinePolicy::IsDroneRailDefinition(true,uint32_t(placement->BuildingID)))return false;
            auto it=definitions.find(placement);
            if(it==definitions.end())
            {
                auto definition=std::make_shared<Definition>();
                n.asset.retainObject(placement);definition->Root=placement;
                it=definitions.emplace(placement,std::move(definition)).first;
            }
            entry.Definition=placement;
            r.BuildingId=uint32_t(placement->BuildingID);r.DefinitionObjectIndex=placement->Index;
            r.DefinitionIndex=placement->Name.ComparisonIndex;r.DefinitionNumber=placement->Name.Number;r.DefinitionValid=true;
        }
        if(!r.TransformValid)
        {
            const auto* t=static_cast<const SDK::FTransform*>(n.mass.getFragmentDataPtr(manager,id,types.Transform));
            if(!t)return false;
            r.Position={t->Translation.X,t->Translation.Y,t->Translation.Z};r.Scale={t->Scale3D.X,t->Scale3D.Y,t->Scale3D.Z};
            r.Rotation={t->Rotation.X,t->Rotation.Y,t->Rotation.Z,t->Rotation.W};
            double norm=0;for(auto q:r.Rotation)norm+=q*q;
            if(!G::Finite(r.Position) || !G::Finite(r.Scale) || !std::isfinite(norm) || norm<=0)return false;
            for(auto& q:r.Rotation)q/=std::sqrt(norm);
            r.TransformValid=true;
        }
        const auto* mass=static_cast<const SDK::FAuSplineConnectionFragment*>(n.mass.getFragmentDataPtr(manager,id,types.Spline));
        r.HasSpline=mass!=nullptr;
        if(r.HasSpline && !r.Curve)
        {
            auto curve=std::make_shared<SplineGeometry>();
            if(mass)G::CopySpline(*mass,*curve);
            if(!mass || curve->Issue!=GeometryIssue::None || curve->Segments.empty())
            {
                // A complete/unsupported Mass source is never silently
                // replaced. Zero/one source point is initially incomplete.
                if(mass && (mass->bSplineCurvesInitialized || mass->Data.SplineData.Num()>1))return false;
                const auto* ap=static_cast<const SDK::FAuAPMassFragment*>(n.mass.getFragmentDataPtr(manager,id,types.AP));
                if(!ap || MiniMapBuildingSplinePayload::Copy(ap->Payload,*curve)!=MiniMapBuildingSplinePayload::CopyResult::Complete)return false;
            }
            r.Curve=std::move(curve);r.Extent=r.Curve->CenterlineBounds;
        }
        return true;
    }
    Bounds SplineBounds(const Record& r)
    {
        if(!r.Curve)return {};
        double inflation=MiniMapBuildingSplineEnvelope::ThinHalfWidth;
        if(!MiniMapBuildingSplinePolicy::IsDroneRailDefinition(true,r.BuildingId))
        {
            if(!r.DefinitionShape || !r.DefinitionShape->Profiles)return {};
            inflation=0;
            for(const auto& p:r.DefinitionShape->Profiles->Profiles)
            {
                // Project uses unit orthogonal Y/Z frame vectors. The sum of
                // their maximum scalar magnitudes bounds every orientation;
                // signed TRS is bounded by its largest singular scale.
                const auto& b=p.LocalBounds;const auto& x=p.Section;
                const double y=(std::abs(b.Origin[1])+b.Extent[1])*std::abs(x.StartScale[0])+std::abs(x.StartOffset[0]);
                const double z=(std::abs(b.Origin[2])+b.Extent[2])*std::abs(x.StartScale[1])+std::abs(x.StartOffset[1]);
                double component=0,root=0;
                for(auto v:p.Transform.Scale)component=(std::max)(component,std::abs(v));
                for(auto v:r.Scale)root=(std::max)(root,std::abs(v));
                const auto& t=p.Transform.Translation;
                const double reach=root*(component*(y+z)+std::hypot(t[0],t[1],t[2]));
                if(!std::isfinite(reach))return {}; // Unknown bounds are always admitted.
                inflation=(std::max)(inflation,reach);
            }
        }
        return G::Inflate(r.Curve->CenterlineBounds,inflation+0.01);
    }
    void SpriteBounds(Record& r)
    {
        r.Extent={};
        if(!r.CanonicalBlueprint)return;
        const auto* entry=MiniMapBuildingAtlas::Find(*r.CanonicalBlueprint);
        std::array<MiniMapBuildingVisualGeometry::XY,4> quad;
        if(entry && MiniMapBuildingSpriteGeometry::BuildWorldQuad(r,*entry,quad))
            for(const auto& point:quad)G::Add(r.Extent,point.X,point.Y);
        r.Extent=G::Inflate(r.Extent,0.01);
    }
    void UpdateAtlasBounds()
    {
        std::scoped_lock lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        uint64_t session=0,revision=0;MiniMapBuildingAtlas::ObservePublication(session,revision);
        if(session==atlasSession && revision==atlasRevision)return;
        if(session!=atlasSession)++measurements.AtlasSessionChanges;
        if(revision!=atlasRevision)++measurements.AtlasRevisionChanges;
        for(auto& [key,e]:entities)if(e.Complete && !e.Data.HasSpline){SpriteBounds(e.Data);++measurements.AtlasBoundsRecalculations;}
        atlasSession=session;atlasRevision=revision;dirty=true;++measurements.DirtyAtlas;
    }
    bool Acquire(const Native& n,uint64_t key,std::unordered_set<const Definition*>& attemptedDefinitions,const MiniMapConfig::BuildingRenderSettings& settings)
    {
        auto it=entities.find(key);if(it==entities.end() || it->second.Complete)return false;
        auto& e=it->second;
        if(!settings.RenderDroneRails && MiniMapBuildingSplinePolicy::IsDroneRailDefinition(e.Data.DefinitionValid,e.Data.BuildingId))return false;
        if(!n.mass.isEntityValid(manager,{e.Data.Index,e.Data.Serial}) || !n.mass.isEntityBuilt(manager,{e.Data.Index,e.Data.Serial}) || !CopyInitial(n,e,settings))return false;
        // CopyInitial has returned: no Mass borrow spans definition loading.
        // Loading may dispatch engine callbacks: keep the definition strongly
        // owned, and re-find the entity after returning instead of holding a
        // map entry/reference across that call.
        const auto definition=definitions.at(e.Definition);
        auto result=e.Data;const auto epoch=generation;
        const bool definitionAttempted=!definition->Prepared;
        if(definitionAttempted && !attemptedDefinitions.insert(definition.get()).second)return false;
        if(definitionAttempted)++measurements.DefinitionAttempts;
        const bool definitionReady=PrepareDefinition(n,*definition,result.HasSpline,result.BuildingId);
        if(definitionAttempted){if(definitionReady)++measurements.DefinitionSuccesses;else ++measurements.DefinitionFailures;}
        if(!definitionReady)return false;
        if(epoch!=generation || world!=MiniMapMap::GetWorld())return false;
        it=entities.find(key);
        if(it==entities.end() || it->second.Complete ||
            !n.mass.isEntityValid(manager,{result.Index,result.Serial}) || !n.mass.isEntityBuilt(manager,{result.Index,result.Serial}))return false;
        result.CanonicalBlueprint=definition->Blueprint;result.DefinitionShape=definition->Recipe;
        result.InteriorKnown=true;result.InteriorOnly=definition->Interior;
        if(result.HasSpline)result.Extent=SplineBounds(result);
        else {std::scoped_lock lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);SpriteBounds(result);}
        // Owned Record move and key erasure do not allocate or call native
        // code: completion and removal from scheduling commit together.
        it->second.Data=std::move(result);it->second.Complete=true;it->second.Definition=nullptr;pending.erase(key);dirty=true;++measurements.DirtyAcquisition;return true;
    }
    void Publish(const MiniMapConfig::BuildingRenderSettings& settings)
    {
        ++measurements.PublicationAttempts;
        if(!dirty)return;
        auto value=std::make_shared<Snapshot>();value->Generation=generation;value->Complete=membershipComplete;value->Region=region;
        value->IncludedDroneRails=settings.RenderDroneRails;
        value->PublishedAtSeconds=std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
        for(const auto& [key,e]:entities)if(e.Complete && !e.Data.InteriorOnly &&
            (settings.RenderDroneRails || !MiniMapBuildingSplinePolicy::IsDroneRailDefinition(e.Data.DefinitionValid,e.Data.BuildingId)) &&
            (!region.Valid || !e.Data.Extent.Valid || G::Intersects(e.Data.Extent,region)))value->Records.push_back(e.Data);
        std::sort(value->Records.begin(),value->Records.end(),[](const auto& a,const auto& b){return Key({a.Index,a.Serial})<Key({b.Index,b.Serial});});
        {std::scoped_lock lock(publicationMutex);snapshot=std::move(value);}dirty=false;++measurements.Publications;
    }
    void MaybeReportMeasurements(Clock::time_point callbackBegin,uint64_t scanSerialAtEntry) noexcept
    {
        const auto now=Clock::now();
        const double callbackMs=std::chrono::duration<double,std::milli>(now-callbackBegin).count();
        if(lastMembershipScanSerial!=scanSerialAtEntry && measurements.WorstScanSerial==lastMembershipScanSerial)
            measurements.WorstScanCallbackMs=callbackMs;
        if(measurementGeneration!=generation){measurements.Reset();measurementGeneration=generation;measurementIntervalStart=now;return;}
        // Observe actual counts even on local-first exits, quiet callbacks,
        // and allocation failures; no authoritative-map enumeration required.
        measurements.PendingN=entities.size();measurements.PendingP=pending.size();
        if(measurementIntervalStart==Clock::time_point{}){measurementIntervalStart=now;return;}
        const double intervalMs=std::chrono::duration<double,std::milli>(now-measurementIntervalStart).count();
        if(intervalMs<double(std::chrono::duration_cast<std::chrono::milliseconds>(kMeasurementInterval).count()))return;
        const auto m=measurements;measurements.Reset();measurementIntervalStart=now;
        LOG_INFO("MiniMap: Stage42J collector generation=%llu intervalMs=%.1f membership={samples:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f polls:%llu changes:%llu} grid={samples:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f visitors:%llu uniqueKeys:%llu} pending={samples:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f lastN:%llu lastP:%llu zeroCallbacks:%llu remoteSkipped:%llu} acquisition={samples:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f localAttempts:%llu remoteAttempts:%llu completed:%llu definitionAttempts:%llu definitionSuccesses:%llu definitionFailures:%llu} maintenance={samples:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f atlasSessions:%llu atlasRevisions:%llu boundsRecalcs:%llu publishAttempts:%llu publications:%llu dirtyMembership:%llu dirtyRegion:%llu dirtyAtlas:%llu dirtyAcquisition:%llu}",
            static_cast<unsigned long long>(generation),intervalMs,
            (unsigned long long)m.Membership.Samples,m.Membership.TotalMs,m.Membership.MeanMs(),m.Membership.MaxMs,(unsigned long long)m.MembershipPolls,(unsigned long long)m.MembershipChanges,
            (unsigned long long)m.LocalGrid.Samples,m.LocalGrid.TotalMs,m.LocalGrid.MeanMs(),m.LocalGrid.MaxMs,(unsigned long long)m.GridVisitors,(unsigned long long)m.UniqueLocalKeys,
            (unsigned long long)m.PendingEnumeration.Samples,m.PendingEnumeration.TotalMs,m.PendingEnumeration.MeanMs(),m.PendingEnumeration.MaxMs,(unsigned long long)m.PendingN,(unsigned long long)m.PendingP,(unsigned long long)m.PendingZeroCallbacks,(unsigned long long)m.RemotePassesSkipped,
            (unsigned long long)m.Acquisition.Samples,m.Acquisition.TotalMs,m.Acquisition.MeanMs(),m.Acquisition.MaxMs,(unsigned long long)m.LocalAttempts,(unsigned long long)m.RemoteAttempts,(unsigned long long)m.NewlyCompleted,(unsigned long long)m.DefinitionAttempts,(unsigned long long)m.DefinitionSuccesses,(unsigned long long)m.DefinitionFailures,
            (unsigned long long)m.Maintenance.Samples,m.Maintenance.TotalMs,m.Maintenance.MeanMs(),m.Maintenance.MaxMs,(unsigned long long)m.AtlasSessionChanges,(unsigned long long)m.AtlasRevisionChanges,(unsigned long long)m.AtlasBoundsRecalculations,(unsigned long long)m.PublicationAttempts,(unsigned long long)m.Publications,(unsigned long long)m.DirtyMembership,(unsigned long long)m.DirtyRegion,(unsigned long long)m.DirtyAtlas,(unsigned long long)m.DirtyAcquisition);
        LOG_INFO("MiniMap: Stage42P generation=%llu intervalMs=%.1f native={n:%llu t:%.3f avg:%.3f max:%.3f} book={n:%llu t:%.3f avg:%.3f max:%.3f} sweep={n:%llu t:%.3f avg:%.3f max:%.3f} free={n:%llu t:%.3f avg:%.3f max:%.3f} total={n:%llu t:%.3f avg:%.3f max:%.3f} H:%llu N:%llu P:%llu added:%llu removed:%llu dup:%llu invalid:%llu failed:%llu malformed:%llu worst={ms:%.3f native:%.3f book:%.3f sweep:%.3f free:%.3f monoMs:%.3f tickMs:%.3f gen:%llu H:%llu N:%llu P:%llu add:%llu rem:%llu}",
            (unsigned long long)generation,intervalMs,
            (unsigned long long)m.MembershipNative.Samples,m.MembershipNative.TotalMs,m.MembershipNative.MeanMs(),m.MembershipNative.MaxMs,
            (unsigned long long)m.MembershipBookkeeping.Samples,m.MembershipBookkeeping.TotalMs,m.MembershipBookkeeping.MeanMs(),m.MembershipBookkeeping.MaxMs,
            (unsigned long long)m.MembershipSweep.Samples,m.MembershipSweep.TotalMs,m.MembershipSweep.MeanMs(),m.MembershipSweep.MaxMs,
            (unsigned long long)m.MembershipRelease.Samples,m.MembershipRelease.TotalMs,m.MembershipRelease.MeanMs(),m.MembershipRelease.MaxMs,
            (unsigned long long)m.Membership.Samples,m.Membership.TotalMs,m.Membership.MeanMs(),m.Membership.MaxMs,
            (unsigned long long)m.NativeHandles,(unsigned long long)m.AuthoritativeN,(unsigned long long)m.PendingCount,
            (unsigned long long)m.MembershipAdded,(unsigned long long)m.MembershipRemoved,(unsigned long long)m.MembershipDuplicates,(unsigned long long)m.MembershipInvalid,
            (unsigned long long)m.FailedScans,(unsigned long long)m.MalformedScans,
            m.WorstScanMs,m.WorstScanNativeMs,m.WorstScanBookkeepingMs,m.WorstScanSweepMs,m.WorstScanReleaseMs,
            m.WorstScanBeginMonoMs,m.WorstScanCallbackMs,(unsigned long long)m.WorstScanGeneration,
            (unsigned long long)m.WorstScanHandles,(unsigned long long)m.WorstScanN,(unsigned long long)m.WorstScanPending,
            (unsigned long long)m.WorstScanAdded,(unsigned long long)m.WorstScanRemoved);
    }
    struct MeasurementReporter
    {
        Clock::time_point Started;uint64_t ScanSerial;
        explicit MeasurementReporter(Clock::time_point started):Started(started),ScanSerial(membershipScanSerial){}
        ~MeasurementReporter(){MaybeReportMeasurements(Started,ScanSerial);}
    };
    struct GridVisitContext {std::unordered_set<uint64_t>* Keys=nullptr;uint64_t Visitors=0;};
    bool Near(void* context,const MiniMapNative::GridEntityHandle& handle)
    {
        auto& visit=*static_cast<GridVisitContext*>(context);++visit.Visitors;
        const auto key=Key(handle.Entity);
        if(pending.contains(key))visit.Keys->insert(key);
        return true;
    }
    void OnTick(float)
    {
        const auto callbackBegin=Clock::now();
        MiniMapCallbackTiming::Scope timing(MiniMapCallbackTiming::Id::CollectorTick,true);
        if(!MiniMapUI::IsVisible())return;
        if(!ready || !MiniMapMap::GameplayReady())return;
        const auto settings=MiniMapConfig::GetRenderSettings().Buildings;
        if(!settings.ShowLayer)return;
        // Provenance belongs to immutable output, not to mutable preferences.
        // A policy change must republish even when membership is unchanged.
        {
            std::scoped_lock lock(publicationMutex);
            if(snapshot && snapshot->Generation==generation && snapshot->IncludedDroneRails!=settings.RenderDroneRails)dirty=true;
        }
        const auto* n=MiniMapNative::Get();if(!n || !n->player.isGameThread() || !Ensure(*n))return;
        if(measurementGeneration!=generation){measurements.Reset();measurementGeneration=generation;measurementIntervalStart=Clock::now();}
        MeasurementReporter reportAtExit(callbackBegin);
        try
        {
            if(Clock::now()>=nextMembership)
            {
                ++measurements.MembershipPolls;uint64_t changes=0;
                const bool complete=Reconcile(*n,changes);
                measurements.MembershipChanges+=changes;measurements.DirtyMembership+=changes;
                if(complete!=membershipComplete){membershipComplete=complete;dirty=true;++measurements.DirtyMembership;}
                nextMembership=Clock::now()+std::chrono::seconds(1);
            }
            {const auto begin=Clock::now();UpdateAtlasBounds();measurements.Maintenance.Add(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());}
            const auto gridBegin=Clock::now();
            MiniMapMap::PresentationSample sample{};double radius;
            {std::scoped_lock lock(publicationMutex);radius=viewRadius;}
            const bool hasPose=radius>0 && MiniMapMap::TryGetPresentationSample(sample);
            const auto& pose=sample.Tracking;
            if(hasPose)
            {
                const Bounds requested{pose.WorldX-radius,pose.WorldY-radius,pose.WorldX+radius,pose.WorldY+radius,true};
                if(!region.Valid || requested.MinX<region.MinX || requested.MinY<region.MinY || requested.MaxX>region.MaxX || requested.MaxY>region.MaxY)
                {region=G::Inflate(requested,radius*0.5);dirty=true;++measurements.DirtyRegion;}
            }
            if(pending.empty()){++measurements.PendingZeroCallbacks;remoteTurn=false;}
            else
            {
                std::unordered_set<uint64_t> localCandidates;
                GridVisitContext gridVisit{&localCandidates,0};bool gridEnumerated=false;
                if(hasPose && n->gridDiagnostic.forEachCellInRadius)
                    if(auto* grid=n->gridDiagnostic.getSubsystem(world))
                    {
                        gridEnumerated=true;
                        const MiniMapNative::GridDiagnosticApi::VisitorRef visitor{&Near,&gridVisit};
                        const SDK::FVector center{pose.WorldX,pose.WorldY,pose.WorldZ};
                        n->gridDiagnostic.forEachCellInRadius(grid,center,std::nextafter(float(radius),INFINITY),visitor,false);
                    }
                if(gridEnumerated)measurements.LocalGrid.Add(std::chrono::duration<double,std::milli>(Clock::now()-gridBegin).count());
                measurements.GridVisitors+=gridVisit.Visitors;measurements.UniqueLocalKeys+=localCandidates.size();
                std::unordered_set<const Definition*> attemptedDefinitions;
                const auto localEpoch=generation;
                if(!localCandidates.empty())
                {
                    const auto begin=Clock::now();
                    for(auto key:localCandidates){if(!pending.contains(key))continue;++measurements.LocalAttempts;if(Acquire(*n,key,attemptedDefinitions,settings))++measurements.NewlyCompleted;if(localEpoch!=generation)return;}
                    measurements.Acquisition.Add(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
                }
                // Publish local data before remote definition preparation. Remote
                // work is serviced on the next callback after any local progress;
                // there is no entity quota or permanent spatial admission limit.
                if(dirty && !remoteTurn)
                {
                    ++measurements.RemotePassesSkipped;const auto begin=Clock::now();Publish(settings);measurements.Maintenance.Add(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
                    remoteTurn=true;return;
                }
                remoteTurn=false;
                if(!pending.empty())
                {
                    // Acquire may erase pending keys or reenter session lifecycle
                    // code. Keep only a P-sized key snapshot across native calls.
                    const auto pendingBegin=Clock::now();
                    const std::vector<uint64_t> pendingKeys(pending.begin(),pending.end());
                    measurements.PendingEnumeration.Add(std::chrono::duration<double,std::milli>(Clock::now()-pendingBegin).count());
                    const auto epoch=generation;
                    const auto begin=Clock::now();
                    for(auto key:pendingKeys){if(!pending.contains(key))continue;++measurements.RemoteAttempts;if(Acquire(*n,key,attemptedDefinitions,settings))++measurements.NewlyCompleted;if(epoch!=generation)return;}
                    measurements.Acquisition.Add(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
                }
            }
            {const auto begin=Clock::now();Publish(settings);measurements.Maintenance.Add(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());}
        }
        catch(const std::bad_alloc&){/* Keep membership/owned records; retry pending work. */}
    }
}
namespace MiniMapBuildingCollector
{
    bool Initialize(IPluginSelf* self)
    {
        if(initialized)return true;
        if(!self || !self->hooks || !self->hooks->Engine)return false;
        Reset();InitializeMetadata();self->hooks->Engine->RegisterOnTick(&OnTick);initialized=true;return true;
    }
    void Reset(){ready=false;ClearSession();std::scoped_lock lock(publicationMutex);viewRadius=0;}
    void OnExperienceLoadComplete()
    {
        if(initialized)
        {
            ready=InitializeMetadata();
            if(!ready)LOG_ERROR("MiniMap: building acquisition initialization metadata unavailable");
        }
    }
    void SetViewport(const MiniMapMap::Transform& t)
    {
        const double radius=t.Valid && t.PixelsPerWorldUnit>0?std::hypot(t.Width,t.Height)*0.5/t.PixelsPerWorldUnit:0;
        std::scoped_lock lock(publicationMutex);viewRadius=std::isfinite(radius)?radius:0;
    }
    std::shared_ptr<const Snapshot> GetSnapshot(){std::scoped_lock lock(publicationMutex);return snapshot;}
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
    DiagnosticVisitStatus VisitDiagnosticDefinition(const Record& r,bool(*visitor)(const SDK::UObject*,void*),void* context)
    {
        const auto* n=MiniMapNative::Get();if(!n || !ready || !manager || !visitor || !n->player.isGameThread())return DiagnosticVisitStatus::CollectorUnavailable;
        if(r.Generation!=generation || world!=MiniMapMap::GetWorld())return DiagnosticVisitStatus::WorldGenerationChanged;
        const Id id{r.Index,r.Serial};
        if(!n->mass.isEntityValid(manager,id) || !n->mass.isEntityBuilt(manager,id))return DiagnosticVisitStatus::StaleEntity;
        const auto* p=Parameters(*n,id);if(!p || !p->PlacementData)return DiagnosticVisitStatus::ParametersUnavailable;
        return visitor(p->PlacementData,context)?DiagnosticVisitStatus::Visited:DiagnosticVisitStatus::VisitorRejected;
    }
#endif
    void Shutdown()
    {
        if(!initialized)return;GetSelf()->hooks->Engine->UnregisterOnTick(&OnTick);Reset();
        MiniMapBuildingBlueprintIdentity::Shutdown();
        if(const auto* n=MiniMapNative::Get())for(auto* root:metadata)n->asset.releaseObject(root);
        metadata.clear();types={};initialized=false;
    }
}
#endif
