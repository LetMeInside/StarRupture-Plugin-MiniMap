// Implements StarRupture acquisition and freezes immutable sprite and spline source snapshots.
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingCollector.h"
#include "BuildingSourcePublication.h"
#include "DiagnosticExports.h"
#include "Config/Config.h"
#include "UI/MiniMapUI.h"
#include "BuildingBlueprintIdentity.h"
#include "BuildingGeometry.h"
#include "BuildingSplinePayload.h"
#include "BuildingSplinePolicy.h"
#include "BuildingSplineEnvelope.h"
#include "BuildingVisuals.h"
#include "Native/NativeApi.h"
#include "Map/Map.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
#include "plugin_helpers.h"
#include "Services/Runtime.h"
#include "Services/RuntimeEvents.h"
#include <SDK/Chimera_classes.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
    MiniMap::Services::Registration g_tickRegistration{};
    bool g_initialized=false;
}
namespace MiniMapBuildingCollector
{
    using Native=MiniMapNative::NativeApi;
    using Id=MiniMapNative::MassEntityHandle;
    using Clock=std::chrono::steady_clock;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    constexpr auto kMeasurementInterval=std::chrono::seconds(10);
#endif
    namespace P=MiniMapBuildingSplineProfiles;
    namespace G=MiniMapBuildingGeometry;
    struct BuildingCollector::State
    {
    struct Types
    {
        SDK::UScriptStruct *Building=nullptr,*Transform=nullptr,*Parameters=nullptr,*Spline=nullptr,*AP=nullptr;
        SDK::UClass *BuildingData=nullptr,*Visual=nullptr,*Cosmetic=nullptr,*Walkway=nullptr,*Blueprint=nullptr,*SplineMesh=nullptr;
    } types;
    struct Entry
    {
        Record Data;
        const SDK::UAuActorPlacementData* Definition=nullptr;
        bool Complete=false;
        bool DroneRailClassificationKnown=false;
        bool IsDroneRail=false;
        uint64_t SeenScan=0;
    };
    /** Holds session-owned authored definition facts shared by matching incomplete Entries.
     * The native root remains collector-owned; only immutable key/profile owners enter Snapshots.
     */
    struct Definition
    {
        // A strong engine reference makes this engine-facing lookup key stable
        // for the session. It is never exposed to snapshots or renderers.
        SDK::UAuActorPlacementData* Root=nullptr;
        std::shared_ptr<const std::string> Blueprint;
        std::shared_ptr<const DefinitionGeometry> Recipe;
        bool InteriorKnown=false,Interior=false,Prepared=false;
        Definition()=default;
        Definition(const Definition&)=delete;
        Definition& operator=(const Definition&)=delete;
        ~Definition(){if(Root)if(const auto* n=MiniMapNative::Get())n->asset.releaseObject(Root);}
    };
    std::unordered_map<uint64_t,Entry> entities;
    // Scheduling index only: complete index/serial keys of exactly the
    // incomplete authoritative entries in this session; no entity data.
    std::unordered_set<uint64_t> pending;
    std::unordered_map<const SDK::UAuActorPlacementData*,std::shared_ptr<Definition>> definitions;
    std::vector<SDK::UObject*> metadata;
    alignas(16) std::array<std::byte,0x350> query{};
    bool queryConstructed=false,ready=false,dirty=false,remoteTurn=false,membershipComplete=false;
    const void* manager=nullptr;
    SDK::UWorld* world=nullptr;
    uint64_t generation=0;
    uint64_t membershipScanEpoch=0;
    Clock::time_point nextMembership{};
    std::mutex publicationMutex;
    std::shared_ptr<const Snapshot> snapshot;
    std::shared_ptr<BuildingSourcePublication::EligibilityToken> publicationEligibility;
    double viewRadius=0;
    bool renderDroneRails=true;
    bool regionOverride=false;
    Bounds region;
    enum class DiscoveryInput : uint8_t { MiniMapPresentation, ExplicitRegion };
    DiscoveryInput discoveryInput=DiscoveryInput::MiniMapPresentation;
    MiniMapBuildingVisuals::CollectorIntegration::DrawingBoundsVersion drawingBoundsVersion{};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    struct PhaseAggregate
    {
        uint64_t Samples=0;
        double TotalMs=0,MaxMs=0;
        void Add(double ms) noexcept {++Samples;TotalMs+=ms;MaxMs=(std::max)(MaxMs,ms);}
        double MeanMs() const noexcept {return Samples?TotalMs/double(Samples):0;}
    };
    struct Measurements
    {
        PhaseAggregate Membership,MembershipNative,MembershipBookkeeping,MembershipSweep,MembershipRelease,LocalGrid,PendingEnumeration,Acquisition,Maintenance;
        uint64_t MembershipPolls=0,MembershipChanges=0,GridVisitors=0,UniqueLocalKeys=0;
        uint64_t NativeHandles=0,MembershipAdded=0,MembershipRemoved=0,MembershipDuplicates=0,MembershipInvalid=0;
        uint64_t FailedScans=0,MalformedScans=0,AuthoritativeN=0,PendingCount=0;
        double WorstScanMs=0,WorstScanNativeMs=0,WorstScanBookkeepingMs=0,WorstScanSweepMs=0,WorstScanReleaseMs=0;
        double WorstScanBeginMonoMs=0,WorstScanCallbackMs=0;
        uint64_t WorstScanHandles=0,WorstScanN=0,WorstScanPending=0,WorstScanAdded=0,WorstScanRemoved=0,WorstScanGeneration=0,WorstScanSerial=0;
        uint64_t LocalAttempts=0,RemoteAttempts=0,NewlyCompleted=0;
        uint64_t DefinitionAttempts=0,DefinitionSuccesses=0,DefinitionFailures=0;
        uint64_t PendingN=0,PendingP=0,PendingZeroCallbacks=0,RemotePassesSkipped=0;
        uint64_t AtlasSessionChanges=0,AtlasRevisionChanges=0,AtlasBoundsRecalculations=0;
        uint64_t PublicationAttempts=0,Publications=0;
        uint64_t DirtyMembership=0,DirtyRegion=0,DirtyAtlas=0,DirtyAcquisition=0;
        void Reset() noexcept {*this={};}
    } measurements;
    Clock::time_point measurementIntervalStart{};
    uint64_t measurementGeneration=0;
    uint64_t membershipScanSerial=0,lastMembershipScanSerial=0;
#endif
    uint64_t Key(Id id){return uint64_t(uint32_t(id.Index))|(uint64_t(uint32_t(id.SerialNumber))<<32);}
    uint64_t NextMembershipScanEpoch()
    {
        if(membershipScanEpoch==(std::numeric_limits<uint64_t>::max)())
        {
            // Epoch 1 is reused only after every old mark is deterministically cleared.
            for(auto& item:entities)item.second.SeenScan=0;
            membershipScanEpoch=1;
        }
        else ++membershipScanEpoch; // Starts at 1; zero is never an active scan.
        return membershipScanEpoch;
    }
    bool Is(const SDK::UObject* object,const SDK::UClass* base)
    {
        if(!object || !base)return false;
        for(auto* type=object->Class;type;type=static_cast<SDK::UClass*>(type->SuperStruct))if(type==base)return true;
        return false;
    }
    // Called only at initialization/experience initialization, never in an
    // entity-dependent loop. Metadata is rooted until collector shutdown.
    bool InitializeMetadata()
    {
        if(types.Building)return true;
        auto* objects=MiniMap::Services::TryObjects();const auto* n=MiniMapNative::Get();
        if(!objects || !objects->Ready() || !n)return false;
        // Roll back partial initialization without duplicating roots on retry.
        struct Roots
        {
            const Native& N;std::vector<SDK::UObject*> Values;
            ~Roots(){for(auto* p:Values)N.asset.releaseObject(p);}
        } roots{*n,{}};
        auto lookup=[&](const wchar_t* path)
        {
            const auto result=objects->FindResident(nullptr,nullptr,path,false);
            auto* value=result.value?*result.value:nullptr;
            if(value){roots.Values.push_back(value);n->asset.retainObject(value);}return value;
        };
        Types candidate;
        candidate.Building=static_cast<SDK::UScriptStruct*>(lookup(L"/Script/Chimera.CrMassBuildingTag"));
        candidate.Transform=static_cast<SDK::UScriptStruct*>(lookup(L"/Script/MassCommon.TransformFragment"));
        candidate.Parameters=static_cast<SDK::UScriptStruct*>(lookup(L"/Script/Chimera.CrBuildingParameters"));
        candidate.Spline=static_cast<SDK::UScriptStruct*>(lookup(L"/Script/AuActorPlacement.AuSplineConnectionFragment"));
        candidate.AP=static_cast<SDK::UScriptStruct*>(lookup(L"/Script/AuActorPlacement.AuAPMassFragment"));
        candidate.BuildingData=static_cast<SDK::UClass*>(lookup(L"/Script/Chimera.CrBuildingData"));
        candidate.Visual=static_cast<SDK::UClass*>(lookup(L"/Script/MassRepresentation.MassVisualizationTrait"));
        candidate.Cosmetic=static_cast<SDK::UClass*>(lookup(L"/Script/Chimera.CrMassRepVisCosmeticsTrait"));
        candidate.Walkway=static_cast<SDK::UClass*>(lookup(L"/Script/Chimera.CrModularRampWalkway"));
        candidate.Blueprint=static_cast<SDK::UClass*>(lookup(L"/Script/Engine.BlueprintGeneratedClass"));
        candidate.SplineMesh=static_cast<SDK::UClass*>(lookup(L"/Script/Engine.SplineMeshComponent"));
        if(!candidate.Building || !candidate.Transform || !candidate.Parameters || !candidate.Spline || !candidate.AP ||
            !candidate.BuildingData || !candidate.Visual || !candidate.Cosmetic || !candidate.Walkway || !candidate.Blueprint || !candidate.SplineMesh ||
            !MiniMapBuildingBlueprintIdentity::Initialize())return false;
        metadata=std::move(roots.Values);types=candidate;return true;
    }
    const SDK::FCrBuildingParameters* Parameters(const Native& n,Id id)
    {
        const auto* wrapper=n.mass.getConstSharedFragmentPtr(manager,id,types.Parameters);
        const uint8_t* storage=nullptr;if(wrapper)std::memcpy(&storage,wrapper,sizeof(storage));
        if(!storage)return nullptr;
        const auto alignment=uintptr_t(types.Parameters->MinAlignment);
        if(!alignment || (alignment&(alignment-1)))return nullptr;
        const auto address=(reinterpret_cast<uintptr_t>(storage)+sizeof(void*)+alignment-1)&~(alignment-1);
        return reinterpret_cast<const SDK::FCrBuildingParameters*>(address);
    }
    /** Revokes current use as soon as an existing source fact changes. */
    void InvalidateSourcePublication() noexcept
    {
        if(dirty)
        {
            return;
        }

        dirty=true;
        if(publicationEligibility)
        {
            publicationEligibility->IsEligible=false;
        }
    }
    void ClearSession()
    {
        InvalidateSourcePublication();
        if(const auto* n=MiniMapNative::Get())
        {
            if(queryConstructed)n->mass.queryDestruct(query.data());
        }
        queryConstructed=false;manager=nullptr;world=nullptr;region={};regionOverride=false;discoveryInput=DiscoveryInput::MiniMapPresentation;drawingBoundsVersion={};
        pending.clear();entities.clear();definitions.clear();nextMembership={};membershipScanEpoch=0;remoteTurn=false;membershipComplete=false;
        {std::scoped_lock lock(publicationMutex);++generation;snapshot.reset();}
    }
    /** Marks only positively classified drone-rail entries as policy-excluded. */
    void RecordDroneRailClassification(Entry& entry,bool isDroneRail)
    {
        if(entry.DroneRailClassificationKnown && entry.IsDroneRail==isDroneRail)return;
        entry.DroneRailClassificationKnown=true;
        entry.IsDroneRail=isDroneRail;
        InvalidateSourcePublication();
    }
    /** Adds one admitted sprite requirement and retains the immutable bytes behind its view.
     * Completed and pending contributors each append an occurrence; equal Blueprint keys are
     * deliberately not collapsed. Placement is copied only from a validated Record transform.
     * @param target Snapshot receiving the descriptor and key owner.
     * @param record Source facts whose canonical key and optional placement are published.
     * @param canonicalBlueprint Shared immutable authored Blueprint key owner.
     */
    void AppendSpriteRequirement(
        Snapshot& target,
        const Record& record,
        const std::shared_ptr<const std::string>& canonicalBlueprint)
    {
        if (!canonicalBlueprint || canonicalBlueprint->empty())
        {
            return;
        }

        const bool hasPlacement = record.TransformValid && record.SplineClassificationKnown && !record.HasSpline;
        Services::Map::Buildings::Visuals::SpritePlacement placement{};
        if (hasPlacement)
        {
            placement.Position = record.Position;
            placement.SignedScale = record.Scale;
            placement.Rotation = record.Rotation;
        }

        Services::Map::Buildings::BuildingSpriteSourceRecord descriptor{
            *canonicalBlueprint, hasPlacement, placement};
        constexpr std::string_view baseCoreKey = "/Game/Chimera/Buildings/BaseCore/BP_BaseCore.BP_BaseCore_C";
        auto& destination = *canonicalBlueprint == baseCoreKey ? target.OverlayLastSprites : target.OrdinarySprites;
        target.SpriteRequirementKeyOwners.push_back(canonicalBlueprint);
        destination.push_back(std::move(descriptor));
    }
    /** Appends one incomplete entry whose authored sprite source and policies are known.
     * Unknown Blueprint, spline, interior, or drone-rail classification omits that occurrence.
     * @param target Snapshot retaining the descriptor and its key storage.
     * @param entry Incomplete authoritative collector entry being considered.
     */
    void AppendKnownPendingSpriteRequirement(Snapshot& target, const Entry& entry)
    {
        if (entry.Complete || entry.Data.Generation != generation || !entry.Data.DefinitionValid ||
            !entry.Data.SplineClassificationKnown || entry.Data.HasSpline || !entry.DroneRailClassificationKnown ||
            (!target.IncludedDroneRails && entry.IsDroneRail) || !entry.Definition)
        {
            return;
        }

        const auto definition = definitions.find(entry.Definition);
        if (definition == definitions.end() || !definition->second->InteriorKnown || definition->second->Interior ||
            !definition->second->Blueprint || definition->second->Blueprint->empty())
        {
            return;
        }

        AppendSpriteRequirement(target, entry.Data, definition->second->Blueprint);
    }
    /** Sorts reusable subject requirements deterministically while preserving contributor multiplicity.
     * Equal descriptors remain repeated; this order enumerates requirements and does not certify draw order.
     * @param requirements Frozen descriptors for one sprite pass.
     */
    static void SortSpriteRequirements(
        std::vector<Services::Map::Buildings::BuildingSpriteSourceRecord>& requirements)
    {
        std::sort(requirements.begin(), requirements.end(), [](const auto& left, const auto& right)
        {
            if (left.SpriteRequirementKey != right.SpriteRequirementKey)
            {
                return left.SpriteRequirementKey < right.SpriteRequirementKey;
            }
            if (left.HasPlacement != right.HasPlacement)
            {
                return left.HasPlacement < right.HasPlacement;
            }
            return std::tie(left.Placement.Position, left.Placement.SignedScale, left.Placement.Rotation) <
                std::tie(right.Placement.Position, right.Placement.SignedScale, right.Placement.Rotation);
        });
    }
    /** Sorts already-selected sprite requirement spans within their separate deterministic passes.
     * The Snapshot owns both descriptor arrays and every canonical string referenced by their views.
     * @param target Snapshot being published.
     */
    void FreezeSpriteRequirements(Snapshot& target)
    {
        SortSpriteRequirements(target.OrdinarySprites);
        SortSpriteRequirements(target.OverlayLastSprites);
    }
    /** Converts every accepted copied native interpolation mode without changing its key interval.
     * The controls are already world-space XYZ Bezier values and receive no Record transform.
     * @param source Completed immutable copied curve from one Snapshot Record.
     * @param target Receives neutral segments in source order.
     * @return Whether every source segment has a supported native interpolation mode.
     */
    static bool ConvertCurveSegments(
        const SplineGeometry& source,
        std::vector<Services::Map::Buildings::Visuals::SplineCurveSegment>& target)
    {
        using Interpolation = Services::Map::Buildings::Visuals::SplineInterpolation;
        target.reserve(source.Segments.size());
        for (const CurveSegment& sourceSegment : source.Segments)
        {
            Services::Map::Buildings::Visuals::SplineCurveSegment segment{};
            segment.Controls = sourceSegment.Controls;
            segment.StartKey = sourceSegment.StartKey;
            segment.EndKey = sourceSegment.EndKey;
            switch (static_cast<SDK::EInterpCurveMode>(sourceSegment.Mode))
            {
            case SDK::EInterpCurveMode::CIM_Constant:
                segment.Interpolation = Interpolation::Constant;
                break;
            case SDK::EInterpCurveMode::CIM_Linear:
                segment.Interpolation = Interpolation::Linear;
                break;
            case SDK::EInterpCurveMode::CIM_CurveAuto:
            case SDK::EInterpCurveMode::CIM_CurveUser:
            case SDK::EInterpCurveMode::CIM_CurveBreak:
            case SDK::EInterpCurveMode::CIM_CurveAutoClamped:
                segment.Interpolation = Interpolation::Cubic;
                break;
            default:
                return false;
            }
            target.push_back(segment);
        }
        return !target.empty();
    }
    /** Converts a copied profile transform with the existing Unreal FRotator quaternion convention.
     * Signed scale and relative translation are preserved; the caller stores the result per surface.
     * @param source Immutable copied profile transform.
     * @param target Receives the neutral quaternion transform.
     * @return Whether the established converter accepts the source transform.
     */
    static bool ConvertProfileTransform(
        const MiniMapBuildingSplineProfiles::RelativeTransform& source,
        Services::Map::Buildings::Visuals::SplineSurfaceTransform& target)
    {
        MiniMapBuildingGeometryTypes::Transform converted{};
        if (!MiniMapBuildingSplineEnvelope::ComponentTransform(source, converted))
        {
            return false;
        }
        target.Translation = converted.Translation;
        target.Scale = converted.Scale;
        target.Rotation = converted.Rotation;
        return true;
    }
    /** Converts a positively supported copied mesh profile into one neutral alternative.
     * Existing eligibility rejects non-X forward axes, varying endpoint values and nonzero rolls.
     * @param profile Ordered immutable profile copied from the completed definition.
     * @param appearanceRgb Established game-specific representative surface color.
     * @param target Receives local bounds, fixed section values, appearance and component override.
     * @return Whether this profile is representable by the current neutral projector.
     */
    static bool ConvertProfile(
        const MiniMapBuildingSplineProfiles::Profile& profile,
        uint32_t appearanceRgb,
        Services::Map::Buildings::Visuals::BuildingSplineSurfaceRenderInput& target)
    {
        using namespace Services::Map::Buildings::Visuals;
        if (MiniMapBuildingSplineEnvelope::Eligibility(profile) != MiniMapBuildingSplineEnvelope::None)
        {
            return false;
        }
        target.Bounds.Origin = profile.LocalBounds.Origin;
        target.Bounds.Extent = profile.LocalBounds.Extent;
        target.CrossSection.Scale = profile.Section.StartScale;
        target.CrossSection.Offset = profile.Section.StartOffset;
        target.CrossSection.Up = profile.Section.Up;
        target.Appearance.Mode = SplineSurfaceAppearanceMode::SolidColor;
        target.Appearance.SolidRgb = appearanceRgb;
        // Legacy spline submission uses fixed alpha 255; neutral opacity is normalized.
        target.Appearance.Opacity = 1.0;
        SplineSurfaceTransform componentTransform{};
        if (!ConvertProfileTransform(profile.Transform, componentTransform))
        {
            return false;
        }
        target.ComponentTransformOverride = componentTransform;
        return true;
    }
    /** Builds an owned neutral adaptation for one included completed spline Record.
     * Drone rails use the established fixed 10-unit thin ribbon; ordinary sources retain every
     * ordered profile alternative. World-space controls are never transformed again.
     * @param snapshot Fixed Snapshot whose effective inclusion policy is authoritative.
     * @param record Completed Snapshot-owned contributor being adapted.
     * @param target Receives all owned curve and surface arrays for this contributor.
     * @return Whether all source facts required by this supported category were representable.
     */
    static bool BuildSplineAdaptation(
        const Snapshot& snapshot,
        const Record& record,
        OwnedSplineAdaptation& target)
    {
        using namespace Services::Map::Buildings::Visuals;
        const bool droneRail = MiniMapBuildingSplinePolicy::IsDroneRailDefinition(
            record.DefinitionValid, record.BuildingId);
        if (record.Generation != snapshot.Generation || !record.SplineClassificationKnown ||
            !record.HasSpline || !record.TransformValid ||
            !record.Curve || record.Curve->Issue != GeometryIssue::None ||
            (droneRail && !snapshot.IncludedDroneRails))
        {
            return false;
        }
        if (!ConvertCurveSegments(*record.Curve, target.Segments))
        {
            return false;
        }
        if (droneRail)
        {
            return BuildDroneRailSurface(record, target);
        }
        target.SelectionMode = BuildingSplineSurfaceSelectionMode::FirstNumericallyDrawableAlternative;
        return BuildProfileSurfaces(record, target);
    }
    /** Supplies the established projected thin ribbon for included drone-rail sources.
     * Its curve is already world-space, so identity transforms preserve the fixed-width policy.
     * @param record Completed drone-rail source and category appearance identity.
     * @param target Owned adaptation receiving the one numerical thin-ribbon surface.
     * @return Whether the category identity and copied geometry support this representation.
     */
    static bool BuildDroneRailSurface(const Record& record, OwnedSplineAdaptation& target)
    {
        using namespace Services::Map::Buildings::Visuals;
        if (!record.DefinitionValid)
        {
            return false;
        }
        BuildingSplineSurfaceRenderInput surface{};
        surface.Bounds.Origin = {0.0, 0.0, 0.0};
        surface.Bounds.Extent = {0.0, MiniMapBuildingSplineEnvelope::ThinHalfWidth, 0.0};
        surface.CrossSection.Scale = {1.0, 1.0};
        surface.CrossSection.Offset = {0.0, 0.0};
        surface.CrossSection.Up = {0.0, 0.0, 1.0};
        surface.Appearance.Mode = SplineSurfaceAppearanceMode::SolidColor;
        surface.Appearance.SolidRgb = MiniMapBuildingSplinePolicy::RepresentativeRgb(
            record.DefinitionValid, record.BuildingId);
        // Legacy spline submission uses fixed alpha 255; neutral opacity is normalized.
        surface.Appearance.Opacity = 1.0;
        target.Surfaces.push_back(surface);
        return true;
    }
    /** Copies every source-ordered normal profile alternative without preselection or sorting.
     * Each surface keeps its own relative component transform and established solid-color policy.
     * @param record Completed normal spline with immutable copied profile alternatives.
     * @param target Owned adaptation receiving all alternatives in their original order.
     * @return Whether the complete ordered profile set is representable by the neutral contract.
     */
    static bool BuildProfileSurfaces(const Record& record, OwnedSplineAdaptation& target)
    {
        if (!record.DefinitionValid || !record.DefinitionShape || !record.DefinitionShape->Profiles ||
            record.DefinitionShape->Profiles->Profiles.empty())
        {
            return false;
        }
        const uint32_t rgb = MiniMapBuildingSplinePolicy::RepresentativeRgb(
            record.DefinitionValid, record.BuildingId);
        const auto& profiles = record.DefinitionShape->Profiles->Profiles;
        target.Surfaces.reserve(profiles.size());
        for (const auto& profile : profiles)
        {
            Services::Map::Buildings::Visuals::BuildingSplineSurfaceRenderInput surface{};
            if (!ConvertProfile(profile, rgb, surface))
            {
                return false;
            }
            target.Surfaces.push_back(surface);
        }
        return !target.Surfaces.empty();
    }
    /** Adapts eligible completed spline records and records whether any included spline was unsupported.
     * Classification-unknown records remain unresolved; ordinary non-splines require no spline descriptor.
     * @param target Snapshot being assembled from its own deterministic Record order.
     * @return Whether every included completed spline was adapted or intentionally excluded.
     */
    static bool BuildSplineAdaptations(Snapshot& target)
    {
        target.OwnedSplineAdaptations.reserve(target.Records.size());
        bool complete = true;
        for (const Record& record : target.Records)
        {
            if (!record.SplineClassificationKnown)
            {
                complete = false;
                continue;
            }
            if (!record.HasSpline || record.InteriorOnly)
            {
                continue;
            }
            OwnedSplineAdaptation adaptation{};
            if (!BuildSplineAdaptation(target, record, adaptation))
            {
                complete = false;
                continue;
            }
            if (!MiniMapBuildingSplinePolicy::IsDroneRailDefinition(record.DefinitionValid, record.BuildingId))
            {
                adaptation.RootTransform.Translation = record.Position;
                adaptation.RootTransform.Scale = record.Scale;
                adaptation.RootTransform.Rotation = record.Rotation;
            }
            target.OwnedSplineAdaptations.push_back(std::move(adaptation));
        }
        FreezeSplineDescriptors(target);
        return complete;
    }
    /** Forms borrowed descriptors only after every nested vector has reached its final size.
     * The zero pixel-density marker leaves resolution-dependent policy selection to the future coordinator.
     * @param target Snapshot whose owned adaptation arrays back the descriptor spans.
     */
    static void FreezeSplineDescriptors(Snapshot& target)
    {
        using namespace Services::Map::Buildings::Visuals;
        target.Splines.clear();
        target.Splines.reserve(target.OwnedSplineAdaptations.size());
        for (const OwnedSplineAdaptation& owned : target.OwnedSplineAdaptations)
        {
            BuildingSplineRenderInput input{};
            input.Segments = owned.Segments;
            input.Surfaces = owned.Surfaces;
            input.RootTransform = owned.RootTransform;
            input.ComponentTransform = owned.ComponentTransform;
            input.SurfaceSelectionMode = owned.SelectionMode;
            input.SamplingPolicy.PixelsPerUnit = 0.0;
            target.Splines.push_back(input);
        }
    }
    bool Ensure(const Native& n)
    {
        auto* current=MiniMapMap::GetWorld();
        auto* subsystem=current?n.mass.getMassEntitySubsystem(current):nullptr;
        const void* currentManager=nullptr;
        if(subsystem)std::memcpy(&currentManager,reinterpret_cast<const uint8_t*>(subsystem)+0x38,sizeof(currentManager));
        if(world && (world!=current || manager!=currentManager))ClearSession();
        if(!currentManager || !types.Building)return false;
        if(queryConstructed)return true;
        world=current;manager=currentManager;
        n.mass.queryConstruct(query.data(),reinterpret_cast<const uint8_t*>(subsystem)+0x38);queryConstructed=true;
        n.mass.addTagRequirement(query.data(),types.Building,SDK::EMassFragmentPresence::All);
        n.mass.addTransformRequirement(query.data(),uint8_t(SDK::EMassFragmentAccess::ReadOnly),uint8_t(SDK::EMassFragmentPresence::Optional));
        return true;
    }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    struct ReconcileTiming
    {
        State& Owner;
        Clock::time_point Started=Clock::now();
        double NativeMs=0,BookkeepingMs=0,SweepMs=0,ReleaseMs=0;
        uint64_t Handles=0,Added=0,Removed=0,Duplicates=0,Invalid=0,Generation=0;
        bool Complete=false,Malformed=false;
        explicit ReconcileTiming(State& owner):Owner(owner){}
        ~ReconcileTiming() noexcept
        {
            const double total=std::chrono::duration<double,std::milli>(Clock::now()-Started).count();
            ++Owner.membershipScanSerial;
            Owner.measurements.Membership.Add(total);Owner.measurements.MembershipNative.Add(NativeMs);
            Owner.measurements.MembershipBookkeeping.Add(BookkeepingMs);Owner.measurements.MembershipSweep.Add(SweepMs);Owner.measurements.MembershipRelease.Add(ReleaseMs);
            Owner.measurements.NativeHandles+=Handles;Owner.measurements.MembershipAdded+=Added;Owner.measurements.MembershipRemoved+=Removed;
            Owner.measurements.MembershipDuplicates+=Duplicates;Owner.measurements.MembershipInvalid+=Invalid;
            Owner.measurements.AuthoritativeN=Owner.entities.size();Owner.measurements.PendingCount=Owner.pending.size();
            if(!Complete)++Owner.measurements.FailedScans;
            if(Malformed)++Owner.measurements.MalformedScans;
            if(total>=Owner.measurements.WorstScanMs)
            {
                Owner.measurements.WorstScanMs=total;Owner.measurements.WorstScanNativeMs=NativeMs;
                Owner.measurements.WorstScanBookkeepingMs=BookkeepingMs;Owner.measurements.WorstScanSweepMs=SweepMs;
                Owner.measurements.WorstScanReleaseMs=ReleaseMs;Owner.measurements.WorstScanHandles=Handles;
                Owner.measurements.WorstScanN=Owner.entities.size();Owner.measurements.WorstScanPending=Owner.pending.size();
                Owner.measurements.WorstScanAdded=Added;Owner.measurements.WorstScanRemoved=Removed;Owner.measurements.WorstScanGeneration=Generation;
                Owner.measurements.WorstScanBeginMonoMs=std::chrono::duration<double,std::milli>(Started.time_since_epoch()).count();
                Owner.measurements.WorstScanSerial=Owner.membershipScanSerial;
            }
            Owner.lastMembershipScanSerial=Owner.membershipScanSerial;
        }
    };
#endif
    struct MembershipArrayOwner
    {
        const Native& N;MiniMapNative::MassEntityHandleArray& H;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        double& ReleaseMs;
#endif
        ~MembershipArrayOwner() noexcept
        {
            if(H.Data)
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                const auto begin=Clock::now();
#endif
                N.texture.memoryFree(H.Data);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ReleaseMs=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
#endif
            }
        }
    };
    bool CurrentScanContext(const Native& n,uint64_t capturedGeneration,SDK::UWorld* capturedWorld,const void* capturedManager)
    {
        if(!queryConstructed || generation!=capturedGeneration || world!=capturedWorld || manager!=capturedManager ||
            MiniMapMap::GetWorld()!=capturedWorld)return false;
        auto* subsystem=capturedWorld?n.mass.getMassEntitySubsystem(capturedWorld):nullptr;
        const void* currentManager=nullptr;
        if(subsystem)std::memcpy(&currentManager,reinterpret_cast<const uint8_t*>(subsystem)+0x38,sizeof(currentManager));
        return currentManager==capturedManager;
    }
    bool Reconcile(const Native& n
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        ,uint64_t& changes
#endif
    )
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        ReconcileTiming timing{*this};timing.Generation=generation;
#endif
        const auto scanEpoch=NextMembershipScanEpoch();
        MiniMapNative::MassEntityHandleArray handles{};
        MembershipArrayOwner owner{n,handles
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            ,timing.ReleaseMs
#endif
        };
        const auto capturedGeneration=generation;auto* const capturedWorld=world;const void* const capturedManager=manager;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        const auto nativeBegin=Clock::now();
#endif
        MiniMapNative::MassEntityHandleArray* returned=nullptr;
        try {returned=n.mass.getMatchingEntityHandles(query.data(),&handles);}
        catch(...)
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            timing.NativeMs=std::chrono::duration<double,std::milli>(Clock::now()-nativeBegin).count();
#endif
            throw;
        }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        timing.NativeMs=std::chrono::duration<double,std::milli>(Clock::now()-nativeBegin).count();
