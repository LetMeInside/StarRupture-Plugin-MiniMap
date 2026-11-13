// Copies native spline channels and payload data without deriving any render geometry.
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSplineSource.h"
#include "Native/NativeApi.h"
#include <SDK/AuActorPlacement_structs.hpp>
#include <SDK/CoreUObject_structs.hpp>
#include <SDK/Engine_structs.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

static_assert(sizeof(SDK::FAuAPSplineData) == 0x50);
static_assert(alignof(SDK::FAuAPSplineData) == 8);
static_assert(offsetof(SDK::FAuAPSplineData, SplineData) == 0x08);
static_assert(sizeof(decltype(SDK::FAuAPSplineData::SplineData)) == 0x10);
static_assert(std::is_same_v<decltype(SDK::FAuAPSplineData::SplineData),
    SDK::TArray<SDK::FAuAPSplinePointData>>);
static_assert(offsetof(SDK::FAuAPSplineData, SplineLocationWS) == 0x18);
static_assert(sizeof(decltype(SDK::FAuAPSplineData::SplineLocationWS)) == 0x18);
static_assert(offsetof(SDK::FAuAPSplineData, SplineRotationWS) == 0x30);
static_assert(sizeof(decltype(SDK::FAuAPSplineData::SplineRotationWS)) == 0x18);
static_assert(offsetof(SDK::FAuAPSplineData, SplineRealLength) == 0x48);
static_assert(sizeof(decltype(SDK::FAuAPSplineData::SplineRealLength)) == sizeof(float));
static_assert(sizeof(SDK::FAuAPSplinePointData) == 0x40);
static_assert(alignof(SDK::FAuAPSplinePointData) == 8);
static_assert(offsetof(SDK::FAuAPSplinePointData, Index) == 0x08);
static_assert(sizeof(decltype(SDK::FAuAPSplinePointData::Index)) == sizeof(std::uint8_t));
static_assert(offsetof(SDK::FAuAPSplinePointData, Position) == 0x10);
static_assert(sizeof(decltype(SDK::FAuAPSplinePointData::Position)) == 0x18);
static_assert(std::is_same_v<decltype(SDK::FAuAPSplinePointData::Position), SDK::FVector_NetQuantize>);
static_assert(offsetof(SDK::FAuAPSplinePointData, Tangent) == 0x28);
static_assert(sizeof(decltype(SDK::FAuAPSplinePointData::Tangent)) == 0x18);
static_assert(std::is_same_v<decltype(SDK::FAuAPSplinePointData::Tangent), SDK::FVector_NetQuantize10>);
static_assert(sizeof(SDK::FSplineCurves) == 0x68);
static_assert(alignof(SDK::FSplineCurves) == 8);
static_assert(offsetof(SDK::FSplineCurves, Position) == 0x00);
static_assert(offsetof(SDK::FSplineCurves, Rotation) == 0x18);
static_assert(offsetof(SDK::FSplineCurves, Scale) == 0x30);
static_assert(offsetof(SDK::FSplineCurves, ReparamTable) == 0x48);
static_assert(offsetof(SDK::FSplineCurves, Version) == 0x60);
static_assert(sizeof(decltype(SDK::FSplineCurves::Version)) == sizeof(std::uint32_t));
static_assert(sizeof(SDK::FInterpCurveVector) == 0x18);
static_assert(alignof(SDK::FInterpCurveVector) == 8);
static_assert(offsetof(SDK::FInterpCurveVector, Points) == 0x00);
static_assert(sizeof(decltype(SDK::FInterpCurveVector::Points)) == 0x10);
static_assert(std::is_same_v<decltype(SDK::FInterpCurveVector::Points),
    SDK::TArray<SDK::FInterpCurvePointVector>>);
static_assert(offsetof(SDK::FInterpCurveVector, bIsLooped) == 0x10);
static_assert(sizeof(decltype(SDK::FInterpCurveVector::bIsLooped)) == sizeof(bool));
static_assert(offsetof(SDK::FInterpCurveVector, LoopKeyOffset) == 0x14);
static_assert(sizeof(decltype(SDK::FInterpCurveVector::LoopKeyOffset)) == sizeof(float));
static_assert(sizeof(SDK::FInterpCurveQuat) == 0x18);
static_assert(alignof(SDK::FInterpCurveQuat) == 8);
static_assert(offsetof(SDK::FInterpCurveQuat, Points) == 0x00);
static_assert(sizeof(decltype(SDK::FInterpCurveQuat::Points)) == 0x10);
static_assert(std::is_same_v<decltype(SDK::FInterpCurveQuat::Points),
    SDK::TArray<SDK::FInterpCurvePointQuat>>);
static_assert(offsetof(SDK::FInterpCurveQuat, bIsLooped) == 0x10);
static_assert(sizeof(decltype(SDK::FInterpCurveQuat::bIsLooped)) == sizeof(bool));
static_assert(offsetof(SDK::FInterpCurveQuat, LoopKeyOffset) == 0x14);
static_assert(sizeof(decltype(SDK::FInterpCurveQuat::LoopKeyOffset)) == sizeof(float));
static_assert(sizeof(SDK::FInterpCurveFloat) == 0x18);
static_assert(alignof(SDK::FInterpCurveFloat) == 8);
static_assert(offsetof(SDK::FInterpCurveFloat, Points) == 0x00);
static_assert(sizeof(decltype(SDK::FInterpCurveFloat::Points)) == 0x10);
static_assert(std::is_same_v<decltype(SDK::FInterpCurveFloat::Points),
    SDK::TArray<SDK::FInterpCurvePointFloat>>);
