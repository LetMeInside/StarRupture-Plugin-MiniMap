#if defined(MODLOADER_CLIENT_BUILD)
#include "NativeVisualAccess.h"
#include "NativeApi.h"
#include "ResidentMetadata.h"
#include "DefinitionAcquisitionCore.h"
#include "Map/Map.h"
#include <SDK/Engine_classes.hpp>
#include <SDK/MassRepresentation_structs.hpp>
#include <cstring>
#include <Windows.h>

namespace
{
    namespace V=MiniMapVisualAccess;
    namespace N=MiniMapNative;
    namespace D=MiniMapDefinitionAcquisition;
    enum Type : size_t { Actor,ActorComponent,Scene,Primitive,Static,SplineMesh,Spline,Instanced,Subsystem,Level,MeshAsset,Material,Representation,Count };
    std::array<uintptr_t,Count> g_types{};
    const SDK::UWorld* g_world=nullptr;
    const void* g_manager=nullptr;
    uintptr_t g_subsystem=0;
    uint64_t g_generation=0;
    bool g_ready=false,g_faulted=false;

    // Required-field reads, not speculative page-readability probes. No
    // VirtualQuery/ReadMemory/ResidentMetadata validation on this hot path.
    bool ReadBytes(uintptr_t p,void* value,size_t size) noexcept
    {
        if(!p || p>UINTPTR_MAX-size)return false;
        __try {std::memcpy(value,reinterpret_cast<const void*>(p),size);return true;}
        __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
            ?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {g_faulted=true;return false;}
    }
    template<class T> bool Read(uintptr_t p,T& value) {return ReadBytes(p,&value,sizeof(value));}
    struct Reader {template<class T> bool Read(uintptr_t p,T& value){return ::Read(p,value);}};
    template<class Fn,class Result,class... Args> bool Call(Fn fn,Result& result,Args... args) noexcept
    {
        __try {result=fn(args...);return true;}
        __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
            ?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {g_faulted=true;return false;}
    }
    bool Free(void* p) noexcept
    {
        __try {N::Get()->texture.memoryFree(p);return true;}
        __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
            ?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {g_faulted=true;return false;}
    }
    bool Identity(uintptr_t p,V::Identity& id)
    {
        if(!p || (p&7))return false;
        int32_t index=-1;uint32_t flags=0;Reader reader;D::Slot slot;
        if(!Read(p+0xc,index) || !Read(p+8,flags) || (flags&0x1b400) ||
            D::ReadSlot(reader,reinterpret_cast<uintptr_t>(N::Get()->asset.objectArray),index,slot)!=D::Status::Success ||
            slot.Object!=p || slot.Serial<=0 || (slot.Flags&0x10200000))return false;
        id={index,slot.Serial};return true;
    }
    bool IsA(uintptr_t object,Type expected)
    {
        uintptr_t type=0;if(!Read(object+0x10,type))return false;
        for(unsigned depth=0;type&&depth<64;++depth)
        {
            V::Identity id;if(!Identity(type,id))return false;
            if(type==g_types[expected])return true;
            if(!Read(type+offsetof(SDK::UStruct,SuperStruct),type))return false;
        }
        return false;
    }
    struct Array {uintptr_t Data=0;int32_t Num=0,Max=0;};
    static_assert(sizeof(Array)==0x10 && sizeof(SDK::FTransform)==0x60);
    static_assert(sizeof(SDK::AActor)==0x2a8 && sizeof(SDK::UActorComponent)==0xb8 &&
        sizeof(SDK::USceneComponent)==0x250 && sizeof(SDK::UPrimitiveComponent)==0x530 &&
        sizeof(SDK::UStaticMeshComponent)==0x630 && sizeof(SDK::USplineMeshComponent)==0x760 &&
        sizeof(SDK::USplineComponent)==0x5e0 && sizeof(SDK::UInstancedStaticMeshComponent)==0x9c0 &&
        sizeof(SDK::ULevel)==0x3f0 && sizeof(SDK::UStaticMesh)==0x290 && sizeof(SDK::UMaterialInterface)==0xe8);
    static_assert(offsetof(SDK::USplineMeshComponent,SplineParams)==0x630 &&
        offsetof(SDK::USplineMeshComponent,SplineUpDir)==0x6e0 &&
        offsetof(SDK::USplineMeshComponent,ForwardAxis)==0x754 &&
        offsetof(SDK::USplineMeshComponent,SplineBoundaryMin)==0x6f8 &&
        offsetof(SDK::USplineMeshComponent,SplineBoundaryMax)==0x750 &&
        offsetof(SDK::UStaticMeshComponent,StaticMesh)==0x588 &&
        offsetof(SDK::UMeshComponent,OverrideMaterials)==0x530 &&
        offsetof(SDK::UStaticMesh,StaticMaterials)==0x160 && sizeof(SDK::FStaticMaterial)==0x38 &&
        sizeof(SDK::FSplineMeshParams)==0xb0 && offsetof(SDK::FSplineMeshParams,EndScale)==0x70 &&
        offsetof(SDK::FSplineMeshParams,EndTangent)==0x80);
    bool ArrayValid(const Array& a,size_t limit)
    {return a.Num>=0 && a.Max>=a.Num && size_t(a.Num)<=limit && a.Max<=1048576 && (!a.Num||a.Data);}
    bool ObjectValue(uintptr_t p,V::Identity& id)
    {if(!p){id={};return true;}return Identity(p,id);}
    V::Point Point(const SDK::FVector& p){return {p.X,p.Y,p.Z};}
    struct Ops
    {
        MiniMapNativeVisualAccess::CurrentFn Current;
        V::Token Source;
        uintptr_t ActorObject=0;
        V::Identity ActorId;
        uint8_t RepresentationValue=0xff;
        N::NativeComponentArray Components;
        bool OwnsArray=false;
        std::array<V::Identity,V::MaximumComponents> ComponentIds{};
        ~Ops(){if(OwnsArray && Components.Data && !g_faulted)Free(Components.Data);}
        V::Status Admit(const V::Token& token)
        {
            const auto* n=N::Get();
            if(!n || !n->player.isGameThread)return V::Status::Unavailable;
            if(!n->player.isGameThread())return V::Status::WrongThread;
            if(!Current || !Current(token.Generation,g_world,g_manager) || token.Generation!=g_generation)
                return V::Status::WorldChanged;
            if(!g_ready || g_faulted || !n->mass.isEntityValid || !n->mass.isEntityBuilt || !n->mass.getFragmentDataPtr ||
                !n->texture.memoryFree || !MiniMapResidentMetadata::Available())return V::Status::Unavailable;
            if(token.Entity.Index<0 || token.Entity.Serial<=0 || !n->mass.isEntityValid(g_manager,{token.Entity.Index,token.Entity.Serial}))return V::Status::StaleEntity;
            if(!n->mass.isEntityBuilt(g_manager,{token.Entity.Index,token.Entity.Serial}))return V::Status::UnbuiltEntity;
            Source=token;return V::Status::Ready;
        }
        bool RepresentationNow(uint8_t& result)
        {
            const auto* n=N::Get();
            const void* fragment=n->mass.getFragmentDataPtr(g_manager,{Source.Entity.Index,Source.Entity.Serial},
                reinterpret_cast<const SDK::UScriptStruct*>(g_types[Representation]));
            return fragment && Read(reinterpret_cast<uintptr_t>(fragment),result);
        }
        bool Association()
        {
            V::Identity id;uintptr_t outer=0,collection=0,manager=0,entityManager=0;
            // Matching PDB: subsystem ActorManager +38; manager EntityManager +68.
            if(!Identity(g_subsystem,id) || !IsA(g_subsystem,Subsystem) ||
                !Read(g_subsystem+0x20,outer) || outer!=reinterpret_cast<uintptr_t>(g_world) ||
                !Read(g_subsystem+0x28,collection) || collection!=outer+0x9f0 ||
                !Read(g_subsystem+0x38,manager) || !manager ||
                !Read(manager+0x68,entityManager) || entityManager!=reinterpret_cast<uintptr_t>(g_manager))return false;
            const auto& api=N::Get()->visualAccess;
            SDK::AActor* actor=nullptr;
            if(!Call(api.actorFromEntity,actor,reinterpret_cast<const SDK::UMassActorSubsystem*>(g_subsystem),
                N::MassEntityHandle{Source.Entity.Index,Source.Entity.Serial},uint32_t(0)))return false;
            if(!actor || reinterpret_cast<uintptr_t>(actor)!=ActorObject || !Identity(ActorObject,id) || id!=ActorId)return false;
            N::MassEntityHandle reverse{},*returned=nullptr;
            const uint64_t key=uint64_t(uint32_t(ActorId.Index))|(uint64_t(uint32_t(ActorId.Serial))<<32);
            if(!Call(api.entityFromActor,returned,reinterpret_cast<const SDK::UMassActorSubsystem*>(g_subsystem),&reverse,key) || returned!=&reverse ||
                reverse.Index!=Source.Entity.Index || reverse.SerialNumber!=Source.Entity.Serial)return false;
            if(!Read(ActorObject+0x20,outer) || !Identity(outer,id) || !IsA(outer,Level) ||
                !Read(outer+0xc0,outer) || outer!=reinterpret_cast<uintptr_t>(g_world))return false;
            uint8_t flags=0;
            return Read(ActorObject+0x5d,flags) && !(flags&2);
        }
        V::Status Associate(V::Observation& out)
        {
            if(!Identity(reinterpret_cast<uintptr_t>(g_world),out.World))return V::Status::WorldChanged;
            if(!RepresentationNow(RepresentationValue))return V::Status::RepresentationUnavailable;
            out.Representation=RepresentationValue;
            if(RepresentationValue==2 || RepresentationValue==3)return V::Status::NoActorRepresentation;
            if(RepresentationValue>3)return V::Status::UnsupportedRepresentation;
            uintptr_t manager=0,entityManager=0,outer=0,collection=0;V::Identity subsystemId;
            // Validate BEFORE native wrappers which assert on a missing manager.
            if(!Identity(g_subsystem,subsystemId) || !IsA(g_subsystem,Subsystem) ||
                !Read(g_subsystem+0x20,outer) || outer!=reinterpret_cast<uintptr_t>(g_world) ||
                !Read(g_subsystem+0x28,collection) || collection!=outer+0x9f0 ||
                !Read(g_subsystem+0x38,manager) || !manager || !Read(manager+0x68,entityManager) ||
                entityManager!=reinterpret_cast<uintptr_t>(g_manager))return V::Status::Unavailable;
            SDK::AActor* actor=nullptr;
            if(!Call(N::Get()->visualAccess.actorFromEntity,actor,reinterpret_cast<const SDK::UMassActorSubsystem*>(g_subsystem),
                N::MassEntityHandle{Source.Entity.Index,Source.Entity.Serial},uint32_t(0)))return V::Status::Interrupted;
            ActorObject=reinterpret_cast<uintptr_t>(actor);
            if(!ActorObject)return V::Status::ActorUnavailable;
            if(!Identity(ActorObject,ActorId) || !IsA(ActorObject,Actor) || !Association())return V::Status::AssociationChanged;
            uint8_t hidden=0;if(!Read(ActorObject+0x58,hidden))return V::Status::AssociationChanged;
            out.ActorHiddenInGame=bool(hidden&0x80);
            out.Actor=ActorId;return V::Status::Ready;
        }
        V::Status Enumerate(size_t& count)
        {
            uint8_t state=0;int32_t slots=0;
            // PDB AActor flags and OwnedComponents sparse storage Num: native
            // enumeration walks at most this many slots, not an actor-specific array.
            if(!Read(ActorObject+0x5c,state) || (state&0x10) || !(state&0x80))return V::Status::IncompleteActor;
            if(!Read(ActorObject+0x238,slots) || slots<0 || size_t(slots)>V::MaximumComponents)return V::Status::EnumerationLimit;
            N::NativeComponentClass type{reinterpret_cast<SDK::UClass*>(g_types[ActorComponent])};
            N::NativeComponentArray* returned=nullptr;
            if(!Call(N::Get()->visualAccess.components,returned,reinterpret_cast<const SDK::AActor*>(ActorObject),&Components,&type))return V::Status::Interrupted;
            OwnsArray=true;
            if(returned!=&Components || Components.Num<0 || Components.Max<Components.Num ||
                Components.Max>1048576 || (Components.Num&&!Components.Data))return V::Status::EnumerationIncomplete;
            if(size_t(Components.Num)>V::MaximumComponents)return V::Status::EnumerationLimit;
            count=size_t(Components.Num);return V::Status::Ready;
        }
        bool ComponentOwned(uintptr_t object,V::Identity& id)
        {
            uintptr_t owner=0,world=0;uint8_t destroying=0,registered=0;
            return Identity(object,id) && IsA(object,ActorComponent) && Read(object+0xa0,owner) && owner==ActorObject &&
                Read(object+0xa8,world) && Read(object+0x94,registered) &&
                (world==reinterpret_cast<uintptr_t>(g_world) || (!world&&!(registered&1))) &&
                Read(object+0x97,destroying) && !(destroying&0x40);
        }
        V::Status Copy(size_t index,V::Component& out)
        {
            uintptr_t object=0;if(!Read(reinterpret_cast<uintptr_t>(Components.Data)+index*8,object) ||
                !ComponentOwned(object,out.Object))return V::Status::InvalidComponent;
            ComponentIds[index]=out.Object;
            if(!IsA(object,Scene))return V::Status::Ready; // Nonvisual actor components still validated.
            uintptr_t componentClass=0;
            if(!Read(object+0x10,componentClass) || !Identity(componentClass,out.Class))return V::Status::InvalidComponent;
            if(IsA(object,SplineMesh))out.Type=V::Kind::SplineMesh;
            else if(IsA(object,Instanced))out.Type=V::Kind::InstancedStaticMesh;
            else if(IsA(object,Static))out.Type=V::Kind::StaticMesh;
            else if(IsA(object,Spline))out.Type=V::Kind::Spline;
            else if(IsA(object,Primitive))out.Type=V::Kind::UnsupportedPrimitive;
            else out.Type=V::Kind::UnsupportedScene; // Includes child-actor holders; no hidden completeness claim.
            uint8_t registered=0,visible=0,hidden=0;
            if(!Read(object+0x94,registered) || !Read(object+0x1a0,visible) || !Read(object+0x1a1,hidden))return V::Status::InvalidComponent;
            if(!(visible&1))return V::Status::InvalidTransform; // Native component-world cache not initialized.
            out.Registered=bool(registered&1);out.Visible=bool(visible&0x20);out.HiddenInGame=bool(hidden&8);
            alignas(16) SDK::FTransform transform{};SDK::FTransform* returned=nullptr;
            if((object&15) || !Call(N::Get()->visualAccess.componentTransform,returned,reinterpret_cast<const SDK::USceneComponent*>(object),&transform) || returned!=&transform)return V::Status::InvalidTransform;
            out.ComponentToWorld.Translation=Point(transform.Translation);out.ComponentToWorld.Scale=Point(transform.Scale3D);
            out.ComponentToWorld.Rotation={transform.Rotation.X,transform.Rotation.Y,transform.Rotation.Z,transform.Rotation.W};
            if(out.Type==V::Kind::SplineMesh)
            {
                SDK::FSplineMeshParams p{};SDK::FVector up{};uint8_t axis=0,flags=0;float low=0,high=0;
                if(!Read(object+offsetof(SDK::USplineMeshComponent,SplineParams),p) ||
                    !Read(object+offsetof(SDK::USplineMeshComponent,SplineUpDir),up) ||
                    !Read(object+offsetof(SDK::USplineMeshComponent,ForwardAxis),axis) ||
                    !Read(object+offsetof(SDK::USplineMeshComponent,SplineBoundaryMin),low) ||
                    !Read(object+offsetof(SDK::USplineMeshComponent,SplineBoundaryMax),high) || !Read(object+0x755,flags))return V::Status::InvalidComponent;
                V::SplineMeshParameters value;
                value.Start=Point(p.StartPos);value.End=Point(p.EndPos);value.StartTangent=Point(p.StartTangent);value.EndTangent=Point(p.EndTangent);value.Up=Point(up);
                value.StartScale={p.StartScale.X,p.StartScale.Y};value.EndScale={p.EndScale.X,p.EndScale.Y};
                value.StartOffset={p.StartOffset.X,p.StartOffset.Y};value.EndOffset={p.EndOffset.X,p.EndOffset.Y};
                value.StartRoll=p.StartRoll;value.EndRoll=p.EndRoll;value.BoundaryMin=low;value.BoundaryMax=high;
                value.NaniteBoundsScale=p.NaniteClusterBoundsScale;
                value.ForwardAxis=axis;value.SmoothRollScale=bool(flags&2);value.MeshDirty=bool(flags&4);
                out.SplineMesh=value; // Raw copied descriptor, not a renderability verdict.
            }
            if(out.Type==V::Kind::StaticMesh || out.Type==V::Kind::SplineMesh || out.Type==V::Kind::InstancedStaticMesh)
            {
                uintptr_t mesh=0;Array defaults,overrides;
                if(!Read(object+offsetof(SDK::UStaticMeshComponent,StaticMesh),mesh) || !ObjectValue(mesh,out.Mesh))return V::Status::InvalidComponent;
                if(mesh && (!IsA(mesh,MeshAsset) || !Read(mesh+offsetof(SDK::UStaticMesh,StaticMaterials),defaults)))return V::Status::InvalidComponent;
                if(!Read(object+offsetof(SDK::UMeshComponent,OverrideMaterials),overrides) || !ArrayValid(defaults,V::MaximumMaterials) || !ArrayValid(overrides,V::MaximumMaterials))return V::Status::EnumerationLimit;
                const size_t materialCount=(std::max)(size_t(defaults.Num),size_t(overrides.Num));out.Materials.resize(materialCount);
                for(size_t i=0;i<materialCount;++i)
                {
                    uintptr_t base=0,overrideValue=0;
                    if((i<size_t(defaults.Num)&&!Read(defaults.Data+i*sizeof(SDK::FStaticMaterial),base)) ||
                        (i<size_t(overrides.Num)&&!Read(overrides.Data+i*8,overrideValue)))return V::Status::InvalidComponent;
                    auto& slot=out.Materials[i];
                    if(!ObjectValue(base,slot.MeshDefault) || !ObjectValue(overrideValue,slot.Override) ||
                        (base&&!IsA(base,Material)) || (overrideValue&&!IsA(overrideValue,Material)))return V::Status::InvalidComponent;
                    slot.Effective=overrideValue?slot.Override:slot.MeshDefault;
                }
            }
            return V::Status::Ready;
        }
        bool Unchanged(const V::Observation& out)
        {
            uint8_t representation=0xff;
            if(!RepresentationNow(representation) || representation!=RepresentationValue || !Association())return false;
            // A second bounded native enumeration detects additions/removals
            // as well as recycled components. Do not treat matching counts as
            // matching membership; native TSet order is not an identity.
            const auto oldCount=Components.Num;
            if(Components.Data && !Free(Components.Data))return false;
            Components={};OwnsArray=false;
            size_t count=0;if(Enumerate(count)!=V::Status::Ready || count!=size_t(oldCount))return false;
            auto before=ComponentIds;std::array<V::Identity,V::MaximumComponents> after{};
            for(int32_t i=0;i<Components.Num;++i)
            {
                uintptr_t object=0;V::Identity id;
                if(!Read(reinterpret_cast<uintptr_t>(Components.Data)+size_t(i)*8,object) || !ComponentOwned(object,id))return false;
                after[size_t(i)]=id;
            }
            auto less=[](V::Identity a,V::Identity b){return a.Index==b.Index?a.Serial<b.Serial:a.Index<b.Index;};
            std::sort(before.begin(),before.begin()+count,less);std::sort(after.begin(),after.begin()+count,less);
            if(!std::equal(before.begin(),before.begin()+count,after.begin()))return false;
            // Resource identities are weak provenance, not strong native owners.
            auto alive=[](V::Identity id)
            {
                if(id.Index<0)return id.Serial==0;
                Reader reader;D::Slot slot;V::Identity checked;
                return D::ReadSlot(reader,reinterpret_cast<uintptr_t>(N::Get()->asset.objectArray),id.Index,slot)==D::Status::Success &&
                    slot.Serial==id.Serial && Identity(slot.Object,checked) && checked==id;
            };
            for(const auto& component:out.Components)
            {
                if(!alive(component.Class)||!alive(component.Mesh))return false;
                for(const auto& slot:component.Materials)
                    if(!alive(slot.MeshDefault)||!alive(slot.Override)||!alive(slot.Effective))return false;
            }
            return RepresentationNow(representation) && representation==RepresentationValue && Association();
        }
    };
}

