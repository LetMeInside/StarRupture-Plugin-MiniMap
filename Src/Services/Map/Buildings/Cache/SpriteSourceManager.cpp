#if defined(MODLOADER_CLIENT_BUILD)

// Implements cache-aware shared sprite ownership, deterministic incremental atlas packing,
// and one whole-texture GPU publication at the service's synchronous flush boundary.
#include "SpriteSourceManager.h"

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <chrono>
#endif
#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

#include "SpriteDiskCache.h"

namespace Services::Map::Buildings::Cache
{
	/** Retains borrowed cache and texture services and initializes one persistent transparent atlas. */
	SpriteSourceManager::SpriteSourceManager(
		std::filesystem::path buildingsCacheDirectory,
		MiniMap::Services::ITextures* textures)
		: spriteDiskCache_(new SpriteDiskCache(std::move(buildingsCacheDirectory)))
		, textures_(textures)
		, atlasPixels_(512u * 512u * 4u, 0)
	{
		freeRectangles_.push_back({0, 0, atlasWidth_, atlasHeight_});
	}

	/** Destroys the cache and CPU data without calling the texture backend. */
	SpriteSourceManager::~SpriteSourceManager() = default;

	/** Returns a resident payload or loads it once from the configured disk cache. */
	SpriteSourceManager::SpriteResult SpriteSourceManager::GetSprite(const OwnedSpriteKey& key)
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		using Clock = std::chrono::steady_clock;
		const auto lookupStarted = Clock::now();
		SpriteResult result = GetSpriteCore(key);
		result.Diagnostics.TotalMs = std::chrono::duration<double, std::milli>(
			Clock::now() - lookupStarted).count();
		return result;
#else
		return GetSpriteCore(key);
#endif
	}

	/** Resolves resident data before delegating one absent key to the disk cache. */
	SpriteSourceManager::SpriteResult SpriteSourceManager::GetSpriteCore(const OwnedSpriteKey& key)
	{
		const auto resident = payloadsByKey_.find(key.Bytes());
		if (resident != payloadsByKey_.end())
		{
			SpriteResult result{Status::Ready, resident->second};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			result.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::ResidentPayloadFound;
#endif
			if (GetAtlasAdmission(key) == AtlasAdmission::CapacityExceeded)
			{
				result.Result = Status::Unavailable;
				result.Payload.reset();
				return result;
			}
			return result;
		}
		return LoadSpriteFromDisk(key);
	}

	/** Loads, validates and adopts one disk payload while preserving established status evidence. */
	SpriteSourceManager::SpriteResult SpriteSourceManager::LoadSpriteFromDisk(const OwnedSpriteKey& key)
	{
		if (spriteDiskCache_ == nullptr)
		{
			SpriteResult result{Status::Unavailable, nullptr};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			result.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::NoDiskCacheProvider;
#endif
			return result;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		using Clock = std::chrono::steady_clock;
		const auto diskLoadStarted = Clock::now();
#endif
		SpriteDiskCache::LoadResult loaded = spriteDiskCache_->TryLoad(key);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const double diskLoadMs = std::chrono::duration<double, std::milli>(Clock::now() - diskLoadStarted).count();
#endif
		if (loaded.Status == SpriteDiskCache::LoadStatus::Loaded && loaded.Payload)
		{
			SpriteResult result = AdoptDiskPayload(key, std::move(loaded.Payload));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			result.Diagnostics.DiskLoadMs = diskLoadMs;
#endif
			return result;
		}
		const bool captureRequired = loaded.Status == SpriteDiskCache::LoadStatus::Missing ||
			loaded.Status == SpriteDiskCache::LoadStatus::Invalid;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		using LookupRoute = SpriteResult::LookupEvidence::LookupRoute;
		const std::uint8_t lookupRoute = static_cast<std::uint8_t>(
			loaded.Status == SpriteDiskCache::LoadStatus::Missing
				? LookupRoute::DiskEntryMissing
				: loaded.Status == SpriteDiskCache::LoadStatus::Invalid
					? LookupRoute::DiskEntryInvalid
					: loaded.Status == SpriteDiskCache::LoadStatus::IOError
						? LookupRoute::DiskReadIOError
						: loaded.Status == SpriteDiskCache::LoadStatus::Unavailable
							? LookupRoute::DiskReadUnavailable
							: LookupRoute::OtherDiskLoadFailure);
#else
		const std::uint8_t lookupRoute = 0;
#endif
		SpriteResult result = BuildDiskLoadFailure(captureRequired, lookupRoute);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		result.Diagnostics.DiskLoadMs = diskLoadMs;
#endif
		return result;
	}

	/** Adopts one disk result and retrieves the shared resident owner without another cache read. */
	SpriteSourceManager::SpriteResult SpriteSourceManager::AdoptDiskPayload(
		const OwnedSpriteKey& key,
		std::unique_ptr<const ProcessedSpritePayload>&& payload)
	{
		if (!Adopt(key, std::move(payload)))
		{
			SpriteResult result{Status::Unavailable, nullptr};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			result.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::LoadedPayloadCouldNotBeAdopted;
#endif
			return result;
		}
		const auto adopted = payloadsByKey_.find(key.Bytes());
		if (adopted == payloadsByKey_.end())
		{
			SpriteResult result{Status::Unavailable, nullptr};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			result.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::AdoptedPayloadNotFound;
#endif
			return result;
		}
		SpriteResult result{Status::Ready, adopted->second};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		result.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::DiskLoadedAndPayloadAdopted;
#endif
		return result;
	}

	/** Maps confirmed cache misses separately from all uncertain disk retrieval outcomes. */
	SpriteSourceManager::SpriteResult SpriteSourceManager::BuildDiskLoadFailure(
		bool captureRequired,
		std::uint8_t lookupRoute) const noexcept
	{
		SpriteResult result;
		if (captureRequired)
		{
			result.Result = Status::CaptureRequired;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		result.Diagnostics.Route = static_cast<SpriteResult::LookupEvidence::LookupRoute>(lookupRoute);
#endif
		return result;
	}

	/** Transfers a captured payload into the map without copying its image allocation. */
	bool SpriteSourceManager::Adopt(
		const OwnedSpriteKey& key,
		std::unique_ptr<const ProcessedSpritePayload>&& payload)
	{
		if (key.Bytes().empty() || !payload || payload->Pixels().empty())
		{
			return false;
		}
		std::shared_ptr<const ProcessedSpritePayload> sharedPayload(std::move(payload));
		const std::string keyBytes = key.Bytes();
		payloadsByKey_.insert_or_assign(keyBytes, std::move(sharedPayload));
		publishedDrawingChanged_ = publishedEntriesByKey_.erase(keyBytes) != 0 || publishedDrawingChanged_;
		auto atlasEntry = atlasEntriesByKey_.find(keyBytes);
		const ProcessedSpritePayload& adoptedPayload = *payloadsByKey_.at(keyBytes);
		if (atlasEntry != atlasEntriesByKey_.end() &&
			atlasEntry->second.Content.Width == adoptedPayload.Width() &&
			atlasEntry->second.Content.Height == adoptedPayload.Height())
		{
			RemovePendingKey(keyBytes);
			CopySpriteToAtlas(adoptedPayload, atlasEntry->second);
			admissionByKey_.insert_or_assign(keyBytes, AtlasAdmission::Admitted);
			atlasDirty_ = true;
		}
		else
		{
			if (atlasEntry != atlasEntriesByKey_.end())
			{
				atlasEntry->second.DrawingEligible = false;
			}
			if (pendingKeySet_.insert(keyBytes).second)
			{
				pendingKeys_.push_back(keyBytes);
			}
			admissionByKey_.insert_or_assign(keyBytes, AtlasAdmission::Pending);
		}
		return true;
	}

	/** Returns whether one key has been packed, failed capacity, or awaits its next batch. */
	SpriteSourceManager::AtlasAdmission SpriteSourceManager::GetAtlasAdmission(const OwnedSpriteKey& key) const noexcept
	{
		const auto found = admissionByKey_.find(key.Bytes());
		return found == admissionByKey_.end() ? AtlasAdmission::InvalidPayload : found->second;
	}

	/** Returns the manager-owned disk cache used by BuildingScheduler persistence. */
	SpriteDiskCache* SpriteSourceManager::DiskCache() const noexcept
	{
		return spriteDiskCache_.get();
	}

	/** Erases one resident owner while existing shared results keep payload bytes alive. */
	bool SpriteSourceManager::Withdraw(const OwnedSpriteKey& key) noexcept
	{
		const bool removed = payloadsByKey_.erase(key.Bytes()) != 0;
		RemovePendingKey(key.Bytes());
		admissionByKey_.erase(key.Bytes());
		publishedDrawingChanged_ = publishedEntriesByKey_.erase(key.Bytes()) != 0 || publishedDrawingChanged_;
		const auto allocation = atlasEntriesByKey_.find(key.Bytes());
		if (allocation != atlasEntriesByKey_.end())
		{
			allocation->second.DrawingEligible = false;
		}
		return removed;
	}

	/** Releases resident owners while payloads held by active Sections remain valid. */
	void SpriteSourceManager::Clear() noexcept
	{
		publishedDrawingChanged_ = !publishedEntriesByKey_.empty() || publishedDrawingChanged_;
		payloadsByKey_.clear();
		atlasEntriesByKey_.clear();
		publishedEntriesByKey_.clear();
		admissionByKey_.clear();
		pendingKeys_.clear();
		pendingKeySet_.clear();
		freeRectangles_.clear();
		freeRectangles_.push_back({0, 0, atlasWidth_, atlasHeight_});
		atlasDirty_ = false;
	}

	/** Removes one key from both pending containers without changing atlas pixels. */
	void SpriteSourceManager::RemovePendingKey(const std::string& key) noexcept
	{
		if (pendingKeySet_.erase(key) == 0)
		{
			return;
		}
		const auto pending = std::find(pendingKeys_.begin(), pendingKeys_.end(), key);
		if (pending != pendingKeys_.end())
		{
			pendingKeys_.erase(pending);
		}
	}

	/** Packs a whole pending batch, uploads one complete image and atomically publishes its registry. */
	MiniMap::Services::UploadState SpriteSourceManager::FlushAtlasToGPU()
	{
		if ((!atlasDirty_ && pendingKeys_.empty()) || ownershipUncertain_)
		{
			return ownershipUncertain_
				? MiniMap::Services::UploadState::Unavailable
				: MiniMap::Services::UploadState::Complete;
		}
		if (!PackPendingSprites())
		{
			return MiniMap::Services::UploadState::Unavailable;
		}
		if (!atlasDirty_)
		{
			return MiniMap::Services::UploadState::Complete;
		}
		if (textures_ == nullptr || !textures_->Available())
		{
			return MiniMap::Services::UploadState::Unavailable;
		}
		std::unordered_map<std::string, AtlasSpriteEntry> candidateEntries;
		for (const auto& entry : atlasEntriesByKey_)
		{
			if (entry.second.DrawingEligible)
			{
				candidateEntries.emplace(entry.first, entry.second);
			}
		}
		return UploadAtlasCandidate(std::move(candidateEntries));
	}

	/** Submits one complete atlas and preserves the old published texture on any upload failure. */
	MiniMap::Services::UploadState SpriteSourceManager::UploadAtlasCandidate(
		std::unordered_map<std::string, AtlasSpriteEntry>&& candidateEntries)
	{
		MiniMap::Services::OwnedTexture candidate;
		const MiniMap::Services::TextureSize size{atlasWidth_, atlasHeight_};
		const MiniMap::Services::UploadState upload = textures_->UploadRGBA(
			atlasPixels_, size, MiniMap::Services::TextureRequest{"", size}, candidate);
		const std::optional<MiniMap::Services::TextureSize> uploadedSize = candidate.Size();
		if (upload != MiniMap::Services::UploadState::Complete || !candidate.IsValidated() ||
			!uploadedSize || uploadedSize->width != size.width || uploadedSize->height != size.height)
		{
			bool releaseUncertain = false;
			if (candidate.HasRecord() && upload != MiniMap::Services::UploadState::OwnershipUncertain)
			{
				const MiniMap::Services::ReleaseState release = textures_->Release(candidate);
				releaseUncertain = release != MiniMap::Services::ReleaseState::Empty &&
					release != MiniMap::Services::ReleaseState::Returned;
			}
			if (upload == MiniMap::Services::UploadState::OwnershipUncertain || releaseUncertain)
			{
				unresolvedTexture_.Swap(candidate);
				ownershipUncertain_ = true;
			}
			return upload == MiniMap::Services::UploadState::Complete
				? MiniMap::Services::UploadState::Rejected
				: upload;
		}
		publishedTexture_.Swap(candidate);
		publishedSize_ = size;
		publishedEntriesByKey_.swap(candidateEntries);
		const std::weak_ptr<const void> oldLease = publishedTextureLease_;
		publishedTextureLease_ = std::make_shared<const std::uint8_t>(0);
		RetireTexture(candidate, oldLease);
		publishedDrawingChanged_ = true;
		atlasDirty_ = false;
		DrainRetiredTextures();
		return MiniMap::Services::UploadState::Complete;
	}

	/** Retires the published atlas without invalidating snapshots that still borrow it. */
	MiniMap::Services::ReleaseState SpriteSourceManager::ReleaseAtlasTexture()
	{
		if (textures_ == nullptr) return MiniMap::Services::ReleaseState::Empty;
		const bool hadTexture = publishedTexture_.HasRecord();
		const std::weak_ptr<const void> oldLease = publishedTextureLease_;
		RetireTexture(publishedTexture_, oldLease);
		publishedTextureLease_.reset();
		publishedEntriesByKey_.clear();
		publishedSize_ = {0, 0};
		atlasDirty_ = true;
		publishedDrawingChanged_ = true;
		DrainRetiredTextures();
		return hadTexture && !retiredTextures_.empty()
			? MiniMap::Services::ReleaseState::UnavailableRetained
			: MiniMap::Services::ReleaseState::Returned;
	}

	/** Returns the draw identity of the matching published atlas. */
	MiniMap::Services::Texture SpriteSourceManager::PublishedTexture() const noexcept
	{
		return publishedTexture_.View();
	}

	/** Copies one matching published registry entry into caller-owned storage. */
	bool SpriteSourceManager::TryGetPublishedSprite(const OwnedSpriteKey& key, AtlasSpriteEntry& output) const noexcept
	{
		const auto found = publishedEntriesByKey_.find(key.Bytes());
		if (found == publishedEntriesByKey_.end())
		{
			return false;
		}
		output = found->second;
		return true;
	}

	/** Returns the dimensions associated with the currently published texture. */
	MiniMap::Services::TextureSize SpriteSourceManager::PublishedAtlasSize() const noexcept
	{
		return publishedSize_;
	}

	/** Returns the immutable lease paired with the current published texture and registry. */
	std::shared_ptr<const void> SpriteSourceManager::PublishedTextureLease() const noexcept
	{
		return publishedTextureLease_;
	}

	/** Reports and clears one atlas-registry change event for service snapshot refresh. */
	bool SpriteSourceManager::ConsumePublishedDrawingChange() noexcept
	{
		const bool changed = publishedDrawingChanged_;
		publishedDrawingChanged_ = false;
		return changed;
	}

	/** Releases old texture owners only after no immutable snapshot can submit their identity. */
	void SpriteSourceManager::DrainRetiredTextures() noexcept
	{
		for (auto retired = retiredTextures_.begin(); retired != retiredTextures_.end();)
		{
			if (!retired->Lease.expired() || retired->OwnershipUncertain)
			{
				++retired;
				continue;
			}
			const MiniMap::Services::ReleaseState result = textures_ != nullptr
				? textures_->Release(retired->Owner)
				: MiniMap::Services::ReleaseState::UnavailableRetained;
			if (result == MiniMap::Services::ReleaseState::Empty || result == MiniMap::Services::ReleaseState::Returned)
			{
				retired = retiredTextures_.erase(retired);
			}
			else
			{
				if (result == MiniMap::Services::ReleaseState::OwnershipUncertain)
				{
					ownershipUncertain_ = true;
					retired->OwnershipUncertain = true;
				}
				++retired;
			}
		}
	}

	/** Keeps the manager alive while current, uncertain or retired texture receipts remain. */
	bool SpriteSourceManager::HasPendingTextureRetirement() const noexcept
	{
		return publishedTexture_.HasRecord() || unresolvedTexture_.HasRecord() || !retiredTextures_.empty();
	}

	/** Retains a texture owner until its last published presentation snapshot is gone. */
	void SpriteSourceManager::RetireTexture(
		MiniMap::Services::OwnedTexture& texture,
		std::weak_ptr<const void> lease)
	{
		if (!texture.HasRecord())
		{
			return;
		}
		retiredTextures_.emplace_back();
		retiredTextures_.back().Owner.Swap(texture);
		retiredTextures_.back().Lease = std::move(lease);
	}

	/** Places pending sprites in canonical deterministic order, growing once before rejecting overflow. */
	bool SpriteSourceManager::PackPendingSprites()
	{
		if (pendingKeys_.empty())
		{
			return true;
		}
		std::sort(pendingKeys_.begin(), pendingKeys_.end(), [this](const std::string& left, const std::string& right)
		{
			const auto& a = *payloadsByKey_.at(left);
			const auto& b = *payloadsByKey_.at(right);
			const std::uint32_t aw = a.Width() + 4;
			const std::uint32_t ah = a.Height() + 4;
			const std::uint32_t bw = b.Width() + 4;
			const std::uint32_t bh = b.Height() + 4;
			if ((std::max)(aw, ah) != (std::max)(bw, bh)) return (std::max)(aw, ah) > (std::max)(bw, bh);
			if (aw * ah != bw * bh) return aw * ah > bw * bh;
			if (ah != bh) return ah > bh;
			if (aw != bw) return aw > bw;
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
				[](unsigned char x, unsigned char y) { return x < y; });
		});
		for (const std::string& key : pendingKeys_)
		{
			const ProcessedSpritePayload& payload = *payloadsByKey_.at(key);
			AtlasSpriteEntry entry;
			if (!TryAllocateRectangle(payload.Width() + 4, payload.Height() + 4, entry.Allocated) &&
				(atlasWidth_ == 512 && GrowAtlas() && TryAllocateRectangle(payload.Width() + 4, payload.Height() + 4, entry.Allocated)))
			{
				CopySpriteToAtlas(payload, entry);
				atlasEntriesByKey_.insert_or_assign(key, entry);
				admissionByKey_.insert_or_assign(key, AtlasAdmission::Admitted);
				continue;
			}
			if (entry.Allocated.Width == 0)
			{
				admissionByKey_.insert_or_assign(key, AtlasAdmission::CapacityExceeded);
			}
			else
			{
				CopySpriteToAtlas(payload, entry);
				atlasEntriesByKey_.insert_or_assign(key, entry);
				admissionByKey_.insert_or_assign(key, AtlasAdmission::Admitted);
			}
		}
		pendingKeys_.clear();
		pendingKeySet_.clear();
		RecalculateAtlasUvs();
		return true;
	}

	/** Scores free rectangles by short side, long side, Y and X while retaining exact ties. */
	bool SpriteSourceManager::TryAllocateRectangle(
		std::uint32_t width,
		std::uint32_t height,
		AtlasSpriteEntry::Rectangle& output)
	{
		std::size_t best = freeRectangles_.size();
		std::uint32_t bestShort = (std::numeric_limits<std::uint32_t>::max)();
		std::uint32_t bestLong = (std::numeric_limits<std::uint32_t>::max)();
		for (std::size_t index = 0; index < freeRectangles_.size(); ++index)
		{
			const FreeRectangle& free = freeRectangles_[index];
			if (width > free.Width || height > free.Height) continue;
			const std::uint32_t shortFit = (std::min)(free.Width - width, free.Height - height);
			const std::uint32_t longFit = (std::max)(free.Width - width, free.Height - height);
			if (shortFit > bestShort || (shortFit == bestShort && longFit > bestLong)) continue;
			if (shortFit == bestShort && longFit == bestLong && best != freeRectangles_.size() &&
				(free.Y > freeRectangles_[best].Y || (free.Y == freeRectangles_[best].Y && free.X >= freeRectangles_[best].X))) continue;
			best = index;
			bestShort = shortFit;
			bestLong = longFit;
		}
		if (best == freeRectangles_.size()) return false;
		const FreeRectangle& selected = freeRectangles_[best];
		output = {selected.X, selected.Y, width, height};
		UpdateFreeRectangles(output);
		return true;
	}

	/** Splits every intersected free rectangle and removes fully contained remnants. */
	void SpriteSourceManager::UpdateFreeRectangles(const AtlasSpriteEntry::Rectangle& allocation)
	{
		std::vector<FreeRectangle> updated;
		for (const FreeRectangle& free : freeRectangles_)
		{
			if (allocation.X >= free.X + free.Width || allocation.X + allocation.Width <= free.X ||
				allocation.Y >= free.Y + free.Height || allocation.Y + allocation.Height <= free.Y)
			{
				updated.push_back(free);
				continue;
			}
			if (allocation.X > free.X) updated.push_back({free.X, free.Y, allocation.X - free.X, free.Height});
			if (allocation.X + allocation.Width < free.X + free.Width)
				updated.push_back({allocation.X + allocation.Width, free.Y, free.X + free.Width - allocation.X - allocation.Width, free.Height});
			if (allocation.Y > free.Y) updated.push_back({free.X, free.Y, free.Width, allocation.Y - free.Y});
			if (allocation.Y + allocation.Height < free.Y + free.Height)
				updated.push_back({free.X, allocation.Y + allocation.Height, free.Width, free.Y + free.Height - allocation.Y - allocation.Height});
		}
		freeRectangles_.clear();
		for (std::size_t index = 0; index < updated.size(); ++index)
		{
			bool contained = false;
			for (std::size_t other = 0; other < updated.size(); ++other)
			{
				if (index != other && updated[index].X >= updated[other].X && updated[index].Y >= updated[other].Y &&
					updated[index].X + updated[index].Width <= updated[other].X + updated[other].Width &&
					updated[index].Y + updated[index].Height <= updated[other].Y + updated[other].Height)
				{
					contained = true;
					break;
				}
			}
			if (!contained) freeRectangles_.push_back(updated[index]);
		}
	}

	/** Copies source texels and extrudes their nearest edge/corner pixels into two-pixel gutters. */
	void SpriteSourceManager::CopySpriteToAtlas(const ProcessedSpritePayload& payload, AtlasSpriteEntry& entry) noexcept
	{
		entry.Content = {entry.Allocated.X + 2, entry.Allocated.Y + 2, payload.Width(), payload.Height()};
		const auto& source = payload.Pixels();
		for (std::uint32_t y = 0; y < entry.Allocated.Height; ++y)
		{
			const std::uint32_t sourceY = (std::clamp)(y, 2u, payload.Height() + 1) - 2;
			for (std::uint32_t x = 0; x < entry.Allocated.Width; ++x)
			{
				const std::uint32_t sourceX = (std::clamp)(x, 2u, payload.Width() + 1) - 2;
				const std::size_t sourceIndex = (std::size_t(sourceY) * payload.Width() + sourceX) * 4;
				const std::size_t atlasIndex = (std::size_t(entry.Allocated.Y + y) * atlasWidth_ + entry.Allocated.X + x) * 4;
				std::memcpy(atlasPixels_.data() + atlasIndex, source.data() + sourceIndex, 4);
			}
		}
		entry.CaptureSize = payload.CaptureSize();
		entry.Bounds = payload.Bounds();
		entry.Projection = payload.Projection();
		entry.DrawingEligible = true;
		atlasDirty_ = true;
	}

	/** Grows once from 512 to 1024, preserving rows, coordinates and existing source entries. */
	bool SpriteSourceManager::GrowAtlas()
	{
		if (atlasWidth_ != 512) return false;
		std::vector<std::uint8_t> grown(1024u * 1024u * 4u, 0);
		for (std::uint32_t row = 0; row < atlasHeight_; ++row)
			std::memcpy(grown.data() + std::size_t(row) * 1024 * 4, atlasPixels_.data() + std::size_t(row) * atlasWidth_ * 4, std::size_t(atlasWidth_) * 4);
		atlasPixels_.swap(grown);
		atlasWidth_ = 1024;
		atlasHeight_ = 1024;
		freeRectangles_.push_back({512, 0, 512, 1024});
		freeRectangles_.push_back({0, 512, 512, 512});
		RecalculateAtlasUvs();
		atlasDirty_ = true;
		return true;
	}

	/** Recomputes normalized content-edge UVs after packing or atlas growth. */
	void SpriteSourceManager::RecalculateAtlasUvs() noexcept
	{
		for (auto& sprite : atlasEntriesByKey_)
		{
			const auto& rectangle = sprite.second.Content;
			sprite.second.Uv = {float(rectangle.X) / atlasWidth_, float(rectangle.Y) / atlasHeight_,
				float(rectangle.X + rectangle.Width) / atlasWidth_, float(rectangle.Y + rectangle.Height) / atlasHeight_};
		}
	}
}

#endif // MODLOADER_CLIENT_BUILD
