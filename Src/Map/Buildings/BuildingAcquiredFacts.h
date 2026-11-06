// Owns pointer-free StarRupture acquisition values, including unresolved occurrences.
// These are native facts, not rendering descriptions or Section membership evidence.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

        /** Copied authored definition facts; no engine root or rendering profile. */
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
            /** Compares copied values to avoid publishing unchanged retry results. */
            bool operator==(const Definition&) const = default;
        };

        /** One authored source point in native order, before curve conversion. */
        struct SplinePoint
        {
            std::uint8_t Index = 0;
            Point Position{};
            Point Tangent{};
            /** Compares all copied source values. */
            bool operator==(const SplinePoint&) const = default;
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
            bool operator==(const CurvePoint&) const = default;
        };

        /** Native channel and loop parameters, including empty optional channels. */
        template<class Value> struct Curve
        {
            std::vector<CurvePoint<Value>> Points;
            bool IsLooped = false;
            double LoopKeyOffset = 0;
            /** Compares all owned channel values. */
            bool operator==(const Curve&) const = default;
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
            bool operator==(const SplineData&) const = default;
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
            /** Compares raw facts without treating a content hash as source identity. */
            bool operator==(const Spline&) const = default;
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
            Point Position{};
            Quaternion Rotation{};
            Point SignedScale{};
            std::shared_ptr<const Definition> DefinitionFacts;
            std::shared_ptr<const Spline> SplineFacts;
            /** Compares values/immutable owners, not content hashes as identity. */
            bool operator==(const Building&) const = default;
        };

        std::uint64_t SessionGeneration = 0;
        std::uint64_t WorldGeneration = 0;
        bool HasRequestedRegion = false;
        Region RequestedRegion;
        bool MembershipComplete = false;
        bool AcquisitionComplete = false;
        std::vector<Building> Buildings;
    };
}
