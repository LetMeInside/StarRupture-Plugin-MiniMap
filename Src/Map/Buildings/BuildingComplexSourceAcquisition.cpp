#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingComplexSourceAcquisition.h"
#include "BuildingBlueprintIdentity.h"
#include "BuildingCollector.h"
#include "BuildingGeometry.h"
#include "BuildingSplinePayload.h"
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

    bool SameDouble(double a, double b) noexcept
    { return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b); }

    template<size_t N>
    bool SameValues(const std::array<double, N>& a, const std::array<double, N>& b) noexcept
    {
        for (size_t i = 0; i < N; ++i) if (!SameDouble(a[i], b[i])) return false;
        return true;
    }

    bool EqualSource(const B::SourcePayload& a, const B::SourcePayload& b)
    {
        const bool sameBlueprint = (!a.BlueprintIdentity && !b.BlueprintIdentity) ||
            (a.BlueprintIdentity && b.BlueprintIdentity && *a.BlueprintIdentity == *b.BlueprintIdentity);
        if (a.CanonicalBuildingType != b.CanonicalBuildingType ||
            !sameBlueprint ||
            !SameValues(a.Transform.Translation, b.Transform.Translation) ||
            !SameValues(a.Transform.Scale, b.Transform.Scale) || !SameValues(a.Transform.Rotation, b.Transform.Rotation) ||
            a.SplineSourceIdentity != b.SplineSourceIdentity || a.SplineCurve.size() != b.SplineCurve.size()) return false;
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
        return true;
    }
}

namespace MiniMapBuildingComplexSourceAcquisition
{
    bool IsSameSource(const B::SourcePayload& a, const B::SourcePayload& b) noexcept
    { return EqualSource(a, b); }

    bool Initialize() noexcept
    {
        if (Types.Parameters && Types.Transform && Types.Spline && Types.AP)
            return MiniMapBuildingBlueprintIdentity::Initialize();
        auto* objects = MiniMap::Services::TryObjects();
        const auto* native = MiniMapNative::Get();
        if (!objects || !objects->Ready() || !native || !native->asset.retainObject ||
            !native->asset.releaseObject) return false;
        struct RootScope
        {
            const MiniMapNative::NativeApi& Native;
            std::vector<SDK::UObject*> Values;
            bool Committed = false;
            ~RootScope() { if (!Committed) for (auto* value : Values) Native.asset.releaseObject(value); }
        } roots{*native};
        try
        {
            roots.Values.reserve(4);
            auto lookup = [&](const wchar_t* path) -> SDK::UScriptStruct*
            {
                const auto result = objects->FindResident(nullptr, nullptr, path, false);
                auto* value = result.value ? static_cast<SDK::UScriptStruct*>(*result.value) : nullptr;
                if (value) { native->asset.retainObject(value); roots.Values.push_back(value); }
                return value;
            };
            Metadata candidate;
            candidate.Parameters = lookup(L"/Script/Chimera.CrBuildingParameters");
            candidate.Transform = lookup(L"/Script/MassCommon.TransformFragment");
            candidate.Spline = lookup(L"/Script/AuActorPlacement.AuSplineConnectionFragment");
            candidate.AP = lookup(L"/Script/AuActorPlacement.AuAPMassFragment");
            if (!candidate.Parameters || !candidate.Transform || !candidate.Spline || !candidate.AP ||
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
            output = std::move(candidate);
            complete = splineKnown;
            return true;
        }
        catch (...) { return false; }
    }
}
#endif
