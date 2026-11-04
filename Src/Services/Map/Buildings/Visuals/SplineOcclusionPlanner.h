// Retains complete numerical spline-occlusion plans and reuses unchanged cells across compatible inputs.
#pragma once

#include "SplineOcclusionGeometry.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <stdexcept>
#include <vector>

namespace Services::Map::Buildings::Visuals
{
	/** Supplies caller-owned evidence for global context compatibility and exact per-cell numerical equivalence.
	 * SourceContextToken identifies one compatible source owner and lifetime/world epoch; it must remain stable
	 * across individual cell geometry edits and change on world reset or incompatible source replacement. Cell
	 * revisions certify each ordered cell's exact XYZ/UV geometry. Other revisions certify atlas, policy, and eligible
	 * ordinary-sprite quads. These are equality tokens, not hashes or publication generations.
	 */
	struct SplineOcclusionPlanningEvidence
	{
		std::uint64_t SourceContextToken = 0;
		std::uint64_t AtlasRevision = 0;
		std::uint64_t PolicyRevision = 0;
		std::uint64_t OccluderSetRevision = 0;
		bool Complete = false;
	};

	/** Owns one independent caller-retained reusable plan context, including bounded staging and clipping storage. */
	class SplineOcclusionPlanner final
	{
		/** Maps one spline-profile cell to flat dependency and visible-fragment ranges. */
		struct CellPlan
		{
			std::uint64_t GeometryRevision = 0;
			std::size_t FirstVertex = 0;
			std::size_t VertexCount = 0;
			std::size_t FirstIndex = 0;
			std::size_t IndexCount = 0;
			std::size_t DependencyBegin = 0;
			std::size_t DependencyCount = 0;
			std::size_t VisibleBegin = 0;
			std::size_t VisibleCount = 0;
		};

		/** Owns one complete or staging plan using flat vectors to avoid per-cell allocations. */
		struct PlanStorage
		{
			SplineOcclusionPlanningEvidence Evidence;
			std::size_t VertexCount = 0;
			std::size_t IndexCount = 0;
			bool Valid = false;
			bool Reusable = false;
			std::vector<std::uint64_t> CellRevisions;
			std::vector<CellPlan> Cells;
			std::vector<SplineOcclusionSpriteQuad> Dependencies;
			std::vector<SplineOcclusionTriangle> Visible;

			/** Clears plan values while retaining capacities for the next composition. */
			void Clear() noexcept
			{
				Evidence = {};
				VertexCount = 0;
				IndexCount = 0;
				Valid = false;
				Reusable = false;
				CellRevisions.clear();
				Cells.clear();
				Dependencies.clear();
				Visible.clear();
			}
		};

		PlanStorage Completed;
		PlanStorage Staging;
		SplineOcclusionWorkspace Workspace;
		SplineOcclusionStatus LastStatus = SplineOcclusionStatus::NotPrepared;

	public:
		/** Constructs an empty independent plan context with no shared or global cache state. */
		SplineOcclusionPlanner() = default;

		/** Prevents accidental deep copies of completed plans and retained numerical scratch. */
		SplineOcclusionPlanner(const SplineOcclusionPlanner&) = delete;

		/** Prevents accidental deep assignment of completed plans and retained numerical scratch. */
		SplineOcclusionPlanner& operator=(const SplineOcclusionPlanner&) = delete;

		/** Transfers caller-owned plan and scratch storage without copying its vectors. */
		SplineOcclusionPlanner(SplineOcclusionPlanner&&) noexcept = default;

		/** Transfers caller-owned plan and scratch storage without copying its vectors. */
		SplineOcclusionPlanner& operator=(SplineOcclusionPlanner&&) noexcept = default;

