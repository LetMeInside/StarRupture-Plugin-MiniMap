// Owns one V2 Building Map viewport and prepares coalesced CPU presentation
// updates without creating or managing GPU textures.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace Services::Map::Buildings
{
	class BuildingMapViewport;
}

namespace MiniMapUI
{
	/**
	 * Owns one caller-created V2 viewport and converts its completed Section images
	 * into independently owned RGBA8 pending updates. All operations run on the
	 * service's serialized calling thread. The service and associated collector
	 * remain caller-owned and must outlive this layer's attached viewport.
	 */
	class BuildingLayer final
	{
	public:
		/** Releases the image-change handler and closes the owned viewport. */
		~BuildingLayer();

		/** Prevents copies from duplicating ownership of one viewport subscription. */
		BuildingLayer(const BuildingLayer&) = delete;
		/** Prevents copies from duplicating ownership of one viewport subscription. */
		BuildingLayer& operator=(const BuildingLayer&) = delete;
		/** Keeps the callback's captured this pointer stable for the viewport lifetime. */
		BuildingLayer(BuildingLayer&&) = delete;
		/** Keeps the callback's captured this pointer stable for the viewport lifetime. */
		BuildingLayer& operator=(BuildingLayer&&) = delete;

		/** Creates an unattached CPU presentation layer. */
		BuildingLayer();

		/**
		 * Replaces the owned viewport, clearing prior CPU updates and discovering the
		 * new viewport's existing Sections. A null viewport detaches the current one.
		 * The caller remains responsible for keeping the service and collector alive.
		 *
		 * @param viewport Viewport previously created by its caller-owned service.
		 */
		void SetViewport(std::unique_ptr<Services::Map::Buildings::BuildingMapViewport> viewport);

		/**
		 * Applies a complete rectangle in the attached service's map units. Accepted
		 * changes discover newly entered grid cells and discard departed CPU updates.
		 * Rejection preserves all dirty and pending presentation work.
		 *
		 * @param xMapUnits New upper-left X coordinate.
		 * @param yMapUnits New upper-left Y coordinate.
		 * @param widthMapUnits New positive rectangle width.
		 * @param heightMapUnits New positive rectangle height.
		 * @return True when the attached viewport accepts and stores the rectangle.
		 */
		bool SetRectangle(
			double xMapUnits,
			double yMapUnits,
			double widthMapUnits,
			double heightMapUnits);

		/**
		 * Resolves dirty current Sections and records owned RGBA8 replacements or
		 * removals. Borrowed Section bytes are consumed synchronously; this operation
		 * does not schedule work, acquire sources, upload textures, or draw.
		 */
		void PrepareCpuImages();

	private:
		using Coordinate = std::pair<std::int64_t, std::int64_t>;

		/** Distinguishes a prepared image replacement from absent-image removal. */
		enum class PendingUpdateKind
		{
			Replacement,
			Removal
		};

		/** Owns one pending RGBA8 image or represents removal of its prior presentation. */
		struct PendingPresentationUpdate
		{
			PendingUpdateKind Kind = PendingUpdateKind::Removal;
			std::uint32_t PixelWidth = 0;
			std::uint32_t PixelHeight = 0;
			std::vector<std::uint8_t> Rgba8Bytes;
		};

		/**
		 * Enumerates each current viewport-local cell as a signed global coordinate.
		 * @return A temporary ordered set, empty when no viewport is attached.
		 */
		std::set<Coordinate> CollectVisibleCoordinates() const;

		/** Adds every current viewport coordinate to the coalesced dirty set. */
		void MarkVisibleCoordinatesDirty();

		/**
		 * Resolves one coordinate against the attached viewport and stores its latest
		 * replacement, removal, or conversion-failure state.
		 * @param coordinate Signed global Section column and row.
		 */
		void PrepareCoordinate(const Coordinate& coordinate);

		/**
		 * Records current image absence as one coalesced pending removal.
		 * @param coordinate Signed global Section column and row.
		 */
		void RecordImageRemoval(const Coordinate& coordinate);

		/**
		 * Clears the failure marker only when the same coordinate has recovered.
		 * @param coordinate Signed global Section column and row just resolved.
		 */
		void ClearConversionFailureFor(const Coordinate& coordinate);

		/**
		 * Converts a signed grid coordinate to local offsets in the current rectangle.
		 * @param coordinate Signed global Section column and row to locate.
		 * @param localX Receives the rightward local column on success.
		 * @param localY Receives the downward local row on success.
		 * @return True only when the coordinate is covered by the attached viewport.
		 */
		bool TryFindLocalCoordinate(
			const Coordinate& coordinate,
			std::size_t& localX,
			std::size_t& localY) const;

		/**
		 * Removes dirty work, pending updates and stale diagnostics outside the range.
		 * @param visibleCoordinates Signed coordinates in the newly accepted range.
		 */
		void DiscardCoordinatesOutside(const std::set<Coordinate>& visibleCoordinates);

		/**
		 * Validates the complete borrowed RGBA16F payload and converts it into one
		 * owned RGBA8 replacement without retaining its source span.
		 * @param pixelWidth Actual source width in pixels.
		 * @param pixelHeight Actual source height in pixels.
		 * @param rgba16fBytes Borrowed row-major little-endian RGBA16F bytes.
		 * @param update Receives the owned replacement only on success.
		 * @return True only when dimensions, byte count and every pixel are valid.
		 */
		static bool TryConvertCompletedImage(
			std::uint32_t pixelWidth,
			std::uint32_t pixelHeight,
			std::span<const std::uint8_t> rgba16fBytes,
			PendingPresentationUpdate& update);

		/**
		 * Decodes one finite source pixel and writes straight-alpha display RGBA8.
		 * @param rgba16fBytes Borrowed complete source image bytes.
		 * @param byteOffset First byte of the source pixel.
		 * @param rgba8Pixel Receives four output bytes in R, G, B, A order.
		 * @return False when any source channel is nonfinite or truncated.
		 */
		static bool TryConvertPixel(
			std::span<const std::uint8_t> rgba16fBytes,
			std::size_t byteOffset,
			std::uint8_t* rgba8Pixel);

		/**
		 * Decodes one little-endian IEEE-754 binary16 value, including subnormals.
		 * @param bytes Borrowed bytes containing the encoded value.
		 * @param byteOffset Index of the value's first byte.
		 * @param value Receives the decoded finite value on success.
		 * @return False for an out-of-span value, infinity or NaN.
		 */
		static bool TryDecodeBinary16(
			std::span<const std::uint8_t> bytes,
			std::size_t byteOffset,
			double& value) noexcept;

		/**
		 * Clamps a finite component to the normalized display interval.
		 * @param value Finite opacity or color component.
		 * @return The value clamped to [0, 1].
		 */
		static double ClampUnit(double value) noexcept;

		/**
		 * Encodes one normalized linear RGB component with the standard sRGB curve.
		 * @param linearComponent Clamped linear-light color component.
		 * @return The display-encoded sRGB component.
		 */
		static double EncodeSrgb(double linearComponent) noexcept;

		/** Rounds one normalized display channel to an eight-bit value. */
		static std::uint8_t ToRgba8(double normalizedComponent) noexcept;

		std::unique_ptr<Services::Map::Buildings::BuildingMapViewport> viewport_;
		std::set<Coordinate> dirtyCoordinates_;
		std::map<Coordinate, PendingPresentationUpdate> pendingUpdates_;
		/** Retains only the most recent malformed/nonfinite coordinate for diagnostics. */
		std::optional<Coordinate> lastConversionFailureCoordinate_;
	};
}
