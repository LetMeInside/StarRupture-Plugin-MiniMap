// Rasterizes Building Map image and triangle submissions into one owned RGBA8 image.
#pragma once

#include "../IImageDestination.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Services::Map::Buildings::Rendering
{
	/** Implements neutral submissions with one tightly packed RGBA8 destination buffer. */
	class Rgba8ImageDestination final : public IImageDestination
	{
		/** Stores one world-space raster vertex and, for image draws, its normalized source UV. */
		struct RasterVertex
		{
			double X = 0.0;
			double Y = 0.0;
			double U = 0.0;
			double V = 0.0;
		};

		/** Holds one sampled color as linear RGB when needed, with original bytes for exact copies. */
		struct LinearPixel
		{
			float R = 0.0f;
			float G = 0.0f;
			float B = 0.0f;
			float A = 0.0f;
			std::array<std::uint8_t, 4> Original{};
			bool PreserveOriginal = false;
		};

		/** Holds only the integer axes and boundary offsets needed by an aligned one-to-one image copy. */
		struct ExactImageMapping
		{
			std::array<int, 2> DestinationAxes{};
			std::array<int, 2> DestinationEdges{};
			std::array<std::int64_t, 2> SourceStarts{};
			std::array<bool, 2> Forward{{true, true}};
			bool Enabled = false;
		};

		/** Holds a call-local borrowed image command and quad coordinates for both image triangles. */
		struct ImageShadeContext
		{
			const ImageSubmissionCommand& Command;
			const std::array<RasterVertex, 4>& Corners;
			const ExactImageMapping& ExactMapping;
			bool OtherTriangle = false;
		};

		/** Holds one immutable textured triangle batch while a single triangle is shaded. */
		struct TextureShadeContext
		{
			const TexturedTriangleBatch& Batch;
		};

		/** Stores value-only setup and next-pixel progress for one raster triangle. */
		struct RasterProgress
		{
			std::array<RasterVertex, 3> Vertices{};
			std::array<RasterVertex, 3> SourceVertices{};
			std::array<int, 4> Bounds{};
			std::array<bool, 3> TopLeftEdges{};
			double Area = 0.0;
			int NextPixelX = 0;
			int NextPixelY = 0;
			bool Swapped = false;
			bool SetupComplete = false;
			bool HasPixels = false;
		};

		using PixelBuffer = std::vector<std::uint8_t>;
		static constexpr std::size_t ChannelsPerPixel = 4;

	public:
		/** Returns whether a value-only incremental submission operation finished in this slice. */
		using SubmissionSliceResult = IImageDestination::SubmissionSliceResult;
		using SubmissionCursor = IImageDestination::SubmissionCursor;
		using FinalizationSliceResult = IImageDestination::FinalizationSliceResult;
		using FinalizationStepStatus = IImageDestination::FinalizationStepStatus;
		using FinalizationCursor = IImageDestination::FinalizationCursor;

		/** Creates a validated destination and optionally makes its allocation immediately ready.
		 * @param grid Global image grid and output dimensions.
		 * @param initializeImmediately Whether to permit drawing without a deadline step.
		 */
		explicit Rgba8ImageDestination(ImageGrid grid, bool initializeImmediately = true) noexcept;
		/** Completes the initialization state transition after allocation zero-initialized the pixels.
		 * @param deadline Absolute end of the caller's shared work allowance.
		 * @return True when drawing is available.
		 */
		bool StepInitializePixels(std::chrono::steady_clock::time_point deadline) noexcept override;
		/** Reports allocation or destination failure without exposing partial render output. */
		bool IsFailed() const noexcept override;
		Rgba8ImageDestination(const Rgba8ImageDestination&) = delete;
		Rgba8ImageDestination& operator=(const Rgba8ImageDestination&) = delete;
		Rgba8ImageDestination(Rgba8ImageDestination&&) = delete;
		Rgba8ImageDestination& operator=(Rgba8ImageDestination&&) = delete;

		/** Synchronously rasterizes borrowed RGBA8 pixels at their global world-space quad without retaining the pixel view. */
		ImageSubmission SubmitImage(const ImageSubmissionCommand& command) override;

		/** Rasterizes caller-owned XYZ triangles in span order without retaining or copying them. */
		ImageSubmission SubmitSolidTriangles(const SolidTriangleBatch& batch) override;

		/** Synchronously rasterizes borrowed textured XYZ/UV triangles using caller-owned pixel storage. */
		ImageSubmission SubmitTexturedTriangles(const TexturedTriangleBatch& batch) override;
		/** Advances one image quad until the absolute deadline at resumable triangle/pixel boundaries.
		 * The same logical command and a valid borrowed source view must be supplied on every continuation.
		 */
		SubmissionSliceResult StepSubmitImage(
			const ImageSubmissionCommand& command,
			SubmissionCursor& cursor,
			std::chrono::steady_clock::time_point deadline) noexcept override;
		/** Advances an ordered solid batch until the absolute deadline at resumable triangle/pixel boundaries.
		 * The same logical batch must be supplied on every continuation.
		 */
		SubmissionSliceResult StepSubmitSolidTriangles(
			const SolidTriangleBatch& batch,
			SubmissionCursor& cursor,
			std::chrono::steady_clock::time_point deadline) noexcept override;
		/** Advances an ordered textured batch using only this call's borrowed pixels and triangles.
		 * Equivalent immutable source and batch data must be supplied on every continuation.
		 */
		SubmissionSliceResult StepSubmitTexturedTriangles(
			const TexturedTriangleBatch& batch,
			SubmissionCursor& cursor,
			std::chrono::steady_clock::time_point deadline) noexcept override;

		/** Resolves the complete working buffer to row-major little-endian RGBA8 bytes. */
		bool Finalize(std::vector<std::uint8_t>& output) noexcept override;
		/** Resolves complete pixels to caller-owned RGBA8 output until the absolute deadline.
		 * The caller must retain the output unchanged and perform no drawing while the cursor is pending.
		 */
		FinalizationSliceResult StepFinalize(
			std::vector<std::uint8_t>& output,
			FinalizationCursor& cursor,
			std::chrono::steady_clock::time_point deadline) noexcept override;

	private:
		/** Owns RGBA8 raster progress for one active interface submission cursor. */
		struct SubmissionProgress final : IImageDestination::CursorState
		{
			enum class OperationKind : std::uint8_t
			{
				None,
				Image,
				SolidTriangles,
				TexturedTriangles
			};
			OperationKind Kind = OperationKind::None;
			std::uintptr_t DestinationIdentity = 0;
			std::uint64_t OperationId = 0;
			std::size_t TriangleIndex = 0;
			std::array<RasterVertex, 4> ImageCorners{};
			ExactImageMapping ImageMapping{};
			RasterProgress Raster{};
			LinearPixel SolidColor{};
			ImageSubmission TerminalOutcome = ImageSubmission::Rejected;
			bool Terminal = false;
			bool SubmittedAny = false;
			bool ImageInitialized = false;
			bool TexturedBatchValidated = false;
			bool IsTerminal() const noexcept override { return Terminal; }
			/** Clears completed operation data while retaining this cursor-state allocation. */
			void ResetTerminal() noexcept override { *this = SubmissionProgress{}; }
		};

		/** Owns identity and pixel cursor state for one interface completion operation. */
		struct FinalizationProgress final : IImageDestination::CursorState
		{
			std::uintptr_t DestinationIdentity = 0;
			std::uintptr_t OutputIdentity = 0;
			std::uint64_t OperationId = 0;
			std::size_t NextPixel = 0;
			FinalizationStepStatus TerminalStatus = FinalizationStepStatus::Pending;
			bool FullyTransparent = true;
			bool Started = false;
			bool Terminal = false;
			bool IsTerminal() const noexcept override { return Terminal; }
			/** Clears completed operation data while retaining this cursor-state allocation. */
			void ResetTerminal() noexcept override { *this = FinalizationProgress{}; }
		};

		/** Classifies a triangle's setup, culling and raster validation outcome. */
		enum class RasterSetupResult
		{
			DeadlineReached,
			Ready,
			Skipped,
			InvalidInput,
			Rejected
		};
		/** Classifies whether a pixel advanced, failed or reached the deadline. */
		enum class RasterPixelStepResult
		{
			DeadlineReached,
			Advanced,
			Rejected
		};
		/** Classifies the progress from one setup or pixel step in an ordered batch. */
		enum class RasterSequenceStepResult
		{
			DeadlineReached,
			Progressed,
			InvalidInput,
			Rejected
		};

		/** Starts or validates the sole active incremental submission on this destination. */
		bool BeginSubmission(
			SubmissionCursor& cursor,
			SubmissionProgress::OperationKind kind,
			SubmissionProgress*& progress,
			SubmissionSliceResult& output) noexcept;
		/** Records one submission's terminal outcome and releases its destination operation slot. */
		void CompleteSubmission(
			SubmissionProgress& progress,
			ImageSubmission outcome,
			SubmissionSliceResult& output) noexcept;
		/** Poison-protects pixels already written when a submission protocol is violated. */
		void RejectSubmissionProtocol(
			SubmissionProgress& progress,
			SubmissionSliceResult& output) noexcept;
		/** Clears partial output and records a terminal incremental-resolution failure. */
		FinalizationSliceResult FailFinalization(
			std::vector<std::uint8_t>& output,
			FinalizationProgress& progress,
			FinalizationSliceResult result) const noexcept;
		/** Validates and retains image geometry by value before the first raster unit. */
		bool InitializeImageSubmission(
			const ImageSubmissionCommand& command,
			SubmissionProgress& progress,
			SubmissionSliceResult& output) noexcept;
		/** Validates one solid triangle, classifies clipping and prepares its numerical raster state. */
		RasterSetupResult PrepareSolidRasterTriangle(
			const SolidTriangleBatch& batch,
			std::size_t triangleIndex,
			SubmissionProgress& progress,
			RasterProgress& rasterProgress) noexcept;
		/** Validates one textured triangle and prepares transformed UV raster state by value. */
		RasterSetupResult PrepareTexturedRasterTriangle(
			const TexturedTriangleBatch& batch,
			std::size_t triangleIndex,
			SubmissionProgress& progress,
			RasterProgress& rasterProgress) noexcept;
		/** Validates borrowed textured pixels, UV transforms and address modes. */
		static bool IsValidTexturedBatch(const TexturedTriangleBatch& batch) noexcept;
		/** Validates destination state and reserves its sole active completion operation. */
		bool BeginFinalization(
			std::vector<std::uint8_t>& output,
			FinalizationProgress& progress,
			FinalizationSliceResult& result) noexcept;
		/** Captures destination/output identities and allocates output for a fresh cursor. */
		bool StartFinalization(
			std::vector<std::uint8_t>& output,
			FinalizationProgress& progress,
			FinalizationSliceResult& result) noexcept;
		/** Moves the completed RGBA8 destination into caller ownership without copying pixels. */
		bool TransferCompletedImage(std::vector<std::uint8_t>& output) noexcept;
		/** Prepares one triangle using the synchronous path's exact validation and winding rules. */
		bool PrepareRasterProgress(
			std::array<RasterVertex, 3> vertices,
			RasterProgress& progress) const noexcept;
		/** Moves the retained raster position to the next Y-major, then X-major pixel. */
		static void AdvanceRasterPixel(RasterProgress& progress) noexcept;
		/** Resolves one triangle sequence until its deadline using per-call setup and shade functions. */
		template <typename PrepareFunction, typename ShadeFunction>
		SubmissionSliceResult StepRasterTriangleSequence(
			SubmissionProgress& progress,
			std::size_t triangleCount,
			std::chrono::steady_clock::time_point deadline,
			PrepareFunction&& prepare,
			ShadeFunction&& shade) noexcept;
		/** Advances one triangle by one setup or complete-pixel work unit. */
		template <typename PrepareFunction, typename ShadeFunction>
		RasterSequenceStepResult StepRasterTriangle(
			SubmissionProgress& progress,
			std::chrono::steady_clock::time_point deadline,
			SubmissionSliceResult& result,
			PrepareFunction&& prepare,
			ShadeFunction&& shade) noexcept;
		/** Checks the deadline before one indivisible triangle setup. */
		template <typename PrepareFunction>
		RasterSetupResult StepRasterTriangleSetup(
			SubmissionProgress& progress,
			std::chrono::steady_clock::time_point deadline,
			SubmissionSliceResult& result,
			PrepareFunction&& prepare) noexcept;
		/** Visits one destination pixel center as one indivisible operation. */
		template <typename ShadeFunction>
		RasterPixelStepResult StepRasterPixel(
			SubmissionProgress& progress,
			std::chrono::steady_clock::time_point deadline,
			SubmissionSliceResult& result,
			ShadeFunction&& shade) noexcept;
		/** Validates grid dimensions, world scale, allocation arithmetic, and signed-cell bounds. */
		static bool IsValidGrid(
			const ImageGrid& grid,
			std::array<long double, 4>& worldBounds) noexcept;

		/** Calculates checked half-open global map bounds for one grid. */
		static bool TryGetGridBounds(
			const ImageGrid& grid,
			std::array<long double, 4>& bounds) noexcept;

		/** Validates the borrowed RGBA8 view, exact byte size, and the half-open source region. */
		static bool IsValidImage(const ImageSubmissionCommand& command) noexcept;

		/** Tests strict XY separation between a world quad and this destination grid. */
		bool IntersectsGrid(const ImageWorldQuad& quad) const noexcept;

		/** Tests strict XY separation between one XYZ triangle and this destination grid. */
		bool IntersectsGrid(const SolidTriangle& triangle) const noexcept;

		/** Verifies that each triangle vertex has finite world-space XY coordinates. */
		static bool HasFiniteTriangleXY(const SolidTriangle& triangle) noexcept;

		/** Verifies finite XYZ/UV values for one textured triangle. */
		static bool HasFiniteTexturedTriangle(const TexturedTriangle& triangle) noexcept;

		/** Samples and retains the nearest source texel without decoding bytes needed for direct copies. */
		static bool SampleRgba8(
			const Rgba8ImageSource& source,
			double u,
			double v,
			TextureAddressMode addressU,
			TextureAddressMode addressV,
			LinearPixel& output) noexcept;
		/** Copies one validated source texel into a sampling result without color conversion. */
		static bool FetchOriginalPixel(
			const Rgba8ImageSource& source,
			std::int64_t pixelX,
			std::int64_t pixelY,
			LinearPixel& output) noexcept;
		/** Recognizes a cardinal one-to-one quad aligned to source texels and destination pixels. */
		void BuildExactImageMapping(
			const ImageSubmissionCommand& command,
			const std::array<RasterVertex, 4>& corners,
			ExactImageMapping& output) const noexcept;

		/** Reads one addressed source texel while preserving its original RGBA8 values. */
		static bool FetchPixel(
			const Rgba8ImageSource& source,
			std::int64_t pixelX,
			std::int64_t pixelY,
			std::int64_t minX,
			std::int64_t minY,
			std::int64_t maxX,
			std::int64_t maxY,
			TextureAddressMode addressU,
			TextureAddressMode addressV,
			LinearPixel& output) noexcept;

		/** Wraps or clamps one texel index within a validated source-region interval. */
		static std::int64_t AddressPixel(
			std::int64_t pixel,
			std::int64_t minimum,
			std::int64_t maximum,
			TextureAddressMode addressMode) noexcept;

		/** Returns the lookup-table sRGB-to-linear value for one 8-bit color channel. */
		static double SrgbToLinear(std::uint8_t channel) noexcept;

		/** Copies an aligned source texel or samples the nearest source texel at the pixel center. */
		static bool ShadeImage(
			const ImageShadeContext& context,
			double firstWeight,
			double secondWeight,
			double thirdWeight,
			int pixelX,
			int pixelY,
			LinearPixel& output) noexcept;

		/** Interpolates triangle UVs, samples the texture, and applies linear modulation/opacity. */
		static bool ShadeTexture(
			const TextureShadeContext& context,
			const std::array<RasterVertex, 3>& vertices,
			double firstWeight,
			double secondWeight,
			double thirdWeight,
			LinearPixel& output) noexcept;

		/** Rasterizes one triangle with center sampling, top-left ownership, and source-over blending. */
		template <typename ShadeFunction>
		bool RasterTriangle(
			std::array<RasterVertex, 3> vertices,
			PixelBuffer& pixels,
			ShadeFunction&& shade) const noexcept;

		/** Computes conservative integer pixel bounds and distinguishes no-overlap from numeric failure. */
		bool TryGetRasterBounds(
			const std::array<RasterVertex, 3>& vertices,
			std::array<int, 4>& bounds,
			bool& hasPixels) const noexcept;

		/** Converts a clipped low bound to the reference one-pixel-expanded integer raster coordinate. */
		static int LowPixel(long double value, std::uint32_t extent) noexcept;

		/** Converts a clipped high bound to the reference one-pixel-expanded integer raster coordinate. */
		static int HighPixel(long double value, std::uint32_t extent) noexcept;

		/** Visits covered pixel centers and applies the color sampler and source-over blend. */
		template <typename ShadeFunction>
		bool RasterizePixels(
			const std::array<RasterVertex, 3>& vertices,
			const std::array<int, 4>& bounds,
			double area,
			bool swapped,
			const std::array<bool, 3>& topLeftEdges,
			PixelBuffer& pixels,
			ShadeFunction&& shade) const noexcept;

		/** Tests one pixel center and blends its covered source contribution. */
		template <typename ShadeFunction>
		bool RasterPixel(
			const std::array<RasterVertex, 3>& vertices,
			int pixelX,
			int pixelY,
			double area,
			bool swapped,
			const std::array<bool, 3>& topLeftEdges,
			PixelBuffer& pixels,
			ShadeFunction&& shade) const noexcept;

		/** Applies edge ownership, interpolates a covered pixel, and blends source-over. */
		template <typename ShadeFunction>
		bool ShadeCoveredPixel(
			const std::array<RasterVertex, 3>& vertices,
			double worldX,
			double worldY,
			int pixelX,
			int pixelY,
			double area,
			bool swapped,
			const std::array<bool, 3>& topLeftEdges,
			std::uint8_t* destination,
			ShadeFunction&& shade) const noexcept;

		/** Reports whether any edge excludes the pixel center under the shared top-left fill rule. */
		static bool IsOutsideTriangle(
			double firstEdge,
			double secondEdge,
			double thirdEdge,
			const std::array<bool, 3>& topLeftEdges) noexcept;

		/** Rasterizes one solid RGB triangle into the reusable destination buffer. */
		bool RasterSolidTriangle(const SolidTriangle& triangle, std::uint32_t rgb, std::uint8_t opacity) noexcept;

		/** Rasterizes one textured XYZ/UV triangle into the shared RGBA8 destination buffer. */
		bool RasterTexturedTriangle(const TexturedTriangle& triangle, const TexturedTriangleBatch& batch) noexcept;

		/** Maps a signed section and local pixel index to its global pixel-center coordinate. */
		static double GlobalPixel(
			std::int64_t cell,
			std::size_t pixel,
			double origin,
			std::uint32_t side,
			double worldUnitsPerPixel) noexcept;

		/** Computes one oriented edge function in world XY. */
		static double Edge(const RasterVertex& first, const RasterVertex& second, double x, double y) noexcept;

		/** Identifies inclusive edges for deterministic shared-edge pixel-center ownership. */
		static bool IsTopLeftEdge(const RasterVertex& first, const RasterVertex& second) noexcept;

		/** Converts one normalized linear channel to a rounded 8-bit sRGB channel. */
		static std::uint8_t LinearToSrgb(double channel) noexcept;

		/** Composites one straight-alpha source directly into one stored RGBA8 destination pixel. */
		static void CompositePixel(const LinearPixel& source, std::uint8_t* destination) noexcept;
		/** Composites overlapping source and destination colors in linear light. */
		static void CompositeOverlappingPixel(
			const LinearPixel& source,
			double sourceAlpha,
			std::uint8_t* destination) noexcept;

		ImageGrid grid_{};
		std::array<long double, 4> worldBounds_{};
		PixelBuffer pixels_;
		mutable std::uint64_t nextOperationId_ = 1;
		std::uint64_t activeSubmissionId_ = 0;
		mutable std::uint64_t activeFinalizationId_ = 0;
		bool initialized_ = false;
		bool failed_ = false;
		bool finalized_ = false;
	};
}
