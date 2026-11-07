// Represents one service-owned cell in the shared sparse Building Map grid and
// stores the latest certified source-membership observation and at most one
// privately owned completed numerical image.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Services::Map::Buildings::Scheduling
{
	class SectionScheduler;
}

namespace Services::Map::Buildings::Visuals
{
	class BuildingVisualsContext;
}

namespace Services::Map::Buildings
{
	/**
	 * Identifies one fixed service-owned map cell and retains its private numerical
	 * rendering context for the Section lifetime. X columns and Y rows increase
	 * rightward and downward; the service remains the Section's lifetime owner.
	 */
	class BuildingMapSection
	{
	public:
		/** Releases Section-owned rendering state when this service-owned Section is destroyed. */
		~BuildingMapSection();

		/** Returns the integer world-grid coordinate on the X axis. */
		std::int64_t GetGridX() const;

		/** Returns the integer world-grid coordinate on the Y axis. */
		std::int64_t GetGridY() const;

		/** Returns true when the section has a currently certified membership observation. */
		bool HasCurrentMembershipObservation() const;

		/**
		 * Copies the current certified fingerprint to the output when available.
		 * Returns false without changing the output when membership is uncertified.
		 *
		 * @param fingerprint Receives the valid fingerprint, including a possible zero value.
		 * @return True only while a complete observation is currently certified.
		 */
		bool TryGetCurrentMembershipFingerprint(std::uint64_t& fingerprint) const;

	private:
		friend class BuildingMapService;
		friend class Scheduling::SectionScheduler;

		/** Creates a section for the service-owned grid coordinate. */
		BuildingMapSection(std::int64_t gridX, std::int64_t gridY);

		/**
		 * Owns one finalized row-major RGBA16F image for this Section. Image
		 * presence represents availability; bytes are transferred by move. Each
		 * pixel contains four little-endian binary16 channels: associated linear
		 * RGB and transmittance.
		 */
		struct CompletedImage
		{
			std::uint32_t PixelWidth = 0;
			std::uint32_t PixelHeight = 0;
			std::vector<std::uint8_t> Rgba16fBytes;
		};

		/**
		 * Borrows immutable bytes from the currently stored completed image.
		 * The span remains valid only while this Section and its stored image
		 * remain alive and unchanged.
		 */
		struct CompletedImageView
		{
			std::uint32_t PixelWidth = 0;
			std::uint32_t PixelHeight = 0;
			std::span<const std::uint8_t> Rgba16fBytes;
		};

		/**
		 * Records a complete fingerprint or revokes current certification when incomplete.
		 *
		 * @param isComplete Whether the collector certified complete source coverage.
		 * @param membershipFingerprint Fingerprint to retain when isComplete is true.
		 * @return True for the first complete fingerprint or a change from the previous one.
		 */
		bool RecordMembershipObservation(bool isComplete, std::uint64_t membershipFingerprint);

		/** Reports whether this Section currently owns a completed image. */
		bool HasCompletedImage() const;

		/**
		 * Borrows the current image metadata and bytes without copying pixels.
		 * The view remains valid only while the Section and image remain alive
		 * and unchanged.
		 *
		 * @param image Receives an immutable borrowed view when available.
		 * @return True only when a completed image is stored.
		 */
		bool TryBorrowCompletedImage(CompletedImageView& image) const;

		/** Releases only the completed image, preserving this Section and its rendering context. */
		void InvalidateCompletedImage();

		/**
		 * Validates shape and moves a completed RGBA16F image into this Section.
		 * Rejection leaves both existing storage and the supplied bytes unchanged.
		 * This validates shape only, not source or dependency freshness.
		 *
		 * @param pixelWidth Image width in pixels.
		 * @param pixelHeight Image height in pixels.
		 * @param rgba16fBytes Finalized row-major little-endian RGBA16F bytes to move.
		 * @return True when the validated image replaces the currently stored image.
		 */
		bool TryAdoptCompletedImage(
			std::uint32_t pixelWidth,
			std::uint32_t pixelHeight,
			std::vector<std::uint8_t>&& rgba16fBytes);

		/**
		 * Computes the exact RGBA16F byte count after rejecting zero dimensions
		 * and size arithmetic overflow.
		 *
		 * @param pixelWidth Image width in pixels.
		 * @param pixelHeight Image height in pixels.
		 * @param byteCount Receives the required byte count on success.
		 * @return True only when the dimensions produce a safe nonzero byte count.
		 */
		static bool TryGetCompletedImageByteCount(
			std::uint32_t pixelWidth,
			std::uint32_t pixelHeight,
			std::size_t& byteCount);

		/**
		 * Returns this Section's private rendering context, allocating it on the
		 * first explicit request. The returned reference is borrowed and remains
		 * valid only while this Section and its owned context remain alive.
		 *
		 * @return The context exclusively owned by this Section.
		 */
		Visuals::BuildingVisualsContext& GetOrCreateVisualsContext();

		std::int64_t gridX_;
		std::int64_t gridY_;
		bool hasCurrentMembershipObservation_ = false;
		bool hasPreviousMembershipFingerprint_ = false;
		std::uint64_t membershipFingerprint_ = 0;
		std::unique_ptr<Visuals::BuildingVisualsContext> visualsContext_;
		std::optional<CompletedImage> completedImage_;
	};
}
