// Provides reusable game-independent sprite-prism occlusion planning for prepared XYZ spline triangles.
#pragma once

#include "SplineSurfaceGeometry.h"
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "../BuildingMapPerformanceDiagnostics.h"
#include <chrono>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <vector>

// Uncomment to set the leaf capacity in Debug Experimental builds to 8 (default), 16, or 32.
// #define MINIMAP_EXPERIMENTAL_BVH_LEAF_CAPACITY 8

namespace Services::Map::Buildings::Visuals
{
	class BuildingGeometryPreparation;
	#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Aggregates BVH query work for one Section preparation attempt without allocating. */
	struct SplineOcclusionQueryCounters
	{
		std::uint64_t CellQueries = 0;
		std::uint64_t NodesVisited = 0;
		std::uint64_t LeafItemAabbTests = 0;
		std::uint64_t CandidateOrdinalsReturned = 0;
	};
	#endif

	/** Names the canonical position component shared with prepared spline surfaces. */
	using SplineOcclusionPoint3D = std::array<double, 3>;

	/** Holds one generated visible XYZ/UV triangle and its deterministic source-order key. */
	struct SplineOcclusionTriangle
	{
		std::array<SplineSurfaceVertex, 3> Vertices{};
		std::uint64_t Key = 0;
	};

	/** Holds one convex world-XY sprite quad and the explicit world-space top height used for occlusion. */
	struct SplineOcclusionSpriteQuad
	{
		std::array<std::array<double, 2>, 4> Corners{};
		double TopHeight = 0.0;
		std::uint64_t Key = 0;
	};

	/** Reports numerical planning outcomes without renderer, source, or policy concepts. */
	enum class SplineOcclusionStatus : std::uint8_t
	{
		Complete,
		NotPrepared,
		InvalidInput,
		NumericalFailure,
		AllocationFailure
	};

	/** Owns validated occluders and a deterministic BVH, immutable while compatible clipping operations read it. */
	class SplineOcclusionIndex final
	{
		friend class SplineOcclusionGeometry;
		friend class SplineOcclusionPlanner;
#if defined(_DEBUG) && defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		friend class BuildingGeometryPreparation;
#endif

		/** Stores a conservative world-XY broad-phase box. */
		struct Bounds2D
		{
			std::array<double, 2> Minimum{};
			std::array<double, 2> Maximum{};
		};

		/** Owns one validated occluder value and its precomputed XY bounds. */
		struct PreparedOccluder
		{
			SplineOcclusionSpriteQuad Value;
			Bounds2D Bounds;
		};

		/** Stores one median BVH node or leaf ordinal range. */
		struct BvhNode
		{
			Bounds2D Bounds;
			std::size_t Begin = 0;
			std::size_t End = 0;
			std::size_t Left = 0;
			std::size_t Right = 0;
		};

		std::vector<PreparedOccluder> Occluders;
		std::vector<BvhNode> Nodes;
		std::vector<std::size_t> Ordinals;
		SplineOcclusionStatus PreparationStatus = SplineOcclusionStatus::NotPrepared;

	public:
		/** Constructs an empty index whose storage is owned by its caller. */
		SplineOcclusionIndex() = default;

		/** Prevents accidental deep copies of prepared occluders and BVH storage. */
		SplineOcclusionIndex(const SplineOcclusionIndex&) = delete;

		/** Prevents accidental deep assignment of prepared occluders and BVH storage. */
		SplineOcclusionIndex& operator=(const SplineOcclusionIndex&) = delete;

		/** Transfers prepared-index ownership without copying its spatial data. */
		SplineOcclusionIndex(SplineOcclusionIndex&&) noexcept = default;

		/** Transfers prepared-index ownership without copying its spatial data. */
		SplineOcclusionIndex& operator=(SplineOcclusionIndex&&) noexcept = default;

		/** Reports whether the current immutable numerical index is ready for queries. */
		SplineOcclusionStatus Status() const noexcept
		{
			return PreparationStatus;
		}
	};

	/** Owns mutable per-operation query, clipping, and visible-output storage while retaining vector capacities. */
	class SplineOcclusionWorkspace final
	{
		friend class SplineOcclusionGeometry;
		friend class SplineOcclusionPlanner;
#if defined(MINIMAP_GEOMETRY_TESTS)
		friend class SplineOcclusionGeometryTestAccess;
#endif
#if defined(_DEBUG) && defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		friend class BuildingGeometryPreparation;
#endif

		/** Identifies a polygon in the flat reusable clipping vertex buffer. */
		struct PolygonRange
		{
			std::size_t Begin = 0;
			std::size_t Count = 0;
		};

		/** Retains original-edge provenance through consecutive plane splits. */
		struct SupportedVertex
		{
			std::array<double, 3> Position{};
			std::array<double, 2> TextureCoordinates{};
			std::array<std::array<SplineSurfaceVertex, 2>, 2> Supports{};
			std::size_t SupportCount = 0;
		};

		std::vector<std::size_t> Candidates;
		std::vector<std::size_t> TriangleCandidates;
		std::vector<SupportedVertex> CurrentVertices;
		std::vector<SupportedVertex> NextVertices;
		std::vector<PolygonRange> CurrentPolygons;
		std::vector<PolygonRange> NextPolygons;
		std::vector<SupportedVertex> WorkingPolygon;
		std::vector<SupportedVertex> InsidePolygon;
		std::vector<SupportedVertex> OutsidePolygon;
		std::vector<SplineOcclusionTriangle> Visible;
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		// Per-surface counts are published only when the entire clip succeeds.
		std::size_t RemovedTriangles = 0;
		std::size_t FragmentTriangles = 0;
		SplineOcclusionQueryCounters QueryActivity;
		bool IsCollectingQueryActivity = false;
#endif

	public:
		/** Constructs empty per-operation scratch owned by its caller. */
		SplineOcclusionWorkspace() = default;

		/** Prevents accidental copying of potentially large retained clipping buffers. */
		SplineOcclusionWorkspace(const SplineOcclusionWorkspace&) = delete;

		/** Prevents accidental copying of potentially large retained clipping buffers. */
		SplineOcclusionWorkspace& operator=(const SplineOcclusionWorkspace&) = delete;

		/** Transfers caller-owned scratch storage without copying retained buffers. */
		SplineOcclusionWorkspace(SplineOcclusionWorkspace&&) noexcept = default;

		/** Transfers caller-owned scratch storage without copying retained buffers. */
		SplineOcclusionWorkspace& operator=(SplineOcclusionWorkspace&&) noexcept = default;

		/** Returns the visible XYZ/UV fragments generated by the last successful clipping call. */
		const std::vector<SplineOcclusionTriangle>& VisibleTriangles() const noexcept
		{
			return Visible;
		}
	#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Clears query counts and begins collection for one Section geometry preparation attempt. */
		void BeginQueryActivityCollection() noexcept
		{
			QueryActivity = {};
			IsCollectingQueryActivity = true;
		}

