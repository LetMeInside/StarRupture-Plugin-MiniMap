// Development-only Building Map timing, interval counters and bounded presentation histories.
#pragma once

#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
// Set to 0 in Client Debug Experimental to omit per-insertion ordinary batch capacity observations.
#define MINIMAP_ORDINARY_BATCH_CAPACITY_DIAGNOSTICS 0
// Set to 0 in Client Debug builds to omit ordinary sprite range and batch sampling.
#define MINIMAP_ORDINARY_RANGE_SAMPLING_DIAGNOSTICS 0

#include "../../Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>

namespace Services::Map::Buildings
{
	/** Aggregates game-thread work and hands sampled presentation timestamps to draw owners. */
	class BuildingMapPerformanceDiagnostics final
	{
	public:
		using Clock = std::chrono::steady_clock;
		using Count = unsigned long long;

		/** Counts measured wall durations on their invoking thread. */
		struct Duration
		{
			Count Samples = 0;
			double TotalMs = 0.0;
			double MaxMs = 0.0;

			/** Adds one timing in milliseconds. */
			void Add(double elapsed) noexcept
			{
				++Samples;
				TotalMs += elapsed;
				MaxMs = (std::max)(MaxMs, elapsed);
			}

			/** Combines a callback's local timings with the interval aggregate. */
			void Merge(const Duration& other) noexcept
			{
				Samples += other.Samples;
				TotalMs += other.TotalMs;
				MaxMs = (std::max)(MaxMs, other.MaxMs);
			}

			/** Formats the invocation count and total, mean and maximum in milliseconds. */
			std::array<char, 112> Describe() const noexcept
			{
				std::array<char, 112> result{};
				std::snprintf(result.data(), result.size(), "{n:%llu total:%.3f mean:%.3f max:%.3f}",
					Samples, TotalMs, Samples == 0 ? 0.0 : TotalMs / static_cast<double>(Samples), MaxMs);
				return result;
			}
		};

		/** Times one existing scope. A null aggregate performs no clock reads. */
		class Timer final
		{
		public:
			/** Starts a measurement for an aggregate or leaves this scope inactive. */
			explicit Timer(Duration* output, Duration* nested = nullptr) noexcept
				: output_(output), nested_(nested)
			{
				if (output_ != nullptr)
				{
					started_ = Clock::now();
				}
			}

			/** Finishes a measurement if the caller has yet to finish it. */
			~Timer() noexcept { Finish(); }

			/** Adds the same elapsed sample to an optional per-publication aggregate. */
			void TrackNested(Duration* nested) noexcept { nested_ = nested; }

			/** Adds the elapsed wall time once and returns it in milliseconds. */
			double Finish() noexcept
			{
				if (output_ == nullptr)
				{
					return 0.0;
				}
				const double elapsed = Milliseconds(Clock::now() - started_);
				output_->Add(elapsed);
				if (nested_ != nullptr)
				{
					nested_->Add(elapsed);
				}
				output_ = nullptr;
				return elapsed;
			}

			Timer(const Timer&) = delete;
			Timer& operator=(const Timer&) = delete;

		private:
			Duration* output_;
			Duration* nested_;
			Clock::time_point started_{};
		};

