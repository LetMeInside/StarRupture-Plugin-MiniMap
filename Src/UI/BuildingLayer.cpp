#if defined(MODLOADER_CLIENT_BUILD)

// Owns one V2 viewport and prepares coalesced CPU-side RGBA8 Section images.
#include "BuildingLayer.h"

#include "../Services/Map/Buildings/BuildingMapSection.h"
#include "../Services/Map/Buildings/BuildingMapViewport.h"

#include <cmath>
#include <limits>
#include <utility>

namespace MiniMapUI
{
	/** Creates an unattached CPU presentation layer. */
	BuildingLayer::BuildingLayer() = default;

	/** Removes the callback before the owned viewport closes. */
	BuildingLayer::~BuildingLayer()
	{
		SetViewport(nullptr);
	}

	/** Replaces the viewport subscription and discovers its current cells. */
	void BuildingLayer::SetViewport(
		std::unique_ptr<Services::Map::Buildings::BuildingMapViewport> viewport)
	{
		if (viewport_ != nullptr)
		{
			viewport_->SetSectionImageChangedHandler({});
		}
		dirtyCoordinates_.clear();
		pendingUpdates_.clear();
		lastConversionFailureCoordinate_.reset();
		viewport_.reset();
		if (viewport == nullptr)
		{
			return;
		}
		viewport_ = std::move(viewport);
		viewport_->SetSectionImageChangedHandler(
			[this](std::int64_t gridX, std::int64_t gridY)
			{
				dirtyCoordinates_.emplace(gridX, gridY);
			});
		MarkVisibleCoordinatesDirty();
	}

	/** Updates the rectangle and reconciles CPU work only after acceptance. */
	bool BuildingLayer::SetRectangle(
		double xMapUnits,
		double yMapUnits,
		double widthMapUnits,
		double heightMapUnits)
	{
		if (viewport_ == nullptr)
		{
			return false;
		}
		const std::set<Coordinate> previousCoordinates = CollectVisibleCoordinates();
		if (!viewport_->SetRectangle(xMapUnits, yMapUnits, widthMapUnits, heightMapUnits))
		{
			return false;
		}
		const std::set<Coordinate> visibleCoordinates = CollectVisibleCoordinates();
		for (const Coordinate& coordinate : visibleCoordinates)
		{
			if (previousCoordinates.find(coordinate) == previousCoordinates.end())
			{
				dirtyCoordinates_.insert(coordinate);
			}
		}
		DiscardCoordinatesOutside(visibleCoordinates);
		return true;
	}

	/** Converts each accumulated coordinate against the viewport's latest image state. */
	void BuildingLayer::PrepareCpuImages()
	{
		if (viewport_ == nullptr)
		{
			return;
		}
		while (!dirtyCoordinates_.empty())
		{
			const Coordinate coordinate = *dirtyCoordinates_.begin();
			dirtyCoordinates_.erase(dirtyCoordinates_.begin());
			PrepareCoordinate(coordinate);
		}
	}

	/** Enumerates current local cells into signed global coordinates. */
	std::set<BuildingLayer::Coordinate> BuildingLayer::CollectVisibleCoordinates() const
	{
		std::set<Coordinate> coordinates;
		if (viewport_ == nullptr)
		{
			return coordinates;
		}
		const std::size_t width = viewport_->GetVisibleGridWidth();
		const std::size_t height = viewport_->GetVisibleGridHeight();
		for (std::size_t localY = 0; localY < height; ++localY)
		{
			for (std::size_t localX = 0; localX < width; ++localX)
			{
				std::int64_t gridX = 0;
				std::int64_t gridY = 0;
				if (viewport_->GetWorldGridCoordinate(localX, localY, gridX, gridY))
				{
					coordinates.emplace(gridX, gridY);
				}
			}
		}
		return coordinates;
	}

	/** Marks every local viewport cell for initial or replacement discovery. */
	void BuildingLayer::MarkVisibleCoordinatesDirty()
	{
		if (viewport_ == nullptr)
		{
			return;
		}
		const std::size_t width = viewport_->GetVisibleGridWidth();
		const std::size_t height = viewport_->GetVisibleGridHeight();
		for (std::size_t localY = 0; localY < height; ++localY)
		{
			for (std::size_t localX = 0; localX < width; ++localX)
			{
				std::int64_t gridX = 0;
				std::int64_t gridY = 0;
				if (viewport_->GetWorldGridCoordinate(localX, localY, gridX, gridY))
				{
					dirtyCoordinates_.emplace(gridX, gridY);
				}
			}
		}
	}

