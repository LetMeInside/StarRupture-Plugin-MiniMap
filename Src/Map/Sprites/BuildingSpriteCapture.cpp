#include "CallbackTiming.h"
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE) && defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSpriteCapture.h"
#include "PrivateCaptureWorld.h"
#include "SceneCaptureConfiguration.h"
#include "StaticAssemblyCaptureCore.h"
#include "SpriteCaptureCore.h"
#include "RendererReadiness.h"
#include "SpriteCandidateIdentity.h"
#include "Native/NativeApi.h"
#include "Native/ReadbackNativeAdapter.h"
#include "CapturePng.h"
#include "BuildingRepresentation.h"
#include "Map/Buildings/BuildingCollector.h"
#include "plugin_helpers.h"
#include "Map/Map.h"
#include "BuildingSprites.h"
#include "CaptureLeaseCore.h"
#include <Engine_classes.hpp>
#include <Engine_parameters.hpp>
#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <memory>

namespace MiniMapBuildingCapture
{
    namespace Native = MiniMapReadbackNative;
    namespace
    {
        MiniMapSpriteCapture::PrivateCaptureWorld privateWorld;
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
        struct CandidateRequest
        {
            MiniMapSpriteCapture::Candidate Candidate;
            MiniMapBuildingCollector::Record Source; // CPU-owned metadata only.
            MiniMapCaptureLeases::Token Lease;
            // A validated authored recipe is not a renderer-readiness proof.
            // No installed contract currently certifies all meshes, materials,
            // effective LOD/Nanite selection and scene proxies. Unknown permits
            // provisional display, never complete-quality certification.
            MiniMapRendererReadiness::AssemblyObservation Observation;
            bool PublicationFinished=false;
        };
        CandidateRequest* candidateRequest=nullptr; // Owned with the assembly roots.
        MiniMapCaptureLeases::Token ownerLease; // survives renderer generation reset and quarantine
        void Drop(SDK::UObject* object);
        struct SkeletalTest
        {
            CandidateRequest Request;
            SA::Recipe SourceRecipe;
            SDK::UWorld* World=nullptr; // comparison token, never dereferenced
            std::array<SDK::UObject*,SA::Slots+4> Roots{};
            size_t RootCount=0;
            ~SkeletalTest(){while(RootCount)Drop(Roots[--RootCount]);}
        };
        SkeletalTest* pendingSkeletalTest=nullptr;
        void RetirePendingSkeletalTest(){delete pendingSkeletalTest;pendingSkeletalTest=nullptr;}
        struct ResourceWatch
        {
            CandidateRequest Request;SA::Recipe Recipe;SA::Footprint Bounds;SDK::UWorld* World=nullptr;
            MiniMapRendererReadiness::Ticket Tracking;
            std::array<std::chrono::steady_clock::time_point,16> NextEligible{};
            size_t TaskCursor{},Samples{},ChangedTask{};bool Retiring=false,NeedsPublication=false,PublicationDeferred=false;
            std::array<uint64_t,5> PublicationWake{};
            std::array<SDK::UObject*,SA::Assignments+SA::Limit> Roots{};size_t Count{};
            ~ResourceWatch(){Tracking.Cancel();while(Count)Drop(Roots[--Count]);}
        };
        std::array<ResourceWatch*,32> resourceWatches{}; // explicit GT retirement, no static owning destructor
        size_t resourceCursor=0;
        std::chrono::steady_clock::time_point nextResourceService{};
        void ClearResourceWatches(){for(size_t i=0;i<resourceWatches.size();++i)if(resourceWatches[i]){
            MiniMapBuildingSprites::WatchState(resourceWatches[i]->Request.Candidate.Identity,MiniMapRendererReadiness::Dependencies::Watch::None);
            delete resourceWatches[i];resourceWatches[i]=nullptr;}resourceCursor=0;nextResourceService={};}
        void DisposeResourceWatch(size_t index){if(auto* w=resourceWatches[index])
            MiniMapBuildingSprites::WatchState(w->Request.Candidate.Identity,MiniMapRendererReadiness::Dependencies::Watch::None);
            delete resourceWatches[index];resourceWatches[index]=nullptr;}
        bool RetireResourceWatch(size_t index,bool invalidated=false)
        {
            auto* w=resourceWatches[index];if(!w)return true;
            if(w->NeedsPublication&&!invalidated)
            {
                using Ack=MiniMapRendererReadiness::NotificationAck;
                auto proof=w->Request.Observation.Tasks[w->ChangedTask].Evidence;proof.RequiresEvaluation=false;
                const auto ack=MiniMapBuildingSprites::NotifyProviderChange(w->Request.Candidate.Identity,proof,w->Request.Observation.Source);
                if(ack==Ack::Rejected){w->PublicationDeferred=true;
                    w->PublicationWake=MiniMapBuildingSprites::PublicationWake(w->Request.Candidate.Identity);return false;}
            }
            w->NeedsPublication=false;
            w->Tracking.Cancel();w->Retiring=true;
            MiniMapBuildingSprites::WatchState(w->Request.Candidate.Identity,MiniMapRendererReadiness::Dependencies::Watch::Retiring);
            w->Recipe={};w->World=nullptr;
            for(auto& t:w->Request.Observation.Tasks)t.Resource={};
            if(!w->Count)DisposeResourceWatch(index);return true;
        }
        // Native roots are released explicitly on the game thread, never by a
        // static destructor during an uncertified DLL unload.
        bool skeletalTestObserved=false,skeletalOnly=false; // one attempt per plugin lifetime
        uint64_t staticEvidenceGeneration=0;
        size_t staticEvidenceReports=0; // at most eight static captures per generation
        bool staticEvidenceActive=false;
        using CaptureClock=MiniMapCallbackTiming::Clock;
        CaptureClock::time_point componentsRegisteredAt{},captureSubmittedAt{};
        bool publicStaticCapture=false;

        // A.4.9l TEMPORARY NEXT-TICK STATIC CAPTURE. Same owner and retained objects.
        uint64_t captureServiceSequence=0;
        struct PreparedStaticCapture
        {
            bool Pending=false;
            // A.4.9w TEMPORARY GENERIC VIEW PREPARATION: no preliminary readback.
            bool PreliminaryInvoked=false,Cancelling=false;
            uint64_t PreliminarySequence=0;
            Native::Job* Job=nullptr;
            SDK::UWorld* World=nullptr;
            SDK::UClass* MeshClass=nullptr;
            SDK::UClass* CaptureClass=nullptr;
            SDK::UClass* TargetClass=nullptr;
            SDK::UFunction* Function=nullptr; // owns the existing FindPublicCapture retain
            void* Resource=nullptr;
            std::array<double,3> Extent{},Center{};
            uint64_t ServiceSequence=0;
            CaptureClock::time_point PreparedAt{};
        } preparedStatic;
        void ClearPreparedStatic()
        {
            if(preparedStatic.Function)Drop(preparedStatic.Function);
            preparedStatic={};
        }

        SA::Footprint assemblyBounds;
        SA::Ledger assemblyComponents;
        SDK::UObject* assemblyRoots[SA::Assignments+SA::Limit]{};
        size_t assemblyRootCount=0;
        SDK::UWorld* recipeWorld=nullptr; // comparison token only
        bool assemblyPending=false,assemblyActive=false,baselineRetired=false;
        bool assemblyAssociationUncertain=false;
        MiniMapCaptureRetirement::FenceOwner& RetirementFence(){return assemblyActive?assemblyFence:baselineFence;}
        bool captureCommitted=false, retirementPending=false;
        SDK::UObject* asset=nullptr;
        SDK::UMeshComponent* mesh=nullptr;
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

