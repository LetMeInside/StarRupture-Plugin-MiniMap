// Owns V2 viewport presentation state, converts Section images, publishes GPU
// texture receipts, retires resources and submits rotated Section quads.
#pragma once

#include "../Services/Textures.h"
#include "../Services/Viewport.h"

#include <cstddef>
#include <cstdint>
#if defined(MODLOADER_CLIENT_BUILD) && !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <array>
#include <chrono>
#endif
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
	class BuildingMapService;
	class BuildingMapViewport;
}

namespace MiniMap::BuildingAcquisition
{
	class BuildingCollectorV2;
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
	 * Owns one BuildingCollectorV2, its viewport, CPU image updates and validated
	 * texture receipts. The caller owns BuildingMapService and ITextures; both must
	 * outlive this layer's viewport and texture resources. It creates one viewport
	 * through the borrowed service using its owned collector. Native collector
	 * resources require explicit Shutdown before BuildingLayer destruction.
	 * Non-render operations use the serialized game thread. Render accesses only
	 * the mutex-protected texture publication and supplied transform.
	 */
	class BuildingLayer final
	{
	public:
#if defined(MODLOADER_CLIENT_BUILD) && !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Fixed-size game-thread presentation measurements owned by one layer instance. */
		struct DiagnosticsSnapshot
		{
			/** Repeated steady-clock elapsed duration; MeanMs is derived by the reporter. */
			struct Duration
			{
				std::uint64_t Samples = 0;
				double TotalMs = 0.0;
				double MaxMs = 0.0;

				/** Adds one elapsed operation duration. */
				void Add(double elapsedMs) noexcept
				{
					++Samples;
					TotalMs += elapsedMs;
					if (elapsedMs > MaxMs)
					{
						MaxMs = elapsedMs;
					}
				}
			};

			double IntervalMs = 0.0;
			Duration OpenViewport;
			std::uint64_t SuccessfulOpenViewports = 0;
			Duration ViewportUpdate;
			std::uint64_t SuccessfulViewportUpdates = 0;
			std::uint64_t RejectedViewportUpdates = 0;
			Duration PrepareCpuImages;
			std::uint64_t PreparedCoordinates = 0;
			Duration ImageConversion;
			/** NotCovered, SectionUnavailable, ImageUnavailable, Converted, ConversionFailed. */
			std::array<std::uint64_t, 5> CoordinateResolutionCounts{};
			Duration GpuPublicationPass;
			std::uint64_t PublicationPassAlreadyActive = 0;
			std::uint64_t OutstandingTextureOperationRejections = 0;
			Duration TextureUploadCall;
			/** Entries follow UploadState declaration order. */
			std::array<std::uint64_t, 4> TextureUploadStateCounts{};
			std::uint64_t ValidatedTexturePublications = 0;
			std::uint64_t ProcessedRemovalUpdates = 0;
			Duration TextureReleaseCall;
			/** Entries follow ReleaseState declaration order. */
			std::array<std::uint64_t, 4> TextureReleaseStateCounts{};
			std::uint64_t DischargedTextureReceipts = 0;
		};

		/** Hands off one due nonempty game-thread interval without changing layer state. */
		bool TakeDiagnosticsSnapshot(DiagnosticsSnapshot& output) noexcept;

		/** Fixed-size UI callback measurements synchronized by the publication mutex. */
		struct RenderDiagnosticsSnapshot
		{
			using Duration = DiagnosticsSnapshot::Duration;

			double IntervalMs = 0.0;
			Duration RenderMutexWait;
			Duration RenderLockedExecution;
			/** HiddenBySettings, ShutdownSuppressed, InvalidClip, RenderedWithValidClip. */
			std::array<std::uint64_t, 4> RenderOutcomeCounts{};
			std::uint64_t PublishedEntriesExamined = 0;
			std::uint64_t InvalidDrawIdentitySkipped = 0;
			std::uint64_t QuadProjectionFailed = 0;
			std::uint64_t QuadOutsideClip = 0;
			std::uint64_t ImageQuadSubmissions = 0;
		};

		/** Copies and resets one due render interval under the publication mutex. */
		bool TakeRenderDiagnosticsSnapshot(RenderDiagnosticsSnapshot& output) noexcept;
#endif