		/** Interval totals written only on the game thread. */
		struct GameWork
		{
			Duration Scan;
			Duration CollectorUpdate;
			Duration PublicationSelection;
			Duration SectionInitialization;
			Duration OccurrenceOrder;
			Duration OccurrenceBatches;
			Duration DemandLookup;
			Duration ReadyGeometrySample;
			Duration Continuation;
			Duration Indivisible;
			Duration Admission;
			Duration Assembly;
			Duration SpriteCopy;
			Duration SplineCopy;
			Duration Binding;
			Duration Rebind;
			Duration Seal;
			Duration Snapshot;
			Duration AtlasUpload;
			Count SectionSlices = 0;
			Count SectionWorkUnits = 0;
			Count SlicesWithoutLookup = 0;
			Count ResourceLookups = 0;
			Count ResidentLookups = 0;
			Count GeometryAttempts = 0;
			Count ClipAttempts = 0;
			Count DrawableSprites = 0;
			Count AssembliesStarted = 0;
			Count AssembliesSealed = 0;
			Count ResumedOnLaterTick = 0;
			Count SameGenerationSeals = 0;
			Count PriorTrianglesRecopied = 0;
			Count SpriteTrianglesCopied = 0;
			Count SplineTrianglesCopied = 0;
			Count AssemblyBindingsBuilt = 0;
			Count BindingsRebound = 0;
			Count BindingLookups = 0;
			Count SnapshotReplacements = 0;
			Count SnapshotHandles = 0;
			Count MiniMapVisibleTicks = 0;
			Count MiniMapHiddenTicks = 0;
			Count SplineSamplingIntervals = 0;
			Count SplineGeneratedTriangles = 0;
			Count SplinePostOcclusionTriangles = 0;
			Count SplinePostSectionClipTriangles = 0;
			Count SplineOcclusionRemovedTriangles = 0;
			Count SplineOcclusionFragmentTriangles = 0;
			Count SplineSectionClipRemovedTriangles = 0;
			Count SplineSectionClipFragmentTriangles = 0;
			Count SplineCandidateAttempts = 0;
			Count SplineCandidateSelections = 0;
			Count SplineCandidateRejections = 0;
			Count SplineRejectedCells = 0;
			Count SplineTerminalOmissions = 0;
			Count SplineSelectedTriangles = 0;
		};

        /** Exact pass counts and elapsed wall time. Submitted counts successfully drawn triangles. */
		struct DrawPass
		{
			Duration Time;
			Duration OrdinaryCanvasSubmission;
			Duration SplineTraversal;
			Duration SplineCanvasSubmission;
			Duration ProjectionSample;
			Duration CanvasSample;
			Count Sections = 0;
			Count Visited = 0;
			Count Submitted = 0;
			Count NonfiniteProjectedXY = 0;
			Count NonfiniteSignedArea = 0;
			Count ZeroSignedArea = 0;
			Count OutsideScreenClip = 0;
			Count NonfiniteUv = 0;
			Count Accepted = 0;
			Count VertexCapacityIncreases = 0;
			Count IndexCapacityIncreases = 0;
			Count MaximumVertexCount = 0;
			Count MaximumIndexCount = 0;
			Count MaximumVertexCapacity = 0;
			Count MaximumIndexCapacity = 0;
			Count VertexCapacityGrowthElements = 0;
			Count IndexCapacityGrowthElements = 0;
			Count CanvasCalls = 0;
			Count FailedSubmissions = 0;
			Count BoundRanges = 0;
			Count UnboundRanges = 0;
			Count SplineRangesVisited = 0;
			Count SplineRangesCulled = 0;
			Count SplineTrianglesInCulledRanges = 0;
			Count SplineGroupsVisited = 0;
			Count SplineGroupsCulled = 0;
			Count SplineTrianglesInCulledGroups = 0;

			/** Adds one callback's measurements to the corresponding pass. */
			void Merge(const DrawPass& other) noexcept
			{
				Time.Merge(other.Time);
				OrdinaryCanvasSubmission.Merge(other.OrdinaryCanvasSubmission);
				SplineTraversal.Merge(other.SplineTraversal);
				SplineCanvasSubmission.Merge(other.SplineCanvasSubmission);
				ProjectionSample.Merge(other.ProjectionSample);
				CanvasSample.Merge(other.CanvasSample);
				Sections += other.Sections;
				Visited += other.Visited;
				Submitted += other.Submitted;
				NonfiniteProjectedXY += other.NonfiniteProjectedXY;
				NonfiniteSignedArea += other.NonfiniteSignedArea;
				ZeroSignedArea += other.ZeroSignedArea;
				OutsideScreenClip += other.OutsideScreenClip;
				NonfiniteUv += other.NonfiniteUv;
				Accepted += other.Accepted;
				VertexCapacityIncreases += other.VertexCapacityIncreases;
				IndexCapacityIncreases += other.IndexCapacityIncreases;
				MaximumVertexCount = (std::max)(MaximumVertexCount, other.MaximumVertexCount);
				MaximumIndexCount = (std::max)(MaximumIndexCount, other.MaximumIndexCount);
				MaximumVertexCapacity = (std::max)(MaximumVertexCapacity, other.MaximumVertexCapacity);
				MaximumIndexCapacity = (std::max)(MaximumIndexCapacity, other.MaximumIndexCapacity);
				VertexCapacityGrowthElements += other.VertexCapacityGrowthElements;
				IndexCapacityGrowthElements += other.IndexCapacityGrowthElements;
				CanvasCalls += other.CanvasCalls;
				FailedSubmissions += other.FailedSubmissions;
				BoundRanges += other.BoundRanges;
				UnboundRanges += other.UnboundRanges;
				SplineRangesVisited += other.SplineRangesVisited;
				SplineRangesCulled += other.SplineRangesCulled;
				SplineTrianglesInCulledRanges += other.SplineTrianglesInCulledRanges;
				SplineGroupsVisited += other.SplineGroupsVisited;
				SplineGroupsCulled += other.SplineGroupsCulled;
				SplineTrianglesInCulledGroups += other.SplineTrianglesInCulledGroups;
			}
		};