	/** Resolves one dirty coordinate and records its current image or absence. */
	void BuildingLayer::PrepareCoordinate(const Coordinate& coordinate)
	{
		std::size_t localX = 0;
		std::size_t localY = 0;
		if (!TryFindLocalCoordinate(coordinate, localX, localY))
		{
			pendingUpdates_.erase(coordinate);
			return;
		}
		const Services::Map::Buildings::BuildingMapSection* section =
			viewport_->GetSectionAt(localX, localY);
		if (section == nullptr)
		{
			RecordImageRemoval(coordinate);
			return;
		}
		Services::Map::Buildings::BuildingMapSection::CompletedImageView image;
		if (!section->TryBorrowCompletedImage(image))
		{
			RecordImageRemoval(coordinate);
			return;
		}
		PendingPresentationUpdate replacement;
		if (!TryConvertCompletedImage(
			image.PixelWidth,
			image.PixelHeight,
			image.Rgba16fBytes,
			replacement))
		{
			pendingUpdates_.erase(coordinate);
			lastConversionFailureCoordinate_ = coordinate;
			return;
		}
		pendingUpdates_.insert_or_assign(coordinate, std::move(replacement));
		ClearConversionFailureFor(coordinate);
	}

	/** Records image absence without treating it as a conversion failure. */
	void BuildingLayer::RecordImageRemoval(const Coordinate& coordinate)
	{
		PendingPresentationUpdate removal;
		pendingUpdates_.insert_or_assign(coordinate, std::move(removal));
		ClearConversionFailureFor(coordinate);
	}

	/** Clears the diagnostic marker only when the same coordinate was resolved. */
	void BuildingLayer::ClearConversionFailureFor(const Coordinate& coordinate)
	{
		if (lastConversionFailureCoordinate_ == coordinate)
		{
			lastConversionFailureCoordinate_.reset();
		}
	}

	/** Maps a signed grid coordinate to a local offset in the contiguous viewport range. */
	bool BuildingLayer::TryFindLocalCoordinate(
		const Coordinate& coordinate,
		std::size_t& localX,
		std::size_t& localY) const
	{
		std::int64_t firstGridX = 0;
		std::int64_t firstGridY = 0;
		if (viewport_ == nullptr ||
			!viewport_->GetWorldGridCoordinate(0, 0, firstGridX, firstGridY) ||
			coordinate.first < firstGridX || coordinate.second < firstGridY)
		{
			return false;
		}
		const std::uint64_t offsetX = static_cast<std::uint64_t>(coordinate.first) -
			static_cast<std::uint64_t>(firstGridX);
		const std::uint64_t offsetY = static_cast<std::uint64_t>(coordinate.second) -
			static_cast<std::uint64_t>(firstGridY);
		if (offsetX >= viewport_->GetVisibleGridWidth() || offsetY >= viewport_->GetVisibleGridHeight())
		{
			return false;
		}
		localX = static_cast<std::size_t>(offsetX);
		localY = static_cast<std::size_t>(offsetY);
		return true;
	}

	/** Erases coalesced work for cells that left the accepted viewport range. */
	void BuildingLayer::DiscardCoordinatesOutside(
		const std::set<Coordinate>& visibleCoordinates)
	{
		for (auto iterator = dirtyCoordinates_.begin(); iterator != dirtyCoordinates_.end();)
		{
			if (visibleCoordinates.find(*iterator) == visibleCoordinates.end())
			{
				iterator = dirtyCoordinates_.erase(iterator);
			}
			else
			{
				++iterator;
			}
		}
		for (auto iterator = pendingUpdates_.begin(); iterator != pendingUpdates_.end();)
		{
			if (visibleCoordinates.find(iterator->first) == visibleCoordinates.end())
			{
				iterator = pendingUpdates_.erase(iterator);
			}
			else
			{
				++iterator;
			}
		}
		if (lastConversionFailureCoordinate_.has_value() &&
			visibleCoordinates.find(*lastConversionFailureCoordinate_) == visibleCoordinates.end())
		{
			lastConversionFailureCoordinate_.reset();
		}
	}

