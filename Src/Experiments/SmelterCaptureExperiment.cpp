#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE) && defined(MODLOADER_CLIENT_BUILD)
#include "SmelterCaptureExperiment.h"
#include "StaticAssemblyCaptureCore.h"
#include "SmelterCaptureNativeBindings.h"
#include "ReadbackNativeAdapter.h"
#include "ReadbackNativeBindings.h"
#include "CapturePng.h"
#include "BuildingRepresentationDiagnostic.h"
#include "../Map/BuildingCollector.h"
#include "../plugin_helpers.h"
#include "../Map/Map.h"
#include <Engine_classes.hpp>
#include <Engine_parameters.hpp>
#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstddef>
#include <type_traits>

namespace MiniMapSmelterCapture
{
    namespace Native = MiniMapReadbackNative;
    namespace
    {
        Bindings bindings{};
        bool resolved=false, registered=false, attempted=false, finished=false;
        std::atomic<bool> stopping{false}, cancelled{false};
        std::atomic_flag scheduling=ATOMIC_FLAG_INIT;
        struct Scope { ~Scope(){ scheduling.clear(std::memory_order_release); } };
        IPluginEngineEvents* engine=nullptr;
        IPluginWorldEvents* worlds=nullptr;
        IPluginObjectWalker* invocation=nullptr;
        Native::Job* job=nullptr;
        MiniMapCaptureRetirement::FenceOwner baselineFence,assemblyFence;
        namespace SA=MiniMapStaticAssembly;
        SA::Recipe assemblyRecipe;
        SA::Footprint assemblyBounds;
        SA::Ledger assemblyComponents;
        SDK::UObject* assemblyRoots[SA::Assignments+SA::Limit]{};
        size_t assemblyRootCount=0;
        SDK::UWorld* recipeWorld=nullptr; // comparison token only
        bool assemblyOffered=false,assemblyPending=false,assemblyActive=false,baselineRetired=false;
        bool assemblyAssociationUncertain=false;
        MiniMapCaptureRetirement::FenceOwner& RetirementFence(){return assemblyActive?assemblyFence:baselineFence;}
        bool captureCommitted=false, retirementPending=false;
        SDK::UStaticMesh* asset=nullptr;
        SDK::UStaticMeshComponent* mesh=nullptr;
        SDK::USceneCaptureComponent2D* capture=nullptr;
        SDK::UTextureRenderTarget2D* target=nullptr;
        SDK::UWorld* captureWorld=nullptr;
        SDK::UWorld* endingWorld=nullptr; // game-thread only; never dereferenced
        bool meshRegistered=false, captureRegistered=false;
        uint64_t frame=0;
        uint32_t reported=0;
        constexpr uint32_t Bit(Native::Phase p){return 1u<<uint32_t(p);}
        constexpr int Dimension=256;
        static_assert(sizeof(SDK::UTextureRenderTarget2D)==0x170);
        static_assert(sizeof(SDK::USceneCaptureComponent2D)==0xbf0);
        static_assert(offsetof(SDK::UStaticMesh,ExtendedBounds)==0x240);
        static_assert(offsetof(SDK::UStaticMesh,StaticMaterials)==0x160);
        static_assert(uint8_t(SDK::EPixelFormat::PF_FloatRGBA)==10);
        static_assert(sizeof(SDK::TSubclassOf<SDK::UObject>)==8);

        bool GameThread(){return bindings.GameThread && reinterpret_cast<bool(*)()>(bindings.GameThread)();}
        struct RetirementOperations
        {
            bool Available() const
            {
                return GameThread() && bindings.RetirementConstruct && bindings.RetirementDestroy &&
                    bindings.RetirementBegin && bindings.RetirementPoll;
            }
            bool Construct(void* storage) const
            { reinterpret_cast<void(*)(void*)>(bindings.RetirementConstruct)(storage); return true; }
            bool Begin(void* storage) const
            {
                // Installed PDB ESyncDepth: RenderThread=0, RHIThread=1, Swapchain=2.
                reinterpret_cast<void(*)(void*,int32_t)>(bindings.RetirementBegin)(storage,1); return true;
            }
            bool Poll(void* storage) const
            { return reinterpret_cast<bool(*)(const void*)>(bindings.RetirementPoll)(storage); }
            void Destroy(void* storage) const
            { reinterpret_cast<void(*)(void*)>(bindings.RetirementDestroy)(storage); }
        };
        void Hold(SDK::UObject* object){reinterpret_cast<void(*)(SDK::UObject*)>(bindings.Retain)(object);}
        void Drop(SDK::UObject* object){reinterpret_cast<void(*)(SDK::UObject*)>(bindings.Release)(object);}
        void ReleaseAssemblySources()
        {
            while(assemblyRootCount)
            {auto*& root=assemblyRoots[--assemblyRootCount];Drop(root);root=nullptr;}
            assemblyPending=false;recipeWorld=nullptr;
            assemblyRecipe={};
        }
        void Unregister()
        {
            if(assemblyActive&&assemblyAssociationUncertain)return;
            using Fn=void(*)(SDK::UActorComponent*);
            if(captureRegistered){reinterpret_cast<Fn>(bindings.Unregister)(capture); captureRegistered=false;}
            if(assemblyActive)
            {
                assemblyComponents.Detach([](uintptr_t p){reinterpret_cast<Fn>(bindings.Unregister)(reinterpret_cast<SDK::UActorComponent*>(p));});
                meshRegistered=false;
            }
            else if(meshRegistered){reinterpret_cast<Fn>(bindings.Unregister)(mesh); meshRegistered=false;}
            captureWorld=nullptr;
        }
        void ReleaseObjects()
        {
            if(assemblyActive&&assemblyAssociationUncertain)return;
            // Called only on the game thread, before capture or after native job
            // retirement. Unregistration queues native proxy teardown; UE GC's
            // BeginDestroy/IsReadyForFinishDestroy owns resource-fence retirement.
            Unregister();
            using Destroy=void(*)(SDK::UActorComponent*,bool);
            if(capture){reinterpret_cast<Destroy>(bindings.DestroyComponent)(capture,false); Drop(capture); capture=nullptr;}
            if(assemblyActive)
            {
                assemblyComponents.Release(true,[](uintptr_t p){auto* c=reinterpret_cast<SDK::UActorComponent*>(p);reinterpret_cast<Destroy>(bindings.DestroyComponent)(c,false);Drop(c);});
                mesh=nullptr;
            }
            else if(mesh){reinterpret_cast<Destroy>(bindings.DestroyComponent)(mesh,false); Drop(mesh); mesh=nullptr;}
            if(target){Drop(target);target=nullptr;}
            if(asset){Drop(asset);asset=nullptr;}
            if(assemblyActive)ReleaseAssemblySources();
        }
        SDK::UObject* Find(const wchar_t* path,SDK::UClass* type=nullptr)
        {
            // Native hash/path lookup, never Dumper SDK FindObject/StaticClass.
            const auto address=engine->GetStaticFindObjectByNameAddress();
            using Fn=SDK::UObject*(*)(SDK::UClass*,SDK::UObject*,const wchar_t*,bool);
            return address ? reinterpret_cast<Fn>(address)(type,nullptr,path,false) : nullptr;
        }
        // Exact hash lookup under an explicit native class outer. Never use
        // ObjectWalker's ResolveUFunction/InvokeUFunctionByName (global class scan),
        // SDK GetFunction/GetName, or SDK ProcessEvent wrappers.
        SDK::UFunction* FindFunction(SDK::UClass* functionClass,const wchar_t* ownerPath,
            const wchar_t* name,size_t minimumSize,size_t bufferSize,uint8_t parameters)
        {
            auto* owner=static_cast<SDK::UClass*>(Find(ownerPath));
            if(!owner)return nullptr;
            const auto address=engine->GetStaticFindObjectByNameAddress();
            using Fn=SDK::UObject*(*)(SDK::UClass*,SDK::UObject*,const wchar_t*,bool);
            auto* fn=static_cast<SDK::UFunction*>(reinterpret_cast<Fn>(address)(functionClass,owner,name,true));
            if(!fn||fn->Class!=functionClass||fn->Outer!=owner||!fn->ExecFunction||!(static_cast<uint32_t>(fn->FunctionFlags)&0x400))return nullptr;
            // CL-127004 PDB UFunction: NumParms +0xb4, ParmsSize +0xb6.
            // Accept only the expected tail-padding range, never an oversized frame.
            uint8_t count=0;uint16_t size=0;
            std::memcpy(&count,reinterpret_cast<const uint8_t*>(fn)+0xb4,1);
            std::memcpy(&size,reinterpret_cast<const uint8_t*>(fn)+0xb6,2);
            if(count!=parameters||size<minimumSize||size>bufferSize)return nullptr;
            Hold(fn);return fn;
        }
        struct ComponentFunctions
        {
            SDK::UFunction* Items[4]{};
            ~ComponentFunctions(){for(auto* fn:Items)if(fn)Drop(fn);}
        };
        template<class Params> bool Invoke(SDK::UObject* object,SDK::UFunction* fn,Params& params)
        {
            // v70 public dispatch calls the supplied UFunction directly; no name
            // discovery, UObject enumeration, or MiniMap SDK native address is used.
            return invocation->InvokeResolvedUFunction(object,fn,&params);
        }
        // CL-127004 lowers SpawnObject's by-value TSubclassOf<UObject> to an
        // indirect argument: RCX points at its single UClass* field; RDX is Outer.
        // This is only call-argument storage, not a replacement UE owning type.
        struct SpawnSubclassArgument { SDK::UClass* Class; };
        static_assert(sizeof(SpawnSubclassArgument)==8);
        static_assert(alignof(SpawnSubclassArgument)==8);
        static_assert(offsetof(SpawnSubclassArgument,Class)==0);
        static_assert(std::is_standard_layout_v<SpawnSubclassArgument>);
        static_assert(std::is_trivially_copyable_v<SpawnSubclassArgument>);
        static_assert(offsetof(SDK::UObject,Class)==0x10);

