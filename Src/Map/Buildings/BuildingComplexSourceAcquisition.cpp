#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingComplexSourceAcquisition.h"
#include "BuildingBlueprintIdentity.h"
#include "BuildingCollector.h"
#include "BuildingGeometry.h"
#include "BuildingSplinePayload.h"
#include "BuildingSplinePolicy.h"
#include "Native/NativeApi.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
#include <SDK/AuActorPlacement_classes.hpp>
#include <SDK/Chimera_classes.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <vector>

namespace
{
    namespace B = MiniMapBuildingComplexIndex;
    namespace G = MiniMapBuildingGeometry;
    using Id = MiniMapNative::MassEntityHandle;

    struct Metadata
    {
        SDK::UScriptStruct* Parameters = nullptr;
        SDK::UScriptStruct* Transform = nullptr;
        SDK::UScriptStruct* Spline = nullptr;
        SDK::UScriptStruct* AP = nullptr;
        SDK::UClass* Visual = nullptr;
        SDK::UClass* Cosmetic = nullptr;
        SDK::UClass* Walkway = nullptr;
        SDK::UClass* Blueprint = nullptr;
        SDK::UClass* SplineMesh = nullptr;
        std::vector<SDK::UObject*> Roots;
    } Types;

    const SDK::FCrBuildingParameters* GetParameters(const MiniMapNative::NativeApi& native,
        const void* manager, Id id)
    {
        const auto* wrapper = native.mass.getConstSharedFragmentPtr(manager, id, Types.Parameters);
        const uint8_t* storage = nullptr;
        if (wrapper) std::memcpy(&storage, wrapper, sizeof(storage));
        if (!storage) return nullptr;
        const auto alignment = uintptr_t(Types.Parameters->MinAlignment);
        if (!alignment || (alignment & (alignment - 1))) return nullptr;
        const auto address = (reinterpret_cast<uintptr_t>(storage) + sizeof(void*) + alignment - 1) & ~(alignment - 1);
        return reinterpret_cast<const SDK::FCrBuildingParameters*>(address);
    }

    bool CopySplineSource(const MiniMapNative::NativeApi& native, const void* manager,
        Id id, B::SourcePayload& output, bool& known)
    {
        known = true;
        const auto* spline = static_cast<const SDK::FAuSplineConnectionFragment*>(
            native.mass.getFragmentDataPtr(manager, id, Types.Spline));
        if (!spline) return true;

        MiniMapBuildingCollector::SplineGeometry copied;
        G::CopySpline(*spline, copied);
        if (copied.Issue != MiniMapBuildingCollector::GeometryIssue::None || copied.Segments.empty())
        {
            if (spline->bSplineCurvesInitialized || spline->Data.SplineData.Num() > 1)
                return false;
            const auto* ap = static_cast<const SDK::FAuAPMassFragment*>(
                native.mass.getFragmentDataPtr(manager, id, Types.AP));
            if (!ap || MiniMapBuildingSplinePayload::Copy(ap->Payload, copied) !=
                MiniMapBuildingSplinePayload::CopyResult::Complete)
            {
                known = false;
                return false;
            }
        }

        output.SplineCurve.reserve(copied.Segments.size());
        for (const auto& segment : copied.Segments)
        {
            B::SplineCurveSegmentSource value;
            value.WorldControls = segment.Controls;
            value.StartKey = segment.StartKey;
            value.EndKey = segment.EndKey;
            value.InterpolationMode = segment.Mode;
            output.SplineCurve.push_back(value);
        }
        // No stable public spline ID exists in the acquired fragments. Keep the
        // source identity unset instead of substituting a pointer or Mass index.
        return true;
    }

    bool Is(const SDK::UObject* object, const SDK::UClass* base)
    {
        if (!object || !base) return false;
        for (auto* type = object->Class; type; type = static_cast<SDK::UClass*>(type->SuperStruct))
            if (type == base) return true;
        return false;
    }

