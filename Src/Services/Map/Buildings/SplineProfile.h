// Defines neutral authored spline profile values shared by acquisition and candidate publication.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

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
		bool operator==(const SplineProfileBounds&) const = default;
	};

	/** Stores authored component translation, Euler rotation in degrees, and scale. */
	struct SplineProfileTransform
	{
		std::array<double, 3> Translation{};
		std::array<double, 3> RotationDegrees{};
		std::array<double, 3> Scale{1.0, 1.0, 1.0};
		bool operator==(const SplineProfileTransform&) const = default;
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
		bool operator==(const SplineProfileCrossSection&) const = default;
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
		bool operator==(const SplineProfileAlternative&) const = default;
	};

	/** Owns profile alternatives in their authored selection order as immutable CPU-only values. */
	struct SplineProfileSet
	{
		std::vector<SplineProfileAlternative> Alternatives;
		bool operator==(const SplineProfileSet&) const = default;
	};
}