#endif
        if(returned!=&handles)
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            timing.Malformed=true;
#endif
            return false;
        }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if(handles.Num>0)timing.Handles=uint64_t(handles.Num);
#endif
        if(handles.Num<0 || handles.Max<handles.Num || (handles.Num && !handles.Data))
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            timing.Malformed=true;
#endif
            return false;
        }
        if(!CurrentScanContext(n,capturedGeneration,capturedWorld,capturedManager))return false;

        // A fresh epoch is consumed even when traversal later fails. Partial marks
        // can therefore never certify a future complete scan.
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        const auto bookkeepingBegin=Clock::now();
#endif
        try
        {
            for(int i=0;i<handles.Num;++i)
            {
                const auto id=handles.Data[i];
                if(id.Index<0 || !id.SerialNumber)
                {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ++timing.Invalid;
#endif
                    continue;
                }
                const auto key=Key(id);
                auto [it,added]=entities.try_emplace(key);
                if(added)
                {
                    // Preserve the established rollback if pending-index insertion fails.
                    try {pending.insert(key);}
                    catch(...) {entities.erase(it);throw;}
                    auto& r=it->second.Data;r.Generation=capturedGeneration;r.Index=int32_t(uint32_t(key));r.Serial=int32_t(uint32_t(key>>32));
                    InvalidateSourcePublication();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ++timing.Added;++changes;
#endif
                }
                else if(it->second.SeenScan==scanEpoch)
                {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ++timing.Duplicates;
#endif
                }
                it->second.SeenScan=scanEpoch;
            }
        }
        catch(...)
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            timing.BookkeepingMs=std::chrono::duration<double,std::milli>(Clock::now()-bookkeepingBegin).count();
#endif
            throw;
        }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        timing.BookkeepingMs=std::chrono::duration<double,std::milli>(Clock::now()-bookkeepingBegin).count();