    bool CopyMeshProfile(const SDK::UStaticMesh* mesh, const SDK::USplineMeshComponent* spline,
        uint8_t rank, B::SplineProfileSource& output)
    {
        if (!mesh) return false;
        const auto& bounds = mesh->ExtendedBounds;
        output.BoundsOrigin = {bounds.Origin.X, bounds.Origin.Y, bounds.Origin.Z};
        output.BoundsExtent = {bounds.BoxExtent.X, bounds.BoxExtent.Y, bounds.BoxExtent.Z};
        output.BoundsSphereRadius = bounds.SphereRadius;
        if (spline)
        {
            output.ComponentTranslation = {spline->RelativeLocation.X, spline->RelativeLocation.Y, spline->RelativeLocation.Z};
            output.ComponentRotationDegrees = {spline->RelativeRotation.Pitch, spline->RelativeRotation.Yaw, spline->RelativeRotation.Roll};
            output.ComponentScale = {spline->RelativeScale3D.X, spline->RelativeScale3D.Y, spline->RelativeScale3D.Z};
            const auto& params = spline->SplineParams;
            output.StartScale = {params.StartScale.X, params.StartScale.Y};
            output.EndScale = {params.EndScale.X, params.EndScale.Y};
            output.StartOffset = {params.StartOffset.X, params.StartOffset.Y};
            output.EndOffset = {params.EndOffset.X, params.EndOffset.Y};
            output.StartRoll = params.StartRoll;
            output.EndRoll = params.EndRoll;
            output.Up = {spline->SplineUpDir.X, spline->SplineUpDir.Y, spline->SplineUpDir.Z};
            output.ForwardAxis = uint8_t(spline->ForwardAxis);
        }
        output.SourceRank = rank;
        for (double value : output.BoundsOrigin) if (!std::isfinite(value)) return false;
        for (double value : output.BoundsExtent) if (!std::isfinite(value) || value < 0) return false;
        if (!std::isfinite(output.BoundsSphereRadius) || output.BoundsSphereRadius < 0) return false;
        for (double value : output.ComponentTranslation) if (!std::isfinite(value)) return false;
        for (double value : output.ComponentRotationDegrees) if (!std::isfinite(value)) return false;
        for (double value : output.ComponentScale) if (!std::isfinite(value)) return false;
        for (double value : output.StartScale) if (!std::isfinite(value)) return false;
        for (double value : output.EndScale) if (!std::isfinite(value)) return false;
        for (double value : output.StartOffset) if (!std::isfinite(value)) return false;
        for (double value : output.EndOffset) if (!std::isfinite(value)) return false;
        if (!std::isfinite(output.StartRoll) || !std::isfinite(output.EndRoll) ||
            !MiniMapBuildingGeometryTypes::Finite(output.Up) || output.ForwardAxis > 2) return false;
        return true;
    }

    bool CopyProfilesFromClass(SDK::UClass* type, uint8_t rank, std::vector<B::SplineProfileSource>& output)
    {
        if (!type || !type->ClassDefaultObject) return false;
        const auto* cdo = type->ClassDefaultObject;
        if (Is(cdo, Types.Walkway))
        {
            const auto* walkway = static_cast<const SDK::ACrModularRampWalkway*>(cdo);
            // The mesh reference is optional in the walkway source. Preserve
            // that absence, but do not treat an existing malformed mesh as if
            // the walkway profile were absent.
            if (walkway->SplineMesh)
            {
                B::SplineProfileSource profile;
                if (!CopyMeshProfile(walkway->SplineMesh, nullptr, rank, profile)) return false;
                profile.SelectionOrder = {rank, 0, type == Types.Walkway ? 0 : 1, type->Index, 0, 0, 0, 0, 0,
                    cdo->Index, 0, int64_t(output.size())};
                output.push_back(std::move(profile));
            }
        }
        for (auto* current = type; current; current = static_cast<SDK::UClass*>(current->SuperStruct))
        {
            if (!Is(current, Types.Blueprint)) continue;
            const auto& components = static_cast<SDK::UBlueprintGeneratedClass*>(current)->ComponentTemplates;
            for (auto* component : components)
            {
                if (!Is(component, Types.SplineMesh)) continue;
                const auto* spline = static_cast<const SDK::USplineMeshComponent*>(component);
                B::SplineProfileSource profile;
                if (!CopyMeshProfile(spline->StaticMesh, spline, rank, profile)) return false;
                profile.SelectionOrder = {rank, 1, 1, type->Index, 0, current->Index, 0, component->Index, 0,
                    cdo->Index, 0, int64_t(output.size())};
                output.push_back(std::move(profile));
            }
        }
        return true;
    }

