#include "CallbackTiming.h"
#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
#include "BuildingRepresentation.h"
#include "Native/DefinitionDiagnosticCore.h"
#include <set>
#include <map>
#include "BlueprintIdentityCacheCore.h"
#include "Native/NativeApi.h"
#include "RepresentationMetadataCore.h"
#include "RepresentationSelectionCore.h"
#include "AssemblyRecipeCore.h"
#include "StaticAssemblyCaptureCore.h"
#include "SpriteCaptureCore.h"
#include "SpriteCandidateIdentity.h"
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
#include "SpriteValidationCore.h"
#endif
#include "BuildingSpriteCapture.h"
#include "Map/Buildings/BuildingCollector.h"
#include "Map/Map.h"
#include "BuildingSprites.h"
#include "plugin_helpers.h"
#include <SDK/Chimera_classes.hpp>
#include <SDK/Engine_parameters.hpp>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <unordered_map>
#include <type_traits>
#include <mutex>
#include <chrono>

namespace MiniMapBuildingRepresentation
{
    namespace C=MiniMapRepresentationCore;
    namespace S=MiniMapRepresentationSelection;
    namespace A=MiniMapAssemblyRecipe;
    namespace
    {
        // Only native interfaces/addresses and lifecycle comparison tokens persist.
        // All UObject identities, strong roots and traversal containers are local.
        IPluginEngineEvents* engine=nullptr;
        IPluginWorldEvents* worlds=nullptr;
        SDK::UWorld* endingWorld=nullptr; // comparison only; never dereferenced
        std::atomic<bool> stopped{false};
        std::atomic_flag executing=ATOMIC_FLAG_INIT;
        bool registered=false;
        constexpr auto NativeExcluded="sparse-set traversal bound not yet independently verified";

        bool Readable(uintptr_t p,size_t bytes,size_t alignment=1)
        {
            return MiniMapNative::IsMemoryRangeReadable(p,bytes,alignment);
        }
        template<class T> bool Read(uintptr_t p,size_t offset,T& out)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if(p>UINTPTR_MAX-offset)return false;
            return MiniMapNative::ReadMemory(p+offset,&out,sizeof(T));
        }
        template<class T> T Value(uintptr_t p,size_t offset)
        {T out{};Read(p,offset,out);return out;} // Use only after validating the containing extent.

        // Targeted indexed identity check, not a global object scan or weak-pointer
        // resolution. A candidate always comes from a retained ownership chain.
        bool Identity(uintptr_t p,size_t extent=sizeof(SDK::UObject))
        {
            const auto* nativeApi=MiniMapNative::Get();
            const uintptr_t objectArray=nativeApi?reinterpret_cast<uintptr_t>(nativeApi->asset.objectArray):0;
            return MiniMapNative::ValidateObjectIdentity(p,objectArray,extent);
        }
        // Strongly retained native CLASS anchors only. No candidate/asset cursor survives.
        struct NativeAnchor {const wchar_t* Path;uintptr_t Object{};};
        NativeAnchor nativeAnchors[]={
            {L"/Script/CoreUObject.Class",0},
            {L"/Script/Engine.Actor",0},
            {L"/Script/Engine.ActorComponent",0},
            {L"/Script/Engine.SceneComponent",0},
            {L"/Script/Engine.MeshComponent",0},
            {L"/Script/Engine.StaticMeshComponent",0},
            {L"/Script/Engine.SkeletalMeshComponent",0},
            {L"/Script/Engine.SkinnedMeshComponent",0},
            {L"/Script/Engine.StaticMesh",0},
            {L"/Script/Engine.SkeletalMesh",0},
            {L"/Script/Engine.SkinnedAsset",0},
            {L"/Script/Engine.MaterialInterface",0},
            {L"/Script/Engine.BlueprintGeneratedClass",0},
            {L"/Script/Engine.SimpleConstructionScript",0},
            {L"/Script/Engine.SCS_Node",0},
            {L"/Script/Engine.InheritableComponentHandler",0},
            {L"/Script/Engine.ChildActorComponent",0},
            {L"/Script/AuActorPlacement.AuActorPlacementData",0},
            {L"/Script/MassSpawner.MassEntityConfigAsset",0},
            {L"/Script/MassSpawner.MassEntityTraitBase",0},
            {L"/Script/MassRepresentation.MassVisualizationTrait",0},
            {L"/Script/Chimera.CrMassRepVisCosmeticsTrait",0},
            {L"/Script/Chimera.CrBuildingActorBase",0}
        };
        size_t nativeAnchorCursor=0;
        // Tokens address definition metadata within a generation, never sprite identity.
        struct BlueprintIdentities : MiniMapBlueprintIdentityCache::Store<BlueprintIdentityResult>
        {
            size_t Reported{},AcquisitionReports{},InventoryReports{},ProfileReports{},NativeBodyReports{};
            bool LimitReported=false,ContractReported=false;
            void ResetGeneration(uint64_t generation=0)noexcept
            {Reset(generation);Reported=AcquisitionReports=InventoryReports=ProfileReports=NativeBodyReports=0;LimitReported=ContractReported=false;}
        } blueprintIdentities;
        std::mutex blueprintIdentityAccess;
        void ClearBlueprintIdentities()
        {std::scoped_lock lock(blueprintIdentityAccess);blueprintIdentities.ResetGeneration();}
        struct AuditMetrics
        {
            uint64_t ClassCalls{},ClassNs{},ClassHits{},InheritanceCalls{},InheritanceNs{},InheritanceHits{},IdentityHits{},FieldHits{},HoldCalls{},HoldNs{},Retained{},Released{},PrimaryRoots{},RecipeNs{},InspectionNs{};
        };
        struct AuditElapsed
        {
            uint64_t* Target;
            std::chrono::steady_clock::time_point Start{};
            explicit AuditElapsed(uint64_t* target):Target(target){if(Target)Start=std::chrono::steady_clock::now();}
            ~AuditElapsed(){if(Target)*Target+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-Start).count());}
        };
        struct Context
        {
            AuditMetrics* Metrics=nullptr;
            // Validated metadata reuse only within this synchronous, game-thread
            // Context. Every reused object is strongly retained. No ownership,
            // range or failed-validation result survives an audit/GC boundary.
            std::map<uintptr_t,std::pair<uintptr_t,uintptr_t>> Classes;
            std::map<std::pair<uintptr_t,uintptr_t>,bool> Inheritance;
            std::map<uintptr_t,size_t> ValidatedExtents;
            bool OwnedRange(uintptr_t p,size_t bytes) const
            {
                if(!p||p>UINTPTR_MAX-bytes)return false;
                auto it=ValidatedExtents.upper_bound(p);
                if(it==ValidatedExtents.begin())return false;
                --it;
                return Held.contains(it->first)&&p-it->first<=it->second&&bytes<=it->second-(p-it->first);
            }
            bool Readable(uintptr_t p,size_t bytes,size_t alignment=1)
            {
                if(!alignment||(alignment&(alignment-1))||(p&(alignment-1)))return false;
                return OwnedRange(p,bytes)||MiniMapNative::IsMemoryRangeReadable(p,bytes,alignment);
            }
            template<class T> bool Read(uintptr_t p,size_t offset,T& out)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                if(p>UINTPTR_MAX-offset)return false;
                const auto address=p+offset;
                if(!OwnedRange(address,sizeof(T)))return MiniMapNative::ReadMemory(address,&out,sizeof(T));
                std::memcpy(&out,reinterpret_cast<const void*>(address),sizeof(T));
                if(Metrics)++Metrics->FieldHits;
                return true;
            }
            template<class T> T Value(uintptr_t p,size_t offset)
            {T out{};Read(p,offset,out);return out;}
            struct StaticObservation {uintptr_t Component{},Owner{};std::string Source;};
            std::vector<StaticObservation> StaticObservations;
            bool StaticObservationOverflow=false;
#if defined(MINIMAP_STAGE_R26B4_CLASSIFICATION)
            MiniMapSpriteClassification::Observation Classification;
#include "SpriteClassificationAudit.inl"
#endif
            bool GenericValidation=false,GenericInherited=false,GenericChild=false,CaptureRequested=false;
            bool DeferCapture=false; // A.4.9h TEMPORARY 60-SECOND CAPTURE DELAY
            uintptr_t AuditClass=0;
            bool AuditActive=false,AuditTruncated=false,AuditCollectionTruncated=false;
            size_t AuditVisits=0;
            std::vector<uintptr_t> AuditNodes;
            std::map<std::string,std::string> AuditParents;
            std::set<std::string> AuditConflictingParents;
            std::map<uintptr_t,uintptr_t> AuditAssignments;
            std::map<std::string,uintptr_t> AuditMaterialPointers;
            C::Graph Graph;
            S::WorkBudget Work;
            bool FullTraversal=false;
            struct ProbeState
            {
                S::Route Route=S::Route::None;
                uintptr_t High=0,Low=0;
                size_t Traits=0,VisualTraits=0,MeshReferences=0;
                bool Mesh=false,Stale=false,Truncated=false,Incomplete=false;
                std::string MassIdentity="not_examined",DefinitionIdentity="not_examined",CachedConfig="not_examined",
                    ConfigValidation="not_examined",HighStatus="not_examined",LowStatus="not_examined",MassStatus="not_examined";
                std::vector<std::string> Reasons;
                void Note(const char* reason)
                {if(Reasons.size()<16&&std::find(Reasons.begin(),Reasons.end(),reason)==Reasons.end())Reasons.emplace_back(reason);}
            };
            ProbeState ProbeResult;
            C::JsonArray Sources,Candidates,Mass,Relationships,Components;
            std::unordered_map<uintptr_t,std::string> Ids;
            std::unordered_set<uintptr_t> Held,ComponentSet;
            std::vector<uintptr_t> Roots;
            uintptr_t Class=0,Actor=0,Component=0,Scene=0,MeshComponent=0,StaticComponent=0,
                SkeletalComponent=0,SkinnedComponent=0,StaticMesh=0,SkeletalMesh=0,SkinnedAsset=0,
                Material=0,Blueprint=0,SCS=0,Node=0,Handler=0,Child=0,Placement=0,Config=0,Trait=0,Visual=0,Cosmetic=0;
            bool Truncated=false,Invalid=false,DiagnosticTruncated=false;
            size_t NodeCount=0;
            std::string Selected="null",SelectedTier="none",Status="unavailable",Reason="no safe resident representation";
            MiniMapBuildingCollector::Record Record{};
            BlueprintIdentityResult BlueprintIdentity;
            bool AuthoredFallback=false,AuthoredLookupAttempted=false;
            uintptr_t AuthoredClass=0,InstancedComponent=0;
            size_t TemplateStatic=0,TemplateSkeletal=0,TemplateInstanced=0,TemplateValidStatic=0;
            std::string AuthoredClassStatus="not_examined_configuration_route";
            uintptr_t ResolveResidentAuthoredClass(uintptr_t definition)
            {
                MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::ResidentAuthoredClass);
                AuthoredFallback=true;
                if(!BlueprintIdentity.Path)BlueprintIdentity=ResolveAuthoredIdentity(definition);
                if(!BlueprintIdentity.Path)
                {AuthoredClassStatus="canonical_identity_unavailable";return 0;}
                // StaticFindObject searches existing objects only. Never load the soft reference.
                // Keep the exact engine-produced path, including its generated-class suffix.
                const auto& path=*BlueprintIdentity.Path;
                if(path.empty()||path.size()>4096)
                {AuthoredClassStatus="canonical_path_length_invalid";return 0;}
                const auto length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.data(),int(path.size()),nullptr,0);
                if(length<=0||length>1024)
                {AuthoredClassStatus="canonical_path_encoding_invalid";return 0;}
                std::wstring wide(size_t(length),L'\0');
                if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.data(),int(path.size()),wide.data(),length)!=length)
                {AuthoredClassStatus="canonical_path_encoding_invalid";return 0;}
                AuthoredLookupAttempted=true;
                const auto type=Find(wide.c_str());
                if(!type)
                {AuthoredClassStatus=Work.Exhausted?"work_budget_exhausted":"not_resident_or_identity_invalid";return 0;}
                if(!ClassObject(type)||!Is(type,Blueprint)||!Derives(type,Actor)||!Hold(type))
                {AuthoredClassStatus=Work.Exhausted?"work_budget_exhausted":"resident_class_validation_failed";return 0;}
                if(AuthoredClass&&AuthoredClass!=type)
                {AuthoredClassStatus="resident_class_changed_during_inspection";return 0;}
                AuthoredClass=type;AuthoredClassStatus="resident_validated_blueprint_actor_class";
                return type;
            }
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
            void ReportAuthoredAcquisition(const MiniMapSpriteValidation::Result& result)
            {
                if(!AuthoredFallback)return;
                std::scoped_lock lock(blueprintIdentityAccess);
                if(blueprintIdentities.Generation!=Record.Generation||blueprintIdentities.AcquisitionReports>=64)return;
                if(BlueprintIdentity.Path)if(auto* path=blueprintIdentities.FindPath(*BlueprintIdentity.Path))
                {if(path->AcquisitionReported)return;path->AcquisitionReported=true;}
                ++blueprintIdentities.AcquisitionReports;
                LOG_INFO("MiniMap: resident authored-class acquisition %zu/64: generation=%llu building=%u path=%s cachedConfig=null lookupAttempted=%d lookup=%s classValid=%d templates=%zu scsNodes=%zu static=%zu skeletal=%zu instanced=%zu instancedClassAvailable=%d validStaticMeshAssets=%zu auditedStatic=%zu horizontalSurface=unverified_template_geometry_requires_runtime_inspection captureRequested=%d recipe=%s reason=%s",
                    blueprintIdentities.AcquisitionReports,(unsigned long long)Record.Generation,Record.BuildingId,
                    BlueprintIdentity.Path?BlueprintIdentity.Path->c_str():"unresolved",int(AuthoredLookupAttempted),AuthoredClassStatus.c_str(),
                    int(AuthoredClass!=0),ComponentSet.size(),NodeCount,TemplateStatic,TemplateSkeletal,TemplateInstanced,
                    int(InstancedComponent!=0),TemplateValidStatic,result.Static,int(CaptureRequested),
                    MiniMapSpriteValidation::Label(result.Status),result.Reason.c_str());
            }