		/** Adapter counts and sparse timings for immediate Building Map texture submissions. */
		struct TextureWork
		{
			Duration AdmissionSample;
			Duration LedgerWaitSample;
			Duration LedgerHoldSample;
			Duration BackendSample;
			Count Calls = 0;
			Count Admissions = 0;
			Count GateAcquisitions = 0;
			Count LedgerAcquisitions = 0;

			/** Adds callback-local adapter evidence to the interval. */
			void Merge(const TextureWork& other) noexcept
			{
				AdmissionSample.Merge(other.AdmissionSample);
				LedgerWaitSample.Merge(other.LedgerWaitSample);
				LedgerHoldSample.Merge(other.LedgerHoldSample);
				BackendSample.Merge(other.BackendSample);
				Calls += other.Calls;
				Admissions += other.Admissions;
				GateAcquisitions += other.GateAcquisitions;
				LedgerAcquisitions += other.LedgerAcquisitions;
			}
		};

		/** Callback-local accumulators, merged by the owning layer at its existing diagnostic lock. */
		struct RenderWork
		{
			Duration Draw;
			DrawPass Ordinary;
			DrawPass Spline;
			DrawPass Overlay;
			TextureWork Texture;
			Count MissingSnapshot = 0;

			/** Aggregates one frame without walking what it rendered. */
			void Merge(const RenderWork& frame) noexcept
			{
				Draw.Merge(frame.Draw);
				Ordinary.Merge(frame.Ordinary);
				Spline.Merge(frame.Spline);
				Overlay.Merge(frame.Overlay);
				Texture.Merge(frame.Texture);
				MissingSnapshot += frame.MissingSnapshot;
			}
		};

		/** One sampled sprite-bearing publication. Only the first submission stamp crosses threads. */
		struct PresentationTiming
		{
			Clock::time_point FirstIncludedSprite{};
			Clock::time_point Admitted{};
			Clock::time_point Sealed{};
			Clock::time_point Adopted{};
			std::atomic<Clock::duration::rep> FirstSubmissionTicks{0};
			Duration Assembly;
			Count Triangles = 0;

			/** Records the first callback reaching a sprite submission for this presentation. */
			void RecordFirstSubmission() noexcept
			{
				if (FirstSubmissionTicks.load(std::memory_order_relaxed) == 0)
				{
					auto unset = Clock::duration::rep{0};
					FirstSubmissionTicks.compare_exchange_strong(
						unset, Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
				}
			}
		};

		/** One diagnostic-only reachability scope for downstream canvas calls on this thread. */
		class RenderScope final
		{
		public:
			/** Activates the frame's measurements for its synchronous draw. */
			explicit RenderScope(RenderWork* current) noexcept : previous_(ActiveRender)
			{
				ActiveRender = current;
			}

			/** Restores the caller's scope even if a canvas call throws. */
			~RenderScope() noexcept { ActiveRender = previous_; }

			RenderScope(const RenderScope&) = delete;
			RenderScope& operator=(const RenderScope&) = delete;

		private:
			RenderWork* previous_;
		};