		/** Stops query-count collection when the owning Section preparation becomes terminal. */
		void EndQueryActivityCollection() noexcept
		{
			IsCollectingQueryActivity = false;
		}

		/** Returns aggregate query counts owned by this Section's numerical workspace. */
		const SplineOcclusionQueryCounters& GetQueryActivity() const noexcept
		{
			return QueryActivity;
		}
	#endif
	};

	/** Builds deterministic occluder BVHs and clips caller-prepared XYZ/UV triangles against sprite prisms. */
	class SplineOcclusionGeometry final
	{
		friend class SplineOcclusionPlanner;
#if defined(MINIMAP_GEOMETRY_TESTS)
		friend class SplineOcclusionGeometryTestAccess;
#endif

		using Point2D = std::array<double, 2>;
		using Point3D = SplineOcclusionPoint3D;
		using Bounds2D = SplineOcclusionIndex::Bounds2D;
		using PreparedOccluder = SplineOcclusionIndex::PreparedOccluder;
		using BvhNode = SplineOcclusionIndex::BvhNode;
		using PolygonRange = SplineOcclusionWorkspace::PolygonRange;
		using SupportedVertex = SplineOcclusionWorkspace::SupportedVertex;

		/** Represents either a quad side plane or the occluder's top-height plane. */
		struct ClipPlane
		{
			Point2D First{};
			Point2D Second{};
			double TopHeight = 0.0;
			bool IsHeight = false;
		};

		/** Caches one source triangle's broad-phase bounds and minimum world height. */
		struct TriangleMetrics
		{
			Bounds2D Bounds;
			double MinimumZ = 0.0;
		};

	public:
		/**
		 * Compile-time maximum occluders in one BVH leaf. Development diagnostic builds may select
		 * 8, 16, or 32 with MINIMAP_EXPERIMENTAL_BVH_LEAF_CAPACITY; all other builds use 8.
		 */
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS && \
	defined(_DEBUG) && !defined(NDEBUG) && \
	defined(MINIMAP_EXPERIMENTAL_BVH_LEAF_CAPACITY)
		static constexpr std::size_t BvhLeafCapacity =
			MINIMAP_EXPERIMENTAL_BVH_LEAF_CAPACITY;
		static_assert(BvhLeafCapacity == 8 || BvhLeafCapacity == 16 || BvhLeafCapacity == 32,
			"MINIMAP_EXPERIMENTAL_BVH_LEAF_CAPACITY must be 8, 16, or 32");
#else
		static constexpr std::size_t BvhLeafCapacity = 8;
#endif

#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Holds independent elapsed times for the three existing index preparation stages. */
		struct PreparationStageTimings
		{
			double CopyOccludersMs = 0.0;
			bool CopyOccludersMeasured = false;
			double CanonicalSortingMs = 0.0;
			bool CanonicalSortingMeasured = false;
			bool CanonicalSortingSkipped = false;
			double BvhConstructionMs = 0.0;
			bool BvhConstructionMeasured = false;
		};
#endif

		/** Validates, canonicalizes, sorts, bounds, and indexes sprite quads into caller-owned immutable index storage.
		 * Input values are copied into the index; no span or caller-owned memory is retained.
		 * The index must not be prepared again while clipping operations hold const references to it.
		 * @param input World-XY convex quads with explicit world-space top heights and stable numerical keys.
		 * @param index Caller-owned prepared index that may be read by multiple independent workspaces.
		 * @return Complete on success, otherwise the validation or allocation failure category.
		 */
		static SplineOcclusionStatus PrepareOccluders(
			std::span<const SplineOcclusionSpriteQuad> input,
			SplineOcclusionIndex& index
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, PreparationStageTimings* stageTimings = nullptr
#endif
			) noexcept
		{
			return PrepareOccludersInternal(input, index, false
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, stageTimings
#endif
			);
		}

		/** Prepares input whose unique keys are strictly increasing; this unchecked precondition preserves canonical order.
		 * @param input World-XY convex quads ordered by unique, strictly increasing Key values.
		 * @param index Caller-owned index that receives the complete prepared values on success.
		 * @return Complete on success, otherwise the same validation or allocation failure category as the generic path.
		 */
		static SplineOcclusionStatus PrepareOccludersWithStrictlyIncreasingKeys(
			std::span<const SplineOcclusionSpriteQuad> input,
			SplineOcclusionIndex& index
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, PreparationStageTimings* stageTimings = nullptr
#endif
			) noexcept
		{
			return PrepareOccludersInternal(input, index, true
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, stageTimings
#endif
			);
		}

		/** Clips every prepared spline cell using one shared-index query per cell and reusable caller scratch.
		 * Each call clears prior visible output. Any invalid input or allocation failure clears partial output.
		 * @param vertices Non-owning world-space XYZ/UV vertices from prepared spline surface geometry.
		 * @param indices Non-owning six-index ranges into vertices, partitioned exactly by cells.
		 * @param cells Non-owning accepted spline-profile cells with contiguous vertex and index ranges.
		 * @param index Immutable prepared occluder index shared only among equivalent input operations.
		 * @param workspace Caller-owned mutable query/clipping scratch and visible output storage.
		 * @return Complete when every source triangle was processed, or the failure category.
		 */
		static SplineOcclusionStatus ClipSurface(
			std::span<const SplineSurfaceVertex> vertices,
			std::span<const std::uint32_t> indices,
			std::span<const SplineSurfaceCell> cells,
			const SplineOcclusionIndex& index,
			SplineOcclusionWorkspace& workspace) noexcept
		{
			workspace.Visible.clear();
			if (index.Status() != SplineOcclusionStatus::Complete)
			{
				return index.Status();
			}

#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			workspace.RemovedTriangles = 0;
			workspace.FragmentTriangles = 0;
#endif
			try
			{
				std::size_t nextVertex = 0;
				std::size_t nextIndex = 0;
				for (std::size_t cellIndex = 0; cellIndex < cells.size(); ++cellIndex)
				{
					const auto& cell = cells[cellIndex];
					ValidateCellRanges(vertices, indices, cell, nextVertex, nextIndex);
					ValidateCellIndices(indices, cell);
					if (!ClipCell(vertices, indices, cell, index, workspace))
					{
						workspace.Visible.clear();
						return SplineOcclusionStatus::NumericalFailure;
					}
					nextVertex += cell.VertexCount;
					nextIndex += cell.IndexCount;
				}
				Require(nextVertex == vertices.size() && nextIndex == indices.size(), SplineOcclusionStatus::InvalidInput);
			}
			catch (SplineOcclusionStatus status)
			{
				workspace.Visible.clear();
				return status;
			}
			catch (const std::bad_alloc&)
			{
				workspace.Visible.clear();
				return SplineOcclusionStatus::AllocationFailure;
			}
			catch (const std::length_error&)
			{
				workspace.Visible.clear();
				return SplineOcclusionStatus::AllocationFailure;
			}
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			auto& work = BuildingMapPerformanceDiagnostics::Game();
			work.SplineGeneratedTriangles += indices.size() / 3;
			work.SplinePostOcclusionTriangles += workspace.Visible.size();
			work.SplineOcclusionRemovedTriangles += workspace.RemovedTriangles;
			work.SplineOcclusionFragmentTriangles += workspace.FragmentTriangles;
#endif
			return SplineOcclusionStatus::Complete;
		}