#endif
            Context()
            {
                Components.Limit=512*1024;Mass.Limit=128*1024;Candidates.Limit=32*1024;
                Sources.Limit=128*1024;Relationships.Limit=64*1024;
            }
            bool CheckIdentity(uintptr_t p,size_t extent=sizeof(SDK::UObject))
            {
                if(AuditActive&&(!AuditSpend()))return false;
                if(!Work.Spend(FullTraversal))return false;
                // Strong ownership prevents index reuse/deallocation during this
                // synchronous game-thread scope. New pointers and larger extents
                // still receive the full indexed identity/range validation.
                if(const auto found=ValidatedExtents.find(p);found!=ValidatedExtents.end()&&
                    found->second>=extent&&Held.contains(p))
                {
                    const auto flags=uint32_t(reinterpret_cast<const SDK::UObject*>(p)->Flags);
                    if(flags&0x1b400)return false;
                    if(Metrics)++Metrics->IdentityHits;
                    return true;
                }
                if(!Identity(p,extent))return false;
                if(ValidatedExtents.size()<C::VisitLimit||ValidatedExtents.contains(p))
                    ValidatedExtents[p]=(std::max)(ValidatedExtents[p],extent);
                return true;
            }
            bool AuditSpend()
            {if(AuditVisits>=A::VisitLimit){AuditTruncated=true;return false;}++AuditVisits;return true;}
            ~Context()
            {
                const auto address=MiniMapNative::Get()->asset.releaseObject;
                for(auto it=Roots.rbegin();it!=Roots.rend();++it)
                {
                    reinterpret_cast<void(*)(SDK::UObject*)>(address)(reinterpret_cast<SDK::UObject*>(*it));
                    if(Metrics)++Metrics->Released;
                }
            }
            bool Hold(uintptr_t p,size_t extent=sizeof(SDK::UObject))
            {
                AuditElapsed elapsed(Metrics?&Metrics->HoldNs:nullptr);if(Metrics)++Metrics->HoldCalls;
                if(!CheckIdentity(p,extent))return false;
                if(Held.insert(p).second)
                {
                    Roots.push_back(p);
                    MiniMapNative::Get()->asset.retainObject(reinterpret_cast<SDK::UObject*>(p));
                    if(Metrics)++Metrics->Retained;
                }
                return true;
            }
            uintptr_t Find(const wchar_t* path)
            {
                for(const auto& anchor:nativeAnchors)
                    if(std::wcscmp(path,anchor.Path)==0)return anchor.Object&&Hold(anchor.Object)?anchor.Object:0;
                using Fn=SDK::UObject*(*)(SDK::UClass*,SDK::UObject*,const wchar_t*,bool);
                const auto p=reinterpret_cast<uintptr_t>(reinterpret_cast<Fn>(engine->GetStaticFindObjectByNameAddress())(nullptr,nullptr,path,false));
                return Hold(p)?p:0;
            }
            bool ClassObject(uintptr_t p)
            {
                AuditElapsed elapsed(Metrics?&Metrics->ClassNs:nullptr);if(Metrics)++Metrics->ClassCalls;
                if(!CheckIdentity(p,sizeof(SDK::UClass)))return false;
                // The containing SDK class extent was just validated; read the
                // declared fields without querying those same bytes again.
                const auto* klass=reinterpret_cast<const SDK::UClass*>(p);
                uintptr_t meta=reinterpret_cast<uintptr_t>(klass->Class),parent=reinterpret_cast<uintptr_t>(klass->SuperStruct);
                if(const auto found=Classes.find(p);found!=Classes.end()&&found->second==std::pair{meta,parent})
                {if(Metrics)++Metrics->ClassHits;return true;}
                // A changed hierarchy invalidates all derived answers, even in
                // this short-lived scope. Never accept a changed class from cache.
                if(Classes.contains(p)){Classes.erase(p);Inheritance.clear();}
                const auto originalMeta=meta;
                for(size_t n=0;meta&&n<C::DepthLimit;++n)
                {
                    if(!CheckIdentity(meta,sizeof(SDK::UClass)))return false;
                    if(meta==Class)
                    {
                        if(!Hold(p))return false;
                        if(Classes.size()<C::VisitLimit)Classes.emplace(p,std::pair{originalMeta,parent});
                        return true;
                    }
                    if(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(meta)->Class)!=Class)return false;
                    meta=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UStruct*>(meta)->SuperStruct);
                }
                return false;
            }
            bool Derives(uintptr_t type,uintptr_t expected)
            {
                AuditElapsed elapsed(Metrics?&Metrics->InheritanceNs:nullptr);if(Metrics)++Metrics->InheritanceCalls;
                if(!expected)return false;
                const auto key=std::pair{type,expected};
                if(!ClassObject(type))return false;
                if(const auto found=Inheritance.find(key);found!=Inheritance.end())
                {if(Metrics)++Metrics->InheritanceHits;return found->second;}
                std::unordered_set<uintptr_t> chain;
                for(size_t n=0;type&&n<C::DepthLimit;++n)
                {
                    if(!ClassObject(type)||!chain.insert(type).second)return false;
                    if(type==expected)
                    {if(Inheritance.size()<C::VisitLimit)Inheritance.emplace(key,true);return true;}
                    type=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UStruct*>(type)->SuperStruct);
                }
                if(type)Truncated=true;
                else if(Inheritance.size()<C::VisitLimit)Inheritance.emplace(key,false);
                return false;
            }
            bool Is(uintptr_t p,uintptr_t type,size_t extent=sizeof(SDK::UObject))
            {return CheckIdentity(p,extent)&&Derives(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(p)->Class),type);}
            bool BoundedNativeClassChain(uintptr_t type)
            {
                // Validate the SDK-declared base chain before the native getter indexes it.
                if(!ClassObject(type))return false;
                const auto chain=reinterpret_cast<const SDK::UStruct*>(type)->BaseChain;
                const auto last=chain.NumStructBasesInChainMinusOne;
                const auto data=reinterpret_cast<uintptr_t>(chain.StructBaseChainArray);
                if(last<0||size_t(last)>=C::DepthLimit||!Readable(data,(size_t(last)+1)*sizeof(SDK::FStructBaseChain*),alignof(SDK::FStructBaseChain*)))return false;
                uintptr_t previous=0;
                for(int32_t i=0;i<=last;++i)
                {
                    if(!Graph.Spend())return false;
                    const auto subobject=reinterpret_cast<uintptr_t>(chain.StructBaseChainArray[i]);
                    if(subobject<offsetof(SDK::UStruct,BaseChain))return false;
                    const auto owner=subobject-offsetof(SDK::UStruct,BaseChain);
                    if(!ClassObject(owner)||!Hold(owner)||reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UStruct*>(owner)->SuperStruct)!=previous)return false;
                    previous=owner;
                }
                return previous==type;
            }
            uintptr_t NativeClass(const wchar_t* path)
            {
                const auto p=Find(path);
                return p&&ClassObject(p)&&reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(p)->Class)==Class?p:0;
            }
            bool Initialize()
            {
#if defined(MINIMAP_BUILDING_SPRITES)
                MiniMapBuildingSprites::TimingScope timing(MiniMapBuildingSprites::Timing::NativeClassResolution);
#endif
                Class=Find(L"/Script/CoreUObject.Class");
                if(!Class||!CheckIdentity(Class,sizeof(SDK::UClass))||reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(Class)->Class)!=Class)return false;
#define NATIVE(field,path) field=NativeClass(L"/Script/" path);if(!field)Source("native_metadata_class",0,"unavailable_not_resident",#field)
                NATIVE(Actor,"Engine.Actor");NATIVE(Component,"Engine.ActorComponent");NATIVE(Scene,"Engine.SceneComponent");
                NATIVE(MeshComponent,"Engine.MeshComponent");NATIVE(StaticComponent,"Engine.StaticMeshComponent");
                NATIVE(SkeletalComponent,"Engine.SkeletalMeshComponent");NATIVE(SkinnedComponent,"Engine.SkinnedMeshComponent");
                NATIVE(StaticMesh,"Engine.StaticMesh");NATIVE(SkeletalMesh,"Engine.SkeletalMesh");NATIVE(SkinnedAsset,"Engine.SkinnedAsset");
                NATIVE(Material,"Engine.MaterialInterface");NATIVE(Blueprint,"Engine.BlueprintGeneratedClass");
                NATIVE(SCS,"Engine.SimpleConstructionScript");NATIVE(Node,"Engine.SCS_Node");
                NATIVE(Handler,"Engine.InheritableComponentHandler");NATIVE(Child,"Engine.ChildActorComponent");
                NATIVE(Placement,"AuActorPlacement.AuActorPlacementData");NATIVE(Config,"MassSpawner.MassEntityConfigAsset");
                NATIVE(Trait,"MassSpawner.MassEntityTraitBase");NATIVE(Visual,"MassRepresentation.MassVisualizationTrait");
                NATIVE(Cosmetic,"Chimera.CrMassRepVisCosmeticsTrait");
