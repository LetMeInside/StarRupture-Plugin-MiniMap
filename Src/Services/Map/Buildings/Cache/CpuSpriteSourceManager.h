// Owns resident immutable sprites and retrieves cache misses through the configured disk cache.
#pragma once

#include "OwnedSpriteKey.h"
#include "ProcessedSpritePayload.h"

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <cstdint>
#endif
#include <memory>
#include <string>
#include <unordered_map>

namespace Services::Map::Buildings::Cache
{
	class SpriteDiskCache;

	/** Owns immutable CPU payloads by key and loads absent entries from disk.
	 * All calls are serialized on the designated game thread. Shared results remain valid
	 * after an entry is replaced, withdrawn or cleared.
	 */
	class CpuSpriteSourceManager final
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

		/** Borrows the disk cache for lazy retrieval; the cache must outlive this manager.
		 * @param spriteDiskCache Cache used only when a key is not resident.
		 */
		explicit CpuSpriteSourceManager(SpriteDiskCache* spriteDiskCache) noexcept;
		/** Prevents copies from duplicating resident payload ownership. */
		CpuSpriteSourceManager(const CpuSpriteSourceManager&) = delete;
		/** Prevents copies from duplicating resident payload ownership. */
		CpuSpriteSourceManager& operator=(const CpuSpriteSourceManager&) = delete;
		/** Prevents moving the manager while borrowers may hold payload pointers. */
		CpuSpriteSourceManager(CpuSpriteSourceManager&&) = delete;
		/** Prevents moving the manager while borrowers may hold payload pointers. */
		CpuSpriteSourceManager& operator=(CpuSpriteSourceManager&&) = delete;

		/** Returns a resident payload or loads and adopts one through the disk cache.
		 * @param key Canonical owned sprite identity to retrieve.
		 * @return Ready with shared payload ownership, CaptureRequired for Missing/Invalid, or Unavailable.
		 */
		SpriteResult GetSprite(const OwnedSpriteKey& key);

		/** Adopts a captured immutable payload without copying its pixel vector.
		 * Existing shared owners keep the previous payload alive after replacement.
		 * @param key Canonical owned sprite identity for the payload.
		 * @param payload Exclusive payload ownership transferred into the resident map.
		 * @return True when the payload is retained under a valid nonempty key.
		 */
		bool Adopt(const OwnedSpriteKey& key, std::unique_ptr<const ProcessedSpritePayload>&& payload);

		/** Erases one resident entry; outstanding shared owners remain valid.
		 * @param key Canonical owned sprite identity to withdraw.
		 * @return True when a resident entry was removed.
		 */
		bool Withdraw(const OwnedSpriteKey& key) noexcept;

		/** Clears all resident sprites while preserving outstanding shared owners.
		 */
		void Clear() noexcept;

	private:
		SpriteDiskCache* spriteDiskCache_ = nullptr;
		std::unordered_map<std::string, std::shared_ptr<const ProcessedSpritePayload>> payloadsByKey_;
	};
}
