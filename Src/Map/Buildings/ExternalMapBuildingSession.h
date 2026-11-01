#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingCollector.h"
#include "ExternalMapBuildingSessionAbi.h"
#include "BuildingVisuals.h"
#include <cstdint>
#include <memory>

namespace SDK { class UWorld; }
namespace SDK { class UTexture2D; }

namespace MiniMapBuildingCollector
{
    struct ExternalMapSessionDiagnostics
    {
        uint64_t SessionGeneration = 0;
        CollectorDiagnostics Collector{};
        bool Active = false;
        bool Admitted = false;
        bool WorldInvalidated = false;
    };
    // Caller-driven generic external renderer. It owns no callbacks
    // and is inactive until OpenSession succeeds.
    class ExternalMapBuildingSession
    {
    public:
        ExternalMapBuildingSession() = default;
        ~ExternalMapBuildingSession();
        ExternalMapBuildingSession(const ExternalMapBuildingSession&) = delete;
        ExternalMapBuildingSession& operator=(const ExternalMapBuildingSession&) = delete;

        bool OpenSession(const Bounds& worldRectangle);
        bool UpdateWorldRectangle(const Bounds& worldRectangle);
        void Service(float delta);
        std::shared_ptr<const Snapshot> GetSnapshot();
        // Explicit external-render request only. Returns null if the latest
        // published atlas cannot be imported with the required texture state.
        SDK::UTexture2D* SynchronizeNativeAtlasTexture();
        ExternalMapSessionDiagnostics GetDiagnostics() const;
        bool CloseSession();
        MiniMapBuildingAbiResult Render(const MiniMapBuildingRenderRequest&, MiniMapBuildingRenderStatus&);
        void InvalidateOnWorldEnd();
        void Shutdown();

    private:
        bool IsCurrentAdmission(SDK::UWorld*& world, uint64_t& worldGeneration) const;

        std::unique_ptr<BuildingCollector> collector_;
        std::shared_ptr<MiniMapBuildingVisuals::State> visuals_;
        std::shared_ptr<const Snapshot> renderedSnapshot_;
        Bounds renderedRectangle_{};
        SDK::UTextureRenderTarget2D* renderedDestination_=nullptr; // Identity only, caller retains it.
        uint64_t renderedAtlasSession_=0,renderedAtlasRevision_=0;
        bool rendered_=false,renderedShowLayer_=false,renderedDroneRails_=false;
        SDK::UWorld* admittedWorld_ = nullptr;
        uint64_t admittedWorldGeneration_ = 0;
        uint64_t sessionGeneration_ = 0;
        SDK::UTexture2D* nativeAtlasTexture_ = nullptr;
        uint64_t nativeAtlasSession_ = 0;
        uint64_t nativeAtlasRevision_ = 0;
        // Failed attempt identity is separate from successfully uploaded state.
        uint64_t rejectedAtlasSession_ = 0;
        uint64_t rejectedAtlasRevision_ = 0;
        int32_t nativeAtlasWidth_ = 0;
        int32_t nativeAtlasHeight_ = 0;
        bool active_ = false;
        bool worldInvalidated_ = false;
        bool stopping_ = false;
    };
    void ShutdownExternalMapBuildingSessionAbi();
    void InvalidateExternalMapBuildingSessionAbi();
}
#endif