static_assert(offsetof(SDK::FInterpCurveFloat, bIsLooped) == 0x10);
static_assert(sizeof(decltype(SDK::FInterpCurveFloat::bIsLooped)) == sizeof(bool));
static_assert(offsetof(SDK::FInterpCurveFloat, LoopKeyOffset) == 0x14);
static_assert(sizeof(decltype(SDK::FInterpCurveFloat::LoopKeyOffset)) == sizeof(float));
static_assert(sizeof(SDK::FInterpCurvePointVector) == 0x58);
static_assert(alignof(SDK::FInterpCurvePointVector) == 8);
static_assert(offsetof(SDK::FInterpCurvePointVector, InVal) == 0x00);
static_assert(sizeof(decltype(SDK::FInterpCurvePointVector::InVal)) == sizeof(float));
static_assert(offsetof(SDK::FInterpCurvePointVector, OutVal) == 0x08);
static_assert(sizeof(decltype(SDK::FInterpCurvePointVector::OutVal)) == sizeof(SDK::FVector));
static_assert(offsetof(SDK::FInterpCurvePointVector, ArriveTangent) == 0x20);
static_assert(sizeof(decltype(SDK::FInterpCurvePointVector::ArriveTangent)) == sizeof(SDK::FVector));
static_assert(offsetof(SDK::FInterpCurvePointVector, LeaveTangent) == 0x38);
static_assert(sizeof(decltype(SDK::FInterpCurvePointVector::LeaveTangent)) == sizeof(SDK::FVector));
static_assert(offsetof(SDK::FInterpCurvePointVector, InterpMode) == 0x50);
static_assert(sizeof(decltype(SDK::FInterpCurvePointVector::InterpMode)) == sizeof(std::uint8_t));
static_assert(std::is_same_v<std::underlying_type_t<SDK::EInterpCurveMode>, std::uint8_t>);
static_assert(sizeof(SDK::FInterpCurvePointQuat) == 0x80);
static_assert(alignof(SDK::FInterpCurvePointQuat) == 0x10);
static_assert(offsetof(SDK::FInterpCurvePointQuat, InVal) == 0x00);
static_assert(sizeof(decltype(SDK::FInterpCurvePointQuat::InVal)) == sizeof(float));
static_assert(offsetof(SDK::FInterpCurvePointQuat, OutVal) == 0x10);
static_assert(sizeof(decltype(SDK::FInterpCurvePointQuat::OutVal)) == sizeof(SDK::FQuat));
static_assert(offsetof(SDK::FInterpCurvePointQuat, ArriveTangent) == 0x30);
static_assert(sizeof(decltype(SDK::FInterpCurvePointQuat::ArriveTangent)) == sizeof(SDK::FQuat));
static_assert(offsetof(SDK::FInterpCurvePointQuat, LeaveTangent) == 0x50);
static_assert(sizeof(decltype(SDK::FInterpCurvePointQuat::LeaveTangent)) == sizeof(SDK::FQuat));
static_assert(offsetof(SDK::FInterpCurvePointQuat, InterpMode) == 0x70);
static_assert(sizeof(decltype(SDK::FInterpCurvePointQuat::InterpMode)) == sizeof(std::uint8_t));
static_assert(sizeof(SDK::FInterpCurvePointFloat) == 0x14);
static_assert(alignof(SDK::FInterpCurvePointFloat) == 4);
static_assert(offsetof(SDK::FInterpCurvePointFloat, InVal) == 0x00);
static_assert(sizeof(decltype(SDK::FInterpCurvePointFloat::InVal)) == sizeof(float));
static_assert(offsetof(SDK::FInterpCurvePointFloat, OutVal) == 0x04);
static_assert(sizeof(decltype(SDK::FInterpCurvePointFloat::OutVal)) == sizeof(float));
static_assert(offsetof(SDK::FInterpCurvePointFloat, ArriveTangent) == 0x08);
static_assert(sizeof(decltype(SDK::FInterpCurvePointFloat::ArriveTangent)) == sizeof(float));
static_assert(offsetof(SDK::FInterpCurvePointFloat, LeaveTangent) == 0x0C);
static_assert(sizeof(decltype(SDK::FInterpCurvePointFloat::LeaveTangent)) == sizeof(float));
static_assert(offsetof(SDK::FInterpCurvePointFloat, InterpMode) == 0x10);
static_assert(sizeof(decltype(SDK::FInterpCurvePointFloat::InterpMode)) == sizeof(std::uint8_t));
static_assert(sizeof(SDK::FQuat) == 0x20);
static_assert(alignof(SDK::FQuat) == 0x10);
static_assert(offsetof(SDK::FQuat, X) == 0x00);
static_assert(offsetof(SDK::FQuat, Y) == 0x08);
static_assert(offsetof(SDK::FQuat, Z) == 0x10);
static_assert(offsetof(SDK::FQuat, W) == 0x18);
static_assert(std::is_same_v<decltype(SDK::FQuat::X), double>);
static_assert(std::is_same_v<decltype(SDK::FQuat::Y), double>);
static_assert(std::is_same_v<decltype(SDK::FQuat::Z), double>);
static_assert(std::is_same_v<decltype(SDK::FQuat::W), double>);
static_assert(sizeof(SDK::FVector_NetQuantize) == sizeof(SDK::FVector));
static_assert(alignof(SDK::FVector_NetQuantize) == alignof(SDK::FVector));
static_assert(sizeof(SDK::FVector_NetQuantize10) == sizeof(SDK::FVector));
static_assert(alignof(SDK::FVector_NetQuantize10) == alignof(SDK::FVector));

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
        static_assert(sizeof(decltype(SDK::FAuAPSplineActorPayloadData::Data)) == 0x50);
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