		/** Plans the supplied surface, reusing a complete whole plan or unchanged cells when evidence permits.
		 * Cell revisions must change whenever that ordered cell's XYZ/UV vertices, indices, or metadata change. The source
		 * context token tracks ownership/lifetime compatibility only and must not change for a cell-local edit. Atlas,
		 * policy, and occluder-set revisions certify their named global inputs; a placement fingerprint alone does not
		 * certify numerical geometry. Complete evidence tokens must be exact caller revisions, not content hashes.
		 * Input spans are consumed synchronously and never retained.
		 * @param vertices Prepared world-space surface vertices.
		 * @param indices Prepared indices partitioned by spline-profile cells.
		 * @param cells Accepted spline-profile cells in deterministic source order.
		 * @param cellRevisions Exact geometry evidence aligned positionally with cells.
		 * @param index Immutable prepared occluder index for this planning operation.
		 * @param evidence Relevant source, atlas, numerical-policy and eligible-occluder evidence.
		 * @return Complete on success, or the validation, numerical, preparation, or allocation failure.
		 */
		SplineOcclusionStatus Plan(
			std::span<const SplineSurfaceVertex> vertices,
			std::span<const std::uint32_t> indices,
			std::span<const SplineSurfaceCell> cells,
			std::span<const std::uint64_t> cellRevisions,
			const SplineOcclusionIndex& index,
			const SplineOcclusionPlanningEvidence& evidence) noexcept
		{
			LastStatus = index.Status();
			if (LastStatus != SplineOcclusionStatus::Complete)
			{
				return LastStatus;
			}
			if (cellRevisions.size() != cells.size())
			{
				LastStatus = SplineOcclusionStatus::InvalidInput;
				return LastStatus;
			}
			if (CanReuseWholePlan(vertices, indices, cells, cellRevisions, evidence))
			{
				LastStatus = SplineOcclusionStatus::Complete;
				return LastStatus;
			}
			return BuildPlan(vertices, indices, cells, cellRevisions, index, evidence);
		}

		/** Reports the latest planning result, including a failed attempt that left prior complete storage private. */
		SplineOcclusionStatus Status() const noexcept
		{
			return LastStatus;
		}

		/** Returns visible fragments until the next Plan call, only when that latest operation completed successfully. */
		std::span<const SplineOcclusionTriangle> VisibleTriangles() const noexcept
		{
			if (LastStatus != SplineOcclusionStatus::Complete || !Completed.Valid)
			{
				return {};
			}
			return Completed.Visible;
		}

		/** Reports whether the latest complete plan has sufficient evidence for later reuse. */
		bool IsReusable() const noexcept
		{
			return LastStatus == SplineOcclusionStatus::Complete && Completed.Valid && Completed.Reusable;
		}

	private:
		/** Returns true when all complete evidence and span sizes certify an unchanged plan. */
		bool CanReuseWholePlan(
			std::span<const SplineSurfaceVertex> vertices,
			std::span<const std::uint32_t> indices,
			std::span<const SplineSurfaceCell> cells,
			std::span<const std::uint64_t> cellRevisions,
			const SplineOcclusionPlanningEvidence& evidence) const noexcept
		{
			return evidence.Complete && Completed.Valid && Completed.Reusable &&
				SameGlobalEvidence(Completed.Evidence, evidence) && Completed.VertexCount == vertices.size() &&
				Completed.IndexCount == indices.size() && Completed.Cells.size() == cells.size() &&
				std::equal(Completed.CellRevisions.begin(), Completed.CellRevisions.end(), cellRevisions.begin());
		}

		/** Builds a staged plan while retaining the prior complete plan until every cell succeeds. */
		SplineOcclusionStatus BuildPlan(
			std::span<const SplineSurfaceVertex> vertices,
			std::span<const std::uint32_t> indices,
			std::span<const SplineSurfaceCell> cells,
			std::span<const std::uint64_t> cellRevisions,
			const SplineOcclusionIndex& index,
			const SplineOcclusionPlanningEvidence& evidence) noexcept
		{
			Staging.Clear();
			try
			{
				InitializeStaging(vertices, indices, cells, cellRevisions, evidence);
				const bool canReuseCells = CanReuseCells(cells, evidence);
				const auto status = BuildCells(vertices, indices, cells, cellRevisions, index, canReuseCells);
				Require(status == SplineOcclusionStatus::Complete, status);
				CommitStaging();
				LastStatus = SplineOcclusionStatus::Complete;
			}
			catch (SplineOcclusionStatus status)
			{
				return Fail(status);
			}
			catch (const std::bad_alloc&)
			{
				return Fail(SplineOcclusionStatus::AllocationFailure);
			}
			catch (const std::length_error&)
			{
				return Fail(SplineOcclusionStatus::AllocationFailure);
			}
			return LastStatus;
		}

		/** Initializes plan evidence and reusable flat storage for one complete staging attempt. */
		void InitializeStaging(
			std::span<const SplineSurfaceVertex> vertices,
			std::span<const std::uint32_t> indices,
			std::span<const SplineSurfaceCell> cells,
			std::span<const std::uint64_t> cellRevisions,
			const SplineOcclusionPlanningEvidence& evidence)
		{
			Staging.Evidence = evidence;
			Staging.VertexCount = vertices.size();
			Staging.IndexCount = indices.size();
			Staging.CellRevisions.assign(cellRevisions.begin(), cellRevisions.end());
			Staging.Cells.reserve(cells.size());
		}