    bool CopySplineProfiles(const MiniMapNative::NativeApi& native,
        SDK::UAuActorPlacementData& placement, uint32_t buildingType,
        std::vector<B::SplineProfileSource>& output)
    {
        if (MiniMapBuildingSplinePolicy::IsDroneRailDefinition(true, buildingType)) return true;
        auto* config = placement.EntityType.EntityConfigPtr;
        if (!config && placement.EntityType.EntityConfig.ObjectID.AssetPath.PackageName.ComparisonIndex != 0)
            config = static_cast<SDK::UMassEntityConfigAsset*>(native.asset.loadSynchronous(&placement.EntityType.EntityConfig));
        std::unordered_set<const SDK::UMassEntityConfigAsset*> visited;
        for (; config; config = config->Config.Parent)
        {
            if (!visited.insert(config).second) return false;
            for (auto* trait : config->Config.Traits)
            {
                SDK::UClass *high = nullptr, *low = nullptr;
                if (Is(trait, Types.Cosmetic))
                {
                    const auto* value = static_cast<const SDK::UCrMassRepVisCosmeticsTrait*>(trait);
                    high = value->HighResTemplateActor; low = value->LowResTemplateActor;
                }
                else if (Is(trait, Types.Visual))
                {
                    const auto* value = static_cast<const SDK::UMassVisualizationTrait*>(trait);
                    high = value->HighResTemplateActor; low = value->LowResTemplateActor;
                }
                if ((high && !CopyProfilesFromClass(high, 0, output)) ||
                    (low && !CopyProfilesFromClass(low, 1, output))) return false;
            }
        }
        auto* authored = static_cast<SDK::UClass*>(native.asset.loadSynchronous(&placement.ActorClass));
        return authored && CopyProfilesFromClass(authored, 2, output) && !output.empty();
    }

    bool SameDouble(double a, double b) noexcept
    { return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b); }

    template<size_t N>
    bool SameValues(const std::array<double, N>& a, const std::array<double, N>& b) noexcept
    {
        for (size_t i = 0; i < N; ++i) if (!SameDouble(a[i], b[i])) return false;
        return true;
    }

    bool SameBounds(const B::WorldBounds& a, const B::WorldBounds& b) noexcept
    {
        return a.Valid == b.Valid && SameDouble(a.MinX, b.MinX) && SameDouble(a.MinY, b.MinY) &&
            SameDouble(a.MaxX, b.MaxX) && SameDouble(a.MaxY, b.MaxY);
    }

    bool EqualSource(const B::SourcePayload& a, const B::SourcePayload& b)
    {
        const bool sameBlueprint = (!a.BlueprintIdentity && !b.BlueprintIdentity) ||
            (a.BlueprintIdentity && b.BlueprintIdentity && *a.BlueprintIdentity == *b.BlueprintIdentity);
        if (a.CanonicalBuildingType != b.CanonicalBuildingType ||
            !sameBlueprint ||
            a.BaseCoreOverlay != b.BaseCoreOverlay ||
            !SameValues(a.Transform.Translation, b.Transform.Translation) ||
            !SameValues(a.Transform.Scale, b.Transform.Scale) || !SameValues(a.Transform.Rotation, b.Transform.Rotation) ||
            a.SplineSourceIdentity != b.SplineSourceIdentity || a.SplineCurve.size() != b.SplineCurve.size() ||
            a.SplineProfiles.size() != b.SplineProfiles.size() ||
            a.SplineSections.size() != b.SplineSections.size() || a.Support != b.Support) return false;
        if (a.NumericalProfileInputs.size() != b.NumericalProfileInputs.size()) return false;
        for (size_t i = 0; i < a.NumericalProfileInputs.size(); ++i)
            if (!SameDouble(a.NumericalProfileInputs[i], b.NumericalProfileInputs[i])) return false;
        for (size_t i = 0; i < a.SplineCurve.size(); ++i)
        {
            const auto& x = a.SplineCurve[i]; const auto& y = b.SplineCurve[i];
            if (!SameValues(x.WorldControls[0], y.WorldControls[0]) ||
                !SameValues(x.WorldControls[1], y.WorldControls[1]) ||
                !SameValues(x.WorldControls[2], y.WorldControls[2]) ||
                !SameValues(x.WorldControls[3], y.WorldControls[3]) ||
                !SameDouble(x.StartKey, y.StartKey) || !SameDouble(x.EndKey, y.EndKey) ||
                x.InterpolationMode != y.InterpolationMode) return false;
        }
        for (size_t i = 0; i < a.SplineProfiles.size(); ++i)
        {
            const auto& x = a.SplineProfiles[i]; const auto& y = b.SplineProfiles[i];
            if (!SameValues(x.BoundsOrigin, y.BoundsOrigin) || !SameValues(x.BoundsExtent, y.BoundsExtent) ||
                !SameDouble(x.BoundsSphereRadius, y.BoundsSphereRadius) ||
                !SameValues(x.ComponentTranslation, y.ComponentTranslation) ||
                !SameValues(x.ComponentRotationDegrees, y.ComponentRotationDegrees) || !SameValues(x.ComponentScale, y.ComponentScale) ||
                !SameValues(x.StartScale, y.StartScale) || !SameValues(x.EndScale, y.EndScale) ||
                !SameValues(x.StartOffset, y.StartOffset) || !SameValues(x.EndOffset, y.EndOffset) ||
                !SameDouble(x.StartRoll, y.StartRoll) || !SameDouble(x.EndRoll, y.EndRoll) ||
                !SameValues(x.Up, y.Up) || x.ForwardAxis != y.ForwardAxis || x.SourceRank != y.SourceRank ||
                x.SelectionOrder != y.SelectionOrder) return false;
        }
        for (size_t i = 0; i < a.SplineSections.size(); ++i)
        {
            const auto& x = a.SplineSections[i]; const auto& y = b.SplineSections[i];
            if (x.Identity != y.Identity || !SameBounds(x.Bounds, y.Bounds) ||
                x.NumericalInputs.size() != y.NumericalInputs.size()) return false;
            for (size_t j = 0; j < x.NumericalInputs.size(); ++j)
                if (!SameDouble(x.NumericalInputs[j], y.NumericalInputs[j])) return false;
        }
        return true;
    }
}