	private:
	#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Measures one sequential stage without changing its exception behavior. */
		class PreparationStageTimer final
		{
		public:
			/** Starts one existing stage only when its diagnostic duration is requested. */
			explicit PreparationStageTimer(double* output, bool* measured) noexcept
				: output_(output), measured_(measured), started_(output_ != nullptr ? Clock::now() : Clock::time_point{}) {}
			/** Stores elapsed time when the stage completes or exits by exception. */
			~PreparationStageTimer() noexcept { Finish(); }
			PreparationStageTimer(const PreparationStageTimer&) = delete;
			PreparationStageTimer& operator=(const PreparationStageTimer&) = delete;

			/** Stores the elapsed stage once, including a stage that exits by exception. */
			void Finish() noexcept
			{
				if (output_ == nullptr)
				{
					return;
				}
				*output_ = std::chrono::duration<double, std::milli>(Clock::now() - started_).count();
				*measured_ = true;
				output_ = nullptr;
			}

		private:
			using Clock = std::chrono::steady_clock;
			double* output_;
			bool* measured_;
			Clock::time_point started_;
		};
	#endif

#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Runs the existing copy, optional canonical sort, and BVH stages in their established order. */
		static void PrepareOccluderStages(
			std::span<const SplineOcclusionSpriteQuad> input,
			SplineOcclusionIndex& index
			, bool skipCanonicalSorting
			, PreparationStageTimings* stageTimings
		)
		{
			PreparationStageTimer copyTimer(stageTimings != nullptr ? &stageTimings->CopyOccludersMs : nullptr,
				stageTimings != nullptr ? &stageTimings->CopyOccludersMeasured : nullptr);
			CopyOccluders(input, index);
			copyTimer.Finish();
			if (!skipCanonicalSorting)
			{
				PreparationStageTimer sortingTimer(
					stageTimings != nullptr ? &stageTimings->CanonicalSortingMs : nullptr,
					stageTimings != nullptr ? &stageTimings->CanonicalSortingMeasured : nullptr);
				std::sort(index.Occluders.begin(), index.Occluders.end(), OccluderLess);
				sortingTimer.Finish();
			}
			PreparationStageTimer bvhTimer(stageTimings != nullptr ? &stageTimings->BvhConstructionMs : nullptr,
				stageTimings != nullptr ? &stageTimings->BvhConstructionMeasured : nullptr);
			BuildBvh(index);
			bvhTimer.Finish();
		}
#endif

		/** Shares preparation while allowing a verified producer to omit only canonical sorting. */
		static SplineOcclusionStatus PrepareOccludersInternal(
			std::span<const SplineOcclusionSpriteQuad> input,
			SplineOcclusionIndex& index,
			bool skipCanonicalSorting
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, PreparationStageTimings* stageTimings
#endif
			) noexcept
		{
			ResetPrepared(index);
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (stageTimings != nullptr)
			{
				*stageTimings = {};
				stageTimings->CanonicalSortingSkipped = skipCanonicalSorting;
			}
#endif
			try
			{
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
				PrepareOccluderStages(input, index, skipCanonicalSorting, stageTimings);
#else
				CopyOccluders(input, index);
				if (!skipCanonicalSorting)
				{
					std::sort(index.Occluders.begin(), index.Occluders.end(), OccluderLess);
				}
				BuildBvh(index);
#endif
				index.PreparationStatus = SplineOcclusionStatus::Complete;
			}
			catch (SplineOcclusionStatus status)
			{
				SetPreparationFailure(index, status);
			}
			catch (const std::bad_alloc&)
			{
				SetPreparationFailure(index, SplineOcclusionStatus::AllocationFailure);
			}
			catch (const std::length_error&)
			{
				SetPreparationFailure(index, SplineOcclusionStatus::AllocationFailure);
			}
			return index.PreparationStatus;
		}

		/** Clears index-owned values while preserving prepared vector capacities. */
		static void ResetPrepared(SplineOcclusionIndex& index) noexcept
		{
			index.Occluders.clear();
			index.Nodes.clear();
			index.Ordinals.clear();
			index.PreparationStatus = SplineOcclusionStatus::NotPrepared;
		}

		/** Clears invalid prepared state and records its failure category. */
		static void SetPreparationFailure(
			SplineOcclusionIndex& index,
			SplineOcclusionStatus status) noexcept
		{
			index.Occluders.clear();
			index.Nodes.clear();
			index.Ordinals.clear();
			index.PreparationStatus = status;
		}

		/** Validates and copies every supplied quad, computing its canonical bounds once. */
		static void CopyOccluders(
			std::span<const SplineOcclusionSpriteQuad> input,
			SplineOcclusionIndex& index)
		{
			index.Occluders.reserve(input.size());
			for (auto value : input)
			{
				Require(Canonicalize(value), SplineOcclusionStatus::InvalidInput);
				index.Occluders.push_back({value, CalculateBounds(value.Corners)});
			}
		}

		/** Canonicalizes a finite strictly convex quad to counter-clockwise perimeter order. */
		static bool Canonicalize(SplineOcclusionSpriteQuad& value) noexcept
		{
			if (!std::isfinite(value.TopHeight))
			{
				return false;
			}
			for (const auto& point : value.Corners)
			{
				if (!IsFinite(point))
				{
					return false;
				}
			}
			const long double turn = Turn(value.Corners[0], value.Corners[1], value.Corners[2]);
			if (!std::isfinite(turn) || turn == 0.0)
			{
				return false;
			}
			const bool reversed = turn < 0.0;
			if (reversed)
			{
				std::reverse(value.Corners.begin(), value.Corners.end());
			}
			for (std::size_t edge = 0; edge < value.Corners.size(); ++edge)
			{
				for (std::size_t point = 0; point < value.Corners.size(); ++point)
				{
					if (point == edge || point == (edge + 1) % value.Corners.size())
					{
						continue;
					}
					if (edge == 0 && point == 2 && !reversed)
					{
						continue;
					}
					const long double side = Turn(value.Corners[edge], value.Corners[(edge + 1) % 4], value.Corners[point]);
					if (!std::isfinite(side) || side <= 0.0)
					{
						return false;
					}
				}
			}
			RotateToMinimum(value.Corners);
			return true;
		}

		/** Orders canonical quads by key, perimeter corners, then explicit top height. */
		static bool OccluderLess(const PreparedOccluder& first, const PreparedOccluder& second) noexcept
		{
			const auto& a = first.Value;
			const auto& b = second.Value;
			if (a.Key != b.Key)
			{
				return a.Key < b.Key;
			}
			if (a.Corners != b.Corners)
			{
				return a.Corners < b.Corners;
			}
			return a.TopHeight < b.TopHeight;
		}