		/** Converts a steady-clock duration to milliseconds. */
		static double Milliseconds(Clock::duration elapsed) noexcept
		{
			return std::chrono::duration<double, std::milli>(elapsed).count();
		}

		/** Returns the current game-thread interval. */
		static GameWork& Game() noexcept
		{
			static thread_local GameWork work;
			return work;
		}

		/** Samples one ready-geometry attempt per 64 attempts across slices. */
		static Duration* ReadyGeometrySample() noexcept
		{
			static thread_local Count sequence = 0;
			++Game().GeometryAttempts;
			return (++sequence & 63) == 1 ? &Game().ReadyGeometrySample : nullptr;
		}

		/** Samples one adapter call per 256 calls across Building Map render callbacks. */
		static bool SampleTextureCall() noexcept
		{
			static thread_local Count sequence = 0;
			return (++sequence & 255) == 1;
		}

		/** Samples one bound sprite range per 256 to split projection and canvas work. */
		static bool SampleDrawRange() noexcept
		{
			static thread_local Count sequence = 0;
			return (++sequence & 255) == 1;
		}

		/** Identifies a newly allocated preparing scene independently of its content hash. */
		static std::uint64_t NextGeneration() noexcept
		{
			static thread_local std::uint64_t sequence = 0;
			return ++sequence;
		}

		/** Starts a new diagnostic session, retiring game-thread references to old histories. */
		static void ResetSession() noexcept
		{
			Game() = {};
			Histories() = {};
			IntervalStart() = Clock::now();
		}

		/** Arms a deterministic slot for one of the first four selected dense Sections. */
		static void Select(std::int64_t x, std::int64_t y, std::uint64_t hash, std::size_t count) noexcept
		{
			if (count < 256 || Find(x, y, hash) != nullptr)
			{
				return;
			}
			for (auto& history : Histories())
			{
				if (history.Selected == Clock::time_point{})
				{
					history.X = x;
					history.Y = y;
					history.Hash = hash;
					history.Occurrences = count;
					history.Selected = Clock::now();
					return;
				}
			}
		}

		/** Records the first drawable append since the last sampled admission. */
		static void RecordDrawable(std::int64_t x, std::int64_t y, std::uint64_t hash,
			std::uint64_t generation) noexcept
		{
			auto* history = Find(x, y, hash);
			if (history == nullptr ||
				(history->Generation != 0 && history->Generation != generation))
			{
				return;
			}
			history->Generation = generation;
			history->PendingDrawable = Clock::now();
			if (history->FirstDrawable == Clock::time_point{})
			{
				history->FirstDrawable = history->PendingDrawable;
			}
		}

		/** Admits a bounded timing owner for this generation's initial publication and three updates. */
		static std::shared_ptr<PresentationTiming> Admit(std::int64_t x, std::int64_t y,
			std::uint64_t hash, std::uint64_t generation) noexcept
		{
			auto* history = Find(x, y, hash);
			if (history == nullptr || history->Generation != generation || history->Used == history->Updates.size())
			{
				return {};
			}
			try
			{
				auto timing = std::make_shared<PresentationTiming>();
				timing->FirstIncludedSprite = history->PendingDrawable;
				timing->Admitted = Clock::now();
				history->PendingDrawable = {};
				history->Updates[history->Used++] = timing;
				return timing;
			}
			catch (...)
			{
				return {};
			}
		}

		/** Returns an elapsed interval on the existing game tick, including idle or hidden intervals. */
		static double TakeInterval() noexcept
		{
			const auto now = Clock::now();
			auto& started = IntervalStart();
			const double elapsed = Milliseconds(now - started);
			if (elapsed < 10000.0)
			{
				return 0.0;
			}
			started = now;
			return elapsed;
		}

		/** Reports and clears game-thread aggregates and reports changed bounded histories. */
		static void ReportGame(double interval, Count dirtyWaiting, bool miniMapVisible) noexcept;

		/** Reports a completed layer interval using its render-thread aggregates. */
		static void ReportDraw(double interval, const RenderWork& work) noexcept;

		static inline thread_local RenderWork* ActiveRender = nullptr;