		/** Queries one cell once, records its unfiltered dependencies, and reuses or reclips its visible result. */
		SplineOcclusionStatus BuildCells(
			std::span<const SplineSurfaceVertex> vertices,
			std::span<const std::uint32_t> indices,
			std::span<const SplineSurfaceCell> cells,
			std::span<const std::uint64_t> cellRevisions,
			const SplineOcclusionIndex& index,
			bool canReuseCells)
		{
			std::size_t nextVertex = 0;
			std::size_t nextIndex = 0;
			for (std::size_t cellIndex = 0; cellIndex < cells.size(); ++cellIndex)
			{
				BuildCell(vertices, indices, cells[cellIndex], cellRevisions[cellIndex], cellIndex,
					index, canReuseCells);
				nextVertex += cells[cellIndex].VertexCount;
				nextIndex += cells[cellIndex].IndexCount;
			}
			Require(nextVertex == vertices.size() && nextIndex == indices.size(), SplineOcclusionStatus::InvalidInput);
			return SplineOcclusionStatus::Complete;
		}

		/** Validates one cell, performs its sole BVH query, and appends dependency and output ranges. */
		void BuildCell(
			std::span<const SplineSurfaceVertex> vertices,
			std::span<const std::uint32_t> indices,
			const SplineSurfaceCell& cell,
			std::uint64_t geometryRevision,
			std::size_t cellIndex,
			const SplineOcclusionIndex& index,
			bool canReuseCells)
		{
			SplineOcclusionGeometry::ValidateCellRanges(vertices, indices, cell,
				ExpectedVertexStart(), ExpectedIndexStart());
			SplineOcclusionGeometry::ValidateCellIndices(indices, cell);
			const auto bounds = SplineOcclusionGeometry::CalculateVertexBounds(
				vertices.subspan(cell.FirstVertex, cell.VertexCount));
			SplineOcclusionGeometry::QueryCandidates(bounds, index, Workspace);
			const auto dependencyRange = AppendCurrentDependencies(index);
			const bool reused = canReuseCells && CanReuseCell(cell, geometryRevision, cellIndex, dependencyRange);
			const auto visibleBegin = Staging.Visible.size();
			if (reused)
			{
				AppendPreviousVisible(cellIndex);
			}
			else
			{
				ReclipCell(vertices, indices, cell, index);
			}
			Staging.Cells.push_back({geometryRevision, cell.FirstVertex, cell.VertexCount, cell.FirstIndex,
				cell.IndexCount, dependencyRange.first, dependencyRange.second, visibleBegin,
				Staging.Visible.size() - visibleBegin});
		}

		/** Returns the expected contiguous input cursor before validating the next cell. */
		std::size_t ExpectedVertexStart() const noexcept
		{
			return Staging.Cells.empty() ? 0 : Staging.Cells.back().FirstVertex + Staging.Cells.back().VertexCount;
		}

		/** Returns the expected contiguous index cursor before validating the next cell. */
		std::size_t ExpectedIndexStart() const noexcept
		{
			return Staging.Cells.empty() ? 0 : Staging.Cells.back().FirstIndex + Staging.Cells.back().IndexCount;
		}

		/** Copies this cell's already sorted BVH candidate values into flat staging evidence. */
		std::array<std::size_t, 2> AppendCurrentDependencies(const SplineOcclusionIndex& index)
		{
			const auto begin = Staging.Dependencies.size();
			for (const auto candidate : Workspace.Candidates)
			{
				Staging.Dependencies.push_back(index.Occluders[candidate].Value);
			}
			const auto end = Staging.Dependencies.size();
			return {begin, end - begin};
		}

		/** Reuses prior output only at the same cell position with equal geometry revision and candidate list. */
		bool CanReuseCell(
			const SplineSurfaceCell& cell,
			std::uint64_t geometryRevision,
			std::size_t cellIndex,
			const std::array<std::size_t, 2>& dependencyRange) const noexcept
		{
			if (cellIndex >= Completed.Cells.size())
			{
				return false;
			}
			if (Completed.CellRevisions[cellIndex] != geometryRevision)
			{
				return false;
			}
			const auto& previous = Completed.Cells[cellIndex];
			if (previous.GeometryRevision != geometryRevision || previous.FirstVertex != cell.FirstVertex ||
				previous.VertexCount != cell.VertexCount || previous.FirstIndex != cell.FirstIndex ||
				previous.IndexCount != cell.IndexCount || previous.DependencyCount != dependencyRange[1])
			{
				return false;
			}
			return SameDependencies(previous.DependencyBegin, dependencyRange[0], dependencyRange[1]);
		}