		/** Builds a balanced median BVH with alternating axes and the selected leaf capacity. */
		static void BuildBvh(SplineOcclusionIndex& index)
		{
			index.Ordinals.clear();
			index.Ordinals.reserve(index.Occluders.size());
			for (std::size_t ordinal = 0; ordinal < index.Occluders.size(); ++ordinal)
			{
				index.Ordinals.push_back(ordinal);
			}
			if (index.Occluders.empty())
			{
				return;
			}
			const auto count = index.Occluders.size();
			index.Nodes.reserve(RequiredBvhNodeCount(count));
			std::vector<Bounds2D> compactBounds;
			compactBounds.reserve(count);
			for (const auto& occluder : index.Occluders)
			{
				compactBounds.push_back(occluder.Bounds);
			}
			BuildRange(index, compactBounds, 0, count, 0);
		}

		/** Counts nodes for the supplied occluder count without visiting data or building ranges.
		 * @param count Number of occluders in the median tree.
		 * @return Exact node count, or throws length_error if the result cannot fit size_t.
		 */
		static std::size_t RequiredBvhNodeCount(std::size_t count)
		{
			if (count == 0)
			{
				return 0;
			}
			const auto maximum = (std::numeric_limits<std::size_t>::max)();
			std::size_t partitionCount = 1;
			while (count / partitionCount + (count % partitionCount != 0) > BvhLeafCapacity + 1)
			{
				if (partitionCount > maximum / 2)
				{
					throw std::length_error("occlusion BVH node count overflow");
				}
				partitionCount *= 2;
			}
			const auto basePartitionSize = count / partitionCount;
			const auto largerPartitionCount = count % partitionCount;
			auto leafCount = partitionCount;
			if (basePartitionSize == BvhLeafCapacity)
			{
				if (largerPartitionCount > maximum - leafCount)
				{
					throw std::length_error("occlusion BVH leaf count overflow");
				}
				leafCount += largerPartitionCount;
			}
			else if (basePartitionSize == BvhLeafCapacity + 1)
			{
				if (partitionCount > maximum / 2)
				{
					throw std::length_error("occlusion BVH leaf count overflow");
				}
				leafCount = partitionCount * 2;
			}
			if (leafCount > maximum / 2 + 1)
			{
				throw std::length_error("occlusion BVH node count overflow");
			}
			return leafCount * 2 - 1;
		}

		/** Creates one node, partitions by the existing median rule, and derives bounds from leaves or children. */
		static std::size_t BuildRange(
			SplineOcclusionIndex& index,
			const std::vector<Bounds2D>& compactBounds,
			std::size_t begin,
			std::size_t end,
			std::size_t axis)
		{
			const auto node = index.Nodes.size();
			index.Nodes.push_back({{}, begin, end, 0, 0});
			if (end - begin <= BvhLeafCapacity)
			{
				index.Nodes[node].Bounds = UnionBounds(index, begin, end);
				return node;
			}
			const auto middle = begin + (end - begin) / 2;
			std::nth_element(index.Ordinals.begin() + begin, index.Ordinals.begin() + middle,
				index.Ordinals.begin() + end, [&compactBounds, axis](std::size_t first, std::size_t second)
				{
					return OrdinalLess(compactBounds, first, second, axis);
				});
			const auto left = BuildRange(index, compactBounds, begin, middle, 1 - axis);
			const auto right = BuildRange(index, compactBounds, middle, end, 1 - axis);
			index.Nodes[node].Left = left;
			index.Nodes[node].Right = right;
			const auto& leftBounds = index.Nodes[left].Bounds;
			const auto& rightBounds = index.Nodes[right].Bounds;
			for (std::size_t boundsAxis = 0; boundsAxis < 2; ++boundsAxis)
			{
				index.Nodes[node].Bounds.Minimum[boundsAxis] =
					(std::min)(leftBounds.Minimum[boundsAxis], rightBounds.Minimum[boundsAxis]);
				index.Nodes[node].Bounds.Maximum[boundsAxis] =
					(std::max)(leftBounds.Maximum[boundsAxis], rightBounds.Maximum[boundsAxis]);
			}
			return node;
		}

		/** Orders BVH partition values by selected-axis minimum, maximum, then canonical ordinal. */
		static bool OrdinalLess(
			const std::vector<Bounds2D>& compactBounds,
			std::size_t first,
			std::size_t second,
			std::size_t axis) noexcept
		{
			const auto& a = compactBounds[first];
			const auto& b = compactBounds[second];
			if (a.Minimum[axis] != b.Minimum[axis])
			{
				return a.Minimum[axis] < b.Minimum[axis];
			}
			if (a.Maximum[axis] != b.Maximum[axis])
			{
				return a.Maximum[axis] < b.Maximum[axis];
			}
			return first < second;
		}

		/** Unions one bounded leaf range without computing potentially overflowing centers. */
		static Bounds2D UnionBounds(
			const SplineOcclusionIndex& index,
			std::size_t begin,
			std::size_t end) noexcept
		{
			Bounds2D result = index.Occluders[index.Ordinals[begin]].Bounds;
			for (std::size_t position = begin + 1; position < end; ++position)
			{
				const auto& bounds = index.Occluders[index.Ordinals[position]].Bounds;
				for (std::size_t axis = 0; axis < 2; ++axis)
				{
					result.Minimum[axis] = (std::min)(result.Minimum[axis], bounds.Minimum[axis]);
					result.Maximum[axis] = (std::max)(result.Maximum[axis], bounds.Maximum[axis]);
				}
			}
			return result;
		}

		/** Ensures each cell exactly follows the accepted four-vertex/six-index surface layout. */
		static void ValidateCellRanges(
			std::span<const SplineSurfaceVertex> vertices,
			std::span<const std::uint32_t> indices,
			const SplineSurfaceCell& cell,
			std::size_t nextVertex,
			std::size_t nextIndex)
		{
			Require(cell.VertexCount == 4 && cell.IndexCount == 6 &&
				cell.FirstVertex == nextVertex && cell.FirstIndex == nextIndex,
				SplineOcclusionStatus::InvalidInput);
			Require(cell.FirstVertex <= vertices.size() && cell.VertexCount <= vertices.size() - cell.FirstVertex,
				SplineOcclusionStatus::InvalidInput);
			Require(cell.FirstIndex <= indices.size() && cell.IndexCount <= indices.size() - cell.FirstIndex,
				SplineOcclusionStatus::InvalidInput);
		}