	private:
		/** Holds one selected generation and up to four immutable presentation owners' observations. */
		struct SectionHistory
		{
			std::int64_t X = 0;
			std::int64_t Y = 0;
			std::uint64_t Hash = 0;
			std::uint64_t Generation = 0;
			std::size_t Occurrences = 0;
			Clock::time_point Selected{};
			Clock::time_point FirstDrawable{};
			Clock::time_point PendingDrawable{};
			std::array<std::shared_ptr<PresentationTiming>, 4> Updates{};
			std::array<unsigned, 4> ReportedStages{};
			std::size_t Used = 0;
			unsigned ReportedPreparation = 0;
		};

		/** Returns session histories owned by the game thread. */
		static std::array<SectionHistory, 4>& Histories() noexcept
		{
			static thread_local std::array<SectionHistory, 4> histories;
			return histories;
		}

		/** Returns the start time of the current diagnostic interval. */
		static Clock::time_point& IntervalStart() noexcept
		{
			static thread_local auto started = Clock::now();
			return started;
		}

		/** Looks up the armed selection without touching any production index. */
		static SectionHistory* Find(std::int64_t x, std::int64_t y, std::uint64_t hash) noexcept
		{
			for (auto& history : Histories())
			{
				if (history.Selected != Clock::time_point{} && history.X == x && history.Y == y &&
					history.Hash == hash)
				{
					return &history;
				}
			}
			return nullptr;
		}

		/** Logs only changed milestones since the previous interval. */
		static void ReportHistories(MiniMap::Services::ILog& log) noexcept;

		/** Logs a changed presentation stage using times relative to publication selection. */
		static void ReportPresentation(MiniMap::Services::ILog& log, SectionHistory& history,
			std::size_t index) noexcept;

		/** Logs one draw pass with exact counts and inclusive elapsed wall time. */
		static void ReportDrawPass(MiniMap::Services::ILog& log, const char* name, const DrawPass& pass) noexcept;
	};

