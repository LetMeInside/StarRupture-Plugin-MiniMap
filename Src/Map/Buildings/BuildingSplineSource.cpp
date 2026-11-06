// Copies native spline channels and payload data without deriving any render geometry.
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSplineSource.h"
#include "Native/NativeApi.h"
#include <SDK/AuActorPlacement_structs.hpp>
#include <cmath>
#include <cstddef>
#include <utility>

namespace MiniMap::BuildingAcquisition
{
    using Facts = BuildingAcquiredFacts;
    using Status = Facts::Status;

    /** Implements typed value copies and temporary payload lifetime only. */
    struct BuildingSplineSource::Copier
    {
        /** Copies native vector components without a coordinate transform. */
        static Facts::Point Value(const SDK::FVector& value)
        {
            return {value.X, value.Y, value.Z};
        }

        /** Copies native quaternion components, including channel tangent quaternions. */
        static Facts::Quaternion Value(const SDK::FQuat& value)
        {
            return {value.X, value.Y, value.Z, value.W};
        }

        /** Widens native float channel values without interpretation. */
        static double Value(float value)
        {
            return value;
        }

        /** Validates a copied scalar, not native memory readability. */
        static bool Finite(double value)
        {
            return std::isfinite(value);
        }

        /** Validates copied vector/quaternion components. */
        template<std::size_t Size> static bool Finite(const std::array<double, Size>& value)
        {
            for (double component : value)
                if (!std::isfinite(component))
                    return false;
            return true;
        }

        /** Checks native array shape before synchronous element copying. */
        template<class Array> static bool ValidArray(const Array& values)
        {
            return values.Num() >= 0 && values.Max() >= values.Num() &&
                (!values.Num() || values.GetDataPtr());
        }

        /** Copies the common fragment/payload source exactly once per supplied source. */
        static bool CopyData(const SDK::FAuAPSplineData& source, Facts::SplineData& output)
        {
            output.NativePointCount = source.SplineData.Num();
            output.ValueStatus = Status::Invalid;
            output.LocationWS = Value(source.SplineLocationWS);
            output.RotationDegreesWS = {source.SplineRotationWS.Pitch,
                source.SplineRotationWS.Yaw, source.SplineRotationWS.Roll};
            output.RecordedLength = source.SplineRealLength;
            bool valid = Finite(output.LocationWS) && Finite(output.RotationDegreesWS) &&
                Finite(output.RecordedLength);
            if (!ValidArray(source.SplineData))
                return false;
            output.Points.reserve(source.SplineData.Num());
            for (const auto& point : source.SplineData)
            {
                Facts::SplinePoint copy{point.Index, Value(point.Position), Value(point.Tangent)};
                valid = Finite(copy.Position) && Finite(copy.Tangent) && valid;
                output.Points.push_back(std::move(copy));
            }
            output.ValueStatus = valid ? Status::Ready : Status::Invalid;
            return valid;
        }

        /** Copies every native curve key/mode/tangent and its loop metadata, even unsupported modes. */
        template<class NativeCurve, class ValueType>
        static bool CopyCurve(const NativeCurve& source, Facts::Curve<ValueType>& output)
        {
            output.IsLooped = source.bIsLooped;
            output.LoopKeyOffset = source.LoopKeyOffset;
            if (!ValidArray(source.Points))
                return false;
            bool valid = Finite(output.LoopKeyOffset);
            output.Points.reserve(source.Points.Num());
            for (const auto& point : source.Points)
            {
                Facts::CurvePoint<ValueType> copy{point.InVal, Value(point.OutVal),
                    Value(point.ArriveTangent), Value(point.LeaveTangent), static_cast<std::uint8_t>(point.InterpMode)};
                valid = Finite(copy.Key) && Finite(copy.ValueAtKey) &&
                    Finite(copy.ArriveTangent) && Finite(copy.LeaveTangent) && valid;
                output.Points.push_back(std::move(copy));
            }
            return valid;
        }