		/** Requires the exact two-triangle diagonal patterns emitted by spline surface preparation. */
		static void ValidateCellIndices(
			std::span<const std::uint32_t> indices,
			const SplineSurfaceCell& cell)
		{
			std::array<std::uint32_t, 6> local{};
			for (std::size_t offset = 0; offset < local.size(); ++offset)
			{
				const auto index = indices[cell.FirstIndex + offset];
				Require(index >= cell.FirstVertex && index - cell.FirstVertex < cell.VertexCount,
					SplineOcclusionStatus::InvalidInput);
				local[offset] = static_cast<std::uint32_t>(static_cast<std::size_t>(index) - cell.FirstVertex);
			}
			const bool firstDiagonal = local == std::array<std::uint32_t, 6>{0, 1, 2, 0, 2, 3};
			const bool secondDiagonal = local == std::array<std::uint32_t, 6>{0, 1, 3, 1, 2, 3};
			Require(firstDiagonal || secondDiagonal, SplineOcclusionStatus::InvalidInput);
		}

		/** Reads and canonicalizes one indexed source triangle without allocating or retaining input storage. */
		static SplineOcclusionTriangle ReadTriangle(
			std::span<const SplineSurfaceVertex> vertices,
			std::span<const std::uint32_t> indices,
			std::size_t firstIndex)
		{
			SplineOcclusionTriangle triangle;
			triangle.Key = static_cast<std::uint64_t>(firstIndex / 3);
			for (std::size_t corner = 0; corner < 3; ++corner)
			{
				const auto vertexIndex = indices[firstIndex + corner];
				Require(vertexIndex < vertices.size(), SplineOcclusionStatus::InvalidInput);
				triangle.Vertices[corner] = vertices[vertexIndex];
			}
			Require(Canonicalize(triangle), SplineOcclusionStatus::InvalidInput);
			return triangle;
		}

		/** Rejects non-finite or zero-area triangles and rotates positive-XY winding to a canonical start. */
		static bool Canonicalize(SplineOcclusionTriangle& triangle) noexcept
		{
			for (const auto& vertex : triangle.Vertices)
			{
				if (!IsFinite(vertex.Position) || !std::isfinite(vertex.TextureCoordinates[0]) ||
					!std::isfinite(vertex.TextureCoordinates[1]))
				{
					return false;
				}
			}
			const long double area = TriangleTurn(triangle.Vertices[0].Position,
				triangle.Vertices[1].Position, triangle.Vertices[2].Position);
			if (!std::isfinite(area) || area == 0.0L)
			{
				return false;
			}
			if (area < 0.0L)
			{
				std::swap(triangle.Vertices[1], triangle.Vertices[2]);
			}
			RotateToMinimum(triangle.Vertices);
			return true;
		}

		/** Canonicalizes a three-point perimeter to its lexicographically smallest vertex. */
		template <typename Point, std::size_t Count>
		static void RotateToMinimum(std::array<Point, Count>& points) noexcept
		{
			const auto minimum = std::min_element(points.begin(), points.end());
			std::rotate(points.begin(), minimum, points.end());
		}

		/** Queries once for the full accepted cell, filters its two triangles, and preserves source index order. */
		static bool ClipCell(
			std::span<const SplineSurfaceVertex> vertices,
			std::span<const std::uint32_t> indices,
			const SplineSurfaceCell& cell,
			const SplineOcclusionIndex& index,
			SplineOcclusionWorkspace& workspace)
		{
			const auto cellBounds = CalculateVertexBounds(vertices.subspan(cell.FirstVertex, cell.VertexCount));
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (workspace.IsCollectingQueryActivity)
			{
				++workspace.QueryActivity.CellQueries;
			}
#endif
			QueryCandidates(cellBounds, index, workspace);
			return ClipCellWithCandidates(vertices, indices, cell, index, workspace);
		}

		/** Clips one validated cell using the already queried cell-level candidate ordinals. */
		static bool ClipCellWithCandidates(
			std::span<const SplineSurfaceVertex> vertices,
			std::span<const std::uint32_t> indices,
			const SplineSurfaceCell& cell,
			const SplineOcclusionIndex& index,
			SplineOcclusionWorkspace& workspace)
		{
			for (std::size_t triangleOffset = 0; triangleOffset < cell.IndexCount; triangleOffset += 3)
			{
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
				const std::size_t priorVisibleCount = workspace.Visible.size();
#endif
				const auto triangle = ReadTriangle(vertices, indices, cell.FirstIndex + triangleOffset);
				const auto metrics = MeasureTriangle(triangle);
				FilterTriangleCandidates(metrics, index, workspace);
				if (workspace.TriangleCandidates.empty())
				{
					workspace.Visible.push_back(triangle);
				}
				else if (!ClipTriangle(triangle, index, workspace))
				{
					return false;
				}
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
				const std::size_t addedVisibleCount = workspace.Visible.size() - priorVisibleCount;
				if (addedVisibleCount == 0)
				{
					++workspace.RemovedTriangles;
				}
				else if (addedVisibleCount > 1)
				{
					workspace.FragmentTriangles += addedVisibleCount - 1;
				}
#endif
			}
			return true;
		}

		/** Computes triangle XY bounds and minimum Z in one pass over its three canonical vertices. */
		static TriangleMetrics MeasureTriangle(const SplineOcclusionTriangle& triangle) noexcept
		{
			TriangleMetrics metrics;
			std::array<Point3D, 3> positions{};
			for (std::size_t index = 0; index < positions.size(); ++index)
			{
				positions[index] = triangle.Vertices[index].Position;
			}
			metrics.Bounds = CalculateBounds(positions);
			metrics.MinimumZ = (std::min)({positions[0][2], positions[1][2], positions[2][2]});
			return metrics;
		}

		/** Retains candidates that touch the triangle in XY and whose top can reach its minimum height. */
		static void FilterTriangleCandidates(
			const TriangleMetrics& metrics,
			const SplineOcclusionIndex& index,
			SplineOcclusionWorkspace& workspace)
		{
			workspace.TriangleCandidates.clear();
			for (const auto candidate : workspace.Candidates)
			{
				const auto& occluder = index.Occluders[candidate];
				if (!Disjoint(metrics.Bounds, occluder.Bounds) && metrics.MinimumZ <= occluder.Value.TopHeight)
				{
					workspace.TriangleCandidates.push_back(candidate);
				}
			}
		}

		/** Clips one candidate-bearing triangle and emits its visible fragments. */
		static bool ClipTriangle(
			const SplineOcclusionTriangle& triangle,
			const SplineOcclusionIndex& index,
			SplineOcclusionWorkspace& workspace)
		{
			InitializeFrontier(triangle, workspace);
			for (const auto candidate : workspace.TriangleCandidates)
			{
				if (!SubtractOccluder(index.Occluders[candidate].Value, workspace))
				{
					return false;
				}
				if (workspace.CurrentPolygons.empty())
				{
					break;
				}
			}
			return EmitVisible(triangle.Key, workspace);
		}

