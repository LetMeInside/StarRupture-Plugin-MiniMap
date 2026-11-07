#if defined(MODLOADER_CLIENT_BUILD)

// Implements Section identity, membership observation tracking, private
// completed-image storage, and lazy ownership of numerical rendering context.
#include "BuildingMapSection.h"

#include "Visuals/BuildingVisualsContext.h"

#include <limits>
#include <memory>
#include <utility>

namespace Services::Map::Buildings
{
	/** Stores the fixed integer coordinate assigned by the owning service. */
	BuildingMapSection::BuildingMapSection(std::int64_t gridX, std::int64_t gridY)
		: gridX_(gridX)
		, gridY_(gridY)
	{
	}

	/** Destroys the Section-owned numerical context with its service-owned Section. */
	BuildingMapSection::~BuildingMapSection() = default;

	/** Returns this section's world-grid X coordinate. */
	std::int64_t BuildingMapSection::GetGridX() const
	{
		return gridX_;
	}

	/** Returns this section's world-grid Y coordinate. */
	std::int64_t BuildingMapSection::GetGridY() const
	{
		return gridY_;
	}

	/** Reports whether the most recent observation certifies current membership. */
	bool BuildingMapSection::HasCurrentMembershipObservation() const
	{
		return hasCurrentMembershipObservation_;
	}

	/** Returns the current fingerprint only while a complete observation is certified. */
	bool BuildingMapSection::TryGetCurrentMembershipFingerprint(std::uint64_t& fingerprint) const
	{
		if (!hasCurrentMembershipObservation_)
		{
			return false;
		}
		fingerprint = membershipFingerprint_;
		return true;
	}

	/** Tracks complete observations and reports first certification or fingerprint changes. */
	bool BuildingMapSection::RecordMembershipObservation(
		bool isComplete,
		std::uint64_t membershipFingerprint)
	{
		if (!isComplete)
		{
			hasCurrentMembershipObservation_ = false;
			return false;
		}
		const bool fingerprintChanged = !hasPreviousMembershipFingerprint_ ||
			membershipFingerprint_ != membershipFingerprint;
		membershipFingerprint_ = membershipFingerprint;
		hasPreviousMembershipFingerprint_ = true;
		hasCurrentMembershipObservation_ = true;
		return fingerprintChanged;
	}

	/** Reports whether this Section currently owns a completed image. */
	bool BuildingMapSection::HasCompletedImage() const
	{
		return completedImage_.has_value();
	}

	/** Borrows immutable metadata and bytes from the stored image when available. */
	bool BuildingMapSection::TryBorrowCompletedImage(CompletedImageView& image) const
	{
		image = {};
		if (!completedImage_)
		{
			return false;
		}
		image.PixelWidth = completedImage_->PixelWidth;
		image.PixelHeight = completedImage_->PixelHeight;
		image.Rgba16fBytes = completedImage_->Rgba16fBytes;
		return true;
	}

	/** Releases the image without changing Section identity or numerical context. */
	void BuildingMapSection::InvalidateCompletedImage()
	{
		completedImage_.reset();
	}

	/** Validates image dimensions and replaces storage by moving the byte vector. */
	bool BuildingMapSection::TryAdoptCompletedImage(
		std::uint32_t pixelWidth,
		std::uint32_t pixelHeight,
		std::vector<std::uint8_t>&& rgba16fBytes)
	{
		std::size_t expectedByteCount = 0;
		if (!TryGetCompletedImageByteCount(pixelWidth, pixelHeight, expectedByteCount))
		{
			return false;
		}
		if (rgba16fBytes.size() != expectedByteCount)
		{
			return false;
		}

		CompletedImage replacement;
		replacement.PixelWidth = pixelWidth;
		replacement.PixelHeight = pixelHeight;
		replacement.Rgba16fBytes = std::move(rgba16fBytes);
		completedImage_ = std::move(replacement);
		return true;
	}

	/** Computes the exact byte count while rejecting zero dimensions and overflow. */
	bool BuildingMapSection::TryGetCompletedImageByteCount(
		std::uint32_t pixelWidth,
		std::uint32_t pixelHeight,
		std::size_t& byteCount)
	{
		if (pixelWidth == 0 || pixelHeight == 0)
		{
			return false;
		}

		const std::size_t width = static_cast<std::size_t>(pixelWidth);
		const std::size_t height = static_cast<std::size_t>(pixelHeight);
		const std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
		if (width > maximum / height)
		{
			return false;
		}

		const std::size_t pixelCount = width * height;
		constexpr std::size_t bytesPerPixel = 8;
		if (pixelCount > maximum / bytesPerPixel)
		{
			return false;
		}
		byteCount = pixelCount * bytesPerPixel;
		return true;
	}

	/** Lazily creates and returns this Section's exclusively owned numerical context. */
	Visuals::BuildingVisualsContext& BuildingMapSection::GetOrCreateVisualsContext()
	{
		if (!visualsContext_)
		{
			visualsContext_ = std::make_unique<Visuals::BuildingVisualsContext>();
		}
		return *visualsContext_;
	}
}

#endif // MODLOADER_CLIENT_BUILD
