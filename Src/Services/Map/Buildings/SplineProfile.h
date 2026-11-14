// Defines neutral authored spline profile values shared by acquisition and candidate publication.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "SourceContentPrecision.h"

namespace Services::Map::Buildings
{
	/** Describes whether authored profile facts were acquired, remain retryable, or need no profile. */
	enum class SplineProfileAcquisitionStatus : std::uint8_t
	{
		Unknown,
		NotApplicable,
		Unavailable,
		Invalid,
		Unsupported,
		Complete
	};

	/** Preserves the state of one ordered authored alternative without claiming numerical renderability. */
	enum class SplineProfileAlternativeStatus : std::uint8_t
	{
		Complete,
		MissingMesh,
		InvalidBounds,
		InvalidValues,
		Unsupported
	};

	/** Stores the mesh-local bounds values copied from an authored StaticMesh. */
	struct SplineProfileBounds
	{
		std::array<double, 3> Origin{};
		std::array<double, 3> Extent{};
		double SphereRadius = 0.0;
		/** Compares bounds using source-content precision. */
		bool operator==(const SplineProfileBounds& other) const noexcept
		{
			return SourceContentPrecision::Equal(Origin, other.Origin) &&
				SourceContentPrecision::Equal(Extent, other.Extent) &&
				SourceContentPrecision::Equal(SphereRadius, other.SphereRadius);
		}
	};

	/** Stores authored component translation, source Euler rotation, its established Unreal quaternion, and signed scale. */
	struct SplineProfileTransform
	{
		std::array<double, 3> Translation{};
		std::array<double, 3> RotationDegrees{};
		/** Neutral XYZW quaternion using the legacy Unreal Pitch/Yaw/Roll convention. */
		std::array<double, 4> Rotation{0.0, 0.0, 0.0, 1.0};
		std::array<double, 3> Scale{1.0, 1.0, 1.0};
		/** Compares authored transform values using source-content precision. */
		bool operator==(const SplineProfileTransform& other) const noexcept
		{
			return SourceContentPrecision::Equal(Translation, other.Translation) &&
				SourceContentPrecision::Equal(RotationDegrees, other.RotationDegrees) &&
				SourceContentPrecision::Equal(Rotation, other.Rotation) &&
				SourceContentPrecision::Equal(Scale, other.Scale);
		}
	};

	/** Stores authored endpoint scale, lateral offsets, roll, and spline up direction. */
	struct SplineProfileCrossSection
	{
		std::array<double, 2> StartScale{1.0, 1.0};
		std::array<double, 2> EndScale{1.0, 1.0};
		std::array<double, 2> StartOffset{};
		std::array<double, 2> EndOffset{};
		double StartRoll = 0.0;
		double EndRoll = 0.0;
		std::array<double, 3> Up{0.0, 0.0, 1.0};
		/** Compares cross-section values using source-content precision. */
		bool operator==(const SplineProfileCrossSection& other) const noexcept
		{
			return SourceContentPrecision::Equal(StartScale, other.StartScale) &&
				SourceContentPrecision::Equal(EndScale, other.EndScale) &&
				SourceContentPrecision::Equal(StartOffset, other.StartOffset) &&
				SourceContentPrecision::Equal(EndOffset, other.EndOffset) &&
				SourceContentPrecision::Equal(StartRoll, other.StartRoll) &&
				SourceContentPrecision::Equal(EndRoll, other.EndRoll) &&
				SourceContentPrecision::Equal(Up, other.Up);
		}
	};

	/** Stores one authored alternative and keeps copied values even when a field is unsupported. */
	struct SplineProfileAlternative
	{
		SplineProfileAlternativeStatus Status = SplineProfileAlternativeStatus::Complete;
		bool HasBounds = false;
		SplineProfileBounds Bounds{};
		SplineProfileTransform Transform{};
		SplineProfileCrossSection CrossSection{};
		/** Raw numeric axis value; game SDK enums are not part of the neutral contract. */
		std::uint8_t ForwardAxis = 0;
		/** Compares one complete authored alternative, including rounded numeric source values. */
		bool operator==(const SplineProfileAlternative& other) const noexcept
		{
			return Status == other.Status && HasBounds == other.HasBounds && Bounds == other.Bounds &&
				Transform == other.Transform && CrossSection == other.CrossSection && ForwardAxis == other.ForwardAxis;
		}
	};

	/** Owns profile alternatives in their authored selection order as immutable CPU-only values. */
	struct SplineProfileSet
	{
		std::vector<SplineProfileAlternative> Alternatives;
		/** Compares ordered alternatives using the source precision of their numeric fields. */
		bool operator==(const SplineProfileSet& other) const noexcept
		{
			return Alternatives == other.Alternatives;
		}
	};
}