	inline void BuildingMapPerformanceDiagnostics::ReportGame(
		double interval, Count dirtyWaiting, bool miniMapVisible) noexcept
	{
		const auto work = Game();
		Game() = {};
		auto* log = MiniMap::Services::TryLog();
		if (log == nullptr)
		{
			return;
		}
		using MiniMap::Services::Level;
		using MiniMap::Services::Logf;
		Logf(*log, Level::Info, "MiniMap: BuildingMap perf_activity intervalMs=%.1f minimapVisibleAtReport=%u "
			"visibilityTickSamples={visible:%llu hidden:%llu} "
			"wallMs collectorUpdateInclusive=%s collectorScanNested=%s selection=%s "
			"sectionVisits=%llu sectionWorkUnits=%llu gpuSeals=%llu atlasUploads=%s snapshotStores=%llu dirtyAwaitingAdmission=%llu",
			interval, static_cast<unsigned>(miniMapVisible), work.MiniMapVisibleTicks, work.MiniMapHiddenTicks,
			work.CollectorUpdate.Describe().data(),
			work.Scan.Describe().data(), work.PublicationSelection.Describe().data(),
			work.SectionSlices, work.SectionWorkUnits, work.AssembliesSealed, work.AtlasUpload.Describe().data(),
			work.SnapshotReplacements, dirtyWaiting);
		const double geometryEstimate = work.ReadyGeometrySample.Samples == 0 ?
			(work.GeometryAttempts == 0 ? 0.0 : -1.0) :
			work.ReadyGeometrySample.TotalMs * static_cast<double>(work.GeometryAttempts) /
				static_cast<double>(work.ReadyGeometrySample.Samples);
		Logf(*log, Level::Info, "MiniMap: BuildingMap perf_prepare wallMs initializationInclusive=%s "
			"orderingNested=%s occurrenceBatchInclusive=%s demandLookupNested=%s readyGeometrySample1in64Nested=%s "
			"readyGeometryEstimatedTotalMs=%.3f continuationInclusive=%s "
			"measuredIndivisibleNested=%s lookupAttempts=%llu residentLookups=%llu slicesWithoutLookup=%llu "
			"geometryAttempts=%llu clips=%llu drawableAppends=%llu",
			work.SectionInitialization.Describe().data(), work.OccurrenceOrder.Describe().data(),
			work.OccurrenceBatches.Describe().data(), work.DemandLookup.Describe().data(),
			work.ReadyGeometrySample.Describe().data(),
			geometryEstimate, work.Continuation.Describe().data(), work.Indivisible.Describe().data(),
			work.ResourceLookups, work.ResidentLookups, work.SlicesWithoutLookup, work.GeometryAttempts,
			work.ClipAttempts, work.DrawableSprites);
		Logf(*log, Level::Info, "MiniMap: BuildingMap perf_spline attemptedCandidates=%llu rejectedCandidates=%llu "
			"successfulSelections=%llu samplingIntervals=%llu rejectedCells=%llu terminalOmissions=%llu "
			"attemptedTriangles={generated:%llu postOcclusion:%llu postSectionClip:%llu} "
			"selectedTriangles=%llu occlusionRemoved=%llu occlusionFragmentationAdded=%llu "
			"sectionClipRemoved=%llu sectionClipFragmentationAdded=%llu",
			work.SplineCandidateAttempts, work.SplineCandidateRejections, work.SplineCandidateSelections,
			work.SplineSamplingIntervals, work.SplineRejectedCells, work.SplineTerminalOmissions,
			work.SplineGeneratedTriangles, work.SplinePostOcclusionTriangles,
			work.SplinePostSectionClipTriangles, work.SplineSelectedTriangles,
			work.SplineOcclusionRemovedTriangles, work.SplineOcclusionFragmentTriangles,
			work.SplineSectionClipRemovedTriangles, work.SplineSectionClipFragmentTriangles);
		Logf(*log, Level::Info, "MiniMap: BuildingMap perf_gpu wallMs admissionInclusive=%s assemblyNested=%s "
			"spriteGatherCopyNested=%s splineCopyNested=%s atlasBindNestedInAssembly=%s rebindNestedInSnapshot=%s "
			"sealNested=%s snapshotInclusive=%s started=%llu resumedLaterTick=%llu sameGenerationSeals=%llu "
			"priorTrianglesRecopied=%llu spriteTriangleCopies=%llu splineTriangleCopies=%llu "
			"assemblyBindingsBuilt=%llu bindingsRebound=%llu bindingLookups=%llu snapshotHandlesStored=%llu",
			work.Admission.Describe().data(), work.Assembly.Describe().data(), work.SpriteCopy.Describe().data(),
			work.SplineCopy.Describe().data(), work.Binding.Describe().data(), work.Rebind.Describe().data(),
			work.Seal.Describe().data(),
			work.Snapshot.Describe().data(), work.AssembliesStarted, work.ResumedOnLaterTick,
			work.SameGenerationSeals, work.PriorTrianglesRecopied, work.SpriteTrianglesCopied,
			work.SplineTrianglesCopied, work.AssemblyBindingsBuilt, work.BindingsRebound,
			work.BindingLookups, work.SnapshotHandles);
		ReportHistories(*log);
	}

	inline void BuildingMapPerformanceDiagnostics::ReportHistories(MiniMap::Services::ILog& log) noexcept
	{
		for (auto& history : Histories())
		{
			if (history.Selected == Clock::time_point{})
			{
				continue;
			}
			const unsigned stage = history.FirstDrawable == Clock::time_point{} ? 1 : 2;
			if (stage != history.ReportedPreparation)
			{
				history.ReportedPreparation = stage;
				MiniMap::Services::Logf(log, MiniMap::Services::Level::Info,
					"MiniMap: BuildingMap perf_section grid=(%lld,%lld) hash=0x%016llx generation=%llu "
					"occurrences=%llu selectedClockMs=%.3f selectedToDrawableMs=%.3f pending=-1",
					static_cast<long long>(history.X), static_cast<long long>(history.Y),
					static_cast<Count>(history.Hash), static_cast<Count>(history.Generation),
					static_cast<Count>(history.Occurrences), Milliseconds(history.Selected.time_since_epoch()),
					stage == 1 ? -1.0 : Milliseconds(history.FirstDrawable - history.Selected));
			}
			for (std::size_t index = 0; index < history.Used; ++index)
			{
				ReportPresentation(log, history, index);
			}
		}
	}