        template<class T> T* Make(SDK::UClass* type,SDK::UObject* outer,const char* label)
        {
            if(!assemblyActive){LOG_INFO("MiniMap: R2 %s construction entered; class=%p outer=%p",label,static_cast<void*>(type),static_cast<void*>(outer));}
            const SpawnSubclassArgument argument{type};
            using Fn=SDK::UObject*(*)(const SpawnSubclassArgument*,SDK::UObject*);
            auto* object=reinterpret_cast<Fn>(bindings.Spawn)(&argument,outer);
            if(!object){LOG_ERROR("MiniMap: R2 %s construction returned null",label);return nullptr;}
            Hold(object);
            // Exact native classes are requested, so pointer equality is sufficient
            // and avoids SDK IsA/name helpers. Protect the result before inspection.
            if(object->Class!=type)
            {
                LOG_ERROR("MiniMap: R2 %s class identity failed; object=%p requested=%p actual=%p; releasing strong reference",
                    label,static_cast<void*>(object),static_cast<void*>(type),static_cast<void*>(object->Class));
                Drop(object);return nullptr;
            }
            reinterpret_cast<void(*)(SDK::UObject*,uint32_t)>(bindings.Flags)(object,0x40);
            if(!assemblyActive){LOG_INFO("MiniMap: R2 %s construction returned nonnull; exact class validated; strong-reference acquisition and RF_Transient application returned",label);}
            return static_cast<T*>(object);
        }
        bool ConfigureAssembly(SDK::UClass* meshClass,SDK::UObject* outer,SDK::UFunction* collisionFunction,const SDK::FVector& anchor)
        {
            static_assert(sizeof(SDK::FTransform)==0x60 && alignof(SDK::FTransform)==16);
            static_assert(offsetof(SDK::FTransform,Rotation)==0 && offsetof(SDK::FTransform,Translation)==0x20 && offsetof(SDK::FTransform,Scale3D)==0x40);
            for(size_t i=0;i<assemblyRecipe.Count;++i)
            {
                auto* c=i?Make<SDK::UStaticMeshComponent>(meshClass,outer,"assembly static instance"):mesh;
                if(!c)return false;
                if(i)assemblyComponents.Entries[assemblyComponents.Count++].Component=reinterpret_cast<uintptr_t>(c);
                if(!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(c))||c->AttachParent)return false;
                const auto& input=assemblyRecipe.Instances[i];
                c->PrimaryComponentTick.bCanEverTick=false;c->bReplicates=false;c->bAutoActivate=false;c->bCanEverAffectNavigation=false;
                c->bGenerateOverlapEvents=false;c->CastShadow=false;c->bAffectDynamicIndirectLighting=false;c->bAffectDistanceFieldLighting=false;
                c->bVisibleInRayTracing=false;c->bVisibleInReflectionCaptures=false;c->bVisibleInRealTimeSkyCaptures=false;
                c->bVisible=input.Visible;c->bHiddenInGame=input.Hidden;
                SDK::Params::PrimitiveComponent_SetCollisionEnabled collision{};collision.NewType=SDK::ECollisionEnabled::NoCollision;
                if(!Invoke(c,collisionFunction,collision))return false;
                using SetMesh=bool(*)(SDK::UStaticMeshComponent*,SDK::UStaticMesh*);
                if(!reinterpret_cast<SetMesh>(bindings.SetMesh)(c,reinterpret_cast<SDK::UStaticMesh*>(input.Mesh)))return false;
                reinterpret_cast<void(*)(SDK::UPrimitiveComponent*,bool)>(bindings.CaptureOnly)(c,true);
                SDK::FTransform t{};
                t.Rotation=SDK::FQuat(input.Transform.R[0],input.Transform.R[1],input.Transform.R[2],input.Transform.R[3]);
                t.Translation=SDK::FVector(anchor.X+input.Transform.T[0],anchor.Y+input.Transform.T[1],anchor.Z+input.Transform.T[2]);
                t.Scale3D=SDK::FVector(input.Transform.S[0],input.Transform.S[1],input.Transform.S[2]);
                // Exact installed SetWorldTransform const FTransform&, false,
                // nullptr, ETeleportType::TeleportPhysics=1. No attachment.
                using SetTransform=void(*)(SDK::USceneComponent*,const SDK::FTransform&,bool,void*,uint8_t);
                reinterpret_cast<SetTransform>(bindings.AssemblySetWorldTransform)(c,t,false,nullptr,1);
                // Verify stored unattached world inputs through the same scene
                // layout already audited in R2.5a. Quaternion sign is equivalent.
                SA::A::Transform actual,expected=input.Transform;
                actual.R=SA::A::FromRotator({c->RelativeRotation.Pitch,c->RelativeRotation.Yaw,c->RelativeRotation.Roll});
                actual.T={c->RelativeLocation.X,c->RelativeLocation.Y,c->RelativeLocation.Z};
                actual.S={c->RelativeScale3D.X,c->RelativeScale3D.Y,c->RelativeScale3D.Z};
                expected.T={t.Translation.X,t.Translation.Y,t.Translation.Z};
                if(!SA::Equivalent(actual,expected))return false;
                using SetMaterial=void(*)(SDK::UMeshComponent*,int32_t,SDK::UMaterialInterface*);
                size_t failedSlot=0;
                if(!MiniMapBuildingRepresentation::ValidateCaptureMaterialSlots(reinterpret_cast<uintptr_t>(c),input.Materials.data(),0,failedSlot))return false;
                for(size_t s=0;s<input.MaterialCount;++s)
                {
                    auto* material=reinterpret_cast<SDK::UMaterialInterface*>(input.Materials[s]);
                    if(!MiniMapBuildingRepresentation::ValidateCaptureObject(input.Materials[s]))return false;
                    reinterpret_cast<SetMaterial>(bindings.AssemblySetMaterial)(c,int32_t(s),material);
                }
                if(!MiniMapBuildingRepresentation::ValidateCaptureMaterialSlots(reinterpret_cast<uintptr_t>(c),input.Materials.data(),input.MaterialCount,failedSlot))
                {LOG_ERROR("MiniMap: R2.5b material assignment failed: instance=%zu slot=%zu",i,failedSlot);return false;}
                size_t overrides=0;for(size_t s=0;s<input.MaterialCount;++s)overrides+=input.Overrides[s];
                LOG_INFO("MiniMap: R2.5b configured static instance %zu: materialSlots=%zu authoredOverrides=%zu; stored transform verified",i,input.MaterialCount,overrides);
            }
            return assemblyComponents.Count==SA::Expected;
        }
        void FailBeforeCapture(const char* reason)
        {
            LOG_ERROR("MiniMap: R2 failed before capture: %s; no retry",reason);
            if(assemblyActive&&!SA::CanCleanBeforeCommit(assemblyAssociationUncertain,
                captureRegistered||!assemblyComponents.Detached(),
                captureWorld&&captureWorld==MiniMapMap::GetWorld()&&captureWorld!=endingWorld))
            {
                finished=true;
                LOG_ERROR("MiniMap: R2.5b component/world association uncertain; resources retained, no stale-world cleanup");
                return;
            }
            Native::DiscardExternalBeforeCapture(job);
            RetirementOperations operations;
            RetirementFence().Release(operations); // constructed, never inserted
            ReleaseObjects(); finished=true;
        }
        bool RecoverRejectedCapture()
        {
            RetirementOperations operations;
            // Detach only through a still-current world or the earlier verified
            // before-EndPlay detachment. No old-world access during polling.
            if (!MiniMapCaptureRetirement::CanStart(captureCommitted && Native::ExternalRejectedWithoutReadback(job),
                Native::CaptureRetirementRendererAvailable(),operations.Available(),meshRegistered || captureRegistered || !assemblyComponents.Detached(),
                captureWorld && captureWorld==MiniMapMap::GetWorld() && captureWorld!=endingWorld)) return false;
            cancelled.store(true,std::memory_order_release);
            Native::Cancel(job);
            LOG_WARN("MiniMap: R2 readback rejected before native enqueue; empty-job recovery eligible: %s",Native::Inspect(job).Failure);
            Unregister();
            // Capture Execute and all detach commands precede this insertion on
            // the same game thread. No subsequent detach is needed by this job.
            if (!RetirementFence().Insert(operations,!meshRegistered && !captureRegistered && assemblyComponents.Detached())) return false;
            retirementPending=true;
            LOG_INFO("MiniMap: R2 capture retirement RHI-depth fence inserted; publication cancelled");
            return true;
        }
        void ServiceCaptureRetirement()
        {
            RetirementOperations operations;
            if (!MiniMapCaptureRetirement::CanPoll(Native::ExternalRejectedWithoutReadback(job),
                Native::CaptureRetirementRendererAvailable(),operations.Available(),!meshRegistered && !captureRegistered && assemblyComponents.Detached()))
            {
                finished=true;
                LOG_ERROR("MiniMap: R2 capture retirement prerequisites uncertain; existing quarantine retained");
                return;
            }
            if (!RetirementFence().Poll(operations)) return; // one nonblocking poll
            LOG_INFO("MiniMap: R2 capture retirement fence complete (RHI submission, not GPU idle)");
            if (!Native::CompleteExternalCaptureRetirement(job) || !RetirementFence().Release(operations) ||
                !Native::DestroyIfRetired(job))
            {
                finished=true;
                LOG_ERROR("MiniMap: R2 capture retirement final ownership check failed; resources retained");
                return;
            }
            ReleaseObjects(); retirementPending=false; finished=true;
            LOG_INFO("MiniMap: R2 failed capture retired: empty CPU job released; components destroyed; strong references released");
        }
        void ExportImage() noexcept
        {
            LOG_INFO("MiniMap: R2 PNG export entered (one-shot CPU image pair)");
            // One-shot CPU-only export on the game thread, after native GPU
            // cleanup/callable retirement and before the CPU job is deleted.
            // Same module-relative path resolution as TerrainCache; no Steam path.
            try
            {
                const auto* pixels=Native::FloatPixels(job);
                if(!pixels){LOG_ERROR("MiniMap: R2 PNG export failed: completed CPU pixels unavailable");return;}
                MiniMapCapturePng::Image image;
                if(!MiniMapCapturePng::Convert(pixels->data(),pixels->size(),Dimension,Dimension,Dimension,Dimension,image))
                {LOG_ERROR("MiniMap: R2 PNG export failed: invalid packed RGBA16F dimensions/bytes");return;}
                HMODULE module=nullptr;
                if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&ExportImage),&module))
                {LOG_ERROR("MiniMap: R2 PNG path resolution failed: GetModuleHandleExW error=%lu",GetLastError());return;}
                std::wstring modulePath(32768,L'\0');
                const auto length=GetModuleFileNameW(module,modulePath.data(),DWORD(modulePath.size()));
                if(!length||length>=modulePath.size())
                {LOG_ERROR("MiniMap: R2 PNG path resolution failed: GetModuleFileNameW error=%lu",GetLastError());return;}
                modulePath.resize(length);
                const auto directory=std::filesystem::path(modulePath).parent_path()/L"MiniMap"/L"Cache"/L"Buildings";
                std::error_code ec;std::filesystem::create_directories(directory,ec);
                if(ec){LOG_ERROR("MiniMap: R2 PNG directory creation failed: %s",ec.message().c_str());return;}
                LOG_INFO("MiniMap: R2 PNG conversion: inverse-opacity alpha; unit-exposure Reinhard + sRGB; no unpremultiply; sanitized channels=%zu",image.Sanitized);
                auto write=[&](const wchar_t* name,const std::vector<uint8_t>& bytes,bool gray)
                {
                    const auto path=directory/name;
                    const auto result=MiniMapCapturePng::Write(path,Dimension,Dimension,bytes,gray);
                    if(result.Ok)
                    {
                        LOG_INFO("MiniMap: R2 %s exported: %ls (%dx%d, %llu bytes)",gray?"Smelter alpha diagnostic":"Smelter PNG",path.c_str(),Dimension,Dimension,(unsigned long long)result.Bytes);
                    }
                    else
                    {
                        LOG_ERROR("MiniMap: R2 PNG export failed: %ls; stage=%s HRESULT=0x%08lx",path.c_str(),result.Stage,(unsigned long)result.Error);
                    }
                };
                write(assemblyActive?L"Smelter_StaticAssembly.png":L"Smelter.png",image.Rgba,false);
                write(assemblyActive?L"Smelter_StaticAssembly_Alpha.png":L"Smelter_Alpha.png",image.Mask,true);
            }
            catch(const std::exception& e){LOG_ERROR("MiniMap: R2 PNG export failed: %s; native cleanup continues",e.what());}
            catch(...){LOG_ERROR("MiniMap: R2 PNG export failed: unknown exception; native cleanup continues");}
        }
        void Submit(SDK::UWorld* world,const SDK::FVector& player)
        {
            attempted=true;
            if(!Native::Preflight()){FailBeforeCapture(Native::Status());return;}
            RetirementOperations retirementOperations;
            if(!RetirementFence().Prepare(retirementOperations)){FailBeforeCapture("retirement fence construction unavailable");return;}
            LOG_INFO("MiniMap: R2 gameplay-world eligibility established via lifecycle pointer; setup entered");
            auto* type=static_cast<SDK::UClass*>(Find(L"/Script/Engine.StaticMesh"));
            auto* meshClass=static_cast<SDK::UClass*>(Find(L"/Script/Engine.StaticMeshComponent"));
            auto* captureClass=static_cast<SDK::UClass*>(Find(L"/Script/Engine.SceneCaptureComponent2D"));
            auto* targetClass=static_cast<SDK::UClass*>(Find(L"/Script/Engine.TextureRenderTarget2D"));
            if(!type||!meshClass||!captureClass||!targetClass){FailBeforeCapture("resident native class metadata unavailable");return;}
            LOG_INFO("MiniMap: R2 resident Smelter lookup entered (no asset load)");
            asset=assemblyActive?reinterpret_cast<SDK::UStaticMesh*>(assemblyRecipe.Instances[0].Mesh):
                static_cast<SDK::UStaticMesh*>(Find(L"/Game/Chimera/Buildings/Smelter/Meshes/SM_Smelter_main_body.SM_Smelter_main_body",type));
            if(!asset){FailBeforeCapture("Smelter mesh is not resident (no load attempted)");return;}
            Hold(asset);
            LOG_INFO("MiniMap: R2 resident Smelter lookup returned a retained asset");
            const auto bounds=asset->ExtendedBounds;
            auto center=bounds.Origin, extent=bounds.BoxExtent;
            if(assemblyActive)
            {
                if(const auto* failure=SA::Validate(assemblyRecipe,assemblyBounds)){FailBeforeCapture(failure);return;}
                center=SDK::FVector((assemblyBounds.Min[0]+assemblyBounds.Max[0])*0.5,(assemblyBounds.Min[1]+assemblyBounds.Max[1])*0.5,(assemblyBounds.Min[2]+assemblyBounds.Max[2])*0.5);
                extent=SDK::FVector((assemblyBounds.Max[0]-assemblyBounds.Min[0])*0.5,(assemblyBounds.Max[1]-assemblyBounds.Min[1])*0.5,(assemblyBounds.Max[2]-assemblyBounds.Min[2])*0.5);
            }
            if(!std::isfinite(center.X)||!std::isfinite(center.Y)||!std::isfinite(center.Z)||
                !std::isfinite(extent.X)||!std::isfinite(extent.Y)||!std::isfinite(extent.Z)||
                extent.X<=0||extent.Y<=0||extent.Z<=0||extent.X>10000||extent.Y>10000||extent.Z>10000||
                (!assemblyActive&&asset->StaticMaterials.Num()!=9)){FailBeforeCapture("mesh bounds/material-slot cross-check failed");return;}
            if(!assemblyActive)for(int i=0;i<9;++i) if(!asset->StaticMaterials[i].MaterialInterface){FailBeforeCapture("authored material slot unavailable");return;}
            auto* outer=reinterpret_cast<SDK::UObject*(*)()>(bindings.Transient)();
            if(!outer){FailBeforeCapture("transient package unavailable");return;}
            using CollisionParams=SDK::Params::PrimitiveComponent_SetCollisionEnabled;
            using TransformParams=SDK::Params::SceneComponent_K2_SetWorldLocationAndRotation;
            using ShowOnlyParams=SDK::Params::SceneCaptureComponent_ShowOnlyComponent;
            using FlagsParams=SDK::Params::SceneCaptureComponent_SetShowFlagSettings;
            static_assert(sizeof(CollisionParams)==1 && sizeof(TransformParams)==0x140);
            static_assert(offsetof(TransformParams,SweepHitResult)==0x38 && offsetof(TransformParams,bTeleport)==0x138);
            static_assert(sizeof(ShowOnlyParams)==8 && sizeof(FlagsParams)==0x10);
            static_assert(sizeof(SDK::FEngineShowFlagsSetting)==0x18 && sizeof(SDK::FString)==0x10);
            static_assert(offsetof(SDK::UFunction,FunctionFlags)==0xb0 && offsetof(SDK::UFunction,ExecFunction)==0xd8);
            auto* functionClass=static_cast<SDK::UClass*>(Find(L"/Script/CoreUObject.Function"));
            if(!functionClass){FailBeforeCapture("native UFunction class metadata unavailable");return;}
            ComponentFunctions functions;
            functions.Items[0]=FindFunction(functionClass,L"/Script/Engine.PrimitiveComponent",L"SetCollisionEnabled",1,sizeof(CollisionParams),1);
            functions.Items[1]=FindFunction(functionClass,L"/Script/Engine.SceneComponent",L"K2_SetWorldLocationAndRotation",0x139,sizeof(TransformParams),5);
            functions.Items[2]=FindFunction(functionClass,L"/Script/Engine.SceneCaptureComponent",L"ShowOnlyComponent",8,sizeof(ShowOnlyParams),1);
            functions.Items[3]=FindFunction(functionClass,L"/Script/Engine.SceneCaptureComponent",L"SetShowFlagSettings",0x10,sizeof(FlagsParams),1);
            for(auto* fn:functions.Items)if(!fn){FailBeforeCapture("exact native function lookup/parameter ABI validation failed");return;}
            if(assemblyActive&&reinterpret_cast<uintptr_t>(functions.Items[2]->ExecFunction)!=bindings.AssemblyShowOnlyExec)
            {FailBeforeCapture("installed show-only wrapper fingerprint mismatch");return;}
            LOG_INFO("MiniMap: R2 UObject creation/configuration entered; exact function lookups validated");
            const SDK::FVector location=assemblyActive?SDK::FVector(0,0,200000):SDK::FVector(player.X,player.Y,player.Z+2000.0);
            mesh=Make<SDK::UStaticMeshComponent>(meshClass,outer,"UStaticMeshComponent");
            if(!mesh){FailBeforeCapture("UStaticMeshComponent construction/validation failed");return;}
            if(assemblyActive)assemblyComponents.Entries[assemblyComponents.Count++].Component=reinterpret_cast<uintptr_t>(mesh);
            if(assemblyActive&&!ConfigureAssembly(meshClass,outer,functions.Items[0],location))
            {FailBeforeCapture("assembly component/material/transform configuration failed");return;}
            capture=Make<SDK::USceneCaptureComponent2D>(captureClass,outer,"USceneCaptureComponent2D");
            if(!capture){FailBeforeCapture("USceneCaptureComponent2D construction/validation failed");return;}
            target=Make<SDK::UTextureRenderTarget2D>(targetClass,outer,"UTextureRenderTarget2D");
            if(!target){FailBeforeCapture("UTextureRenderTarget2D construction/validation failed");return;}
            if(!assemblyActive)
            {
            mesh->PrimaryComponentTick.bCanEverTick=false; mesh->bReplicates=false;
            mesh->bAutoActivate=false; mesh->bCanEverAffectNavigation=false;
            mesh->bGenerateOverlapEvents=false; mesh->CastShadow=false;
            mesh->bAffectDynamicIndirectLighting=false;mesh->bAffectDistanceFieldLighting=false;
            mesh->bVisibleInRayTracing=false;mesh->bVisibleInReflectionCaptures=false;
            mesh->bVisibleInRealTimeSkyCaptures=false;
            mesh->bVisible=true; mesh->bHiddenInGame=false;
            CollisionParams collision{};collision.NewType=SDK::ECollisionEnabled::NoCollision;
            if(!Invoke(mesh,functions.Items[0],collision)){FailBeforeCapture("collision dispatch failed");return;}
            using SetMesh=bool(*)(SDK::UStaticMeshComponent*,SDK::UStaticMesh*);
            if(!reinterpret_cast<SetMesh>(bindings.SetMesh)(mesh,asset)){FailBeforeCapture("SetStaticMesh failed");return;}
            reinterpret_cast<void(*)(SDK::UPrimitiveComponent*,bool)>(bindings.CaptureOnly)(mesh,true);
            // Near the active player for normal world lighting. No collision,
            // navigation, shadows, gameplay owner or main-view visibility.
            TransformParams meshTransform{};meshTransform.NewLocation=location;
            meshTransform.NewRotation=SDK::FRotator(0,0,0);meshTransform.bTeleport=true;
            if(!Invoke(mesh,functions.Items[1],meshTransform)){FailBeforeCapture("mesh transform dispatch failed");return;}
            }
            capture->PrimaryComponentTick.bCanEverTick=false; capture->bReplicates=false;
            capture->bAutoActivate=false; capture->bCanEverAffectNavigation=false;
            capture->bCaptureEveryFrame=false;capture->bCaptureOnMovement=false;
            capture->bAlwaysPersistRenderingState=false;capture->bUseRayTracingIfEnabled=false;
            capture->ProjectionType=SDK::ECameraProjectionMode::Orthographic;
            capture->PrimitiveRenderMode=SDK::ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
            capture->CaptureSource=SDK::ESceneCaptureSource::SCS_SceneColorHDR;
            capture->CompositeMode=SDK::ESceneCaptureCompositeMode::SCCM_Overwrite;
            capture->OrthoWidth=float(2.0*(std::max)(extent.X,extent.Y)*1.1);
            capture->bAutoCalculateOrthoPlanes=true;capture->bUpdateOrthoPlanes=true;
            capture->bUseCameraHeightAsViewTarget=false; capture->bEnableOrthographicTiling=false;
            capture->bRenderInMainRenderer=false;capture->bMainViewFamily=false;
            capture->bMainViewResolution=false;capture->bMainViewCamera=false;
            capture->bInheritMainViewCameraPostProcessSettings=false;
            capture->PostProcessBlendWeight=0;capture->bEnableClipPlane=false;
            capture->bUseCustomProjectionMatrix=false;
            SDK::FEngineShowFlagsSetting flags[10]{};
            const wchar_t* names[]={L"Fog",L"Atmosphere",L"VolumetricFog",L"Cloud",L"MotionBlur",L"TemporalAA",L"AntiAliasing",L"PostProcessing",L"Bloom",L"EyeAdaptation"};
            for(int i=0;i<10;++i){flags[i].ShowFlagName=SDK::FString(names[i]);flags[i].Enabled=false;}
            // Borrowed SDK view is input only; native setter makes its own copy.
            FlagsParams showFlags{};showFlags.InShowFlagSettings=SDK::TArray<SDK::FEngineShowFlagsSetting>(flags,10,10);
            if(!Invoke(capture,functions.Items[3],showFlags)){FailBeforeCapture("show-flags dispatch failed");return;}
            target->bAutoGenerateMips=false;target->bSupportsUAV=false;
            // CL PDB: UTextureRenderTarget::bNeedsTwoCopies is uint32 bit 0
            // at +0x144 (SDK hides this native field in padding).
            static_assert(offsetof(SDK::UTextureRenderTarget,Pad_144)==0x144);
            uint32_t targetFlags=0;
            std::memcpy(&targetFlags,reinterpret_cast<uint8_t*>(target)+0x144,4);
            targetFlags &= ~uint32_t(1);
            std::memcpy(reinterpret_cast<uint8_t*>(target)+0x144,&targetFlags,4);
            target->ClearColor=SDK::FLinearColor(0,0,0,1);
            using Init=void(*)(SDK::UTextureRenderTarget2D*,uint32_t,uint32_t,uint8_t,bool);
            reinterpret_cast<Init>(bindings.InitTarget)(target,Dimension,Dimension,10,true);
            capture->TextureTarget=target;
            const SDK::FVector camera(location.X+center.X,location.Y+center.Y,location.Z+center.Z+extent.Z+1000.0);
            // UE basis: pitch=-90/yaw=-90 => forward -Z, right +X, up -Y.
            TransformParams cameraTransform{};cameraTransform.NewLocation=camera;
            cameraTransform.NewRotation=SDK::FRotator(-90,-90,0);cameraTransform.bTeleport=true;
            if(!Invoke(capture,functions.Items[1],cameraTransform)){FailBeforeCapture("capture transform dispatch failed");return;}
            LOG_INFO("MiniMap: R2 UObject configuration calls returned; %dx%d PF_FloatRGBA target initialized",Dimension,Dimension);
            void* scene=nullptr;std::memcpy(&scene,reinterpret_cast<const uint8_t*>(world)+0x258,8); // CL PDB UWorld::Scene
            if(!scene){FailBeforeCapture("world render scene unavailable");return;}
            using Register=void(*)(SDK::UActorComponent*,SDK::UWorld*,void*);
            LOG_INFO("MiniMap: R2 component registration entered");
            captureWorld=world;
            if(assemblyActive)
            {
                for(size_t i=0;i<assemblyComponents.Count;++i)
                {
                    auto& entry=assemblyComponents.Entries[i];auto* c=reinterpret_cast<SDK::UStaticMeshComponent*>(entry.Component);
                    reinterpret_cast<Register>(bindings.Register)(c,world,nullptr);
                    uint8_t state=0;std::memcpy(&state,reinterpret_cast<const uint8_t*>(c)+0x94,1);
                    entry.Registered=(state&1)!=0;
                    SDK::UWorld* current=nullptr;std::memcpy(&current,reinterpret_cast<const uint8_t*>(c)+0xa8,8);
                    if(!entry.Registered||current!=world)
                    {
                        assemblyAssociationUncertain=current!=world&&(entry.Registered||current);
                        FailBeforeCapture("assembly component registration failed");return;
                    }
                }
            }
            else {reinterpret_cast<Register>(bindings.Register)(mesh,world,nullptr);meshRegistered=true;}
            reinterpret_cast<Register>(bindings.Register)(capture,world,nullptr);captureRegistered=true;
            if(assemblyActive)
            {
                uint8_t state=0;std::memcpy(&state,reinterpret_cast<const uint8_t*>(capture)+0x94,1);
                captureRegistered=(state&1)!=0;
            }
            SDK::UWorld* registeredMeshWorld=nullptr;SDK::UWorld* registeredCaptureWorld=nullptr;
            std::memcpy(&registeredMeshWorld,reinterpret_cast<const uint8_t*>(mesh)+0xa8,8);
            std::memcpy(&registeredCaptureWorld,reinterpret_cast<const uint8_t*>(capture)+0xa8,8);
            if(assemblyActive)
            {
                assemblyAssociationUncertain=registeredMeshWorld!=world||
                    (registeredCaptureWorld!=world&&(captureRegistered||registeredCaptureWorld));
                if(assemblyAssociationUncertain||!captureRegistered)
                {FailBeforeCapture("assembly capture component/world registration failed");return;}
            }
            if(registeredMeshWorld!=world||registeredCaptureWorld!=world){FailBeforeCapture("component world registration failed");return;}
            LOG_INFO("MiniMap: R2 component registration returned; both world pointers validated");
            if(assemblyActive)
            {
                uintptr_t members[SA::Limit]{};
                for(size_t i=0;i<assemblyComponents.Count;++i)
                {
                    auto& entry=assemblyComponents.Entries[i];members[i]=entry.Component;
                    if(!MiniMapBuildingRepresentation::ValidateCaptureMembership(reinterpret_cast<uintptr_t>(capture),members,i))
                    {FailBeforeCapture("assembly show-only prefix invalid before append");return;}
                    ShowOnlyParams showOnly{};showOnly.InComponent=reinterpret_cast<SDK::UPrimitiveComponent*>(entry.Component);
                    if(!Invoke(capture,functions.Items[2],showOnly)){FailBeforeCapture("assembly show-only dispatch failed");return;}
                    entry.Member=true;
                }
                if(!assemblyComponents.Complete()||!MiniMapBuildingRepresentation::ValidateCaptureMembership(reinterpret_cast<uintptr_t>(capture),members,assemblyComponents.Count))
                {FailBeforeCapture("assembly show-only membership mismatch");return;}
            }
            else
            {
                ShowOnlyParams showOnly{};showOnly.InComponent=mesh;
                if(!Invoke(capture,functions.Items[2],showOnly)){FailBeforeCapture("show-only dispatch failed");return;}
            }
            auto* resource=reinterpret_cast<void*(*)(SDK::UTextureRenderTarget2D*)>(bindings.TargetResource)(target);
            job=Native::CreateExternalJob(resource,Dimension,Dimension);
            if(!job){FailBeforeCapture("target resource or CPU readback allocation unavailable");return;}
            const auto& native=Native::GetBindings();
            void* builder=reinterpret_cast<void*(*)(size_t,uint32_t)>(native.Malloc)(0x20,8);
            if(!builder){FailBeforeCapture("builder storage allocation failed");return;}
            const auto snapshot=assemblyActive?MiniMapBuildingCollector::GetSnapshot():nullptr;
            if(!GameThread() || world!=MiniMapMap::GetWorld() || world==endingWorld ||
                (assemblyActive&&(!snapshot||!snapshot->Complete||snapshot->Generation!=assemblyRecipe.Generation||!assemblyComponents.Complete()))||
                !Native::CaptureRetirementRendererAvailable())
            {
                reinterpret_cast<void(*)(void*)>(native.Free)(builder);
                FailBeforeCapture("final pre-commit world/RHI prerequisite changed");return;
            }
            LOG_INFO("MiniMap: R2 capture-builder submission entered");
            if(assemblyActive)LOG_INFO("MiniMap: R2.5b isolated static assembly: instances=%zu uniqueMeshes=7 width=%.3f; fixed anchor=(0,0,200000); skeletal excluded",assemblyComponents.Count,double(capture->OrthoWidth));
            reinterpret_cast<void(*)(void*,void*)>(bindings.BuilderConstruct)(builder,scene);
            reinterpret_cast<void(*)(SDK::USceneCaptureComponent2D*,void*,void*)>(bindings.UpdateCapture)(capture,scene,builder);
            // This is the commit boundary: all subsequent failures retain the
            // rooted target/components until real GPU retirement is established.
            reinterpret_cast<void(*)(void*)>(bindings.BuilderExecute)(builder);
            captureCommitted=true;
            const bool committedJob=Native::MarkExternalCaptureCommitted(job);
            reinterpret_cast<void(*)(void*)>(bindings.BuilderDestroy)(builder);
            reinterpret_cast<void(*)(void*)>(native.Free)(builder);
            if(!committedJob){finished=true;Native::Cancel(job);LOG_ERROR("MiniMap: R2 committed-job identity uncertain; resources retained");return;}
            LOG_INFO("MiniMap: R2 capture queued: %dx%d PF_FloatRGBA, %s; bounds XY=%.3fx%.3f center=(%.3f,%.3f) orthoWidth=%.3f",
                Dimension,Dimension,assemblyActive?"ten static authored instances":"9 authored materials",extent.X*2,extent.Y*2,center.X,center.Y,double(capture->OrthoWidth));
            LOG_INFO("MiniMap: R2 GPU readback scheduling entered (copy submission reported separately)");
            Native::Service(job,++frame);
        }
        void BeforeEndPlay(SDK::UWorld* world,const char*)
        {
            if(!GameThread()){LOG_ERROR("MiniMap: R2 before-EndPlay is not game thread; component retirement NOT certified");return;}
            if(scheduling.test_and_set(std::memory_order_acquire)){LOG_ERROR("MiniMap: R2 before-EndPlay overlaps capture scheduling; retirement NOT certified");return;}
            Scope scope;
            endingWorld=world; // stop eligibility before native EndPlay, even before any capture
            if(!assemblyActive&&world==recipeWorld)ReleaseAssemblySources();
            if(world!=captureWorld)return;
            cancelled.store(true,std::memory_order_release);
            Native::Cancel(job);
            if(assemblyActive&&assemblyAssociationUncertain)
            {LOG_ERROR("MiniMap: R2.5b world transition: uncertain component association retained; no unregistration attempted");return;}
            Unregister(); // before native world EndPlay; roots and job remain.
            LOG_WARN("MiniMap: R2 world transition: publication cancelled, components unregistered; nonblocking native retirement continues");
        }
        void Tick(float)
        {
            if(stopping.load()||!GameThread()||scheduling.test_and_set(std::memory_order_acquire))return;
            Scope scope;
            if(finished)
            {
                if(assemblyActive)return;
                if(!baselineRetired||job){if(assemblyPending)ReleaseAssemblySources();return;}
                if(!assemblyPending)return;
                const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
                if(!recipeWorld||recipeWorld!=MiniMapMap::GetWorld()||recipeWorld==endingWorld||!snapshot||
                    !snapshot->Complete||snapshot->Generation!=assemblyRecipe.Generation||
                    !MiniMapBuildingRepresentation::ValidateCaptureObject(assemblyRecipe.ActorClass))
                {ReleaseAssemblySources();LOG_WARN("MiniMap: R2.5b pending authored recipe cancelled: world/generation changed; no retry");return;}
                assemblyActive=true;assemblyPending=false;finished=false;attempted=false;captureCommitted=false;
                retirementPending=false;cancelled.store(false,std::memory_order_release);reported=0;frame=0;
                LOG_INFO("MiniMap: R2.5b static-template experiment eligible: ten instances, seven meshes; original R2 retired; skeletal omitted");
            }
            if(!attempted)
            {
                auto* world=MiniMapMap::GetWorld();SDK::FVector player{};
                // Map publishes only the host's ChimeraMain-filtered lifecycle pointer.
                // All reads/clears and submission are serialized on the game thread.
                if(!world||world==endingWorld||(!assemblyActive&&!MiniMapMap::TryGetPlayerWorldPosition(player)))return;
                if(world!=MiniMapMap::GetWorld())return;
                if(!assemblyActive)Submit(world,player);
                else
                {
                    try {Submit(world,player);}
                    catch(...)
                    {
                        if(!captureCommitted)FailBeforeCapture("assembly setup exception before commitment");
                        else {Native::Cancel(job);finished=true;LOG_ERROR("MiniMap: R2.5b exception after commitment; native state uncertain, resources retained");}
                    }
                }
                return;
            }
            if(!job)return;
            if(retirementPending)
            {
                try { ServiceCaptureRetirement(); }
                catch(...) { finished=true;LOG_ERROR("MiniMap: R2 capture retirement exception; resources retained"); }
                return;
            }
            Native::Service(job,++frame); // never consult the old world here.
            const auto report=Native::Inspect(job);
            if((report.Visited&Bit(Native::Phase::CopySubmitted))&&!(reported&Bit(Native::Phase::CopySubmitted)))
            {reported|=Bit(Native::Phase::CopySubmitted);LOG_INFO("MiniMap: R2 GPU readback copy submitted; later-frame readiness polling");}
            if(report.Quarantined)
            {
                try { if(RecoverRejectedCapture())return; }
                catch(...) { LOG_ERROR("MiniMap: R2 capture-retirement native state uncertain; quarantine retained"); }
                finished=true;LOG_ERROR("MiniMap: R2 recovery rejected; existing quarantine retained: %s",report.Failure?report.Failure:"unknown");return;
            }
            if(report.Current!=Native::Phase::CleanupComplete||report.Callables||report.Outstanding)return;
            if(!cancelled.load())
            {
                if(const auto* stats=Native::FloatStats(job))
                {
                    LOG_INFO("MiniMap: R2 readback %dx%d pitchPixels=%d height=%d latency=%.3fms/%llu frames; finite=%llu nonfinite=%llu RGB=[%.6g,%.6g] rawAlpha=[%.6g,%.6g] candidate inverse-alpha foreground=%llu background=%llu partial=%llu coverageSum=%.3f",
                        Dimension,Dimension,report.Pitch,report.BufferHeight,report.CompletionMs,(unsigned long long)report.CompletionFrames,
                        (unsigned long long)stats->Finite,(unsigned long long)stats->Nonfinite,double(stats->RgbMin),double(stats->RgbMax),double(stats->AlphaMin),double(stats->AlphaMax),
                        (unsigned long long)stats->Foreground,(unsigned long long)stats->Background,(unsigned long long)stats->Partial,stats->Coverage);
                    for(size_t i=0;i<stats->SampleCount;++i){const auto& p=stats->Samples[i];LOG_INFO("MiniMap: R2 sample (%zu,%zu) rawRGBA=(%.6g,%.6g,%.6g,%.6g)",p.X,p.Y,double(p.Rgba[0]),double(p.Rgba[1]),double(p.Rgba[2]),double(p.Rgba[3]));}
                    if(!stats->Foreground||!stats->Background) LOG_WARN("MiniMap: R2 image evidence inconclusive: candidate foreground/background separation absent");
                }
                if(report.Result!=Native::Outcome::Verified)
                {
                    LOG_ERROR("MiniMap: R2 readback failed: %s",report.Failure?report.Failure:"unknown");
                }
                else
                {
                    ExportImage();
                }
            }
            RetirementOperations retirementOperations;
            if(RetirementFence().Release(retirementOperations) && Native::DestroyIfRetired(job))
            {ReleaseObjects();finished=true;if(!assemblyActive)baselineRetired=report.Result==Native::Outcome::Verified&&!cancelled.load();LOG_INFO("MiniMap: R2 cleanup complete: no outstanding callable, readback released, components destroyed and strong references released");}
        }
        void BeginShutdown()
        {
            stopping.store(true,std::memory_order_release);cancelled.store(true,std::memory_order_release);
            // Shared job pointer is inspected only under the scheduling guard.
            if(!scheduling.test_and_set(std::memory_order_acquire))
            {Scope scope;Native::Cancel(job);if(GameThread())Unregister();}
        }
    }
    const Bindings& GetBindings(){return bindings;}
    bool OfferStaticAssembly(const MiniMapStaticAssembly::Recipe& recipe,void* world)
    {
#if !defined(MINIMAP_STAGE_R25B_STATIC_ASSEMBLY)
        (void)recipe;(void)world;return false;
#else
        if(!GameThread()||stopping.load()||assemblyOffered||scheduling.test_and_set(std::memory_order_acquire))return false;
        Scope scope;assemblyOffered=true;
        const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
        SA::Footprint footprint;
        const auto* reason=SA::Validate(recipe,footprint);
        if(!reason&&(!bindings.AssemblySetMaterial||!bindings.AssemblySetWorldTransform||!bindings.AssemblyShowOnlyExec))reason="native_assembly_bindings_unavailable";
        if(!reason&&(!snapshot||!snapshot->Complete||snapshot->Generation!=recipe.Generation||world!=MiniMapMap::GetWorld()||world==endingWorld))reason="world_generation_changed";
        if(reason){LOG_WARN("MiniMap: R2.5b experimental_static_template_eligibility rejected: %s; no retry",reason);return false;}
        auto retain=[&](uintptr_t p)
        {
            if(!MiniMapBuildingRepresentation::ValidateCaptureObject(p))return false;
            auto* object=reinterpret_cast<SDK::UObject*>(p);
            for(size_t i=0;i<assemblyRootCount;++i)if(assemblyRoots[i]==object)return true;
            if(assemblyRootCount>=std::size(assemblyRoots))return false;
            Hold(object);assemblyRoots[assemblyRootCount++]=object;return true;
        };
        if(!retain(recipe.ActorClass)){ReleaseAssemblySources();return false;}
        for(size_t i=0;i<recipe.Count;++i)
        {
            if(!retain(recipe.Instances[i].Mesh)){ReleaseAssemblySources();return false;}
            for(size_t s=0;s<recipe.Instances[i].MaterialCount;++s)
                if(!retain(recipe.Instances[i].Materials[s])){ReleaseAssemblySources();return false;}
        }
        assemblyRecipe=recipe;assemblyBounds=footprint;recipeWorld=static_cast<SDK::UWorld*>(world);assemblyPending=true;
        LOG_INFO("MiniMap: R2.5b validated authored recipe handed off with independent strong roots; waiting for original R2 retirement");
        return true;
#endif
    }
    bool ResolvePrerequisites()
    {
        if(!Native::MatchesCaptureRetirementBuild())
        {LOG_WARN("MiniMap: R2 capture-retirement executable identity mismatch; R2 disabled");return false;}
        struct Pattern{const char* Name;const char* Bytes;uintptr_t Bindings::*Field;bool Leaf;};
        const Pattern patterns[]={
#include "SmelterCapturePatterns.inl"
        };
        Bindings candidate{};
        for(const auto& p:patterns)
        {
            const auto address=Native::ResolveOptionalFunction(p.Bytes,p.Leaf);
            if(!address){LOG_WARN("MiniMap: R2 optional prerequisite failed: %s; R2 disabled",p.Name);return false;}
            candidate.*p.Field=address;
        }
        bindings=candidate;resolved=true;
#if defined(MINIMAP_STAGE_R25B_STATIC_ASSEMBLY)
        struct AssemblyPattern{const char* Bytes;uintptr_t Bindings::*Field;};
        const AssemblyPattern assemblyPatterns[]={
#include "StaticAssemblyCapturePatterns.inl"
        };
        for(const auto& p:assemblyPatterns)bindings.*p.Field=Native::ResolveOptionalFunction(p.Bytes,false);
        LOG_INFO("MiniMap: R2.5b additional native bindings %s; single-mesh baseline independent",
            bindings.AssemblySetMaterial&&bindings.AssemblySetWorldTransform&&bindings.AssemblyShowOnlyExec?"resolved":"unavailable");
#endif
        LOG_INFO("MiniMap: R2 optional native signatures resolved (22), including retirement fence; no capture submitted");return true;
    }
    void Initialize()
    {
        if(!resolved||registered)return;
        auto* hooks=GetHooks();
        if(!hooks||!hooks->Engine||!hooks->World)return;
        engine=hooks->Engine;worlds=hooks->World;invocation=hooks->ObjectWalker;
        if(!invocation||!invocation->InvokeResolvedUFunction||!engine->RegisterOnTick||!engine->UnregisterOnTick||!engine->RegisterOnShutdown||!engine->UnregisterOnShutdown||
            !engine->GetStaticFindObjectByNameAddress||!engine->GetStaticFindObjectByNameAddress()||
            !worlds->RegisterOnBeforeWorldEndPlay||!worlds->UnregisterOnBeforeWorldEndPlay)
        {LOG_WARN("MiniMap: R2 lifecycle/resident lookup prerequisites unavailable; disabled");return;}
        engine->RegisterOnTick(&Tick);engine->RegisterOnShutdown(&BeginShutdown);
        worlds->RegisterOnBeforeWorldEndPlay(&BeforeEndPlay);registered=true;
        LOG_INFO("MiniMap: R2 enabled: waiting for ChimeraMain/player; one resident Smelter lookup and one capture attempt per process");
    }
    void Shutdown()
    {
        BeginShutdown();
        if(registered){engine->UnregisterOnTick(&Tick);engine->UnregisterOnShutdown(&BeginShutdown);worlds->UnregisterOnBeforeWorldEndPlay(&BeforeEndPlay);registered=false;}
        if(scheduling.test_and_set(std::memory_order_acquire)){LOG_ERROR("MiniMap: R2 shutdown overlaps callback; DLL unload NOT certified");return;}
        Scope scope;
        Native::Cancel(job);
        if(!GameThread()){if(mesh||capture||target||job)LOG_ERROR("MiniMap: R2 shutdown off game thread: resources retained; cleanup NOT certified");return;}
        if(Native::DestroyIfRetired(job))ReleaseObjects();
        else LOG_ERROR("MiniMap: R2 shutdown with pending native work: resources retained, no wait; callback code is NOT safe for FreeLibrary; active hot reload unsupported");
    }
}
#endif
