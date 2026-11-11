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
	class SectionSchedulingCoordinator;
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

		/**
		 * Describes the actual dimensions and borrowed immutable bytes of a stored
		 * completed RGBA16F Section image. The Section owns the storage; this view
		 * does not extend its lifetime and is usable only under the borrowing
		 * restrictions documented by TryBorrowCompletedImage.
		 */
		struct CompletedImageView
		{
			std::uint32_t PixelWidth = 0;
			std::uint32_t PixelHeight = 0;
			std::span<const std::uint8_t> Rgba16fBytes;
		};

		/**
		 * Borrows immutable metadata and RGBA16F bytes from the completed image.
		 * The borrowed span is valid only while this Section remains alive, its
		 * completed image remains unchanged, and the service's serialized
		 * calling-thread contract is respected. Consume the bytes synchronously;
		 * do not retain the span across image replacement or removal, residency
		 * eviction, service destruction, or any other image-ownership mutation.
		 * A caller may immediately convert the bytes into independently owned
		 * presentation storage.
		 *
		 * @param image Receives actual pixel dimensions and a const borrowed byte span.
		 *               It is reset to an empty view when no completed image exists.
		 * @return True only when a completed image is stored.
		 */
		bool TryBorrowCompletedImage(CompletedImageView& image) const;

	private:
		friend class BuildingMapService;
		friend class Scheduling::SectionSchedulingCoordinator;
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
		 * Records a complete fingerprint or revokes current certification when incomplete.
		 *
		 * @param isComplete Whether the collector certified complete source coverage.
		 * @param membershipFingerprint Fingerprint to retain when isComplete is true.
		 * @return True for the first complete fingerprint or a change from the previous one.
		 */
		bool RecordMembershipObservation(bool isComplete, std::uint64_t membershipFingerprint);

		/** Reports whether this Section currently owns a completed image. */
		bool HasCompletedImage() const;

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
		/** Releases only unfinished scheduler state while preserving the numerical context and committed image. */
		void ResetRenderingState() noexcept;

		std::int64_t gridX_;
		std::int64_t gridY_;
		bool hasCurrentMembershipObservation_ = false;
		bool hasPreviousMembershipFingerprint_ = false;
		std::uint64_t membershipFingerprint_ = 0;
		std::unique_ptr<Visuals::BuildingVisualsContext> visualsContext_;
		/** Opaque scheduler state owned only for this Section's unfinished render generation. */
		std::shared_ptr<void> renderingState_;
		std::optional<CompletedImage> completedImage_;
	};
}
