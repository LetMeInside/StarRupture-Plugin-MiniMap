#if defined(MODLOADER_CLIENT_BUILD)

// Owns inert acquisition resources and delegates the existing session lifecycle to BuildingScheduler.
#include "SectionScheduler.h"

#include "BuildingScheduler.h"
#include "../Cache/CpuSpriteSourceManager.h"
#include "../Cache/SpriteDiskCache.h"

#include <utility>

namespace Services::Map::Buildings::Scheduling
{
	/** Stores service configuration, then creates each owner before its borrowing dependents. */
	SectionScheduler::SectionScheduler(
		double sectionSizeMapUnits,
		std::uint32_t sectionResolutionPixels,
		double collectionMarginMapUnits,
		std::filesystem::path buildingsCacheDirectory)
		: sectionSizeMapUnits_(sectionSizeMapUnits)
		, sectionResolutionPixels_(sectionResolutionPixels)
		, collectionMarginMapUnits_(collectionMarginMapUnits)
	{
		spriteDiskCache_.reset(new Cache::SpriteDiskCache(std::move(buildingsCacheDirectory)));
		cpuSpriteSourceManager_.reset(new Cache::CpuSpriteSourceManager(spriteDiskCache_.get()));
		buildingScheduler_.reset(new BuildingScheduler(
			cpuSpriteSourceManager_.get(), spriteDiskCache_.get()));
	}

	/** Destroys subordinates in reverse member order; native retirement remains service-directed. */
	SectionScheduler::~SectionScheduler() = default;

	/** Delegates session creation without changing BuildingScheduler's generation contract. */
	bool SectionScheduler::BeginCaptureGameWorldSession(std::uint64_t sessionGeneration)
	{
		const double pixelsPerMapUnit =
			static_cast<double>(sectionResolutionPixels_) / sectionSizeMapUnits_;
		return buildingScheduler_ != nullptr &&
			buildingScheduler_->BeginSession(sessionGeneration, pixelsPerMapUnit);
	}

	/** Advances the existing retirement path and releases no native owner before completion. */
	bool SectionScheduler::AdvanceCaptureGameWorldSessionShutdown()
	{
		return buildingScheduler_ == nullptr || buildingScheduler_->AdvanceSessionShutdown();
	}
}

#endif // MODLOADER_CLIENT_BUILD