	inline void BuildingMapPerformanceDiagnostics::ReportPresentation(
		MiniMap::Services::ILog& log, SectionHistory& history, std::size_t index) noexcept
	{
		const auto& update = *history.Updates[index];
		const auto drawTicks = update.FirstSubmissionTicks.load(std::memory_order_relaxed);
		const unsigned stage = drawTicks != 0 ? 4 : update.Adopted != Clock::time_point{} ? 3 :
			update.Sealed != Clock::time_point{} ? 2 : 1;
		if (stage == history.ReportedStages[index])
		{
			return;
		}
		history.ReportedStages[index] = stage;
		const Clock::time_point draw{Clock::duration{drawTicks}};
		MiniMap::Services::Logf(log, MiniMap::Services::Level::Info,
			"MiniMap: BuildingMap perf_presentation grid=(%lld,%lld) hash=0x%016llx generation=%llu "
			"update=%llu stage=%u selectedToAdmissionMs=%.3f firstIncludedSpriteToAdmissionMs=%.3f "
			"admissionToSealMs=%.3f assemblyActiveWallMs=%.3f sealToAdoptMs=%.3f adoptToSubmitMs=%.3f "
			"storedTriangles=%llu pendingOrNotApplicable=-1",
			static_cast<long long>(history.X), static_cast<long long>(history.Y),
			static_cast<Count>(history.Hash), static_cast<Count>(history.Generation),
			static_cast<Count>(index + 1), stage, Milliseconds(update.Admitted - history.Selected),
			update.FirstIncludedSprite != Clock::time_point{} ?
				Milliseconds(update.Admitted - update.FirstIncludedSprite) : -1.0,
			stage >= 2 ? Milliseconds(update.Sealed - update.Admitted) : -1.0, update.Assembly.TotalMs,
			stage >= 3 ? Milliseconds(update.Adopted - update.Sealed) : -1.0,
			stage >= 4 ? Milliseconds(draw - update.Adopted) : -1.0, update.Triangles);
	}

