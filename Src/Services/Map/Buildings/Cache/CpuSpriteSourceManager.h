// Owns the currently eligible immutable processed sprites for one Building Map service.
#pragma once

#include "CpuSpriteSourceLease.h"
#include "OwnedSpriteKey.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace Services::Map::Buildings
{
	class BuildingMapService;
}

namespace Services::Map::Buildings::Cache
{
	/** Retains one current processed payload per opaque canonical sprite key.
	 * The manager performs no I/O, capture, scheduling, atlas work or synchronization.
	 * Calls are serialized on the owning BuildingMapService calling thread.
	 */
	class CpuSpriteSourceManager final
	{
	public:
		/** Distinguishes an absent source from a retained, currently eligible source. */
		enum class Availability
		{
			Unavailable,
			Ready
		};

		/** Returns availability and, when ready, a lease for that exact publication. */
		struct LookupResult
		{
			Availability Status = Availability::Unavailable;
			CpuSpriteSourceLease Lease;
		};

		/** Invalidates all source leases before releasing the manager's current entries. */
		~CpuSpriteSourceManager();
		/** Prevents duplicate owners from mutating the same publication tokens. */
		CpuSpriteSourceManager(const CpuSpriteSourceManager&) = delete;
		/** Prevents duplicate owners from mutating the same publication tokens. */
		CpuSpriteSourceManager& operator=(const CpuSpriteSourceManager&) = delete;
		/** Keeps the unique source owner at its fixed service-owned address. */
		CpuSpriteSourceManager(CpuSpriteSourceManager&&) = delete;
		/** Keeps the unique source owner at its fixed service-owned address. */
		CpuSpriteSourceManager& operator=(CpuSpriteSourceManager&&) = delete;

		/** Publishes an already validated immutable payload under its owned opaque key.
		 * Re-publishing a key revokes all leases to its previous publication first.
		 * A null payload is rejected without disturbing an existing source.
		 * @param key Canonical owned sprite identity.
		 * @param payload Validated payload retained by shared immutable ownership.
		 * @return True when the payload becomes the current source for this key.
		 */
		bool Publish(
			const OwnedSpriteKey& key,
			std::shared_ptr<const ProcessedSpritePayload> payload);

		/** Acquires the currently eligible source and its metadata through one lease.
		 * @param key Canonical owned sprite identity to find.
		 * @return Ready with a lease, or Unavailable when no current source exists.
		 */
		LookupResult Lookup(const OwnedSpriteKey& key) const noexcept;

		/** Revokes and removes one source while existing leases remain safely readable.
		 * @param key Canonical owned sprite identity to withdraw.
		 * @return True when a current source was removed.
		 */
		bool Withdraw(const OwnedSpriteKey& key) noexcept;

		/** Revokes every current source for a session or world reset.
		 * Existing leases keep their payloads readable but permanently stale.
		 */
		void InvalidateAll() noexcept;

	private:
		friend class Services::Map::Buildings::BuildingMapService;

		/** Creates the inert owner only as a subobject of BuildingMapService. */
		CpuSpriteSourceManager() = default;

		/** Associates one manager-owned payload with its publication-specific token. */
		struct SourceEntry
		{
			std::shared_ptr<const ProcessedSpritePayload> Payload;
			std::shared_ptr<CpuSpriteSourceLease::CurrentnessState> Currentness;
		};

		std::unordered_map<std::string, SourceEntry> sourcesByKey_;
	};
}
