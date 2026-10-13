#pragma once
#include "RendererReadinessCore.h"
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE) && defined(MODLOADER_CLIENT_BUILD)
namespace MiniMapStaticAssembly {struct Recipe;}
namespace MiniMapRendererReadiness
{
    struct PreparedResource
    {
        uintptr_t Object{},Class{}; // UObject, independently retained by ticket/capture owner
        uint64_t World{};
        int32_t Index=-1,Name=-1;uint32_t Number{};
    };
    struct Provider
    {
        uint32_t Id{},Conditions{},PollMilliseconds{};
        ObservationMethod Method=ObservationMethod::Unavailable;
        bool (*Relevant)(const ProviderObservation&,const ProviderObservation&)=nullptr;
        bool (*Observe)(const PreparedResource&,const ProviderObservation&,ProviderObservation&)=nullptr;
    };
    // Registration and observations are GT-only. No provider is registered
    // until an actual implementation with a verified lifetime contract exists.
    bool RegisterProvider(const Provider&);
    const Provider* FindProvider(uint32_t);
    uint64_t ProviderEpoch();
    uint32_t ProviderConditions();
    struct ObservationTask
    {
        PreparedResource Resource;
        ProviderObservation Evidence;
        uint16_t Contributors{};
    };
    struct AssemblyObservation
    {
        Result Readiness;
        std::vector<uint64_t> Source;
        std::array<MeshState,16> Meshes{};
        size_t UniqueMeshes{};
        std::array<ObservationTask,16> Tasks{};
        size_t TaskCount{};
        const char* Reason="renderer_readiness_unknown";
    };
    // GT only, caller holds independent strong references to every recipe asset.
    // Synchronous bounded snapshots; no native render pointer escapes the call.
    AssemblyObservation ObserveRetainedAssembly(const MiniMapStaticAssembly::Recipe&,const Dependencies* baseline=nullptr);
    // Uses prepared rooted identities, no metadata acquisition/class discovery.
    bool ObserveTask(AssemblyObservation&,size_t,ProviderObservation&);
    void InvalidateTask(AssemblyObservation&,size_t); // GT: unavailable evidence becomes Unknown
    void ReleaseMetadata(); // GT, after tickets/captures no longer need preparation metadata
}
#endif