#endif
        // Query execution may reenter lifecycle code; only the owning world,
        // manager, query and generation may authorize the removal sweep.
        if(!CurrentScanContext(n,capturedGeneration,capturedWorld,capturedManager))return false;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        const auto sweepBegin=Clock::now();
#endif
        for(auto it=entities.begin();it!=entities.end();)
            if(it->second.SeenScan!=scanEpoch)
            {
                pending.erase(it->first);it=entities.erase(it);InvalidateSourcePublication();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ++timing.Removed;++changes;
#endif
            }
            else ++it;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        timing.SweepMs=std::chrono::duration<double,std::milli>(Clock::now()-sweepBegin).count();
        timing.Complete=true;
#endif
        return true;
    }
    struct Candidate
    {
        P::Profile Numbers;
        std::array<int64_t,12> Order{};
    };
    bool Mesh(SDK::UStaticMesh* mesh,P::Profile& profile)
    {
        if(!mesh)return false;const auto& b=mesh->ExtendedBounds;
        profile.LocalBounds={{b.Origin.X,b.Origin.Y,b.Origin.Z},{b.BoxExtent.X,b.BoxExtent.Y,b.BoxExtent.Z},b.SphereRadius};
        return P::Valid(profile.LocalBounds);
    }
    bool Profiles(SDK::UClass* type,int rank,std::vector<Candidate>& result)
    {
        if(!type || !type->ClassDefaultObject)return false;
        const auto* cdo=type->ClassDefaultObject;
        // The old selector's exact-class preference is preserved numerically.
        if(Is(cdo,types.Walkway))
        {
            Candidate p;p.Order={rank,0,type==types.Walkway?0:1,type->Index,0,0,0,0,0,cdo->Index,0,int64_t(result.size())};
            if(Mesh(static_cast<const SDK::ACrModularRampWalkway*>(cdo)->SplineMesh,p.Numbers))result.push_back(std::move(p));
        }
        for(auto* current=type;current;current=static_cast<SDK::UClass*>(current->SuperStruct))
        {
            if(!Is(current,types.Blueprint))continue;
            const auto& components=static_cast<SDK::UBlueprintGeneratedClass*>(current)->ComponentTemplates;
            for(auto* component:components)
            {
                if(!Is(component,types.SplineMesh))continue;
                const auto* spline=static_cast<const SDK::USplineMeshComponent*>(component);
                Candidate p;p.Order={rank,1,1,type->Index,0,current->Index,0,component->Index,0,cdo->Index,0,int64_t(result.size())};
                auto& v=p.Numbers;
                if(!Mesh(spline->StaticMesh,v))continue;
                v.Transform={{spline->RelativeLocation.X,spline->RelativeLocation.Y,spline->RelativeLocation.Z},
                    {spline->RelativeRotation.Pitch,spline->RelativeRotation.Yaw,spline->RelativeRotation.Roll},
                    {spline->RelativeScale3D.X,spline->RelativeScale3D.Y,spline->RelativeScale3D.Z}};
                const auto& params=spline->SplineParams;
                v.Section={{params.StartScale.X,params.StartScale.Y},{params.EndScale.X,params.EndScale.Y},
                    {params.StartOffset.X,params.StartOffset.Y},{params.EndOffset.X,params.EndOffset.Y},
                    params.StartRoll,params.EndRoll,{spline->SplineUpDir.X,spline->SplineUpDir.Y,spline->SplineUpDir.Z}};
                v.ForwardAxis=uint8_t(spline->ForwardAxis);result.push_back(std::move(p));
            }
        }
        return true;
    }
    /** Establishes definition policy and prepares existing immutable Blueprint/profile facts.
     * InteriorKnown is set only after the authored tag scan; Blueprint may be known while later
     * spline profile preparation remains unavailable. Definition loading can reenter lifecycle code.
     * @param n Native asset operations used by the existing preparation path.
     * @param d Collector-owned definition facts being prepared.
     * @param spline Whether the Mass source was positively classified as a spline.
     * @param building Authored BuildingID used by existing spline policy.
     * @return Whether all definition preparation required by this contributor completed.
     */
    bool PrepareDefinition(const Native& n,Definition& d,bool spline,uint32_t building)
    {
        if(d.Prepared)return true;
        auto* placement=d.Root;
        d.Interior=false;
        if(Is(placement,types.BuildingData))
            for(const auto tag:static_cast<const SDK::UCrBuildingData*>(placement)->Tags)
                if(tag.Number==0 && n.asset.nameEqualsWide(tag,L"Interior"))d.Interior=true;
        d.InteriorKnown=true;
        if(!d.Blueprint)d.Blueprint=MiniMapBuildingBlueprintIdentity::Copy(*placement);
        if(!d.Blueprint)return false;
        if(d.Interior){d.Prepared=true;return true;}
        if(!spline || MiniMapBuildingSplinePolicy::IsDroneRailDefinition(true,building))
        {d.Prepared=true;return true;}
        std::vector<Candidate> candidates;
        auto* config=placement->EntityType.EntityConfigPtr;
        if(!config)config=static_cast<SDK::UMassEntityConfigAsset*>(n.asset.loadSynchronous(&placement->EntityType.EntityConfig));
        if(!config && placement->EntityType.EntityConfig.ObjectID.AssetPath.PackageName.ComparisonIndex!=0)return false;
        std::unordered_set<const SDK::UMassEntityConfigAsset*> visited;
        for(;config;config=config->Config.Parent)
        {
            if(!visited.insert(config).second)return false;
            for(auto* trait:config->Config.Traits)
            {
                SDK::UClass *high=nullptr,*low=nullptr;
                if(Is(trait,types.Cosmetic))
                {const auto* v=static_cast<const SDK::UCrMassRepVisCosmeticsTrait*>(trait);high=v->HighResTemplateActor;low=v->LowResTemplateActor;}
                else if(Is(trait,types.Visual))
                {const auto* v=static_cast<const SDK::UMassVisualizationTrait*>(trait);high=v->HighResTemplateActor;low=v->LowResTemplateActor;}
                if(high && !Profiles(high,0,candidates))return false;
                if(low && !Profiles(low,1,candidates))return false;
            }
        }
        auto* authored=static_cast<SDK::UClass*>(n.asset.loadSynchronous(&placement->ActorClass));
        if(!authored || !Profiles(authored,2,candidates))return false;
        std::stable_sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.Order<b.Order;});
        auto profiles=std::make_shared<P::Set>();
        for(auto& candidate:candidates)profiles->Profiles.push_back(std::move(candidate.Numbers));
        if(profiles->Profiles.empty())return false;
        auto recipe=std::make_shared<DefinitionGeometry>();recipe->Profiles=std::move(profiles);d.Recipe=std::move(recipe);d.Prepared=true;return true;
    }
    /** Copies spline source facts and establishes whether the Mass spline fragment exists.
     * @param n Native access used for existing Mass fragment reads.
     * @param id Private Mass handle for the incomplete contributor.
     * @param record Owned copied record receiving classification and curve data.
     * @return Whether existing spline or fallback payload acquisition produced valid geometry.
     */
    bool CopySplineSource(const Native& n, Id id, Record& record)
    {
        const auto* mass = static_cast<const SDK::FAuSplineConnectionFragment*>(
            n.mass.getFragmentDataPtr(manager, id, types.Spline));
        record.HasSpline = mass != nullptr;
        record.SplineClassificationKnown = true;
        if (!record.HasSpline || record.Curve)
        {
            return true;
        }

        auto curve = std::make_shared<SplineGeometry>();
        G::CopySpline(*mass, *curve);
        if (curve->Issue == GeometryIssue::None && !curve->Segments.empty())
        {
            record.Curve = std::move(curve);
            record.Extent = record.Curve->CenterlineBounds;
            return true;
        }

        if (mass->bSplineCurvesInitialized || mass->Data.SplineData.Num() > 1)
        {
            return false;
        }

        const auto* ap = static_cast<const SDK::FAuAPMassFragment*>(
            n.mass.getFragmentDataPtr(manager, id, types.AP));
        if (!ap || MiniMapBuildingSplinePayload::Copy(ap->Payload, *curve) !=
            MiniMapBuildingSplinePayload::CopyResult::Complete)
        {
            return false;
        }

        record.Curve = std::move(curve);
        record.Extent = record.Curve->CenterlineBounds;
        return true;
    }
    /** Copies one contributor's existing definition and transform facts before spline source data.
     * It does not load assets or retain borrowed Mass fragments; spline kind becomes known only
     * after CopySplineSource performs the existing fragment lookup.
     * @param n Native Mass access for the existing acquisition path.
     * @param entry Authoritative incomplete collector entry being copied.
     * @param includeDroneRails Effective policy controlling early rail exclusion.
     * @return Whether currently required initial and spline data are copied successfully.
     */
    bool CopyInitial(const Native& n,Entry& entry,bool includeDroneRails)
    {
        auto& r=entry.Data;
        if(r.DefinitionValid)
            RecordDroneRailClassification(entry,MiniMapBuildingSplinePolicy::IsDroneRailDefinition(true,r.BuildingId));
        if(r.DefinitionValid && r.TransformValid && r.SplineClassificationKnown && (!r.HasSpline || r.Curve))return true;
        const Id id{r.Index,r.Serial};
        if(!r.DefinitionValid)
        {
            const auto* parameters=Parameters(n,id);auto* placement=parameters?parameters->PlacementData:nullptr;
            if(!placement)return false;
            // Exact validated definition admission precedes rooting, transform
            // and spline/payload copying. Leave the identity pending on exclusion.
            const bool isDroneRail=MiniMapBuildingSplinePolicy::IsDroneRailDefinition(true,uint32_t(placement->BuildingID));
            RecordDroneRailClassification(entry,isDroneRail);
            if(!includeDroneRails && isDroneRail)return false;
            auto it=definitions.find(placement);
            if(it==definitions.end())
            {
                auto definition=std::make_shared<Definition>();
                n.asset.retainObject(placement);definition->Root=placement;
                it=definitions.emplace(placement,std::move(definition)).first;
            }
            entry.Definition=placement;
            r.BuildingId=uint32_t(placement->BuildingID);r.DefinitionObjectIndex=placement->Index;
            r.DefinitionValid=true;
        }
        if(!r.TransformValid)
        {
            const auto* t=static_cast<const SDK::FTransform*>(n.mass.getFragmentDataPtr(manager,id,types.Transform));
            if(!t)return false;
            r.Position={t->Translation.X,t->Translation.Y,t->Translation.Z};r.Scale={t->Scale3D.X,t->Scale3D.Y,t->Scale3D.Z};
            r.Rotation={t->Rotation.X,t->Rotation.Y,t->Rotation.Z,t->Rotation.W};
            double norm=0;for(auto q:r.Rotation)norm+=q*q;
            if(!G::Finite(r.Position) || !G::Finite(r.Scale) || !std::isfinite(norm) || norm<=0)return false;
            for(auto& q:r.Rotation)q/=std::sqrt(norm);
            r.TransformValid=true;
        }
        return CopySplineSource(n,id,r);
    }
    Bounds SplineBounds(const Record& r)
    {
        if(!r.Curve)return {};
        double inflation=MiniMapBuildingSplineEnvelope::ThinHalfWidth;
        if(!MiniMapBuildingSplinePolicy::IsDroneRailDefinition(true,r.BuildingId))
        {
            if(!r.DefinitionShape || !r.DefinitionShape->Profiles)return {};
            inflation=0;
            for(const auto& p:r.DefinitionShape->Profiles->Profiles)
            {
                // Project uses unit orthogonal Y/Z frame vectors. The sum of
                // their maximum scalar magnitudes bounds every orientation;
                // signed TRS is bounded by its largest singular scale.
                const auto& b=p.LocalBounds;const auto& x=p.Section;
                const double y=(std::abs(b.Origin[1])+b.Extent[1])*std::abs(x.StartScale[0])+std::abs(x.StartOffset[0]);
                const double z=(std::abs(b.Origin[2])+b.Extent[2])*std::abs(x.StartScale[1])+std::abs(x.StartOffset[1]);
                double component=0,root=0;
                for(auto v:p.Transform.Scale)component=(std::max)(component,std::abs(v));
                for(auto v:r.Scale)root=(std::max)(root,std::abs(v));
                const auto& t=p.Transform.Translation;
                const double reach=root*(component*(y+z)+std::hypot(t[0],t[1],t[2]));
                if(!std::isfinite(reach))return {}; // Unknown bounds are always admitted.
                inflation=(std::max)(inflation,reach);
            }
        }
        return G::Inflate(r.Curve->CenterlineBounds,inflation+0.01);
    }
    void UpdateDrawingBounds()
    {
        const auto change=MiniMapBuildingVisuals::CollectorIntegration::ObserveDrawingBoundsVersion(drawingBoundsVersion);
        if(!change.Changed)return;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if(change.SessionChanged)++measurements.AtlasSessionChanges;
        if(change.RevisionChanged)++measurements.AtlasRevisionChanges;
#endif
        for(auto& [key,e]:entities)if(e.Complete && !e.Data.HasSpline)
        {
            MiniMapBuildingVisuals::CollectorIntegration::RefreshRecordBounds(e.Data);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            ++measurements.AtlasBoundsRecalculations;
#endif
        }
        InvalidateSourcePublication();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        ++measurements.DirtyAtlas;
#endif
    }
    /** Adds one byte to the deterministic FNV-1a source fingerprint. */
    static void HashFingerprintByte(uint64_t& hash,uint8_t value)
    {
        hash^=value;
        hash*=1099511628211ull;
    }
    /** Adds a 64-bit value in a platform-independent little-endian byte order. */
    static void HashFingerprintUint64(uint64_t& hash,uint64_t value)
    {
        for(unsigned int shift=0;shift<64;shift+=8)
            HashFingerprintByte(hash,static_cast<uint8_t>(value>>shift));
    }
    /** Adds an IEEE-754 value while representing either zero sign identically. */
    static void HashFingerprintDouble(uint64_t& hash,double value)
    {
        const double canonicalValue=value==0.0 ? 0.0 : value;
        HashFingerprintUint64(hash,std::bit_cast<uint64_t>(canonicalValue));
    }
    /** Hashes only the canonical blueprint name, world position and rotation. */
    static uint64_t CalculateSourceFingerprint(const Record& record)
    {
        static_assert(sizeof(double)==sizeof(uint64_t));
        static_assert(std::numeric_limits<double>::is_iec559);
        uint64_t hash=14695981039346656037ull;
        const std::string& blueprint=*record.CanonicalBlueprint;
        HashFingerprintUint64(hash,static_cast<uint64_t>(blueprint.size()));
        for(const unsigned char character:blueprint)
            HashFingerprintByte(hash,character);
        for(const double position:record.Position)
            HashFingerprintDouble(hash,position);
        auto rotation=record.Rotation;
        constexpr std::array<size_t,4> signOrder{3,0,1,2};
        bool negateRotation=false;
        for(const size_t componentIndex:signOrder)
        {
            if(rotation[componentIndex]==0.0)
                continue;
            negateRotation=rotation[componentIndex]<0.0;
            break;
        }
        if(negateRotation)
            for(double& component:rotation)
                component=-component;
        for(const double component:rotation)
            HashFingerprintDouble(hash,component);
        return hash;
    }
    /** Detects a one-time change to facts that can admit a pending sprite descriptor.
     * @param previous Record facts before CopyInitial ran.
     * @param current Record facts after CopyInitial ran.
     * @param previousDefinition Entry definition identity before copying.
     * @param entry Current authoritative incomplete entry.
     * @return Whether identity, sprite classification, or validated placement changed.
     */
    bool HasPendingSpriteFactsChanged(
        const Record& previous,
        const Record& current,
        const SDK::UAuActorPlacementData* previousDefinition,
        const Entry& entry) const
    {
        if (previousDefinition != entry.Definition || previous.DefinitionValid != current.DefinitionValid ||
            previous.SplineClassificationKnown != current.SplineClassificationKnown ||
            previous.HasSpline != current.HasSpline || previous.TransformValid != current.TransformValid)
        {
            return true;
        }

        return current.TransformValid &&
            (previous.Position != current.Position || previous.Rotation != current.Rotation || previous.Scale != current.Scale);
    }
    /** Revokes the active publication when definition-owned sprite facts change.
     * @param definition Shared prepared facts used by the incomplete entry.
     * @param previousBlueprint Canonical key owner before preparation.
     * @param previousInteriorKnown Whether interior policy was established before preparation.
     * @param previousInterior Interior-only value before preparation.
     */
    void InvalidateChangedDefinitionFacts(
        const Definition& definition,
        const std::shared_ptr<const std::string>& previousBlueprint,
        bool previousInteriorKnown,
        bool previousInterior)
    {
        if (previousBlueprint != definition.Blueprint || previousInteriorKnown != definition.InteriorKnown ||
            (definition.InteriorKnown && previousInterior != definition.Interior))
        {
            InvalidateSourcePublication();
        }
    }
    /** Copies currently available Mass facts and revokes only on a real fact transition.
     * @param native Serialized native access used by CopyInitial.
     * @param entry Authoritative incomplete entry being advanced.
     * @param includeDroneRails Effective Snapshot rail policy.
     * @return Whether transform and source classification facts are ready for definition work.
     */
    bool CopyEntryFacts(const Native& native, Entry& entry, bool includeDroneRails)
    {
        const Record previousData = entry.Data;
        const auto* previousDefinition = entry.Definition;
        const bool ready = CopyInitial(native, entry, includeDroneRails);
        if (HasPendingSpriteFactsChanged(previousData, entry.Data, previousDefinition, entry))
        {
            InvalidateSourcePublication();
        }
        return ready;
    }
    /** Advances one existing pending entry through the established acquisition sequence.
     * Definition loading may reenter lifecycle code, so the entry is looked up again before commit.
     * @param n Native services used by existing Mass and definition acquisition.
     * @param key Collector-private key for the incomplete authoritative entry.
     * @param attemptedDefinitions Definitions already attempted during this callback.
     * @param includeDroneRails Effective policy captured for the next Snapshot.
     * @return Whether the entry completed and was removed from pending acquisition.
     */
    bool Acquire(const Native& n,uint64_t key,std::unordered_set<const Definition*>& attemptedDefinitions,bool includeDroneRails)
    {
        auto it=entities.find(key);if(it==entities.end() || it->second.Complete)return false;
        auto& e=it->second;
        if(!includeDroneRails && MiniMapBuildingSplinePolicy::IsDroneRailDefinition(e.Data.DefinitionValid,e.Data.BuildingId))return false;
        if(!n.mass.isEntityValid(manager,{e.Data.Index,e.Data.Serial}) || !n.mass.isEntityBuilt(manager,{e.Data.Index,e.Data.Serial}))return false;
        if (!CopyEntryFacts(n, e, includeDroneRails))
        {
            return false;
        }
        // CopyInitial has returned: no Mass borrow spans definition loading.
        // Loading may dispatch engine callbacks: keep the definition strongly
        // owned, and re-find the entity after returning instead of holding a
        // map entry/reference across that call.
        const auto definition=definitions.at(e.Definition);
        auto result=e.Data;const auto epoch=generation;
        const auto previousBlueprint = definition->Blueprint;
        const bool previousInteriorKnown = definition->InteriorKnown;
        const bool previousInterior = definition->Interior;
        const bool definitionAttempted=!definition->Prepared;
        if(definitionAttempted && !attemptedDefinitions.insert(definition.get()).second)return false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if(definitionAttempted)++measurements.DefinitionAttempts;
#endif
        const bool definitionReady=PrepareDefinition(n,*definition,result.HasSpline,result.BuildingId);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if(definitionAttempted){if(definitionReady)++measurements.DefinitionSuccesses;else ++measurements.DefinitionFailures;}
#endif
        if(epoch!=generation || world!=MiniMapMap::GetWorld())return false;
        InvalidateChangedDefinitionFacts(*definition, previousBlueprint, previousInteriorKnown, previousInterior);
        if(!definitionReady)return false;
        it=entities.find(key);
        if(it==entities.end() || it->second.Complete ||
            !n.mass.isEntityValid(manager,{result.Index,result.Serial}) || !n.mass.isEntityBuilt(manager,{result.Index,result.Serial}))return false;
        result.CanonicalBlueprint=definition->Blueprint;result.DefinitionShape=definition->Recipe;
        result.InteriorKnown=true;result.InteriorOnly=definition->Interior;
        if(result.HasSpline)result.Extent=SplineBounds(result);
        else MiniMapBuildingVisuals::CollectorIntegration::RefreshRecordBounds(result);
        result.SourceFingerprint=CalculateSourceFingerprint(result);
        // Owned Record move and key erasure do not allocate or call native
        // code: completion and removal from scheduling commit together.
        it->second.Data=std::move(result);it->second.Complete=true;it->second.Definition=nullptr;pending.erase(key);InvalidateSourcePublication();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        ++measurements.DirtyAcquisition;
#endif
        return true;
    }
    /** Validates copied support bounds before they are used for certification. */
    static bool HasValidSupportExtent(const Bounds& extent) noexcept
    {
        return BuildingSourcePublication::HasValidSupportExtent(extent);
    }
    /** Publishes current immutable records and known sprite requirements when source facts are dirty.
     * Pending descriptors are frozen during the existing Entry traversal and do not affect completion.
     * @param includeDroneRails Effective policy captured for this Snapshot publication.
     */
    void Publish(bool includeDroneRails)
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        ++measurements.PublicationAttempts;
#endif
        if(!dirty)return;
        auto value=std::make_shared<Snapshot>();value->Generation=generation;value->Complete=membershipComplete;value->Region=region;
        value->IncludedDroneRails=includeDroneRails;
        bool acquisitionComplete=membershipComplete;
        for(const auto& [key,e]:entities)
        {
            const bool excludedDroneRail=!includeDroneRails && e.DroneRailClassificationKnown && e.IsDroneRail;
            if(!e.Complete)
            {
                if(!excludedDroneRail)acquisitionComplete=false;
                AppendKnownPendingSpriteRequirement(*value, e);
                continue;
            }
            if(e.Data.InteriorOnly || excludedDroneRail)continue;
            if(!HasValidSupportExtent(e.Data.Extent))acquisitionComplete=false;
            if(!region.Valid || !e.Data.Extent.Valid || G::Intersects(e.Data.Extent,region))
            {
                value->Records.push_back(e.Data);
                if (e.Data.SplineClassificationKnown && !e.Data.HasSpline && e.Data.InteriorKnown && !e.Data.InteriorOnly)
                {
                    AppendSpriteRequirement(*value, e.Data, e.Data.CanonicalBlueprint);
                }
            }
        }
        value->AcquisitionComplete=acquisitionComplete;
        std::sort(value->Records.begin(),value->Records.end(),[this](const auto& a,const auto& b){return Key({a.Index,a.Serial})<Key({b.Index,b.Serial});});
        FreezeSpriteRequirements(*value);
        value->SplineAdaptationComplete = BuildSplineAdaptations(*value);
        auto eligibility=std::make_shared<BuildingSourcePublication::EligibilityToken>();
        if(publicationEligibility)
        {
            publicationEligibility->IsEligible=false;
        }
        {
            std::scoped_lock lock(publicationMutex);
            snapshot=std::move(value);
            publicationEligibility=std::move(eligibility);
        }
        dirty=false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        ++measurements.Publications;