namespace MiniMapBuildingComplexSourceAcquisition
{
    bool IsSameSource(const B::SourcePayload& a, const B::SourcePayload& b) noexcept
    { return EqualSource(a, b); }

    bool Initialize() noexcept
    {
        if (Types.Parameters && Types.Transform && Types.Spline && Types.AP && Types.Visual && Types.Cosmetic &&
            Types.Walkway && Types.Blueprint && Types.SplineMesh)
            return MiniMapBuildingBlueprintIdentity::Initialize();
        auto* objects = MiniMap::Services::TryObjects();
        const auto* native = MiniMapNative::Get();
        if (!objects || !objects->Ready() || !native || !native->asset.retainObject ||
            !native->asset.releaseObject || !native->asset.loadSynchronous) return false;
        struct RootScope
        {
            const MiniMapNative::NativeApi& Native;
            std::vector<SDK::UObject*> Values;
            bool Committed = false;
            ~RootScope() { if (!Committed) for (auto* value : Values) Native.asset.releaseObject(value); }
        } roots{*native};
        try
        {
            roots.Values.reserve(9);
            auto lookup = [&](const wchar_t* path) -> SDK::UObject*
            {
                const auto result = objects->FindResident(nullptr, nullptr, path, false);
                auto* value = result.value ? *result.value : nullptr;
                if (value) { native->asset.retainObject(value); roots.Values.push_back(value); }
                return value;
            };
            Metadata candidate;
            candidate.Parameters = static_cast<SDK::UScriptStruct*>(lookup(L"/Script/Chimera.CrBuildingParameters"));
            candidate.Transform = static_cast<SDK::UScriptStruct*>(lookup(L"/Script/MassCommon.TransformFragment"));
            candidate.Spline = static_cast<SDK::UScriptStruct*>(lookup(L"/Script/AuActorPlacement.AuSplineConnectionFragment"));
            candidate.AP = static_cast<SDK::UScriptStruct*>(lookup(L"/Script/AuActorPlacement.AuAPMassFragment"));
            candidate.Visual = static_cast<SDK::UClass*>(lookup(L"/Script/MassRepresentation.MassVisualizationTrait"));
            candidate.Cosmetic = static_cast<SDK::UClass*>(lookup(L"/Script/Chimera.CrMassRepVisCosmeticsTrait"));
            candidate.Walkway = static_cast<SDK::UClass*>(lookup(L"/Script/Chimera.CrModularRampWalkway"));
            candidate.Blueprint = static_cast<SDK::UClass*>(lookup(L"/Script/Engine.BlueprintGeneratedClass"));
            candidate.SplineMesh = static_cast<SDK::UClass*>(lookup(L"/Script/Engine.SplineMeshComponent"));
            if (!candidate.Parameters || !candidate.Transform || !candidate.Spline || !candidate.AP ||
                !candidate.Visual || !candidate.Cosmetic || !candidate.Walkway || !candidate.Blueprint || !candidate.SplineMesh ||
                !MiniMapBuildingBlueprintIdentity::Initialize())
                return false;
            candidate.Roots = std::move(roots.Values);
            Types = std::move(candidate);
            roots.Committed = true;
            return true;
        }
        catch (...) { return false; }
    }

