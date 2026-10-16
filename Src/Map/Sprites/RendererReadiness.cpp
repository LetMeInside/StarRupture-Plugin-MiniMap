#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE) && defined(MODLOADER_CLIENT_BUILD)
#include "RendererReadiness.h"
#include "StaticAssemblyCaptureCore.h"
#include "BuildingRepresentation.h"
#include "Native/NativeApi.h"
#include <Engine_classes.hpp>
#include <cstring>
#include <cstddef>

namespace MiniMapRendererReadiness
{
    namespace
    {
        // SDK-declared layouts; no private structure overlay.
        static_assert(sizeof(SDK::UStreamableRenderAsset)==0xd0);
        static_assert(sizeof(SDK::UStaticMesh)==0x290);
        static_assert(offsetof(SDK::UStaticMesh,MinLOD)==0x158);
        static_assert(offsetof(SDK::UStaticMesh,StaticMaterials)==0x160);
        bool StaticMesh(uintptr_t p,uintptr_t type)
        {
            if(!MiniMapBuildingRepresentation::ValidateCaptureObject(p))return false;
            if(!MiniMapNative::IsMemoryRangeReadable(p,sizeof(SDK::UStaticMesh),8))return false;
            auto* klass=reinterpret_cast<const SDK::UObject*>(p)->Class;
            for(size_t n=0;klass&&n<64;++n)
            {
                if(!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(klass)))return false;
                if(reinterpret_cast<uintptr_t>(klass)==type)return true;
                if(!MiniMapNative::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(klass),sizeof(SDK::UClass),8))return false;
                klass=static_cast<SDK::UClass*>(klass->SuperStruct);
            }
            return false;
        }
        uintptr_t meshType=0; // one explicit GT metadata root, released on world end/shutdown
        std::array<Provider,8> providers{};size_t providerCount=0;uint64_t providerEpoch=0,resourceVersion=0,observationRevision=0;
        bool OnGameThread()
        {
            if (!MiniMapNative::Get()) return false;
            const auto& b=*MiniMapNative::Get();
            return b.player.isGameThread&&b.player.isGameThread();
        }
        MeshState ObserveMesh(const PreparedResource& resource)
        {
            MeshState s;
            // Preparation verified receiver/allocation/type; the caller still owns
            // its strong reference. UObject identity/class are immutable while rooted.
            // These scalar checks detect explicit teardown/replacement without a
            // repeated object-array walk, class lookup or broad VirtualQuery audit.
            if(!resource.Object)return s;
            const auto* mesh=reinterpret_cast<const SDK::UStaticMesh*>(resource.Object);
            if(mesh->Index!=resource.Index||mesh->Name.ComparisonIndex!=resource.Name||
                mesh->Name.Number!=resource.Number||reinterpret_cast<uintptr_t>(mesh->Class)!=resource.Class||
                (uint32_t(mesh->Flags)&(uint32_t(SDK::EObjectFlags::BeginDestroyed)|uint32_t(SDK::EObjectFlags::FinishDestroyed))))return s;
            uintptr_t update{},data{};
            std::memcpy(s.Cache.data(),reinterpret_cast<const uint8_t*>(mesh)+0xc8,8);
            std::memcpy(&update,reinterpret_cast<const uint8_t*>(mesh)+0x38,8);
            std::memcpy(&data,reinterpret_cast<const uint8_t*>(mesh)+0xe8,8);
            s.PendingUpdate=update!=0;s.HasRenderData=data!=0;s.NeverStream=mesh->NeverStream;
            // Opaque token for equality/invalidation only. Never
            // preserve a render-data pointer or dereference a previous sample.
            s.ResourceToken=(uint64_t(data)^uint64_t(resource.Index))*0x9e3779b97f4a7c15ull;
            if(data)
            {
                // UStaticMesh owns RenderData. Use the installed GT initialization
                // predicate's narrow scalar reads only during this non-reentrant
                // call; GT replacement/GC cannot run inside it. RT streaming does
                // not own/delete the mesh's render-data container. Rooting does
                // NOT pin RenderData across calls: reacquire it on every sample.
                if(data>UINTPTR_MAX-0x93||!MiniMapNative::IsMemoryRangeReadable(data+0x8e,5,1))return {};
                uint8_t ready{},initialized{};
                std::memcpy(&ready,reinterpret_cast<const void*>(data+0x8e),1);
                std::memcpy(&initialized,reinterpret_cast<const void*>(data+0x92),1);
                if(ready>1||initialized>1)return {};
                s.ReadyForStreaming=ready!=0;s.Initialized=initialized!=0;
                uintptr_t current{};std::memcpy(&current,reinterpret_cast<const uint8_t*>(mesh)+0xe8,8);
                if(current!=data)return {}; // defensive invalidation, not the lifetime proof
            }
            s.Observed=true;return s;
        }
        bool StreamingProvider(const PreparedResource& resource,const ProviderObservation& before,ProviderObservation& after)
        {
            after={};after.Provider=1;after.World=resource.World;
            after.ObjectIndex=resource.Index;after.NameIndex=resource.Name;after.NameNumber=resource.Number;
            after.ResourceIdentity=(uint64_t(uint32_t(resource.Index))<<32)^uint32_t(resource.Name)^uint64_t(resource.Number)*0x9e3779b97f4a7c15ull;
            after.ObservationRevision=++observationRevision;
            after.Conditions=Bit(Condition::Initialization)|Bit(Condition::Residency);
            after.Unknown=after.Conditions;after.Mesh=ObserveMesh(resource);
            after.ResourceVersion=before.ResourceVersion?before.ResourceVersion:++resourceVersion;
            bool replaced=before.Mesh.Observed&&after.Mesh.ResourceToken!=before.Mesh.ResourceToken;
            for(size_t i=1;i<=4;++i)replaced|=before.Mesh.Observed&&before.Mesh.Cache[i]!=after.Mesh.Cache[i];
            replaced|=before.Mesh.Observed&&before.Mesh.Cache[7]!=after.Mesh.Cache[7];
            if(replaced||!after.Mesh.Observed)after.ResourceVersion=++resourceVersion;
            if(!after.Mesh.Observed)return true;
            const auto streaming=InspectStreaming(after.Mesh);
            if(streaming.State==Proof::Pending)
            {
                after.Pending=Bit(after.Mesh.Initialized&&!after.Mesh.ReadyForStreaming?Condition::Initialization:Condition::Residency);
                after.Unknown&=~after.Pending;after.CanChange=true;after.Method=ObservationMethod::BoundedPolling;
            }
            after.RequiresEvaluation=before.Mesh.Observed&&
                (after.Mesh.ResourceToken!=before.Mesh.ResourceToken||after.Mesh.Cache!=before.Mesh.Cache||
                 after.Mesh.PendingUpdate!=before.Mesh.PendingUpdate||after.Mesh.Initialized!=before.Mesh.Initialized||
                 after.Mesh.ReadyForStreaming!=before.Mesh.ReadyForStreaming);
            after.RecaptureEligible=!replaced&&Improved(before.Mesh,after.Mesh);
            // Full RHI usability/selected representation remains Unknown even
            // after the cached operation completes; no completed condition minted.
            return true;
        }
        bool StreamingRelevant(const ProviderObservation& proof,const ProviderObservation& current)
        {
            if(!proof.RecaptureEligible||!SameResource(proof,current)||proof.ResourceVersion!=current.ResourceVersion||
                current.ObservationRevision<proof.ObservationRevision||!current.Mesh.Observed||
                !current.Mesh.Initialized||!current.Mesh.ReadyForStreaming||current.Mesh.PendingUpdate)return false;
            const auto a=InspectStreaming(proof.Mesh),b=InspectStreaming(current.Mesh);
            return a.FirstResident>=0&&b.FirstResident>=0&&b.FirstResident<=a.FirstResident;
        }
        void Rebuild(AssemblyObservation& observation)
        {
            std::array<Component,16> components{};uint32_t unresolved=0;
            for(size_t i=0;i<observation.Readiness.Components;++i)
            {
                auto& component=components[i];component.Identity=i+1;component.Generation=observation.Readiness.Generation;
                component.Conditions[size_t(Condition::Representation)]=Proof::Satisfied;
                for(size_t n=0;n<size_t(Condition::Count);++n)
                {
                    const uint32_t bit=uint32_t(1)<<n;
                    bool found=false,unknown=false,pending=false,unsupported=false,wake=false;
                    for(size_t t=0;t<observation.TaskCount;++t)
                    {
                        const auto& task=observation.Tasks[t];const auto& e=task.Evidence;
                        if(!(task.Contributors&(uint16_t(1)<<i))||!(e.Conditions&bit))continue;
                        found=true;unsupported|=(e.Unsupported&bit)!=0;pending|=(e.Pending&bit)!=0;
                        unknown|=(e.Unknown&bit)!=0||!((e.Completed|e.Pending|e.Unsupported)&bit)||
                            ((e.Pending&bit)&&(!e.CanChange||e.Method==ObservationMethod::Unavailable));
                        wake|=(e.Pending&bit)&&e.CanChange&&e.Method!=ObservationMethod::Unavailable;
                    }
                    if(!found)continue;
                    // All contributing resources must complete a prerequisite.
                    // Preserve Unknown alongside another resource's Pending proof.
                    if(unknown)unresolved|=bit;
                    component.Conditions[n]=unsupported?Proof::Unsupported:pending?Proof::Pending:unknown?Proof::Unknown:Proof::Satisfied;
                    if(wake)component.Wakes[n]=Wake::ResourceChanged;
                }
            }
            observation.Readiness=Evaluate(components.data(),observation.Readiness.Components,observation.Readiness.Generation);
            observation.Readiness.Unknown|=unresolved;
            if(!observation.Readiness.Unsupported&&observation.Readiness.Unknown)observation.Readiness.State=RenderReadiness::Unknown;
            PreserveVisualUncertainty(observation.Readiness,observation.VisualIncomplete);
        }

    }
    bool RegisterProvider(const Provider& provider)
    {
        if(!OnGameThread()||!provider.Id||!provider.Conditions||!provider.Observe||!provider.Relevant||
            (provider.Conditions&~((uint32_t(1)<<unsigned(Condition::Count))-1))||
            provider.Method==ObservationMethod::Unavailable||FindProvider(provider.Id)||providerCount==providers.size()||
            (provider.Method==ObservationMethod::BoundedPolling&&provider.PollMilliseconds<50))return false;
        providers[providerCount++]=provider;++providerEpoch;return true;
    }
    const Provider* FindProvider(uint32_t id)
    {for(size_t i=0;i<providerCount;++i)if(providers[i].Id==id)return &providers[i];return nullptr;}
    uint64_t ProviderEpoch(){return providerEpoch;}
    uint32_t ProviderConditions(){uint32_t result=0;for(size_t i=0;i<providerCount;++i)result|=providers[i].Conditions;return result;}
    void ReleaseMetadata()
    {if(OnGameThread()&&meshType){MiniMapBuildingRepresentation::ReleaseResidentMetadata(meshType);meshType=0;}}
    void InvalidateTask(AssemblyObservation& observation,size_t index)
    {
        if(!OnGameThread()||index>=observation.TaskCount)return;
        auto& e=observation.Tasks[index].Evidence;
        e.Unknown=e.Conditions;e.Pending=e.Completed=e.Unsupported=0;
        e.CanChange=e.RequiresEvaluation=e.RecaptureEligible=false;e.Method=ObservationMethod::Unavailable;
        e.Mesh.Observed=false;Rebuild(observation);
    }
    bool ObserveTask(AssemblyObservation& observation,size_t index,ProviderObservation& change)
    {
        if(!OnGameThread()||index>=observation.TaskCount)return false;
        auto& task=observation.Tasks[index];const auto* provider=FindProvider(task.Evidence.Provider);
        if(!provider||task.Resource.World!=observation.Readiness.Generation.World||
            !provider->Observe(task.Resource,task.Evidence,change)||change.World!=task.Resource.World||
            change.Provider!=provider->Id||change.Conditions!=provider->Conditions||
            change.ObjectIndex!=task.Resource.Index||change.NameIndex!=task.Resource.Name||change.NameNumber!=task.Resource.Number||
            change.ObjectIndex<0||change.NameIndex<0)return false;
        const auto all=change.Pending|change.Unknown|change.Completed|change.Unsupported;
        if((all&~change.Conditions)||(change.Pending&(change.Unknown|change.Completed|change.Unsupported))||
            (change.Unknown&(change.Completed|change.Unsupported))||(change.Completed&change.Unsupported)||
            !change.ResourceVersion||change.ResourceVersion<task.Evidence.ResourceVersion||
            !change.ObservationRevision||change.ObservationRevision<=task.Evidence.ObservationRevision)return false;
        task.Evidence=change;
        if(index<observation.UniqueMeshes&&provider->Id==1)observation.Meshes[index]=change.Mesh;
        Rebuild(observation);return true;
    }
    AssemblyObservation ObserveRetainedAssembly(const MiniMapStaticAssembly::Recipe& recipe,const Dependencies* baseline)
    {
        AssemblyObservation out;out.Readiness.Generation.World=recipe.Generation;out.VisualIncomplete=recipe.VisualIncomplete;
        out.Readiness.Unknown=(uint32_t(1)<<unsigned(Condition::Count))-1;
        if(!OnGameThread())return out;
        if(!FindProvider(1))RegisterProvider({1,Bit(Condition::Initialization)|Bit(Condition::Residency),250,
            ObservationMethod::BoundedPolling,&StreamingRelevant,&StreamingProvider});
        MiniMapStaticAssembly::Footprint bounds;
        if(MiniMapStaticAssembly::Validate(recipe,bounds)){out.Reason="renderer_readiness_unknown_representation";return out;}
        if(!meshType)meshType=MiniMapBuildingRepresentation::AcquireResidentMetadata(L"/Script/Engine.StaticMesh",false,sizeof(SDK::UStaticMesh));
        if(!meshType){out.Reason="renderer_readiness_unknown_static_mesh_type";return out;}
        out.Readiness.Components=recipe.Count;
        out.Source.reserve(5+MiniMapStaticAssembly::Limit*21+MiniMapStaticAssembly::Assignments*4);
        // Assets are retained/validated by admission. Store exact scalar identities
        // and authored recipe values; never store a borrowed UObject in session metadata.
        auto object=[&](uintptr_t p){const auto* o=reinterpret_cast<const SDK::UObject*>(p);
            out.Source.push_back(uint32_t(o->Index));out.Source.push_back(uint32_t(o->Name.ComparisonIndex));out.Source.push_back(o->Name.Number);};
        auto values=[&](const auto& a){for(double v:a){uint64_t bits{};std::memcpy(&bits,&v,8);out.Source.push_back(bits);}};
        out.Source.push_back(recipe.Generation);object(recipe.ActorClass);out.Source.push_back(recipe.Count);
        for(size_t i=0;i<recipe.Count;++i){const auto& v=recipe.Instances[i];object(v.Mesh);out.Source.push_back(v.ComponentIndex);
            values(v.Transform.T);values(v.Transform.R);values(v.Transform.S);values(v.LocalBounds.Origin);values(v.LocalBounds.Extent);
            out.Source.push_back(v.MaterialCount);for(size_t m=0;m<v.MaterialCount;++m){object(v.Materials[m]);out.Source.push_back(v.Overrides[m]);}}
        if(recipe.VisualIncomplete){out.Source.push_back(UINT64_MAX);out.Source.push_back(1);}
        for(size_t i=0;i<recipe.Count;++i)
        {
            const auto p=recipe.Instances[i].Mesh;size_t n=0;
            for(;n<out.TaskCount;++n)if(out.Tasks[n].Resource.Object==p)break;
            if(n==out.TaskCount)
            {
                auto& task=out.Tasks[out.TaskCount++];task.Evidence.Provider=1;
                if(StaticMesh(p,meshType))
                {
                    const auto* object=reinterpret_cast<const SDK::UObject*>(p);
                    task.Resource={p,reinterpret_cast<uintptr_t>(object->Class),recipe.Generation,
                        object->Index,object->Name.ComparisonIndex,object->Name.Number};
                }
            }
            out.Tasks[n].Contributors|=uint16_t(1)<<i;
        }
        out.UniqueMeshes=out.TaskCount;
        for(size_t i=0;i<out.TaskCount;++i)
        {
            auto& t=out.Tasks[i];
            if(baseline&&baseline->Source==out.Source)
                for(size_t n=0;n<baseline->ResourceCount;++n){const auto& r=baseline->Resources[n];
                    if(r.Evidence.Provider==t.Evidence.Provider&&r.ObjectIndex==t.Resource.Index&&
                        r.NameIndex==t.Resource.Name&&r.NameNumber==t.Resource.Number)t.Evidence=r.Evidence;}
            ProviderObservation change;ObserveTask(out,i,change);
        }
        PreserveVisualUncertainty(out.Readiness,recipe.VisualIncomplete);
        out.Reason=recipe.VisualIncomplete?"supported_static_subset_visual_completeness_unverified":"renderer_readiness_unknown_scene_view_material_contract";return out;
    }
}
#endif