        bool GameThread(){return MiniMapNative::Get() && MiniMapNative::Get()->player.isGameThread && MiniMapNative::Get()->player.isGameThread();}
        struct RetirementOperations
        {
            bool Available() const
            {
                return GameThread() && MiniMapNative::Get()->texture.renderFenceConstruct && MiniMapNative::Get()->texture.renderFenceDestroy &&
                    MiniMapNative::Get()->texture.renderFenceBegin && MiniMapNative::Get()->texture.renderFencePoll;
            }
            bool Construct(void* storage) const
            { MiniMapNative::Get()->texture.renderFenceConstruct(storage); return true; }
            bool Begin(void* storage) const
            {
                // Installed PDB ESyncDepth: RenderThread=0, RHIThread=1, Swapchain=2.
                MiniMapNative::Get()->texture.renderFenceBegin(storage,1); return true;
            }
            bool Poll(void* storage) const
            { return MiniMapNative::Get()->texture.renderFencePoll(storage); }
            void Destroy(void* storage) const
            { MiniMapNative::Get()->texture.renderFenceDestroy(storage); }
        };
        void Hold(SDK::UObject* object){MiniMapNative::Get()->asset.retainObject(object);}
        void Drop(SDK::UObject* object){MiniMapNative::Get()->asset.releaseObject(object);}
        void FailSpritePublication(const char* reason) noexcept
        {
#if defined(MINIMAP_BUILDING_SPRITES)
            try {if(candidateRequest&&!skeletalOnly&&!candidateRequest->PublicationFinished)
                MiniMapBuildingSprites::RecordForLease(candidateRequest->Lease,MiniMapBuildingSpriteCore::State::Failed,reason);}catch(...){
                if(candidateRequest)MiniMapBuildingSprites::AbandonCapture(candidateRequest->Lease);}
#else
            (void)reason;
#endif
        }
        void ReleaseAssemblySources()
        {
            if(assemblyAssociationUncertain)return; // never retry an uncertain native release
            FailSpritePublication("capture_ended_without_ready_sprite");
            while(assemblyRootCount)
            {
                auto*& root=assemblyRoots[assemblyRootCount-1];
                try{Drop(root);}
                catch(...){assemblyAssociationUncertain=true;LOG_ERROR("MiniMap: source-root release uncertain; native lease quarantined");return;}
                root=nullptr;--assemblyRootCount;
            }
            assemblyPending=false;recipeWorld=nullptr;
            assemblyRecipe={};
            delete candidateRequest;candidateRequest=nullptr;
#if defined(MINIMAP_BUILDING_SPRITES)
            // This function is also used for pending-request rollback. An
            // outcome/renderer release alone cannot certify native retirement.
            // Job deletion/discard and fence release happen at the existing
            // checked cleanup endpoints BEFORE this final root/request cleanup.
            const auto fence=assemblyFence.GetState();
            MiniMapCaptureLeases::RetirementReceipt receipt{
                job==nullptr,
                fence==MiniMapCaptureRetirement::FenceState::Empty||fence==MiniMapCaptureRetirement::FenceState::Destroyed,
                !mesh&&!capture&&!target&&!asset&&!meshRegistered&&!captureRegistered&&!assemblyComponents.Count,
                assemblyRootCount==0,
                !preparedStatic.Pending&&!preparedStatic.Function,
                candidateRequest==nullptr,
                !assemblyAssociationUncertain};
            if(ownerLease&&receipt.Certified())
            {
                if(MiniMapBuildingSprites::ReturnLease(ownerLease,receipt))
                {LOG_INFO("MiniMap: native capture lease retired: generation=%llu request=%llu lease=%llu",
                    (unsigned long long)ownerLease.Generation,(unsigned long long)ownerLease.RequestSerial,(unsigned long long)ownerLease.LeaseSerial);ownerLease={};}
                else LOG_ERROR("MiniMap: exact capture lease retirement rejected; ownership retained");
            }
#endif
        }
        MiniMapSpriteCapture::Decision CheckCandidate(CandidateRequest& request,const SA::Recipe& recipe,void* world,bool retained)
        {
            namespace P=MiniMapSpriteCapture;
            try
            {
            auto& candidate=request.Candidate;
            if(!candidate.General||candidate.General!=recipe.General)return {P::State::Rejected,"candidate_recipe_mode_mismatch",P::Cause::RecipeInvalid};
            if(candidate.Classification.ExcludeFromMiniMap==P::K::Exclusion::Exclude||candidate.Classification.Reason=="InteriorTag")
                return {P::State::Rejected,"classification_excluded",P::Cause::Policy};
            const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
            const auto copied=MiniMapSpriteCandidates::Copy(request.Source);
            P::Evidence evidence;
            evidence.SourceCurrent=world&&world==MiniMapMap::GetWorld()&&world!=endingWorld&&snapshot&&snapshot->Complete&&
                snapshot->Generation==recipe.Generation&&candidate.Identity.Generation==recipe.Generation&&
                request.Source.Generation==recipe.Generation;
            if(evidence.SourceCurrent&&copied.DefinitionValid&&copied.VisualInputsValid&&copied.Identity==candidate.Identity)
            {
                P::K::Observation fresh;
                evidence.SourceValidated=MiniMapBuildingRepresentation::RevalidateCaptureSource(request.Source,fresh);
                candidate.Classification=std::move(fresh.CachedPolicy);
                candidate.ClassificationObserved=evidence.SourceValidated;
                candidate.Truncated|=fresh.Truncated;
            }
            if(!MiniMapBuildingRepresentation::ValidateCaptureObject(recipe.ActorClass))candidate.RepresentationVerified=false;
            else
            {
                const auto* klass=reinterpret_cast<const SDK::UClass*>(recipe.ActorClass);
                candidate.RepresentationVerified&=klass->Index==candidate.ActorObject&&
                    klass->Name.ComparisonIndex==candidate.ActorName&&klass->Name.Number==candidate.ActorNumber;
            }
            SA::Footprint bounds;evidence.RecipeCheck=SA::ValidateTyped(recipe,bounds);
            evidence.RecipeVerified=evidence.RecipeCheck.Failure==SA::RecipeFailure::None;
            RetirementOperations retirement;
            const bool gameThread=GameThread(),renderer=Native::CaptureRetirementRendererAvailable(),fence=retirement.Available();
            const bool ownership=MiniMapNative::Get()->asset.retainObject&&MiniMapNative::Get()->asset.releaseObject;
            const bool setters=MiniMapNative::Get()->player.setMaterial&&MiniMapNative::Get()->player.setWorldTransform&&MiniMapNative::Get()->texture.showOnlyComponentExec;
            evidence.LifecycleReady=gameThread&&!stopping.load()&&renderer&&fence&&ownership&&setters;
            if(!evidence.LifecycleReady)
                LOG_WARN("MiniMap: capture lifecycle rejected: gameThread=%d renderer=%d fence=%d ownership=%d setters=%d stopping=%d",
                    int(gameThread),int(renderer),int(fence),int(ownership),int(setters),int(stopping.load()));
            evidence.SourcesRetained=retained;
            return P::Decide(candidate,evidence,true);
            }
            catch(...){return {P::State::Rejected,"candidate_revalidation_exception",P::Cause::OperationalException};}
        }
        void WatchResources(const CandidateRequest& request,const SA::Recipe& recipe,SDK::UWorld* world)
        {
            // An existing ticket retains its own resource generation. Replacement
            // roots retire incrementally; the CPU demand waits for that disposal.
            for(size_t i=0;i<resourceWatches.size();++i)
                if(auto* w=resourceWatches[i];w&&w->Request.Candidate.Identity==request.Candidate.Identity)
                {RetireResourceWatch(i);return;}
            if(!request.Observation.Readiness.Pending)return; // Unknown-only is never polled.
            for(auto& slot:resourceWatches)if(!slot)
            {
                auto watch=std::make_unique<ResourceWatch>();watch->Request=request;watch->Recipe=recipe;watch->Bounds=assemblyBounds;watch->World=world;
                watch->Tracking.Apply(request.Observation.Readiness,1);
                const auto now=std::chrono::steady_clock::now();
                for(size_t t=0;t<request.Observation.TaskCount;++t)
                    if(const auto* p=MiniMapRendererReadiness::FindProvider(request.Observation.Tasks[t].Evidence.Provider))
                        watch->NextEligible[t]=now+std::chrono::milliseconds(p->PollMilliseconds);
                // Assembly admission has already retained and deduplicated these sources.
                for(size_t n=0;n<assemblyRootCount;++n)
                {Hold(assemblyRoots[n]);watch->Roots[watch->Count++]=assemblyRoots[n];}
                slot=watch.release();
                MiniMapBuildingSprites::WatchState(request.Candidate.Identity,MiniMapRendererReadiness::Dependencies::Watch::Active);return;
            }
            LOG_WARN("MiniMap: resource observation capacity reached; provisional quality remains uncertified");
        }
        void PublishResourceObservation(ResourceWatch& watch,size_t index)
        {
            const auto& fresh=watch.Request.Observation;
            const auto& change=fresh.Tasks[watch.ChangedTask].Evidence;
            // A CPU publication exception leaves the sampled proof in the rooted
            // ticket. Replay publication before another sample can overwrite it.
            MiniMapBuildingSprites::ObserveAssembly(watch.Request.Candidate.Identity,fresh);
            auto notification=change;notification.RequiresEvaluation=false;
            using Ack=MiniMapRendererReadiness::NotificationAck;
            const auto ack=MiniMapBuildingSprites::NotifyProviderChange(watch.Request.Candidate.Identity,notification,fresh.Source);
            if(ack==Ack::Rejected){watch.PublicationDeferred=true;
                watch.PublicationWake=MiniMapBuildingSprites::PublicationWake(watch.Request.Candidate.Identity);return;}
            if(ack==Ack::Invalidated){RetireResourceWatch(index,true);return;}
            watch.NeedsPublication=watch.PublicationDeferred=false;++watch.Samples;
            watch.Tracking.Apply(fresh.Readiness,watch.Tracking.Revision+1);
            if(change.RecaptureEligible||!fresh.Readiness.Pending||
                (watch.Samples>=16&&MiniMapBuildingSprites::WaitingDemand()))RetireResourceWatch(index);
        }
        bool PrepareResourcePublication(const MiniMapSpriteDiscoveryCore::Key& key,
            const MiniMapRendererReadiness::AssemblyObservation& incoming)
        {
            for(size_t i=0;i<resourceWatches.size();++i)if(auto* w=resourceWatches[i];w&&w->Request.Candidate.Identity==key&&w->NeedsPublication)
            {
                if(!incoming.Source.empty()&&incoming.Source!=w->Request.Observation.Source)
                {RetireResourceWatch(i,true);return true;} // fresh rooted/validated recipe proves source replacement
                // A source-admission attempt is meaningful new eligibility. Resolve
                // the exact old publication before a new snapshot can overwrite it.
                PublishResourceObservation(*w,i);
                return !resourceWatches[i]||!resourceWatches[i]->NeedsPublication;
            }
            return true;
        }
        void ServiceResourceWatch(bool observe=true) // scheduling guard held; retirement also services a busy owner
        {
            if(observe&&(!MiniMapMap::GameplayReady()||assemblyPending||pendingSkeletalTest))return;
            namespace R=MiniMapRendererReadiness;
            const auto now=std::chrono::steady_clock::now();if(now<nextResourceService)return;
            nextResourceService=now+std::chrono::milliseconds(50);
            MiniMapBuildingSprites::TimingScope timing(MiniMapBuildingSprites::Timing::ReadinessService);
            const auto start=std::chrono::steady_clock::now();
            // Four ticket slots, at most sixteen CPU task descriptors and ONE
            // prepared native receiver. No recipe/class audit in sampling.
            ResourceWatch* watch=nullptr;size_t index=0;
            for(size_t n=0;n<4;++n)
            {index=resourceCursor++%resourceWatches.size();if(resourceWatches[index]){watch=resourceWatches[index];break;}}
            if(!watch)return;
            if(watch->Retiring)
            {
                // Native release can itself cost CPU. Never drain a complete
                // assembly's material roots inside one sampling invocation.
                for(size_t n=0;n<4&&watch->Count;++n)
                {
                    if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()>=0.5)return;
                    Drop(watch->Roots[--watch->Count]);watch->Roots[watch->Count]=nullptr;
                }
                if(!watch->Count)DisposeResourceWatch(index);
                return;
            }
            if(!observe)return; // independent roots may retire while capture owns its own references
#if defined(MINIMAP_BUILDING_SPRITES)
            if(!MiniMapBuildingSprites::WatchCurrent(watch->Request.Candidate.Identity,watch->Request.Observation.Source))
            {RetireResourceWatch(index,true);return;}
#endif
            const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
            if(watch->World!=MiniMapMap::GetWorld()||watch->World==endingWorld||
                (snapshot&&snapshot->Generation!=watch->Recipe.Generation))
            {RetireResourceWatch(index,true);return;}
            // A partial/unavailable collector snapshot is not source invalidation.
            if(!snapshot||!snapshot->Complete)return;
            if(watch->NeedsPublication)
            {
                const auto wake=MiniMapBuildingSprites::PublicationWake(watch->Request.Candidate.Identity);
                if(watch->PublicationDeferred&&wake==watch->PublicationWake)return;
                // Gate exceptional retries too; no publication work on every idle tick.
                watch->PublicationDeferred=true;watch->PublicationWake=wake;
                PublishResourceObservation(*watch,index);return;
            }
            if(watch->Samples>=16&&MiniMapBuildingSprites::WaitingDemand()){RetireResourceWatch(index);return;}
            auto& fresh=watch->Request.Observation;
            {
                for(size_t n=0;n<fresh.TaskCount;++n)
                {
                    if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()>=0.5)return;
                    const size_t t=watch->TaskCursor++%fresh.TaskCount;const auto& old=fresh.Tasks[t].Evidence;
                    const auto* provider=R::FindProvider(old.Provider);
                    if(!provider||!old.Pending||!old.CanChange||old.Method!=R::ObservationMethod::BoundedPolling||now<watch->NextEligible[t])continue;
                    // Advance eligibility BEFORE native work, including exceptional exits.
                    watch->NextEligible[t]=now+std::chrono::milliseconds(provider->PollMilliseconds);
                    R::ProviderObservation change;
                    try
                    {
                        MiniMapBuildingSprites::TimingScope sample(MiniMapBuildingSprites::Timing::ReadinessSample);
                        if(!R::ObserveTask(fresh,t,change))
                        {R::InvalidateTask(fresh,t);change=fresh.Tasks[t].Evidence;}
                    }
                    catch(...)
                    {
                        R::InvalidateTask(fresh,t);
#if defined(MINIMAP_BUILDING_SPRITES)
                        try{MiniMapBuildingSprites::ObserveAssembly(watch->Request.Candidate.Identity,fresh);}catch(...){}
#endif
                        if(!fresh.Readiness.Pending)RetireResourceWatch(index);
                        throw;
                    }
                    watch->ChangedTask=t;watch->NeedsPublication=true;watch->PublicationDeferred=true;
                    watch->PublicationWake=MiniMapBuildingSprites::PublicationWake(watch->Request.Candidate.Identity);
                    PublishResourceObservation(*watch,index);
                    return;
                }
                if(!fresh.Readiness.Pending)RetireResourceWatch(index);
                return;
            }
        }

        void Unregister()
        {
            if(assemblyAssociationUncertain)return;
            try
            {

            if(captureRegistered){MiniMapNative::Get()->player.unregisterComponent(capture); captureRegistered=false;}
            if(assemblyActive)
            {
                assemblyComponents.Detach([](uintptr_t p){MiniMapNative::Get()->player.unregisterComponent(reinterpret_cast<SDK::UActorComponent*>(p));});
                meshRegistered=false;
            }
            else if(meshRegistered){MiniMapNative::Get()->player.unregisterComponent(mesh); meshRegistered=false;}
            captureWorld=nullptr;
            }
            catch(...){assemblyAssociationUncertain=true;LOG_ERROR("MiniMap: capture detachment uncertain; native lease quarantined");}
        }
        void ReleaseObjects()
        {
            if(assemblyAssociationUncertain)return;
            try
            {
            // Called only on the game thread, before capture or after native job
            // retirement. Unregistration queues native proxy teardown; UE GC's
            // BeginDestroy/IsReadyForFinishDestroy owns resource-fence retirement.
            Unregister();
            if(assemblyAssociationUncertain)return;

            if(capture){MiniMapNative::Get()->player.destroyComponent(capture,false); Drop(capture); capture=nullptr;}
            if(assemblyActive)
            {
                assemblyComponents.Release(true,[](uintptr_t p){auto* c=reinterpret_cast<SDK::UActorComponent*>(p);MiniMapNative::Get()->player.destroyComponent(c,false);Drop(c);});
                mesh=nullptr;
            }
            else if(mesh){MiniMapNative::Get()->player.destroyComponent(mesh,false); Drop(mesh); mesh=nullptr;}
            if(target){Drop(target);target=nullptr;}
            if(asset){Drop(asset);asset=nullptr;}
            if(assemblyActive)ReleaseAssemblySources();
            }
            catch(...){assemblyAssociationUncertain=true;LOG_ERROR("MiniMap: capture cleanup uncertain; native lease quarantined");}
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
            SDK::UFunction* Items[5]{};
            ~ComponentFunctions(){for(auto* fn:Items)if(fn)Drop(fn);}
        };
        SDK::UFunction* FindPublicCapture(SDK::UClass* functionClass,SDK::UClass* captureClass)
        {
            if(!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(captureClass)))return nullptr;
            auto* fn=FindFunction(functionClass,L"/Script/Engine.SceneCaptureComponent2D",L"CaptureScene",0,0,0);
            if(!fn)return nullptr;
            // No arguments, return value or property frame. The installed public
            // wrapper flushes World::SendAllEndOfFrameUpdates before its builder.
            MEMORY_BASIC_INFORMATION memory{};
            const bool valid=MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(fn))&&
                MiniMapNative::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(fn),sizeof(SDK::UFunction),8)&&
                fn->Outer==captureClass&&!fn->ChildProperties&&!(uint32_t(fn->FunctionFlags)&0x2000)&&
                (MiniMapNative::RecordVirtualQuery(),VirtualQuery(reinterpret_cast<const void*>(fn->ExecFunction),&memory,sizeof(memory)))&&
                memory.State==MEM_COMMIT&&!(memory.Protect&(PAGE_GUARD|PAGE_NOACCESS))&&
                (memory.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
            if(!valid){Drop(fn);return nullptr;}
            return fn;
        }
        template<class Params> bool Invoke(SDK::UObject* object,SDK::UFunction* fn,Params& params)
        {
            // v70 public dispatch calls the supplied UFunction directly; no name
            // discovery, UObject enumeration, or MiniMap SDK native address is used.
            return invocation->InvokeResolvedUFunction(object,fn,&params);
        }
        // Read-only reflection observations, never admission/readiness predicates.
        // No generated wrappers, render-resource pointers, or new native calls.
        std::string CapturePropertyEvidence(SDK::UObject* object,bool material=false)
        {
            namespace J=MiniMapRepresentationCore;
            auto* p=GetHooks()?GetHooks()->ObjectProperties:nullptr;
            if(!object||!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(object))||
                !p||!p->IsReady||!p->IsReady()||!p->FindPropertyOnObject||!p->GetPropertyKind||!p->GetPropertyArrayDim)
                return "{\"status\":\"reflection_or_object_unavailable\"}";
            J::JsonArray fields;fields.Limit=4096;
            const char* componentFields[]={"bVisible","bHiddenInGame","bVisibleInSceneCaptureOnly","bHiddenInSceneCapture",
                "bRenderInMainPass","bRenderInDepthPass","bReverseCulling","bDisallowNanite","bForceDisableNanite",
                "bEvaluateWorldPositionOffset","bOverrideMinLOD","ForcedLodModel","MinLOD","DetailMode",
                "MinDrawDistance","CachedMaxDrawDistance","BoundsScale"};
            const char* materialFields[]={"BlendMode","TwoSided","OpacityMaskClipValue","bDisableDepthTest"};
            const auto fieldsToRead=material?materialFields:componentFields;
            const size_t count=material?std::size(materialFields):std::size(componentFields);
            for(size_t field=0;field<count;++field)
            {
                const auto* name=fieldsToRead[field];
                auto property=p->FindPropertyOnObject(object,name);std::string value="null";
                if(property&&p->GetPropertyArrayDim(property)==1)
                {
                    const auto kind=p->GetPropertyKind(property);
                    bool b=false;int64_t i=0;double f=0;
                    if(kind==PluginPropertyKind::Bool&&p->GetBoolProperty&&p->GetBoolProperty(object,property,&b))value=J::Bool(b);
                    else if((kind==PluginPropertyKind::Int||kind==PluginPropertyKind::Enum)&&p->GetIntProperty&&p->GetIntProperty(object,property,&i))value=std::to_string(i);
                    else if(kind==PluginPropertyKind::Float&&p->GetFloatProperty&&p->GetFloatProperty(object,property,&f)&&std::isfinite(f))value=J::Number(f);
                }
                fields.Add(J::Object({{"property",J::Quote(name)},{"value",value}}));
            }
            return J::Object({{"fields",fields.String()},{"null_means",J::Quote("absent_or_no_verified_typed_accessor")}});
        }
        std::string CaptureCustomDataEvidence(SDK::UPrimitiveComponent* component)
        {
            namespace J=MiniMapRepresentationCore;
            static_assert(offsetof(SDK::UPrimitiveComponent,CustomPrimitiveData)==0x2b8&&
                offsetof(SDK::UPrimitiveComponent,CustomPrimitiveDataInternal)==0x2c8);
            auto copy=[&](size_t offset)
            {
                J::ArrayHeader header{};
                if(!MiniMapNative::ReadMemory(reinterpret_cast<uintptr_t>(component)+offset,&header,sizeof(header)))return std::string("null");
                const auto bound=J::BoundArray(header,sizeof(float),alignof(float),32);
                if(!bound.Valid||bound.Truncated||(bound.Bytes&&!MiniMapNative::IsMemoryRangeReadable(header.Data,bound.Bytes,alignof(float))))return std::string("null");
                J::JsonArray values;
                for(size_t i=0;i<bound.Count;++i)
                {
                    float value=0;
                    std::memcpy(&value,reinterpret_cast<const void*>(header.Data+i*sizeof(float)),sizeof(value));
                    if(!std::isfinite(value))return std::string("null");
                    values.Add(J::Number(value));
                }
                return values.String();
            };
            return J::Object({{"authored",copy(offsetof(SDK::UPrimitiveComponent,CustomPrimitiveData))},
                {"internal",copy(offsetof(SDK::UPrimitiveComponent,CustomPrimitiveDataInternal))}});
        }
        void ReportStaticCaptureEvidence(const SDK::FVector& camera,const SDK::FVector& center,const SDK::FVector& extent,const SDK::FVector& anchor) noexcept
        {
            staticEvidenceActive=false;
            if(!assemblyActive||skeletalOnly||!candidateRequest||!GameThread())return;
            if(staticEvidenceGeneration!=assemblyRecipe.Generation)
            {staticEvidenceGeneration=assemblyRecipe.Generation;staticEvidenceReports=0;}
            if(staticEvidenceReports>=8)return;
            ++staticEvidenceReports;staticEvidenceActive=true;
            try
            {
                LOG_INFO("MiniMap: static capture evidence A4.4a %zu/8: generation=%llu building=%u blueprint=%s members=%zu camera=(%.3f,%.3f,%.3f) rotation=(-90,-90,0) centerLocal=(%.3f,%.3f,%.3f) extent=(%.3f,%.3f,%.3f) orthoWidth=%.3f distanceToBounds=[1000,%.3f] autoPlanes=%d updatePlanes=%d maxViewDistance=%.3f; registration/show-only verified separately; render-proxy/LOD-residency readiness unavailable; no timing/settings change",
                    staticEvidenceReports,(unsigned long long)assemblyRecipe.Generation,candidateRequest->Source.BuildingId,
                    candidateRequest->Candidate.CanonicalBlueprint?candidateRequest->Candidate.CanonicalBlueprint->c_str():"unresolved",
                    assemblyComponents.Count,camera.X,camera.Y,camera.Z,center.X,center.Y,center.Z,extent.X,extent.Y,extent.Z,
                    double(capture->OrthoWidth),1000.0+2.0*extent.Z,int(capture->bAutoCalculateOrthoPlanes),int(capture->bUpdateOrthoPlanes),double(capture->MaxViewDistanceOverride));
                LOG_INFO("MiniMap: static capture camera readback: unattached=%d storedLocation=(%.3f,%.3f,%.3f) storedRotation=(%.3f,%.3f,%.3f) customProjection=%d clipPlane=%d lodDistanceFactor=%.3f target=%dx%d; component inputs only, effective renderer view unobserved",
                    int(capture->AttachParent==nullptr),capture->RelativeLocation.X,capture->RelativeLocation.Y,capture->RelativeLocation.Z,
                    capture->RelativeRotation.Pitch,capture->RelativeRotation.Yaw,capture->RelativeRotation.Roll,
                    int(capture->bUseCustomProjectionMatrix),int(capture->bEnableClipPlane),double(capture->LODDistanceFactor),target->SizeX,target->SizeY);
                // Independent input-space framing check using the actual stored camera
                // and the validated assembly AABB. For (-90,-90,0), screen right is
                // +X, screen down is +Y, and forward is -Z. This does not inspect
                // the renderer's auto-adjusted ortho planes or prove proxy readiness.
                const auto& stored=capture->RelativeLocation;
                const auto& rotation=capture->RelativeRotation;
                const double width=capture->OrthoWidth;
                const bool basisVerified=capture->AttachParent==nullptr&&
                    std::abs(rotation.Pitch+90.0)<1e-6&&std::abs(rotation.Yaw+90.0)<1e-6&&std::abs(rotation.Roll)<1e-6&&
                    !capture->bUseCustomProjectionMatrix&&target->SizeX==target->SizeY&&
                    std::isfinite(width)&&width>0&&std::isfinite(stored.X)&&std::isfinite(stored.Y)&&std::isfinite(stored.Z);
                if(basisVerified)
                {
                    const double u0=0.5+(anchor.X+assemblyBounds.Min[0]-stored.X)/width;
                    const double u1=0.5+(anchor.X+assemblyBounds.Max[0]-stored.X)/width;
                    const double v0=0.5+(anchor.Y+assemblyBounds.Min[1]-stored.Y)/width;
                    const double v1=0.5+(anchor.Y+assemblyBounds.Max[1]-stored.Y)/width;
                    const double nearDepth=stored.Z-anchor.Z-assemblyBounds.Max[2];
                    const double farDepth=stored.Z-anchor.Z-assemblyBounds.Min[2];
                    const bool inside=u0>=0&&u1<=1&&v0>=0&&v1<=1&&nearDepth>0&&farDepth>=nearDepth;
                    LOG_INFO("MiniMap: static capture framing A4.4b: generation=%llu building=%u inputBoundsUV=[%.6f,%.6f]-[%.6f,%.6f] inputDepthCm=[%.3f,%.3f] insideInputXYAndForward=%d; stored camera and validated bounds only; effective clip planes, scene proxy, render-data/PSO readiness and shader output unobserved; no capture settings changed",
                        (unsigned long long)assemblyRecipe.Generation,candidateRequest->Source.BuildingId,u0,v0,u1,v1,nearDepth,farDepth,int(inside));
                }
                else LOG_WARN("MiniMap: static capture framing A4.4b unavailable: stored camera basis/projection prerequisites differ; capture behavior unchanged");
                size_t materialReports=0;
                for(size_t i=0;i<(std::min)(assemblyComponents.Count,size_t(4));++i)
                {
                    auto* component=reinterpret_cast<SDK::UStaticMeshComponent*>(assemblyComponents.Entries[i].Component);
                    if(!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(component)))continue;
                    const auto& input=assemblyRecipe.Instances[i];
                    if(!MiniMapBuildingRepresentation::ValidateCaptureObject(input.Mesh))continue;
                    LOG_INFO("MiniMap: static capture component evidence: instance=%zu meshObject=%u component=%s customData=%s; mesh/transform/material assignment already verified",
                        i,static_cast<unsigned>(reinterpret_cast<SDK::UObject*>(input.Mesh)->Index),
                        CapturePropertyEvidence(component).c_str(),CaptureCustomDataEvidence(component).c_str());
                    // These are public SDK input flags on already retained, validated
                    // static assets/components. They are NOT a render readiness gate.
                    static_assert(offsetof(SDK::UStaticMeshComponent,ForcedLodModel)==0x578&&sizeof(SDK::UStreamableRenderAsset)==0xd0);
                    const auto* meshAsset=reinterpret_cast<const SDK::UStaticMesh*>(input.Mesh);
                    LOG_INFO("MiniMap: static quality inputs A4.8: instance=%zu forcedLodModel=%d (0=automatic,1=request_LOD0) neverStream=%d streamingUpdatePending=%d; effectiveLOD=unverified LOD0Residency=unverified renderData/proxy/PSO=unverified Nanite=unverified; capture route and nonempty readback do not prove full detail; retries=disabled",
                        i,component->ForcedLodModel,int(meshAsset->NeverStream),int(meshAsset->bHasStreamingUpdatePending));
                    LOG_INFO("MiniMap: static capture component transform readback: instance=%zu unattached=%d T=(%.3f,%.3f,%.3f) rotation=(%.3f,%.3f,%.3f) scale=(%.3f,%.3f,%.3f)",
                        i,int(component->AttachParent==nullptr),component->RelativeLocation.X,component->RelativeLocation.Y,component->RelativeLocation.Z,
                        component->RelativeRotation.Pitch,component->RelativeRotation.Yaw,component->RelativeRotation.Roll,
                        component->RelativeScale3D.X,component->RelativeScale3D.Y,component->RelativeScale3D.Z);
                    for(size_t slot=0;slot<input.MaterialCount&&materialReports<8;++slot,++materialReports)
                    {
                        auto* material=reinterpret_cast<SDK::UObject*>(input.Materials[slot]);
                        if(!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(material)))continue;
                        // Only validated, explicitly retained reflected Parent links.
                        // A base material is descriptive evidence, not proof of effective overrides or opacity.
                        SDK::UObject* current=material;SDK::UObject* roots[3]{};size_t count=0;
                        struct Release {SDK::UObject** Roots;size_t& Count;~Release(){while(Count)Drop(Roots[--Count]);}} release{roots,count};
                        for(size_t depth=0;depth<4;++depth)
                        {
                            const int32_t classIndex=current->Class&&MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(current->Class))?current->Class->Index:-1;
                            if(classIndex<0){LOG_WARN("MiniMap: static capture material evidence unavailable: class identity invalid; capture behavior unchanged");break;}
                            LOG_INFO("MiniMap: static capture material evidence: instance=%zu slot=%zu parentDepth=%zu object=%d class=%d properties=%s; authored/parent observations only, shader output unverified",
                                i,slot,depth,current->Index,classIndex,CapturePropertyEvidence(current,true).c_str());
                            auto* p=GetHooks()?GetHooks()->ObjectProperties:nullptr;
                            if(depth==3||!p||!p->IsReady||!p->IsReady()||!p->FindPropertyOnObject||!p->GetPropertyKind||
                                !p->GetPropertyArrayDim||!p->GetObjectProperty)break;
                            auto property=p->FindPropertyOnObject(current,"Parent");void* parent=nullptr;
                            if(!property||p->GetPropertyKind(property)!=PluginPropertyKind::Object||p->GetPropertyArrayDim(property)!=1||
                                !p->GetObjectProperty(current,property,&parent)||!parent||parent==current||
                                !MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(parent)))break;
                            auto* next=static_cast<SDK::UObject*>(parent);Hold(next);roots[count++]=next;current=next;
                        }
                    }
                }
            }
            catch(...){LOG_WARN("MiniMap: static capture evidence unavailable; capture behavior unchanged");}
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
            auto* object=reinterpret_cast<Fn>(MiniMapNative::Get()->asset.spawnObject)(&argument,outer);
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
            MiniMapNative::Get()->asset.setObjectFlags(object,0x40);
            if(!assemblyActive){LOG_INFO("MiniMap: R2 %s construction returned nonnull; exact class validated; strong-reference acquisition and RF_Transient application returned",label);}
            return static_cast<T*>(object);
        }
        // CL-127004 SDK layouts cross-checked against the installed PDB. This
        // bounded walk inspects only the exact function's parameters, never UObjects
        // globally. The SDK's padded C++ argument buffer is NOT the live frame size.
        bool SkeletalPropertiesAvailable(SDK::UObject* object)
        {
            auto* p=GetHooks()?GetHooks()->ObjectProperties:nullptr;
            if(!p||!p->IsReady||!p->IsReady()||!p->FindPropertyOnObject||!p->GetPropertyKind||
                !p->GetPropertyArrayDim||!p->GetBoolProperty)return false;
            for(const auto* name:{"bRenderStatic","bDisablePostProcessBlueprint","bAllowClothActors"})
            {
                auto property=p->FindPropertyOnObject(object,name);
                if(!property||p->GetPropertyKind(property)!=PluginPropertyKind::Bool||p->GetPropertyArrayDim(property)!=1)
                {LOG_WARN("MiniMap: skeletal-only reflected Bool property unavailable: %s; no component created",name);return false;}
            }
            return true;
        }
        struct SkeletalFunctions
        {
            enum Id {Assign,RenderStatic,AnimationMode,AnimClass,ForcedLOD,PostProcess,Cloth,Physics,
                Bounds,MeshRead,AnimRead,PostRead,ForcedRead,PredictedRead,Count};
            std::array<SDK::UFunction*,Count> Items{};
            ~SkeletalFunctions(){for(auto* fn:Items)if(fn)Drop(fn);}
            struct Part {int Offset,Size;SDK::EClassCastFlags Kind;const wchar_t* Type=nullptr;bool Return=false;};
            bool ResolveOne(Id id,SDK::UClass* functionClass,const wchar_t* owner,const wchar_t* name,
                size_t minimum,size_t capacity,std::initializer_list<Part> parts)
            {
                static_assert(offsetof(SDK::UStruct,ChildProperties)==0x50);
                static_assert(offsetof(SDK::FProperty,Offset)==0x44&&sizeof(SDK::FProperty)==0x70);
                static_assert(offsetof(SDK::FField,Next)==0x18&&offsetof(SDK::FFieldClass,CastFlags)==0x10);
                static_assert(offsetof(SDK::FBoolProperty,FieldSize)==0x70&&offsetof(SDK::FObjectPropertyBase,PropertyClass)==0x70);
                auto read=[](const void* p,size_t bytes){return MiniMapNative::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(p),bytes,8);};
                auto* declaring=Find(owner);
                if(!declaring||!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(declaring)))return false;
                auto* fn=Items[id]=FindFunction(functionClass,owner,name,minimum,capacity,uint8_t(parts.size()));
                if(!fn||!read(fn,sizeof(SDK::UFunction))||fn->Outer!=declaring||
                    (uint32_t(fn->FunctionFlags)&0x2000))return false; // instance functions only
                MEMORY_BASIC_INFORMATION memory{};
                if(!(MiniMapNative::RecordVirtualQuery(),VirtualQuery(reinterpret_cast<const void*>(fn->ExecFunction),&memory,sizeof(memory)))||
                    memory.State!=MEM_COMMIT||(memory.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
                    !(memory.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
                uint16_t frameSize=0;std::memcpy(&frameSize,reinterpret_cast<const uint8_t*>(fn)+0xb6,2);
                unsigned seen=0;size_t visits=0;
                for(auto* field=fn->ChildProperties;field;field=field->Next)
                {
                    if(++visits>parts.size()||!read(field,sizeof(SDK::FProperty))||!read(field->ClassPrivate,0x18))return false;
                    const auto* prop=reinterpret_cast<const SDK::FProperty*>(field);
                    uintptr_t owning=0;std::memcpy(&owning,&prop->Owner,8);
                    if((owning&~uintptr_t(1))!=reinterpret_cast<uintptr_t>(fn)||!(owning&1)||
                        !(prop->PropertyFlags&0x80)||prop->ArrayDim!=1)return false;
                    size_t n=0;bool matched=false;
                    for(const auto& part:parts)
                    {
                        if(prop->Offset!=part.Offset){++n;continue;}
                        if((seen&(1u<<n))||prop->ElementSize!=part.Size||prop->Offset<0||
                            size_t(prop->Offset)+size_t(prop->ElementSize)>frameSize||
                            bool(prop->PropertyFlags&0x400)!=part.Return||
                            (!part.Return&&(prop->PropertyFlags&(0x100|0x8000000))))return false;
                        const auto kind=prop->ClassPrivate->CastFlags;
                        const bool enumeration=part.Kind==SDK::EClassCastFlags::EnumProperty;
                        if(!(kind&uint64_t(part.Kind))&&!(enumeration&&(kind&uint64_t(SDK::EClassCastFlags::ByteProperty))))return false;
                        if(part.Kind==SDK::EClassCastFlags::BoolProperty)
                        {
                            if(!read(prop,sizeof(SDK::FBoolProperty)))return false;
                            const auto* b=reinterpret_cast<const SDK::FBoolProperty*>(prop);
                            if(b->FieldSize!=1||b->ByteOffset||b->FieldMask!=0xff||!b->ByteMask)return false;
                        }
                        if(part.Type)
                        {
                            auto* expected=Find(part.Type);
                            if(!expected||!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(expected)))return false;
                            SDK::UObject* actual=nullptr;
                            if(enumeration&&(kind&uint64_t(SDK::EClassCastFlags::EnumProperty)))
                            {
                                if(!read(prop,sizeof(SDK::FEnumProperty)))return false;
                                auto* e=reinterpret_cast<const SDK::FEnumProperty*>(prop);
                                actual=e->Enum;
                                if(!read(e->UnderlayingProperty,sizeof(SDK::FProperty))||e->UnderlayingProperty->ElementSize!=1||
                                    !read(e->UnderlayingProperty->ClassPrivate,0x18)||
                                    !(e->UnderlayingProperty->ClassPrivate->CastFlags&uint64_t(SDK::EClassCastFlags::ByteProperty)))return false;
                            }
                            else
                            {
                                if(!read(prop,0x78))return false;
                                // FObjectPropertyBase::PropertyClass, FStructProperty::Struct,
                                // FByteProperty::Enum share this verified pointer member layout.
                                std::memcpy(&actual,reinterpret_cast<const uint8_t*>(prop)+0x70,8);
                            }
                            if(actual!=expected)return false;
                        }
                        seen|=1u<<n;matched=true;break;
                    }
                    if(!matched)return false;
                }
                if(seen!=(1u<<parts.size())-1)return false;
                LOG_INFO("MiniMap: skeletal-only native contract %ls:%ls validated: params=%zu liveFrame=%u buffer=%zu",owner,name,parts.size(),unsigned(frameSize),capacity);
                return true;
            }
            bool Resolve()
            {
                static_assert(sizeof(SDK::Params::SkinnedMeshComponent_SetSkinnedAssetAndUpdate)==16&&
                    offsetof(SDK::Params::SkinnedMeshComponent_SetSkinnedAssetAndUpdate,bReinitPose)==8);
                static_assert(sizeof(SDK::Params::SkeletalMeshComponent_SetAnimationMode)==2&&
                    offsetof(SDK::Params::SkeletalMeshComponent_SetAnimationMode,bForceInitAnimScriptInstance)==1);
                static_assert(sizeof(SDK::Params::SkeletalMesh_GetImportedBounds)==0x38);
                auto* klass=static_cast<SDK::UClass*>(Find(L"/Script/CoreUObject.Function"));
                if(!klass||!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(klass)))return false;
                using K=SDK::EClassCastFlags;
                const wchar_t* skinned=L"/Script/Engine.SkinnedMeshComponent";
                const wchar_t* skeletal=L"/Script/Engine.SkeletalMeshComponent";
                auto get=[&](Id id,const wchar_t* owner,const wchar_t* name,size_t min,size_t cap,std::initializer_list<Part> p)
                {
                    const bool ok=ResolveOne(id,klass,owner,name,min,cap,p);
                    if(!ok)LOG_WARN("MiniMap: skeletal-only preflight rejected %ls:%ls; owner/native/executable/parameter contract mismatch; no invocation",owner,name);
                    return ok;
                };
                return get(Assign,skinned,L"SetSkinnedAssetAndUpdate",9,16,{{0,8,K::ObjectProperty,L"/Script/Engine.SkinnedAsset"},{8,1,K::BoolProperty}})&&
                    get(RenderStatic,skinned,L"SetRenderStatic",1,1,{{0,1,K::BoolProperty}})&&
                    get(AnimationMode,skeletal,L"SetAnimationMode",2,2,{{0,1,K::EnumProperty,L"/Script/Engine.EAnimationMode"},{1,1,K::BoolProperty}})&&
                    get(AnimClass,skeletal,L"SetAnimInstanceClass",8,8,{{0,8,K::ClassProperty,L"/Script/CoreUObject.Class"}})&&
                    get(ForcedLOD,skinned,L"SetForcedLOD",4,4,{{0,4,K::IntProperty}})&&
                    get(PostProcess,skeletal,L"SetDisablePostProcessBlueprint",1,1,{{0,1,K::BoolProperty}})&&
                    get(Cloth,skeletal,L"SetAllowClothActors",1,1,{{0,1,K::BoolProperty}})&&
                    get(Physics,L"/Script/Engine.PrimitiveComponent",L"SetSimulatePhysics",1,1,{{0,1,K::BoolProperty}})&&
                    get(Bounds,L"/Script/Engine.SkeletalMesh",L"GetImportedBounds",0x38,0x38,{{0,0x38,K::StructProperty,L"/Script/CoreUObject.BoxSphereBounds",true}})&&
                    get(MeshRead,skeletal,L"GetSkeletalMeshAsset",8,8,{{0,8,K::ObjectProperty,L"/Script/Engine.SkeletalMesh",true}})&&
                    get(AnimRead,skeletal,L"GetAnimInstance",8,8,{{0,8,K::ObjectProperty,L"/Script/Engine.AnimInstance",true}})&&
                    get(PostRead,skeletal,L"GetPostProcessInstance",8,8,{{0,8,K::ObjectProperty,L"/Script/Engine.AnimInstance",true}})&&
                    get(ForcedRead,skinned,L"GetForcedLOD",4,4,{{0,4,K::IntProperty,nullptr,true}})&&
                    get(PredictedRead,skinned,L"GetPredictedLODLevel",4,4,{{0,4,K::IntProperty,nullptr,true}});
            }
            template<class P> bool Call(SDK::UObject* receiver,Id id,P& params)
            {
                if(!GameThread()||!receiver||!Items[id]||
                    !MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(receiver)))return false;
                // Verify declaring-owner compatibility without SDK IsA/StaticClass.
                auto* type=receiver->Class;
                for(size_t depth=0;type&&depth<16;++depth)
                {
                    if(!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(type))||
                        !MiniMapNative::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(type),sizeof(SDK::UStruct),8))return false;
                    if(type==Items[id]->Outer)return Invoke(receiver,Items[id],params);
                    type=static_cast<SDK::UClass*>(type->SuperStruct);
                }
                return false;
            }
        };
        bool SkeletalState(SDK::USkeletalMeshComponent* c,SkeletalFunctions& f)
        {
            SDK::Params::SkeletalMeshComponent_GetSkeletalMeshAsset assigned{};
            SDK::Params::SkeletalMeshComponent_GetAnimInstance anim{};
            SDK::Params::SkeletalMeshComponent_GetPostProcessInstance post{};
            SDK::Params::SkinnedMeshComponent_GetForcedLOD lod{};
            if(!f.Call(c,SkeletalFunctions::MeshRead,assigned)||assigned.ReturnValue!=asset||
                !f.Call(c,SkeletalFunctions::AnimRead,anim)||anim.ReturnValue||
                !f.Call(c,SkeletalFunctions::PostRead,post)||post.ReturnValue||
                !f.Call(c,SkeletalFunctions::ForcedRead,lod)||lod.ReturnValue!=1)return false;
            auto* properties=GetHooks()?GetHooks()->ObjectProperties:nullptr;
            if(!properties||!properties->IsReady||!properties->IsReady()||!properties->FindPropertyOnObject||
                !properties->GetPropertyKind||!properties->GetPropertyArrayDim||!properties->GetBoolProperty)return false;
            const std::pair<const char*,bool> checks[]={{"bRenderStatic",true},{"bDisablePostProcessBlueprint",true},{"bAllowClothActors",false}};
            for(const auto& [name,expected]:checks)
            {
                auto p=properties->FindPropertyOnObject(c,name);bool actual=false;
                if(!p||properties->GetPropertyKind(p)!=PluginPropertyKind::Bool||properties->GetPropertyArrayDim(p)!=1||
                    !properties->GetBoolProperty(c,p,&actual)||actual!=expected)return false;
            }
            return true;
        }
        bool ConfigureSkeletal(SkeletalFunctions& f,SDK::UFunction* collisionFunction,const SDK::FVector& anchor)
        {
            auto* c=static_cast<SDK::USkeletalMeshComponent*>(mesh);
            const auto& input=assemblyRecipe.SkeletalTest.Input;
            if(!c||c->AttachParent||!input.Verified||!input.Visible||input.Hidden||!SA::A::Valid(input.Transform))return false;
            c->PrimaryComponentTick.bCanEverTick=false;c->bReplicates=false;c->bAutoActivate=false;c->bCanEverAffectNavigation=false;
            c->bGenerateOverlapEvents=false;c->CastShadow=false;c->bAffectDynamicIndirectLighting=false;c->bAffectDistanceFieldLighting=false;
            c->bVisibleInRayTracing=false;c->bVisibleInReflectionCaptures=false;c->bVisibleInRealTimeSkyCaptures=false;
            c->bVisible=input.Visible;c->bHiddenInGame=input.Hidden;
            SDK::Params::PrimitiveComponent_SetCollisionEnabled collision{};collision.NewType=SDK::ECollisionEnabled::NoCollision;
            SDK::Params::PrimitiveComponent_SetSimulatePhysics physics{};physics.bSimulate=false;
            SDK::Params::SkeletalMeshComponent_SetDisablePostProcessBlueprint post{};post.bInDisablePostProcess=true;
            SDK::Params::SkeletalMeshComponent_SetAllowClothActors cloth{};cloth.bInAllow=false;
            SDK::Params::SkeletalMeshComponent_SetAnimInstanceClass animClass{};
            SDK::Params::SkeletalMeshComponent_SetAnimationMode mode{};mode.InAnimationMode=SDK::EAnimationMode::AnimationCustomMode;
            SDK::Params::SkinnedMeshComponent_SetRenderStatic reference{};reference.bNewValue=true;
            SDK::Params::SkinnedMeshComponent_SetSkinnedAssetAndUpdate assign{};
            assign.NewMesh=static_cast<SDK::USkeletalMesh*>(asset);assign.bReinitPose=true;
            SDK::Params::SkinnedMeshComponent_SetForcedLOD lod{};lod.InNewForcedLOD=1;
            if(!Invoke(c,collisionFunction,collision)||!f.Call(c,SkeletalFunctions::Physics,physics)||
                !f.Call(c,SkeletalFunctions::PostProcess,post)||!f.Call(c,SkeletalFunctions::Cloth,cloth)||
                !f.Call(c,SkeletalFunctions::AnimClass,animClass)||!f.Call(c,SkeletalFunctions::AnimationMode,mode)||
                !f.Call(c,SkeletalFunctions::RenderStatic,reference)||!f.Call(c,SkeletalFunctions::Assign,assign)||
                !f.Call(c,SkeletalFunctions::ForcedLOD,lod)||!SkeletalState(c,f))return false;
            MiniMapNative::Get()->player.setVisibleInSceneCaptureOnly(c,true);
            SDK::FTransform t{};
            t.Rotation=SDK::FQuat(input.Transform.R[0],input.Transform.R[1],input.Transform.R[2],input.Transform.R[3]);
            t.Translation=SDK::FVector(anchor.X+input.Transform.T[0],anchor.Y+input.Transform.T[1],anchor.Z+input.Transform.T[2]);
            t.Scale3D=SDK::FVector(input.Transform.S[0],input.Transform.S[1],input.Transform.S[2]);

            MiniMapNative::Get()->player.setWorldTransform(c,t,false,nullptr,1);
            SA::A::Transform actual,expected=input.Transform;
            actual.R=SA::A::FromRotator({c->RelativeRotation.Pitch,c->RelativeRotation.Yaw,c->RelativeRotation.Roll});
            actual.T={c->RelativeLocation.X,c->RelativeLocation.Y,c->RelativeLocation.Z};
            actual.S={c->RelativeScale3D.X,c->RelativeScale3D.Y,c->RelativeScale3D.Z};expected.T={t.Translation.X,t.Translation.Y,t.Translation.Z};
            if(!SA::Equivalent(actual,expected))return false;
            size_t failed=0;
            if(!MiniMapBuildingRepresentation::ValidateCaptureMaterialSlots(reinterpret_cast<uintptr_t>(c),input.Materials.data(),0,failed))return false;

            for(size_t slot=0;slot<input.MaterialCount;++slot)
            {
                if(!MiniMapBuildingRepresentation::ValidateCaptureObject(input.Materials[slot]))return false;
                MiniMapNative::Get()->player.setMaterial(c,int32_t(slot),reinterpret_cast<SDK::UMaterialInterface*>(input.Materials[slot]));
            }
            if(!MiniMapBuildingRepresentation::ValidateCaptureMaterialSlots(reinterpret_cast<uintptr_t>(c),input.Materials.data(),input.MaterialCount,failed))return false;
            LOG_INFO("MiniMap: skeletal-only setup: mesh assignment readback matched; renderStatic=true anim/post instances=null postProcessDisabled=true cloth=false; physics=false requested, collision disabled, tick/activation/replication/navigation disabled; materials=%zu overrides=%zu; localT=(%.3f,%.3f,%.3f) Q=(%.6f,%.6f,%.6f,%.6f) S=(%.3f,%.3f,%.3f); requested LOD=1 (LOD0), forced getter=1; residency not certified",
                input.MaterialCount,size_t(std::count(input.Overrides.begin(),input.Overrides.end(),true)),input.Transform.T[0],input.Transform.T[1],input.Transform.T[2],
                input.Transform.R[0],input.Transform.R[1],input.Transform.R[2],input.Transform.R[3],input.Transform.S[0],input.Transform.S[1],input.Transform.S[2]);
            return true;
        }
        void QueueSkeletalTest(const CandidateRequest& request,const SA::Recipe& recipe,SDK::UWorld* world) noexcept
        {
            static size_t queueReports=0;
            const bool report=queueReports<8||(!skeletalTestObserved&&recipe.SkeletalTest.Present);
            if(report)
            {
                ++queueReports;
                LOG_INFO("MiniMap: skeletal-only queue entered: generation=%llu building=%u recordPresent=%d alreadyObserved=%d outcome=%s",
                    (unsigned long long)recipe.Generation,request.Source.BuildingId,int(recipe.SkeletalTest.Present),int(skeletalTestObserved),
                    skeletalTestObserved?"skipped_already_observed":!recipe.SkeletalTest.Present?"skipped_no_admitted_record":"checking_retention");
            }
            if(skeletalTestObserved||!recipe.SkeletalTest.Present)return;
            skeletalTestObserved=true; // one bounded attempt, including preflight rejection
            try
            {
                const auto& test=recipe.SkeletalTest;const auto& i=test.Input;
                if(!recipe.HighDetail||!i.Verified||!i.Visible||i.Hidden||!SA::A::Valid(i.Transform)||
                    !i.MaterialCount||i.MaterialCount>SA::Slots)
                {LOG_WARN("MiniMap: skeletal-only queue rejected: highDetail=%d recordVerified=%d visible=%d hidden=%d transformValid=%d materials=%zu capacity=%zu; static capture unchanged",
                    int(recipe.HighDetail),int(i.Verified),int(i.Visible),int(i.Hidden),int(SA::A::Valid(i.Transform)),i.MaterialCount,SA::Slots);return;}
                auto pending=std::make_unique<SkeletalTest>();pending->Request=request;pending->SourceRecipe=recipe;pending->World=world;
                auto retain=[&](uintptr_t p)
                {
                    if(!MiniMapBuildingRepresentation::ValidateCaptureObject(p))return false;
                    auto* o=reinterpret_cast<SDK::UObject*>(p);
                    for(size_t n=0;n<pending->RootCount;++n)if(pending->Roots[n]==o)return true;
                    if(pending->RootCount==pending->Roots.size())return false;
                    Hold(o);pending->Roots[pending->RootCount++]=o;return true;
                };
                if(!retain(recipe.ActorClass)||!retain(test.Template)||!retain(test.ComponentClass)||!retain(i.Mesh))
                {LOG_WARN("MiniMap: skeletal-only candidate retention failed; static capture unchanged");return;}
                if(reinterpret_cast<SDK::UObject*>(test.Template)->Class!=Find(L"/Script/Engine.SkeletalMeshComponent")||
                    test.ComponentClass!=reinterpret_cast<uintptr_t>(Find(L"/Script/Engine.SkeletalMeshComponent"))||
                    reinterpret_cast<SDK::UObject*>(i.Mesh)->Class!=Find(L"/Script/Engine.SkeletalMesh"))
                {LOG_WARN("MiniMap: skeletal-only exact resident asset/template class mismatch; static capture unchanged");return;}
                for(size_t n=0;n<i.MaterialCount;++n)if(!retain(i.Materials[n]))
                {LOG_WARN("MiniMap: skeletal-only material retention failed; static capture unchanged");return;}
                LOG_INFO("MiniMap: skeletal-only candidate retained: generation=%llu candidate=%s blueprint=%s template=%p asset=%p; waits for existing static job retirement, no production publication",
                    (unsigned long long)recipe.Generation,MiniMapSpriteCapture::SessionVisualKey(request.Candidate).c_str(),
                    request.Candidate.CanonicalBlueprint?request.Candidate.CanonicalBlueprint->c_str():"unresolved",reinterpret_cast<void*>(test.Template),reinterpret_cast<void*>(i.Mesh));
                pendingSkeletalTest=pending.release();
            }
            catch(...){LOG_WARN("MiniMap: skeletal-only candidate allocation/retention exception; static capture unchanged");}
        }
        bool ConfigureAssembly(SDK::UClass* meshClass,SDK::UObject* outer,SDK::UFunction* collisionFunction,const SDK::FVector& anchor)
        {
            if(skeletalOnly)return false; // No static downcasts or SetStaticMesh in diagnostic mode.
            static_assert(sizeof(SDK::FTransform)==0x60 && alignof(SDK::FTransform)==16);
            static_assert(offsetof(SDK::FTransform,Rotation)==0 && offsetof(SDK::FTransform,Translation)==0x20 && offsetof(SDK::FTransform,Scale3D)==0x40);
            for(size_t i=0;i<assemblyRecipe.Count;++i)
            {
                auto* c=i?Make<SDK::UStaticMeshComponent>(meshClass,outer,"assembly static instance"):static_cast<SDK::UStaticMeshComponent*>(mesh);
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

                if(!MiniMapNative::Get()->player.setStaticMesh(c,reinterpret_cast<SDK::UStaticMesh*>(input.Mesh)))return false;
                // SDK field, configured before registration/proxy construction.
                // Engine clamps this LOD0 request to policy and resident resources.
                c->ForcedLodModel=0;
                MiniMapNative::Get()->player.setVisibleInSceneCaptureOnly(c,true);
                SDK::FTransform t{};
                t.Rotation=SDK::FQuat(input.Transform.R[0],input.Transform.R[1],input.Transform.R[2],input.Transform.R[3]);
                t.Translation=SDK::FVector(anchor.X+input.Transform.T[0],anchor.Y+input.Transform.T[1],anchor.Z+input.Transform.T[2]);
                t.Scale3D=SDK::FVector(input.Transform.S[0],input.Transform.S[1],input.Transform.S[2]);
                // Exact installed SetWorldTransform const FTransform&, false,
                // nullptr, ETeleportType::TeleportPhysics=1. No attachment.

                MiniMapNative::Get()->player.setWorldTransform(c,t,false,nullptr,1);
                // Verify stored unattached world inputs through the same scene
                // layout already audited in R2.5a. Quaternion sign is equivalent.
                SA::A::Transform actual,expected=input.Transform;
                actual.R=SA::A::FromRotator({c->RelativeRotation.Pitch,c->RelativeRotation.Yaw,c->RelativeRotation.Roll});
                actual.T={c->RelativeLocation.X,c->RelativeLocation.Y,c->RelativeLocation.Z};
                actual.S={c->RelativeScale3D.X,c->RelativeScale3D.Y,c->RelativeScale3D.Z};
                expected.T={t.Translation.X,t.Translation.Y,t.Translation.Z};
                if(!SA::Equivalent(actual,expected))return false;

                size_t failedSlot=0;
                if(!MiniMapBuildingRepresentation::ValidateCaptureMaterialSlots(reinterpret_cast<uintptr_t>(c),input.Materials.data(),0,failedSlot))return false;
                for(size_t s=0;s<input.MaterialCount;++s)
                {
                    auto* material=reinterpret_cast<SDK::UMaterialInterface*>(input.Materials[s]);
                    if(!MiniMapBuildingRepresentation::ValidateCaptureObject(input.Materials[s]))return false;
                    MiniMapNative::Get()->player.setMaterial(c,int32_t(s),material);
                }
                if(!MiniMapBuildingRepresentation::ValidateCaptureMaterialSlots(reinterpret_cast<uintptr_t>(c),input.Materials.data(),input.MaterialCount,failedSlot))
                {LOG_ERROR("MiniMap: R2.5b material assignment failed: instance=%zu slot=%zu",i,failedSlot);return false;}
                size_t overrides=0;for(size_t s=0;s<input.MaterialCount;++s)overrides+=input.Overrides[s];
                LOG_INFO("MiniMap: R2.5b configured static instance %zu: materialSlots=%zu authoredOverrides=%zu; stored transform verified",i,input.MaterialCount,overrides);
            }
            return assemblyComponents.Count==assemblyRecipe.Count;
        }
        void QuarantinePreparedStatic() noexcept
        {
            assemblyAssociationUncertain=true;preparedStatic.Pending=false;finished=true;
            LOG_ERROR("MiniMap: prepared capture retirement uncertain; retained owner quarantined");
        }
        void FailBeforeCapture(const char* reason)
        {
            // A.4.9l TEMPORARY NEXT-TICK STATIC CAPTURE: cancel before native commitment.
            if(preparedStatic.Pending)
                LOG_WARN("MiniMap: A.4.9l pending static capture cancelled: generation=%llu building=%u job=%p preparationService=%llu currentService=%llu elapsedMs=%.3f reason=%s",
                    (unsigned long long)assemblyRecipe.Generation,candidateRequest?candidateRequest->Source.BuildingId:0,
                    static_cast<void*>(preparedStatic.Job),(unsigned long long)preparedStatic.ServiceSequence,
                    (unsigned long long)captureServiceSequence,
                    std::chrono::duration<double,std::milli>(CaptureClock::now()-preparedStatic.PreparedAt).count(),reason);
            // C0 may already own render work even though the final readback job
            // is still uncommitted. Detach and retire in order; never discard it
            // through the original never-rendered cleanup path.
            if(preparedStatic.PreliminaryInvoked&&!captureCommitted)
            {
                if(preparedStatic.Cancelling)return;
                preparedStatic.Cancelling=true;preparedStatic.Pending=true;
                cancelled.store(true,std::memory_order_release);
                FailSpritePublication(reason);
                try
                {
                    const auto report=Native::Inspect(job);
                    if(!job||job!=preparedStatic.Job||report.Current!=Native::Phase::ReadyToSubmit||
                        report.Callables||report.Outstanding||report.NativeEnqueueEverInvoked||
                        !Native::CaptureRetirementRendererAvailable()||
                        assemblyAssociationUncertain||!captureWorld||captureWorld!=MiniMapMap::GetWorld()||captureWorld==endingWorld)
                    {QuarantinePreparedStatic();return;}
                    Native::Cancel(job);
                    Unregister(); // all detach commands follow C0 on the same game thread
                    RetirementOperations operations;
                    if(!RetirementFence().Insert(operations,!captureRegistered&&assemblyComponents.Detached()))
                    {QuarantinePreparedStatic();}
                }
                catch(...){QuarantinePreparedStatic();}
                return;
            }
            ClearPreparedStatic();
            if(candidateRequest&&!skeletalOnly&&(!std::strcmp(reason,"invalid_or_stale_source")||
                !std::strcmp(reason,"candidate_revalidation_exception")||!std::strcmp(reason,"capture_lifecycle_unavailable")))
                MiniMapBuildingSprites::AbandonCapture(candidateRequest->Lease,true);
            FailSpritePublication(reason);
            LOG_ERROR("MiniMap: %s failed before capture: %s; no retry",skeletalOnly?"skeletal-only diagnostic":"R2",reason);
            if(assemblyActive&&!SA::CanCleanBeforeCommit(assemblyAssociationUncertain,
                captureRegistered||!assemblyComponents.Detached(),
                captureWorld&&captureWorld==MiniMapMap::GetWorld()&&captureWorld!=endingWorld))
            {
                finished=true;
                LOG_ERROR("MiniMap: R2.5b component/world association uncertain; resources retained, no stale-world cleanup");
                return;
            }
            RetirementOperations operations;
            if(!Native::DiscardExternalBeforeCapture(job)||!RetirementFence().Release(operations))
            {finished=true;LOG_ERROR("MiniMap: pre-capture cleanup ownership uncertain; resources and lease retained");return;}
            ReleaseObjects(); finished=true;assemblyActive=false;baselineRetired=true;
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
            FailSpritePublication("readback_rejected_before_enqueue; retirement_pending");
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
            MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::CaptureRetire);
            RetirementOperations operations;
            if (!MiniMapCaptureRetirement::CanPoll(Native::ExternalRejectedWithoutReadback(job),
                Native::CaptureRetirementRendererAvailable(),operations.Available(),!meshRegistered && !captureRegistered && assemblyComponents.Detached()))
            {
                finished=true;
                FailSpritePublication("retirement_prerequisites_uncertain; resources_retained");
                LOG_ERROR("MiniMap: R2 capture retirement prerequisites uncertain; existing quarantine retained");
                return;
            }
            if (!RetirementFence().Poll(operations)) return; // one nonblocking poll
            LOG_INFO("MiniMap: R2 capture retirement fence complete (RHI submission, not GPU idle)");
            if (!Native::CompleteExternalCaptureRetirement(job) || !RetirementFence().Release(operations) ||
                !Native::DestroyIfRetired(job))
            {
                finished=true;
                FailSpritePublication("retirement_ownership_uncertain; resources_retained");
                LOG_ERROR("MiniMap: R2 capture retirement final ownership check failed; resources retained");
                return;
            }
            ReleaseObjects(); retirementPending=false; finished=true;assemblyActive=false;baselineRetired=true;
            LOG_INFO("MiniMap: R2 failed capture retired: empty CPU job released; components destroyed; strong references released");
        }
        void ExportImage() noexcept
        {
            MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::CaptureExport);
            LOG_INFO("MiniMap: capture image completion entered; provisional session publication and optional diagnostic PNG export");
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
                if(skeletalOnly)
                {
                    const auto foreground=std::count_if(image.Mask.begin(),image.Mask.end(),[](uint8_t a){return a!=0;});
                    LOG_INFO("MiniMap: skeletal-only converted image: %dx%d nonzero-alpha=%zu; only one verified show-only member; pose/actual LOD require visual verification; production publication disabled",
                        Dimension,Dimension,size_t(foreground));
                    if(!foreground){LOG_WARN("MiniMap: skeletal-only empty image; no diagnostic PNG exported; cleanup continues");return;}
                }
                #if defined(MINIMAP_BUILDING_SPRITES)
                if(assemblyActive&&candidateRequest&&!skeletalOnly)
                {
                    // Display eligibility and renderer-quality certification are independent.
                    // The native owner has verified capture/readback safety; nonempty image
                    // validity permits provisional display, never quality inference.
                    if(!CheckCandidate(*candidateRequest,assemblyRecipe,captureWorld,true).Allowed())
                    {FailSpritePublication("capture_source_changed_before_publication");return;}
                    privateWorld.ObserveProduction(candidateRequest->Candidate,image.Rgba.data(),image.Rgba.size());
                    MiniMapBuildingSprites::Complete(candidateRequest->Lease,assemblyBounds,image.Rgba,
                        candidateRequest->Observation.Readiness);
                    candidateRequest->PublicationFinished=true; // completion/failure recorded by publication
                }