#undef NATIVE
                return Actor&&Component&&Scene&&Blueprint&&SCS&&Node&&Placement&&Config&&Trait&&(Visual||Cosmetic);
            }
            BlueprintIdentityResult ConvertAuthoredIdentity(uintptr_t definition) noexcept
            {
                auto report=[](const char* status,const std::string& =std::string{})
                {return BlueprintIdentityResult{{},status};};
                try
                {
                    auto* hooks=GetHooks();const auto* bindings=MiniMapNative::Get();
                    auto* memory=hooks?hooks->Memory:nullptr;auto* invocation=hooks?hooks->ObjectWalker:nullptr;
                    if(!bindings||!bindings->player.isGameThread||!bindings||!bindings->player.isGameThread()||
                        !memory||!memory->IsAllocatorAvailable||!memory->IsAllocatorAvailable()||!memory->Free||
                        !invocation||!invocation->InvokeResolvedUFunction)
                    {return report("invocation_or_engine_allocator_unavailable");}
                    // Independent work budget/roots: identity resolution cannot exhaust the capture audit.
                    Context diagnostic;diagnostic.Metrics=Metrics;diagnostic.Class=Class;diagnostic.Placement=Placement;
                    if(!diagnostic.Is(definition,Placement)||
                        !diagnostic.Hold(definition))
                    {return report("definition_validation_failed");}
                    using Params=SDK::Params::KismetSystemLibrary_Conv_SoftClassReferenceToString;
                    Params params{};
                    // Borrowed shallow copy. The retained definition owns its soft-reference storage.
                    params.SoftClassReference=reinterpret_cast<const SDK::UAuActorPlacementData*>(definition)->ActorClass;
                    const auto& path=params.SoftClassReference.ObjectID;
                    const auto& package=path.AssetPath.PackageName;const auto& asset=path.AssetPath.AssetName;
                    const auto* subpath=path.SubPathString.CStr();
                    const auto count=path.SubPathString.Num(),capacity=path.SubPathString.Max();
                    if(package.ComparisonIndex<0||asset.ComparisonIndex<0||count<0||capacity<count||capacity>1024||
                        (count&&(!Readable(reinterpret_cast<uintptr_t>(subpath),size_t(count)*sizeof(wchar_t),alignof(wchar_t))||subpath[count-1]!=0)))
                    {return report("malformed_authored_soft_reference");}
                    // A top-level blueprint identity must not name a subobject.
                    if(count>1){return report("unexpected_authored_subobject_reference");}
                    const auto functionClass=diagnostic.NativeClass(L"/Script/CoreUObject.Function");
                    const auto library=diagnostic.NativeClass(L"/Script/Engine.KismetSystemLibrary");
                    if(!functionClass||!library){return report("qualified_conversion_owner_unavailable");}
                    using FindFunction=SDK::UObject*(*)(SDK::UClass*,SDK::UObject*,const wchar_t*,bool);
                    const auto fn=reinterpret_cast<uintptr_t>(reinterpret_cast<FindFunction>(engine->GetStaticFindObjectByNameAddress())(
                        reinterpret_cast<SDK::UClass*>(functionClass),reinterpret_cast<SDK::UObject*>(library),L"Conv_SoftClassReferenceToString",true));
                    if(!diagnostic.Hold(fn)||!diagnostic.Is(fn,functionClass)||
                        reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(fn)->Class)!=functionClass||
                        reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(fn)->Outer)!=library)
                    {return report("qualified_conversion_function_unavailable");}
                    const auto* function=reinterpret_cast<const SDK::UFunction*>(fn);
                    // Same CL-127004 UFunction contract as the existing capture invocation.
                    const auto flags=uint32_t(function->FunctionFlags);
                    if((flags&0x2400)!=0x2400||!function->ExecFunction||
                        Value<uint8_t>(fn,0xb4)!=2||Value<uint16_t>(fn,0xb6)!=sizeof(Params))
                    {return report("conversion_function_contract_mismatch");}
                    const auto cdo=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UClass*>(library)->ClassDefaultObject);
                    if(!diagnostic.Is(cdo,library)||!diagnostic.Hold(cdo)||
                        reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(cdo)->Class)!=library||
                        !(uint32_t(reinterpret_cast<const SDK::UObject*>(cdo)->Flags)&0x10))
                    {return report("kismet_default_object_validation_failed");}
                    if(!blueprintIdentities.ContractReported)
                    {LOG_INFO("MiniMap: authored ActorClass conversion contract verified: Engine.KismetSystemLibrary:Conv_SoftClassReferenceToString native/static parameters=2 frame=0x38; game-thread public dispatch; engine allocator available");blueprintIdentities.ContractReported=true;}
                    struct ReturnedString
                    {
                        SDK::FString& Value;IPluginMemoryUtils* Memory;
                        ~ReturnedString(){if(Value.CStr())Memory->Free(Value.CStr());}
                    } returned{params.ReturnValue,memory};
                    if(!invocation->InvokeResolvedUFunction(reinterpret_cast<void*>(cdo),reinterpret_cast<void*>(fn),&params))
                    {return report("conversion_invocation_failed");}
                    const auto& value=params.ReturnValue;const auto* text=value.CStr();const auto length=value.Num();
                    if(length<0||length>1025||value.Max()<length||
                        (length&&(!Readable(reinterpret_cast<uintptr_t>(text),size_t(length)*sizeof(wchar_t),alignof(wchar_t))||text[length-1]!=0)))
                    {return report("malformed_conversion_result");}
                    if(length<=1){return report("empty_authored_reference");}
                    for(int32_t i=0;i<length-1;++i)if(text[i]<32||text[i]==127)
                    {return report("unexpected_control_character_in_path");}
                    const std::string owned=value.ToString(); // UTF-16 -> owned UTF-8, before engine Free.
                    if(owned.find("/Engine/Transient")!=std::string::npos||owned.find("UEDPIE_")!=std::string::npos||
                        owned.find("REINST_")!=std::string::npos||owned.find("SKEL_")!=std::string::npos)
                    {return report("transient_or_reinstanced_reference",owned);}
                    const auto dot=owned.rfind('.');
                    if(owned.front()!='/'||owned.find(':')!=std::string::npos||dot==std::string::npos||
                        dot<=owned.rfind('/')||dot+1==owned.size()||!owned.ends_with("_C"))
                    {return report("unexpected_non_generated_class_path",owned);}
                    return {std::make_shared<const std::string>(owned),"resolved"};
                }
                catch(...){return {{},"identity_resolution_exception"};}
            }
            BlueprintIdentityResult ResolveAuthoredIdentity(uintptr_t definition) noexcept
            {
                MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::ActorIdentity);
                BlueprintIdentities::Ticket ticket;bool hasTicket=false;
                try
                {
                    if(Record.HasSpline)return {{},"spline_identity_not_requested"};
                    const std::array<int64_t,3> token={Record.DefinitionObjectIndex,Record.DefinitionIndex,Record.DefinitionNumber};
                    {
                        std::scoped_lock lock(blueprintIdentityAccess);
                        if(blueprintIdentities.Generation!=Record.Generation)blueprintIdentities.ResetGeneration(Record.Generation);
                        ticket=blueprintIdentities.Ensure(Record.Generation,token);hasTicket=true;
                        if(const auto* entry=blueprintIdentities.Find(token);entry->Identity.Path)return entry->Identity;
                    }
                    auto converted=ConvertAuthoredIdentity(definition); // Original native conversion/lifetime contract.
                    auto prepared=BlueprintIdentities::Prepare(converted); // Allocations before commit lock.
                    BlueprintIdentityResult result;bool report=false,reportLimit=false;
                    {
                        std::scoped_lock lock(blueprintIdentityAccess);
                        result=blueprintIdentities.Commit(ticket,prepared);
                        if(!blueprintIdentities.Current(ticket))return result;
                        if(blueprintIdentities.Reported<64){++blueprintIdentities.Reported;report=true;}
                        else if(!blueprintIdentities.LimitReported){blueprintIdentities.LimitReported=true;reportLimit=true;}
                    }
                    // Formatting and unused converted/path-node destruction occur
                    // outside the cache mutex. Returned paths are independently owned.
                    if(report)LOG_INFO("MiniMap: authored ActorClass identity: generation=%llu building=%u definition=%d non_spline=true status=%s path=%s",
                        static_cast<unsigned long long>(Record.Generation),Record.BuildingId,Record.DefinitionObjectIndex,
                        result.Reason,C::Quote(result.Path?*result.Path:std::string{}).c_str());
                    if(reportLimit)LOG_INFO("MiniMap: authored ActorClass diagnostic limit reached: generation=%llu limit=64",static_cast<unsigned long long>(Record.Generation));
                    return result;
                }
                catch(const std::bad_alloc&)
                {
                    size_t failures;
                    {std::scoped_lock lock(blueprintIdentityAccess);blueprintIdentities.AllocationFailed(hasTicket?&ticket:nullptr);failures=blueprintIdentities.AllocationFailures;}
                    try{if(failures&&(failures&(failures-1))==0)LOG_WARN("MiniMap: blueprint identity allocation failures=%zu; resolution deferred without changing successful cached identities",failures);}
                    catch(...){/* Allocation failure reporting cannot escape this noexcept resolver. */}
                    return {{},"identity_metadata_allocation_failure"};
                }
                catch(...){return {{},"identity_resolution_exception"};}
            }
            std::string Id(uintptr_t p)
            {
                auto it=Ids.find(p);if(it!=Ids.end())return it->second;
                const auto id="o"+std::to_string(Ids.size()+1);Ids.emplace(p,id);return id;
            }
            std::string Ref(uintptr_t p,uintptr_t expected=0)
            {
                if(!p)return C::Object({{"status",C::Quote("absent")}});
                if((expected&&!Is(p,expected))||!Hold(p))
                {Invalid|=!Work.Exhausted;return C::Object({{"status",C::Quote(Work.Exhausted?"truncated":"invalid")}});}
                const auto type=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(p)->Class);
                if(!ClassObject(type)||!Hold(type))
                {Invalid|=!Work.Exhausted;return C::Object({{"status",C::Quote(Work.Exhausted?"truncated":"invalid_class")}});}
                const auto name=reinterpret_cast<const SDK::UObject*>(p)->Name;
                return C::Object({{"id",C::Quote(Id(p))},{"class_id",C::Quote(Id(type))},
                    {"status",C::Quote("present")},{"resident_validated","true"},{"readable_name", "null"},
                    {"name_status",C::Quote("not_attempted_native_conversion_not_verified")},
                    {"fname_comparison_index",std::to_string(name.ComparisonIndex)},{"fname_number",std::to_string(name.Number)}});
            }
            void Source(const std::string& category,uintptr_t owner,const char* status,const std::string& reason="",const std::string& details="null")
            {
                if(Work.Exhausted&&std::strcmp(status,"invalid")==0)
                {Source(category,owner,"truncated","native object-validation work budget exhausted",details);return;}
                Sources.Add(C::Object({{"source",C::Quote(category)},{"owner_id",owner?C::Quote(Id(owner)):"null"},
                    {"status",C::Quote(status)},{"reason",C::Quote(reason)},{"details",details}}));
                if(std::strcmp(status,"truncated")==0)Truncated=true;
                if(std::strcmp(status,"invalid")==0)Invalid=true;
            }
            template<class T,class Visitor> void Array(const SDK::TArray<T>& values,size_t limit,
                const std::string& source,uintptr_t owner,Visitor visitor)
            {
                const C::ArrayHeader a{reinterpret_cast<uintptr_t>(values.GetDataPtr()),values.Num(),values.Max()};
                const auto b=C::BoundArray(a,sizeof(T),alignof(T),limit);
                if(!b.Valid||!(!b.Bytes||Readable(a.Data,b.Bytes,alignof(T))))
                {Source(source,owner,"invalid","array bounds, alignment, arithmetic or readable storage check failed");return;}
                Source(source,owner,b.Status,"num="+std::to_string(a.Num)+" max="+std::to_string(a.Max)+" bounded_count="+std::to_string(b.Count));
                for(size_t i=0;i<b.Count;++i)
                {
                    if(!Graph.Spend()){Source(source,owner,"truncated","total traversal visit limit");break;}
                    visitor(a.Data+i*sizeof(T),i);
                }
            }
            std::string NameValue(const SDK::FName& n)
            {
                return C::Object({{"comparison_index",std::to_string(n.ComparisonIndex)},{"number",std::to_string(n.Number)},
                    {"readable_name","null"},{"status",C::Quote("opaque_fname")}});
            }
            std::string Transform(const SDK::FTransform& t)
            {
                return C::Object({{"rotation_xyzw",CopiedNumbers(std::array{t.Rotation.X,t.Rotation.Y,t.Rotation.Z,t.Rotation.W})},
                    {"translation",CopiedNumbers(std::array{t.Translation.X,t.Translation.Y,t.Translation.Z})},
                    {"scale",CopiedNumbers(std::array{t.Scale3D.X,t.Scale3D.Y,t.Scale3D.Z})},{"composed","false"}});
            }
            template<size_t N> std::string CopiedNumbers(const std::array<double,N>& values)
            {
                C::JsonArray a;
                for(double v:values){if(!std::isfinite(v))Invalid=true;a.Add(C::Number(v));}
                return a.String();
            }
            template<class T> std::string Materials(const SDK::TArray<T>& values,uintptr_t owner,const std::string& source)
            {
                C::JsonArray slots;slots.Limit=16*1024;
                if(!Material){Source(source,owner,"not_attempted","native MaterialInterface class unavailable");return slots.String();}
                Array(values,C::MaterialLimit,source,owner,[&](uintptr_t item,size_t slot)
                {
                    const auto& value=*reinterpret_cast<const T*>(item);
                    const auto material=reinterpret_cast<uintptr_t>([&]{if constexpr(std::is_pointer_v<T>)return value;else return value.MaterialInterface;}());
                    slots.Add(C::Object({{"slot",std::to_string(slot)},{"material",Ref(material,Material)}}));
                });
                if(slots.Truncated)DiagnosticTruncated=true;
                return slots.String();
            }
            void Edge(uintptr_t from,uintptr_t to,const std::string& kind,const char* status="present")
            {
                if(GenericValidation&&kind=="scs_child")
                {
                    const auto child=Id(to),parent=Id(from);
                    if(AuditParents.size()<A::NodeLimit||AuditParents.contains(child))
                    {auto [it,inserted]=AuditParents.emplace(child,parent);if(!inserted&&it->second!=parent)AuditConflictingParents.insert(child);}
                    else AuditCollectionTruncated=true;
                }
                Relationships.Add(C::Object({{"from",C::Quote(Id(from))},{"to",to?C::Quote(Id(to)):"null"},
                    {"kind",C::Quote(kind)},{"status",C::Quote(status)}}));
            }
            void InspectComponent(uintptr_t p,uintptr_t owner,const std::string& source,size_t childDepth)
            {
                if(!Is(p,Component)||!Hold(p)){Source(source,owner,"invalid","component identity/type");return;}
                Edge(owner,p,source);
                if(ComponentSet.contains(p))return;
                if(ComponentSet.size()>=C::ComponentLimit){Source(source,owner,"truncated","component record limit");return;}
                ComponentSet.insert(p);
                const auto type=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(p)->Class);
                if(AuthoredFallback)
                {
                    if(Is(p,StaticComponent))
                    {++TemplateStatic;if(Is(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UStaticMeshComponent*>(p)->StaticMesh),StaticMesh))++TemplateValidStatic;}
                    if(Is(p,SkeletalComponent))++TemplateSkeletal;
                    if(InstancedComponent&&Is(p,InstancedComponent))++TemplateInstanced;
                }
                std::string scene="null",mesh="null",defaults="[]",overrides="[]",meshKind="none",
                    meshInputs="null",meshStatus="not_applicable";
                if(Is(p,Scene))
                {
                    const auto parent=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::USceneComponent*>(p)->AttachParent);
                    const auto parentRef=Ref(parent,Scene);
                    if(parent&&Is(parent,Scene))Edge(p,parent,"scene_attachment_parent");
                    const auto* sceneComponent=reinterpret_cast<const SDK::USceneComponent*>(p);
                    scene=C::Object({{"location",CopiedNumbers(std::array{sceneComponent->RelativeLocation.X,sceneComponent->RelativeLocation.Y,sceneComponent->RelativeLocation.Z})},{"rotation_pitch_yaw_roll",CopiedNumbers(std::array{sceneComponent->RelativeRotation.Pitch,sceneComponent->RelativeRotation.Yaw,sceneComponent->RelativeRotation.Roll})},
                        {"scale",CopiedNumbers(std::array{sceneComponent->RelativeScale3D.X,sceneComponent->RelativeScale3D.Y,sceneComponent->RelativeScale3D.Z})},{"parent",parentRef},{"socket",NameValue(reinterpret_cast<const SDK::USceneComponent*>(p)->AttachSocketName)},
                        {"absolute_location",C::Bool(sceneComponent->bAbsoluteLocation)},{"absolute_rotation",C::Bool(sceneComponent->bAbsoluteRotation)},
                        {"absolute_scale",C::Bool(sceneComponent->bAbsoluteScale)},{"visible",C::Bool(sceneComponent->bVisible)},
                        {"hidden_in_game",C::Bool(sceneComponent->bHiddenInGame)}});
                }
                uintptr_t asset=0;
                if(Is(p,SkinnedComponent))
                {
                    meshInputs=C::Object({{"legacy_skeletal_mesh",Ref(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::USkinnedMeshComponent*>(p)->SkeletalMesh),SkeletalMesh)},
                        {"skinned_asset",Ref(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::USkinnedMeshComponent*>(p)->SkinnedAsset),SkinnedAsset)},
                        {"provenance",C::Quote("verified resident USkinnedMeshComponent assignment fields; not evaluated as additive geometry")}});
                }
                if(Is(p,StaticComponent))
                {
                    if(AuthoredFallback)try{StaticObservations.push_back({p,owner,source});}catch(...){StaticObservationOverflow=true;}
                    meshKind="static";asset=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UStaticMeshComponent*>(p)->StaticMesh);mesh=Ref(asset,StaticMesh);
                    meshStatus=!StaticMesh?"not_attempted":!asset?"absent":Is(asset,StaticMesh)?"present":"invalid";
                    Source("static_mesh_assignment",p,meshStatus.c_str());
                    if(Is(asset,StaticMesh))defaults=Materials(reinterpret_cast<const SDK::UStaticMesh*>(asset)->StaticMaterials,asset,"static_mesh_authored_materials");
                }
                else if(Is(p,SkeletalComponent))
                {
                    meshKind="skeletal";meshStatus="not_attempted";
                    // Validate BOTH references read by the installed fixed getter
                    // before calling it. Native SkeletalMesh class is already resident.
                    const auto legacy=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::USkinnedMeshComponent*>(p)->SkeletalMesh),skinned=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::USkinnedMeshComponent*>(p)->SkinnedAsset);
                    if(!SkeletalMesh||!SkinnedAsset)
                        Source("skeletal_mesh_assignment",p,"not_attempted","fixed getter/native asset classes unavailable");
                    else if((legacy&&!Is(legacy,SkeletalMesh))||(skinned&&!Is(skinned,SkinnedAsset)))
                        Source("skeletal_mesh_assignment",p,"invalid","getter input reference identity/type");
                    else if(!BoundedNativeClassChain(SkeletalMesh)||
                        (legacy&&!BoundedNativeClassChain(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(legacy)->Class)))||
                        (skinned&&!BoundedNativeClassChain(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(skinned)->Class))))
                        Source("skeletal_mesh_assignment",p,Graph.Truncated?"truncated":"invalid","native getter class-chain preflight failed");
                    else
                    {
                        if((legacy&&!Hold(legacy))||(skinned&&!Hold(skinned)))
                        {Source("skeletal_mesh_assignment",p,"invalid","getter inputs could not be retained");return;}
                        asset=reinterpret_cast<uintptr_t>(MiniMapNative::Get()->player.getSkeletalMeshAsset(reinterpret_cast<const SDK::USkeletalMeshComponent*>(p)));
                        mesh=Ref(asset,SkeletalMesh);
                        meshStatus=!asset?"absent_as_skeletal_mesh":Is(asset,SkeletalMesh)?"present":"invalid";
                        Source("skeletal_mesh_assignment",p,asset?meshStatus.c_str():"absent","fixed getter result; raw skinned assignment fields recorded separately");
                        if(Is(asset,SkeletalMesh))defaults=Materials(reinterpret_cast<const SDK::USkeletalMesh*>(asset)->Materials,asset,"skeletal_mesh_authored_materials");
                    }
                }
                else if(Is(p,SkinnedComponent))
                {meshKind="skinned";meshStatus="not_attempted";Source("effective_skinned_mesh_assignment",p,"not_attempted","raw assignment inputs recorded; non-skeletal effective getter not verified for this stage");}
                if(Is(p,MeshComponent))overrides=Materials(reinterpret_cast<const SDK::UMeshComponent*>(p)->OverrideMaterials,p,"component_authored_material_overrides");
                Components.Add(C::Object({{"component",Ref(p,Component)},{"source",C::Quote(source)},
                    {"owner",Ref(owner)},{"component_class",Ref(type,Class)},{"registered",C::Bool(Value<uint8_t>(p,0x94)&1)},
                    {"object_outer",Ref(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(p)->Outer))},
                    {"scene",scene},{"mesh_kind",C::Quote(meshKind)},{"mesh",mesh},
                    {"mesh_assignment_status",C::Quote(meshStatus)},{"mesh_assignment_inputs",meshInputs},
                    {"authored_mesh_materials",defaults},{"authored_component_overrides",overrides},
                    {"effective_materials_status",C::Quote("not_attempted_unbounded_array_getter_and_virtual_resolution_not_verified")},
                    {"live_instance_state","false"}}));
                // A component may first be encountered in ComponentTemplates;
                // keep its already validated assignment for the later SCS join.
                if(GenericValidation&&owner==AuditClass&&asset&&meshStatus=="present")
                {
                    if(AuditAssignments.size()<A::NodeLimit)AuditAssignments.emplace(p,asset);
                    else AuditCollectionTruncated=true;
                }
                if(Is(p,Child))
                {
                    GenericChild=true;
                    const auto childClass=reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(reinterpret_cast<const SDK::UChildActorComponent*>(p)->ChildActorClass)),childTemplate=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UChildActorComponent*>(p)->ChildActorTemplate);
                    const bool validClass=childClass&&ClassObject(childClass)&&Derives(childClass,Actor);
                    const bool validTemplate=childTemplate&&Is(childTemplate,Actor);
                    Source("child_actor_class",p,!childClass?"absent":validClass?"present":"invalid","",Ref(childClass,Class));
                    Source("child_actor_template",p,!childTemplate?"absent":validTemplate?"present":"invalid","",Ref(childTemplate,Actor));
                    if(childTemplate&&Is(childTemplate,Actor)&&childClass&&ClassObject(childClass)&&
                        Derives(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(childTemplate)->Class),childClass))
                    {Hold(childTemplate);Edge(p,childTemplate,"resident_child_actor_template");}
                    else if(childTemplate)Source("child_actor_template_relationship",p,"invalid","class/template mismatch");
                    if(childClass&&ClassObject(childClass)&&Derives(childClass,Actor))
                    {
                        Hold(childClass);Edge(p,childClass,"child_actor_representation_class");
                        if(childDepth>=C::ChildDepthLimit)Source("child_actor_class_templates",p,"truncated","child-template recursion depth");
                        else InspectClass(childClass,childDepth+1,"child_actor_class_templates");
                    }
                    Source("child_actor_native_components",p,"not_attempted",NativeExcluded);
                }
            }
            void InspectNode(uintptr_t p,uintptr_t owner,size_t depth,size_t childDepth)
            {
                if(!Is(p,Node)||!Hold(p)){Source("scs_node",owner,"invalid","node identity/type");return;}
                const auto state=Graph.Enter(p,depth);
                if(std::strcmp(state,"present")!=0){Source("scs_node",p,state);return;}
                if(NodeCount>=C::NodeLimit){Source("scs_node",p,"truncated","total unique SCS node limit");Graph.Leave(p);return;}
                ++NodeCount;
                if(owner!=AuditClass)GenericInherited=true;
                if(GenericValidation&&owner==AuditClass&&childDepth==0)
                {if(AuditNodes.size()<A::NodeLimit)AuditNodes.push_back(p);else AuditCollectionTruncated=true;}
                const auto declared=reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(reinterpret_cast<const SDK::USCS_Node*>(p)->ComponentClass)),component=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::USCS_Node*>(p)->ComponentTemplate);
                Source("scs_node",p,"present","",C::Object({{"node",Ref(p,Node)},{"declared_class",Ref(declared,Class)},
                    {"attach_to",NameValue(reinterpret_cast<const SDK::USCS_Node*>(p)->AttachToName)},{"parent_variable",NameValue(reinterpret_cast<const SDK::USCS_Node*>(p)->ParentComponentOrVariableName)},
                    {"parent_owner_class",NameValue(reinterpret_cast<const SDK::USCS_Node*>(p)->ParentComponentOwnerClassName)},{"parent_is_native",C::Bool(uint8_t(reinterpret_cast<const SDK::USCS_Node*>(p)->bIsParentComponentNative)!=0)},
                    {"variable_name",NameValue(reinterpret_cast<const SDK::USCS_Node*>(p)->InternalVariableName)},{"variable_guid_words",Guid(reinterpret_cast<const SDK::USCS_Node*>(p)->VariableGUID)}}));
                if(!component)Source("scs_component_template",p,"absent");
                else if(!ClassObject(declared)||!Derives(declared,Component)||!Is(component,declared))
                    Source("scs_component_template",p,"invalid","declared component class mismatch");
                else {Edge(p,component,"scs_template");InspectComponent(component,owner,"scs_component_template",childDepth);}
                Array(reinterpret_cast<const SDK::USCS_Node*>(p)->ChildNodes,C::NodeLimit,"scs_children",p,[&](uintptr_t item,size_t)
                {
                    const auto node=reinterpret_cast<uintptr_t>(*reinterpret_cast<SDK::UObject* const*>(item));
                    if(Is(node,Node)){Hold(node);Edge(p,node,"scs_child");}
                    InspectNode(node,owner,depth+1,childDepth);
                });
                Graph.Leave(p);
            }
            std::string Guid(const SDK::FGuid& guid)
            {
                C::JsonArray a;for(const auto word:{guid.A,guid.B,guid.C,guid.D})a.Add(std::to_string(word));return a.String();
            }
            void InspectClass(uintptr_t initial,size_t childDepth,const std::string& source)
            {
                if(!ClassObject(initial)||!Derives(initial,Actor)||!Hold(initial))
                {Source(source,initial,"invalid","actor representation class identity/type");return;}
                const auto entry=Graph.Enter(initial,childDepth,C::ChildDepthLimit);
                if(std::strcmp(entry,"present")!=0){Source(source,initial,entry);return;}
                std::unordered_set<uintptr_t> supers;
                auto type=initial;
                for(size_t depth=0;type&&depth<C::DepthLimit;++depth)
                {
                    if(!Graph.Spend()){Source("class_hierarchy",initial,"truncated","total visit limit");break;}
                    if(!ClassObject(type)||!Hold(type)){Source("class_hierarchy",initial,"invalid");break;}
                    if(!supers.insert(type).second){Source("class_hierarchy",type,"cycle");break;}
                    const auto cdo=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UClass*>(type)->ClassDefaultObject);
                    const bool validCdo=cdo&&Is(cdo,type)&&(uint32_t(reinterpret_cast<const SDK::UObject*>(cdo)->Flags)&0x10);
                    Source("existing_cdo",type,!cdo?"unavailable_not_resident":validCdo?"present":"invalid","",Ref(cdo,type));
                    Source("native_default_components",type,"not_attempted",NativeExcluded);
                    if(Is(type,Blueprint))
                    {
                        Array(reinterpret_cast<const SDK::UBlueprintGeneratedClass*>(type)->ComponentTemplates,C::ComponentLimit,"blueprint_component_templates",type,[&](uintptr_t item,size_t)
                            {InspectComponent(reinterpret_cast<uintptr_t>(*reinterpret_cast<SDK::UObject* const*>(item)),type,"blueprint_component_templates",childDepth);});
                        const auto scs=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UBlueprintGeneratedClass*>(type)->SimpleConstructionScript);
                        if(!scs)Source("blueprint_scs",type,"absent");
                        else if(!Is(scs,SCS)||!Hold(scs))Source("blueprint_scs",type,"invalid");
                        else
                        {
                            Source("blueprint_scs",type,"present","",Ref(scs,SCS));
                            Array(reinterpret_cast<const SDK::USimpleConstructionScript*>(scs)->RootNodes,C::NodeLimit,"scs_root_nodes",scs,[&](uintptr_t item,size_t)
                                {InspectNode(reinterpret_cast<uintptr_t>(*reinterpret_cast<SDK::UObject* const*>(item)),type,0,childDepth);});
                            Array(reinterpret_cast<const SDK::USimpleConstructionScript*>(scs)->AllNodes,C::NodeLimit,"scs_all_nodes",scs,[&](uintptr_t item,size_t)
                                {InspectNode(reinterpret_cast<uintptr_t>(*reinterpret_cast<SDK::UObject* const*>(item)),type,0,childDepth);});
                        }
                        const auto handler=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UBlueprintGeneratedClass*>(type)->InheritableComponentHandler);
                        if(!handler)Source("inherited_override_records",type,"absent");
                        else if(!Is(handler,Handler)||!Hold(handler))Source("inherited_override_records",type,"invalid");
                        else Array(reinterpret_cast<const SDK::UInheritableComponentHandler*>(handler)->Records,C::ComponentLimit,"inherited_override_records",handler,[&](uintptr_t record,size_t ordinal)
                        {
                            GenericInherited=true;
                            const auto* overrideRecord=reinterpret_cast<const SDK::FComponentOverrideRecord*>(record);
                            const auto declared=reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(overrideRecord->ComponentClass)),component=reinterpret_cast<uintptr_t>(overrideRecord->ComponentTemplate),
                                keyOwner=reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(overrideRecord->ComponentKey.OwnerClass));
                            Source("inherited_override_key",handler,"present","",C::Object({{"ordinal",std::to_string(ordinal)},
                                {"owner_class",Ref(keyOwner,Class)},{"variable_name",NameValue(reinterpret_cast<const SDK::FComponentOverrideRecord*>(record)->ComponentKey.SCSVariableName)},
                                {"declared_component_class",Ref(declared,Class)},{"component_template",Ref(component,Component)},
                                {"associated_guid_words",Guid(reinterpret_cast<const SDK::FComponentOverrideRecord*>(record)->ComponentKey.AssociatedGuid)},{"effective_selection",C::Quote("not_evaluated")}}));
                            if(!component)Source("inherited_override_template",handler,"absent");
                            else if(!ClassObject(declared)||!Derives(declared,Component)||!Is(component,declared))
                                Source("inherited_override_template",handler,"invalid","declared class mismatch");
                            else InspectComponent(component,type,"inherited_override_template",childDepth);
                        });
                        Source("native_inherited_template_resolver",type,"not_attempted","internal class/record traversal has no diagnostic limit; raw override records kept distinct");
                    }
                    type=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UStruct*>(type)->SuperStruct);
                    if(type&&depth+1==C::DepthLimit)Source("class_hierarchy",initial,"truncated","superclass depth limit");
                }
                Graph.Leave(initial);
            }
            bool InspectMass(uintptr_t trait,bool visual)
            {
                const auto& descriptor=visual?reinterpret_cast<const SDK::UMassVisualizationTrait*>(trait)->StaticMeshInstanceDesc:reinterpret_cast<const SDK::UCrMassRepVisCosmeticsTrait*>(trait)->StaticMeshInstanceDesc;
                C::JsonArray meshes,offsets;meshes.Limit=96*1024;
                bool validMesh=false;
                Array(descriptor.Meshes,64,"mass_mesh_descriptors",trait,[&](uintptr_t item,size_t ordinal)
                {
                    const auto mesh=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::FMassStaticMeshInstanceVisualizationMeshDesc*>(item)->Mesh);const bool valid=Is(mesh,StaticMesh);validMesh|=valid;
                    const auto ref=Ref(mesh,StaticMesh),local=Transform(reinterpret_cast<const SDK::FMassStaticMeshInstanceVisualizationMeshDesc*>(item)->LocalTransform),materials=Materials(reinterpret_cast<const SDK::FMassStaticMeshInstanceVisualizationMeshDesc*>(item)->MaterialOverrides,trait,"mass_mesh_material_overrides"),
                        min=C::Number(float(reinterpret_cast<const SDK::FMassStaticMeshInstanceVisualizationMeshDesc*>(item)->MinLODSignificance)),max=C::Number(float(reinterpret_cast<const SDK::FMassStaticMeshInstanceVisualizationMeshDesc*>(item)->MaxLODSignificance));
                    meshes.Add(C::Object({{"ordinal",std::to_string(ordinal)},{"mesh",ref},{"local_transform",local},
                        {"material_overrides",materials},{"min_lod_significance",min},{"max_lod_significance",max}}));
                });
                Array(descriptor.TransformOffsets,64,"mass_raw_transform_offsets",trait,[&](uintptr_t item,size_t ordinal)
                    {const auto transform=Transform(*reinterpret_cast<const SDK::FTransform*>(item));offsets.Add(C::Object({{"ordinal",std::to_string(ordinal)},{"transform",transform}}));});
                Mass.Add(C::Object({{"descriptor_id",C::Quote(Id(trait)+"_ism")},{"owner_trait",Ref(trait)},
                    {"tier",C::Quote("mass_ism_alternative")},{"mesh_references",meshes.String()},
                    {"use_transform_offset",C::Bool(uint8_t(descriptor.bUseTransformOffset)!=0)},
                    {"descriptor_transform_offset",Transform(descriptor.TransformOffset)},{"raw_transform_offsets",offsets.String()},
                    {"renderer_equivalent_transforms","false"},{"provenance",C::Quote("resident visualization trait; raw inputs only")}}));
                DiagnosticTruncated|=meshes.Truncated||offsets.Truncated;
                return validMesh;
            }
            template<class T,class Visitor> bool ProbeArray(const SDK::TArray<T>& values,size_t limit,Visitor visitor)
            {
                const C::ArrayHeader a{reinterpret_cast<uintptr_t>(values.GetDataPtr()),values.Num(),values.Max()};
                const auto b=C::BoundArray(a,sizeof(T),alignof(T),limit);
                if(!b.Valid||(b.Bytes&&!Readable(a.Data,b.Bytes,alignof(T))))
                {ProbeResult.Incomplete=true;ProbeResult.Note("invalid_array_storage");return false;}
                if(b.Truncated){ProbeResult.Truncated=true;ProbeResult.Note("array_visit_limit");}
                for(size_t i=0;i<b.Count;++i)
                {
                    if(!Graph.Spend()||Work.Exhausted){ProbeResult.Truncated=true;ProbeResult.Note("work_budget_exhausted");return false;}
                    visitor(a.Data+i*sizeof(T));
                }
                return !b.Truncated&&!Work.Exhausted;
            }
            bool ProbeDefinition(uintptr_t definition)
            {
                ProbeResult.MassIdentity="valid"; // Collector invokes visitor only after entity revalidation.
                if(!definition){ProbeResult.DefinitionIdentity="null";ProbeResult.Note("placement_null");return false;}
                if(!Is(definition,Placement)||!Hold(definition))
                {ProbeResult.DefinitionIdentity=Work.Exhausted?"not_examined_budget_exhausted":"invalid";
                    ProbeResult.Note(Work.Exhausted?"work_budget_exhausted":"invalid_placement");return false;}
                const auto name=reinterpret_cast<const SDK::UObject*>(definition)->Name;
                if(int32_t(reinterpret_cast<const SDK::UObject*>(definition)->Index)!=Record.DefinitionObjectIndex||name.ComparisonIndex!=Record.DefinitionIndex||
                    name.Number!=Record.DefinitionNumber||uint32_t(reinterpret_cast<const SDK::UAuActorPlacementData*>(definition)->BuildingID)!=Record.BuildingId)
                {ProbeResult.DefinitionIdentity="mismatch";ProbeResult.Note("definition_identity_mismatch");return false;}
                ProbeResult.DefinitionIdentity="valid";
                BlueprintIdentity=ResolveAuthoredIdentity(definition);
#if defined(MINIMAP_STAGE_R26B4_CLASSIFICATION)
                if(GenericValidation)ClassificationInputs(definition);
#if defined(MINIMAP_BUILDING_SPRITES)
                if(CaptureRequested&&Classification.CachedPolicy.ExcludeFromMiniMap==MiniMapSpriteClassification::Exclusion::Exclude)
                {ProbeResult.Note("classification_excluded");return false;}
#endif
#endif
                // Null transient cache never implies authored absence. Only this case
                // may inspect the independently authored, already-resident actor class.
                auto config=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UAuActorPlacementData*>(definition)->EntityType.EntityConfigPtr);
                ProbeResult.CachedConfig=config?"populated":"null";
                if(!config)
                {
                    const auto type=ResolveResidentAuthoredClass(definition);
                    if(!type){ProbeResult.Note(AuthoredClassStatus.c_str());return false;}
                    ProbeResult.Route=S::Route::PlacementActor;
                    return !Work.Exhausted&&!Graph.Truncated;
                }
                std::unordered_set<uintptr_t> configs,traits;
                for(size_t depth=0;config&&depth<16;++depth)
                {
                    if(!Graph.Spend()||Work.Exhausted){ProbeResult.Truncated=true;ProbeResult.Note("work_budget_exhausted");break;}
                    if(!Is(config,Config)||!Hold(config))
                    {ProbeResult.ConfigValidation=Work.Exhausted?"not_examined_budget_exhausted":"invalid";ProbeResult.Incomplete=true;
                        ProbeResult.Note(Work.Exhausted?"work_budget_exhausted":"invalid_config");break;}
                    ProbeResult.ConfigValidation="valid_in_examined_chain";
                    if(!configs.insert(config).second){ProbeResult.Incomplete=true;ProbeResult.Note("configuration_cycle");break;}
                    ProbeArray(reinterpret_cast<const SDK::UMassEntityConfigAsset*>(config)->Config.Traits,256,[&](uintptr_t item)
                    {
                        ++ProbeResult.Traits;const auto trait=reinterpret_cast<uintptr_t>(*reinterpret_cast<SDK::UObject* const*>(item));
                        if(!Is(trait,Trait)||!Hold(trait)){ProbeResult.Incomplete=true;
                            ProbeResult.Note(Work.Exhausted?"work_budget_exhausted":"invalid_trait");return;}
                        if(!traits.insert(trait).second)return;
                        const bool visual=Is(trait,Visual);
                        if(!visual&&!Is(trait,Cosmetic))return;
                        ++ProbeResult.VisualTraits;
                        const uintptr_t actors[]={(visual?reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(reinterpret_cast<const SDK::UMassVisualizationTrait*>(trait)->HighResTemplateActor)):reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(reinterpret_cast<const SDK::UCrMassRepVisCosmeticsTrait*>(trait)->HighResTemplateActor))),(visual?reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(reinterpret_cast<const SDK::UMassVisualizationTrait*>(trait)->LowResTemplateActor)):reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(reinterpret_cast<const SDK::UCrMassRepVisCosmeticsTrait*>(trait)->LowResTemplateActor)))};
                        for(size_t tier=0;tier<2;++tier)
                        {
                            const auto type=actors[tier];
                            const bool valid=type&&ClassObject(type)&&Derives(type,Actor)&&Hold(type);
                            auto& state=tier==0?ProbeResult.HighStatus:ProbeResult.LowStatus;
                            auto& first=tier==0?ProbeResult.High:ProbeResult.Low;
                            if(valid){state="resident_validated";if(!first)first=type;}
                            else if(state!="resident_validated")state=Work.Exhausted?"not_examined_budget_exhausted":!type?"null":"invalid_or_unavailable";
                        }
                        // Eligibility reads assignments only: no transforms,
                        // materials, SCS, CDOs or component traversal in a probe.
                        const bool massComplete=ProbeArray((visual?reinterpret_cast<const SDK::UMassVisualizationTrait*>(trait)->StaticMeshInstanceDesc.Meshes:reinterpret_cast<const SDK::UCrMassRepVisCosmeticsTrait*>(trait)->StaticMeshInstanceDesc.Meshes),64,[&](uintptr_t meshItem)
                        {
                            ++ProbeResult.MeshReferences;const auto mesh=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::FMassStaticMeshInstanceVisualizationMeshDesc*>(meshItem)->Mesh);
                            if(Is(mesh,StaticMesh)&&Hold(mesh))ProbeResult.Mesh=true;
                        });
                        if(!massComplete)ProbeResult.MassStatus="unavailable_or_truncated";
                        else if(ProbeResult.MassStatus!="unavailable_or_truncated")ProbeResult.MassStatus="examined";
                    });
                    config=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UMassEntityConfigAsset*>(config)->Config.Parent);
                    if(config&&depth==15){ProbeResult.Truncated=true;ProbeResult.Note("configuration_depth_limit");}
                }
                if(!ProbeResult.VisualTraits&&!ProbeResult.Incomplete&&!ProbeResult.Truncated&&!Work.Exhausted)
                    ProbeResult.Note("no_supported_visualization_trait");
                else if(ProbeResult.VisualTraits&&!Work.Exhausted&&!Truncated)
                {
                    if(!ProbeResult.High&&!ProbeResult.Low)ProbeResult.Note("no_valid_actor_class");
                    if(!ProbeResult.Mesh&&ProbeResult.MassStatus=="examined")ProbeResult.Note("no_valid_mass_mesh");
                }
                ProbeResult.Route=S::EligibleRoute(false,ProbeResult.High||ProbeResult.Low,ProbeResult.Mesh);
                if(Work.Exhausted||Graph.Truncated){ProbeResult.Truncated=true;ProbeResult.Note("work_budget_exhausted");}
                return ProbeResult.Route!=S::Route::None;
            }
            bool Discover(uintptr_t definition)
            {
                if(!Is(definition,Placement)||!Hold(definition))return false;
                const auto name=reinterpret_cast<const SDK::UObject*>(definition)->Name;
                if(int32_t(reinterpret_cast<const SDK::UObject*>(definition)->Index)!=Record.DefinitionObjectIndex||name.ComparisonIndex!=Record.DefinitionIndex||
                    name.Number!=Record.DefinitionNumber||uint32_t(reinterpret_cast<const SDK::UAuActorPlacementData*>(definition)->BuildingID)!=Record.BuildingId)return false;
#if defined(MINIMAP_STAGE_R26B4_CLASSIFICATION)
                if(!GenericValidation)ClassificationInputs(definition);
#endif
                Source("mass_building_definition",definition,"present","",Ref(definition,Placement));
                uintptr_t high=0,low=0;bool hasMass=false;
                auto config=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UAuActorPlacementData*>(definition)->EntityType.EntityConfigPtr);
                std::unordered_set<uintptr_t> configs;
                for(size_t depth=0;config&&depth<16;++depth)
                {
                    if(!Graph.Spend()){Source("entity_configuration",definition,"truncated","total visit limit");break;}
                    if(!Is(config,Config)||!Hold(config)){Source("entity_configuration",definition,"invalid");break;}
                    if(!configs.insert(config).second){Source("entity_configuration",config,"cycle");break;}
                    Source("entity_configuration",config,"present","",Ref(config,Config));
                    Array(reinterpret_cast<const SDK::UMassEntityConfigAsset*>(config)->Config.Traits,256,"configuration_traits",config,[&](uintptr_t item,size_t)
                    {
                        const auto trait=reinterpret_cast<uintptr_t>(*reinterpret_cast<SDK::UObject* const*>(item));
                        if(!Is(trait,Trait)||!Hold(trait)){Source("configuration_trait",config,"invalid");return;}
                        Edge(config,trait,"configuration_trait");
                        const bool visual=Is(trait,Visual);
                        if(!visual&&!Is(trait,Cosmetic))return;
                        for(const auto& entry:{std::pair{(visual?reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(reinterpret_cast<const SDK::UMassVisualizationTrait*>(trait)->HighResTemplateActor)):reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(reinterpret_cast<const SDK::UCrMassRepVisCosmeticsTrait*>(trait)->HighResTemplateActor))),"high_detail_actor"},std::pair{(visual?reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(reinterpret_cast<const SDK::UMassVisualizationTrait*>(trait)->LowResTemplateActor)):reinterpret_cast<uintptr_t>(static_cast<SDK::UClass*>(reinterpret_cast<const SDK::UCrMassRepVisCosmeticsTrait*>(trait)->LowResTemplateActor))),"low_detail_actor"}})
                        {
                            const auto candidate=entry.first;
                            const bool valid=candidate&&ClassObject(candidate)&&Derives(candidate,Actor)&&Hold(candidate);
                            Candidates.Add(C::Object({{"tier",C::Quote(entry.second)},{"trait",Ref(trait)},
                                {"class",Ref(candidate,Class)},{"status",C::Quote(!candidate?"absent":valid?"present":"invalid")},
                                {"additive_geometry","false"}}));
                            if(valid){if(std::strcmp(entry.second,"high_detail_actor")==0&&!high)high=candidate;if(std::strcmp(entry.second,"low_detail_actor")==0&&!low)low=candidate;}
                        }
                        hasMass|=InspectMass(trait,visual);
                    });
                    config=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UMassEntityConfigAsset*>(config)->Config.Parent);
                    if(config&&depth==15)Source("entity_configuration",definition,"truncated","configuration parent depth");
                }
                const bool nullConfig=!reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UAuActorPlacementData*>(definition)->EntityType.EntityConfigPtr);
                uintptr_t authored=0;
                if(nullConfig)
                {
                    Source("entity_configuration",definition,"unavailable","cached_config_null; trying resident authored ActorClass templates only");
                    authored=ResolveResidentAuthoredClass(definition);
                    Source("resident_authored_actor_class",authored,AuthoredClassStatus.c_str());
                    if(authored)InstancedComponent=NativeClass(L"/Script/Engine.InstancedStaticMeshComponent");
                }
                const auto selected=nullConfig?authored:high?high:low;
                if(selected)
                {
                    Selected=Ref(selected,Class);SelectedTier=nullConfig?"authored_actor_class":high?"high_detail_actor":"low_detail_actor";
                    Reason=nullConfig?"resident authored ActorClass templates; initialized actor equivalence and horizontal surface unverified":
                        "first validated resident actor class in bounded configuration traversal; high-detail preferred; alternatives remain separate";
                    AuditClass=selected;InspectClass(selected,0,"selected_actor_class_templates");Status="partial";
                }
                else if(hasMass)
                {Selected=Ref(definition,Placement);SelectedTier="mass_ism";Status="partial";Reason="no validated resident actor class; resident Mass metadata recorded separately";}
                return selected||hasMass;
            }
            #include "AssemblyRecipeAudit.inl"
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
            #include "SourceMetadataAudit.inl"
            #include "GenericRepresentationAudit.inl"
            // Independent bounded observer: no mutation of the production audit's
            // graph, budget, recipe, eligibility flags or retained asset inventory.
            std::string InspectStaticInventory(const std::map<std::string,A::Node>& nodes,
                const std::map<std::string,A::Result>& native)
            {
                if(!AuthoredFallback)return "{}";
                {
                    std::scoped_lock lock(blueprintIdentityAccess);
                    if(blueprintIdentities.Generation!=Record.Generation||blueprintIdentities.InventoryReports>=8)return "{}";
                    if(BlueprintIdentity.Path)if(auto* path=blueprintIdentities.FindPath(*BlueprintIdentity.Path))
                    {if(path->InventoryReported)return "{}";path->InventoryReported=true;}
                    ++blueprintIdentities.InventoryReports;
                }
                AuditElapsed inspectionElapsed(Metrics?&Metrics->InspectionNs:nullptr);
                MiniMapNative::ValidationProfile profile;AuditMetrics inspectionMetrics;
                MiniMapNative::ScopedValidationProfile profileScope(profile);
                C::JsonArray records,fields;records.Limit=24*1024;
                bool limited=StaticObservationOverflow;
                size_t discovered=StaticObservations.size();
                std::string status="native_anchors_unavailable";
                {
                    Context d;d.Metrics=&inspectionMetrics;d.FullTraversal=true;d.Record=Record;
                    if(d.Initialize()&&d.ClassObject(AuditClass)&&d.Derives(AuditClass,d.Actor)&&d.Hold(AuditClass))
                    {
                        status="read_only_not_admitted";
                        uintptr_t cdo=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UClass*>(AuditClass)->ClassDefaultObject),root=0;
                        const bool validCdo=d.Is(cdo,AuditClass)&&d.Hold(cdo)&&(uint32_t(reinterpret_cast<const SDK::UObject*>(cdo)->Flags)&0x10);
                        if(validCdo)
                        {root=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::AActor*>(cdo)->RootComponent);if(!d.Is(root,d.Scene)||!d.Hold(root))root=0;}
                        auto inventory=StaticObservations; // bounded comparison tokens; original Context still retains sources.
                        const auto base=d.NativeClass(L"/Script/Chimera.CrBuildingActorBase");
                        if(validCdo&&base&&d.Derives(AuditClass,base)&&d.Is(cdo,AuditClass))
                        {
                            const std::pair<uintptr_t,const char*> offsets[]={{reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::AActor*>(cdo)->RootComponent),"RootComponent"},{reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::ACrBuildingActorBase*>(cdo)->MainComponent),"MainComponent"},{reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::ACrBuildingActorBase*>(cdo)->MainMeshComponent),"MainMeshComponent"}};
                            for(const auto& field:offsets)
                            {
                                const auto component=field.first;
                                const bool valid=d.Is(component,d.StaticComponent)&&d.Hold(component);
                                fields.Add(C::Object({{"field",C::Quote(field.second)},{"static_component_valid_retained",C::Bool(valid)},
                                    {"component",component?d.Ref(component):"null"}}));
                                if(valid)inventory.insert(inventory.begin(),{component,AuditClass,std::string("native_cdo_field:")+field.second});
                            }
                        }
                        discovered=inventory.size();
                        for(const auto& observation:inventory)
                        {
                            if(records.Records.size()>=16||d.Work.Exhausted){limited=true;break;}
                            const auto component=observation.Component;
                            if(!d.Is(component,d.StaticComponent)||!d.Hold(component))
                            {records.Add(C::Object({{"source",C::Quote(observation.Source)},{"rejection",C::Quote("component_identity_type_or_retention_failed")}}));continue;}
                            const auto type=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(component)->Class),mesh=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UStaticMeshComponent*>(component)->StaticMesh);
                            const bool exact=type==d.StaticComponent;
                            const bool assetIdentity=d.Is(mesh,d.StaticMesh);
                            const bool asset=assetIdentity&&d.Hold(mesh);
                            const auto parent=reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::USceneComponent*>(component)->AttachParent);
                            A::Transform local;bool absolute=false,socket=false;
                            const bool localValid=d.AuditTransform(component,local,absolute,socket);
                            C::JsonArray matches;A::Result transform;
                            const auto originalId=Id(component);bool matched=false;
                            for(const auto& [id,node]:nodes)if(node.Component==originalId)
                            {
                                matches.Add(C::Quote(id));matched=true;
                                std::set<std::string> active;size_t remaining=A::VisitLimit;
                                transform=A::Resolve(id,nodes,native,active,0,&remaining);
                            }
                            if(!matched&&root)
                            {std::set<uintptr_t> active;C::JsonArray chain;transform=d.AuditNativeChain(component,root,active,0,chain);}
                            else if(!matched){transform.Status="invalid";transform.Reason="validated_cdo_root_unavailable";}
                            bool defaultsValid=false,overridesValid=false;std::string defaultsJson,overridesJson;
                            const auto defaults=asset?d.AuditMaterials(reinterpret_cast<const SDK::UStaticMesh*>(mesh)->StaticMaterials,defaultsValid,defaultsJson):std::vector<A::Slot>{};
                            const auto overrides=d.AuditMaterials(reinterpret_cast<const SDK::UMeshComponent*>(component)->OverrideMaterials,overridesValid,overridesJson);
                            const auto effective=A::ProposedMaterials(defaults,overrides);
                            bool materials=defaultsValid&&overridesValid&&!effective.empty()&&effective.size()<=MiniMapStaticAssembly::Slots;
                            MiniMapStaticAssembly::Instance instance;instance.Mesh=asset?mesh:0;
                            instance.ComponentIndex=uint32_t(int32_t(reinterpret_cast<const SDK::UObject*>(component)->Index));instance.Transform=transform.Effective;
                            instance.Visible=reinterpret_cast<const SDK::USceneComponent*>(component)->bVisible;
                            instance.Hidden=reinterpret_cast<const SDK::USceneComponent*>(component)->bHiddenInGame;
                            instance.MaterialCount=(std::min)(effective.size(),MiniMapStaticAssembly::Slots);
                            for(const auto& slot:effective)
                            {
                                const auto found=d.AuditMaterialPointers.find(slot.Material);
                                if(!slot.Valid||slot.Index>=instance.MaterialCount||found==d.AuditMaterialPointers.end())materials=false;
                                else instance.Materials[slot.Index]=found->second;
                            }
                            if(asset){const auto& b=reinterpret_cast<const SDK::UStaticMesh*>(mesh)->ExtendedBounds;
                                instance.LocalBounds.Origin={b.Origin.X,b.Origin.Y,b.Origin.Z};instance.LocalBounds.Extent={b.BoxExtent.X,b.BoxExtent.Y,b.BoxExtent.Z};}
                            const bool bounds=asset&&
                                A::Finite(instance.LocalBounds.Origin)&&A::Finite(instance.LocalBounds.Extent);
                            instance.Verified=asset&&materials&&bounds&&transform.Status=="verified";
                            MiniMapStaticAssembly::Recipe one;one.General=true;one.Complete=true;one.RootVerified=root!=0;
                            one.ActorClass=AuditClass;one.Count=1;one.Instances[0]=instance;
                            MiniMapStaticAssembly::Footprint footprint;
                            const auto rejection=MiniMapStaticAssembly::Validate(one,footprint);
                            const char* reason=!exact?"derived_static_component_semantics_unverified":!asset?!mesh?"mesh_assignment_null":!assetIdentity?"mesh_identity_or_type_invalid":"mesh_retention_failed":
                                transform.Status!="verified"?transform.Reason.c_str():rejection?rejection:"individual_static_inputs_pass; overall_admission_unchanged";
                            const bool ownerValid=d.ClassObject(observation.Owner)&&d.Hold(observation.Owner);
                            // Read-only comparison with the fresh isolated component.
                            // Custom primitive inputs may affect material opacity/WPO;
                            // do not copy them or infer shader behavior in this diagnostic stage.
                            auto customData=[&](const SDK::FCustomPrimitiveData& data)
                            {
                                const C::ArrayHeader header{reinterpret_cast<uintptr_t>(data.Data.GetDataPtr()),data.Data.Num(),data.Data.Max()};
                                const auto bound=C::BoundArray(header,sizeof(float),alignof(float),32);
                                if(!bound.Valid||bound.Truncated||(bound.Bytes&&!Readable(header.Data,bound.Bytes,alignof(float))))return std::string("null");
                                C::JsonArray values;
                                for(size_t i=0;i<bound.Count;++i)
                                {
                                    const auto value=data.Data[int32_t(i)];
                                    if(!std::isfinite(value))return std::string("null");
                                    values.Add(C::Number(value));
                                }
                                return values.String();
                            };
                            std::string captureInputs="null";
                            if(d.Is(component,d.StaticComponent))
                            {
                                const auto* source=reinterpret_cast<const SDK::UStaticMeshComponent*>(component);
                                captureInputs=C::Object({{"render_in_main_pass",C::Bool(source->bRenderInMainPass)},
                                    {"hidden_in_scene_capture",C::Bool(source->bHiddenInSceneCapture)},
                                    {"forced_lod_model",std::to_string(source->ForcedLodModel)},{"min_lod",std::to_string(source->MinLOD)},
                                    {"disallow_nanite",C::Bool(source->bDisallowNanite)},{"force_disable_nanite",C::Bool(source->bForceDisableNanite)},
                                    {"evaluate_world_position_offset",C::Bool(source->bEvaluateWorldPositionOffset)},
                                    {"reverse_culling",C::Bool(source->bReverseCulling)},
                                    {"custom_primitive_authored",customData(source->CustomPrimitiveData)},
                                    {"custom_primitive_internal",customData(source->CustomPrimitiveDataInternal)}});
                            }
                            const auto record=C::Object({{"source",C::Quote(observation.Source)},
                                {"audit_component_id",C::Quote(originalId)},{"component",d.Ref(component,d.StaticComponent)},
                                {"component_object_index",std::to_string(instance.ComponentIndex)},{"component_class",d.Ref(type,d.Class)},
                                {"derives_static_mesh_component","true"},{"exact_native_static_class",C::Bool(exact)},
                                {"enumeration_owner_class",ownerValid?d.Ref(observation.Owner,d.Class):"null"},
                                {"component_outer",d.Ref(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(component)->Outer))},
                                {"owner_is_selected_class",C::Bool(observation.Owner==AuditClass)},
                                {"mesh",asset?d.Ref(mesh,d.StaticMesh):"null"},{"mesh_valid_retained",C::Bool(asset)},
                                {"mesh_assignment_null",C::Bool(!mesh)},{"mesh_identity_type_valid",C::Bool(assetIdentity)},
                                {"mesh_object_index",asset?std::to_string(int32_t(reinterpret_cast<const SDK::UObject*>(mesh)->Index)):"null"},
                                {"mesh_class",asset?d.Ref(reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(mesh)->Class),d.Class):"null"},
                                {"mesh_qualified_path","null"},{"mesh_path_status",C::Quote("unavailable_no_verified_object_path_conversion")},
                                {"visible",C::Bool(instance.Visible)},{"hidden_in_game",C::Bool(instance.Hidden)},
                                {"parent",parent?d.Ref(parent,d.Scene):"null"},{"socket",d.NameValue(reinterpret_cast<const SDK::USceneComponent*>(component)->AttachSocketName)},
                                {"socket_readable_name","null"},{"socket_name_status",C::Quote("unavailable_no_verified_name_conversion")},
                                {"local_transform",localValid?A::Json(local):"null"},{"absolute_transform",C::Bool(absolute)},
                                {"effective_root_relative_transform",transform.Status=="verified"?A::Json(transform.Effective):"null"},
                                {"transform_status",C::Quote(transform.Status)},{"transform_reason",C::Quote(transform.Reason)},
                                {"identical_scs_nodes",matches.String()},{"exact_scs_identity_match",C::Bool(matched)},
                                {"default_material_slots",std::to_string(defaults.size())},{"override_material_slots",std::to_string(overrides.size())},
                                {"default_materials",defaultsJson.empty()?"[]":defaultsJson},{"override_materials",overridesJson.empty()?"[]":overridesJson},
                                {"effective_materials_valid",C::Bool(materials)},
                                {"mesh_local_bounds_cm",bounds?C::Object({{"origin",d.CopiedNumbers(instance.LocalBounds.Origin)},
                                    {"extent",d.CopiedNumbers(instance.LocalBounds.Extent)}}):"null"},
                                {"capture_input_state",captureInputs},
                                {"individual_static_input_check_passed",C::Bool(exact&&!rejection&&instance.Verified)},
                                {"rejection_or_status",C::Quote(reason)},{"horizontal_surface","null"},{"admitted_by_inspection","false"}});
                            if(!records.Add(record)){limited=true;break;}
                        }
                        limited|=d.Work.Exhausted||d.AuditTruncated;
                    }
                } // Diagnostic roots released while the original owner is still alive.
                LOG_INFO("MiniMap: native static inspection: generation=%llu path=%s status=%s records=%zu discovered=%zu truncated=%d; SpriteValidation.json; admission unchanged",
                    (unsigned long long)Record.Generation,BlueprintIdentity.Path?BlueprintIdentity.Path->c_str():"unresolved",
                    status.c_str(),records.Records.size(),discovered,int(limited));
                return C::Object({{"status",C::Quote(status)},{"canonical_blueprint",BlueprintIdentity.Path?C::Quote(*BlueprintIdentity.Path):"null"},
                    {"identity_scope",C::Quote("opaque_audit_local_comparison_tokens; not_sprite_keys")},
                    {"record_limit","16"},{"byte_limit","24576"},{"discovered_records",std::to_string(discovered)},
                    {"truncated",C::Bool(limited)},{"native_fields",fields.String()},{"records",records.String()},
                    {"identity_calls",std::to_string(profile.IdentityCalls)},{"identity_ms",C::Number(double(profile.IdentityNanoseconds)/1e6)},
                    {"readable_calls",std::to_string(profile.ReadableCalls)},{"readable_ms",C::Number(double(profile.ReadableNanoseconds)/1e6)},
                    {"virtual_queries",std::to_string(profile.VirtualQueries)},
                    {"retained_root_acquisitions",std::to_string(inspectionMetrics.Retained)},
                    {"roots_released",std::to_string(inspectionMetrics.Released)}});
            }
