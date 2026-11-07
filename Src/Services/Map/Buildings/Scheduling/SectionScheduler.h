// Owns Building Map acquisition resources, session lifecycle and known-sprite preparation.
#pragma once

#include "../BuildingCandidatePublication.h"
#include "../Visuals/BuildingRenderSceneInput.h"
#include "../Cache/OwnedSpriteKey.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace Services::Map::Buildings
{
	class BuildingMapService;
}

namespace Services::Map::Buildings::Cache
{
	class CpuSpriteSourceManager;
	class SpriteDiskCache;
}

namespace Services::Map::Buildings::Scheduling
{
	class BuildingScheduler;

	/** Owns sibling acquisition resources; the service controls session lifetime and shutdown progression.
	 * SpriteDiskCache outlives the CPU manager, which outlives BuildingScheduler and its renderer.
	 * Construction is inert; session shutdown must finish before this owner is destroyed.
	 */
	class SectionScheduler final
	{
	public:
		/** Reports how one fixed publication's known sprite requirements were examined.
		 * KnownRequirementsProcessed means only that its published candidates were examined;
		 * it does not certify complete source enumeration, geometry support, or Section readiness.
		 * Prepared sprite inputs borrow CPU pixels owned by CpuSpriteSourceManager and are valid
		 * only for immediate synchronous consumption before those payloads can be replaced, cleared,
		 * or withdrawn. They must never be retained as Section state.
		 */
		struct KnownSpritePreparationResult
		{
			/** Distinguishes a processed descriptor batch from publication/key validity failures. */
			enum class Status
			{
				KnownRequirementsProcessed,
				InvalidPublication,
				StalePublication,
				InvalidSourceRequirement
			};

			/** Outcome of this single preparation operation. */
			Status PreparationStatus = Status::InvalidPublication;
			/** Candidate publication evidence; sprite readiness cannot upgrade these facts. */
			BuildingCandidateEvidence CandidateEvidence{};
			/** True when evidence or unprepared source kinds leave possible requirements unresolved. */
			bool MayHaveUnknownRequirements = true;
			/** Number of known requirements with an outstanding or newly queued acquisition. */
			std::size_t PendingCount = 0;
			/** Number of known requirements with an established session-local terminal failure. */
			std::size_t FailedCount = 0;
			/** Number of known requirements whose cache/payload access is temporarily unavailable. */
			std::size_t UnavailableCount = 0;
			/** Number of ready CPU sprites that cannot draw because their placement is unknown. */
			std::size_t ReadyWithoutPlacementCount = 0;
			/** Number of ready placed sprites whose independent numerical quad could not be built. */
			std::size_t UnresolvedSpriteGeometryCount = 0;
			/** Number of malformed or unrepresentable source keys. */
			std::size_t InvalidRequirementCount = 0;
			/** Describes one short-lived half-open world-space XY envelope derived from a sprite quad.
			 * X increases rightward and Y downward. This bound does not certify Section membership.
			 */
			struct PreparedSpriteSupport
			{
				/** Inclusive left edge, the minimum world-space X of the quad corners. */
				double MinimumX = 0.0;
				/** Inclusive top edge, the minimum world-space Y of the quad corners. */
				double MinimumY = 0.0;
				/** Exclusive right edge, the maximum world-space X of the quad corners. */
				double MaximumX = 0.0;
				/** Exclusive bottom edge, the maximum world-space Y of the quad corners. */
				double MaximumY = 0.0;
			};
			/** One ready sprite occurrence without an assumed ordinary/overlay rendering pass. */
			struct PreparedSpriteOccurrence
			{
				/** Ordinal of the corresponding occurrence in the retained candidate publication. */
				std::uint64_t PublicationOrdinal = 0;
				/** Neutral pass designation; Unknown remains unresolved for later scene eligibility. */
				BuildingCandidateSpritePass SpritePass = BuildingCandidateSpritePass::Unknown;
				/** Render input borrowing pixels from CpuSpriteSourceManager. */
				Visuals::BuildingSpriteRenderInput RenderInput{};
				/** Independent projected XY support paired with RenderInput. */
				PreparedSpriteSupport Support{};
			};
			/** Ready known sprite occurrences, preserving duplicates and publication correspondence. */
			std::vector<PreparedSpriteOccurrence> PreparedSprites;
		};

		/** Prevents copies from duplicating acquisition resources or borrowed dependencies. */
		SectionScheduler(const SectionScheduler&) = delete;
		/** Prevents copies from duplicating acquisition resources or borrowed dependencies. */
		SectionScheduler& operator=(const SectionScheduler&) = delete;
		/** Prevents moves from invalidating subordinate borrowed resource pointers. */
		SectionScheduler(SectionScheduler&&) = delete;
		/** Prevents moves from invalidating subordinate borrowed resource pointers. */
		SectionScheduler& operator=(SectionScheduler&&) = delete;
		/** Releases owned objects without initiating native cleanup. */
		~SectionScheduler();

