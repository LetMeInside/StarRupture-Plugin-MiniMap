// Owns pointer-free StarRupture acquisition values, including unresolved occurrences.
// These are native facts, not rendering descriptions or Section membership evidence.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Services/Map/Buildings/SourceContentPrecision.h"
#include "Services/Map/Buildings/SplineProfile.h"

namespace MiniMap::BuildingAcquisition
{
    /** Immutable after publication; nested owners contain CPU values only. */
    struct BuildingAcquiredFacts
    {
        using Point = std::array<double, 3>;
        using Quaternion = std::array<double, 4>;

        /** Describes native acquisition evidence, independently of render support. */
        enum class Status : std::uint8_t { Unknown, Missing, Invalid, Unavailable, Ready };

        /** Exact half-open map rectangle in native centimetres; X right, Y down. */
        struct Region
        {
            double X = 0;
            double Y = 0;
            double Width = 0;
            double Height = 0;
            /** Compares exact request values without introducing a selection margin. */
            bool operator==(const Region&) const = default;
        };

        /** Copied authored definition facts and immutable pointer-free spline profiles. */
        struct Definition
        {
            std::int32_t ObjectIndex = 0;
            std::uint32_t BuildingId = 0;
            std::string CanonicalClassPath;
            std::string EntityConfigPath;
            Status ClassStatus = Status::Unknown;
            Status ConfigReferenceStatus = Status::Unknown;
            Status InteriorStatus = Status::Unknown;
            bool InteriorOnly = false;
            ::Services::Map::Buildings::SplineProfileAcquisitionStatus SplineProfileStatus =
                ::Services::Map::Buildings::SplineProfileAcquisitionStatus::Unknown;
            std::shared_ptr<const ::Services::Map::Buildings::SplineProfileSet> SplineProfiles;
            /** Compares copied values to avoid publishing unchanged retry results. */
            bool operator==(const Definition& other) const
            {
                const bool profilesMatch = SplineProfiles == other.SplineProfiles ||
                    (SplineProfiles && other.SplineProfiles && *SplineProfiles == *other.SplineProfiles);
                return ObjectIndex == other.ObjectIndex && BuildingId == other.BuildingId &&
                    CanonicalClassPath == other.CanonicalClassPath && EntityConfigPath == other.EntityConfigPath &&
                    ClassStatus == other.ClassStatus && ConfigReferenceStatus == other.ConfigReferenceStatus &&
                    InteriorStatus == other.InteriorStatus && InteriorOnly == other.InteriorOnly &&
                    SplineProfileStatus == other.SplineProfileStatus && profilesMatch;
            }
        };

        /** One authored source point in native order, before curve conversion. */
        struct SplinePoint
        {
            std::uint8_t Index = 0;
            Point Position{};
            Point Tangent{};
            /** Compares all copied source values. */
            bool operator==(const SplinePoint& other) const noexcept
            {
                return Index == other.Index &&
                    ::Services::Map::Buildings::SourceContentPrecision::Equal(Position, other.Position) &&
                    ::Services::Map::Buildings::SourceContentPrecision::Equal(Tangent, other.Tangent);
            }
        };

        /** Native curve point, preserving its key, tangents and uninterpreted mode. */
        template<class Value> struct CurvePoint
        {
            double Key = 0;
            Value ValueAtKey{};
            Value ArriveTangent{};
            Value LeaveTangent{};
            std::uint8_t Mode = 0;
            /** Compares original channel data without numerical preparation. */
            bool operator==(const CurvePoint& other) const noexcept
            {
                return ::Services::Map::Buildings::SourceContentPrecision::Equal(Key, other.Key) &&
                    EqualValue(ValueAtKey, other.ValueAtKey) &&
                    EqualValue(ArriveTangent, other.ArriveTangent) &&
                    EqualValue(LeaveTangent, other.LeaveTangent) && Mode == other.Mode;
            }

        private:
            /** Compares one scalar channel value at source-content precision. */
            static bool EqualValue(double first, double second) noexcept
            {
                return ::Services::Map::Buildings::SourceContentPrecision::Equal(first, second);
            }

            /** Compares an ordered authored channel array at source-content precision. */
            template<std::size_t Size>
            static bool EqualValue(const std::array<double, Size>& first,
                const std::array<double, Size>& second) noexcept
            {
                return ::Services::Map::Buildings::SourceContentPrecision::Equal(first, second);
            }
        };

        /** Native channel and loop parameters, including empty optional channels. */
        template<class Value> struct Curve
        {
            std::vector<CurvePoint<Value>> Points;
            bool IsLooped = false;
            double LoopKeyOffset = 0;
            /** Compares all owned channel values. */
            bool operator==(const Curve& other) const noexcept
            {
                if (IsLooped != other.IsLooped || Points.size() != other.Points.size() ||
                    !::Services::Map::Buildings::SourceContentPrecision::Equal(LoopKeyOffset, other.LoopKeyOffset))
                {
                    return false;
                }
                for (std::size_t index = 0; index < Points.size(); ++index)
                {
                    if (!(Points[index] == other.Points[index]))
                    {
                        return false;
                    }
                }
                return true;
            }
        };