#endif
#if !MINIMAP_DEBUG_UI
                // Development PNGs are disabled here; session publication above is independent.
                if(assemblyActive&&candidateRequest&&!skeletalOnly)return;
#endif
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
                        LOG_INFO("MiniMap: R2 %s exported: %ls (%dx%d, %llu bytes)",skeletalOnly?(gray?"skeletal-only alpha diagnostic":"skeletal-only PNG"):(gray?"static readiness alpha diagnostic":"static readiness diagnostic PNG"),path.c_str(),Dimension,Dimension,(unsigned long long)result.Bytes);
                    }
                    else
                    {
                        LOG_ERROR("MiniMap: R2 PNG export failed: %ls; stage=%s HRESULT=0x%08lx",path.c_str(),result.Stage,(unsigned long)result.Error);
                    }
                };
                if(assemblyActive&&candidateRequest)
                {
                    // Session diagnostics are never read as durable cache entries.
                    const auto stem=std::wstring(skeletalOnly?L"Building_SkeletalOnly_Session_":L"Building_ReadinessUnknown_Diagnostic_Session_")+std::to_wstring(assemblyRecipe.Generation)+L"_"+
                        std::to_wstring(candidateRequest->Candidate.Identity.DefinitionObject)+L"_"+std::to_wstring(candidateRequest->Candidate.Identity.Building)+
                        L"_Process_"+std::to_wstring(GetCurrentProcessId())+L"_Capture_"+std::to_wstring(GetTickCount64());
                    write((stem+L".png").c_str(),image.Rgba,false);write((stem+L"_Alpha.png").c_str(),image.Mask,true);

                }
            }
            catch(const std::exception& e){LOG_ERROR("MiniMap: R2 PNG export failed: %s; native cleanup continues",e.what());}
            catch(...){LOG_ERROR("MiniMap: R2 PNG export failed: unknown exception; native cleanup continues");}
        }
        // Skeletal diagnostics dispatch immediately; static assemblies resume this tail next tick.
        void DispatchCapture(SDK::UClass* captureClass,SDK::UFunction* captureFunction,
            void* builder,void* scene,const SDK::FVector& extent,const SDK::FVector& center)
        {
            bool committedJob=false;
            captureSubmittedAt=CaptureClock::now();
            if(publicStaticCapture)
            {
                if(!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(capture))||
                    capture->Class!=captureClass||!capture->bVisible)
                {FailBeforeCapture("public CaptureScene receiver/visibility prerequisite failed");return;}
                // EngineTick callbacks run after the original engine tick, outside
                // its end-of-frame update traversal. Public CaptureScene performs
                // the deferred update flush missing from direct UpdateCapture.
                // Mark conservatively BEFORE dispatch: an exception may follow
                // render work submission. Such failures must retain all roots.
                if(candidateRequest&&!skeletalOnly&&!MiniMapBuildingSprites::ReservationCurrent(candidateRequest->Lease))
                {FailBeforeCapture("provider advancement superseded before dispatch");return;}
                committedJob=Native::MarkExternalCaptureCommitted(job);
                if(!committedJob){FailBeforeCapture("public CaptureScene job commitment prerequisite failed");return;}
                captureCommitted=true;
                if(staticEvidenceActive&&candidateRequest)
                    LOG_INFO("MiniMap: static capture lifecycle A4.6: generation=%llu building=%u attempt=1 components=%zu mode=public_CaptureScene registrationToSubmitMs=%.3f visible=%d detailMode=%u; deferred-update flush supplied by installed public contract; proxy/PSO/resident LOD readiness unobserved",
                        (unsigned long long)assemblyRecipe.Generation,candidateRequest->Source.BuildingId,assemblyComponents.Count,
                        std::chrono::duration<double,std::milli>(captureSubmittedAt-componentsRegisteredAt).count(),int(capture->bVisible),unsigned(capture->DetailMode));
                MiniMapCallbackTiming::Phase dispatchTiming(MiniMapCallbackTiming::Id::CaptureSubmit);
                uint8_t emptyFrame=0;
                // No reentrant engine work between reservation validation and
                // this dispatch boundary. A successful reservation was not an attempt.
                if(candidateRequest&&!skeletalOnly&&!MiniMapBuildingSprites::StartCapture(candidateRequest->Lease))
                {finished=true;Native::Cancel(job);FailSpritePublication("capture_reservation_changed_after_commit; resources_retained");return;}
                LOG_INFO("MiniMap: A.4.9w final CaptureScene submission: generation=%llu building=%u C10Service=%llu displacementFromC0=%llu",
                    (unsigned long long)assemblyRecipe.Generation,candidateRequest?candidateRequest->Source.BuildingId:0,
                    (unsigned long long)captureServiceSequence,(unsigned long long)(captureServiceSequence-preparedStatic.PreliminarySequence));
                if(!Invoke(capture,captureFunction,emptyFrame))
                {finished=true;Native::Cancel(job);FailSpritePublication("public_capture_dispatch_uncertain; resources_retained");LOG_ERROR("MiniMap: public CaptureScene dispatch uncertain; resources retained");return;}
                if(staticEvidenceActive)
                    LOG_INFO("MiniMap: static capture lifecycle A4.6: attempt=1 public dispatch returned in %.3fms; void call is not proof of drawable geometry; readback follows; retries=disabled",
                        std::chrono::duration<double,std::milli>(CaptureClock::now()-captureSubmittedAt).count());
            }
            else
            {
                MiniMapNative::Get()->texture.renderGraphBuilderConstruct(builder,scene);
                MiniMapNative::Get()->texture.updateSceneCapture(capture,scene,builder);
                // Existing diagnostic commit boundary and retirement are unchanged.
                MiniMapNative::Get()->texture.renderGraphBuilderExecute(builder);
                captureCommitted=true;
                committedJob=Native::MarkExternalCaptureCommitted(job);
                MiniMapNative::Get()->texture.renderGraphBuilderDestroy(builder);
                MiniMapNative::Get()->texture.memoryFree(builder);
            }
            if(!committedJob){finished=true;Native::Cancel(job);FailSpritePublication("committed_job_identity_uncertain; resources_retained");LOG_ERROR("MiniMap: R2 committed-job identity uncertain; resources retained");return;}
            LOG_INFO("MiniMap: R2 capture queued: %dx%d PF_FloatRGBA, %s; bounds XY=%.3fx%.3f center=(%.3f,%.3f) orthoWidth=%.3f",
                Dimension,Dimension,skeletalOnly?"diagnostic skeletal-only reference render (no publication)":"bounded static authored instances",extent.X*2,extent.Y*2,center.X,center.Y,double(capture->OrthoWidth));
            LOG_INFO("MiniMap: R2 GPU readback scheduling entered (copy submission reported separately)");
            Native::Service(job,++frame);
        }
        void Submit(SDK::UWorld* world)
        {
            MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::CaptureSetup);
            attempted=true;
            if(!assemblyActive)
            {finished=true;LOG_ERROR("MiniMap: capture owner invariant failed; no native setup attempted; resources retained");return;}
            if(!Native::Preflight()){FailBeforeCapture(Native::Status());return;}
            SkeletalFunctions skeletal;
            if(skeletalOnly&&(!SkeletalPropertiesAvailable(reinterpret_cast<SDK::UObject*>(assemblyRecipe.SkeletalTest.Template))||!skeletal.Resolve())){FailBeforeCapture("skeletal-only native contract preflight failed; no component created");return;}
            RetirementOperations retirementOperations;
            if(assemblyActive&&!RetirementFence().Rearm()){FailBeforeCapture("previous fence ownership not retired");return;}
            if(!RetirementFence().Prepare(retirementOperations)){FailBeforeCapture("retirement fence construction unavailable");return;}
            LOG_INFO("MiniMap: R2 gameplay-world eligibility established via lifecycle pointer; setup entered");
            auto* type=static_cast<SDK::UClass*>(Find(skeletalOnly?L"/Script/Engine.SkeletalMesh":L"/Script/Engine.StaticMesh"));
            auto* meshClass=static_cast<SDK::UClass*>(Find(skeletalOnly?L"/Script/Engine.SkeletalMeshComponent":L"/Script/Engine.StaticMeshComponent"));
            auto* captureClass=static_cast<SDK::UClass*>(Find(L"/Script/Engine.SceneCaptureComponent2D"));
            auto* targetClass=static_cast<SDK::UClass*>(Find(L"/Script/Engine.TextureRenderTarget2D"));
            if(!type||!meshClass||!captureClass||!targetClass){FailBeforeCapture("resident native class metadata unavailable");return;}
            asset=reinterpret_cast<SDK::UObject*>(skeletalOnly?assemblyRecipe.SkeletalTest.Input.Mesh:assemblyRecipe.Instances[0].Mesh);
            if(!asset){FailBeforeCapture(skeletalOnly?"skeletal-only retained mesh unavailable":"assembly mesh unavailable");return;}
            Hold(asset);
            SDK::FBoxSphereBounds bounds{};
            if(skeletalOnly)
            {
                if(asset->Class!=type||meshClass!=reinterpret_cast<SDK::UClass*>(assemblyRecipe.SkeletalTest.ComponentClass))
                {FailBeforeCapture("skeletal-only retained class/asset identity mismatch");return;}
                SDK::Params::SkeletalMesh_GetImportedBounds params{};
                if(!skeletal.Call(asset,SkeletalFunctions::Bounds,params))
                {FailBeforeCapture("skeletal-only GetImportedBounds dispatch failed");return;}
                bounds=params.ReturnValue;
                auto input=assemblyRecipe.SkeletalTest.Input;
                input.LocalBounds.Origin={bounds.Origin.X,bounds.Origin.Y,bounds.Origin.Z};
                input.LocalBounds.Extent={bounds.BoxExtent.X,bounds.BoxExtent.Y,bounds.BoxExtent.Z};
                assemblyBounds={};
                if(!std::isfinite(bounds.SphereRadius)||bounds.SphereRadius<=0||!SA::Add(assemblyBounds,input))
                {FailBeforeCapture("skeletal-only imported bounds/transform invalid");return;}
                LOG_INFO("MiniMap: skeletal-only bounds source=GetImportedBounds origin=(%.3f,%.3f,%.3f) extent=(%.3f,%.3f,%.3f); transformed eight corners; no static framing reused",
                    bounds.Origin.X,bounds.Origin.Y,bounds.Origin.Z,bounds.BoxExtent.X,bounds.BoxExtent.Y,bounds.BoxExtent.Z);
            }
            else bounds=static_cast<SDK::UStaticMesh*>(asset)->ExtendedBounds;
            auto center=bounds.Origin, extent=bounds.BoxExtent;
            if(assemblyActive)
            {
                if(!skeletalOnly)if(const auto* failure=SA::Validate(assemblyRecipe,assemblyBounds)){FailBeforeCapture(failure);return;}
                center=SDK::FVector((assemblyBounds.Min[0]+assemblyBounds.Max[0])*0.5,(assemblyBounds.Min[1]+assemblyBounds.Max[1])*0.5,(assemblyBounds.Min[2]+assemblyBounds.Max[2])*0.5);
                extent=SDK::FVector((assemblyBounds.Max[0]-assemblyBounds.Min[0])*0.5,(assemblyBounds.Max[1]-assemblyBounds.Min[1])*0.5,(assemblyBounds.Max[2]-assemblyBounds.Min[2])*0.5);
            }
            if(!std::isfinite(center.X)||!std::isfinite(center.Y)||!std::isfinite(center.Z)||
                !std::isfinite(extent.X)||!std::isfinite(extent.Y)||!std::isfinite(extent.Z)||
                extent.X<=0||extent.Y<=0||extent.Z<=0||extent.X>10000||extent.Y>10000||extent.Z>10000){FailBeforeCapture("mesh bounds validation failed");return;}
            auto* outer=MiniMapNative::Get()->asset.getTransientPackage();
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
            for(size_t i=0;i<4;++i)if(!functions.Items[i]){FailBeforeCapture("exact native function lookup/parameter ABI validation failed");return;}
            publicStaticCapture=assemblyActive&&!skeletalOnly;
            if(publicStaticCapture)
            {
                functions.Items[4]=FindPublicCapture(functionClass,captureClass);
                if(!functions.Items[4]){FailBeforeCapture("public CaptureScene owner/native/executable/zero-frame contract unavailable");return;}
            }
            if(assemblyActive&&reinterpret_cast<uintptr_t>(functions.Items[2]->ExecFunction)!=reinterpret_cast<uintptr_t>(MiniMapNative::Get()->texture.showOnlyComponentExec))
            {FailBeforeCapture("installed show-only wrapper fingerprint mismatch");return;}
            LOG_INFO("MiniMap: R2 UObject creation/configuration entered; exact function lookups validated");
            const SDK::FVector location(0,0,200000);
            mesh=skeletalOnly?static_cast<SDK::UMeshComponent*>(Make<SDK::USkeletalMeshComponent>(meshClass,outer,"diagnostic USkeletalMeshComponent")):
                static_cast<SDK::UMeshComponent*>(Make<SDK::UStaticMeshComponent>(meshClass,outer,"UStaticMeshComponent"));
            if(!mesh){FailBeforeCapture(skeletalOnly?"USkeletalMeshComponent construction/validation failed":"UStaticMeshComponent construction/validation failed");return;}
            if(assemblyActive)
            {
                if(assemblyComponents.Count>=assemblyComponents.Entries.size())
                {MiniMapNative::Get()->player.destroyComponent(mesh,false);Drop(mesh);mesh=nullptr;
                 FailBeforeCapture("component ledger capacity exceeded");return;}
                assemblyComponents.Entries[assemblyComponents.Count++].Component=reinterpret_cast<uintptr_t>(mesh);
            }
            if(skeletalOnly&&!ConfigureSkeletal(skeletal,functions.Items[0],location))
            {FailBeforeCapture("skeletal-only pose/material/transform configuration or readback failed");return;}
            if(assemblyActive&&!skeletalOnly&&!ConfigureAssembly(meshClass,outer,functions.Items[0],location))
            {FailBeforeCapture("assembly component/material/transform configuration failed");return;}
            capture=Make<SDK::USceneCaptureComponent2D>(captureClass,outer,"USceneCaptureComponent2D");
            if(!capture){FailBeforeCapture("USceneCaptureComponent2D construction/validation failed");return;}
            target=Make<SDK::UTextureRenderTarget2D>(targetClass,outer,"UTextureRenderTarget2D");
            if(!target){FailBeforeCapture("UTextureRenderTarget2D construction/validation failed");return;}
            MiniMapSpriteCapture::ConfigureSceneCapture(capture,extent);
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

            MiniMapNative::Get()->texture.initRenderTarget(target,Dimension,Dimension,10,true);
            capture->TextureTarget=target;
            const SDK::FVector camera(location.X+center.X,location.Y+center.Y,location.Z+center.Z+extent.Z+1000.0);
            // UE basis: pitch=-90/yaw=-90 => forward -Z, right +X, up -Y.
            TransformParams cameraTransform{};cameraTransform.NewLocation=camera;
            cameraTransform.NewRotation=SDK::FRotator(-90,-90,0);cameraTransform.bTeleport=true;
            if(!Invoke(capture,functions.Items[1],cameraTransform)){FailBeforeCapture("capture transform dispatch failed");return;}
            LOG_INFO("MiniMap: R2 UObject configuration calls returned; %dx%d PF_FloatRGBA target initialized",Dimension,Dimension);
            void* scene=nullptr;std::memcpy(&scene,reinterpret_cast<const uint8_t*>(world)+0x258,8); // CL PDB UWorld::Scene
            if(!scene){FailBeforeCapture("world render scene unavailable");return;}

            LOG_INFO("MiniMap: R2 component registration entered");
            captureWorld=world;
            if(assemblyActive)
            {
                for(size_t i=0;i<assemblyComponents.Count;++i)
                {
                    auto& entry=assemblyComponents.Entries[i];auto* c=reinterpret_cast<SDK::UActorComponent*>(entry.Component);
                    MiniMapNative::Get()->player.registerComponent(c,world,nullptr);
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
            else {MiniMapNative::Get()->player.registerComponent(mesh,world,nullptr);meshRegistered=true;}
            MiniMapNative::Get()->player.registerComponent(capture,world,nullptr);captureRegistered=true;
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
            componentsRegisteredAt=CaptureClock::now();
            LOG_INFO("MiniMap: R2 component registration returned; both world pointers validated");
            if(skeletalOnly)
            {
                if(!SkeletalState(static_cast<SDK::USkeletalMeshComponent*>(mesh),skeletal))
                {FailBeforeCapture("skeletal-only state changed during registration; no capture");return;}
                SDK::Params::SkinnedMeshComponent_GetPredictedLODLevel lod{};
                if(!skeletal.Call(mesh,SkeletalFunctions::PredictedRead,lod))
                {FailBeforeCapture("skeletal-only predicted LOD getter failed");return;}
                LOG_INFO("MiniMap: skeletal-only registered in owned world=%p; reference-render configuration read back, no animation/post-process instances; predictedLOD=%d (observation only), LOD0 residency/rendering not proven; no delay or pose evaluation task requested",
                    static_cast<void*>(world),lod.ReturnValue);
            }
            if(assemblyActive)
            {
                uintptr_t members[SA::Limit+1]{};
                for(size_t i=0;i<assemblyComponents.Count;++i)
                {
                    auto& entry=assemblyComponents.Entries[i];members[i]=entry.Component;
                    if(!MiniMapBuildingRepresentation::ValidateCaptureMembership(reinterpret_cast<uintptr_t>(capture),members,i))
                    {FailBeforeCapture("assembly show-only prefix invalid before append");return;}
                    ShowOnlyParams showOnly{};showOnly.InComponent=reinterpret_cast<SDK::UPrimitiveComponent*>(entry.Component);
                    if(!Invoke(capture,functions.Items[2],showOnly)){FailBeforeCapture("assembly show-only dispatch failed");return;}
                    entry.Member=true;
                }
                if(!assemblyComponents.Complete(skeletalOnly?1:assemblyRecipe.Count)||!MiniMapBuildingRepresentation::ValidateCaptureMembership(reinterpret_cast<uintptr_t>(capture),members,assemblyComponents.Count))
                {FailBeforeCapture("assembly show-only membership mismatch");return;}
            }
            else
            {
                ShowOnlyParams showOnly{};showOnly.InComponent=mesh;
                if(!Invoke(capture,functions.Items[2],showOnly)){FailBeforeCapture("show-only dispatch failed");return;}
            }
            auto* resource=MiniMapNative::Get()->texture.getRenderTargetResource(target);
            job=Native::CreateExternalJob(resource,Dimension,Dimension);
            if(!job){FailBeforeCapture("target resource or CPU readback allocation unavailable");return;}
            if(assemblyActive)
            {
                const auto decision=candidateRequest?CheckCandidate(*candidateRequest,assemblyRecipe,world,assemblyRootCount!=0):
                    MiniMapSpriteCapture::Decision{MiniMapSpriteCapture::State::Rejected,"candidate_request_missing"};
                if(!decision.Allowed()){FailBeforeCapture(decision.Reason);return;}
            }
            void* builder=publicStaticCapture?nullptr:MiniMapNative::Get()->asset.memoryMalloc(0x20,8);
            if(!publicStaticCapture&&!builder){FailBeforeCapture("builder storage allocation failed");return;}
            const auto snapshot=assemblyActive?MiniMapBuildingCollector::GetSnapshot():nullptr;
            if(!MiniMapMap::GameplayReady() || !GameThread() || world!=MiniMapMap::GetWorld() || world==endingWorld ||
                (assemblyActive&&(!snapshot||!snapshot->Complete||snapshot->Generation!=assemblyRecipe.Generation||!assemblyComponents.Complete(skeletalOnly?1:assemblyRecipe.Count)))||
                !Native::CaptureRetirementRendererAvailable())
            {
                if(builder)MiniMapNative::Get()->texture.memoryFree(builder);
                FailBeforeCapture("final pre-commit world/RHI prerequisite changed");return;
            }
            if(assemblyActive&&!skeletalOnly&&candidateRequest)
            {
                for(size_t t=0;t<candidateRequest->Observation.TaskCount;++t)
                {MiniMapRendererReadiness::ProviderObservation change;MiniMapRendererReadiness::ObserveTask(candidateRequest->Observation,t,change);}
#if defined(MINIMAP_BUILDING_SPRITES)
                if(!PrepareResourcePublication(candidateRequest->Candidate.Identity,candidateRequest->Observation))
                {MiniMapBuildingSprites::AbandonCapture(candidateRequest->Lease,true);
                 FailBeforeCapture("resource advancement publication awaits acknowledgment");return;}
                MiniMapBuildingSprites::ObserveAssembly(candidateRequest->Candidate.Identity,candidateRequest->Observation);
                for(size_t t=0;t<candidateRequest->Observation.TaskCount;++t)
                    MiniMapBuildingSprites::NotifyProviderChange(candidateRequest->Candidate.Identity,candidateRequest->Observation.Tasks[t].Evidence,candidateRequest->Observation.Source);
#endif
                // Refresh the baseline at dispatch so initialization completed during
                // setup does not schedule an unnecessary second capture.
                try{WatchResources(*candidateRequest,assemblyRecipe,world);}
                catch(...){LOG_WARN("MiniMap: resource observation retention unavailable; quality remains uncertified");}
            }
            LOG_INFO("MiniMap: R2 capture-builder submission entered");
            ReportStaticCaptureEvidence(camera,center,extent,location);
            if(skeletalOnly)LOG_INFO("MiniMap: skeletal-only isolated assembly: registered members=1, exact show-only membership verified; width=%.3f; fixed anchor=(0,0,200000); static components absent, publication disabled",double(capture->OrthoWidth));
            else if(assemblyActive)LOG_INFO("MiniMap: R2.5b isolated static assembly: instances=%zu static-reference width=%.3f; fixed anchor=(0,0,200000); skeletal excluded",assemblyComponents.Count,double(capture->OrthoWidth));
            // A.4.9l TEMPORARY NEXT-TICK STATIC CAPTURE. All original preparation is complete.
            if(publicStaticCapture)
            {
                preparedStatic.Pending=true;preparedStatic.Job=job;preparedStatic.World=world;
                preparedStatic.MeshClass=meshClass;preparedStatic.CaptureClass=captureClass;preparedStatic.TargetClass=targetClass;
                preparedStatic.Resource=resource;preparedStatic.Function=functions.Items[4];functions.Items[4]=nullptr;
                preparedStatic.Extent={extent.X,extent.Y,extent.Z};preparedStatic.Center={center.X,center.Y,center.Z};
                preparedStatic.ServiceSequence=captureServiceSequence;preparedStatic.PreparedAt=CaptureClock::now();
                LOG_INFO("MiniMap: A.4.9l static capture prepared; waiting for next engine tick: generation=%llu building=%u job=%p preparationService=%llu dispatchService=not_yet elapsedMs=0.000",
                    (unsigned long long)assemblyRecipe.Generation,candidateRequest?candidateRequest->Source.BuildingId:0,
                    static_cast<void*>(job),(unsigned long long)preparedStatic.ServiceSequence);
                return; // no commitment, attempt consumption, capture or readback in this tick
            }
            DispatchCapture(captureClass,nullptr,builder,scene,extent,center);
        }
        // A.4.9l TEMPORARY NEXT-TICK STATIC CAPTURE. Called only from the existing guarded Tick.
        void DispatchPreparedStatic()
        {
            if(preparedStatic.Cancelling)
            {
                // Poll only native fence/job state after detachment, including
                // across world teardown. No stale-world component inspection.
                try
                {
                    RetirementOperations operations;
                    if(!RetirementFence().Poll(operations))return;
                    if(!RetirementFence().Release(operations)||!Native::DiscardExternalBeforeCapture(job))
                    {QuarantinePreparedStatic();return;}
                    ClearPreparedStatic();ReleaseObjects();finished=true;assemblyActive=false;baselineRetired=true;
                }
                catch(...){QuarantinePreparedStatic();}
                return;
            }
            if(captureServiceSequence<=preparedStatic.ServiceSequence)return;
            if(preparedStatic.PreliminaryInvoked&&captureServiceSequence-preparedStatic.PreliminarySequence<10)return;
            const auto report=Native::Inspect(job);
            if(!job||job!=preparedStatic.Job||captureCommitted||report.Current!=Native::Phase::ReadyToSubmit||
                report.Result!=Native::Outcome::Pending||report.Callables||report.Outstanding||report.NativeEnqueueEverInvoked)
            {
                // Unexpected native ownership is quarantined by the existing failure path.
                assemblyAssociationUncertain=true;FailBeforeCapture("prepared_static_job_ownership_changed");return;
            }
            if(!assemblyActive||skeletalOnly||!publicStaticCapture||!candidateRequest||!assemblyRootCount||
                !MiniMapMap::GameplayReady()||preparedStatic.World!=recipeWorld||preparedStatic.World!=captureWorld||
                preparedStatic.World!=MiniMapMap::GetWorld()||preparedStatic.World==endingWorld||cancelled.load())
            {FailBeforeCapture("prepared_static_world_or_owner_invalidated");return;}
            const auto decision=CheckCandidate(*candidateRequest,assemblyRecipe,preparedStatic.World,true);
            if(!decision.Allowed()){FailBeforeCapture(decision.Reason);return;}
            if(!MiniMapBuildingSprites::ReservationCurrent(candidateRequest->Lease))
            {FailBeforeCapture("prepared_static_reservation_invalidated");return;}
            // Reuse the exact identity, registered/world and weak membership contracts from setup.
            if(!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(capture))||
                !MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(target))||
                !MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(preparedStatic.Function)))
            {assemblyAssociationUncertain=true;FailBeforeCapture("prepared_static_object_identity_invalid");return;}
            if(capture->Class!=preparedStatic.CaptureClass||target->Class!=preparedStatic.TargetClass||
                preparedStatic.Function->Outer!=preparedStatic.CaptureClass||capture->TextureTarget!=target||
                target->SizeX!=Dimension||target->SizeY!=Dimension||!capture->bVisible||
                MiniMapNative::Get()->texture.getRenderTargetResource(target)!=preparedStatic.Resource)
            {FailBeforeCapture("prepared_static_capture_or_target_changed");return;}
            auto registeredInWorld=[&](SDK::UActorComponent* c,bool& ledger)
            {
                if(!MiniMapBuildingRepresentation::ValidateCaptureObject(reinterpret_cast<uintptr_t>(c)))
                {assemblyAssociationUncertain=true;return false;}
                uint8_t state=0;std::memcpy(&state,reinterpret_cast<const uint8_t*>(c)+0x94,1);
                SDK::UWorld* current=nullptr;std::memcpy(&current,reinterpret_cast<const uint8_t*>(c)+0xa8,8);
                ledger=(state&1)!=0;
                if(current!=preparedStatic.World&&(ledger||current))assemblyAssociationUncertain=true;
                return ledger&&current==preparedStatic.World;
            };
            uintptr_t members[SA::Limit+1]{};
            if(assemblyComponents.Count!=assemblyRecipe.Count||!registeredInWorld(capture,captureRegistered))
            {FailBeforeCapture("prepared_static_capture_registration_changed");return;}
            for(size_t i=0;i<assemblyComponents.Count;++i)
            {
                auto& entry=assemblyComponents.Entries[i];auto* c=reinterpret_cast<SDK::UStaticMeshComponent*>(entry.Component);
                if(!registeredInWorld(c,entry.Registered))
                {FailBeforeCapture("prepared_static_component_registration_changed");return;}
                if(c->Class!=preparedStatic.MeshClass)
                {assemblyAssociationUncertain=true;FailBeforeCapture("prepared_static_component_class_changed");return;}
                members[i]=entry.Component;
            }
            if(!assemblyComponents.Complete(assemblyRecipe.Count)||
                !MiniMapBuildingRepresentation::ValidateCaptureMembership(reinterpret_cast<uintptr_t>(capture),members,assemblyComponents.Count))
            {FailBeforeCapture("prepared_static_show_only_changed");return;}
            // A.4.9w: same validated scene/view; consume C0 before invoking so
            // failures cannot replay it. No StartCapture, job commit or copy.
            if(!preparedStatic.PreliminaryInvoked)
            {
                preparedStatic.PreliminaryInvoked=true;preparedStatic.PreliminarySequence=captureServiceSequence;
                uint8_t emptyFrame=0;
                const bool invoked=Invoke(capture,preparedStatic.Function,emptyFrame);
                LOG_INFO("MiniMap: A.4.9w preliminary CaptureScene submission: generation=%llu building=%u C0Service=%llu invocationReturned=%d; no readback or publication; void return is not rendering proof",
                    (unsigned long long)assemblyRecipe.Generation,candidateRequest->Source.BuildingId,
                    (unsigned long long)preparedStatic.PreliminarySequence,int(invoked));
                if(!invoked)FailBeforeCapture("preliminary_capture_dispatch_uncertain");
                return;
            }
            const SDK::FVector extent(preparedStatic.Extent[0],preparedStatic.Extent[1],preparedStatic.Extent[2]);
            const SDK::FVector center(preparedStatic.Center[0],preparedStatic.Center[1],preparedStatic.Center[2]);
            LOG_INFO("MiniMap: A.4.9l next-tick static CaptureScene dispatch: generation=%llu building=%u job=%p preparationService=%llu dispatchService=%llu elapsedMs=%.3f; later engine tick only, render/GPU completion unobserved",
                (unsigned long long)assemblyRecipe.Generation,candidateRequest->Source.BuildingId,static_cast<void*>(job),
                (unsigned long long)preparedStatic.ServiceSequence,(unsigned long long)captureServiceSequence,
                std::chrono::duration<double,std::milli>(CaptureClock::now()-preparedStatic.PreparedAt).count());
            preparedStatic.Pending=false; // consume before dispatch; exceptions can never replay this submission
            struct Finish {~Finish(){if(!preparedStatic.Cancelling)ClearPreparedStatic();}} finish;
            DispatchCapture(preparedStatic.CaptureClass,preparedStatic.Function,nullptr,nullptr,extent,center);
        }
        void BeforeEndPlay(SDK::UWorld* world,const char*)
        {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::CaptureWorldEnd,false);
            if(!world||world!=MiniMapMap::GetWorld())return;
            if(!GameThread()){LOG_ERROR("MiniMap: R2 before-EndPlay is not game thread; component retirement NOT certified");return;}
            if(scheduling.test_and_set(std::memory_order_acquire)){LOG_ERROR("MiniMap: R2 before-EndPlay overlaps capture scheduling; retirement NOT certified");return;}
            Scope scope;
            privateWorld.EndSession(world);
            ClearResourceWatches();MiniMapRendererReadiness::ReleaseMetadata();
            // A.4.9l TEMPORARY NEXT-TICK STATIC CAPTURE: clean while this world is current.
            if(preparedStatic.Pending&&preparedStatic.World==world)
                FailBeforeCapture("world_end_before_static_dispatch");
            endingWorld=world; // stop eligibility before native EndPlay, even before any capture
            if(pendingSkeletalTest&&pendingSkeletalTest->World==world)
            {RetirePendingSkeletalTest();LOG_INFO("MiniMap: skeletal-only pending diagnostic retired at matching world end");}
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
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::CaptureTick,true);
            if(stopping.load()||!GameThread()||scheduling.test_and_set(std::memory_order_acquire))return;
            Scope scope;
            privateWorld.Service(); // Same worker tick: independent private job, never production publication.
            // A.4.9l TEMPORARY NEXT-TICK STATIC CAPTURE: one sequence per guarded GT tick.
            ++captureServiceSequence;
