#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingCollector.h"
#include <cstdint>
#include <memory>

namespace SDK { class UWorld; }

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
    // Caller-driven owner for the future ExternalMap consumer. It owns no callbacks
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
        ExternalMapSessionDiagnostics GetDiagnostics() const;
        void CloseSession();
        void InvalidateOnWorldEnd();
        void Shutdown();

    private:
        bool IsCurrentAdmission(SDK::UWorld*& world, uint64_t& worldGeneration) const;

        std::unique_ptr<BuildingCollector> collector_;
        SDK::UWorld* admittedWorld_ = nullptr;
        uint64_t admittedWorldGeneration_ = 0;
        uint64_t sessionGeneration_ = 0;
        bool active_ = false;
        bool worldInvalidated_ = false;
    };
    void ShutdownExternalMapBuildingSessionAbi();
}
#endif