        /** Common FAuAPSplineData values copied from fragment or temporary payload. */
        struct SplineData
        {
            std::vector<SplinePoint> Points;
            std::int32_t NativePointCount = 0;
            Status ValueStatus = Status::Unknown;
            Point LocationWS{};
            Point RotationDegreesWS{}; // Pitch, Yaw, Roll, copied without conversion.
            double RecordedLength = 0;
            /** Compares source points and authored placement parameters. */
            bool operator==(const SplineData& other) const noexcept
            {
                return Points == other.Points && NativePointCount == other.NativePointCount &&
                    ValueStatus == other.ValueStatus &&
                    ::Services::Map::Buildings::SourceContentPrecision::Equal(LocationWS, other.LocationWS) &&
                    ::Services::Map::Buildings::SourceContentPrecision::Equal(
                        RotationDegreesWS, other.RotationDegreesWS) &&
                    ::Services::Map::Buildings::SourceContentPrecision::Equal(RecordedLength, other.RecordedLength);
            }
        };

        /** Identifies which supplied source can reconstruct the native spline. */
        enum class SplineProvenance : std::uint8_t { FragmentPoints, InitializedCurves, Payload };

        /** Owns raw channels and source placement; never contains hulls or surfaces.
         * Position/tangent values preserve the existing world-space integration basis.
         * Other channels retain native semantics; Mass placement must not be applied twice.
         */
        struct Spline
        {
            SplineData FragmentData;
            std::optional<SplineData> PayloadData;
            Curve<Point> Position;
            Curve<Quaternion> Rotation;
            Curve<Point> Scale;
            Curve<double> Reparameterization;
            std::uint32_t CurveVersion = 0;
            bool CurvesInitialized = false;
            SplineProvenance Provenance = SplineProvenance::FragmentPoints;
            Status SourceStatus = Status::Unknown;
			/** Compares copied source facts at six-decimal precision without treating hashes as identity. */
            bool operator==(const Spline& other) const noexcept
            {
                return FragmentData == other.FragmentData && PayloadData == other.PayloadData &&
                    Position == other.Position && Rotation == other.Rotation && Scale == other.Scale &&
                    Reparameterization == other.Reparameterization && CurveVersion == other.CurveVersion &&
                    CurvesInitialized == other.CurvesInitialized && Provenance == other.Provenance &&
                    SourceStatus == other.SourceStatus;
            }
        };

        /** One authoritative occurrence, including partial/missing native facts. */
        struct Building
        {
            std::int32_t Index = 0;
            std::int32_t Serial = 0;
            Status EntityStatus = Status::Unknown;
            Status DefinitionStatus = Status::Unknown;
            Status TransformStatus = Status::Unknown;
            Status SplineClassificationStatus = Status::Unknown;
            Status SplineStatus = Status::Unknown;
            bool HasSpline = false;
            bool Complete = false;
            /** Maximum XY reach resolved before regional admission and retained for Section selection. */
            double ResolvedReach = 0.0;
            Point Position{};
            Quaternion Rotation{};
            Point SignedScale{};
            std::shared_ptr<const Definition> DefinitionFacts;
            std::shared_ptr<const Spline> SplineFacts;
            /** Compares values/immutable owners, not content hashes as identity. */
            bool operator==(const Building& other) const noexcept
            {
                const bool definitionsMatch = DefinitionFacts == other.DefinitionFacts ||
                    (DefinitionFacts && other.DefinitionFacts && *DefinitionFacts == *other.DefinitionFacts);
                const bool splinesMatch = SplineFacts == other.SplineFacts ||
                    (SplineFacts && other.SplineFacts && *SplineFacts == *other.SplineFacts);
                return Index == other.Index && Serial == other.Serial && EntityStatus == other.EntityStatus &&
                    DefinitionStatus == other.DefinitionStatus && TransformStatus == other.TransformStatus &&
                    SplineClassificationStatus == other.SplineClassificationStatus && SplineStatus == other.SplineStatus &&
                    HasSpline == other.HasSpline && Complete == other.Complete &&
                    ::Services::Map::Buildings::SourceContentPrecision::Equal(ResolvedReach, other.ResolvedReach) &&
                    ::Services::Map::Buildings::SourceContentPrecision::Equal(Position, other.Position) &&
                    (TransformStatus == Status::Ready
                        ? ::Services::Map::Buildings::SourceContentPrecision::EqualQuaternion(Rotation, other.Rotation)
                        : ::Services::Map::Buildings::SourceContentPrecision::Equal(Rotation, other.Rotation)) &&
                    ::Services::Map::Buildings::SourceContentPrecision::Equal(SignedScale, other.SignedScale) &&
                    definitionsMatch && splinesMatch;
            }
        };

        bool HasRequestedRegion = false;
        Region RequestedRegion;
        bool MembershipComplete = false;
        bool AcquisitionComplete = false;
        std::vector<Building> Buildings;
    };
}
