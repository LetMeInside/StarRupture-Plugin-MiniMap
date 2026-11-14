// Defines the shared numeric equivalence used by Building Map source hashes and retained facts.
#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Services::Map::Buildings
{
	/** Defines six-decimal source equivalence without changing stored facts. */
	class SourceContentPrecision final
	{
	public:
		/** Returns stable bits for six-place rounding, signed zero, NaN and infinity. */
		static std::uint64_t CanonicalBits(double value) noexcept
		{
			// Below 2^33 the scaled magnitude stays within 2^53; above it binary64 spacing exceeds the quantum.
			if (std::isfinite(value) && (std::abs)(value) < 0x1p33)
			{
				value = std::round(value * 1000000.0) / 1000000.0;
			}
			if (value == 0.0)
			{
				value = 0.0;
			}
			if (std::isnan(value))
			{
				return 0x7ff8000000000000ULL;
			}
			return std::bit_cast<std::uint64_t>(value);
		}

		/** Tests whether two floating source values have identical canonical hash bits. */
		static bool Equal(double first, double second) noexcept
		{
			return CanonicalBits(first) == CanonicalBits(second);
		}

		/** Tests an ordered source array using the same equivalence as scalar source values. */
		template<std::size_t Size>
		static bool Equal(const std::array<double, Size>& first, const std::array<double, Size>& second) noexcept
		{
			for (std::size_t index = 0; index < Size; ++index)
			{
				if (!Equal(first[index], second[index]))
				{
					return false;
				}
			}
			return true;
		}

		/** Compares already-normalized quaternions while treating either sign as the same rotation. */
		static bool EqualQuaternion(const std::array<double, 4>& first, const std::array<double, 4>& second) noexcept
		{
			const bool negateFirst = QuaternionNeedsNegation(first);
			const bool negateSecond = QuaternionNeedsNegation(second);
			for (std::size_t index = 0; index < first.size(); ++index)
			{
				const double firstValue = negateFirst ? -first[index] : first[index];
				const double secondValue = negateSecond ? -second[index] : second[index];
				if (!Equal(firstValue, secondValue))
				{
					return false;
				}
			}
			return true;
		}

		/** Returns whether sign canonicalization must negate a quaternion in XYZW order. */
		static bool QuaternionNeedsNegation(const std::array<double, 4>& rotation) noexcept
		{
			constexpr std::array<std::size_t, 4> signCheckOrder{3, 0, 1, 2};
			for (std::size_t index : signCheckOrder)
			{
				if (rotation[index] != 0.0)
				{
					return std::signbit(rotation[index]);
				}
			}
			return false;
		}
	};
}