        /** Owns one native hidden-return payload; never copies its shared-handle internals. */
        struct PayloadOwner
        {
            alignas(8) std::byte Storage[0xE8];
            MiniMapNative::SplinePayloadApi::DestroyFn Destroy;
            SDK::FAuAPSplineActorPayloadData* Constructed = nullptr;

            /** Marks the object constructed only after the established native getter returns. */
            PayloadOwner(const MiniMapNative::SplinePayloadApi& api, const SDK::FAuAPPayloadDataHandle& handle)
                : Destroy(api.destroy)
            {
                auto* value = reinterpret_cast<SDK::FAuAPSplineActorPayloadData*>(Storage);
                api.get(value, &handle);
                Constructed = value;
            }
            PayloadOwner(const PayloadOwner&) = delete;
            PayloadOwner& operator=(const PayloadOwner&) = delete;
            /** Destroys only a constructed native value, including on CPU allocation failure. */
            ~PayloadOwner()
            {
                if (Constructed)
                    Destroy(Constructed);
            }
        };
    };

    /** Freezes all native channels before returning; no native array is retained. */
    Facts::Spline BuildingSplineSource::CopyFragment(const SDK::FAuSplineConnectionFragment& fragment)
    {
        Facts::Spline result;
        result.CurvesInitialized = fragment.bSplineCurvesInitialized;
        result.CurveVersion = fragment.SplineCurves.Version;
        result.Provenance = result.CurvesInitialized ? Facts::SplineProvenance::InitializedCurves :
            Facts::SplineProvenance::FragmentPoints;
        bool valid = Copier::CopyData(fragment.Data, result.FragmentData);
        valid = Copier::CopyCurve(fragment.SplineCurves.Position, result.Position) && valid;
        valid = Copier::CopyCurve(fragment.SplineCurves.Rotation, result.Rotation) && valid;
        valid = Copier::CopyCurve(fragment.SplineCurves.Scale, result.Scale) && valid;
        valid = Copier::CopyCurve(fragment.SplineCurves.ReparamTable, result.Reparameterization) && valid;
        const bool hasSource = result.CurvesInitialized ? result.Position.Points.size() >= 2 :
            result.FragmentData.Points.size() >= 2;
        result.SourceStatus = !valid ? Status::Invalid : (hasSource ? Status::Ready : Status::Missing);
        return result;
    }

    /** Matches the original uninitialized/insufficient-point fallback gate, without render-mode policy. */
    bool BuildingSplineSource::NeedsFallback(const Facts::Spline& facts) noexcept
    {
        return !facts.CurvesInitialized && facts.FragmentData.Points.size() <= 1 &&
            facts.FragmentData.NativePointCount <= 1;
    }

    /** Uses the existing Win64 hidden return buffer getter and non-deleting destructor ABI. */
    void BuildingSplineSource::CopyFallback(const SDK::FAuAPPayloadDataHandle& handle, Facts::Spline& facts)
    {
        static_assert(sizeof(SDK::FAuAPSplineActorPayloadData) == 0xE8);
        static_assert(alignof(SDK::FAuAPSplineActorPayloadData) == 8);
        static_assert(offsetof(SDK::FAuAPSplineActorPayloadData, Data) == 0x68);
        static_assert(offsetof(SDK::FAuAPSplinePointData, Position) == 0x10);
        static_assert(offsetof(SDK::FAuAPSplinePointData, Tangent) == 0x28);
        const auto* native = MiniMapNative::Get();
        if (!native || !native->player.isGameThread() || !native->splinePayload.get ||
            !native->splinePayload.destroy || !native->splinePayload.payloadType)
        {
            facts.SourceStatus = Status::Unavailable;
            return;
        }
        const Copier::PayloadOwner payload(native->splinePayload, handle);
        facts.PayloadData.emplace();
        const bool valid = Copier::CopyData(payload.Constructed->Data, *facts.PayloadData);
        facts.Provenance = Facts::SplineProvenance::Payload;
        facts.SourceStatus = !valid ? Status::Invalid :
            (facts.PayloadData->Points.size() >= 2 ? Status::Ready : Status::Missing);
    }
}
#endif
