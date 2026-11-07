// Owns V2 viewport presentation state, converts Section images, publishes GPU
// texture receipts, retires resources and submits rotated Section quads.
#pragma once

#include "../Services/Textures.h"
#include "../Services/Viewport.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace Services::Map::Buildings
{
	class BuildingMapViewport;
}

namespace MiniMap::Services
{
	class ICanvas;
}

namespace MiniMapMap
{
	struct Transform;
}

namespace MiniMapUI
{
	/**
	 * Owns one caller-created V2 viewport, CPU image updates and validated texture
	 * receipts. The caller owns the service, collector, texture service and world
	 * session; those owners must outlive this layer's viewport and resources.
	 * Non-render operations use the serialized game thread. Render accesses only
	 * the mutex-protected texture publication and supplied transform.
	 */
	class BuildingLayer final
	{
	public:
		/**
		 * Removes the callback and closes the viewport without backend resource calls.
		 * The caller must complete Shutdown and keep this layer alive while it returns false.
		 */
		~BuildingLayer();

		/** Prevents copies from duplicating ownership of one viewport subscription. */
		BuildingLayer(const BuildingLayer&) = delete;
		/** Prevents copies from duplicating ownership of one viewport subscription. */
		BuildingLayer& operator=(const BuildingLayer&) = delete;
		/** Keeps the callback's captured this pointer stable for the viewport lifetime. */
		BuildingLayer(BuildingLayer&&) = delete;
		/** Keeps the callback's captured this pointer stable for the viewport lifetime. */
		BuildingLayer& operator=(BuildingLayer&&) = delete;

		/**
		 * Creates a layer using caller-owned texture service and configured Section size.
		 * @param textures Texture service that must remain alive through resource shutdown.
		 * @param sectionSizeMapUnits Positive service-configured width and height of each Section.
		 * @throws std::invalid_argument when the Section size is nonfinite or nonpositive.
		 */
		BuildingLayer(MiniMap::Services::ITextures& textures, double sectionSizeMapUnits);

		/**
		 * Replaces the owned viewport, clearing prior CPU updates and discovering the
		 * new viewport's existing Sections. A null viewport detaches the current one;
		 * after Shutdown begins, attachment requests are ignored.
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

		/**
		 * Uploads each pending coordinate at most once, publishes validated receipts,
		 * then releases withdrawn receipts through the texture backend.
		 * Must run on the texture service's permitted game thread outside rendering.
		 */
		void PublishGpuImages();

		/**
		 * Draws the current coherent texture publication as clipped rotated Section quads.
		 * Must run only from the supplied synchronous UI render callback.
		 *
		 * @param ui Callback-scoped canvas used only for clipping and quad submission.
		 * @param transform Current map-to-screen transform supplied by MiniMapMap.
		 * @param showLayer False suppresses all BuildingLayer drawing.
		 */
		void Render(
			MiniMap::Services::ICanvas& ui,
			const MiniMapMap::Transform& transform,
			bool showLayer);

		/**
		 * Stops publication and drawing, closes the viewport, withdraws entries and
		 * attempts backend releases. Repeat after UnavailableRetained; keep this layer
		 * and the texture service alive while any receipt or operation remains.
		 *
		 * @return True when no published entry, receipt or backend operation remains.
		 */
		bool Shutdown();

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
			std::uint64_t Revision = 0;
			std::vector<std::uint8_t> Rgba8Bytes;
		};

		/** Tracks whether a receipt is published, awaiting release, or unresolved. */
		enum class TextureResourceState
		{
			Acquiring,
			Published,
			PendingRelease,
			Releasing,
			Unresolved
		};

		/** Owns one move-only receipt; its Texture value is only a render-facing identity. */
		struct TextureEntry
		{
			Coordinate GridCoordinate{};
			MiniMap::Services::OwnedTexture Owner;
			MiniMap::Services::Texture DrawIdentity{};
			TextureResourceState State = TextureResourceState::Acquiring;
		};

		/**
		 * Enumerates each current viewport-local cell as a signed global coordinate.
		 * @return A temporary ordered set, empty when no viewport is attached.
		 */
		std::set<Coordinate> CollectVisibleCoordinates() const;

		/** Adds every current viewport coordinate to the coalesced dirty set. */
		void MarkVisibleCoordinatesDirty();

