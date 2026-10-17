#include "CallbackTiming.h"
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE) && defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSpriteCapture.h"
#include "PrivateCaptureWorld.h"
#include "SpriteCaptureCore.h"
#include "BuildingSprites.h"
#include "Map/Map.h"
#include "Native/NativeApi.h"
#include "plugin_helpers.h"
#include <atomic>

namespace MiniMapBuildingCapture
{
    namespace
    {
        MiniMapSpriteCapture::PrivateCaptureWorld privateWorld;
        bool resolved=false,registered=false;
        std::atomic<bool> stopping{false};
        std::atomic_flag scheduling=ATOMIC_FLAG_INIT;
        struct Scope {~Scope(){scheduling.clear(std::memory_order_release);}};
        IPluginEngineEvents* engine=nullptr;
        IPluginWorldEvents* worlds=nullptr;
        bool GameThread(){return MiniMapNative::Get()&&MiniMapNative::Get()->player.isGameThread&&MiniMapNative::Get()->player.isGameThread();}
        void Tick(float)
        {
            MiniMapCallbackTiming::Scope timing(MiniMapCallbackTiming::Id::CaptureTick,true);
            if(stopping.load()||!GameThread()||scheduling.test_and_set(std::memory_order_acquire))return;
            Scope scope;privateWorld.Service();
        }
        void BeforeEndPlay(SDK::UWorld* world,const char*)
        {
            MiniMapCallbackTiming::Scope timing(MiniMapCallbackTiming::Id::CaptureWorldEnd,false);
            if(!world||world!=MiniMapMap::GetWorld())return;
            if(!GameThread()||scheduling.test_and_set(std::memory_order_acquire))
            {LOG_ERROR("MiniMap: private capture world-end overlaps native work or is off game thread; retirement not certified");return;}
            Scope scope;privateWorld.EndSession(world);
        }
        void BeginShutdown()
        {
            MiniMapCallbackTiming::Scope timing(MiniMapCallbackTiming::Id::CaptureShutdown,false);
            stopping.store(true,std::memory_order_release);
            if(!GameThread()||scheduling.test_and_set(std::memory_order_acquire))
            {LOG_ERROR("MiniMap: private capture shutdown overlaps native work or is off game thread; retirement not certified");return;}
            Scope scope;privateWorld.EndSession(MiniMapMap::GetWorld());
        }
    }
    bool CanAcceptCandidate()
    {return resolved&&registered&&GameThread()&&!stopping.load()&&MiniMapMap::GameplayReady()&&privateWorld.CanAcceptCandidate();}
    MiniMapSpriteCapture::Decision OfferBlueprint(const MiniMapSpriteCapture::Candidate& candidate,
        uintptr_t type,uint32_t building,void* world) noexcept
    {
        namespace P=MiniMapSpriteCapture;
        if(!CanAcceptCandidate()||scheduling.test_and_set(std::memory_order_acquire))
            return P::Decision{P::State::Rejected,"private_capture_owner_unavailable",P::Cause::NativeOwnerBusy}.RejectedOffer();
        Scope scope;return privateWorld.OfferCandidate(candidate,type,building,world);
    }
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
            out.QueryAllowed=true;out.NativeOwner=privateWorld.OwnerLease();out.Leases=MiniMapBuildingSprites::ObserveLeases();
            out.Available=out.Leases.Incarnation&&out.Leases.State==MiniMapCaptureLeases::Ownership::Returned&&CanAcceptCandidate();
        }catch(...){return {};}
#endif
        return out;
    }
    MiniMapCaptureDispatch::Reconciliation ReconcileNativeOwner(const MiniMapCaptureLeases::Token& token) noexcept
    {return GameThread()?privateWorld.Reconcile(token):MiniMapCaptureDispatch::Reconciliation{};}
    const char* CandidateServiceStatus()
    {return !resolved||!registered?"native_capture_unavailable":stopping.load()?"stopped":privateWorld.Status();}
    void OnGameplaySessionReady(void* world,uint64_t generation) noexcept
    {
        if(!registered||stopping.load()||!world||!generation||!GameThread()||scheduling.test_and_set(std::memory_order_acquire))return;
        Scope scope;privateWorld.OnSessionReady(world,generation);
    }
    bool ResolvePrerequisites(){resolved=MiniMapNative::IsInitialized();return resolved;}
    void Initialize()
    {
        if(!resolved||registered)return;
        auto* hooks=GetHooks();if(!hooks||!hooks->Engine||!hooks->World)return;
        engine=hooks->Engine;worlds=hooks->World;
        if(!engine->RegisterOnTick||!engine->UnregisterOnTick||!engine->RegisterOnShutdown||!engine->UnregisterOnShutdown||
            !engine->GetStaticFindObjectByNameAddress||!engine->GetStaticFindObjectByNameAddress()||
            !worlds->RegisterOnBeforeWorldEndPlay||!worlds->UnregisterOnBeforeWorldEndPlay)return;
        stopping.store(false);
        engine->RegisterOnTick(&Tick);engine->RegisterOnShutdown(&BeginShutdown);
        worlds->RegisterOnBeforeWorldEndPlay(&BeforeEndPlay);registered=true;
        LOG_INFO("MiniMap: private Blueprint building capture enabled; one active owner, one attempt per session candidate");
    }
    void Shutdown()
    {
        BeginShutdown();
        if(registered){engine->UnregisterOnTick(&Tick);engine->UnregisterOnShutdown(&BeginShutdown);
            worlds->UnregisterOnBeforeWorldEndPlay(&BeforeEndPlay);registered=false;}
    }
}
#endif
