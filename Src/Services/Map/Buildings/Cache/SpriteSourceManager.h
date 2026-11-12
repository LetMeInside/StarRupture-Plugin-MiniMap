// Owns canonical immutable sprite sources, persistent incremental CPU atlas storage,
// deterministic packing state, the published sprite registry and shared GPU texture.
#pragma once

#include "OwnedSpriteKey.h"
#include "ProcessedSpritePayload.h"

#include <cstdint>
#include <memory>
#include <list>
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Services/Textures.h"

namespace Services::Map::Buildings::Cache
{
	class SpriteDiskCache;
	/** Owns immutable CPU payloads, persistent atlas storage, published sprite metadata and its GPU texture.
	 * All calls are serialized on the designated game thread. Shared results remain valid
	 * after an entry is replaced, withdrawn or cleared.
	 */
	class SpriteSourceManager final
	{
	public:
		/** Distinguishes resident/loaded sprites, confirmed cache misses, and uncertain access. */
		enum class Status
		{
			Ready,
			CaptureRequired,
			Unavailable
		};

		/** Returns status and shared access to a resident payload when ready. */
		struct SpriteResult
		{
			Status Result = Status::Unavailable;
			std::shared_ptr<const ProcessedSpritePayload> Payload;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			/** Records the actual resident or disk retrieval outcome and elapsed lookup time. */
			struct LookupEvidence
			{
				/** Identifies the retrieval path observed by one GetSprite call. */
				enum class LookupRoute : std::uint8_t
				{
					Unknown,
					ResidentPayloadFound,
					NoDiskCacheProvider,
					DiskLoadedAndPayloadAdopted,
					DiskEntryMissing,
					DiskEntryInvalid,
					DiskReadIOError,
					DiskReadUnavailable,
					LoadedPayloadCouldNotBeAdopted,
					AdoptedPayloadNotFound,
					OtherDiskLoadFailure
				};

				LookupRoute Route = LookupRoute::Unknown;
				double TotalMs = 0.0;
				double DiskLoadMs = 0.0;
			};
			LookupEvidence Diagnostics{};
#endif
		};

		/** Describes one sprite's atlas pixels and source geometry for GPU drawing. */
		struct AtlasSpriteEntry
		{
			/** Stores an upper-left-origin half-open atlas rectangle in pixels. */
			struct Rectangle
			{
				std::uint32_t X = 0;
				std::uint32_t Y = 0;
				std::uint32_t Width = 0;
				std::uint32_t Height = 0;
			};
			Rectangle Allocated;
			Rectangle Content;
			std::array<float, 4> Uv{};
			Visuals::SpriteSourceSize CaptureSize{};
			Visuals::SpriteCaptureBounds Bounds{};
			Visuals::SpriteProjectionRectangle Projection{};
			/** Separates retained allocation pixels from a current canonical-key source. */
			bool DrawingEligible = true;
		};

		/** Reports CPU residency and atlas capacity separately from GPU publication. */
		enum class AtlasAdmission { Pending, Admitted, CapacityExceeded, InvalidPayload };

		/** Creates the owned lazy disk cache, atlas image and service-bound texture publisher.
		 * @param buildingsCacheDirectory Absolute directory used for compatible sprite files.
		 * @param textures Backend used for game-thread atlas submission and explicit release.
		 */
		explicit SpriteSourceManager(std::filesystem::path buildingsCacheDirectory, MiniMap::Services::ITextures* textures);
		/** Releases CPU owners without issuing backend operations. */
		~SpriteSourceManager();
		/** Prevents copies from duplicating resident payload ownership. */
		SpriteSourceManager(const SpriteSourceManager&) = delete;
		/** Prevents copies from duplicating resident payload ownership. */
		SpriteSourceManager& operator=(const SpriteSourceManager&) = delete;
		/** Prevents moving the manager while borrowers may hold payload pointers. */
		SpriteSourceManager(SpriteSourceManager&&) = delete;
		/** Prevents moving the manager while borrowers may hold payload pointers. */
		SpriteSourceManager& operator=(SpriteSourceManager&&) = delete;

		/** Returns a resident payload or loads and adopts one through the disk cache.
		 * @param key Canonical owned sprite identity to retrieve.
		 * @return Ready with shared payload ownership, CaptureRequired for Missing/Invalid, or Unavailable.
		 */
		SpriteResult GetSprite(const OwnedSpriteKey& key);

		/** Uploads the complete dirty atlas once and publishes matching texture metadata on success. */
		MiniMap::Services::UploadState FlushAtlasToGPU();
		/** Retires the currently published GPU atlas and releases it after snapshot leases expire. */
		MiniMap::Services::ReleaseState ReleaseAtlasTexture();
		/** Returns the published atlas texture identity without transferring ownership. */
		MiniMap::Services::Texture PublishedTexture() const noexcept;
		/** Returns dimensions and metadata from the currently published GPU atlas. */
		bool TryGetPublishedSprite(const OwnedSpriteKey& key, AtlasSpriteEntry& output) const noexcept;
		/** Returns the published texture dimensions, or empty before the first upload. */
		MiniMap::Services::TextureSize PublishedAtlasSize() const noexcept;
		/** Returns a token that keeps the matching published texture owner alive for snapshots. */
		std::shared_ptr<const void> PublishedTextureLease() const noexcept;
		/** Releases retired owners whose presentation snapshots no longer reference them. */
		void DrainRetiredTextures() noexcept;
		/** Reports whether any texture receipt still needs this manager to retain its owner. */
		bool HasPendingTextureRetirement() const noexcept;
		/** Consumes an event set when published drawable atlas bindings have changed. */
		bool ConsumePublishedDrawingChange() noexcept;

