#if defined(MODLOADER_CLIENT_BUILD)

// Implements Section identity, membership observation tracking, private
// completed-image storage, and Section-owned rendering state and context.
#include "BuildingMapSection.h"

#include "Visuals/BuildingVisualsContext.h"

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
		image.Rgba8Bytes = completedImage_->Rgba8Bytes;
		return true;
	}

	/** Releases the image without changing Section identity or numerical context. */
	void BuildingMapSection::InvalidateCompletedImage()
	{
		completedImage_.reset();
	}

	/** Replaces completed storage by moving a trusted finalized RGBA8 byte vector. */
	void BuildingMapSection::AdoptCompletedImage(
		std::uint32_t pixelWidth,
		std::uint32_t pixelHeight,
		std::vector<std::uint8_t>&& rgba8Bytes)
	{
		CompletedImage replacement;
		replacement.PixelWidth = pixelWidth;
		replacement.PixelHeight = pixelHeight;
		replacement.Rgba8Bytes = std::move(rgba8Bytes);
		completedImage_ = std::move(replacement);
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

	/** Releases unfinished render state without changing the context or published image. */
	void BuildingMapSection::ResetRenderingState() noexcept
	{
		renderingState_.reset();
	}
}

#endif // MODLOADER_CLIENT_BUILD