	/** Validates dimensions and converts one complete image without retaining its source bytes. */
	bool BuildingLayer::TryConvertCompletedImage(
		std::uint32_t pixelWidth,
		std::uint32_t pixelHeight,
		std::span<const std::uint8_t> rgba16fBytes,
		PendingPresentationUpdate& update)
	{
		update = {};
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
		if (pixelCount > maximum / 8 || rgba16fBytes.size() != pixelCount * 8)
		{
			return false;
		}
		update.Rgba8Bytes.resize(pixelCount * 4);
		for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
		{
			const std::size_t sourceOffset = pixelIndex * 8;
			const std::size_t targetOffset = pixelIndex * 4;
			if (!TryConvertPixel(rgba16fBytes, sourceOffset, update.Rgba8Bytes.data() + targetOffset))
			{
				update = {};
				return false;
			}
		}
		update.Kind = PendingUpdateKind::Replacement;
		update.PixelWidth = pixelWidth;
		update.PixelHeight = pixelHeight;
		return true;
	}

	/** Decodes one finite RGBA16F sample and writes straight-alpha display RGBA8. */
	bool BuildingLayer::TryConvertPixel(
		std::span<const std::uint8_t> rgba16fBytes,
		std::size_t byteOffset,
		std::uint8_t* rgba8Pixel)
	{
		double red = 0.0;
		double green = 0.0;
		double blue = 0.0;
		double transmittance = 0.0;
		if (!TryDecodeBinary16(rgba16fBytes, byteOffset, red) ||
			!TryDecodeBinary16(rgba16fBytes, byteOffset + 2, green) ||
			!TryDecodeBinary16(rgba16fBytes, byteOffset + 4, blue) ||
			!TryDecodeBinary16(rgba16fBytes, byteOffset + 6, transmittance))
		{
			return false;
		}
		const double opacity = ClampUnit(1.0 - transmittance);
		if (opacity == 0.0)
		{
			rgba8Pixel[0] = 0;
			rgba8Pixel[1] = 0;
			rgba8Pixel[2] = 0;
			rgba8Pixel[3] = 0;
			return true;
		}
		rgba8Pixel[0] = ToRgba8(EncodeSrgb(ClampUnit(red / opacity)));
		rgba8Pixel[1] = ToRgba8(EncodeSrgb(ClampUnit(green / opacity)));
		rgba8Pixel[2] = ToRgba8(EncodeSrgb(ClampUnit(blue / opacity)));
		rgba8Pixel[3] = ToRgba8(opacity);
		return true;
	}

	/** Decodes little-endian IEEE-754 binary16 including subnormal and signed-zero values. */
	bool BuildingLayer::TryDecodeBinary16(
		std::span<const std::uint8_t> bytes,
		std::size_t byteOffset,
		double& value) noexcept
	{
		value = 0.0;
		if (byteOffset >= bytes.size() || bytes.size() - byteOffset < 2)
		{
			return false;
		}
		const std::uint16_t bits = static_cast<std::uint16_t>(bytes[byteOffset]) |
			static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[byteOffset + 1]) << 8);
		const std::uint16_t exponent = static_cast<std::uint16_t>((bits >> 10) & 0x1f);
		const std::uint16_t fraction = static_cast<std::uint16_t>(bits & 0x03ff);
		if (exponent == 0x1f)
		{
			return false;
		}
		const double sign = (bits & 0x8000) == 0 ? 1.0 : -1.0;
		if (exponent == 0)
		{
			value = sign * std::ldexp(static_cast<double>(fraction), -24);
		}
		else
		{
			value = sign * std::ldexp(1024.0 + static_cast<double>(fraction), static_cast<int>(exponent) - 25);
		}
		return std::isfinite(value);
	}

	/** Clamps finite opacity or color values to the normalized display interval. */
	double BuildingLayer::ClampUnit(double value) noexcept
	{
		return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
	}

	/** Applies the standard piecewise linear-to-sRGB display transfer function. */
	double BuildingLayer::EncodeSrgb(double linearComponent) noexcept
	{
		if (linearComponent <= 0.0031308)
		{
			return 12.92 * linearComponent;
		}
		return 1.055 * std::pow(linearComponent, 1.0 / 2.4) - 0.055;
	}

	/** Rounds a normalized channel to the nearest representable RGBA8 byte. */
	std::uint8_t BuildingLayer::ToRgba8(double normalizedComponent) noexcept
	{
		return static_cast<std::uint8_t>(std::lround(ClampUnit(normalizedComponent) * 255.0));
	}
}

#endif // MODLOADER_CLIENT_BUILD