		/** Compares prior and current BVH-ordered candidates by identity, corners and top height. */
		bool SameDependencies(
			std::size_t previousBegin,
			std::size_t currentBegin,
			std::size_t count) const noexcept
		{
			for (std::size_t offset = 0; offset < count; ++offset)
			{
				if (!SameDependencyGeometry(Completed.Dependencies[previousBegin + offset],
					Staging.Dependencies[currentBegin + offset]))
				{
					return false;
				}
			}
			return true;
		}

		/** Reuses previous visible triangles or invokes the existing core with the already queried candidates. */
		void ReclipCell(
			std::span<const SplineSurfaceVertex> vertices,
			std::span<const std::uint32_t> indices,
			const SplineSurfaceCell& cell,
			const SplineOcclusionIndex& index)
		{
			Workspace.Visible.clear();
			Require(SplineOcclusionGeometry::ClipCellWithCandidates(vertices, indices, cell, index, Workspace),
				SplineOcclusionStatus::NumericalFailure);
			Staging.Visible.insert(Staging.Visible.end(), Workspace.Visible.begin(), Workspace.Visible.end());
		}

		/** Appends a prior cell's completed fragments without rerunning triangle preparation or clipping. */
		void AppendPreviousVisible(std::size_t cellIndex)
		{
			const auto& previous = Completed.Cells[cellIndex];
			const auto begin = Completed.Visible.begin() + previous.VisibleBegin;
			Staging.Visible.insert(Staging.Visible.end(), begin, begin + previous.VisibleCount);
		}

		/** Allows positional cell reuse only within a compatible source, atlas, policy, and complete evidence context.
		 * Cell-count changes disable partial reuse; reordered cells are not identity-remapped.
		 */
		bool CanReuseCells(
			std::span<const SplineSurfaceCell> cells,
			const SplineOcclusionPlanningEvidence& evidence) const noexcept
		{
			return evidence.Complete && Completed.Valid && Completed.Reusable &&
				SameContextAndPolicyEvidence(Completed.Evidence, evidence) &&
				Completed.Cells.size() == cells.size() &&
				Completed.CellRevisions.size() == cells.size();
		}

		/** Publishes staging atomically and retains prior vector capacities in the alternate buffer. */
		void CommitStaging() noexcept
		{
			Staging.Valid = true;
			Staging.Reusable = Staging.Evidence.Complete;
			std::swap(Completed, Staging);
		}

		/** Clears failed staging while leaving the previous complete plan inaccessible for this failed call. */
		SplineOcclusionStatus Fail(SplineOcclusionStatus status) noexcept
		{
			Staging.Clear();
			LastStatus = status;
			return LastStatus;
		}

		/** Compares whole-plan evidence including the eligible ordinary occluder set revision. */
		static bool SameGlobalEvidence(
			const SplineOcclusionPlanningEvidence& first,
			const SplineOcclusionPlanningEvidence& second) noexcept
		{
			return SameContextAndPolicyEvidence(first, second) &&
				first.OccluderSetRevision == second.OccluderSetRevision;
		}

		/** Compares source lifetime, atlas, and policy evidence independently of cell revisions. */
		static bool SameContextAndPolicyEvidence(
			const SplineOcclusionPlanningEvidence& first,
			const SplineOcclusionPlanningEvidence& second) noexcept
		{
			return first.SourceContextToken == second.SourceContextToken &&
				first.AtlasRevision == second.AtlasRevision &&
				first.PolicyRevision == second.PolicyRevision;
		}

		/** Requires candidate identity plus exact bitwise corners and explicit top-height equality. */
		static bool SameDependencyGeometry(
			const SplineOcclusionSpriteQuad& first,
			const SplineOcclusionSpriteQuad& second) noexcept
		{
			if (first.Key != second.Key ||
				std::bit_cast<std::uint64_t>(first.TopHeight) != std::bit_cast<std::uint64_t>(second.TopHeight))
			{
				return false;
			}
			for (std::size_t corner = 0; corner < first.Corners.size(); ++corner)
			{
				for (std::size_t axis = 0; axis < 2; ++axis)
				{
					if (std::bit_cast<std::uint64_t>(first.Corners[corner][axis]) !=
						std::bit_cast<std::uint64_t>(second.Corners[corner][axis]))
					{
						return false;
					}
				}
			}
			return true;
		}

		/** Throws the supplied numerical status to unwind an incomplete staging operation. */
		static void Require(bool condition, SplineOcclusionStatus status)
		{
			if (!condition)
			{
				throw status;
			}
		}
	};
}