		/** Adopts a captured immutable payload without copying its pixel vector.
		 * Existing shared owners keep the previous payload alive after replacement.
		 * @param key Canonical owned sprite identity for the payload.
		 * @param payload Exclusive payload ownership transferred into the resident map.
		 * @return True when the payload is retained under a valid nonempty key.
		 */
		bool Adopt(const OwnedSpriteKey& key, std::unique_ptr<const ProcessedSpritePayload>&& payload);
		/** Returns the current admission result for a canonical sprite key. */
		AtlasAdmission GetAtlasAdmission(const OwnedSpriteKey& key) const noexcept;
		/** Returns the cache owned alongside this manager for capture persistence. */
		SpriteDiskCache* DiskCache() const noexcept;

		/** Removes one CPU resident and invalidates drawing while retaining allocated atlas pixels.
		 * Outstanding shared payload owners remain valid; no texture upload is requested.
		 * @param key Canonical owned sprite identity to withdraw.
		 * @return True when a resident entry was removed.
		 */
		bool Withdraw(const OwnedSpriteKey& key) noexcept;

		/** Clears CPU residency and drawing metadata, resets allocation state, and retains atlas bytes and texture ownership.
		 * Outstanding shared payload owners remain valid; this performs no backend operation or upload.
		 */
		void Clear() noexcept;

	private:
		/** Packs newly admitted sources in deterministic order and grows once if required. */
		bool PackPendingSprites();
		/** Copies one processed sprite and its edge-extruded gutter into the persistent image. */
		void CopySpriteToAtlas(const ProcessedSpritePayload& payload, AtlasSpriteEntry& entry) noexcept;
		/** Grows the CPU image while preserving pixel coordinates and existing allocations. */
		bool GrowAtlas();
		/** Recomputes normalized coordinates for all candidate entries at current dimensions. */
		void RecalculateAtlasUvs() noexcept;
		/** Finds the best persistent free rectangle for one padded sprite. */
		bool TryAllocateRectangle(std::uint32_t width, std::uint32_t height, AtlasSpriteEntry::Rectangle& output);
		/** Splits and prunes maximal free rectangles after one allocation. */
		void UpdateFreeRectangles(const AtlasSpriteEntry::Rectangle& allocation);
		/** Removes a key from the pending batch when a retained allocation becomes reusable. */
		void RemovePendingKey(const std::string& key) noexcept;
		/** Submits candidate pixels and adopts their registry only after receipt validation. */
		MiniMap::Services::UploadState UploadAtlasCandidate(
			std::unordered_map<std::string, AtlasSpriteEntry>&& candidateEntries);
		/** Moves an old texture owner into retirement tied to its immutable snapshot lease. */
		void RetireTexture(
			MiniMap::Services::OwnedTexture& texture,
			std::weak_ptr<const void> lease);
		/** Resolves the resident-first path before entering disk retrieval. */
		SpriteResult GetSpriteCore(const OwnedSpriteKey& key);
		/** Loads one missing key, adopts valid data and records exact Debug lookup evidence. */
		SpriteResult LoadSpriteFromDisk(const OwnedSpriteKey& key);
		/** Retains a loaded payload without copying its pixels and returns its shared owner. */
		SpriteResult AdoptDiskPayload(const OwnedSpriteKey& key, std::unique_ptr<const ProcessedSpritePayload>&& payload);
		/** Applies the existing cache miss, invalid-entry and retrieval-error mapping. */
		SpriteResult BuildDiskLoadFailure(bool captureRequired, std::uint8_t lookupRoute) const noexcept;

		std::unique_ptr<SpriteDiskCache> spriteDiskCache_;
		MiniMap::Services::ITextures* textures_ = nullptr;
		std::unordered_map<std::string, std::shared_ptr<const ProcessedSpritePayload>> payloadsByKey_;
		std::unordered_map<std::string, AtlasSpriteEntry> atlasEntriesByKey_;
		std::unordered_map<std::string, AtlasSpriteEntry> publishedEntriesByKey_;
		std::unordered_map<std::string, AtlasAdmission> admissionByKey_;
		std::vector<std::string> pendingKeys_;
		std::unordered_set<std::string> pendingKeySet_;
		/** Stores one persistent unallocated maximal rectangle in the CPU atlas. */
		struct FreeRectangle
		{
			std::uint32_t X = 0;
			std::uint32_t Y = 0;
			std::uint32_t Width = 0;
			std::uint32_t Height = 0;
		};
		std::vector<FreeRectangle> freeRectangles_;
		std::vector<std::uint8_t> atlasPixels_;
		MiniMap::Services::OwnedTexture publishedTexture_;
		std::shared_ptr<const void> publishedTextureLease_;
		struct RetiredTexture
		{
			MiniMap::Services::OwnedTexture Owner;
			std::weak_ptr<const void> Lease;
			bool OwnershipUncertain = false;
		};
		std::list<RetiredTexture> retiredTextures_;
		MiniMap::Services::OwnedTexture unresolvedTexture_;
		MiniMap::Services::TextureSize publishedSize_{512, 512};
		std::uint32_t atlasWidth_ = 512;
		std::uint32_t atlasHeight_ = 512;
		bool atlasDirty_ = false;
		bool publishedDrawingChanged_ = false;
		bool ownershipUncertain_ = false;
	};
}