#endif
    }
    /** Obtains one immutable publication only while it belongs to this session. */
    std::shared_ptr<const Snapshot> GetCurrentSnapshot()
    {
        std::scoped_lock lock(publicationMutex);
        if(!snapshot || snapshot->Generation!=generation)return {};
        return snapshot;
    }
    /** Observes the requested section grid from one immutable publication. */
    void ObserveSections(
        const Services::Map::Buildings::BuildingSectionGrid& grid,
        std::span<Services::Map::Buildings::BuildingSectionObservation> observations)
    {
        const auto selectedSnapshot=GetCurrentSnapshot();
        if(!selectedSnapshot)
        {
            std::fill(observations.begin(),observations.end(),
                Services::Map::Buildings::BuildingSectionObservation{});
            return;
        }

        BuildingSourcePublication::ObserveSnapshot(*selectedSnapshot, grid, observations);
    }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    void MaybeReportMeasurements(Clock::time_point callbackBegin,uint64_t scanSerialAtEntry) noexcept
    {
        const auto now=Clock::now();
        const double callbackMs=std::chrono::duration<double,std::milli>(now-callbackBegin).count();
        if(lastMembershipScanSerial!=scanSerialAtEntry && measurements.WorstScanSerial==lastMembershipScanSerial)
            measurements.WorstScanCallbackMs=callbackMs;
        if(measurementGeneration!=generation){measurements.Reset();measurementGeneration=generation;measurementIntervalStart=now;return;}
        // Observe actual counts even on local-first exits, quiet callbacks,
        // and allocation failures; no authoritative-map enumeration required.
        measurements.PendingN=entities.size();measurements.PendingP=pending.size();
        if(measurementIntervalStart==Clock::time_point{}){measurementIntervalStart=now;return;}
        const double intervalMs=std::chrono::duration<double,std::milli>(now-measurementIntervalStart).count();
        if(intervalMs<double(std::chrono::duration_cast<std::chrono::milliseconds>(kMeasurementInterval).count()))return;
        const auto m=measurements;measurements.Reset();measurementIntervalStart=now;
        LOG_INFO("MiniMap: Stage42J collector generation=%llu intervalMs=%.1f membership={samples:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f polls:%llu changes:%llu} grid={samples:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f visitors:%llu uniqueKeys:%llu} pending={samples:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f lastN:%llu lastP:%llu zeroCallbacks:%llu remoteSkipped:%llu} acquisition={samples:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f localAttempts:%llu remoteAttempts:%llu completed:%llu definitionAttempts:%llu definitionSuccesses:%llu definitionFailures:%llu} maintenance={samples:%llu totalMs:%.3f meanMs:%.3f maxMs:%.3f atlasSessions:%llu atlasRevisions:%llu boundsRecalcs:%llu publishAttempts:%llu publications:%llu dirtyMembership:%llu dirtyRegion:%llu dirtyAtlas:%llu dirtyAcquisition:%llu}",
            static_cast<unsigned long long>(generation),intervalMs,
            (unsigned long long)m.Membership.Samples,m.Membership.TotalMs,m.Membership.MeanMs(),m.Membership.MaxMs,(unsigned long long)m.MembershipPolls,(unsigned long long)m.MembershipChanges,
            (unsigned long long)m.LocalGrid.Samples,m.LocalGrid.TotalMs,m.LocalGrid.MeanMs(),m.LocalGrid.MaxMs,(unsigned long long)m.GridVisitors,(unsigned long long)m.UniqueLocalKeys,
            (unsigned long long)m.PendingEnumeration.Samples,m.PendingEnumeration.TotalMs,m.PendingEnumeration.MeanMs(),m.PendingEnumeration.MaxMs,(unsigned long long)m.PendingN,(unsigned long long)m.PendingP,(unsigned long long)m.PendingZeroCallbacks,(unsigned long long)m.RemotePassesSkipped,
            (unsigned long long)m.Acquisition.Samples,m.Acquisition.TotalMs,m.Acquisition.MeanMs(),m.Acquisition.MaxMs,(unsigned long long)m.LocalAttempts,(unsigned long long)m.RemoteAttempts,(unsigned long long)m.NewlyCompleted,(unsigned long long)m.DefinitionAttempts,(unsigned long long)m.DefinitionSuccesses,(unsigned long long)m.DefinitionFailures,
            (unsigned long long)m.Maintenance.Samples,m.Maintenance.TotalMs,m.Maintenance.MeanMs(),m.Maintenance.MaxMs,(unsigned long long)m.AtlasSessionChanges,(unsigned long long)m.AtlasRevisionChanges,(unsigned long long)m.AtlasBoundsRecalculations,(unsigned long long)m.PublicationAttempts,(unsigned long long)m.Publications,(unsigned long long)m.DirtyMembership,(unsigned long long)m.DirtyRegion,(unsigned long long)m.DirtyAtlas,(unsigned long long)m.DirtyAcquisition);
        LOG_INFO("MiniMap: Stage42P generation=%llu intervalMs=%.1f native={n:%llu t:%.3f avg:%.3f max:%.3f} book={n:%llu t:%.3f avg:%.3f max:%.3f} sweep={n:%llu t:%.3f avg:%.3f max:%.3f} free={n:%llu t:%.3f avg:%.3f max:%.3f} total={n:%llu t:%.3f avg:%.3f max:%.3f} H:%llu N:%llu P:%llu added:%llu removed:%llu dup:%llu invalid:%llu failed:%llu malformed:%llu worst={ms:%.3f native:%.3f book:%.3f sweep:%.3f free:%.3f monoMs:%.3f tickMs:%.3f gen:%llu H:%llu N:%llu P:%llu add:%llu rem:%llu}",
            (unsigned long long)generation,intervalMs,
            (unsigned long long)m.MembershipNative.Samples,m.MembershipNative.TotalMs,m.MembershipNative.MeanMs(),m.MembershipNative.MaxMs,
            (unsigned long long)m.MembershipBookkeeping.Samples,m.MembershipBookkeeping.TotalMs,m.MembershipBookkeeping.MeanMs(),m.MembershipBookkeeping.MaxMs,
            (unsigned long long)m.MembershipSweep.Samples,m.MembershipSweep.TotalMs,m.MembershipSweep.MeanMs(),m.MembershipSweep.MaxMs,
            (unsigned long long)m.MembershipRelease.Samples,m.MembershipRelease.TotalMs,m.MembershipRelease.MeanMs(),m.MembershipRelease.MaxMs,
            (unsigned long long)m.Membership.Samples,m.Membership.TotalMs,m.Membership.MeanMs(),m.Membership.MaxMs,
            (unsigned long long)m.NativeHandles,(unsigned long long)m.AuthoritativeN,(unsigned long long)m.PendingCount,
            (unsigned long long)m.MembershipAdded,(unsigned long long)m.MembershipRemoved,(unsigned long long)m.MembershipDuplicates,(unsigned long long)m.MembershipInvalid,
            (unsigned long long)m.FailedScans,(unsigned long long)m.MalformedScans,
            m.WorstScanMs,m.WorstScanNativeMs,m.WorstScanBookkeepingMs,m.WorstScanSweepMs,m.WorstScanReleaseMs,
            m.WorstScanBeginMonoMs,m.WorstScanCallbackMs,(unsigned long long)m.WorstScanGeneration,
            (unsigned long long)m.WorstScanHandles,(unsigned long long)m.WorstScanN,(unsigned long long)m.WorstScanPending,
            (unsigned long long)m.WorstScanAdded,(unsigned long long)m.WorstScanRemoved);
    }
    struct MeasurementReporter
    {
        State& Owner;
        Clock::time_point Started;uint64_t ScanSerial;
        MeasurementReporter(State& owner,Clock::time_point started):Owner(owner),Started(started),ScanSerial(owner.membershipScanSerial){}
        ~MeasurementReporter(){Owner.MaybeReportMeasurements(Started,ScanSerial);}
    };
