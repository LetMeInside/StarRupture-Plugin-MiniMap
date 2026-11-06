// Declares an internal scheduling capability for resolving copied independent sprite-source metadata.
#pragma once

#include "../Visuals/SpriteGeometry.h"

#include <cstdint>
#include <string_view>

namespace Services::Map::Buildings::Scheduling
{
	/** Classifies one synchronous lookup of independent sprite-source metadata.
	 * These outcomes describe acquisition and metadata availability only; none establishes
	 * contributor membership, spatial relevance, geometry support or Section completeness.
	 */
	enum class BuildingSpriteMetadataStatus
	{
		/** A compatible supported CPU payload exists and all numerical metadata was copied. */
		Ready,
		/** Existing acquisition machinery accepted or is already processing the requirement. */
		Pending,
		/** Existing acquisition machinery established a terminal capture failure this session. */
		Failed,
		/** Source or acquisition access is temporarily unavailable without proving failure or a miss. */
		Unavailable,
		/** The borrowed key cannot be accepted under the existing canonical key contract. */
		InvalidRequirement
	};

	/** Holds copied numerical metadata from a ready independent sprite payload.
	 * A non-Ready result's metadata fields are not valid support evidence. The value owns
	 * no pixels or payload and remains readable after resolution, without promising that
	 * later retrieval will return identical image content.
	 */
	struct BuildingSpriteMetadataResult
	{
		/** Acquisition/metadata outcome; default construction never indicates readiness. */
		BuildingSpriteMetadataStatus Status = BuildingSpriteMetadataStatus::Unavailable;
		/** Original capture dimensions used by SpriteGeometry projection. */
		Visuals::SpriteSourceSize CaptureSize{};
		/** Original capture-space XYZ bounds used by SpriteGeometry projection. */
		Visuals::SpriteCaptureBounds CaptureBounds{};
		/** Projected output-image edges in original capture-pixel coordinates. */
		Visuals::SpriteProjectionRectangle Projection{};
		/** Processed output-image width in pixels, distinct from CaptureSize.Width. */
		std::uint32_t ProcessedWidth = 0;
		/** Processed output-image height in pixels, distinct from CaptureSize.Height. */
		std::uint32_t ProcessedHeight = 0;
	};

	/** Resolves copied independent sprite-source metadata as an internal scheduling capability.
	 * BuildingMapService owns SectionScheduler and coordinates consumer-facing viewports and exact
	 * collection requests. This resolver is not part of those building-oriented consumer APIs.
	 * SectionScheduler will privately provide it from existing resources; BuildingScheduler retains
	 * acquisition status, duplicate suppression, cache-aware readiness and capture FIFO responsibility.
	 * CpuSpriteSourceManager remains the exclusive owner of immutable resident CPU sprite pixels.
	 * A future BuildingCollector borrows this resolver only during an explicit source-support refresh.
	 * This interface owns none of those objects, pixels, cache files or capture-world state, exposes
	 * no native game objects and certifies neither source completeness nor Section membership.
	 *
	 * Calls are expected on the existing serialized game-thread acquisition path. A resolver may
	 * trigger existing lazy retrieval or submit acquisition through the scheduler, but must not
	 * retain the borrowed key. The result's copied values outlive the call; they do not promise
	 * identical content on later payload retrieval. A future rendering transaction must validate
	 * source eligibility and relevant payload consistency.
	 *
	 * Metadata lookup may precede spatial relevance. Pending and Unavailable sources have unknown
	 * support; Failed capture does not prove non-overlap; an invalid key proves no absence; missing
	 * placement prevents support construction; unsupported geometry prevents support proof; spline
	 * contributors are not resolved by sprite lookup; and a distant reference point proves no
	 * non-overlap. Complete Section membership will require source evidence for every potentially
	 * relevant contributor or justified exclusion evidence. The empirical 50 m acquisition region
	 * is not a completeness bound. The collector must combine Ready metadata with its own placement
	 * using SpriteGeometry::BuildWorldQuad before establishing independent sprite support.
	 */
	class IBuildingSpriteMetadataSource
	{
	public:
		/** Supports destruction through the internal scheduling metadata boundary. */
		virtual ~IBuildingSpriteMetadataSource() = default;

		/** Resolves one opaque borrowed sprite requirement synchronously.
		 * The key is a sprite requirement, not a contributor, Mass entity, BuildingID, renderer
		 * revision or Section membership fingerprint. Its bytes are valid only for this call;
		 * the eventual resolver must validate it with the existing OwnedSpriteKey::TryCopy
		 * mechanism before processing; future asynchronous work must use that owned key. Ready
		 * requires a compatible supported payload format, currently StraightRgba8SrgbDisplay,
		 * and copies all numerical metadata into the returned value. The resolver does not
		 * calculate support or retain source bytes. Any retrieval or acquisition side effects
		 * remain within existing scheduler and CPU-manager contracts.
		 *
		 * @param spriteRequirementKey Opaque authored sprite key borrowed for this synchronous call.
		 * @return Readiness and copied metadata; fields are meaningful only when Status is Ready.
		 */
		virtual BuildingSpriteMetadataResult ResolveSpriteMetadata(
			std::string_view spriteRequirementKey) = 0;
	};
}
