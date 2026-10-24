#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSplinePayload.h"
#include "BuildingGeometry.h"
#include "Native/NativeApi.h"
#include <cstddef>
#include <utility>

namespace
{
    namespace G = MiniMapBuildingGeometry;
    using Api = MiniMapNative::SplinePayloadApi;
    using NativeValue = SDK::FAuAPSplineActorPayloadData;
    using Result = MiniMapBuildingSplinePayload::CopyResult;

    static_assert(sizeof(NativeValue) == 0xE8 && alignof(NativeValue) == 8);
    static_assert(offsetof(NativeValue, Data) == 0x68);
    static_assert(offsetof(SDK::FAuAPSplineData, SplineData) == 8);
    static_assert(offsetof(NativeValue, Data) + offsetof(SDK::FAuAPSplineData, SplineData) == 0x70);
    static_assert(sizeof(decltype(SDK::FAuAPSplineData::SplineData)) == 0x10);
    static_assert(sizeof(SDK::FAuAPSplinePointData) == 0x40);
    static_assert(offsetof(SDK::FAuAPSplinePointData, Position) == 0x10);
    static_assert(offsetof(SDK::FAuAPSplinePointData, Tangent) == 0x28);

    struct alignas(8) PayloadStorage { std::byte Bytes[0xE8]; };
    static_assert(sizeof(PayloadStorage) == 0xE8 && alignof(PayloadStorage) == 8);

    class ScopedPayload final
    {
        PayloadStorage storage;
        Api::DestroyFn destroy;
        NativeValue* constructed = nullptr;
    public:
        ScopedPayload(const Api& api, const SDK::FAuAPPayloadDataHandle& handle) : destroy(api.destroy)
        {
            auto* value = reinterpret_cast<NativeValue*>(storage.Bytes);
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
        const NativeValue& Value() const { return *constructed; }
    };

    Result Convert(const SDK::FAuAPSplineData& data, G::SplineGeometry& output)
    {
        const auto& points = data.SplineData;
        const auto count = points.Num();
        // Array-shape/numerical checks on the owned native result, not pointer probing.
        if (count < 0 || points.Max() < count || (count && !points.GetDataPtr()))
        { output.Issue = G::GeometryIssue::InvalidArray; return Result::InvalidData; }
        if (count < 2)
        { output.Issue = G::GeometryIssue::Uninitialized; return Result::NotReady; }
        if (count > 2)
        { output.Issue = G::GeometryIssue::InvalidCurve; return Result::UnsupportedPointCount; }

        const auto p0 = G::Copy(points[0].Position), p1 = G::Copy(points[1].Position);
        const auto t0 = G::Copy(points[0].Tangent), t1 = G::Copy(points[1].Tangent);
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
            !api->splinePayload.payloadType || !api->player.isGameThread) return CopyResult::Unavailable;
        if (!api->player.isGameThread()) return CopyResult::WrongThread;
        const ScopedPayload value(api->splinePayload, handle);
        return Convert(value.Value().Data, output);
    }
}
#endif