		/** Stores a newer owned CPU update and assigns its supersession revision. */
		void StorePendingUpdate(const Coordinate& coordinate, PendingPresentationUpdate update);

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

		/** Withdraws all draw identities while retaining their owned receipts for release. */
		void WithdrawAllPublishedTextures();

		/** Withdraws published entries outside the accepted visible coordinate set. */
		void WithdrawDepartedTextures(const std::set<Coordinate>& visibleCoordinates);

		/** Withdraws one published coordinate and retains its receipt for later release. */
		void WithdrawPublishedTexture(const Coordinate& coordinate);

		/** Releases withdrawn receipts outside the mutex and retains unresolved ownership. */
		void ReleasePendingTextures();

		/** Processes one snapshot of a pending CPU update without repeating a coordinate. */
		void PublishPendingUpdate(const Coordinate& coordinate, std::uint64_t revision);

		/** Uploads one replacement without holding the publication mutex. */
		void UploadPendingReplacement(
			const Coordinate& coordinate,
			PendingPresentationUpdate update);

		/** Revalidates an upload result and either publishes, retains or retries its CPU data. */
		void HandleUploadResult(
			const Coordinate& coordinate,
			PendingPresentationUpdate update,
			MiniMap::Services::UploadState result,
			TextureEntry& entry);

		/** Checks receipt identity and dimensions before a texture can enter the draw map. */
		static bool IsUsableTextureReceipt(
			const MiniMap::Services::OwnedTexture& owner,
			MiniMap::Services::TextureSize expectedSize) noexcept;

		/** Checks whether upload work is still current, visible and not superseded. */
		bool IsCurrentUploadEligible(const Coordinate& coordinate) const;

		/** Restores retryable CPU image data when no newer update has superseded it. */
		void RestorePendingUpdateIfEligible(
			const Coordinate& coordinate,
			PendingPresentationUpdate update);

		/** Publishes a validated stored receipt and withdraws its previous texture atomically. */
		bool PublishValidatedTexture(
			const Coordinate& coordinate,
			MiniMap::Services::TextureSize size,
			TextureEntry& entry);

		/** Completes explicit viewport and CPU-state shutdown without backend calls. */
		void DetachViewportAndClearCpuState();

		/** Tests whether publications, receipts and active backend operations are all discharged. */
		bool HasCompletedResourceShutdown() const;

		/** Calculates checked map-space bounds for one signed Section coordinate. */
		static bool TryGetSectionBounds(
			const Coordinate& coordinate,
			double sectionSizeMapUnits,
			double& left,
			double& top,
			double& right,
			double& bottom) noexcept;

		/** Projects the four Section corners into full-image screen-space quad order. */
		static bool TryProjectSectionQuad(
			const Coordinate& coordinate,
			double sectionSizeMapUnits,
			const MiniMapMap::Transform& transform,
			MiniMap::Services::Quad& positions) noexcept;

		/** Computes the visible transform rectangle used for canvas clipping. */
		static bool TryGetClipRectangle(
			const MiniMapMap::Transform& transform,
			MiniMap::Services::Vec2& clipMinimum,
			MiniMap::Services::Vec2& clipMaximum) noexcept;

		/** Rejects projected quads whose screen bounds miss the visible rectangle. */
		static bool IntersectsClipRectangle(
			const MiniMap::Services::Quad& positions,
			MiniMap::Services::Vec2 clipMinimum,
			MiniMap::Services::Vec2 clipMaximum) noexcept;

		MiniMap::Services::ITextures* textures_;
		double sectionSizeMapUnits_;
		std::unique_ptr<Services::Map::Buildings::BuildingMapViewport> viewport_;
		std::set<Coordinate> dirtyCoordinates_;
		std::map<Coordinate, PendingPresentationUpdate> pendingUpdates_;
		std::uint64_t nextPendingRevision_ = 1;
		/** Retains only the most recent malformed/nonfinite coordinate for diagnostics. */
		std::optional<Coordinate> lastConversionFailureCoordinate_;
		mutable std::mutex presentationMutex_;
		std::map<Coordinate, TextureEntry*> publishedTextures_;
		std::list<TextureEntry> textureResources_;
		std::size_t activeTextureOperations_ = 0;
		bool shutdownRequested_ = false;
		bool publishPassActive_ = false;
	};
}