#endif
        };
        bool Visit(const SDK::UObject* definition,void* context)
        {return static_cast<Context*>(context)->Discover(reinterpret_cast<uintptr_t>(definition));}
        // Temporary Stage A.1 instrumentation. No admission authority.
        MiniMapDefinitionDiagnostic::Session& DefinitionDiagnosticSession()
        {static MiniMapDefinitionDiagnostic::Session session;return session;}
        void SummarizeDefinitionDiagnostic()noexcept
        {
            try
            {
            auto& diagnosticSession=DefinitionDiagnosticSession();if(!diagnosticSession.Generation||diagnosticSession.SummaryWritten)return;diagnosticSession.SummaryWritten=true;
            LOG_INFO("MiniMap: event=definition_diagnostic_summary generation=%llu tokens_observed=%zu zero_tokens=%zu nonzero_tokens=%zu attempts=%zu exact_matches=%zu mismatches=%zu completed_scopes=%zu configuration_changes=%zu source_changes=%zu replacements=%zu unchanged_visits=%zu suppressed=%zu interruptions=%zu token_read_failures=%zu zero_skips=%zu targets_11_227_229_336_238=%d%d%d%d%d; instance_disappearance=not_exercised no_continuous_polling=1 absence_of_change_not_proven=1",
                (unsigned long long)diagnosticSession.Generation,diagnosticSession.Observed,diagnosticSession.Zero,diagnosticSession.Nonzero,diagnosticSession.Attempts,diagnosticSession.Matches,diagnosticSession.Mismatches,diagnosticSession.CompletedScopes,
                diagnosticSession.ConfigChanges,diagnosticSession.SourceChanges,diagnosticSession.Replacements,diagnosticSession.Unchanged,diagnosticSession.Suppressed,diagnosticSession.Interruptions,diagnosticSession.TokenReadFailures,diagnosticSession.ZeroSkips,
                int(diagnosticSession.Samples[0].Used),int(diagnosticSession.Samples[1].Used),int(diagnosticSession.Samples[2].Used),int(diagnosticSession.Samples[3].Used),int(diagnosticSession.Samples[4].Used));
            for(size_t i=0;i<diagnosticSession.Rejections.size();++i)if(diagnosticSession.Rejections[i])
                LOG_INFO("MiniMap: event=definition_diagnostic_rejections generation=%llu status=%s count=%zu",
                    (unsigned long long)diagnosticSession.Generation,MiniMapDefinitionAcquisition::Label(MiniMapDefinitionAcquisition::Status(i)),diagnosticSession.Rejections[i]);
            }
            catch(...){/* Summary failure must not interrupt world retirement. */}
        }
        struct DefinitionSlotReader
        {
            bool Read(uintptr_t p,int32_t& v){return MiniMapNative::ReadMemory(p,&v,sizeof(v));}
            bool Read(uintptr_t p,uint32_t& v){return MiniMapNative::ReadMemory(p,&v,sizeof(v));}
            bool Read(uintptr_t p,uintptr_t& v){return MiniMapNative::ReadMemory(p,&v,sizeof(v));}
        };
        MiniMapDefinitionDiagnostic::Identity DefinitionObjectIdentity(uintptr_t p)
        {
            const auto* nativeApi=MiniMapNative::Get();
            const uintptr_t objectArray=nativeApi?reinterpret_cast<uintptr_t>(nativeApi->asset.objectArray):0;
            MiniMapDefinitionDiagnostic::Identity result;result.Present=p!=0;if(!p||!Identity(p))return result;
            DefinitionSlotReader reader;MiniMapDefinitionAcquisition::Slot slot;
            result.Index=int32_t(reinterpret_cast<const SDK::UObject*>(p)->Index);const auto name=reinterpret_cast<const SDK::UObject*>(p)->Name;
            if(MiniMapDefinitionAcquisition::ReadSlot(reader,objectArray,result.Index,slot)!=MiniMapDefinitionAcquisition::Status::Success||slot.Object!=p)return result;
            result.Serial=slot.Serial;result.NameIndex=name.ComparisonIndex;result.NameNumber=name.Number;result.Validated=true;return result;
        }
        bool DefinitionFields(uintptr_t p,MiniMapDefinitionDiagnostic::Observation& out)
        {
            const auto* source=reinterpret_cast<const SDK::UAuActorPlacementData*>(p);
            const auto& soft=source->ActorClass;const auto& path=soft.ObjectID.AssetPath;
            out.AuthoredReference={soft.WeakPtr.ObjectIndex,soft.WeakPtr.ObjectSerialNumber,path.PackageName.ComparisonIndex,
                int32_t(path.PackageName.Number),path.AssetName.ComparisonIndex,int32_t(path.AssetName.Number)};
            out.SubpathBounds={soft.ObjectID.SubPathString.Num(),soft.ObjectID.SubPathString.Max()};
            out.Config=DefinitionObjectIdentity(reinterpret_cast<uintptr_t>(source->EntityType.EntityConfigPtr));return true;
        }
        void CompareDefinition(const SDK::UObject* definition,Context& original)noexcept
        {
            const auto* nativeApi=MiniMapNative::Get();
            const uintptr_t objectArray=nativeApi?reinterpret_cast<uintptr_t>(nativeApi->asset.objectArray):0;
            namespace D=MiniMapDefinitionAcquisition;namespace G=MiniMapDefinitionDiagnostic;
            const auto thread=nativeApi?nativeApi->player.isGameThread:nullptr;
            if(!thread||!reinterpret_cast<bool(*)()>(thread)()||original.ProbeResult.DefinitionIdentity!="valid")return;
            auto& session=DefinitionDiagnosticSession();
            try
            {
                if(session.Generation&&session.Generation!=original.Record.Generation)SummarizeDefinitionDiagnostic();
                if(session.Generation!=original.Record.Generation){session=G::Session{};session.Generation=original.Record.Generation;}
                const auto snapshot=MiniMapBuildingCollector::GetSnapshot();if(!snapshot)return;
                const auto originalPointer=reinterpret_cast<uintptr_t>(definition);
                // Read-only copy BEFORE checked acquisition. Neither this reader
                // nor ValidateObjectIdentity calls AllocateSerialNumber/weak assignment.
                DefinitionSlotReader reader;D::Slot slot;
                const auto readStatus=D::ReadSlot(reader,objectArray,original.Record.DefinitionObjectIndex,slot);
                if(readStatus!=D::Status::Success){
                    if(session.TokenReadFailures++<4)LOG_INFO("MiniMap: event=definition_token_unavailable generation=%llu building=%u index=%d status=%s",
                        (unsigned long long)session.Generation,original.Record.BuildingId,original.Record.DefinitionObjectIndex,D::Label(readStatus));return;}
                G::Observation observed;
                observed.Token={original.Record.Generation,snapshot->PublishedAtSeconds,original.Record.DefinitionObjectIndex,slot.Serial,
                    original.Record.DefinitionIndex,original.Record.DefinitionNumber,original.Record.BuildingId};
                if(!DefinitionFields(originalPointer,observed)){
                    if(session.TokenReadFailures++<4)LOG_INFO("MiniMap: event=definition_token_unavailable generation=%llu building=%u index=%d serial=%d status=unreadable_definition_fields checked_acquisition_attempted=0",
                        (unsigned long long)session.Generation,observed.Token.Building,observed.Token.Index,observed.Token.Serial);return;}
                observed.AuthoredClass=DefinitionObjectIdentity(original.AuthoredClass);
                observed.High=DefinitionObjectIdentity(original.ProbeResult.High);observed.Low=DefinitionObjectIdentity(original.ProbeResult.Low);
                observed.Route=unsigned(original.ProbeResult.Route);observed.HighStatus=original.ProbeResult.HighStatus;
                observed.LowStatus=original.ProbeResult.LowStatus;observed.AuthoredStatus=original.AuthoredClassStatus;
                if(original.BlueprintIdentity.Path)observed.CanonicalBlueprint=*original.BlueprintIdentity.Path;
                const auto selected=session.Select(observed);if(!selected.Record)return;
                const auto previousStatus=selected.Record->LastStatus;
                const auto name=reinterpret_cast<const SDK::UObject*>(originalPointer)->Name;
                const bool identityMatch=slot.Object==originalPointer&&int32_t(reinterpret_cast<const SDK::UObject*>(originalPointer)->Index)==observed.Token.Index&&
                    name.ComparisonIndex==observed.Token.NameIndex&&name.Number==observed.Token.NameNumber&&uint32_t(reinterpret_cast<const SDK::UAuActorPlacementData*>(originalPointer)->BuildingID)==observed.Token.Building;
                LOG_INFO("MiniMap: event=definition_token_observed generation=%llu building=%u index=%d serial=%d serial_zero=%d identity_match=%d previously_sampled=%d change=%s config_present=%d config_index=%d config_serial=%d route=%u high=%s low=%s; observed_before_acquisition=1 serial_allocation=0 selection_metadata=authoritative_only",
                    (unsigned long long)session.Generation,observed.Token.Building,observed.Token.Index,observed.Token.Serial,int(slot.Serial==0),int(identityMatch),
                    int(selected.PreviouslySampled),G::Label(selected.Why),int(observed.Config.Present),observed.Config.Index,observed.Config.Serial,
                    observed.Route,observed.HighStatus.c_str(),observed.LowStatus.c_str());
                if(slot.Serial==0){++session.ZeroSkips;selected.Record->LastStatus=D::Status::InvalidToken;
                    LOG_INFO("MiniMap: event=definition_serial_zero generation=%llu building=%u index=%d checked_acquisition_attempted=0",
                        (unsigned long long)session.Generation,observed.Token.Building,observed.Token.Index);return;}
                struct Comparison
                {
                    const G::Observation& Expected;uintptr_t Original;
                    int32_t AcquiredIndex=-1,AcquiredSerial{};bool Lifetime=false,Fields=false;
                } comparison{observed,originalPointer};
                D::ScopeObservation scope;auto status=D::Status::DefinitionMismatch;
                if(identityMatch){++session.Attempts;
                    status=VisitResidentDefinition(observed.Token,[](const SDK::UObject* object,void* state)
                    {
                        auto& c=*static_cast<Comparison*>(state);const auto p=reinterpret_cast<uintptr_t>(object);
                        const auto* nativeApi=MiniMapNative::Get();
                        const uintptr_t objectArray=nativeApi?reinterpret_cast<uintptr_t>(nativeApi->asset.objectArray):0;
                        DefinitionSlotReader reader;D::Slot acquired;G::Observation fields;
                        c.AcquiredIndex=int32_t(reinterpret_cast<const SDK::UObject*>(p)->Index);
                        if(D::ReadSlot(reader,objectArray,c.AcquiredIndex,acquired)!=D::Status::Success)return;
                        c.AcquiredSerial=acquired.Serial;
                        const auto name=reinterpret_cast<const SDK::UObject*>(p)->Name;
                        c.Lifetime=p==c.Original&&acquired.Object==p&&c.AcquiredIndex==c.Expected.Token.Index&&c.AcquiredSerial==c.Expected.Token.Serial&&
                            name.ComparisonIndex==c.Expected.Token.NameIndex&&name.Number==c.Expected.Token.NameNumber&&uint32_t(reinterpret_cast<const SDK::UAuActorPlacementData*>(p)->BuildingID)==c.Expected.Token.Building;
                        c.Fields=DefinitionFields(p,fields)&&fields.Config==c.Expected.Config&&fields.AuthoredReference==c.Expected.AuthoredReference&&fields.SubpathBounds==c.Expected.SubpathBounds;
                    },&comparison,&scope);
                }
                const bool match=comparison.Lifetime&&comparison.Fields;
                session.Complete(*selected.Record,status,match,scope);
                LOG_INFO("MiniMap: event=%s generation=%llu building=%u expected_index=%d expected_serial=%d acquired_index=%d acquired_serial=%d lifetime_match=%d fields_match=%d status=%s previous_status=%s readiness_changed=%d",
                    status==D::Status::Success&&match?"definition_acquisition_match":"definition_acquisition_rejected",
                    (unsigned long long)session.Generation,observed.Token.Building,observed.Token.Index,observed.Token.Serial,comparison.AcquiredIndex,comparison.AcquiredSerial,
                    int(comparison.Lifetime),int(comparison.Fields),D::Label(status),selected.PreviouslySampled?D::Label(previousStatus):"not_sampled",int(selected.PreviouslySampled&&previousStatus!=status));
                LOG_INFO("MiniMap: event=definition_retain_scope_completed generation=%llu building=%u retain_reached=%u addref_returned=%u retained_scopes=%u post_retain_validated=%d visitor_entered=%d visitor_returned=%d release_reached=%u release_returned=%u interrupted=%d; engine_refcount_not_inspected=1",
                    (unsigned long long)session.Generation,observed.Token.Building,scope.RetainReached,scope.RetainReturned,scope.Retained,
                    int(scope.PostRetainValidated),int(scope.VisitorEntered),int(scope.VisitorReturned),scope.ReleaseReached,scope.ReleaseReturned,int(scope.Interrupted));
            }
            catch(...){++session.Interruptions;/* Diagnostic failure cannot change authoritative completion. */}
        }
        bool ProbeVisit(const SDK::UObject* definition,void* context)
        {
            auto& c=*static_cast<Context*>(context);
            const bool accepted=c.ProbeDefinition(reinterpret_cast<uintptr_t>(definition));
            CompareDefinition(definition,c);
            return accepted;
        }

        void BeforeEndPlay(SDK::UWorld* world,const char*)
        {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::RepresentationWorldEnd,false);
            if(!world||world!=MiniMapMap::GetWorld())return;
            const auto gameThread=MiniMapNative::Get()?MiniMapNative::Get()->player.isGameThread:nullptr;
            if(gameThread&&reinterpret_cast<bool(*)()>(gameThread)()){SummarizeDefinitionDiagnostic();endingWorld=world;ClearBlueprintIdentities();}
            else stopped.store(true);
        }
        void StopImpl(){
            const auto thread=MiniMapNative::Get()?MiniMapNative::Get()->player.isGameThread:nullptr;
            if(thread&&reinterpret_cast<bool(*)()>(thread)())SummarizeDefinitionDiagnostic();
            stopped.store(true);
        }
        void Stop()
        {
            MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::RepresentationStop,false);
            StopImpl();
        }

    }
    BlueprintIdentityResult GetBlueprintIdentity(const MiniMapBuildingCollector::Record& record)
    {
        if(record.HasSpline||!record.DefinitionValid)return {{},"definition_identity_unavailable"};
        std::scoped_lock lock(blueprintIdentityAccess);
        if(record.Generation!=blueprintIdentities.Generation)return {{},"identity_generation_mismatch"};
        const std::array<int64_t,3> token={record.DefinitionObjectIndex,record.DefinitionIndex,record.DefinitionNumber};
        if(const auto* entry=blueprintIdentities.Find(token))return entry->Identity;
        return {};
    }
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
    MiniMapSpriteValidation::Result ValidateRepresentationImpl(const MiniMapBuildingCollector::Record& record,
        const MiniMapBuildingCollector::Record* alternate,bool captureRequested,bool deferCapture=false)
    {
        namespace V=MiniMapSpriteValidation;
        V::Result out;
        const auto* bindings=MiniMapNative::Get();
        const uintptr_t objectArray=bindings?reinterpret_cast<uintptr_t>(bindings->asset.objectArray):0;
        const auto snapshot=MiniMapBuildingCollector::GetSnapshot();auto* world=MiniMapMap::GetWorld();
        if(!objectArray||!engine||!bindings||!bindings->player.isGameThread||!bindings->asset.retainObject||!bindings->asset.releaseObject||stopped.load()||
            !bindings||!bindings->player.isGameThread()||!world||world==endingWorld||!snapshot||
            !snapshot->Complete||snapshot->Generation!=record.Generation)
        {out.MarkDeferred(V::AuditWait::NativePrerequisites);out.Reason="native_facility_or_world_generation_unavailable";return out;}
        if(executing.test_and_set(std::memory_order_acquire)){out.MarkDeferred(V::AuditWait::NativeOwnerBusy);out.Reason="representation_service_busy";return out;}
        struct Leave {~Leave(){executing.clear(std::memory_order_release);}} leave;
        const auto auditStart=std::chrono::steady_clock::now();
        MiniMapNative::ValidationProfile validationProfile;
        MiniMapNative::ScopedValidationProfile profileScope(validationProfile);
        AuditMetrics metrics;
        try
        {
#if defined(MINIMAP_BUILDING_SPRITES)
            MiniMapBuildingSprites::TimingScope auditTiming(MiniMapBuildingSprites::Timing::RepresentationAudit);
#endif
            Context context;context.Metrics=&metrics;context.GenericValidation=true;context.CaptureRequested=captureRequested;context.Record=record;
            context.DeferCapture=deferCapture;
            if(!context.Initialize()){out.MarkDeferred(V::AuditWait::NativePrerequisites);out.Reason="resident_native_class_anchors_unavailable";return out;}
            auto status=MiniMapBuildingCollector::VisitDiagnosticDefinition(context.Record,&ProbeVisit,&context);out.Probes=1;
            if(status==MiniMapBuildingCollector::DiagnosticVisitStatus::StaleEntity&&alternate&&
                alternate->Generation==record.Generation&&alternate->DefinitionObjectIndex==record.DefinitionObjectIndex&&
                alternate->DefinitionIndex==record.DefinitionIndex&&alternate->DefinitionNumber==record.DefinitionNumber&&alternate->BuildingId==record.BuildingId)
            {
                context.Record=*alternate;context.ProbeResult={};
                status=MiniMapBuildingCollector::VisitDiagnosticDefinition(context.Record,&ProbeVisit,&context);++out.Probes;
            }
            auto unavailable=[](MiniMapBuildingCollector::DiagnosticVisitStatus status)
            {
                using S=MiniMapBuildingCollector::DiagnosticVisitStatus;
                return status==S::CollectorUnavailable||status==S::WorldGenerationChanged||status==S::StaleEntity||status==S::ParametersUnavailable;
            };
            const auto probes=out.Probes;
            if(status==MiniMapBuildingCollector::DiagnosticVisitStatus::Visited)
            {
                context.FullTraversal=true;
#if defined(MINIMAP_BUILDING_SPRITES)
                MiniMapBuildingSprites::TimingScope traversalTiming(MiniMapBuildingSprites::Timing::RepresentationTraversal);
#endif
                const auto full=MiniMapBuildingCollector::VisitDiagnosticDefinition(context.Record,&Visit,&context);
                if(full==MiniMapBuildingCollector::DiagnosticVisitStatus::Visited&&context.AuditClass)out=context.GenericAudit();
                else if(full==MiniMapBuildingCollector::DiagnosticVisitStatus::Visited&&context.SelectedTier=="mass_ism")
                {out.Resident=true;out.Route="mass_ism";out.Status=V::Outcome::Unsupported;out.Reason="resident_mass_only_alternative; renderer_equivalent_transform_chain_not_verified";}
                else {out.Reason="selected_representation_revalidation_failed";if(unavailable(full))out.MarkDeferred(V::AuditWait::SourceUnavailable);}
            }
            else
            {
                out.Status=context.ProbeResult.DefinitionIdentity=="valid"&&!context.ProbeResult.Incomplete?
                    V::Outcome::NoResident:V::Outcome::Invalid;
                using VisitStatus=MiniMapBuildingCollector::DiagnosticVisitStatus;
                out.Reason=status==VisitStatus::StaleEntity?"stale_entity":status==VisitStatus::WorldGenerationChanged?"world_generation_changed":
                    !context.ProbeResult.Reasons.empty()?context.ProbeResult.Reasons.front():"collector_instance_unavailable";
                out.Truncated=context.ProbeResult.Truncated||context.Work.Exhausted||context.Graph.Truncated;
                if(unavailable(status))out.MarkDeferred(V::AuditWait::SourceUnavailable);
            }
            out.Probes=probes;out.High=context.ProbeResult.HighStatus;out.Low=context.ProbeResult.LowStatus;
#if defined(MINIMAP_STAGE_R26B4_CLASSIFICATION)
            out.Classification=context.Classification;
#endif
            out.DiagnosticTruncated|=context.DiagnosticTruncated;
            out.Config=context.ProbeResult.CachedConfig;out.AuthoredClass=context.AuthoredClassStatus;
            out.Mass=context.ProbeResult.Mesh?"validated_resident_mesh_assignments":context.ProbeResult.MassStatus;
            out.IdentityVisits=context.Work.SelectionVisits+context.Work.RepresentationVisits;
            out.GraphVisits=C::VisitLimit-context.Graph.Remaining;out.AuditVisits=context.AuditVisits;
            if(context.ProbeResult.Truncated||context.ProbeResult.Incomplete)
            {out.Truncated=true;if(out.Status==V::Outcome::Eligible){out.Status=V::Outcome::Conditional;out.Reason="selection_probe_incomplete";}}
            const auto current=MiniMapBuildingCollector::GetSnapshot();
            if(world!=MiniMapMap::GetWorld()||!current||current->Generation!=record.Generation)
            {out.MarkDeferred(V::AuditWait::SourceUnavailable);out.Reason="world_generation_changed_during_audit";}
            context.ReportAuthoredAcquisition(out);
            metrics.PrimaryRoots=context.Roots.size();
        } // Context releases every strong root before return/file export.
        catch(...)
        {
            out.MarkInterrupted();
            try{out.Reason="native_audit_exception";}catch(...){out.Reason.clear();}
        }
        // All Context roots have now been released. Times are inclusive/nested;
        // independent component-inspection validations are included in totals.
        const double total=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-auditStart).count();
        // One audit opportunity per scheduler tick. These measure actual blocking
        // time, not the scheduler's pre-entry deadline. CPU-only generation totals.
        static uint64_t profileGeneration=0,profileAudits=0,profileIdentities=0,profileQueries=0;
        static double maxTickAuditMs=0,totalAuditMs=0;
        if(profileGeneration!=record.Generation)
        {profileGeneration=record.Generation;profileAudits=profileIdentities=profileQueries=0;maxTickAuditMs=totalAuditMs=0;}
        ++profileAudits;profileIdentities+=validationProfile.IdentityCalls;profileQueries+=validationProfile.VirtualQueries;
        totalAuditMs+=total;maxTickAuditMs=(std::max)(maxTickAuditMs,total);
        try
        {
            const auto identity=GetBlueprintIdentity(record);
            out.Profile=C::Object({{"canonical_blueprint",identity.Path?C::Quote(*identity.Path):"null"},
                {"scope",C::Quote("plugin_validation_on_auditing_thread; nested_times_not_additive; inspection_validation_included")},
                {"total_audit_ms",C::Number(total)},{"inspection_ms",C::Number(double(metrics.InspectionNs)/1e6)},
                {"generation_audits",std::to_string(profileAudits)},{"generation_total_audit_ms",C::Number(totalAuditMs)},
                {"maximum_tick_audit_ms",C::Number(maxTickAuditMs)},
                {"generation_identity_calls",std::to_string(profileIdentities)},
                {"generation_virtual_queries",std::to_string(profileQueries)},
                {"exceeds_scheduler_2ms_budget",C::Bool(total>2.0)},
                {"identity_calls",std::to_string(validationProfile.IdentityCalls)},
                {"retained_identity_reuses",std::to_string(metrics.IdentityHits)},
                {"owned_field_reads",std::to_string(metrics.FieldHits)},
                {"identity_ms",C::Number(double(validationProfile.IdentityNanoseconds)/1e6)},
                {"readable_calls",std::to_string(validationProfile.ReadableCalls)},
                {"readable_ms",C::Number(double(validationProfile.ReadableNanoseconds)/1e6)},
                {"virtual_queries",std::to_string(validationProfile.VirtualQueries)},
                {"class_calls",std::to_string(metrics.ClassCalls)},{"class_ms",C::Number(double(metrics.ClassNs)/1e6)},
                {"class_metadata_reuses",std::to_string(metrics.ClassHits)},
                {"inheritance_calls",std::to_string(metrics.InheritanceCalls)},{"inheritance_ms",C::Number(double(metrics.InheritanceNs)/1e6)},
                {"inheritance_reuses",std::to_string(metrics.InheritanceHits)},
                {"hold_checks",std::to_string(metrics.HoldCalls)},{"hold_ms",C::Number(double(metrics.HoldNs)/1e6)},
                {"retained_root_acquisitions",std::to_string(metrics.Retained)},{"roots_released",std::to_string(metrics.Released)},
                {"primary_context_unique_roots",std::to_string(metrics.PrimaryRoots)},
                {"recipe_ms_including_inspection",C::Number(double(metrics.RecipeNs)/1e6)}});
            std::scoped_lock lock(blueprintIdentityAccess);
            if(blueprintIdentities.Generation==record.Generation&&blueprintIdentities.ProfileReports<64)
            {
                ++blueprintIdentities.ProfileReports;
                LOG_INFO("MiniMap: validation profile %zu/64: generation=%llu building=%u totalMs=%.3f identityCalls=%llu identityMs=%.3f readableCalls=%llu readableMs=%.3f virtualQueries=%llu classMs=%.3f inheritanceMs=%.3f holdChecks=%llu holdMs=%.3f roots=%llu released=%llu recipeMs=%.3f inspectionMs=%.3f; nested times not additive; per-blueprint details in SpriteValidation.json",
                    blueprintIdentities.ProfileReports,(unsigned long long)record.Generation,record.BuildingId,total,
                    (unsigned long long)validationProfile.IdentityCalls,double(validationProfile.IdentityNanoseconds)/1e6,
                    (unsigned long long)validationProfile.ReadableCalls,double(validationProfile.ReadableNanoseconds)/1e6,
                    (unsigned long long)validationProfile.VirtualQueries,double(metrics.ClassNs)/1e6,double(metrics.InheritanceNs)/1e6,
                    (unsigned long long)metrics.HoldCalls,double(metrics.HoldNs)/1e6,(unsigned long long)metrics.Retained,
                    (unsigned long long)metrics.Released,double(metrics.RecipeNs)/1e6,double(metrics.InspectionNs)/1e6);
                LOG_INFO("MiniMap: audit blocking profile: generation=%llu audits=%llu maximumTickAuditMs=%.3f totalAuditMs=%.3f identityCalls=%llu virtualQueries=%llu classReuses=%llu inheritanceReuses=%llu retainedIdentityReuses=%llu ownedFieldReads=%llu over2ms=%d; synchronous audit is not interrupted by scheduler deadline",
                    (unsigned long long)record.Generation,(unsigned long long)profileAudits,maxTickAuditMs,totalAuditMs,
                    (unsigned long long)profileIdentities,(unsigned long long)profileQueries,
                    (unsigned long long)metrics.ClassHits,(unsigned long long)metrics.InheritanceHits,
                    (unsigned long long)metrics.IdentityHits,(unsigned long long)metrics.FieldHits,int(total>2.0));
            }
        }catch(...){LOG_WARN("MiniMap: validation profile unavailable; admission unchanged");}
        return out;
    }
    MiniMapSpriteValidation::Result ValidateRepresentation(const MiniMapBuildingCollector::Record& r,const MiniMapBuildingCollector::Record* a)
    {return ValidateRepresentationImpl(r,a,false);}
    MiniMapSpriteValidation::Result ValidateAndCapture(const MiniMapBuildingCollector::Record& r,const MiniMapBuildingCollector::Record* a,bool deferCapture)
    {return ValidateRepresentationImpl(r,a,true,deferCapture);}
