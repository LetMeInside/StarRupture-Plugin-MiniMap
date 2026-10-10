#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
#include "BuildingRepresentationDiagnostic.h"
#include "RepresentationMetadataCore.h"
#include "RepresentationSelectionCore.h"
#include "AssemblyRecipeCore.h"
#include "RepresentationMetadataPatterns.h"
#include "ReadbackNativeAdapter.h"
#include "SmelterCaptureNativeBindings.h"
#include "../Map/BuildingCollector.h"
#include "../Map/TerrainCache.h"
#include "../Map/Map.h"
#include "../plugin_helpers.h"
#include <SDK/Chimera_classes.hpp>
#include <atomic>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <type_traits>

namespace MiniMapBuildingRepresentation
{
    namespace C=MiniMapRepresentationCore;
    namespace S=MiniMapRepresentationSelection;
    namespace A=MiniMapAssemblyRecipe;
    namespace
    {
        // Only native interfaces/addresses and lifecycle comparison tokens persist.
        // All UObject identities, strong roots and traversal containers are local.
        uintptr_t objectArray=0,skeletalGetter=0;
        IPluginEngineEvents* engine=nullptr;
        IPluginWorldEvents* worlds=nullptr;
        SDK::UWorld* endingWorld=nullptr; // comparison only; never dereferenced
        std::atomic<bool> stopped{false},attempted{false};
        std::atomic_flag executing=ATOMIC_FLAG_INIT;
        bool registered=false;
        constexpr auto NativeExcluded="sparse-set traversal bound not yet independently verified";