namespace MiniMapNativeVisualAccess
{
    void ResetWorld()
    {
        g_ready=false;g_world=nullptr;g_manager=nullptr;g_generation=0;g_subsystem=0;
        for(auto& object:g_types){if(object)MiniMapResidentMetadata::Release(object);object=0;}
    }
    void InitializeWorld(const SDK::UWorld* world,const void* manager,uint64_t generation)
    {
        if(g_world==world && g_manager==manager && g_generation==generation)return;
        ResetWorld();g_world=world;g_manager=manager;g_generation=generation;
        if(!world || !manager || !generation || g_faulted || !MiniMapResidentMetadata::Available() || !N::Get()->world.getSubsystemInternal)return;
        // Bounded world setup ONLY. Existing page-based metadata validation is
        // intentionally confined here, not amortized once per snapshot/entity.
        const std::array<const wchar_t*,Count> paths={L"/Script/Engine.Actor",L"/Script/Engine.ActorComponent",L"/Script/Engine.SceneComponent",
            L"/Script/Engine.PrimitiveComponent",L"/Script/Engine.StaticMeshComponent",L"/Script/Engine.SplineMeshComponent",L"/Script/Engine.SplineComponent",
            L"/Script/Engine.InstancedStaticMeshComponent",L"/Script/MassActors.MassActorSubsystem",L"/Script/Engine.Level",L"/Script/Engine.StaticMesh",L"/Script/Engine.MaterialInterface",
            L"/Script/MassRepresentation.MassRepresentationFragment"};
        const std::array<size_t,Count> sizes={sizeof(SDK::AActor),sizeof(SDK::UActorComponent),sizeof(SDK::USceneComponent),sizeof(SDK::UPrimitiveComponent),
            sizeof(SDK::UStaticMeshComponent),sizeof(SDK::USplineMeshComponent),sizeof(SDK::USplineComponent),sizeof(SDK::UInstancedStaticMeshComponent),
            0x48,sizeof(SDK::ULevel),sizeof(SDK::UStaticMesh),sizeof(SDK::UMaterialInterface),0x90};
        for(size_t i=0;i<Count;++i)
        {g_types[i]=MiniMapResidentMetadata::Acquire(paths[i],i==Representation,sizes[i]);if(!g_types[i])return;}
        uintptr_t result=0;
        if(!Call(N::Get()->world.getSubsystemInternal,result,reinterpret_cast<uintptr_t>(world)+0x9f0,g_types[Subsystem]))return;
        g_subsystem=result;g_ready=bool(result);
    }
    V::Observation Observe(const V::Token& token,CurrentFn current)
    {
        Ops ops{};ops.Current=current;return V::Acquire(ops,token);
    }
}
#endif