#endif
    void ServiceIdentityMaintenance()
    {blueprintIdentities.Maintain(blueprintIdentityAccess,16,2.0);}
    bool PrepareNativeClasses()
    {
        if(nativeAnchorCursor==std::size(nativeAnchors))return true;
        const auto* b=MiniMapNative::Get();
        const uintptr_t objectArray=b?reinterpret_cast<uintptr_t>(b->asset.objectArray):0;
        if(!MiniMapMap::GameplayReady()||!engine||!objectArray||!b||!b->player.isGameThread||!b->asset.retainObject||!b->asset.releaseObject||
            !b->player.isGameThread())return false;
#if defined(MINIMAP_BUILDING_SPRITES)
        MiniMapBuildingSprites::TimingScope timing(MiniMapBuildingSprites::Timing::NativeClassResolution);
#endif
        // One verified resident lookup is the smallest noninterruptible native unit.
        // Never do all 23 lookups synchronously beneath a candidate audit.
        const auto start=std::chrono::steady_clock::now();
        auto& anchor=nativeAnchors[nativeAnchorCursor++];
        using Find=SDK::UObject*(*)(SDK::UClass*,SDK::UObject*,const wchar_t*,bool);
        const auto p=reinterpret_cast<uintptr_t>(reinterpret_cast<Find>(engine->GetStaticFindObjectByNameAddress())(nullptr,nullptr,anchor.Path,false));
        if(Identity(p,sizeof(SDK::UClass)))
        {b->asset.retainObject(reinterpret_cast<SDK::UObject*>(p));anchor.Object=p;}
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(ms>=2.0){LOG_WARN("MiniMap: resident class preparation took %.3f ms; one noninterruptible lookup: %ls",ms,anchor.Path);}
        return nativeAnchorCursor==std::size(nativeAnchors);
    }
    uintptr_t AcquireResidentMetadata(const wchar_t* path,bool scriptStruct,size_t instanceSize)
    {
        MiniMapCallbackTiming::Phase timing(MiniMapCallbackTiming::Id::ResidentMetadata);
        const auto* b=MiniMapNative::Get();
        const uintptr_t objectArray=b?reinterpret_cast<uintptr_t>(b->asset.objectArray):0;
        if(!path||std::wcsncmp(path,L"/Script/",8)!=0||!instanceSize||instanceSize>INT32_MAX||
            !engine||!objectArray||stopped.load()||!MiniMapMap::GameplayReady()||!b||!b->player.isGameThread||!b->asset.retainObject||!b->asset.releaseObject||
            !b->player.isGameThread())return 0;
        const auto address=engine->GetStaticFindObjectByNameAddress();if(!address)return 0;
        using Find=SDK::UObject*(*)(SDK::UClass*,SDK::UObject*,const wchar_t*,bool);
        const auto find=reinterpret_cast<Find>(address);
        auto* classType=find(nullptr,nullptr,L"/Script/CoreUObject.Class",false);
        if(!Identity(reinterpret_cast<uintptr_t>(classType),sizeof(SDK::UClass))||classType->Class!=classType)return 0;
        const auto retain=b->asset.retainObject;
        retain(classType);
        struct Drop {SDK::UObject* Object;~Drop(){if(Object)MiniMapNative::Get()->asset.releaseObject(Object);}} classRoot{classType};
        auto* expected=scriptStruct?find(static_cast<SDK::UClass*>(classType),nullptr,L"/Script/CoreUObject.ScriptStruct",true):classType;
        if(!Identity(reinterpret_cast<uintptr_t>(expected),sizeof(SDK::UClass))||expected->Class!=classType)return 0;
        retain(expected);Drop expectedRoot{expected};
        // Exact class filtering plus full UObject identity precede typed access.
        // StaticFindObject is a resident hash lookup, never a package/load request.
        auto* object=find(static_cast<SDK::UClass*>(expected),nullptr,path,true);
        const auto p=reinterpret_cast<uintptr_t>(object);
        if(!Identity(p,scriptStruct?sizeof(SDK::UScriptStruct):sizeof(SDK::UClass))||object->Class!=expected)return 0;
        auto* type=static_cast<SDK::UStruct*>(object);
        if(type->Size!=int32_t(instanceSize))return 0;
        retain(object);
        static SDK::UWorld* reportedWorld=nullptr;static size_t reports=0;
        if(reportedWorld!=MiniMapMap::GetWorld()){reportedWorld=MiniMapMap::GetWorld();reports=0;}
        if(reports++<32)LOG_INFO("MiniMap: resident metadata acquired: path=%ls kind=%s instanceSize=%zu exactClass=1 retained=1; no global ObjectWalker scan",path,scriptStruct?"ScriptStruct":"Class",instanceSize);
        return p;
    }
    void ReleaseResidentMetadata(uintptr_t object)
    {
        if(!object)return;
        const auto* b=MiniMapNative::Get();
        if(!b||!b->player.isGameThread||!b->asset.releaseObject||!b->player.isGameThread())
        {LOG_ERROR("MiniMap: resident metadata release off game thread/unavailable; root retained, unload safety unverified");return;}
        b->asset.releaseObject(reinterpret_cast<SDK::UObject*>(object));
    }
    bool ValidateCaptureObject(uintptr_t p)
    {
        const auto* nativeApi=MiniMapNative::Get();
        const uintptr_t objectArray=nativeApi?reinterpret_cast<uintptr_t>(nativeApi->asset.objectArray):0;
        return objectArray&&Identity(p);
    }
    MiniMapDefinitionAcquisition::Status VisitResidentDefinition(
        const MiniMapDefinitionAcquisition::Token& token,void(*visitor)(const SDK::UObject*,void*),void* state,
        MiniMapDefinitionAcquisition::ScopeObservation* observation)
    {
        namespace D=MiniMapDefinitionAcquisition;
        if(observation)*observation={};
        const auto* b=MiniMapNative::Get();
        const uintptr_t objectArray=b?reinterpret_cast<uintptr_t>(b->asset.objectArray):0;
        if(!b||!b->player.isGameThread||!b->asset.retainObject||!b->asset.releaseObject||!objectArray||!visitor)return D::Status::FacilitiesUnavailable;
        if(!b||!b->player.isGameThread())return D::Status::WrongThread;
        if(stopped.load()||!MiniMapMap::GameplayReady())return D::Status::FacilitiesUnavailable;
        // The loader calls this synchronous game-thread scope after engine Tick.
        // UE 5.6 GCScopeLock::LockAsync/TryLockAsync are no-ops on this thread.
        // No engine dispatch, loading or yield occurs between slot check/AddRef.
        try
        {
            Context context;
            // Acquire only the class anchors needed for definition compatibility;
            // capture-resource availability is not definition acquisition evidence.
            context.Class=context.Find(L"/Script/CoreUObject.Class");
            if(!context.Class||!context.CheckIdentity(context.Class,sizeof(SDK::UClass))||
                reinterpret_cast<uintptr_t>(reinterpret_cast<const SDK::UObject*>(context.Class)->Class)!=context.Class)return D::Status::FacilitiesUnavailable;
            context.Placement=context.NativeClass(L"/Script/AuActorPlacement.AuActorPlacementData");
            if(!context.Placement)return D::Status::FacilitiesUnavailable;
            struct Ops
            {
                Context& C;
                bool GameThread()const{return MiniMapNative::Get()->player.isGameThread();}
                bool Available()const{return !stopped.load()&&MiniMapNative::Get()->asset.retainObject&&MiniMapNative::Get()->asset.releaseObject;}
                bool Read(uintptr_t p,int32_t& v){return MiniMapNative::ReadMemory(p,&v,sizeof(v));}
                bool Read(uintptr_t p,uint32_t& v){return MiniMapNative::ReadMemory(p,&v,sizeof(v));}
                bool Read(uintptr_t p,uintptr_t& v){return MiniMapNative::ReadMemory(p,&v,sizeof(v));}
                D::Status Current(const D::Token& t)const
                {
                    const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
                    if(!MiniMapMap::GetWorld()||MiniMapMap::GetWorld()==endingWorld||!snapshot||snapshot->Generation!=t.Generation)return D::Status::WorldChanged;
                    if(!snapshot->Complete||snapshot->PublishedAtSeconds!=t.Publication)return D::Status::PublicationChanged;
                    return D::Status::Success;
                }
                D::Status Compatible(uintptr_t p)
                {
                    const bool compatible=C.Is(p,C.Placement);
                    if(C.Work.Exhausted||C.Graph.Truncated)return D::Status::FacilitiesUnavailable;
                    return compatible?D::Status::Success:D::Status::WrongClass;
                }
                bool Retain(uintptr_t p)noexcept
                {MiniMapNative::Get()->asset.retainObject(reinterpret_cast<SDK::UObject*>(p));return true;}
                void Release(uintptr_t p)noexcept
                {MiniMapNative::Get()->asset.releaseObject(reinterpret_cast<SDK::UObject*>(p));}
            } ops{context};
            return D::Visit(ops,objectArray,token,[&](uintptr_t p){visitor(reinterpret_cast<const SDK::UObject*>(p),state);},observation);
        }
        catch(...){if(observation)observation->Interrupted=true;return D::Status::Interrupted;}
    }
    bool RevalidateCaptureSource(const MiniMapBuildingCollector::Record& record,MiniMapSpriteClassification::Observation& observation)
    {
        observation={};
#if defined(MINIMAP_STAGE_R26B4_CLASSIFICATION)
        const auto* b=MiniMapNative::Get();
        const uintptr_t objectArray=b?reinterpret_cast<uintptr_t>(b->asset.objectArray):0;
        const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
        if(!objectArray||stopped.load()||!b||!b->player.isGameThread||!b||!b->player.isGameThread()||
            !b->asset.retainObject||!b->asset.releaseObject||!MiniMapMap::GetWorld()||MiniMapMap::GetWorld()==endingWorld||
            !snapshot||!snapshot->Complete||snapshot->Generation!=record.Generation)return false;
        Context context;context.Record=record;
        if(!context.Initialize())return false;
        const auto status=MiniMapBuildingCollector::VisitDiagnosticDefinition(record,[](const SDK::UObject* object,void* state)
        {
            auto& c=*static_cast<Context*>(state);const auto p=reinterpret_cast<uintptr_t>(object);
            if(!c.Is(p,c.Placement)||!c.Hold(p))return false;
            const auto name=reinterpret_cast<const SDK::UObject*>(p)->Name;
            if(int32_t(reinterpret_cast<const SDK::UObject*>(p)->Index)!=c.Record.DefinitionObjectIndex||name.ComparisonIndex!=c.Record.DefinitionIndex||
                name.Number!=c.Record.DefinitionNumber||uint32_t(reinterpret_cast<const SDK::UAuActorPlacementData*>(p)->BuildingID)!=c.Record.BuildingId)return false;
            c.ClassificationInputs(p);return true;
        },&context);
        observation=context.Classification;
        const auto current=MiniMapBuildingCollector::GetSnapshot();
        return status==MiniMapBuildingCollector::DiagnosticVisitStatus::Visited&&current&&current->Complete&&
            current->Generation==record.Generation&&observation.Fixed&&observation.Subclass&&
            !observation.Truncated&&observation.InteriorTag!="unknown";
#else
        (void)record;return false;
#endif
    }
    bool ValidateCaptureMaterialSlots(uintptr_t component,const uintptr_t* materials,size_t count,size_t& failedSlot)
    {
        failedSlot=0;
        if(!materials||count>32||!Identity(component))return false;
        // The caller retains this component; keep its indexed identity check above.
        const auto& values=reinterpret_cast<const SDK::UMeshComponent*>(component)->OverrideMaterials;
        const C::ArrayHeader h{reinterpret_cast<uintptr_t>(values.GetDataPtr()),values.Num(),values.Max()};
        const auto b=C::BoundArray(h,sizeof(SDK::UMaterialInterface*),alignof(SDK::UMaterialInterface*),32);
        if(!b.Valid||b.Truncated||b.Count!=count||h.Max>64||(!h.Max&&h.Data)||(h.Max&&!Readable(h.Data,size_t(h.Max)*sizeof(SDK::UMaterialInterface*),alignof(SDK::UMaterialInterface*))))return false;
        for(size_t i=0;i<count;++i)
        {
            failedSlot=i;const auto material=reinterpret_cast<uintptr_t>(values[int32_t(i)]);
            if(material!=materials[i]||!Identity(material))return false;
        }
        return true;
    }
    bool ValidateCaptureMembership(uintptr_t capture,const uintptr_t* components,size_t count)
    {
        const auto* nativeApi=MiniMapNative::Get();
        const uintptr_t objectArray=nativeApi?reinterpret_cast<uintptr_t>(nativeApi->asset.objectArray):0;
        // Read the existing weak entries; never resolve a weak pointer. Match
        // index AND serial against the retained, directly validated component.
        if(!objectArray||!components||count>16||!Identity(capture))return false;
        // Components and actors are separate engine-owned show-only sources.
        // Reject any actor list, rather than allowing it to add world geometry.
        const auto* source=reinterpret_cast<const SDK::USceneCaptureComponent*>(capture);
        const C::ArrayHeader actors{reinterpret_cast<uintptr_t>(source->ShowOnlyActors.GetDataPtr()),source->ShowOnlyActors.Num(),source->ShowOnlyActors.Max()};
        const auto actorBounds=C::BoundArray(actors,sizeof(SDK::AActor*),alignof(SDK::AActor*),0);
        if(!actorBounds.Valid||actorBounds.Truncated||actors.Num!=0||actors.Max>64||
            (!actors.Max&&actors.Data)||(actors.Max&&!Readable(actors.Data,size_t(actors.Max)*sizeof(SDK::AActor*),alignof(SDK::AActor*))))return false;
        const C::ArrayHeader h{reinterpret_cast<uintptr_t>(source->ShowOnlyComponents.GetDataPtr()),source->ShowOnlyComponents.Num(),source->ShowOnlyComponents.Max()};
        const auto b=C::BoundArray(h,sizeof(SDK::FWeakObjectPtr),alignof(SDK::FWeakObjectPtr),16);
        if(!b.Valid||b.Truncated||b.Count!=count||h.Max>64||(!h.Max&&h.Data)||(h.Max&&!Readable(h.Data,size_t(h.Max)*sizeof(SDK::FWeakObjectPtr),alignof(SDK::FWeakObjectPtr))))return false;
        // Snapshot the bounded weak array once. No native dispatch/GC/yield
        // occurs here; every component's indexed identity is still checked below.
        std::array<SDK::FWeakObjectPtr,16> entries{};
        if(b.Bytes)std::memcpy(entries.data(),reinterpret_cast<const void*>(h.Data),b.Bytes);
        std::set<uintptr_t> unique;
        for(size_t i=0;i<count;++i)
        {
            const auto p=components[i];if(!unique.insert(p).second||!Identity(p))return false;
            const auto index=int32_t(reinterpret_cast<const SDK::UObject*>(p)->Index);const auto table=Value<uintptr_t>(objectArray,0x10);
            const auto chunk=Value<uintptr_t>(table,size_t(uint32_t(index)/SDK::TUObjectArray::ElementsPerChunk)*sizeof(SDK::FUObjectItem*));
            const auto serial=Value<int32_t>(chunk,size_t(uint32_t(index)%SDK::TUObjectArray::ElementsPerChunk)*sizeof(SDK::FUObjectItem)+0x10);
            if(serial<=0)return false;
            size_t matches=0;
            for(size_t j=0;j<count;++j)
                if(entries[j].ObjectIndex==index&&entries[j].ObjectSerialNumber==serial)++matches;
            if(matches!=1)return false;
        }
        return true;
    }
    bool ResolvePrerequisites()
    {
        return MiniMapNative::IsInitialized();
    }
    void Initialize()
    {
        const auto* b=MiniMapNative::Get();
        if(registered||!b||!b->asset.objectArray)return;
        auto* hooks=GetHooks();
        if(!hooks||!hooks->Engine||!hooks->World||!b||!b->player.isGameThread||!b->asset.retainObject||!b->asset.releaseObject)return;
        engine=hooks->Engine;worlds=hooks->World;
        if(!engine->GetStaticFindObjectByNameAddress||!engine->GetStaticFindObjectByNameAddress()||
            !engine->RegisterOnShutdown||!engine->UnregisterOnShutdown||!worlds->RegisterOnBeforeWorldEndPlay||!worlds->UnregisterOnBeforeWorldEndPlay)return;
        engine->RegisterOnShutdown(&Stop);worlds->RegisterOnBeforeWorldEndPlay(&BeforeEndPlay);registered=true;
        LOG_INFO("MiniMap: R2.4 capture validation enabled; native class preparation is serviced by sprite discovery");
    }
    void Shutdown()
    {
        StopImpl();
        if(registered){engine->UnregisterOnShutdown(&Stop);worlds->UnregisterOnBeforeWorldEndPlay(&BeforeEndPlay);registered=false;}
        if(executing.test_and_set()){LOG_ERROR("MiniMap: R2.4 shutdown overlaps synchronous representation service; DLL unload not certified");}
        else
        {
            const auto release=MiniMapNative::Get()?MiniMapNative::Get()->asset.releaseObject:nullptr;
            if(release)for(auto& anchor:nativeAnchors)if(anchor.Object)
            {reinterpret_cast<void(*)(SDK::UObject*)>(release)(reinterpret_cast<SDK::UObject*>(anchor.Object));anchor.Object=0;}
            nativeAnchorCursor=0;ClearBlueprintIdentities();
            while(blueprintIdentities.Maintain(blueprintIdentityAccess,16,(std::numeric_limits<double>::max)())){}
            executing.clear();
        }
    }
}
#endif