#if defined(MINIMAP_BUILDING_SPRITES)
            MiniMapBuildingSprites::TimingScope timing(MiniMapBuildingSprites::Timing::CaptureService);
#endif
            if(!finished||assemblyPending||pendingSkeletalTest)
                try{ServiceResourceWatch(false);}catch(...){LOG_WARN("MiniMap: bounded watch retirement deferred after exception");}
            // A.4.9l TEMPORARY NEXT-TICK STATIC CAPTURE: never service readback while prepared.
            if(preparedStatic.Pending)
            {
                try{DispatchPreparedStatic();}
                catch(...)
                {
                    if(!captureCommitted)FailBeforeCapture("deferred_static_dispatch_exception_before_commit");
                    else {ClearPreparedStatic();Native::Cancel(job);finished=true;
                        FailSpritePublication("capture_exception_after_commit; resources_retained");
                        LOG_ERROR("MiniMap: A.4.9l static dispatch exception after commitment; resources retained");}
                }
                return;
            }
            if(finished)
            {
                if(assemblyActive)
                {
                    if(job||mesh||capture||target||assemblyRootCount)return; // quarantine remains fail-closed
                    assemblyActive=false;baselineRetired=true;
                }
                if(!baselineRetired||job){if(assemblyPending)ReleaseAssemblySources();return;}
                if(!assemblyPending&&pendingSkeletalTest)
                {
                    try
                    {
                        auto& pending=*pendingSkeletalTest;
                        if(assemblyRootCount||candidateRequest||assemblyComponents.Count||mesh||capture||target||asset||
                            pending.RootCount>pending.Roots.size()||pending.RootCount>std::size(assemblyRoots))
                        {LOG_ERROR("MiniMap: skeletal-only handoff rejected: capture owner not empty or root capacity invalid; existing resources retained");RetirePendingSkeletalTest();return;}
                        const auto decision=CheckCandidate(pending.Request,pending.SourceRecipe,pending.World,true);
                        if(!decision.Allowed())
                        {LOG_WARN("MiniMap: skeletal-only pending diagnostic cancelled: %s; no retry",decision.Reason);RetirePendingSkeletalTest();return;}
                        // Finish all potentially throwing CPU copies before moving any
                        // native roots. Each deduplicated pending root owns one retain.
                        auto request=std::make_unique<CandidateRequest>(pending.Request);
#if defined(MINIMAP_BUILDING_SPRITES)
                        // The independent diagnostic shares the same native owner,
                        // but must not reuse the retired static request's lease.
                        if(!MiniMapBuildingSprites::AcquireObservationLease(pending.Request.Candidate.Identity,
                            pending.Request.Lease.SourceRevision,request->Lease))return;
                        ownerLease=request->Lease;
#endif
                        static_assert(std::is_nothrow_copy_assignable_v<SA::Recipe>);
                        assemblyRecipe=pending.SourceRecipe;recipeWorld=pending.World;
                        std::copy_n(pending.Roots.begin(),pending.RootCount,std::begin(assemblyRoots));
                        assemblyRootCount=pending.RootCount;
                        pending.Roots.fill(nullptr);pending.RootCount=0; // ownership moved, no Drop here
                        candidateRequest=request.release();skeletalOnly=true;assemblyPending=true;
                        RetirePendingSkeletalTest();
                        LOG_INFO("MiniMap: skeletal-only diagnostic acquired retired single capture owner; production static sprite remains published");
                    }
                    catch(...){RetirePendingSkeletalTest();LOG_WARN("MiniMap: skeletal-only pending handoff exception; no capture");return;}
                }
                if(!assemblyPending)
                {
                    try{ServiceResourceWatch();}catch(...){LOG_WARN("MiniMap: bounded resource observation failed; no capture dispatched");}
                    // A readiness handoff is followed by the owner's existing
                    // source validation/setup on a later invocation, not this slice.
                    if(assemblyPending)return;
                }
                if(!assemblyPending)return;
                const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
                if(!snapshot||!snapshot->Complete)return; // reconciliation is not terminal source removal
                if(!recipeWorld||recipeWorld!=MiniMapMap::GetWorld()||recipeWorld==endingWorld||snapshot->Generation!=assemblyRecipe.Generation||
                    !MiniMapBuildingRepresentation::ValidateCaptureObject(assemblyRecipe.ActorClass))
                {ReleaseAssemblySources();LOG_WARN("MiniMap: R2.5b pending authored recipe cancelled: world/generation changed; no retry");return;}
                const auto decision=candidateRequest?CheckCandidate(*candidateRequest,assemblyRecipe,recipeWorld,assemblyRootCount!=0):
                    MiniMapSpriteCapture::Decision{MiniMapSpriteCapture::State::Rejected,"candidate_request_missing"};
                if(!decision.Allowed())
                {if(candidateRequest&&!skeletalOnly&&(!std::strcmp(decision.Reason,"invalid_or_stale_source")||
                    !std::strcmp(decision.Reason,"candidate_revalidation_exception")||!std::strcmp(decision.Reason,"capture_lifecycle_unavailable")))
                    MiniMapBuildingSprites::AbandonCapture(candidateRequest->Lease,true);
                 ReleaseAssemblySources();LOG_WARN("MiniMap: R2.7a candidate cancelled before setup: %s; no retry",decision.Reason);return;}
                assemblyActive=true;assemblyPending=false;finished=false;attempted=false;captureCommitted=false;
                retirementPending=false;cancelled.store(false,std::memory_order_release);reported=0;frame=0;
                if(!skeletalOnly)LOG_INFO("MiniMap: R2.5b static-template capture eligible: bounded verified instances; original R2 retired; skeletal omitted");
            }
            if(!attempted)
            {
                if(!MiniMapMap::GameplayReady())return;
                auto* world=MiniMapMap::GetWorld();
                // Map publishes only the host's ChimeraMain-filtered lifecycle pointer.
                // All reads/clears and submission are serialized on the game thread.
                if(!world||world==endingWorld||!assemblyActive)return;
                if(world!=MiniMapMap::GetWorld())return;
                try {Submit(world);}
                catch(...)
                {
                    if(!captureCommitted)FailBeforeCapture("assembly setup exception before commitment");
                    else {Native::Cancel(job);finished=true;FailSpritePublication("capture_exception_after_commit; resources_retained");LOG_ERROR("MiniMap: R2.5b exception after commitment; native state uncertain, resources retained");}
                }
                return;
            }
            if(!job)return;
            if(retirementPending)
            {
                try { ServiceCaptureRetirement(); }
                catch(...) { finished=true;FailSpritePublication("retirement_exception; resources_retained");LOG_ERROR("MiniMap: R2 capture retirement exception; resources retained"); }
                return;
            }
            {MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::CaptureReadback);Native::Service(job,++frame);} // never consult the old world here.
            const auto report=Native::Inspect(job);
            if((report.Visited&Bit(Native::Phase::CopySubmitted))&&!(reported&Bit(Native::Phase::CopySubmitted)))
            {reported|=Bit(Native::Phase::CopySubmitted);LOG_INFO("MiniMap: R2 GPU readback copy submitted; later-frame readiness polling");}
            if(report.Quarantined)
            {
                try { if(RecoverRejectedCapture())return; }
                catch(...) { LOG_ERROR("MiniMap: R2 capture-retirement native state uncertain; quarantine retained"); }
                finished=true;FailSpritePublication(report.Failure?report.Failure:"native_state_uncertain; resources_retained");LOG_ERROR("MiniMap: R2 recovery rejected; existing quarantine retained: %s",report.Failure?report.Failure:"unknown");return;
            }
            if(report.Current!=Native::Phase::CleanupComplete||report.Callables||report.Outstanding)return;
            if(assemblyActive&&candidateRequest)
                LOG_INFO("MiniMap: R2.7a candidate=%s readback=%s; PNG publication=%s; existing cleanup follows",
                    MiniMapSpriteCapture::SessionVisualKey(candidateRequest->Candidate).c_str(),
                    report.Result==Native::Outcome::Verified?"verified":"failed_or_cancelled",
                    cancelled.load()?"cancelled":report.Result==Native::Outcome::Verified?"export_follows":"not_attempted");
            if(!MiniMapMap::GameplayReady())
                FailSpritePublication("gameplay_admission_closed; publication_cancelled; native_retirement_continues");
            if(!cancelled.load()&&MiniMapMap::GameplayReady())
            {
                if(const auto* stats=Native::FloatStats(job))
                {
                    LOG_INFO("MiniMap: R2 readback %dx%d pitchPixels=%d height=%d latency=%.3fms/%llu frames; finite=%llu nonfinite=%llu RGB=[%.6g,%.6g] rawAlpha=[%.6g,%.6g] candidate inverse-alpha foreground=%llu background=%llu partial=%llu coverageSum=%.3f",
                        Dimension,Dimension,report.Pitch,report.BufferHeight,report.CompletionMs,(unsigned long long)report.CompletionFrames,
                        (unsigned long long)stats->Finite,(unsigned long long)stats->Nonfinite,double(stats->RgbMin),double(stats->RgbMax),double(stats->AlphaMin),double(stats->AlphaMax),
                        (unsigned long long)stats->Foreground,(unsigned long long)stats->Background,(unsigned long long)stats->Partial,stats->Coverage);
                    for(size_t i=0;i<stats->SampleCount;++i){const auto& p=stats->Samples[i];LOG_INFO("MiniMap: R2 sample (%zu,%zu) rawRGBA=(%.6g,%.6g,%.6g,%.6g)",p.X,p.Y,double(p.Rgba[0]),double(p.Rgba[1]),double(p.Rgba[2]),double(p.Rgba[3]));}
                    if(!stats->Foreground||!stats->Background) LOG_WARN("MiniMap: R2 image evidence inconclusive: candidate foreground/background separation absent");
                    if(staticEvidenceActive&&assemblyActive&&!skeletalOnly&&candidateRequest)
                    {
                        LOG_INFO("MiniMap: static capture lifecycle A4.6: generation=%llu building=%u attempt=1 mode=%s submitToReadbackMs=%.3f foreground=%llu readback=%s; publication validation follows; no retry",
                            (unsigned long long)assemblyRecipe.Generation,candidateRequest->Source.BuildingId,publicStaticCapture?"public_CaptureScene":"direct_builder",
                            std::chrono::duration<double,std::milli>(CaptureClock::now()-captureSubmittedAt).count(),(unsigned long long)stats->Foreground,
                            report.Result==Native::Outcome::Verified?"verified":"failed");
                        staticEvidenceActive=false;
                        const bool clearCompatible=!stats->Foreground&&!stats->Nonfinite&&stats->RgbMin==0&&stats->RgbMax==0&&stats->AlphaMin==1&&stats->AlphaMax==1;
                        LOG_INFO("MiniMap: static capture result evidence A4.4a: generation=%llu building=%u unchangedClearCompatible=%d foreground=%llu; empty output cannot establish tile identity, material opacity, rendered LOD or proxy readiness; no synthetic geometry or pillar acquisition",
                            (unsigned long long)assemblyRecipe.Generation,candidateRequest->Source.BuildingId,int(clearCompatible),(unsigned long long)stats->Foreground);
                    }
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
            {ReleaseObjects();finished=true;assemblyActive=false;baselineRetired=true;if(skeletalOnly)LOG_INFO("MiniMap: skeletal-only retirement complete; component destroyed through shared ledger, strong references released; no production publication");LOG_INFO("MiniMap: R2 cleanup complete: no outstanding callable, readback released, components destroyed and strong references released");}
        }
        void BeginShutdownImpl()
        {

            stopping.store(true,std::memory_order_release);cancelled.store(true,std::memory_order_release);
            // Shared job pointer is inspected only under the scheduling guard.
            if(!scheduling.test_and_set(std::memory_order_acquire))
            {Scope scope;Native::Cancel(job);if(GameThread()){
                privateWorld.EndSession(MiniMapMap::GetWorld());
                // A.4.9l TEMPORARY NEXT-TICK STATIC CAPTURE: unsubmitted jobs need no GPU retirement.
                if(preparedStatic.Pending)FailBeforeCapture("shutdown_before_static_dispatch");
                // One nonblocking retirement opportunity; never wait during shutdown.
                if(preparedStatic.Cancelling&&preparedStatic.Pending)DispatchPreparedStatic();
                // An accepted request can still be waiting for its first setup
                // tick. It owns source roots/lease but no capture resources.
                if(!assemblyActive&&assemblyPending&&!job&&!mesh&&!capture&&!target&&!asset&&
                    !assemblyComponents.Count&&!preparedStatic.Pending)ReleaseAssemblySources();
                ClearResourceWatches();MiniMapRendererReadiness::ReleaseMetadata();RetirePendingSkeletalTest();Unregister();}}
        }
        void BeginShutdown()
        {
            MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::CaptureShutdown,false);
            BeginShutdownImpl();
        }

    }
    bool HasResourceWatchCapacity()
    {for(const auto* w:resourceWatches)if(!w)return true;return false;}
    void OfferParallelBlueprint(const MiniMapSpriteCapture::Candidate& candidate, uintptr_t type,
        uint32_t building, void* gameplayWorld) noexcept
    {
        if (!resolved || !registered || !GameThread() || stopping.load() ||
            scheduling.test_and_set(std::memory_order_acquire)) return;
        Scope scope;
        privateWorld.OfferCandidate(candidate,type,building,gameplayWorld);
    }
    bool CanAcceptCandidate()
    {
        if(!MiniMapMap::GameplayReady())return false;return resolved&&registered&&GameThread()&&!stopping.load()&&finished&&baselineRetired&&!job&&!mesh&&!capture&&!target&&!assemblyActive&&!assemblyPending&&!assemblyRootCount&&!pendingSkeletalTest&&!ownerLease;}
    MiniMapCaptureLeases::ScopedArmResult ArmRetirementObservation(const MiniMapCaptureDispatch::OwnerObservation& before,
        const MiniMapSpriteDiscoveryCore::Key& key) noexcept
    {
#if defined(MINIMAP_BUILDING_SPRITES)
        try{if(GameThread()&&before.QueryAllowed&&before.Available&&!before.NativeOwner&&CanAcceptCandidate()){
            const auto arm=MiniMapBuildingSprites::ArmRetirementObservation(before.Leases,key);
            return {arm.Status,arm.Ticket,MiniMapCaptureLeases::ObservationFence(arm.Ticket,&MiniMapBuildingSprites::SealRetirementObservation)};
        }}catch(...){}
#endif
        return {};
    }
    MiniMapCaptureDispatch::AttemptReconciliation ReconcileRetirementObservation(const MiniMapCaptureLeases::ObservationTicket& ticket) noexcept
    {
#if defined(MINIMAP_BUILDING_SPRITES)
        try{if(GameThread()){
            const auto observation=MiniMapBuildingSprites::ObserveRetirement(ticket);
            if(observation.Sealed&&observation.State==MiniMapCaptureLeases::ObservationState::NotAdmitted)
                return {{MiniMapCaptureDispatch::OwnerState::NotAdmitted,false},{}};
            if(observation.Sealed&&observation.Lease)return {ReconcileNativeOwner(observation.Lease),observation.Lease};
        }}catch(...){}
#endif
        // Awaiting admission is NOT proof that a handoff has definitively ended.
        return {};
    }
    MiniMapCaptureDispatch::OwnerObservation ObserveNativeOwner() noexcept
    {
        MiniMapCaptureDispatch::OwnerObservation out;
#if defined(MINIMAP_BUILDING_SPRITES)
        try{
            if(!GameThread())return out;
            out.QueryAllowed=true;out.NativeOwner=ownerLease;out.Leases=MiniMapBuildingSprites::ObserveLeases();
            out.Available=out.Leases.Incarnation&&out.Leases.State==MiniMapCaptureLeases::Ownership::Returned&&CanAcceptCandidate();
        }catch(...){return {};}
#endif
        return out;
    }
    MiniMapCaptureDispatch::Reconciliation ReconcileNativeOwner(const MiniMapCaptureLeases::Token& token) noexcept
    {
#if defined(MINIMAP_BUILDING_SPRITES)
        try{
            if(!GameThread())return {};
            MiniMapCaptureDispatch::OwnerEvidence evidence;
            evidence.GameThread=true;evidence.OwnerMatches=bool(token)&&ownerLease==token;
            evidence.RequestMatches=candidateRequest&&candidateRequest->Lease==token;
            evidence.CapturePending=assemblyPending||assemblyActive||job||preparedStatic.Pending||pendingSkeletalTest;
            // A newly queued request inherits the prior idle finished/cancelled flags until Tick prepares it.
            evidence.CleanupPending=retirementPending||(!assemblyPending&&(cancelled.load()||finished));
            evidence.Uncertain=assemblyAssociationUncertain||(finished&&job&&!retirementPending);
            return MiniMapCaptureDispatch::Reconcile(MiniMapBuildingSprites::InspectLease(token),evidence);
        }catch(...){return {};}
#else
        (void)token;return {};
#endif
    }
    const char* CandidateServiceStatus()
    {
        if(!resolved||!registered)return "native_capture_unavailable";
        if(stopping.load())return "stopped";
        if(finished&&(job||assemblyAssociationUncertain))return "quarantined_resources_retained; further_captures_deferred";
        if(assemblyPending||assemblyActive||job||retirementPending||pendingSkeletalTest)return "capture_or_retirement_pending";
        if(ownerLease)return "lease_retirement_pending";
        return CanAcceptCandidate()?"idle":"unavailable";
    }
    MiniMapSpriteCapture::Decision OfferStaticAssembly(const MiniMapSpriteCapture::Candidate& candidate,
        const MiniMapBuildingCollector::Record& source,const MiniMapStaticAssembly::Recipe& recipe,void* world)
    {
#if defined(MINIMAP_BUILDING_SPRITES)
        MiniMapBuildingSprites::TimingScope admissionTiming(MiniMapBuildingSprites::Timing::CaptureAdmission);
#endif
        namespace P=MiniMapSpriteCapture;
#if !defined(MINIMAP_STAGE_R25B_STATIC_ASSEMBLY)
        (void)candidate;(void)source;(void)recipe;(void)world;return P::Decision{P::State::Rejected,"static_assembly_experiment_disabled",P::Cause::UnsupportedRepresentation}.RejectedOffer();
#else
        if(!candidate.General)return P::Decision{P::State::Rejected,"unsupported_candidate_mode",P::Cause::UnsupportedRepresentation}.RejectedOffer();
        if(!GameThread()||stopping.load())return P::Decision{P::State::Rejected,"capture_lifecycle_unavailable",P::Cause::LifecycleUnavailable}.RejectedOffer();
        if(!CanAcceptCandidate()||scheduling.test_and_set(std::memory_order_acquire))return P::Decision{P::State::Rejected,"capture_owner_unavailable_or_already_attempted",P::Cause::NativeOwnerBusy}.RejectedOffer();
        Scope scope;
        skeletalOnly=false;
        struct Rollback {bool Committed=false;~Rollback(){if(!Committed)ReleaseAssemblySources();}} rollback;
        if(candidate.Classification.ExcludeFromMiniMap==P::K::Exclusion::Exclude||candidate.Classification.Reason=="InteriorTag")
            return P::Decision{P::State::Rejected,"classification_excluded",P::Cause::Policy}.RejectedOffer();
        // The old startup capture ran Preflight in Submit before any assembly
        // handoff. General mode deliberately skips that capture. Initialize the
        // validated GPU-count snapshot here, BEFORE CheckCandidate consumes it.
        // CanAcceptCandidate proved this owner has no outstanding native job;
        // never write the snapshot from render-thread callbacks or in-flight checks.
        if(!Native::Preflight())
        {LOG_WARN("MiniMap: general sprite admission preflight rejected: %s; no sources retained or capture submitted",Native::Status());
         return P::Decision{P::State::Rejected,"capture_backend_preflight_unavailable",P::Cause::BackendUnavailable}.RejectedOffer();}
        LOG_INFO("MiniMap: general sprite admission preflight complete; idle owner, RHI/threading/GPU-count/backend verified");
        auto request=std::make_unique<CandidateRequest>(CandidateRequest{candidate,source});
        const auto decision=CheckCandidate(*request,recipe,world,false);
        LOG_INFO("MiniMap: R2.7a candidate=%s classification=%s representation=%s eligibility=%s",
            P::D::KeyText(candidate.Identity).c_str(),P::K::Label(request->Candidate.Classification.ExcludeFromMiniMap),
            request->Candidate.RepresentationVerified?"verified_static_reference":"unverified",decision.Reason);
        if(!decision.MayRetain())return decision.RejectedOffer();
        SA::Footprint footprint;
        const auto* reason=SA::Validate(recipe,footprint);
        if(reason)return P::Decision{P::State::Rejected,reason,P::Cause::RecipeInvalid}.RejectedOffer();
        auto retain=[&](uintptr_t p)
        {
            if(!MiniMapBuildingRepresentation::ValidateCaptureObject(p))return false;
            auto* object=reinterpret_cast<SDK::UObject*>(p);
            for(size_t i=0;i<assemblyRootCount;++i)if(assemblyRoots[i]==object)return true;
            if(assemblyRootCount>=std::size(assemblyRoots))return false;
            Hold(object);assemblyRoots[assemblyRootCount++]=object;return true;
        };
        if(!retain(recipe.ActorClass))return P::Decision{P::State::Rejected,"actor_class_retention_failed",P::Cause::RetentionUnavailable}.RejectedOffer();
        for(size_t i=0;i<recipe.Count;++i)
        {
            if(!retain(recipe.Instances[i].Mesh))return P::Decision{P::State::Rejected,"mesh_retention_failed",P::Cause::RetentionUnavailable}.RejectedOffer();
            for(size_t s=0;s<recipe.Instances[i].MaterialCount;++s)
                if(!retain(recipe.Instances[i].Materials[s]))return P::Decision{P::State::Rejected,"material_retention_failed",P::Cause::RetentionUnavailable}.RejectedOffer();
        }
        const auto retained=CheckCandidate(*request,recipe,world,true);
        if(!retained.Allowed())return retained.RejectedOffer();
        MiniMapRendererReadiness::Dependencies baseline;
        MiniMapBuildingSprites::DependencySnapshot(candidate.Identity,baseline);
        request->Observation=MiniMapRendererReadiness::ObserveRetainedAssembly(recipe,&baseline);
        assemblyRecipe=recipe;assemblyBounds=footprint;recipeWorld=static_cast<SDK::UWorld*>(world);assemblyPending=true;
#if defined(MINIMAP_BUILDING_SPRITES)
        if(!PrepareResourcePublication(candidate.Identity,request->Observation))
            return P::Decision{P::State::Rejected,"resource_advancement_publication_pending",P::Cause::ResourcePublicationPending}.RejectedOffer();
        MiniMapBuildingSprites::ObserveAssembly(candidate.Identity,request->Observation);
        // Reevaluation may discover pending work without justifying another GPU
        // capture. Keep that observation ticket independently of capture admission.
        try{WatchResources(*request,recipe,static_cast<SDK::UWorld*>(world));}catch(...){}
        // Reacquisition can itself observe completion while no native watch was
        // held. Provider-owned evidence follows the identical admission rule.
        for(size_t i=0;i<request->Observation.TaskCount;++i)
            MiniMapBuildingSprites::NotifyProviderChange(candidate.Identity,request->Observation.Tasks[i].Evidence,request->Observation.Source);
        const auto admission=MiniMapBuildingSprites::Queue(candidate.Identity,request->Lease);
        if(admission!=MiniMapBuildingSpriteCore::Admission::Accepted)
        {
            // Observation-only source evaluation is successful without reserving
            // a GPU capture. No boolean rejection can erase its CPU advancement.
            auto observation=retained.WithHandoff(P::HandoffOutcome::ObservationOnly);
            observation.Admission=admission;
            const auto source=MiniMapBuildingSprites::InspectDispatch(candidate.Identity,{});
            if(source.Current&&source.SourceKnown)observation.SourceRevision=source.SourceRevision;
            return observation;
        }
#endif
        ownerLease=request->Lease;
        candidateRequest=request.release();rollback.Committed=true;
        QueueSkeletalTest(*candidateRequest,recipe,static_cast<SDK::UWorld*>(world));
        LOG_INFO("MiniMap: renderer readiness A4.9: quality=Unknown; safe capture admitted for provisional session display; upgrades require actual engine-resource improvement");
        LOG_INFO("MiniMap: validated authored recipe handed off with independent strong roots; provisional capture scheduled");
        LOG_INFO("MiniMap: progressive capture accepted: %s; renderer quality unknown; provisional session sprite permitted; no persistent cache or atlas",retained.Reason);
        auto accepted=retained.WithHandoff(P::HandoffOutcome::Admitted);
#if defined(MINIMAP_BUILDING_SPRITES)
        accepted.Admission=MiniMapBuildingSpriteCore::Admission::Accepted;
        accepted.SourceRevision=ownerLease.SourceRevision;accepted.NativeLease=ownerLease;
#endif
        return accepted;
#endif
    }
    void OnGameplaySessionReady(void* world, uint64_t generation) noexcept
    {
        if (!registered || stopping.load() || !world || !generation)
        { LOG_WARN("MiniMap: private_capture_world step=owner_readiness outcome=failure reason=capture_worker_or_session_unavailable"); return; }
        if (!GameThread())
        { LOG_ERROR("MiniMap: private_capture_world step=game_thread outcome=failure reason=session_notification_off_thread"); return; }
        if (scheduling.test_and_set(std::memory_order_acquire))
        { LOG_ERROR("MiniMap: private_capture_world step=owner_readiness outcome=failure reason=session_notification_overlaps_worker"); return; }
        Scope scope;
        privateWorld.OnSessionReady(world, generation);
    }
    bool ResolvePrerequisites()
    {
        resolved = MiniMapNative::IsInitialized();
        return resolved;
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
        finished=true;attempted=true;baselineRetired=true;
        LOG_INFO("MiniMap: skeletal-only scheduling diagnostics A3.4a enabled; capture build=%s %s; one diagnostic attempt, production publication disabled",__DATE__,__TIME__);
        LOG_INFO("MiniMap: building capture owner enabled; registry-driven, one active job");
    }
    void Shutdown()
    {
        BeginShutdownImpl();
        if(registered){engine->UnregisterOnTick(&Tick);engine->UnregisterOnShutdown(&BeginShutdown);worlds->UnregisterOnBeforeWorldEndPlay(&BeforeEndPlay);registered=false;}
        if(scheduling.test_and_set(std::memory_order_acquire)){LOG_ERROR("MiniMap: R2 shutdown overlaps callback; DLL unload NOT certified");return;}
        Scope scope;
        Native::Cancel(job);
        if(!GameThread()){if(mesh||capture||target||job)LOG_ERROR("MiniMap: R2 shutdown off game thread: resources retained; cleanup NOT certified");return;}
        ClearResourceWatches();MiniMapRendererReadiness::ReleaseMetadata();RetirePendingSkeletalTest();
        if(Native::DestroyIfRetired(job))ReleaseObjects();
        else LOG_ERROR("MiniMap: R2 shutdown with pending native work: resources retained, no wait; callback code is NOT safe for FreeLibrary; active hot reload unsupported");
    }
}
#endif