	private:
		friend class Services::Map::Buildings::BuildingMapService;

		/** Stores immutable service configuration and creates acquisition resources without I/O.
		 * @param sectionSizeMapUnits Width and height of the service's square cell in map units.
		 * @param sectionResolutionPixels Width and height of the future square image in pixels.
		 * @param buildingsCacheDirectory Service-validated absolute Buildings cache directory.
		 */
		SectionScheduler(
			double sectionSizeMapUnits,
			std::uint32_t sectionResolutionPixels,
			std::filesystem::path buildingsCacheDirectory);

		/** Begins the existing BuildingScheduler session using the stored service image scale.
		 * @param sessionGeneration Game-world session identity admitted by the service.
		 * @return True when BuildingScheduler establishes this renderer session.
		 */
		bool BeginCaptureGameWorldSession(std::uint64_t sessionGeneration);

		/** Advances existing ordered renderer retirement without performing destructor cleanup.
		 * @return True when native retirement is complete and session state is released.
		 */
		bool AdvanceCaptureGameWorldSessionShutdown();

		/** Processes known sprites from exactly one retained, current source publication.
		 * The caller retains publicationOwner for this operation; its borrowed spans and keys are
		 * never retained. Repeated descriptors are processed independently, while BuildingScheduler
		 * suppresses duplicate acquisition by canonical key. Ready pixels are borrowed from the CPU manager;
		 * callers must consume input/support arrays synchronously before manager mutation or shutdown.
		 * Unknown source requirements remain unknown, and no result certifies a Section or scene.
		 * @param publicationOwner Shared owner of the one fixed source publication.
		 * @param output Receives prepared sprite occurrences and individual outcome counts.
		 */
		void PrepareKnownSpriteRequirements(
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			KnownSpritePreparationResult& output);

		/** Resolves every occurrence in one borrowed pass without collapsing equal keys.
		 * @param candidates Candidate occurrences whose ordinals resolve records in this publication.
		 * @param publication Retained owner used to resolve publication-local source records.
		 * @param output Accumulates per-requirement readiness and validation outcomes.
		 */
		void PrepareSpriteRequirementSpan(
			std::span<const BuildingCandidateOccurrence> candidates,
			const BuildingCandidatePublication& publication,
			KnownSpritePreparationResult& output);

		/** Acquires one valid sprite subject and appends a drawable input when placement is known.
		 * @param publicationOrdinal Corresponding ordinal in the retained candidate publication.
		 * @param requirement Borrowed neutral source key and optional validated placement.
		 * @param ownedKey Validated owned copy of the canonical sprite subject.
		 * @param output Accumulates the outcome for this known requirement.
		 */
		void PrepareSpriteRequirement(
			std::uint64_t publicationOrdinal,
			const BuildingCandidateSourceRecord& requirement,
			const Cache::OwnedSpriteKey& ownedKey,
			KnownSpritePreparationResult& output);

		/** Borrows the ready payload, validates placement geometry and appends one paired occurrence.
		 * @param publicationOrdinal Corresponding ordinal in the retained candidate publication.
		 * @param requirement Borrowed known descriptor with its optional validated placement.
		 * @param ownedKey Validated key whose payload is ready through BuildingScheduler.
		 * @param output Accumulates temporary payload and geometry outcomes.
		 */
		void PrepareReadySpriteRequirement(
			std::uint64_t publicationOrdinal,
			const BuildingCandidateSourceRecord& requirement,
			const Cache::OwnedSpriteKey& ownedKey,
			KnownSpritePreparationResult& output);

		/** Derives the independent world-space XY envelope from the prepared projected quad.
		 * @param renderInput Prepared placement, capture metadata and projected image edges.
		 * @param output Receives the minimum and maximum world-space XY corner coordinates.
		 * @return True when SpriteGeometry builds all finite projected world-space corners.
		 */
		bool TryBuildPreparedSpriteSupport(
			const Visuals::BuildingSpriteRenderInput& renderInput,
			KnownSpritePreparationResult::PreparedSpriteSupport& output) const noexcept;

		/** Rechecks currentness and withdraws all borrowed render inputs if the source went stale.
		 * @param publicationOwner Retained owner of the source used for this preparation.
		 * @param output Preparation result whose borrowed arrays are cleared when stale.
		 * @return True when the fixed publication remains current after processing.
		 */
		bool RevalidatePreparedPublication(
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			KnownSpritePreparationResult& output) const noexcept;

		const double sectionSizeMapUnits_;
		const std::uint32_t sectionResolutionPixels_;
		std::unique_ptr<Cache::SpriteDiskCache> spriteDiskCache_;
		std::unique_ptr<Cache::CpuSpriteSourceManager> cpuSpriteSourceManager_;
		std::unique_ptr<BuildingScheduler> buildingScheduler_;
	};
}
