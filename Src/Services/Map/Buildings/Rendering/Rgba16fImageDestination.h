// Composes Building Map images and solid triangles into one reusable RGBA16F multisample buffer.
#pragma once

#include "../IImageDestination.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Services::Map::Buildings::Rendering
{
	/** Describes one image grid using a base origin, signed global cell coordinates, pixel dimensions, and world scale.
	 * Its lower bounds are origin plus cell index times the corresponding pixel extent and world-units-per-pixel.
	 */
	struct Rgba16fImageGrid
	{
		double WorldOriginX = 0.0;
		double WorldOriginY = 0.0;
		std::int64_t CellX = 0;
		std::int64_t CellY = 0;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		double WorldUnitsPerPixel = 0.0;
	};

	/** Implements image, solid-triangle and textured-triangle submission with one reusable four-sample buffer. */
	class Rgba16fImageDestination final : public IImageDestination
	{
		/** Stores one world-space raster vertex and, for image draws, its normalized source UV. */
		struct RasterVertex
		{
			double X = 0.0;
			double Y = 0.0;
			double U = 0.0;
			double V = 0.0;
		};

		/** Stores one sampled linear-light RGBA value during image rasterization. */
		struct LinearPixel
		{
			float R = 0.0f;
			float G = 0.0f;
			float B = 0.0f;
			float A = 0.0f;
		};

		/** Holds a call-local borrowed image command and quad coordinates for both image triangles. */
		struct ImageShadeContext
		{
			const ImageSubmissionCommand& Command;
			const std::array<RasterVertex, 4>& Corners;
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

		using Sample = std::array<float, 4>;
		using SampleBuffer = std::vector<Sample>;
		static constexpr std::size_t SamplesPerPixel = 4;

	public:
		/** Returns whether a value-only incremental submission operation finished in this slice. */
		struct SubmissionSliceResult
		{
			std::size_t WorkUnitsConsumed = 0;
			std::optional<ImageSubmission> CompletedOutcome;
		};

		/** Retains only numerical progress for one logical submission; borrowed inputs remain call-local.
		 * Active cursors cannot be copied, moved or reset; abandon active work by destroying its destination and cursor together.
		 */
		struct SubmissionCursor
		{
			/** Creates a fresh cursor with no active operation. */
			SubmissionCursor() noexcept = default;
			SubmissionCursor(const SubmissionCursor&) = delete;
			SubmissionCursor& operator=(const SubmissionCursor&) = delete;
			SubmissionCursor(SubmissionCursor&&) = delete;
			SubmissionCursor& operator=(SubmissionCursor&&) = delete;
			/** Resets a fresh or terminal cursor; this does not cancel an active operation. */
			void Reset() noexcept;

		private:
			friend class Rgba16fImageDestination;
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
			RasterProgress Raster{};
			LinearPixel SolidColor{};
			ImageSubmission TerminalOutcome = ImageSubmission::Rejected;
			bool Terminal = false;
			bool SubmittedAny = false;
			bool ImageInitialized = false;
		};

		/** Identifies the terminal or pending result of incremental RGBA16F resolution. */
		enum class FinalizationStepStatus
		{
			Pending,
			Complete,
			Failed
		};

		/** Reports how many complete pixels one finalization slice resolved. */
		struct FinalizationSliceResult
		{
			FinalizationStepStatus Status = FinalizationStepStatus::Pending;
			std::size_t PixelsResolved = 0;
		};

		/** Retains output-object identity, the next pixel and terminal state for incremental resolution.
		 * Active cursors cannot be copied, moved or reset; abandon active work by destroying its destination and cursor together.
		 */
		struct FinalizationCursor
		{
			/** Creates a fresh cursor before output allocation and resolution. */
			FinalizationCursor() noexcept = default;
			FinalizationCursor(const FinalizationCursor&) = delete;
			FinalizationCursor& operator=(const FinalizationCursor&) = delete;
			FinalizationCursor(FinalizationCursor&&) = delete;
			FinalizationCursor& operator=(FinalizationCursor&&) = delete;
			/** Resets a fresh or terminal cursor; this does not cancel an active finalization. */
			void Reset() noexcept;

		private:
			friend class Rgba16fImageDestination;
			std::uintptr_t DestinationIdentity = 0;
			std::uintptr_t OutputIdentity = 0;
			std::uint64_t OperationId = 0;
			std::size_t NextPixel = 0;
			FinalizationStepStatus TerminalStatus = FinalizationStepStatus::Pending;
			bool Started = false;
			bool Terminal = false;
		};

		/** Creates and initializes the destination grid; allocation failure leaves it unavailable. */
		explicit Rgba16fImageDestination(Rgba16fImageGrid grid) noexcept;
		Rgba16fImageDestination(const Rgba16fImageDestination&) = delete;
		Rgba16fImageDestination& operator=(const Rgba16fImageDestination&) = delete;
		Rgba16fImageDestination(Rgba16fImageDestination&&) = delete;
		Rgba16fImageDestination& operator=(Rgba16fImageDestination&&) = delete;

		/** Synchronously rasterizes borrowed RGBA8 pixels at their global world-space quad without retaining the pixel view. */
		ImageSubmission SubmitImage(const ImageSubmissionCommand& command) override;

		/** Rasterizes caller-owned XYZ triangles in span order without retaining or copying them. */
		ImageSubmission SubmitSolidTriangles(const SolidTriangleBatch& batch) override;

		/** Synchronously rasterizes borrowed textured XYZ/UV triangles using caller-owned pixel storage. */
		ImageSubmission SubmitTexturedTriangles(const TexturedTriangleBatch& batch) override;
		/** Advances one image quad by at most maximumWorkUnits pixel visits/triangle setup units.
		 * The same logical command and a valid borrowed source view must be supplied on every continuation.
		 */
		SubmissionSliceResult StepSubmitImage(
			const ImageSubmissionCommand& command,
			SubmissionCursor& cursor,
			std::size_t maximumWorkUnits) noexcept;
		/** Advances an ordered solid batch by at most maximumWorkUnits pixel visits/triangle setup units.
		 * The same logical batch must be supplied on every continuation.
		 */
		SubmissionSliceResult StepSubmitSolidTriangles(
			const SolidTriangleBatch& batch,
			SubmissionCursor& cursor,
			std::size_t maximumWorkUnits) noexcept;
		/** Advances an ordered textured batch using only this call's borrowed pixels and triangles.
		 * Equivalent immutable source and batch data must be supplied on every continuation.
		 */
		SubmissionSliceResult StepSubmitTexturedTriangles(
			const TexturedTriangleBatch& batch,
			SubmissionCursor& cursor,
			std::size_t maximumWorkUnits) noexcept;

		/** Resolves the complete working buffer to row-major little-endian RGBA16F bytes. */
		bool Finalize(std::vector<std::uint8_t>& output) const noexcept;
		/** Resolves at most maximumPixels complete pixels to caller-owned RGBA16F output.
		 * The caller must retain the output unchanged and perform no drawing while the cursor is pending.
		 */
		FinalizationSliceResult StepFinalize(
			std::vector<std::uint8_t>& output,
			FinalizationCursor& cursor,
			std::size_t maximumPixels) const noexcept;

	private:
		/** Classifies a triangle's setup, culling and raster validation outcome. */
		enum class RasterSetupResult
		{
			QuotaExhausted,
			Ready,
			Skipped,
			InvalidInput,
			Rejected
		};
		/** Classifies whether one complete pixel consumed work, failed or needs quota. */
		enum class RasterPixelStepResult
		{
			QuotaExhausted,
			Advanced,
			Rejected
		};
		/** Classifies the progress from one setup or pixel step in an ordered batch. */
		enum class RasterSequenceStepResult
		{
			QuotaExhausted,
			Progressed,
			InvalidInput,
			Rejected
		};

		/** Starts or validates the sole active incremental submission on this destination. */
		bool BeginSubmission(
			SubmissionCursor& cursor,
			SubmissionCursor::OperationKind kind,
			SubmissionSliceResult& output) noexcept;
		/** Records one submission's terminal outcome and releases its destination operation slot. */
		void CompleteSubmission(
			SubmissionCursor& cursor,
			ImageSubmission outcome,
			SubmissionSliceResult& output) noexcept;
		/** Poison-protects prior partial samples when a submission protocol is violated. */
		void RejectSubmissionProtocol(
			SubmissionCursor& cursor,
			SubmissionSliceResult& output) noexcept;
		/** Clears partial output and records a terminal incremental-resolution failure. */
		FinalizationSliceResult FailFinalization(
			std::vector<std::uint8_t>& output,
			FinalizationCursor& cursor,
			FinalizationSliceResult result) const noexcept;
		/** Validates and retains image geometry by value before the first raster unit. */
		bool InitializeImageSubmission(
			const ImageSubmissionCommand& command,
			SubmissionCursor& cursor,
			SubmissionSliceResult& output) noexcept;
		/** Validates one solid triangle, classifies clipping and prepares its numerical raster state. */
		RasterSetupResult PrepareSolidRasterTriangle(
			const SolidTriangleBatch& batch,
			std::size_t triangleIndex,
			SubmissionCursor& cursor,
			RasterProgress& progress) noexcept;
		/** Validates one textured triangle and prepares transformed UV raster state by value. */
		RasterSetupResult PrepareTexturedRasterTriangle(
			const TexturedTriangleBatch& batch,
			std::size_t triangleIndex,
			SubmissionCursor& cursor,
			RasterProgress& progress) noexcept;
		/** Validates borrowed textured pixels, UV transforms and address modes. */
		static bool IsValidTexturedBatch(const TexturedTriangleBatch& batch) noexcept;
		/** Validates destination state and allocates output on the first positive resolve slice. */
		bool BeginFinalization(
			std::vector<std::uint8_t>& output,
			FinalizationCursor& cursor,
			FinalizationSliceResult& result) const noexcept;
		/** Captures destination/output identities and allocates output for a fresh cursor. */
		bool StartFinalization(
			std::vector<std::uint8_t>& output,
			FinalizationCursor& cursor,
			FinalizationSliceResult& result) const noexcept;
		/** Checks the exact resolved byte count without accessing output pixels. */
		bool HasExpectedFinalizationOutput(const std::vector<std::uint8_t>& output) const noexcept;
		/** Resolves and stores all four channels for exactly one complete output pixel. */
		bool ResolveFinalizationPixel(std::vector<std::uint8_t>& output, std::size_t pixel) const noexcept;
		/** Prepares one triangle using the synchronous path's exact validation and winding rules. */
		bool PrepareRasterProgress(
			std::array<RasterVertex, 3> vertices,
			RasterProgress& progress) const noexcept;
		/** Moves the retained raster position to the next Y-major, then X-major pixel. */
		static void AdvanceRasterPixel(RasterProgress& progress) noexcept;
		/** Resolves one quota-bounded triangle sequence using per-call setup and shade functions. */
		template <typename PrepareFunction, typename ShadeFunction>
		SubmissionSliceResult StepRasterTriangleSequence(
			SubmissionCursor& cursor,
			std::size_t triangleCount,
			std::size_t maximumWorkUnits,
			PrepareFunction&& prepare,
			ShadeFunction&& shade) noexcept;
		/** Advances one triangle by one setup or complete-pixel work unit. */
		template <typename PrepareFunction, typename ShadeFunction>
		RasterSequenceStepResult StepRasterTriangle(
			SubmissionCursor& cursor,
			std::size_t maximumWorkUnits,
			SubmissionSliceResult& result,
			PrepareFunction&& prepare,
			ShadeFunction&& shade) noexcept;
		/** Consumes one triangle-setup work unit when quota allows. */
		template <typename PrepareFunction>
		RasterSetupResult StepRasterTriangleSetup(
			SubmissionCursor& cursor,
			std::size_t maximumWorkUnits,
			SubmissionSliceResult& result,
			PrepareFunction&& prepare) noexcept;
		/** Visits one pixel and all four samples as one indivisible work unit. */
		template <typename ShadeFunction>
		RasterPixelStepResult StepRasterPixel(
			SubmissionCursor& cursor,
			std::size_t maximumWorkUnits,
			SubmissionSliceResult& result,
			ShadeFunction&& shade) noexcept;
		/** Validates grid dimensions, world scale, allocation arithmetic, and signed-cell bounds. */
		static bool IsValidGrid(
			const Rgba16fImageGrid& grid,
			std::array<long double, 4>& worldBounds) noexcept;

		/** Calculates checked half-open global map bounds for one grid. */
		static bool TryGetGridBounds(
			const Rgba16fImageGrid& grid,
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

		/** Samples four source texels bilinearly and converts their RGB channels to linear light. */
		static bool SampleRgba8(
			const Rgba8ImageSource& source,
			double u,
			double v,
			TextureAddressMode addressU,
			TextureAddressMode addressV,
			LinearPixel& output) noexcept;

		/** Reads one clamped source texel while preserving the validated source-region boundary. */
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

		/** Fetches and bilinearly combines the four bounded source texels for one image sample. */
		static bool InterpolateSourcePixels(
			const Rgba8ImageSource& source,
			std::int64_t x0,
			std::int64_t y0,
			std::int64_t minX,
			std::int64_t minY,
			std::int64_t maxX,
			std::int64_t maxY,
			double tx,
			double ty,
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

		/** Interpolates image color from one raster sample's barycentric coordinates. */
		static bool ShadeImage(
			const ImageShadeContext& context,
			double firstWeight,
			double secondWeight,
			double thirdWeight,
			LinearPixel& output) noexcept;

		/** Interpolates triangle UVs, samples the texture, and applies linear modulation/opacity. */
		static bool ShadeTexture(
			const TextureShadeContext& context,
			const std::array<RasterVertex, 3>& vertices,
			double firstWeight,
			double secondWeight,
			double thirdWeight,
			LinearPixel& output) noexcept;

		/** Rasterizes one triangle with the established four-sample top-left fill rule and source-over blend. */
		template <typename ShadeFunction>
		bool RasterTriangle(
			std::array<RasterVertex, 3> vertices,
			SampleBuffer& samples,
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

		/** Visits covered quarter-pixel samples and applies the supplied color sampler and source-over blend. */
		template <typename ShadeFunction>
		bool RasterizeSamples(
			const std::array<RasterVertex, 3>& vertices,
			const std::array<int, 4>& bounds,
			double area,
			bool swapped,
			const std::array<bool, 3>& topLeftEdges,
			SampleBuffer& samples,
			ShadeFunction&& shade) const noexcept;

		/** Tests the four samples belonging to one pixel and blends covered results. */
		template <typename ShadeFunction>
		bool RasterPixelSamples(
			const std::array<RasterVertex, 3>& vertices,
			int pixelX,
			int pixelY,
			double area,
			bool swapped,
			const std::array<bool, 3>& topLeftEdges,
			const std::array<std::array<double, 2>, 4>& offsets,
			SampleBuffer& samples,
			ShadeFunction&& shade) const noexcept;

		/** Applies edge ownership, interpolates a covered sample, and blends source-over. */
		template <typename ShadeFunction>
		bool ShadeCoveredSample(
			const std::array<RasterVertex, 3>& vertices,
			double worldX,
			double worldY,
			double area,
			bool swapped,
			const std::array<bool, 3>& topLeftEdges,
			Sample& destination,
			ShadeFunction&& shade) const noexcept;

		/** Reports whether any edge excludes a sample under the shared top-left fill rule. */
		static bool IsOutsideTriangle(
			double firstEdge,
			double secondEdge,
			double thirdEdge,
			const std::array<bool, 3>& topLeftEdges) noexcept;

		/** Rasterizes one solid RGB triangle into the reusable sample buffer. */
		bool RasterSolidTriangle(const SolidTriangle& triangle, std::uint32_t rgb, std::uint8_t opacity) noexcept;

		/** Rasterizes one textured XYZ/UV triangle into the shared multisample buffer. */
		bool RasterTexturedTriangle(const TexturedTriangle& triangle, const TexturedTriangleBatch& batch) noexcept;

		/** Maps a signed section/pixel index and sample offset to one global map coordinate. */
		static double GlobalPixel(
			std::int64_t cell,
			std::size_t pixel,
			double sampleOffset,
			double origin,
			std::uint32_t side,
			double worldUnitsPerPixel) noexcept;

		/** Computes one oriented edge function in world XY. */
		static double Edge(const RasterVertex& first, const RasterVertex& second, double x, double y) noexcept;

		/** Identifies inclusive edges for deterministic shared-edge sample ownership. */
		static bool IsTopLeftEdge(const RasterVertex& first, const RasterVertex& second) noexcept;

		/** Converts a finite IEEE-754 single-precision value to the established round-to-nearest-even half format. */
		static std::uint16_t FloatToHalf(float value) noexcept;

		/** Encodes a rounded subnormal binary16 value. */
		static std::uint16_t ConvertHalfSubnormal(std::uint32_t sign, int exponent, std::uint32_t mantissa) noexcept;

		/** Encodes a rounded normal binary16 value and handles mantissa carry. */
		static std::uint16_t ConvertHalfNormal(std::uint32_t sign, int exponent, std::uint32_t mantissa) noexcept;

		/** Stores one half-float channel in little-endian RGBA16F output order. */
		static void StoreHalf(std::vector<std::uint8_t>& output, std::size_t pixel, std::size_t channel, float value);

		/** Resolves the four initialized samples per pixel into RGBA16F output bytes. */
		bool Resolve(std::vector<std::uint8_t>& output) const;

		Rgba16fImageGrid grid_{};
		std::array<long double, 4> worldBounds_{};
		SampleBuffer samples_;
		mutable std::uint64_t nextOperationId_ = 1;
		std::uint64_t activeSubmissionId_ = 0;
		mutable std::uint64_t activeFinalizationId_ = 0;
		bool initialized_ = false;
		bool failed_ = false;
	};
}