#endif
    struct GridVisitContext
    {
        State* Owner=nullptr;
        std::unordered_set<uint64_t>* Keys=nullptr;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        uint64_t Visitors=0;
#endif
    };
    static bool Near(void* context,const MiniMapNative::GridEntityHandle& handle)
    {
        auto& visit=*static_cast<GridVisitContext*>(context);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        ++visit.Visitors;
#endif
        const auto key=visit.Owner->Key(handle.Entity);
        if(visit.Owner->pending.contains(key))visit.Keys->insert(key);
        return true;
    }
    void OnTick(float)
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        const auto callbackBegin=Clock::now();
#endif
        if(!ready)return;
        const bool includeDroneRails=renderDroneRails;
        const auto* n=MiniMapNative::Get();if(!n || !n->player.isGameThread() || !Ensure(*n))return;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if(measurementGeneration!=generation){measurements.Reset();measurementGeneration=generation;measurementIntervalStart=Clock::now();}
        MeasurementReporter reportAtExit(*this,callbackBegin);
#endif
        try
        {
            if(Clock::now()>=nextMembership)
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ++measurements.MembershipPolls;uint64_t changes=0;
#endif
                const bool complete=Reconcile(*n
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ,changes
#endif
                );
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                measurements.MembershipChanges+=changes;measurements.DirtyMembership+=changes;
#endif
                if(complete!=membershipComplete)
                {
                    membershipComplete=complete;InvalidateSourcePublication();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ++measurements.DirtyMembership;
#endif
                }
                nextMembership=Clock::now()+std::chrono::seconds(1);
            }
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                const auto begin=Clock::now();
#endif
                UpdateDrawingBounds();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                measurements.Maintenance.Add(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
#endif
            }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            const auto gridBegin=Clock::now();