		/** Initializes reusable flat polygon storage with one original triangle and edge provenance. */
		static void InitializeFrontier(
			const SplineOcclusionTriangle& triangle,
			SplineOcclusionWorkspace& workspace)
		{
			workspace.CurrentVertices.clear();
			workspace.CurrentPolygons.clear();
			workspace.CurrentVertices.reserve(3);
			workspace.CurrentPolygons.reserve(1);
			for (std::size_t index = 0; index < 3; ++index)
			{
				SupportedVertex vertex;
				vertex.Position = triangle.Vertices[index].Position;
				vertex.TextureCoordinates = triangle.Vertices[index].TextureCoordinates;
				vertex.Supports[0] = MakeEdge(triangle.Vertices[index], triangle.Vertices[(index + 1) % 3]);
				vertex.Supports[1] = MakeEdge(triangle.Vertices[index], triangle.Vertices[(index + 2) % 3]);
				vertex.SupportCount = 2;
				workspace.CurrentVertices.push_back(vertex);
			}
			workspace.CurrentPolygons.push_back({0, 3});
		}

		/** Queries the BVH and sorts candidate ordinals into deterministic canonical order. */
		static void QueryCandidates(
			const Bounds2D& bounds,
			const SplineOcclusionIndex& index,
			SplineOcclusionWorkspace& workspace)
		{
			workspace.Candidates.clear();
			if (!index.Nodes.empty())
				QueryNode(0, bounds, index, workspace);
			std::sort(workspace.Candidates.begin(), workspace.Candidates.end());
		}

		/** Prunes disjoint BVH nodes and appends only intersecting leaf ordinals. */
		static void QueryNode(
			std::size_t nodeIndex,
			const Bounds2D& bounds,
			const SplineOcclusionIndex& index,
			SplineOcclusionWorkspace& workspace)
		{
		#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (workspace.IsCollectingQueryActivity)
			{
				++workspace.QueryActivity.NodesVisited;
			}
		#endif
			const auto& node = index.Nodes[nodeIndex];
			if (Disjoint(bounds, node.Bounds))
			{
				return;
			}
			if (node.Left != 0)
			{
				QueryNode(node.Left, bounds, index, workspace);
				QueryNode(node.Right, bounds, index, workspace);
				return;
			}
			for (std::size_t ordinalIndex = node.Begin; ordinalIndex < node.End; ++ordinalIndex)
			{
			#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
				if (workspace.IsCollectingQueryActivity)
				{
					++workspace.QueryActivity.LeafItemAabbTests;
				}
			#endif
				const auto ordinal = index.Ordinals[ordinalIndex];
				if (!Disjoint(bounds, index.Occluders[ordinal].Bounds))
				{
					workspace.Candidates.push_back(ordinal);
				#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
					if (workspace.IsCollectingQueryActivity)
					{
						++workspace.QueryActivity.CandidateOrdinalsReturned;
					}
				#endif
				}
			}
		}

		/** Subtracts one quad prism from every current polygon using five exact clip planes. */
		static bool SubtractOccluder(
			const SplineOcclusionSpriteQuad& occluder,
			SplineOcclusionWorkspace& workspace)
		{
			workspace.NextVertices.clear();
			workspace.NextPolygons.clear();
			for (const auto& range : workspace.CurrentPolygons)
			{
				CopyRangeToWorking(range, workspace);
				for (std::size_t planeIndex = 0; planeIndex < 5 && !workspace.WorkingPolygon.empty(); ++planeIndex)
				{
					const auto plane = MakePlane(occluder, planeIndex);
					if (!SplitWorkingPolygon(plane, workspace))
					{
						return false;
					}
					if (HasArea(workspace.OutsidePolygon))
					{
						AppendPolygon(workspace.OutsidePolygon, workspace.NextVertices, workspace.NextPolygons);
					}
					if (HasArea(workspace.InsidePolygon))
					{
						workspace.WorkingPolygon.swap(workspace.InsidePolygon);
					}
					else
					{
						workspace.WorkingPolygon.clear();
					}
				}
			}
			workspace.CurrentVertices.swap(workspace.NextVertices);
			workspace.CurrentPolygons.swap(workspace.NextPolygons);
			return true;
		}

		/** Copies one flat-buffer polygon range into retained working storage. */
		static void CopyRangeToWorking(
			const PolygonRange& range,
			SplineOcclusionWorkspace& workspace)
		{
			workspace.WorkingPolygon.assign(workspace.CurrentVertices.begin() + range.Begin,
				workspace.CurrentVertices.begin() + range.Begin + range.Count);
		}

		/** Appends polygon vertices to flat storage and records their range without a nested allocation. */
		static void AppendPolygon(
			const std::vector<SupportedVertex>& polygon,
			std::vector<SupportedVertex>& vertices,
			std::vector<PolygonRange>& ranges)
		{
			const auto begin = vertices.size();
			vertices.insert(vertices.end(), polygon.begin(), polygon.end());
			ranges.push_back({begin, polygon.size()});
		}

		/** Converts a canonical quad side or its top height into the planner's ordered five planes. */
		static ClipPlane MakePlane(const SplineOcclusionSpriteQuad& occluder, std::size_t index) noexcept
		{
			if (index == 4)
			{
				return {{}, {}, occluder.TopHeight, true};
			}
			return {occluder.Corners[index], occluder.Corners[(index + 1) % 4], occluder.TopHeight, false};
		}

		/** Splits a polygon and reports arithmetic failures without changing caller-visible partial output. */
		static bool SplitWorkingPolygon(
			const ClipPlane& plane,
			SplineOcclusionWorkspace& workspace)
		{
			workspace.InsidePolygon.clear();
			workspace.OutsidePolygon.clear();
			bool positive = false;
			bool negative = false;
			for (const auto& vertex : workspace.WorkingPolygon)
			{
				long double distance = 0.0L;
				if (!Distance(plane, vertex.Position, distance))
				{
					return false;
				}
				positive |= distance > 0.0L;
				negative |= distance < 0.0L;
			}
			if (!positive)
			{
				workspace.InsidePolygon = workspace.WorkingPolygon;
				return true;
			}
			if (!negative)
			{
				workspace.OutsidePolygon = workspace.WorkingPolygon;
				return true;
			}
			return SplitCrossingPolygon(plane, workspace);
		}

		/** Performs inclusive half-plane splitting while retaining exact original-edge support. */
		static bool SplitCrossingPolygon(
			const ClipPlane& plane,
			SplineOcclusionWorkspace& workspace)
		{
			SupportedVertex previous = workspace.WorkingPolygon.back();
			long double previousDistance = 0.0L;
			if (!Distance(plane, previous.Position, previousDistance))
			{
				return false;
			}
			for (const auto& current : workspace.WorkingPolygon)
			{
				long double currentDistance = 0.0L;
				if (!Distance(plane, current.Position, currentDistance))
				{
					return false;
				}
				if (((previousDistance < 0.0L && currentDistance > 0.0L) ||
					(previousDistance > 0.0L && currentDistance < 0.0L)) &&
					!AppendIntersection(previous, current, plane, workspace))
				{
					return false;
				}
				AppendBySign(workspace, current, currentDistance);
				previous = current;
				previousDistance = currentDistance;
			}
			ClosePolygon(workspace.InsidePolygon);
			ClosePolygon(workspace.OutsidePolygon);
			return true;
		}

