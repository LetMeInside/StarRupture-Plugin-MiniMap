// Defines borrowed neutral source records, acquisition readiness and coverage for
// one retained publication, before sprite pixels or complete certification exist.
#pragma once

#include "BuildingMapRegion.h"
#include "Visuals/BuildingRenderSceneInput.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Services::Map::Buildings
{
	/** Summarizes immutable source acquisition, independently of sprite/GPU readiness.
	 * Complete covers acquired numerical source within the declared coverage only;
	 * it requires HasCoverage and no missing required numerical inputs, but does not
	 * certify a section, numerical rendering success or a completed image.
	 */
	enum class BuildingSourceReadiness : std::uint8_t
	{
		Unavailable,
		Incomplete,
		Complete
	};

	/** Borrows one known authored sprite requirement and its optional numerical placement.
	 * The publication retains the key storage. A key identifies a reusable sprite
	 * subject, not a placed contributor; multiple records may have the same key.
	 */
	struct BuildingSpriteSourceRecord
	{
		/** Nonempty opaque bytes interpreted by the future subject provider, not by rendering.
		 * A consumer needing the key after releasing the publication must take ownership.
		 */
		std::string_view SpriteRequirementKey;
		/** True when Placement has been acquired; does not replace numerical validation. */
		bool HasPlacement = false;
		/** Existing neutral world-space placement; ignored while HasPlacement is false. */
		Visuals::SpritePlacement Placement{};
	};

	/** Borrows immutable source arrays and all nested storage from one retained publication.
	 * The publication must retain records, key bytes, curve segments, surfaces and
	 * any adaptation storage for its entire shared lifetime. No mutable alias may
	 * change them; a newer acquisition requires a new publication. Shared appearance
	 * pixels keep their existing immutable owners. Copying this data structure copies descriptors,
	 * not source arrays or pixels. Numerical consumers borrow it synchronously and
	 * must keep the publication alive through every access to a span or key.
	 */
	struct BuildingSourceData
	{
		/** Source acquisition summary, not render-resource availability or revision evidence. */
		BuildingSourceReadiness Readiness = BuildingSourceReadiness::Unavailable;
		/** True only when Coverage is a finite positive acquired map-space rectangle.
		 * False means unknown coverage, never an empty certified region.
		 */
		bool HasCoverage = false;
		/** Upper-left-origin, rightward/downward, half-open bounds; ignored without HasCoverage.
		 * Coverage alone does not prove complete contributor or occluder support.
		 */
		BuildingCollectionRegion Coverage{};
		/** Known ordinary requirements in deterministic pass order, even before certification.
		 * Keys remain discoverable without placement, capture bounds, pixels or an atlas.
		 */
		std::span<const BuildingSpriteSourceRecord> OrdinarySprites;
		/** Acquired numerical curves/transforms/surfaces, using the existing neutral types.
		 * Nested spans are publication-owned; no prepared geometry or acquisition runs here.
		 */
		std::span<const Visuals::BuildingSplineRenderInput> Splines;
		/** Known overlay-last requirements in deterministic pass order, never ordinary occluders. */
		std::span<const BuildingSpriteSourceRecord> OverlayLastSprites;
	};
}