#endif
            double radius=0,centerX=0,centerY=0,centerZ=0;
            bool hasLocalCenter=false;
            if(discoveryInput==DiscoveryInput::ExplicitRegion)
            {
                if(region.Valid)
                {
                    centerX=region.MinX*0.5+region.MaxX*0.5;
                    centerY=region.MinY*0.5+region.MaxY*0.5;
                    radius=std::hypot(region.MaxX-region.MinX,region.MaxY-region.MinY)*0.5;
                    hasLocalCenter=radius>0 && std::isfinite(radius) && std::isfinite(centerX) && std::isfinite(centerY);
                }
            }
            else
            {
                MiniMapMap::PresentationSample sample{};
                {std::scoped_lock lock(publicationMutex);radius=viewRadius;}
                hasLocalCenter=radius>0 && MiniMapMap::TryGetPresentationSample(sample);
                if(hasLocalCenter)
                {
                    centerX=sample.Tracking.WorldX;centerY=sample.Tracking.WorldY;centerZ=sample.Tracking.WorldZ;
                    const Bounds requested{centerX-radius,centerY-radius,centerX+radius,centerY+radius,true};
                    if(!regionOverride && (!region.Valid || requested.MinX<region.MinX || requested.MinY<region.MinY || requested.MaxX>region.MaxX || requested.MaxY>region.MaxY))
                    {
                        region=G::Inflate(requested,radius*0.5);InvalidateSourcePublication();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                        ++measurements.DirtyRegion;
#endif
                    }
                }
            }
            if(pending.empty())
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                ++measurements.PendingZeroCallbacks;
#endif
                remoteTurn=false;
            }
            else
            {
                std::unordered_set<uint64_t> localCandidates;
                GridVisitContext gridVisit{this,&localCandidates};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                bool gridEnumerated=false;
#endif
                if(hasLocalCenter && n->gridDiagnostic.forEachCellInRadius)
                    if(auto* grid=n->gridDiagnostic.getSubsystem(world))
                    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                        gridEnumerated=true;
#endif
                        const MiniMapNative::GridDiagnosticApi::VisitorRef visitor{&State::Near,&gridVisit};
                        const SDK::FVector center{centerX,centerY,centerZ};
                        n->gridDiagnostic.forEachCellInRadius(grid,center,std::nextafter(float(radius),INFINITY),visitor,false);
                    }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                if(gridEnumerated)measurements.LocalGrid.Add(std::chrono::duration<double,std::milli>(Clock::now()-gridBegin).count());
                measurements.GridVisitors+=gridVisit.Visitors;measurements.UniqueLocalKeys+=localCandidates.size();