        bool Readable(uintptr_t p,size_t bytes,size_t alignment=1)
        {
            if(!p||!alignment||(alignment&(alignment-1))||(p&(alignment-1))||p>UINTPTR_MAX-bytes)return false;
            const auto end=p+bytes;
            while(p<end)
            {
                MEMORY_BASIC_INFORMATION m{};
                if(!VirtualQuery(reinterpret_cast<const void*>(p),&m,sizeof(m))||m.State!=MEM_COMMIT||
                    (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
                const auto protection=m.Protect&0xff;
                if(protection!=PAGE_READONLY&&protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY&&
                    protection!=PAGE_EXECUTE_READ&&protection!=PAGE_EXECUTE_READWRITE&&protection!=PAGE_EXECUTE_WRITECOPY)return false;
                const auto base=reinterpret_cast<uintptr_t>(m.BaseAddress);
                if(base>UINTPTR_MAX-m.RegionSize||base+m.RegionSize<=p)return false;
                p=(std::min)(end,base+m.RegionSize);
            }
            return true;
        }
        template<class T> bool Read(uintptr_t p,size_t offset,T& out)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if(p>UINTPTR_MAX-offset||!Readable(p+offset,sizeof(T)))return false;
            std::memcpy(&out,reinterpret_cast<const void*>(p+offset),sizeof(T));return true;
        }
        template<class T> T Value(uintptr_t p,size_t offset)
        {T out{};Read(p,offset,out);return out;} // Use only after validating the containing extent.
        struct Name { int32_t Index{};uint32_t Number{}; };
        static_assert(sizeof(Name)==8);
        static_assert(offsetof(SDK::UObject,Class)==0x10);
        static_assert(offsetof(SDK::UClass,ClassDefaultObject)==0x110);
        static_assert(offsetof(SDK::UAuActorPlacementData,EntityType)==0x120);
        static_assert(offsetof(SDK::UAuActorPlacementData,BuildingID)==0x170);
        static_assert(offsetof(SDK::FAuAPMassSpawnedEntityType,EntityConfigPtr)==0x30);
        static_assert(offsetof(SDK::UMassEntityConfigAsset,Config)==0x30);
        static_assert(offsetof(SDK::FMassEntityConfig,Traits)==8);
        static_assert(offsetof(SDK::UMassVisualizationTrait,StaticMeshInstanceDesc)==0x30);
        static_assert(offsetof(SDK::UMassVisualizationTrait,HighResTemplateActor)==0xd0);
        static_assert(offsetof(SDK::UCrMassRepVisCosmeticsTrait,StaticMeshInstanceDesc)==0x30);
        static_assert(offsetof(SDK::UCrMassRepVisCosmeticsTrait,LowResTemplateActor)==0xd8);
        static_assert(offsetof(SDK::UStaticMeshComponent,StaticMesh)==0x588);
        static_assert(offsetof(SDK::UMeshComponent,OverrideMaterials)==0x530);
        static_assert(offsetof(SDK::UStaticMesh,StaticMaterials)==0x160);
        static_assert(offsetof(SDK::USkeletalMesh,Materials)==0x1a0);
        static_assert(sizeof(SDK::FStaticMaterial)==0x38&&sizeof(SDK::FSkeletalMaterial)==0x30);
        static_assert(sizeof(SDK::FMassStaticMeshInstanceVisualizationMeshDesc)==0xa0);
        static_assert(offsetof(SDK::FMassStaticMeshInstanceVisualizationMeshDesc,Mesh)==0x60);
        static_assert(offsetof(SDK::FMassStaticMeshInstanceVisualizationMeshDesc,MaterialOverrides)==0x68);
        static_assert(sizeof(SDK::FTransform)==0x60);

        // Targeted indexed identity check, not a global object scan or weak-pointer
        // resolution. A candidate always comes from a retained ownership chain.
        bool Identity(uintptr_t p,size_t extent=0x28)
        {
            return C::ValidateObjectIdentity(p,objectArray,extent,
                [](uintptr_t base,size_t offset,auto& out){return Read(base,offset,out);},&Readable);
        }
        struct Context
        {
            uintptr_t AuditClass=0;
            bool AuditActive=false,AuditTruncated=false,AuditCollectionTruncated=false;
            size_t AuditVisits=0;
            std::vector<uintptr_t> AuditNodes;
            std::map<std::string,std::string> AuditParents;
            std::set<std::string> AuditConflictingParents;
            std::map<uintptr_t,uintptr_t> AuditAssignments;
            struct AuditMassRecord {std::string Mesh,Descriptor,Local,Materials,Min,Max;size_t Ordinal;};
            std::vector<AuditMassRecord> AuditMassRecords;
            std::map<std::pair<std::string,size_t>,std::string> AuditMassOffsets;
            C::Graph Graph;
            S::WorkBudget Work;
            S::Plan SelectionPlan;
            S::Decision Decision;
            C::JsonArray Attempts;
            bool FullTraversal=false,SelectionTraversalTruncated=false;
            size_t SelectionTraitVisits=0,SelectionMeshVisits=0,SelectionGraphVisits=0;
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
            bool Truncated=false,Invalid=false;
            size_t NodeCount=0;
            uint32_t BuildingId=0;
            std::string Selected="null",SelectedTier="none",Status="unavailable",Reason="no safe resident representation";
            MiniMapBuildingCollector::Record Record{};
            Context()
            {
                Components.Limit=512*1024;Mass.Limit=128*1024;Candidates.Limit=32*1024;
                Sources.Limit=128*1024;Relationships.Limit=64*1024;
            }
            bool CheckIdentity(uintptr_t p,size_t extent=0x28)
            {
                if(AuditActive&&(!AuditSpend()))return false;
                return Work.Spend(FullTraversal)&&Identity(p,extent);
            }
            bool AuditSpend()
            {if(AuditVisits>=A::VisitLimit){AuditTruncated=true;return false;}++AuditVisits;return true;}
            ~Context()
            {
                const auto address=MiniMapSmelterCapture::GetBindings().Release;
                for(auto it=Roots.rbegin();it!=Roots.rend();++it)
                    reinterpret_cast<void(*)(SDK::UObject*)>(address)(reinterpret_cast<SDK::UObject*>(*it));
            }
            bool Hold(uintptr_t p,size_t extent=0x28)
            {
                if(!CheckIdentity(p,extent))return false;
                if(Held.insert(p).second)
                {
                    Roots.push_back(p);
                    reinterpret_cast<void(*)(SDK::UObject*)>(MiniMapSmelterCapture::GetBindings().Retain)(reinterpret_cast<SDK::UObject*>(p));
                }
                return true;
            }
            uintptr_t Find(const wchar_t* path)
            {
                using Fn=SDK::UObject*(*)(SDK::UClass*,SDK::UObject*,const wchar_t*,bool);
                const auto p=reinterpret_cast<uintptr_t>(reinterpret_cast<Fn>(engine->GetStaticFindObjectByNameAddress())(nullptr,nullptr,path,false));
                return Hold(p)?p:0;
            }
            bool ClassObject(uintptr_t p)
            {
                if(!CheckIdentity(p,0x200))return false;
                auto meta=Value<uintptr_t>(p,0x10);
                for(size_t n=0;meta&&n<C::DepthLimit;++n)
                {
                    if(!CheckIdentity(meta,0x200))return false;
                    if(meta==Class)return true;
                    if(Value<uintptr_t>(meta,0x10)!=Class)return false;
                    meta=Value<uintptr_t>(meta,0x40);
                }
                return false;
            }
            bool Derives(uintptr_t type,uintptr_t expected)
            {
                if(!expected)return false;
                std::unordered_set<uintptr_t> chain;
                for(size_t n=0;type&&n<C::DepthLimit;++n)
                {
                    if(!ClassObject(type)||!chain.insert(type).second)return false;
                    if(type==expected)return true;
                    type=Value<uintptr_t>(type,0x40);
                }
                if(type)Truncated=true;
                return false;
            }
            bool Is(uintptr_t p,uintptr_t type,size_t extent=0x28)
            {return CheckIdentity(p,extent)&&Derives(Value<uintptr_t>(p,0x10),type);}
            bool BoundedNativeClassChain(uintptr_t type)
            {
                // Fixed getter's IsA uses FStructBaseChain at UStruct +0x30.
                // PDB: pointer +0, last-index +8; validate before native indexing.
                if(!ClassObject(type))return false;
                const auto last=Value<int32_t>(type,0x38);
                const auto data=Value<uintptr_t>(type,0x30);
                if(last<0||size_t(last)>=C::DepthLimit||!Readable(data,(size_t(last)+1)*8,8))return false;
                uintptr_t previous=0;
                for(int32_t i=0;i<=last;++i)
                {
                    if(!Graph.Spend())return false;
                    const auto subobject=Value<uintptr_t>(data,size_t(i)*8);
                    if(subobject<0x30)return false;
                    const auto owner=subobject-0x30;
                    if(!ClassObject(owner)||!Hold(owner,0x200)||Value<uintptr_t>(owner,0x40)!=previous)return false;
                    previous=owner;
                }
                return previous==type;
            }
            uintptr_t NativeClass(const wchar_t* path)
            {
                const auto p=Find(path);
                return p&&ClassObject(p)&&Value<uintptr_t>(p,0x10)==Class?p:0;
            }
            bool Initialize()
            {
                Class=Find(L"/Script/CoreUObject.Class");
                if(!Class||!CheckIdentity(Class,0x200)||Value<uintptr_t>(Class,0x10)!=Class)return false;
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
                const auto type=Value<uintptr_t>(p,0x10);
                if(!ClassObject(type)||!Hold(type,0x200))
                {Invalid|=!Work.Exhausted;return C::Object({{"status",C::Quote(Work.Exhausted?"truncated":"invalid_class")}});}
                const auto name=Value<Name>(p,0x18);
                return C::Object({{"id",C::Quote(Id(p))},{"class_id",C::Quote(Id(type))},
                    {"status",C::Quote("present")},{"resident_validated","true"},{"readable_name", "null"},
                    {"name_status",C::Quote("not_attempted_native_conversion_not_verified")},
                    {"fname_comparison_index",std::to_string(name.Index)},{"fname_number",std::to_string(name.Number)}});
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
            template<class Visitor> void Array(uintptr_t header,size_t stride,size_t alignment,size_t limit,
                const std::string& source,uintptr_t owner,Visitor visitor)
            {
                C::ArrayHeader a{};
                if(!Readable(header,sizeof(a),8)||!Read(header,0,a)){Source(source,owner,"invalid","unreadable/misaligned array header");return;}
                const auto b=C::BoundArray(a,stride,alignment,limit);
                if(!b.Valid||!(!b.Bytes||Readable(a.Data,b.Bytes,alignment)))
                {Source(source,owner,"invalid","array bounds, alignment, arithmetic or readable storage check failed");return;}
                Source(source,owner,b.Status,"num="+std::to_string(a.Num)+" max="+std::to_string(a.Max)+" bounded_count="+std::to_string(b.Count));
                for(size_t i=0;i<b.Count;++i)
                {
                    if(!Graph.Spend()){Source(source,owner,"truncated","total traversal visit limit");break;}
                    visitor(a.Data+i*stride,i);
                }
            }
            std::string NameValue(uintptr_t p,size_t offset)
            {
                const auto n=Value<Name>(p,offset);
                return C::Object({{"comparison_index",std::to_string(n.Index)},{"number",std::to_string(n.Number)},
                    {"readable_name","null"},{"status",C::Quote("opaque_fname")}});
            }
            std::string Vector(uintptr_t p,size_t offset,size_t count)
            {
                C::JsonArray a;
                for(size_t i=0;i<count;++i)
                {
                    const double v=Value<double>(p,offset+i*8);
                    if(!std::isfinite(v))Invalid=true;
                    a.Add(C::Number(v));
                }
                return a.String();
            }
            std::string Transform(uintptr_t p)
            {
                return C::Object({{"rotation_xyzw",Vector(p,0,4)},{"translation",Vector(p,0x20,3)},
                    {"scale",Vector(p,0x40,3)},{"composed","false"}});
            }
            template<size_t N> std::string CopiedNumbers(const std::array<double,N>& values)
            {
                C::JsonArray a;
                for(double v:values){if(!std::isfinite(v))Invalid=true;a.Add(C::Number(v));}
                return a.String();
            }
            std::string Materials(uintptr_t header,size_t stride,uintptr_t owner,const std::string& source)
            {
                C::JsonArray slots;slots.Limit=16*1024;
                if(!Material){Source(source,owner,"not_attempted","native MaterialInterface class unavailable");return slots.String();}
                Array(header,stride,8,C::MaterialLimit,source,owner,[&](uintptr_t item,size_t slot)
                {
                    const auto material=Value<uintptr_t>(item,0);
                    slots.Add(C::Object({{"slot",std::to_string(slot)},{"material",Ref(material,Material)}}));
                });
                if(slots.Truncated)Truncated=true;
                return slots.String();
            }
            void Edge(uintptr_t from,uintptr_t to,const std::string& kind,const char* status="present")
            {
                if(BuildingId==336&&kind=="scs_child")
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
                if(!Is(p,Component,0xb8)||!Hold(p,0xb8)){Source(source,owner,"invalid","component identity/type");return;}
                Edge(owner,p,source);
                if(ComponentSet.contains(p))return;
                if(ComponentSet.size()>=C::ComponentLimit){Source(source,owner,"truncated","component record limit");return;}
                ComponentSet.insert(p);
                const auto type=Value<uintptr_t>(p,0x10);
                std::string scene="null",mesh="null",defaults="[]",overrides="[]",meshKind="none",
                    meshInputs="null",meshStatus="not_applicable";
                if(Is(p,Scene,0x250))
                {
                    const auto parent=Value<uintptr_t>(p,0xc8);
                    const auto parentRef=Ref(parent,Scene);
                    if(parent&&Is(parent,Scene))Edge(p,parent,"scene_attachment_parent");
                    const auto flags=Value<uint8_t>(p,0x1a0),flags2=Value<uint8_t>(p,0x1a1);
                    scene=C::Object({{"location",Vector(p,0x140,3)},{"rotation_pitch_yaw_roll",Vector(p,0x158,3)},
                        {"scale",Vector(p,0x170,3)},{"parent",parentRef},{"socket",NameValue(p,0xd0)},
                        {"absolute_location",C::Bool(flags&4)},{"absolute_rotation",C::Bool(flags&8)},
                        {"absolute_scale",C::Bool(flags&16)},{"visible",C::Bool(flags&32)},
                        {"hidden_in_game",C::Bool(flags2&8)}});
                }
                uintptr_t asset=0;
                if(Is(p,SkinnedComponent,0x598))
                {
                    meshInputs=C::Object({{"legacy_skeletal_mesh",Ref(Value<uintptr_t>(p,0x588),SkeletalMesh)},
                        {"skinned_asset",Ref(Value<uintptr_t>(p,0x590),SkinnedAsset)},
                        {"provenance",C::Quote("verified resident USkinnedMeshComponent assignment fields; not evaluated as additive geometry")}});
                }
                if(Is(p,StaticComponent,0x590))
                {
                    meshKind="static";asset=Value<uintptr_t>(p,0x588);mesh=Ref(asset,StaticMesh);
                    meshStatus=!StaticMesh?"not_attempted":!asset?"absent":Is(asset,StaticMesh)?"present":"invalid";
                    Source("static_mesh_assignment",p,meshStatus.c_str());
                    if(Is(asset,StaticMesh,0x170))defaults=Materials(asset+0x160,0x38,asset,"static_mesh_authored_materials");
                }
                else if(Is(p,SkeletalComponent,0x598))
                {
                    meshKind="skeletal";meshStatus="not_attempted";
                    // Validate BOTH references read by the installed fixed getter
                    // before calling it. Native SkeletalMesh class is already resident.
                    const auto legacy=Value<uintptr_t>(p,0x588),skinned=Value<uintptr_t>(p,0x590);
                    if(!skeletalGetter||!SkeletalMesh||!SkinnedAsset)
                        Source("skeletal_mesh_assignment",p,"not_attempted","fixed getter/native asset classes unavailable");
                    else if((legacy&&!Is(legacy,SkeletalMesh))||(skinned&&!Is(skinned,SkinnedAsset)))
                        Source("skeletal_mesh_assignment",p,"invalid","getter input reference identity/type");
                    else if(!BoundedNativeClassChain(SkeletalMesh)||
                        (legacy&&!BoundedNativeClassChain(Value<uintptr_t>(legacy,0x10)))||
                        (skinned&&!BoundedNativeClassChain(Value<uintptr_t>(skinned,0x10))))
                        Source("skeletal_mesh_assignment",p,Graph.Truncated?"truncated":"invalid","native getter class-chain preflight failed");
                    else
                    {
                        if((legacy&&!Hold(legacy))||(skinned&&!Hold(skinned)))
                        {Source("skeletal_mesh_assignment",p,"invalid","getter inputs could not be retained");return;}
                        using Getter=SDK::USkeletalMesh*(*)(const SDK::USkeletalMeshComponent*);
                        asset=reinterpret_cast<uintptr_t>(reinterpret_cast<Getter>(skeletalGetter)(reinterpret_cast<const SDK::USkeletalMeshComponent*>(p)));
                        mesh=Ref(asset,SkeletalMesh);
                        meshStatus=!asset?"absent_as_skeletal_mesh":Is(asset,SkeletalMesh)?"present":"invalid";
                        Source("skeletal_mesh_assignment",p,asset?meshStatus.c_str():"absent","fixed getter result; raw skinned assignment fields recorded separately");
                        if(Is(asset,SkeletalMesh,0x1b0))defaults=Materials(asset+0x1a0,0x30,asset,"skeletal_mesh_authored_materials");
                    }
                }
                else if(Is(p,SkinnedComponent))
                {meshKind="skinned";meshStatus="not_attempted";Source("effective_skinned_mesh_assignment",p,"not_attempted","raw assignment inputs recorded; non-skeletal effective getter not verified for this stage");}
                if(Is(p,MeshComponent,0x540))overrides=Materials(p+0x530,8,p,"component_authored_material_overrides");
                Components.Add(C::Object({{"component",Ref(p,Component)},{"source",C::Quote(source)},
                    {"owner",Ref(owner)},{"component_class",Ref(type,Class)},{"registered",C::Bool(Value<uint8_t>(p,0x94)&1)},
                    {"object_outer",Ref(Value<uintptr_t>(p,0x20))},
                    {"scene",scene},{"mesh_kind",C::Quote(meshKind)},{"mesh",mesh},
                    {"mesh_assignment_status",C::Quote(meshStatus)},{"mesh_assignment_inputs",meshInputs},
                    {"authored_mesh_materials",defaults},{"authored_component_overrides",overrides},
                    {"effective_materials_status",C::Quote("not_attempted_unbounded_array_getter_and_virtual_resolution_not_verified")},
                    {"live_instance_state","false"}}));
                // A component may first be encountered in ComponentTemplates;
                // keep its already validated assignment for the later SCS join.
                if(BuildingId==336&&owner==AuditClass&&asset&&meshStatus=="present")
                {
                    if(AuditAssignments.size()<A::NodeLimit)AuditAssignments.emplace(p,asset);
                    else AuditCollectionTruncated=true;
                }
                if(Is(p,Child,0x268))
                {
                    const auto childClass=Value<uintptr_t>(p,0x250),childTemplate=Value<uintptr_t>(p,0x260);
                    const bool validClass=childClass&&ClassObject(childClass)&&Derives(childClass,Actor);
                    const bool validTemplate=childTemplate&&Is(childTemplate,Actor);
                    Source("child_actor_class",p,!childClass?"absent":validClass?"present":"invalid","",Ref(childClass,Class));
                    Source("child_actor_template",p,!childTemplate?"absent":validTemplate?"present":"invalid","",Ref(childTemplate,Actor));
                    if(childTemplate&&Is(childTemplate,Actor)&&childClass&&ClassObject(childClass)&&
                        Derives(Value<uintptr_t>(childTemplate,0x10),childClass))
                    {Hold(childTemplate);Edge(p,childTemplate,"resident_child_actor_template");}
                    else if(childTemplate)Source("child_actor_template_relationship",p,"invalid","class/template mismatch");
                    if(childClass&&ClassObject(childClass)&&Derives(childClass,Actor))
                    {
                        Hold(childClass,0x200);Edge(p,childClass,"child_actor_representation_class");
                        if(childDepth>=C::ChildDepthLimit)Source("child_actor_class_templates",p,"truncated","child-template recursion depth");
                        else InspectClass(childClass,childDepth+1,"child_actor_class_templates");
                    }
                    Source("child_actor_native_components",p,"not_attempted",NativeExcluded);
                }
            }
            void InspectNode(uintptr_t p,uintptr_t owner,size_t depth,size_t childDepth)
            {
                if(!Is(p,Node,0xd8)||!Hold(p,0xd8)){Source("scs_node",owner,"invalid","node identity/type");return;}
                const auto state=Graph.Enter(p,depth);
                if(std::strcmp(state,"present")!=0){Source("scs_node",p,state);return;}
                if(NodeCount>=C::NodeLimit){Source("scs_node",p,"truncated","total unique SCS node limit");Graph.Leave(p);return;}
                ++NodeCount;
                if(BuildingId==336&&owner==AuditClass&&childDepth==0)
                {if(AuditNodes.size()<A::NodeLimit)AuditNodes.push_back(p);else AuditCollectionTruncated=true;}
                const auto declared=Value<uintptr_t>(p,0x28),component=Value<uintptr_t>(p,0x30);
                Source("scs_node",p,"present","",C::Object({{"node",Ref(p,Node)},{"declared_class",Ref(declared,Class)},
                    {"attach_to",NameValue(p,0x80)},{"parent_variable",NameValue(p,0x88)},
                    {"parent_owner_class",NameValue(p,0x90)},{"parent_is_native",C::Bool(Value<uint8_t>(p,0x98)!=0)},
                    {"variable_name",NameValue(p,0xd0)},{"variable_guid_words",Guid(p,0xc0)}}));
                if(!component)Source("scs_component_template",p,"absent");
                else if(!ClassObject(declared)||!Derives(declared,Component)||!Is(component,declared))
                    Source("scs_component_template",p,"invalid","declared component class mismatch");
                else {Edge(p,component,"scs_template");InspectComponent(component,owner,"scs_component_template",childDepth);}
                Array(p+0xa0,8,8,C::NodeLimit,"scs_children",p,[&](uintptr_t item,size_t)
                {
                    const auto node=Value<uintptr_t>(item,0);
                    if(Is(node,Node)){Hold(node);Edge(p,node,"scs_child");}
                    InspectNode(node,owner,depth+1,childDepth);
                });
                Graph.Leave(p);
            }
            std::string Guid(uintptr_t p,size_t offset)
            {
                C::JsonArray a;for(size_t i=0;i<4;++i)a.Add(std::to_string(Value<uint32_t>(p,offset+i*4)));return a.String();
            }
            void InspectClass(uintptr_t initial,size_t childDepth,const std::string& source)
            {
                if(!ClassObject(initial)||!Derives(initial,Actor)||!Hold(initial,0x200))
                {Source(source,initial,"invalid","actor representation class identity/type");return;}
                const auto entry=Graph.Enter(initial,childDepth,C::ChildDepthLimit);
                if(std::strcmp(entry,"present")!=0){Source(source,initial,entry);return;}
                std::unordered_set<uintptr_t> supers;
                auto type=initial;
                for(size_t depth=0;type&&depth<C::DepthLimit;++depth)
                {
                    if(!Graph.Spend()){Source("class_hierarchy",initial,"truncated","total visit limit");break;}
                    if(!ClassObject(type)||!Hold(type,0x200)){Source("class_hierarchy",initial,"invalid");break;}
                    if(!supers.insert(type).second){Source("class_hierarchy",type,"cycle");break;}
                    const auto cdo=Value<uintptr_t>(type,0x110);
                    const bool validCdo=cdo&&Is(cdo,type)&&(Value<uint32_t>(cdo,8)&0x10);
                    Source("existing_cdo",type,!cdo?"unavailable_not_resident":validCdo?"present":"invalid","",Ref(cdo,type));
                    Source("native_default_components",type,"not_attempted",NativeExcluded);
                    if(Is(type,Blueprint,0x360))
                    {
                        Array(type+0x220,8,8,C::ComponentLimit,"blueprint_component_templates",type,[&](uintptr_t item,size_t)
                            {InspectComponent(Value<uintptr_t>(item,0),type,"blueprint_component_templates",childDepth);});
                        const auto scs=Value<uintptr_t>(type,0x268);
                        if(!scs)Source("blueprint_scs",type,"absent");
                        else if(!Is(scs,SCS,0xb0)||!Hold(scs,0xb0))Source("blueprint_scs",type,"invalid");
                        else
                        {
                            Source("blueprint_scs",type,"present","",Ref(scs,SCS));
                            Array(scs+0x28,8,8,C::NodeLimit,"scs_root_nodes",scs,[&](uintptr_t item,size_t)
                                {InspectNode(Value<uintptr_t>(item,0),type,0,childDepth);});
                            Array(scs+0x38,8,8,C::NodeLimit,"scs_all_nodes",scs,[&](uintptr_t item,size_t)
                                {InspectNode(Value<uintptr_t>(item,0),type,0,childDepth);});
                        }
                        const auto handler=Value<uintptr_t>(type,0x270);
                        if(!handler)Source("inherited_override_records",type,"absent");
                        else if(!Is(handler,Handler,0x48)||!Hold(handler,0x48))Source("inherited_override_records",type,"invalid");
                        else Array(handler+0x28,0x78,8,C::ComponentLimit,"inherited_override_records",handler,[&](uintptr_t record,size_t ordinal)
                        {
                            const auto declared=Value<uintptr_t>(record,0),component=Value<uintptr_t>(record,8),keyOwner=Value<uintptr_t>(record,0x10);
                            Source("inherited_override_key",handler,"present","",C::Object({{"ordinal",std::to_string(ordinal)},
                                {"owner_class",Ref(keyOwner,Class)},{"variable_name",NameValue(record,0x18)},
                                {"declared_component_class",Ref(declared,Class)},{"component_template",Ref(component,Component)},
                                {"associated_guid_words",Guid(record,0x20)},{"effective_selection",C::Quote("not_evaluated")}}));
                            if(!component)Source("inherited_override_template",handler,"absent");
                            else if(!ClassObject(declared)||!Derives(declared,Component)||!Is(component,declared))
                                Source("inherited_override_template",handler,"invalid","declared class mismatch");
                            else InspectComponent(component,type,"inherited_override_template",childDepth);
                        });
                        Source("native_inherited_template_resolver",type,"not_attempted","internal class/record traversal has no diagnostic limit; raw override records kept distinct");
                    }
                    type=Value<uintptr_t>(type,0x40);
                    if(type&&depth+1==C::DepthLimit)Source("class_hierarchy",initial,"truncated","superclass depth limit");
                }
                Graph.Leave(initial);
            }
            bool InspectMass(uintptr_t trait)
            {
                const auto descriptor=trait+0x30;
                C::JsonArray meshes,offsets;meshes.Limit=96*1024;
                bool validMesh=false;
                Array(descriptor+8,0xa0,16,64,"mass_mesh_descriptors",trait,[&](uintptr_t item,size_t ordinal)
                {
                    const auto mesh=Value<uintptr_t>(item,0x60);const bool valid=Is(mesh,StaticMesh);validMesh|=valid;
                    const auto ref=Ref(mesh,StaticMesh),local=Transform(item),materials=Materials(item+0x68,8,trait,"mass_mesh_material_overrides"),
                        min=C::Number(Value<float>(item,0x78)),max=C::Number(Value<float>(item,0x7c));
                    meshes.Add(C::Object({{"ordinal",std::to_string(ordinal)},{"mesh",ref},{"local_transform",local},
                        {"material_overrides",materials},{"min_lod_significance",min},{"max_lod_significance",max}}));
                    if(BuildingId==336)
                    {
                        if(AuditMassRecords.size()<64)AuditMassRecords.push_back({valid?Id(mesh):"",Id(trait)+"_ism",local,materials,min,max,ordinal});
                        else AuditCollectionTruncated=true;
                    }
                });
                Array(descriptor+0x80,0x60,16,64,"mass_raw_transform_offsets",trait,[&](uintptr_t item,size_t ordinal)
                    {const auto transform=Transform(item);offsets.Add(C::Object({{"ordinal",std::to_string(ordinal)},{"transform",transform}}));
                     if(BuildingId==336){if(AuditMassOffsets.size()<64)AuditMassOffsets.emplace(std::pair{Id(trait)+"_ism",ordinal},transform);
                        else AuditCollectionTruncated=true;}});
                Mass.Add(C::Object({{"descriptor_id",C::Quote(Id(trait)+"_ism")},{"owner_trait",Ref(trait)},
                    {"tier",C::Quote("mass_ism_alternative")},{"mesh_references",meshes.String()},
                    {"use_transform_offset",C::Bool(Value<uint8_t>(descriptor,0x18)!=0)},
                    {"descriptor_transform_offset",Transform(descriptor+0x20)},{"raw_transform_offsets",offsets.String()},
                    {"renderer_equivalent_transforms","false"},{"provenance",C::Quote("resident visualization trait; raw inputs only")}}));
                Truncated|=meshes.Truncated||offsets.Truncated;
                return validMesh;
            }
            template<class Visitor> bool ProbeArray(uintptr_t header,size_t stride,size_t alignment,size_t limit,Visitor visitor)
            {
                C::ArrayHeader a{};
                if(!Readable(header,sizeof(a),8)||!Read(header,0,a))
                {ProbeResult.Incomplete=true;ProbeResult.Note("invalid_array_header");return false;}
                const auto b=C::BoundArray(a,stride,alignment,limit);
                if(!b.Valid||(b.Bytes&&!Readable(a.Data,b.Bytes,alignment)))
                {ProbeResult.Incomplete=true;ProbeResult.Note("invalid_array_storage");return false;}
                if(b.Truncated){ProbeResult.Truncated=true;ProbeResult.Note("array_visit_limit");}
                for(size_t i=0;i<b.Count;++i)
                {
                    if(!Graph.Spend()||Work.Exhausted){ProbeResult.Truncated=true;ProbeResult.Note("work_budget_exhausted");return false;}
                    visitor(a.Data+i*stride);
                }
                return !b.Truncated&&!Work.Exhausted;
            }
            bool ProbeDefinition(uintptr_t definition)
            {
                ProbeResult.MassIdentity="valid"; // Collector invokes visitor only after entity revalidation.
                if(!definition){ProbeResult.DefinitionIdentity="null";ProbeResult.Note("placement_null");return false;}
                if(!Is(definition,Placement,0x174)||!Hold(definition,0x174))
                {ProbeResult.DefinitionIdentity=Work.Exhausted?"not_examined_budget_exhausted":"invalid";
                    ProbeResult.Note(Work.Exhausted?"work_budget_exhausted":"invalid_placement");return false;}
                const auto name=Value<Name>(definition,0x18);
                if(Value<int32_t>(definition,0xc)!=Record.DefinitionObjectIndex||name.Index!=Record.DefinitionIndex||
                    name.Number!=Record.DefinitionNumber||Value<uint32_t>(definition,0x170)!=Record.BuildingId)
                {ProbeResult.DefinitionIdentity="mismatch";ProbeResult.Note("definition_identity_mismatch");return false;}
                ProbeResult.DefinitionIdentity="valid";
                // Optional soft-reference interpretation/resolution is deliberately
                // omitted. Null transient cache never implies authored absence.
                auto config=Value<uintptr_t>(definition,0x150);
                ProbeResult.CachedConfig=config?"populated":"null";
                if(!config){ProbeResult.Note("cached_config_null");return false;}
                std::unordered_set<uintptr_t> configs,traits;
                for(size_t depth=0;config&&depth<16;++depth)
                {
                    if(!Graph.Spend()||Work.Exhausted){ProbeResult.Truncated=true;ProbeResult.Note("work_budget_exhausted");break;}
                    if(!Is(config,Config,0x60)||!Hold(config,0x60))
                    {ProbeResult.ConfigValidation=Work.Exhausted?"not_examined_budget_exhausted":"invalid";ProbeResult.Incomplete=true;
                        ProbeResult.Note(Work.Exhausted?"work_budget_exhausted":"invalid_config");break;}
                    ProbeResult.ConfigValidation="valid_in_examined_chain";
                    if(!configs.insert(config).second){ProbeResult.Incomplete=true;ProbeResult.Note("configuration_cycle");break;}
                    ProbeArray(config+0x38,8,8,256,[&](uintptr_t item)
                    {
                        ++ProbeResult.Traits;const auto trait=Value<uintptr_t>(item,0);
                        if(!Is(trait,Trait)||!Hold(trait)){ProbeResult.Incomplete=true;
                            ProbeResult.Note(Work.Exhausted?"work_budget_exhausted":"invalid_trait");return;}
                        if(!traits.insert(trait).second)return;
                        if(!Is(trait,Visual,0xe0)&&!Is(trait,Cosmetic,0xe0))return;
                        ++ProbeResult.VisualTraits;
                        for(const auto offset:{size_t(0xd0),size_t(0xd8)})
                        {
                            const auto type=Value<uintptr_t>(trait,offset);
                            const bool valid=type&&ClassObject(type)&&Derives(type,Actor)&&Hold(type,0x200);
                            auto& state=offset==0xd0?ProbeResult.HighStatus:ProbeResult.LowStatus;
                            auto& first=offset==0xd0?ProbeResult.High:ProbeResult.Low;
                            if(valid){state="resident_validated";if(!first)first=type;}
                            else if(state!="resident_validated")state=Work.Exhausted?"not_examined_budget_exhausted":!type?"null":"invalid_or_unavailable";
                        }
                        // Eligibility reads assignments only: no transforms,
                        // materials, SCS, CDOs or component traversal in a probe.
                        const bool massComplete=ProbeArray(trait+0x38,0xa0,16,64,[&](uintptr_t meshItem)
                        {
                            ++ProbeResult.MeshReferences;const auto mesh=Value<uintptr_t>(meshItem,0x60);
                            if(Is(mesh,StaticMesh)&&Hold(mesh))ProbeResult.Mesh=true;
                        });
                        if(!massComplete)ProbeResult.MassStatus="unavailable_or_truncated";
                        else if(ProbeResult.MassStatus!="unavailable_or_truncated")ProbeResult.MassStatus="examined";
                    });
                    config=Value<uintptr_t>(config,0x30);
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
            std::string ProbeJson(const S::Record& record,size_t attempt)const
            {
                C::JsonArray reasons;for(const auto& reason:ProbeResult.Reasons)reasons.Add(C::Quote(reason));
                return C::Object({{"attempt",std::to_string(attempt)},{"building_id",std::to_string(record.Building)},
                    {"definition_identity",C::Object({{"object_index",std::to_string(record.Def.ObjectIndex)},
                        {"fname_comparison_index",std::to_string(record.Def.NameIndex)},{"fname_number",std::to_string(record.Def.NameNumber)}})},
                    {"entity_index",std::to_string(record.Index)},{"entity_serial",std::to_string(record.Serial)},
                    {"mass_identity",C::Quote(ProbeResult.MassIdentity)},{"definition_validation",C::Quote(ProbeResult.DefinitionIdentity)},
                    {"cached_entity_config",C::Quote(ProbeResult.CachedConfig)},
                    {"configuration_validation",C::Quote(ProbeResult.ConfigValidation)},
                    {"authored_entity_config",S::UnavailableReference("soft-reference state/read contract not verified; no weak resolution or loading")},
                    {"placement_actor_class",S::UnavailableReference("resident soft-class access contract not verified")},
                    {"traits_examined",std::to_string(ProbeResult.Traits)},{"visualization_traits",std::to_string(ProbeResult.VisualTraits)},
                    {"high_detail_class",C::Quote(ProbeResult.HighStatus)},{"low_detail_class",C::Quote(ProbeResult.LowStatus)},
                    {"mass_metadata",C::Quote(ProbeResult.MassStatus)},{"mesh_references_examined",std::to_string(ProbeResult.MeshReferences)},
                    {"valid_mass_mesh",ProbeResult.Mesh?"true":ProbeResult.MassStatus=="examined"?"false":"null"},
                    {"eligible",C::Bool(ProbeResult.Route!=S::Route::None)},
                    {"eligible_route",C::Quote(S::RouteName(ProbeResult.Route))},{"truncated",C::Bool(ProbeResult.Truncated)},
                    {"reasons",reasons.String()}});
            }
            bool Discover(uintptr_t definition)
            {
                if(!Is(definition,Placement,0x174)||!Hold(definition,0x174))return false;
                const auto name=Value<Name>(definition,0x18);
                if(Value<int32_t>(definition,0xc)!=Record.DefinitionObjectIndex||name.Index!=Record.DefinitionIndex||
                    name.Number!=Record.DefinitionNumber||Value<uint32_t>(definition,0x170)!=Record.BuildingId)return false;
                BuildingId=Record.BuildingId;
                Source("mass_building_definition",definition,"present","",Ref(definition,Placement));
                uintptr_t high=0,low=0;bool hasMass=false;
                auto config=Value<uintptr_t>(definition,0x150);
                std::unordered_set<uintptr_t> configs;
                for(size_t depth=0;config&&depth<16;++depth)
                {
                    if(!Graph.Spend()){Source("entity_configuration",definition,"truncated","total visit limit");break;}
                    if(!Is(config,Config,0x60)||!Hold(config,0x60)){Source("entity_configuration",definition,"invalid");break;}
                    if(!configs.insert(config).second){Source("entity_configuration",config,"cycle");break;}
                    Source("entity_configuration",config,"present","",Ref(config,Config));
                    Array(config+0x38,8,8,256,"configuration_traits",config,[&](uintptr_t item,size_t)
                    {
                        const auto trait=Value<uintptr_t>(item,0);
                        if(!Is(trait,Trait)||!Hold(trait)){Source("configuration_trait",config,"invalid");return;}
                        Edge(config,trait,"configuration_trait");
                        if(!Is(trait,Visual,0xe0)&&!Is(trait,Cosmetic,0xe0))return;
                        for(const auto& entry:{std::pair{size_t(0xd0),"high_detail_actor"},std::pair{size_t(0xd8),"low_detail_actor"}})
                        {
                            const auto candidate=Value<uintptr_t>(trait,entry.first);
                            const bool valid=candidate&&ClassObject(candidate)&&Derives(candidate,Actor)&&Hold(candidate,0x200);
                            Candidates.Add(C::Object({{"tier",C::Quote(entry.second)},{"trait",Ref(trait)},
                                {"class",Ref(candidate,Class)},{"status",C::Quote(!candidate?"absent":valid?"present":"invalid")},
                                {"additive_geometry","false"}}));
                            if(valid){if(entry.first==0xd0&&!high)high=candidate;if(entry.first==0xd8&&!low)low=candidate;}
                        }
                        hasMass|=InspectMass(trait);
                    });
                    config=Value<uintptr_t>(config,0x30);
                    if(config&&depth==15)Source("entity_configuration",definition,"truncated","configuration parent depth");
                }
                if(!Value<uintptr_t>(definition,0x150))Source("entity_configuration",definition,"unavailable","cached_config_null; authored configuration state not examined");
                const auto selected=high?high:low;
                if(selected)
                {
                    Selected=Ref(selected,Class);SelectedTier=high?"high_detail_actor":"low_detail_actor";
                    Reason="first validated resident actor class in bounded configuration traversal; high-detail preferred; alternatives remain separate";
                    AuditClass=selected;InspectClass(selected,0,"selected_actor_class_templates");Status="partial";
                }
                else if(hasMass)
                {Selected=Ref(definition,Placement);SelectedTier="mass_ism";Status="partial";Reason="no validated resident actor class; resident Mass metadata recorded separately";}
                return selected||hasMass;
            }
            #include "AssemblyRecipeAudit.inl"
            std::string Document()
            {
                const bool truncated=FullTraversal&&(Truncated||Graph.Truncated||Work.Exhausted||Sources.Truncated||Components.Truncated||Candidates.Truncated||Mass.Truncated||Relationships.Truncated);
                return S::BoundDocument(C::Object({{"schema_version","2"},{"game_build",C::Quote("5.6.1-127004")},
                    {"stage",C::Quote("R2.4b")},{"building_id",Decision.HasSelection()?std::to_string(BuildingId):"null"},
                    {"selection",S::CoverageJson(SelectionPlan,Decision)},{"candidate_attempts",Attempts.String()},
                    {"selection_truncation",S::TruncationJson(SelectionPlan,Decision,SelectionTraversalTruncated,Attempts.Truncated)},
                    {"representation",C::Object({{"attempted",C::Bool(FullTraversal)},
                        {"status",C::Quote(truncated?"truncated":Status)},
                        {"observed_active_tier","null"},{"complete_visual_building","false"}})},
                    {"representation_truncation",C::Object({{"truncated",C::Bool(truncated)},
                        {"native_work_limit",C::Bool(FullTraversal&&Work.Exhausted)}})},
                    {"native_work",C::Object({{"selection_object_validation_visits",std::to_string(Work.SelectionVisits)},
                        {"representation_object_validation_visits",std::to_string(Work.RepresentationVisits)},
                        {"selection_trait_visits",std::to_string(SelectionTraitVisits)},
                        {"selection_mesh_assignment_visits",std::to_string(SelectionMeshVisits)},
                        {"selection_graph_visits",std::to_string(SelectionGraphVisits)},
                        {"representation_graph_visits",std::to_string(C::VisitLimit-Graph.Remaining-SelectionGraphVisits)},
                        {"remaining",std::to_string(Work.Remaining)},
                        {"graph_visits",std::to_string(C::VisitLimit-Graph.Remaining)}})},
                    {"building_transform_input",Decision.HasSelection()?C::Object({{"valid",C::Bool(Record.TransformValid)},
                        {"position",CopiedNumbers(Record.Position)},{"rotation_xyzw",CopiedNumbers(Record.Rotation)},
                        {"scale",CopiedNumbers(Record.Scale)},{"refreshed_at_steady_seconds",C::Number(Record.RefreshedAtSeconds)},
                        {"provenance",C::Quote("copied collector record; not refreshed or composed by diagnostic")}}):"null"},
                    {"status",C::Quote(truncated?"truncated":Status)},{"invalid_metadata_seen",C::Bool(Invalid)},
                    {"selected_representation_type",C::Quote(SelectedTier)},{"selected",Selected},{"selection_reason",C::Quote(Reason)},
                    {"runtime_source",C::Quote("collector snapshot -> revalidated Mass entity -> current placement -> resident entity configuration -> visualization traits")},
                    {"selection_window_records",std::to_string(S::RecordLimit)},{"complete_visual_building","false"},{"transforms_composed","false"},
                    {"component_records_are_additive_geometry","false"},{"inherited_override_selection_status",C::Quote("not_evaluated")},
                    {"truncated",C::Bool(truncated)},{"sources",Sources.String()},{"candidates",Candidates.String()},
                    {"truncation_summary",C::Object({{"traversal_limits",C::Bool(Truncated)},
                        {"total_visit_limit",C::Bool(FullTraversal&&Graph.Truncated)},{"sources_byte_limit",C::Bool(Sources.Truncated)},
                        {"components_byte_limit",C::Bool(Components.Truncated)},{"mass_byte_limit",C::Bool(Mass.Truncated)},
                        {"candidates_byte_limit",C::Bool(Candidates.Truncated)},{"relationships_byte_limit",C::Bool(Relationships.Truncated)}})},
                    {"components",Components.String()},{"mass_representations",Mass.String()},{"relationships",Relationships.String()},
                    {"limits",C::Object({{"components",std::to_string(C::ComponentLimit)},{"scs_nodes_total_and_per_array",std::to_string(C::NodeLimit)},
                        {"materials_per_array",std::to_string(C::MaterialLimit)},{"child_depth",std::to_string(C::ChildDepthLimit)},
                        {"graph_depth",std::to_string(C::DepthLimit)},{"total_visits",std::to_string(C::VisitLimit)},
                        {"copied_records",std::to_string(S::RecordLimit)},{"candidate_definitions",std::to_string(S::DefinitionLimit)},
                        {"native_probes",std::to_string(S::ProbeLimit)},{"native_validation_work",std::to_string(S::NativeWorkLimit)},
                        {"output_bytes",std::to_string(C::OutputLimit)}})},
                    {"limitations",C::Quote("Native default components and native inherited resolution not attempted. Authored defaults/overrides are separate; effective virtual materials not evaluated. Construction scripts, live state, animation, skeletal poses, LOD/residency/Nanite and signed-scale transform composition not evaluated. Null numeric channels indicate invalid nonfinite input. Readable names unavailable.")}}));
            }
        };
        bool Visit(const SDK::UObject* definition,void* context)
        {return static_cast<Context*>(context)->Discover(reinterpret_cast<uintptr_t>(definition));}
        bool ProbeVisit(const SDK::UObject* definition,void* context)
        {return static_cast<Context*>(context)->ProbeDefinition(reinterpret_cast<uintptr_t>(definition));}

        void Export(uint32_t building,bool selected,const std::string& json,bool assembly=false)
        {
            std::filesystem::path directory;
            if(!MiniMapTerrainCache::GetBuildingDiagnosticsDirectory(directory))return;
            std::error_code ec;std::filesystem::create_directories(directory,ec);
            if(ec){LOG_ERROR("MiniMap: R2.4 diagnostic directory creation failed: %s",ec.message().c_str());return;}
            const auto path=directory/(assembly?L"Smelter_AssemblyRecipe.json":selected?L"Building_"+std::to_wstring(building)+L"_Representation.json":L"Representation_Selection.json");
            const auto temporary=path.wstring()+L".tmp";
            {
                std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
                file.write(json.data(),std::streamsize(json.size()));file.close();
                if(!file){LOG_ERROR("MiniMap: R2.4 diagnostic write failed");std::filesystem::remove(temporary,ec);return;}
            }
            if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            {LOG_ERROR("MiniMap: R2.4 diagnostic publication failed: %lu",GetLastError());std::filesystem::remove(temporary,ec);return;}
            LOG_INFO("MiniMap: R2.4 representation metadata exported: %ls (%zu bytes); visual completeness unproven",path.c_str(),json.size());
        }
        void BeforeEndPlay(SDK::UWorld* world,const char*)
        {
            const auto gameThread=MiniMapSmelterCapture::GetBindings().GameThread;
            if(gameThread&&reinterpret_cast<bool(*)()>(gameThread)())endingWorld=world;
            else stopped.store(true);
        }
        void Stop(){stopped.store(true);}
        void Tick(float)
        {
            const auto& b=MiniMapSmelterCapture::GetBindings();
            if(stopped.load()||attempted.load()||!b.GameThread||!reinterpret_cast<bool(*)()>(b.GameThread)())return;
            auto* world=MiniMapMap::GetWorld();
            if(!world||world==endingWorld)return;
            const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
            if(!snapshot||!snapshot->Complete||snapshot->Records.empty())return;
            if(attempted.exchange(true))return;
            if(executing.test_and_set())return;
            struct EndExecution {~EndExecution(){executing.clear();}} endExecution;
            try
            {
                LOG_INFO("MiniMap: R2.4b one-shot bounded representation-eligible selection entered");
                std::string json,assemblyJson;uint32_t building=0;bool hasSelection=false;
                {
                    Context context;
                    context.Source("native_default_components",0,"not_attempted",NativeExcluded);
                    context.SelectionPlan=S::Scan(snapshot->Records.size(),[&](size_t i)
                    {
                        const auto& r=snapshot->Records[i];
                        return S::Record{{r.DefinitionObjectIndex,r.DefinitionIndex,r.DefinitionNumber},r.BuildingId,
                            r.Index,r.Serial,r.Generation,r.DefinitionValid,i};
                    });
                    if(context.Initialize())
                    {
                        context.Decision=S::Choose(context.SelectionPlan,[&](const S::Record& record,size_t attempt)
                        {
                            const bool previousTruncated=context.Truncated;
                            context.Record=snapshot->Records[record.SnapshotIndex];context.ProbeResult={};
                            using V=MiniMapBuildingCollector::DiagnosticVisitStatus;
                            if(!context.Work.Spend(false)||!context.Graph.Remaining)
                            {context.ProbeResult.Truncated=true;context.ProbeResult.Note("work_budget_exhausted");}
                            else
                            {
                                const auto result=world==MiniMapMap::GetWorld()?
                                    MiniMapBuildingCollector::VisitDiagnosticDefinition(context.Record,&ProbeVisit,&context):V::WorldGenerationChanged;
                                switch(result)
                                {
                                case V::StaleEntity:context.ProbeResult.Stale=true;context.ProbeResult.MassIdentity="stale";context.ProbeResult.Note("stale_entity");break;
                                case V::WorldGenerationChanged:context.ProbeResult.MassIdentity="not_examined";context.ProbeResult.Note("world_generation_changed");break;
                                case V::CollectorUnavailable:context.ProbeResult.Note("collector_unavailable");break;
                                case V::ParametersUnavailable:context.ProbeResult.MassIdentity="valid";context.ProbeResult.Note("parameters_unavailable");break;
                                case V::InvalidCopiedDefinition:context.ProbeResult.Note("invalid_copied_definition");break;
                                default:break; // Visitor supplies precise tested predicates.
                                }
                            }
                            if(context.Work.Exhausted||!context.Work.Remaining||!context.Graph.Remaining)
                            {context.ProbeResult.Truncated=true;context.ProbeResult.Note("work_budget_exhausted");}
                            context.ProbeResult.Truncated|=context.Truncated&&!previousTruncated;
                            context.Truncated=previousTruncated;
                            context.SelectionTraversalTruncated|=context.ProbeResult.Truncated;
                            context.SelectionTraitVisits+=context.ProbeResult.Traits;context.SelectionMeshVisits+=context.ProbeResult.MeshReferences;
                            context.Attempts.Add(context.ProbeJson(record,attempt));
                            return S::Probe{context.ProbeResult.Route,context.ProbeResult.Stale,
                                context.Work.Exhausted||!context.Work.Remaining||!context.Graph.Remaining};
                        },S::Route::ConfigurationActor); // Placement soft-class route is deliberately unsupported.
                        context.SelectionGraphVisits=C::VisitLimit-context.Graph.Remaining;
                        if(context.Decision.HasSelection())
                        {
                            context.Record=snapshot->Records[context.Decision.Selected.SnapshotIndex];context.BuildingId=context.Record.BuildingId;
                            context.Status="not_attempted";context.SelectedTier=S::RouteName(context.Decision.SelectedRoute);
                            context.Reason="resident representation eligibility established; full metadata traversal pending";
                            if(context.Work.Remaining&&context.Graph.Remaining&&world==MiniMapMap::GetWorld())
                            {
                                context.FullTraversal=true;
                                if(MiniMapBuildingCollector::VisitDiagnosticDefinition(context.Record,&Visit,&context)!=MiniMapBuildingCollector::DiagnosticVisitStatus::Visited)
                                {
                                    context.Status="unavailable";context.Reason="selected eligible candidate failed full-snapshot revalidation";
                                    context.Source("selected_representation_revalidation",0,"unavailable","selected candidate could not be revalidated; no second full snapshot");
                                }
                            }
                            else context.Source("selected_representation_traversal",0,"not_attempted","world changed or shared work budget exhausted");
                        }
                        else context.Source("representation_selection",0,"unavailable","no eligible representation in bounded examined candidates; see candidate_attempts");
                    }
                    else context.Source("native_metadata_classes",0,"unavailable_not_resident","required exact native class anchors unavailable or invalid");
                    building=context.BuildingId;hasSelection=context.Decision.HasSelection();json=context.Document();
                    try {assemblyJson=context.AssemblyDocument(world);}
                    catch(...) {assemblyJson="{\"schema_version\":1,\"stage\":\"R2.5a\",\"status\":\"invalid\",\"reason\":\"audit_exception\",\"readiness\":{\"static_only_diagnostic_capture\":false,\"complete_visual_building\":false}}";}
                } // All borrowed pointers and strong references retire before I/O.
                Export(building,hasSelection,json);
                Export(building,hasSelection,assemblyJson,true);
            }
            catch(const std::exception& e){LOG_ERROR("MiniMap: R2.4 diagnostic failed, no retry: %.200s",e.what());}
            catch(...){LOG_ERROR("MiniMap: R2.4 diagnostic failed, no retry");}
        }
        bool MatchingBuild()
        {
            // Guard direct read contracts with the installed executable's RSDS
            // GUID/age, not just signatures shared by other Unreal builds.
            const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            if(!Readable(base,sizeof(IMAGE_DOS_HEADER)))return false;
            const auto dos=Value<IMAGE_DOS_HEADER>(base,0);
            if(dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<=0||dos.e_lfanew>1024*1024)return false;
            IMAGE_NT_HEADERS64 nt{};
            if(!Read(base,size_t(dos.e_lfanew),nt)||nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64)return false;
            const auto dir=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
            if(!dir.VirtualAddress||dir.Size>4096||dir.Size%sizeof(IMAGE_DEBUG_DIRECTORY)||dir.VirtualAddress>nt.OptionalHeader.SizeOfImage||
                dir.Size>nt.OptionalHeader.SizeOfImage-dir.VirtualAddress||!Readable(base+dir.VirtualAddress,dir.Size))return false;
            constexpr uint8_t guid[16]={0xa9,0xae,0xcb,0x41,0xc3,0x7a,0xda,0x99,0xb9,0x47,0xa1,0xdb,0x9c,0x10,0x21,0xed};
            for(size_t i=0;i<dir.Size/sizeof(IMAGE_DEBUG_DIRECTORY);++i)
            {
                const auto d=Value<IMAGE_DEBUG_DIRECTORY>(base+dir.VirtualAddress,i*sizeof(IMAGE_DEBUG_DIRECTORY));
                if(d.Type!=IMAGE_DEBUG_TYPE_CODEVIEW||d.SizeOfData<24||d.AddressOfRawData>nt.OptionalHeader.SizeOfImage||
                    d.SizeOfData>nt.OptionalHeader.SizeOfImage-d.AddressOfRawData||!Readable(base+d.AddressOfRawData,24))continue;
                const auto data=base+d.AddressOfRawData;
                if(Value<uint32_t>(data,0)==0x53445352&&Value<uint32_t>(data,20)==1&&
                    std::memcmp(reinterpret_cast<const void*>(data+4),guid,16)==0)return true;
            }
            return false;
        }
    }
    bool ResolvePrerequisites()
    {
        if(!MatchingBuild()){LOG_WARN("MiniMap: R2.4 matching CL-127004 executable identity unavailable; disabled");return false;}
        const auto anchor=MiniMapReadbackNative::ResolveOptionalFunction(Patterns::ObjectArrayAnchor,false);
        if(!anchor||Value<uint8_t>(anchor,0x15)!=0x48||Value<uint8_t>(anchor,0x16)!=0x8d||Value<uint8_t>(anchor,0x17)!=0x0d)return false;
        const auto displacement=Value<int32_t>(anchor,0x18);
        const auto target=int64_t(anchor)+0x1c+int64_t(displacement);
        if(target<=0||!Readable(uintptr_t(target),0x30,8))return false;
        objectArray=uintptr_t(target);
        skeletalGetter=MiniMapReadbackNative::ResolveOptionalFunction(Patterns::SkeletalAsset,false);
        LOG_INFO("MiniMap: R2.4 installed-binary identity and targeted object-array anchor verified; skeletal getter=%s",skeletalGetter?"available":"not_attempted");
        return true;
    }
    void Initialize()
    {
        if(registered||!objectArray)return;
        const auto& b=MiniMapSmelterCapture::GetBindings();
        auto* hooks=GetHooks();
        if(!hooks||!hooks->Engine||!hooks->World||!b.GameThread||!b.Retain||!b.Release)return;
        engine=hooks->Engine;worlds=hooks->World;
        if(!engine->GetStaticFindObjectByNameAddress||!engine->GetStaticFindObjectByNameAddress()||!engine->RegisterOnTick||!engine->UnregisterOnTick||
            !engine->RegisterOnShutdown||!engine->UnregisterOnShutdown||!worlds->RegisterOnBeforeWorldEndPlay||!worlds->UnregisterOnBeforeWorldEndPlay)return;
        engine->RegisterOnTick(&Tick);engine->RegisterOnShutdown(&Stop);worlds->RegisterOnBeforeWorldEndPlay(&BeforeEndPlay);registered=true;
        LOG_INFO("MiniMap: R2.4 enabled; waiting for complete collector snapshot; native default components explicitly excluded");
    }
    void Shutdown()
    {
        Stop();
        if(registered){engine->UnregisterOnTick(&Tick);engine->UnregisterOnShutdown(&Stop);worlds->UnregisterOnBeforeWorldEndPlay(&BeforeEndPlay);registered=false;}
        if(executing.test_and_set()){LOG_ERROR("MiniMap: R2.4 shutdown overlaps synchronous diagnostic; DLL unload not certified");}
        else executing.clear();
    }
}
#endif
