#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSplinePayload.h"
#include "BuildingGeometry.h"
#include "Native/NativeApi.h"
#include "Adapters/AlienX/GameFields.h"
#include <utility>

namespace
{
    namespace G = MiniMapBuildingGeometry;
    using Api = MiniMapNative::SplinePayloadApi;
    using NativeValue = SDK::FAuAPSplineActorPayloadData;
    using Result = MiniMapBuildingSplinePayload::CopyResult;

    // The block the game constructs the payload in is the payload's size on the
    // running build, and its spline data sits where the map places it.
    class ScopedPayload final
    {
        PluginMap::StructBuffer storage;
        Api::DestroyFn destroy;
        NativeValue* constructed = nullptr;
    public:
        ScopedPayload(const Api& api, const SDK::FAuAPPayloadDataHandle& handle)
            : storage(MiniMapFields::SplinePayloadSize), destroy(api.destroy)
        {
            if (!storage) return;
            auto* value = reinterpret_cast<NativeValue*>(storage.Data());
            api.get(value, &handle);
            // Mark constructed only after the native constructor/getter returns.
            // If it throws before returning, do not destroy unconstructed storage.
            constructed = value;
        }
        ~ScopedPayload() noexcept { if (constructed) destroy(constructed); }
        ScopedPayload(const ScopedPayload&) = delete;
        ScopedPayload& operator=(const ScopedPayload&) = delete;
        ScopedPayload(ScopedPayload&&) = delete;
        ScopedPayload& operator=(ScopedPayload&&) = delete;
        // Null when the block or the member did not resolve on this build.
        const SDK::FAuAPSplineData* Data() const { return MiniMapFields::SplinePayloadData.In(constructed); }
    };

    Result Convert(const SDK::FAuAPSplineData& data, G::SplineGeometry& output)
    {
        const auto* authored = G::AuthoredPoints(data);
        if (!authored) { output.Issue = G::GeometryIssue::InvalidArray; return Result::InvalidData; }
        const auto& points = *authored;
        const auto count = points.Num;
        // Array-shape/numerical checks on the owned native result, not pointer probing.
        if (count < 0 || points.Max < count || (count && !points.Data))
        { output.Issue = G::GeometryIssue::InvalidArray; return Result::InvalidData; }
        if (count < 2)
        { output.Issue = G::GeometryIssue::Uninitialized; return Result::NotReady; }
        if (count > 2)
        { output.Issue = G::GeometryIssue::InvalidCurve; return Result::UnsupportedPointCount; }

        G::Point p0, t0, p1, t1;
        if (!G::AuthoredPoint(points, 0, p0, t0) || !G::AuthoredPoint(points, 1, p1, t1))
        { output.Issue = G::GeometryIssue::InvalidArray; return Result::InvalidData; }
        if (!G::Finite(p0) || !G::Finite(p1) || !G::Finite(t0) || !G::Finite(t1))
        { output.Issue = G::GeometryIssue::NonFinite; return Result::InvalidData; }

        G::CurveSegment segment;
        segment.Mode = uint8_t(SDK::EInterpCurveMode::CIM_CurveUser);
        segment.StartKey = 0; segment.EndKey = 1;
        segment.Controls[0] = p0; segment.Controls[3] = p1;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            segment.Controls[1][axis] = p0[axis] + t0[axis] / 3;
            segment.Controls[2][axis] = p1[axis] - t1[axis] / 3;
        }
        // Match CopySpline's existing two-point numerical bounds/sanity behavior.
        G::LocalBounds range;
        range.Min = range.Max = p0;
        for (const auto& control : segment.Controls)
        {
            if (!G::Finite(control))
            { output.Issue = G::GeometryIssue::NonFinite; return Result::InvalidData; }
            for (size_t axis = 0; axis < 3; ++axis)
            {
                range.Min[axis] = (std::min)(range.Min[axis], control[axis]);
                range.Max[axis] = (std::max)(range.Max[axis], control[axis]);
            }
        }
        for (size_t axis = 0; axis < 3; ++axis)
            if (range.Max[axis] - range.Min[axis] > G::kMaximumGeometrySpan)
            { output.Issue = G::GeometryIssue::Oversized; return Result::InvalidData; }

        G::SplineGeometry owned;
        G::CurveHull(owned.CenterlineBounds, segment.Controls, 2);
        owned.CenterlineBounds = G::Inflate(owned.CenterlineBounds, 0.01);
        owned.Segments.push_back(segment);
        output = std::move(owned);
        return Result::Complete;
    }
}

namespace MiniMapBuildingSplinePayload
{
    CopyResult Copy(const SDK::FAuAPPayloadDataHandle& handle, G::SplineGeometry& output)
    {
        output = {};
        output.Issue = G::GeometryIssue::MissingSource;
        const auto* api = MiniMapNative::Get();
        if (!api || !api->splinePayload.get || !api->splinePayload.destroy ||
            !api->player.isGameThread) return CopyResult::Unavailable;
        if (!api->player.isGameThread()) return CopyResult::WrongThread;
        const ScopedPayload value(api->splinePayload, handle);
        const auto* data = value.Data();
        if (!data) return CopyResult::Unavailable;
        return Convert(*data, output);
    }
}
#endif