#endif
                std::unordered_set<const Definition*> attemptedDefinitions;
                const auto localEpoch=generation;
                if(!localCandidates.empty())
                {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    const auto begin=Clock::now();
#endif
                    for(auto key:localCandidates)
                    {
                        if(!pending.contains(key))continue;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                        ++measurements.LocalAttempts;
#endif
                        if(Acquire(*n,key,attemptedDefinitions,includeDroneRails))
                        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                            ++measurements.NewlyCompleted;
#endif
                        }
                        if(localEpoch!=generation)return;
                    }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    measurements.Acquisition.Add(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
#endif
                }
                // Publish local data before remote definition preparation. Remote
                // work is serviced on the next callback after any local progress;
                // there is no entity quota or permanent spatial admission limit.
                if(dirty && !remoteTurn)
                {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    ++measurements.RemotePassesSkipped;const auto begin=Clock::now();
#endif
                    Publish(includeDroneRails);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    measurements.Maintenance.Add(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
#endif
                    remoteTurn=true;return;
                }
                remoteTurn=false;
                if(!pending.empty())
                {
                    // Acquire may erase pending keys or reenter session lifecycle
                    // code. Keep only a P-sized key snapshot across native calls.
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    const auto pendingBegin=Clock::now();
#endif
                    const std::vector<uint64_t> pendingKeys(pending.begin(),pending.end());
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    measurements.PendingEnumeration.Add(std::chrono::duration<double,std::milli>(Clock::now()-pendingBegin).count());
#endif
                    const auto epoch=generation;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    const auto begin=Clock::now();
#endif
                    for(auto key:pendingKeys)
                    {
                        if(!pending.contains(key))continue;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                        ++measurements.RemoteAttempts;
#endif
                        if(Acquire(*n,key,attemptedDefinitions,includeDroneRails))
                        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                            ++measurements.NewlyCompleted;
#endif
                        }
                        if(epoch!=generation)return;
                    }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    measurements.Acquisition.Add(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
#endif
                }
            }
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                const auto begin=Clock::now();
#endif
                Publish(includeDroneRails);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                measurements.Maintenance.Add(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
#endif
            }
        }
        catch(const std::bad_alloc&){/* Keep membership/owned records; retry pending work. */}
    }
    };

    BuildingCollector::BuildingCollector():state_(std::make_unique<State>()){}
    BuildingCollector::~BuildingCollector()=default;
    BuildingCollector::BuildingCollector(BuildingCollector&&) noexcept=default;
    BuildingCollector& BuildingCollector::operator=(BuildingCollector&&) noexcept=default;
    bool BuildingCollector::InitializeResources(){return state_->InitializeMetadata();}
    void BuildingCollector::Reset(){state_->ready=false;state_->ClearSession();std::scoped_lock lock(state_->publicationMutex);state_->viewRadius=0;}
    void BuildingCollector::OnExperienceLoadComplete()
    {
        state_->ready=state_->InitializeMetadata();
        if(!state_->ready)LOG_ERROR("MiniMap: building acquisition initialization metadata unavailable");
    }
    bool BuildingCollector::BeginExplicitSession()
    {
        const auto* n=MiniMapNative::Get();
        if(!MiniMapMap::GameplayReady() || !MiniMapMap::GetWorld() || !n || !n->player.isGameThread())return false;
        state_->ready=state_->InitializeMetadata();
        return state_->ready;
    }
    void BuildingCollector::SetViewport(const MiniMapMap::Transform& t)
    {
        const double radius=t.Valid && t.PixelsPerWorldUnit>0?std::hypot(t.Width,t.Height)*0.5/t.PixelsPerWorldUnit:0;
        std::scoped_lock lock(state_->publicationMutex);state_->viewRadius=std::isfinite(radius)?radius:0;
    }
    void BuildingCollector::SetRegion(const Bounds& region)
    {
        const bool inputChanged=state_->discoveryInput!=State::DiscoveryInput::ExplicitRegion;
        state_->discoveryInput=State::DiscoveryInput::ExplicitRegion;
        const auto& current=state_->region;
        const bool boundsChanged=current.MinX!=region.MinX || current.MinY!=region.MinY ||
            current.MaxX!=region.MaxX || current.MaxY!=region.MaxY || current.Valid!=region.Valid;
        if(boundsChanged)
        {
            state_->region=region;
        }
        state_->regionOverride=true;
        if(boundsChanged || inputChanged)
        {
            state_->InvalidateSourcePublication();
        }
    }
    /** Validates the map rectangle before forwarding it to the existing region setter. */
    bool BuildingCollector::SetAcquisitionRegion(
        const Services::Map::Buildings::BuildingCollectionRegion& requestedRegion) noexcept
    {
        if(!std::isfinite(requestedRegion.xMapUnits) || !std::isfinite(requestedRegion.yMapUnits) ||
            !std::isfinite(requestedRegion.widthMapUnits) || !std::isfinite(requestedRegion.heightMapUnits) ||
            requestedRegion.widthMapUnits<=0 || requestedRegion.heightMapUnits<=0)return false;
        const double right=requestedRegion.xMapUnits+requestedRegion.widthMapUnits;
        const double bottom=requestedRegion.yMapUnits+requestedRegion.heightMapUnits;
        if(!std::isfinite(right) || !std::isfinite(bottom) ||
            right<=requestedRegion.xMapUnits || bottom<=requestedRegion.yMapUnits)return false;
        const double width=right-requestedRegion.xMapUnits;
        const double height=bottom-requestedRegion.yMapUnits;
        if(!std::isfinite(width) || !std::isfinite(height) ||
            !std::isfinite(std::hypot(width,height)))return false;
        SetRegion(Bounds{requestedRegion.xMapUnits,requestedRegion.yMapUnits,right,bottom,true});
        return true;
    }
    /** Delegates batch observation to the collector's immutable snapshot state. */
    void BuildingCollector::ObserveSections(
        const Services::Map::Buildings::BuildingSectionGrid& grid,
        std::span<Services::Map::Buildings::BuildingSectionObservation> observations) const
    {
        state_->ObserveSections(grid,observations);
    }
    std::shared_ptr<const Services::Map::Buildings::IBuildingSourcePublication>
        BuildingCollector::AcquireSourcePublication() const
    {
        std::scoped_lock lock(state_->publicationMutex);
        if(!state_->snapshot)
        {
            return {};
        }

        return std::make_shared<BuildingSourcePublication>(
            state_->snapshot, state_->publicationEligibility);
    }
    void BuildingCollector::SetRenderDroneRails(bool enabled)
    {
        if(state_->renderDroneRails==enabled)
        {
            return;
        }

        state_->renderDroneRails=enabled;
        state_->InvalidateSourcePublication();
    }
    std::shared_ptr<const Snapshot> BuildingCollector::GetSnapshot(){std::scoped_lock lock(state_->publicationMutex);return state_->snapshot;}
    CollectorDiagnostics BuildingCollector::GetDiagnostics() const
    {
        CollectorDiagnostics result{};
        result.Ready=state_->ready;
        result.MembershipComplete=state_->membershipComplete;
        result.AuthoritativeBuildingCount=state_->entities.size();
        result.PendingAcquisitionCount=state_->pending.size();
        result.CompletedBuildingCount=result.AuthoritativeBuildingCount>=result.PendingAcquisitionCount ?
            result.AuthoritativeBuildingCount-result.PendingAcquisitionCount : 0;
        std::scoped_lock lock(state_->publicationMutex);
        if(state_->snapshot)
        {
            result.PublicationGeneration=state_->snapshot->Generation;
            result.PublishedInRegionCount=state_->snapshot->Records.size();
            result.MembershipComplete=state_->snapshot->Complete;
        }
        return result;
    }
    void BuildingCollector::OnTick(float delta){state_->OnTick(delta);}
    void BuildingCollector::ShutdownResources()
    {
        Reset();
        ShutdownMetadataResources();
    }
    void BuildingCollector::ShutdownMetadataResources()
    {
        state_->InvalidateSourcePublication();
        if(const auto* n=MiniMapNative::Get())for(auto* root:state_->metadata)n->asset.releaseObject(root);
        state_->metadata.clear();state_->types={};
    }

    namespace
    {
        BuildingCollector& MiniMapInstance(){static BuildingCollector instance;return instance;}
    }

    bool Initialize(IPluginSelf* self)
    {
        if(g_initialized)return true;
        if(!self || !self->hooks || !MiniMap::Services::TryRuntimeEvents())return false;
        auto& collector=MiniMapInstance();collector.Reset();collector.InitializeResources();
        const auto tickReceipt = MiniMap::Services::TryRuntimeEvents()->OnTick(
            [](void*, float delta) { MiniMapBuildingCollector::OnTick(delta); }, nullptr);
        if (tickReceipt.token.id) g_tickRegistration = tickReceipt.token;
        if (tickReceipt.acceptance == MiniMap::Services::Acceptance::Rejected ||
            tickReceipt.acceptance == MiniMap::Services::Acceptance::BackendUnknown)
        {
            // Preserve the existing registration rejection teardown order.
            g_initialized = true;
            Shutdown();
            return false;
        }
        g_initialized=true;return true;
    }
    void Reset(){MiniMapInstance().Reset();}
    void OnExperienceLoadComplete(){if(g_initialized)MiniMapInstance().OnExperienceLoadComplete();}
    void SetViewport(const MiniMapMap::Transform& t){MiniMapInstance().SetViewport(t);}
    std::shared_ptr<const Snapshot> GetSnapshot(){return MiniMapInstance().GetSnapshot();}
    void OnTick(float delta)
    {
        if(!MiniMapUI::IsVisible())return;
        if(!MiniMapMap::GameplayReady())return;
        const auto settings=MiniMapConfig::GetRenderSettings().Buildings;
        if(!settings.ShowLayer)return;
        auto& collector=MiniMapInstance();collector.SetRenderDroneRails(settings.RenderDroneRails);collector.OnTick(delta);
    }
    void Shutdown()
    {
        if(!g_initialized)return;
        if (auto* events = MiniMap::Services::TryRuntimeEvents())
        {
            const auto removal = events->Remove(g_tickRegistration);
            if (removal.registry == MiniMap::Services::RegistryRemoval::RequestReturned &&
                removal.core == MiniMap::Services::CoreDrain::Quiescent) g_tickRegistration = {};
        }
        MiniMapInstance().Reset();
        MiniMapBuildingBlueprintIdentity::Shutdown();
        MiniMapInstance().ShutdownMetadataResources();
        g_initialized=false;
    }
}
#endif