		/**
		 * Removes the callback and closes the viewport without backend resource calls.
		 * Native collector resources require explicit Shutdown before destruction; the
		 * caller must keep this layer alive while Shutdown returns false.
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
		 * Creates a layer with CPU-only collector state, caller-owned texture service
		 * and BuildingMapService. Section size is read from the service. It does not
		 * initialize native collector resources.
		 * @param textures Texture service that must remain alive through resource shutdown.
		 * @param service Caller-owned service that must outlive this layer and its viewport.
		 * @throws std::invalid_argument when the Section size is nonfinite or nonpositive.
		 */
		BuildingLayer(
			MiniMap::Services::ITextures& textures,
			Services::Map::Buildings::BuildingMapService& service);

		/**
		 * Opens and attaches this layer's single viewport through its borrowed service
		 * using the owned collector. Native collector resources are initialized only
		 * when the service reports an active gameplay map.
		 *
		 * @param xMapUnits Upper-left map X coordinate in service map units.
		 * @param yMapUnits Upper-left map Y coordinate in service map units.
		 * @param widthMapUnits Positive viewport width in service map units.
		 * @param heightMapUnits Positive viewport height in service map units.
		 * @return True when collector initialization succeeds and a viewport is attached.
		 */
		bool OpenViewport(
			double xMapUnits,
			double yMapUnits,
			double widthMapUnits,
			double heightMapUnits);

		/**
		 * Updates the complete rectangle in the attached service's map units. Accepted
		 * changes discover newly entered grid cells and discard departed CPU updates.
		 * Rejection preserves all dirty and pending presentation work.
		 *
		 * @param x1 New inclusive upper-left X coordinate.
		 * @param y1 New inclusive upper-left Y coordinate.
		 * @param x2 New exclusive lower-right X coordinate.
		 * @param y2 New exclusive lower-right Y coordinate.
		 * @return True when the attached viewport accepts the rectangle and completes its update.
		 */
		bool Update(double x1, double y1, double x2, double y2);

		/** Applies the existing drone-rail collection policy before the next viewport update. */
		void SetRenderDroneRails(bool enabled);

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
		 * Stops publication and drawing, closes the viewport, shuts down and releases
		 * the owned collector, then withdraws entries and attempts backend releases.
		 * Collector cleanup occurs after viewport closure and only once. Repeat after
		 * UnavailableRetained; keep this layer and ITextures alive while any receipt or
		 * operation remains.
		 *
		 * @return True when no published entry, receipt or backend operation remains.
		 */
		bool Shutdown();

	private:
		using Coordinate = std::pair<std::int64_t, std::int64_t>;

#if defined(MODLOADER_CLIENT_BUILD) && !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Records one suppression or valid-clip outcome in declaration order. */
		enum class RenderOutcome : std::size_t
		{
			HiddenBySettings,
			ShutdownSuppressed,
			InvalidClip,
			RenderedWithValidClip
		};

		/** Records one coordinate-resolution outcome without retaining its coordinate. */
		enum class CoordinateResolution : std::size_t
		{
			NotCovered,
			SectionUnavailable,
			ImageUnavailable,
			Converted,
			ConversionFailed
		};

		/** Adds elapsed time to one game-thread-owned aggregate on scope exit. */
		struct DurationScope final
		{
			explicit DurationScope(DiagnosticsSnapshot::Duration& duration) noexcept;
			~DurationScope() noexcept;

		private:
			DiagnosticsSnapshot::Duration& duration_;
			std::chrono::steady_clock::time_point started_;
		};

		/** Starts a new interval while preserving every functional presentation field. */
		void ResetDiagnosticsInterval() noexcept;
		/** Tests whether the interval contains actual layer operation observations. */
		bool HasDiagnosticsActivity() const noexcept;
		/** Resets render observations; caller holds presentationMutex_ after exposure. */
		void ResetRenderDiagnosticsInterval() noexcept;

		DiagnosticsSnapshot diagnostics_{};
		std::chrono::steady_clock::time_point diagnosticsIntervalStart_{};
		RenderDiagnosticsSnapshot renderDiagnostics_{};
		std::chrono::steady_clock::time_point renderDiagnosticsIntervalStart_{};
#endif

		/** Attaches a service-created viewport and resets prior CPU presentation state. */
		void SetViewport(std::unique_ptr<Services::Map::Buildings::BuildingMapViewport> viewport);

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
		Services::Map::Buildings::BuildingMapService* service_;
		double sectionSizeMapUnits_;
		std::unique_ptr<MiniMap::BuildingAcquisition::BuildingCollectorV2> buildingCollector_;
		bool renderDroneRailsEnabled_ = true;
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