	inline void BuildingMapPerformanceDiagnostics::ReportDraw(double interval, const RenderWork& work) noexcept
	{
		auto* log = MiniMap::Services::TryLog();
		if (log == nullptr)
		{
			return;
		}
		const auto& texture = work.Texture;
		const auto& admission = texture.AdmissionSample;
		const double admissionEstimate = admission.Samples == 0 ? (texture.Calls == 0 ? 0.0 : -1.0) :
			admission.TotalMs * static_cast<double>(texture.Calls) / static_cast<double>(admission.Samples);
		MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
			"MiniMap: BuildingMap perf_draw intervalMs=%.1f inclusiveWallMs=%s missingSnapshot=%llu "
			"textureDrawCalls=%llu admitted=%llu gateAcquires=%llu drawLedgerAcquires=%llu "
			"sample1in256WallMs admission=%s ledgerWait=%s ledgerHoldThroughBackend=%s backendNested=%s "
			"admissionEstimatedTotalMs=%.3f",
			interval, work.Draw.Describe().data(), work.MissingSnapshot, texture.Calls, texture.Admissions,
			texture.GateAcquisitions, texture.LedgerAcquisitions, admission.Describe().data(),
			texture.LedgerWaitSample.Describe().data(), texture.LedgerHoldSample.Describe().data(),
			texture.BackendSample.Describe().data(), admissionEstimate);
		ReportDrawPass(*log, "ordinary", work.Ordinary);
#if defined(MINIMAP_EXPERIMENTAL)
		MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
			"MiniMap: BuildingMap perf_draw_ordinary allBulkCanvasSubmissionsWallMs=%s",
			work.Ordinary.OrdinaryCanvasSubmission.Describe().data());
#endif
		ReportDrawPass(*log, "spline", work.Spline);
		ReportDrawPass(*log, "overlay", work.Overlay);
	}

	inline void BuildingMapPerformanceDiagnostics::ReportDrawPass(
		MiniMap::Services::ILog& log, const char* name, const DrawPass& pass) noexcept
	{
	#if !MINIMAP_ORDINARY_RANGE_SAMPLING_DIAGNOSTICS
		if (std::strcmp(name, "ordinary") == 0)
		{
			MiniMap::Services::Logf(log, MiniMap::Services::Level::Info,
				"MiniMap: BuildingMap perf_draw_pass pass=%s inclusiveWallMs=%s sectionsVisited=%llu "
				"trianglesVisited=%llu trianglesSubmitted=%llu rejected={nonfiniteXY:%llu nonfiniteArea:%llu "
				"zeroArea:%llu outsideClip:%llu nonfiniteUv:%llu} accepted=%llu "
				"canvasCalls=%llu failedSubmissions=%llu boundRanges=%llu unboundRanges=%llu",
				name, pass.Time.Describe().data(), pass.Sections,
				pass.Visited, pass.Submitted, pass.NonfiniteProjectedXY, pass.NonfiniteSignedArea,
				pass.ZeroSignedArea, pass.OutsideScreenClip, pass.NonfiniteUv, pass.Accepted,
				pass.CanvasCalls, pass.FailedSubmissions, pass.BoundRanges, pass.UnboundRanges);
			return;
		}
	#endif
		MiniMap::Services::Logf(log, MiniMap::Services::Level::Info,
			"MiniMap: BuildingMap perf_draw_pass pass=%s inclusiveWallMs=%s sectionsVisited=%llu "
			"trianglesVisited=%llu trianglesSubmitted=%llu rejected={nonfiniteXY:%llu nonfiniteArea:%llu "
			"zeroArea:%llu outsideClip:%llu nonfiniteUv:%llu} accepted=%llu "
			"canvasCalls=%llu failedSubmissions=%llu "
			"boundRanges=%llu unboundRanges=%llu "
			"sample1in256BoundRangesNestedWallMs projection=%s canvas=%s",
			name, pass.Time.Describe().data(), pass.Sections,
			pass.Visited, pass.Submitted, pass.NonfiniteProjectedXY, pass.NonfiniteSignedArea,
			pass.ZeroSignedArea, pass.OutsideScreenClip, pass.NonfiniteUv, pass.Accepted,
			pass.CanvasCalls, pass.FailedSubmissions,
			pass.BoundRanges, pass.UnboundRanges, pass.ProjectionSample.Describe().data(),
			pass.CanvasSample.Describe().data());
		if (name[0] == 's')
		{
			MiniMap::Services::Logf(log, MiniMap::Services::Level::Info,
				"MiniMap: BuildingMap perf_draw_spline timingWallMs traversal=%s canvasSubmission=%s "
				"rangesVisited=%llu rangesCulled=%llu trianglesInCulledRanges=%llu "
				"groupsVisited=%llu groupsCulled=%llu trianglesInCulledGroups=%llu",
				pass.SplineTraversal.Describe().data(), pass.SplineCanvasSubmission.Describe().data(),
				pass.SplineRangesVisited, pass.SplineRangesCulled, pass.SplineTrianglesInCulledRanges,
				pass.SplineGroupsVisited, pass.SplineGroupsCulled, pass.SplineTrianglesInCulledGroups);
		}
#if MINIMAP_ORDINARY_BATCH_CAPACITY_DIAGNOSTICS
		if (name[0] == 'o')
		{
			MiniMap::Services::Logf(log, MiniMap::Services::Level::Info,
				"MiniMap: BuildingMap perf_draw_batch_capacity vertexIncreases=%llu indexIncreases=%llu "
				"vertexGrowthElements=%llu indexGrowthElements=%llu maxVertices=%llu maxIndices=%llu "
				"maxVertexCapacity=%llu maxIndexCapacity=%llu",
				pass.VertexCapacityIncreases, pass.IndexCapacityIncreases,
				pass.VertexCapacityGrowthElements, pass.IndexCapacityGrowthElements,
				pass.MaximumVertexCount, pass.MaximumIndexCount,
				pass.MaximumVertexCapacity, pass.MaximumIndexCapacity);
		}
#endif
	}
}
#endif