		/** Appends an edge-plane intersection to both split results using canonical original endpoints. */
		static bool AppendIntersection(
			const SupportedVertex& first,
			const SupportedVertex& second,
			const ClipPlane& plane,
			SplineOcclusionWorkspace& workspace)
		{
			SupportedVertex intersection;
			FindSharedSupports(first, second, intersection);
			SplineSurfaceVertex edgeFirst{first.Position, first.TextureCoordinates};
			SplineSurfaceVertex edgeSecond{second.Position, second.TextureCoordinates};
			if (intersection.SupportCount != 0)
			{
				edgeFirst = intersection.Supports[0][0];
				edgeSecond = intersection.Supports[0][1];
			}
			if (edgeSecond < edgeFirst)
			{
				std::swap(edgeFirst, edgeSecond);
			}
			long double firstDistance = 0.0L;
			long double secondDistance = 0.0L;
			if (!Distance(plane, edgeFirst.Position, firstDistance) ||
				!Distance(plane, edgeSecond.Position, secondDistance))
			{
				return false;
			}
			SplineSurfaceVertex interpolated;
			if (!CalculateIntersection(edgeFirst, edgeSecond, firstDistance, secondDistance, plane, interpolated))
			{
				return false;
			}
			intersection.Position = interpolated.Position;
			intersection.TextureCoordinates = interpolated.TextureCoordinates;
			AppendDistinct(workspace.InsidePolygon, intersection);
			AppendDistinct(workspace.OutsidePolygon, intersection);
			return true;
		}

		/** Finds shared original-edge provenance carried by adjacent polygon vertices. */
		static void FindSharedSupports(
			const SupportedVertex& first,
			const SupportedVertex& second,
			SupportedVertex& intersection) noexcept
		{
			for (std::size_t firstIndex = 0; firstIndex < first.SupportCount; ++firstIndex)
			{
				for (std::size_t secondIndex = 0; secondIndex < second.SupportCount; ++secondIndex)
				{
					if (first.Supports[firstIndex] == second.Supports[secondIndex] && intersection.SupportCount < 2)
					{
						intersection.Supports[intersection.SupportCount++] = first.Supports[firstIndex];
					}
				}
			}
		}

		/** Interpolates a canonical edge intersection and pins height-plane results to the exact top value. */
		static bool CalculateIntersection(
			const SplineSurfaceVertex& first,
			const SplineSurfaceVertex& second,
			long double firstDistance,
			long double secondDistance,
			const ClipPlane& plane,
			SplineSurfaceVertex& output) noexcept
		{
			if (firstDistance == 0.0L)
			{
				output = first;
				return true;
			}
			if (secondDistance == 0.0L)
			{
				output = second;
				return true;
			}
			long double parameter = 0.0L;
			if (!TryGetIntersectionParameter(firstDistance, secondDistance, parameter))
			{
				return false;
			}
			if (!InterpolateIntersectionVertex(first, second, parameter, output))
			{
				return false;
			}
			if (plane.IsHeight) output.Position[2] = plane.TopHeight;
			return true;
		}

		/** Computes the canonical edge parameter with widened arithmetic.
		 * @param firstDistance Signed clipping-plane distance at the first endpoint.
		 * @param secondDistance Signed clipping-plane distance at the second endpoint.
		 * @param parameter Receives the inclusive edge interpolation parameter.
		 * @return True when the denominator and result are finite and the parameter lies on the edge.
		 */
		static bool TryGetIntersectionParameter(
			long double firstDistance,
			long double secondDistance,
			long double& parameter) noexcept
		{
			const long double denominator = firstDistance - secondDistance;
			if (!std::isfinite(denominator) || denominator == 0.0L) return false;
			parameter = firstDistance / denominator;
			return std::isfinite(parameter) && parameter >= 0.0L && parameter <= 1.0L;
		}

		/** Interpolates position and UV using one shared parameter.
		 * @param first First canonical edge endpoint.
		 * @param second Second canonical edge endpoint.
		 * @param parameter Widened interpolation parameter calculated from clipping distances.
		 * @param output Receives interpolated XYZ and UV values.
		 * @return True when all interpolated values are finite.
		 */
		static bool InterpolateIntersectionVertex(
			const SplineSurfaceVertex& first,
			const SplineSurfaceVertex& second,
			long double parameter,
			SplineSurfaceVertex& output) noexcept
		{
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				const long double value = (1.0L - parameter) * first.Position[axis] + parameter * second.Position[axis];
				if (!std::isfinite(value))
				{
					return false;
				}
				output.Position[axis] = static_cast<double>(value);
				if (!std::isfinite(output.Position[axis]))
				{
					return false;
				}
			}
			for (std::size_t axis = 0; axis < 2; ++axis)
			{
				const long double value = (1.0L - parameter) * first.TextureCoordinates[axis] +
					parameter * second.TextureCoordinates[axis];
				if (!std::isfinite(value))
				{
					return false;
				}
				output.TextureCoordinates[axis] = static_cast<double>(value);
				if (!std::isfinite(output.TextureCoordinates[axis]))
				{
					return false;
				}
			}
			return true;
		}

		/** Appends the current vertex to each inclusive side selected by its signed distance. */
		static void AppendBySign(
			SplineOcclusionWorkspace& workspace,
			const SupportedVertex& vertex,
			long double distance)
		{
			if (distance <= 0.0L)
			{
				AppendDistinct(workspace.InsidePolygon, vertex);
			}
			if (distance >= 0.0L)
			{
				AppendDistinct(workspace.OutsidePolygon, vertex);
			}
		}

		/** Appends a vertex unless its XYZ position duplicates the current polygon tail. */
		static void AppendDistinct(
			std::vector<SupportedVertex>& polygon,
			const SupportedVertex& vertex)
		{
			if (polygon.empty() || polygon.back().Position != vertex.Position)
			{
				polygon.push_back(vertex);
			}
		}

		/** Removes a duplicated closing vertex from one polygon. */
		static void ClosePolygon(std::vector<SupportedVertex>& polygon) noexcept
		{
			if (polygon.size() > 1 && polygon.front().Position == polygon.back().Position)
			{
				polygon.pop_back();
			}
		}

		/** Tests that a polygon has positive XY area and reports non-finite or reversed geometry. */
		static bool HasArea(const std::vector<SupportedVertex>& polygon)
		{
			if (polygon.size() < 3)
			{
				return false;
			}
			bool nonzero = false;
			for (std::size_t index = 1; index + 1 < polygon.size(); ++index)
			{
				const long double area = TriangleTurn(polygon[0].Position, polygon[index].Position,
					polygon[index + 1].Position);
				if (area < 0.0L && IsClippingDegenerate(
					polygon[0], polygon[index], polygon[index + 1]))
				{
					continue;
				}
				Require(std::isfinite(area) && area >= 0.0L, SplineOcclusionStatus::NumericalFailure);
				nonzero |= area > 0.0L;
			}
			return nonzero;
		}

