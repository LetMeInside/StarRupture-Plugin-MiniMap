#pragma once

// Declares the client-side StarRupture collector and immutable Snapshot source data.
#if defined(MODLOADER_CLIENT_BUILD)
#include "plugin.h"
#include "BuildingAcquisitionTypes.h"
#include "BuildingGeometryTypes.h"
#include "BuildingSplineProfiles.h"
#include "Map/MapTransform.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace MiniMapBuildingCollector
{
    struct DefinitionGeometry
    {
        std::shared_ptr<const MiniMapBuildingSplineProfiles::Set> Profiles;
    };
    struct Record
    {
        uint64_t Generation = 0;
        int32_t Index = 0, Serial = 0;
        uint32_t BuildingId = 0;
        // Copied object index for render tracing, never a native lifetime certificate.
        int32_t DefinitionObjectIndex = 0;
        bool TransformValid = false, DefinitionValid = false, HasSpline = false;
        /** True only after the current Mass spline-fragment lookup established source kind. */
        bool SplineClassificationKnown = false;
        bool InteriorKnown = false, InteriorOnly = false;
        std::array<double, 3> Position = {}, Scale = {};
        std::array<double, 4> Rotation = {};
        Bounds Extent;
        std::shared_ptr<const std::string> CanonicalBlueprint;
        std::shared_ptr<const DefinitionGeometry> DefinitionShape;
        std::shared_ptr<const SplineGeometry> Curve;
    };
    /** Retains immutable legacy rendering records for one collector generation. */
    struct Snapshot
    {
        uint64_t Generation = 0;
        bool Complete = false; // Authoritative membership reconciliation succeeded.
        std::vector<Record> Records;
    };
    struct CollectorDiagnostics
    {
        uint64_t PublicationGeneration = 0;
        uint64_t AuthoritativeBuildingCount = 0;
        uint64_t CompletedBuildingCount = 0;
        uint64_t PublishedInRegionCount = 0;
        uint64_t PendingAcquisitionCount = 0;
        bool Ready = false;
        bool MembershipComplete = false;
    };

    // Reusable, caller-scheduled collector. Each object owns its collection,
    // world session, region and publication state. MiniMap's current events
    // continue to use the namespace facade below.
    class BuildingCollector final
    {
    public:
        BuildingCollector();
        ~BuildingCollector();
        BuildingCollector(const BuildingCollector&) = delete;
        BuildingCollector& operator=(const BuildingCollector&) = delete;
        BuildingCollector(BuildingCollector&&) noexcept;
        BuildingCollector& operator=(BuildingCollector&&) noexcept;

        bool InitializeResources();
        void Reset();
        void OnExperienceLoadComplete();
        bool BeginExplicitSession();
        void SetViewport(const MiniMapMap::Transform& transform);
        /** Selects the explicit legacy collector rectangle when it changes.
         * @param region Map-space bounds used by the collector's explicit acquisition session.
         */
        void SetRegion(const Bounds& region);
        /** Changes the effective legacy rendering policy and marks the current Snapshot dirty.
         * @param enabled Whether drone-rail contributors belong in the next Snapshot.
         */
        void SetRenderDroneRails(bool enabled);
        std::shared_ptr<const Snapshot> GetSnapshot();
        CollectorDiagnostics GetDiagnostics() const;
        void OnTick(float delta);
        void ShutdownResources();
        /** Releases native definition metadata after the current Snapshot is withdrawn. */
        void ShutdownMetadataResources();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };

    // Existing MiniMap compatibility facade; it forwards to one persistent
    // MiniMap-owned BuildingCollector instance.
    bool Initialize(IPluginSelf* self);
    void OnTick(float delta);
    void Reset();
    void OnExperienceLoadComplete();
    void SetViewport(const MiniMapMap::Transform& transform);
    std::shared_ptr<const Snapshot> GetSnapshot();
    void Shutdown();
}
#endif