    void Shutdown() noexcept
    {
        if (const auto* native = MiniMapNative::Get(); native && native->asset.releaseObject)
            for (auto* root : Types.Roots) native->asset.releaseObject(root);
        Types = {};
    }

    bool TryAcquire(const MiniMapNative::NativeApi& native, const void* manager,
        Id entity, uint64_t generation, B::SourcePayload& output, bool& complete) noexcept
    {
        output = {};
        output.SourceGeneration = generation;
        output.Support = B::SupportState::Unknown;
        complete = false;
        if (!generation || !manager || entity.Index < 0 || entity.SerialNumber <= 0 ||
            !Types.Parameters || !Types.Transform || !Types.Spline || !Types.AP ||
            !Types.Visual || !Types.Cosmetic || !Types.Walkway || !Types.Blueprint || !Types.SplineMesh ||
            !native.asset.loadSynchronous ||
            !native.player.isGameThread || !native.mass.isEntityValid ||
            !native.mass.isEntityBuilt || !native.mass.getFragmentDataPtr ||
            !native.mass.getConstSharedFragmentPtr || !native.player.isGameThread()) return false;
        try
        {
            if (!native.mass.isEntityValid(manager, entity) || !native.mass.isEntityBuilt(manager, entity)) return false;
            const auto* parameters = GetParameters(native, manager, entity);
            auto* placement = parameters ? parameters->PlacementData : nullptr;
            if (!placement || !native.asset.retainObject || !native.asset.releaseObject) return false;
            struct PlacementRoot
            {
                const MiniMapNative::NativeApi& Native;
                SDK::UObject* Object;
                PlacementRoot(const MiniMapNative::NativeApi& nativeApi, SDK::UObject* object)
                    : Native(nativeApi), Object(object) { Native.asset.retainObject(Object); }
                ~PlacementRoot() { Native.asset.releaseObject(Object); }
            } placementRoot{native, placement};
            const uint32_t buildingType = uint32_t(placement->BuildingID);
            auto blueprint = MiniMapBuildingBlueprintIdentity::Copy(*placement);
            if (!blueprint) return false;
            if (!native.mass.isEntityValid(manager, entity) || !native.mass.isEntityBuilt(manager, entity)) return false;

            const auto* transform = static_cast<const SDK::FTransform*>(
                native.mass.getFragmentDataPtr(manager, entity, Types.Transform));
            if (!transform) return false;
            B::SourcePayload candidate;
            candidate.SourceGeneration = generation;
            candidate.Support = B::SupportState::Unknown;
            candidate.CanonicalBuildingType = buildingType;
            candidate.BlueprintIdentity = std::move(blueprint);
            candidate.BaseCoreOverlay = *candidate.BlueprintIdentity ==
                "/Game/Chimera/Buildings/BaseCore/BP_BaseCore.BP_BaseCore_C";
            candidate.Transform.Translation = {transform->Translation.X, transform->Translation.Y, transform->Translation.Z};
            candidate.Transform.Scale = {transform->Scale3D.X, transform->Scale3D.Y, transform->Scale3D.Z};
            candidate.Transform.Rotation = {transform->Rotation.X, transform->Rotation.Y, transform->Rotation.Z, transform->Rotation.W};
            double norm = 0;
            for (double q : candidate.Transform.Rotation) norm += q * q;
            if (!MiniMapBuildingGeometryTypes::Finite(candidate.Transform.Translation) ||
                !MiniMapBuildingGeometryTypes::Finite(candidate.Transform.Scale) || !std::isfinite(norm) || norm <= 0) return false;
            for (double& q : candidate.Transform.Rotation) q /= std::sqrt(norm);
            if (!MiniMapBuildingGeometryTypes::Valid(candidate.Transform)) return false;

            bool splineKnown = false;
            if (!CopySplineSource(native, manager, entity, candidate, splineKnown)) return false;
            if (!candidate.SplineCurve.empty())
                splineKnown = CopySplineProfiles(native, *placement, buildingType, candidate.SplineProfiles) && splineKnown;
            if (!splineKnown) return false;
            output = std::move(candidate);
            complete = true;
            return true;
        }
        catch (...) { return false; }
    }
}
#endif