		/** Triangulates every surviving convex polygon as a fan into retained output storage. */
		static bool EmitVisible(std::uint64_t key, SplineOcclusionWorkspace& workspace)
		{
			for (const auto& range : workspace.CurrentPolygons)
			{
				for (std::size_t index = 1; index + 1 < range.Count; ++index)
				{
					const auto& first = workspace.CurrentVertices[range.Begin];
					const auto& second = workspace.CurrentVertices[range.Begin + index];
					const auto& third = workspace.CurrentVertices[range.Begin + index + 1];
					const long double area = TriangleTurn(first.Position, second.Position, third.Position);
					if (area < 0.0L && IsClippingDegenerate(first, second, third))
					{
						continue;
					}
					Require(std::isfinite(area) && area >= 0.0L, SplineOcclusionStatus::NumericalFailure);
					if (area > 0.0L)
					{
						SplineOcclusionTriangle triangle;
						triangle.Vertices[0].Position = first.Position;
						triangle.Vertices[0].TextureCoordinates = first.TextureCoordinates;
						triangle.Vertices[1].Position = second.Position;
						triangle.Vertices[1].TextureCoordinates = second.TextureCoordinates;
						triangle.Vertices[2].Position = third.Position;
						triangle.Vertices[2].TextureCoordinates = third.TextureCoordinates;
						triangle.Key = key;
						workspace.Visible.push_back(triangle);
					}
				}
			}
			return true;
		}

		/** Recognizes a clipped fragment collapsed to one representable XY step. */
		static bool IsClippingDegenerate(
			const SupportedVertex& first,
			const SupportedVertex& second,
			const SupportedVertex& third) noexcept
		{
			if (!IsClippingGenerated(first) && !IsClippingGenerated(second) &&
				!IsClippingGenerated(third))
			{
				return false;
			}
			for (std::size_t axis = 0; axis < 2; ++axis)
			{
				const double minimum = (std::min)({first.Position[axis], second.Position[axis],
					third.Position[axis]});
				const double maximum = (std::max)({first.Position[axis], second.Position[axis],
					third.Position[axis]});
				const double extent = maximum - minimum;
				const double spacing = (std::max)({CoordinateSpacing(first.Position[axis]),
					CoordinateSpacing(second.Position[axis]), CoordinateSpacing(third.Position[axis])});
				if (std::isfinite(extent) && extent <= spacing)
				{
					return true;
				}
			}
			return false;
		}

		/** Distinguishes clipping intersections from exact original vertices through edge provenance. */
		static bool IsClippingGenerated(const SupportedVertex& vertex) noexcept
		{
			for (std::size_t support = 0; support < vertex.SupportCount; ++support)
			{
				for (std::size_t endpoint = 0; endpoint < 2; ++endpoint)
				{
					if (vertex.Position == vertex.Supports[support][endpoint].Position)
					{
						return false;
					}
				}
			}
			return true;
		}

		/** Returns the larger adjacent double spacing, ignoring an infinite outer neighbor. */
		static double CoordinateSpacing(double coordinate) noexcept
		{
			const double previous = std::nextafter(coordinate, -INFINITY);
			const double next = std::nextafter(coordinate, INFINITY);
			double spacing = 0.0;
			if (std::isfinite(previous))
			{
				spacing = (std::max)(spacing, coordinate - previous);
			}
			if (std::isfinite(next))
			{
				spacing = (std::max)(spacing, next - coordinate);
			}
			return spacing;
		}

		/** Calculates the signed plane distance for either an XY quad side or a top-Z plane. */
		static bool Distance(
			const ClipPlane& plane,
			const Point3D& point,
			long double& output) noexcept
		{
			if (plane.IsHeight)
			{
				output = static_cast<long double>(point[2]) - plane.TopHeight;
			}
			else
			{
				output = -Turn(plane.First, plane.Second, {point[0], point[1]});
			}
			return std::isfinite(output);
		}

		/** Computes one quad's endpoint-only XY bounds without center or extent arithmetic. */
		template <typename Points>
		static Bounds2D CalculateBounds(const Points& points) noexcept
		{
			Bounds2D result{{INFINITY, INFINITY}, {-INFINITY, -INFINITY}};
			for (const auto& point : points)
			{
				for (std::size_t axis = 0; axis < 2; ++axis)
				{
					result.Minimum[axis] = (std::min)(result.Minimum[axis], point[axis]);
					result.Maximum[axis] = (std::max)(result.Maximum[axis], point[axis]);
				}
			}
			return result;
		}

		/** Builds a lexicographically ordered XYZ edge used to stabilize shared-edge intersections. */
		static std::array<SplineSurfaceVertex, 2> MakeEdge(
			SplineSurfaceVertex first,
			SplineSurfaceVertex second) noexcept
		{
			if (second < first)
			{
				std::swap(first, second);
			}
			return {first, second};
		}

		/** Calculates cell XY bounds directly from position members without copying vertices. */
		static Bounds2D CalculateVertexBounds(std::span<const SplineSurfaceVertex> vertices) noexcept
		{
			Bounds2D bounds{{INFINITY, INFINITY}, {-INFINITY, -INFINITY}};
			for (const auto& vertex : vertices)
			{
				for (std::size_t axis = 0; axis < 2; ++axis)
				{
					bounds.Minimum[axis] = (std::min)(bounds.Minimum[axis], vertex.Position[axis]);
					bounds.Maximum[axis] = (std::max)(bounds.Maximum[axis], vertex.Position[axis]);
				}
			}
			return bounds;
		}

		/** Computes the signed doubled XY area using widened intermediate differences. */
		static long double TriangleTurn(
			const Point3D& first,
			const Point3D& second,
			const Point3D& third) noexcept
		{
			return Turn({first[0], first[1]}, {second[0], second[1]}, {third[0], third[1]});
		}

		/** Computes a signed XY turn using widened intermediate differences. */
		static long double Turn(const Point2D& first, const Point2D& second, const Point2D& third) noexcept
		{
			return (static_cast<long double>(second[0]) - first[0]) *
				(static_cast<long double>(third[1]) - first[1]) -
				(static_cast<long double>(second[1]) - first[1]) *
				(static_cast<long double>(third[0]) - first[0]);
		}

		/** Checks that every coordinate in a fixed-size point is finite. */
		template <typename Point, std::size_t Count>
		static bool IsFinite(const std::array<Point, Count>& point) noexcept
		{
			for (const auto& value : point)
			{
				if (!std::isfinite(value))
				{
					return false;
				}
			}
			return true;
		}

		/** Implements strict AABB separation so boundary contacts continue to exact clipping. */
		static bool Disjoint(const Bounds2D& first, const Bounds2D& second) noexcept
		{
			return first.Maximum[0] < second.Minimum[0] || second.Maximum[0] < first.Minimum[0] ||
				first.Maximum[1] < second.Minimum[1] || second.Maximum[1] < first.Minimum[1];
		}

		/** Throws the requested numerical status when a validation or arithmetic predicate fails. */
		static void Require(bool condition, SplineOcclusionStatus status)
		{
			if (!condition)
			{
				throw status;
			}
		}
	};
}
