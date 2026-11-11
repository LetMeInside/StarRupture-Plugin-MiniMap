// Defines the Building Map library's game-independent synchronous and incremental numerical destination contract.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Services::Map::Buildings
{
	/** Reports the outcome of one synchronous destination submission. */
	enum class ImageSubmission : std::uint8_t
	{
		Submitted,
		Unavailable,
		InvalidInput,
		Culled,
		Rejected
	};

	/** Borrows immutable RGBA8 source pixels and the half-open pixel region used for sampling.
	 * Pixel storage remains caller-owned and is valid only during one synchronous submission or one
	 * incremental advancement call. Producers supply a fresh equivalent view on every continuation;
	 * consumers do not retain the view after that call returns.
	 */
	struct Rgba8ImageSource
	{
		std::span<const std::uint8_t> Pixels;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::array<double, 4> PixelRegion{};
	};

	/** Stores four global world-space XY corners in source-corner correspondence order. */
	struct ImageWorldQuad
	{
		std::array<std::array<double, 2>, 4> Corners{};
	};

	/** Supplies immutable image pixels, the sampled region, and global world placement. */
	struct ImageSubmissionCommand
	{
		Rgba8ImageSource Source;
		ImageWorldQuad Geometry;
	};

	/** Describes a format-neutral Section grid with signed cells, dimensions, and world scale.
	 * Destination pixels map to the half-open world bounds beginning at origin plus cell times extent.
	 */
	struct ImageGrid
	{
		double WorldOriginX = 0.0;
		double WorldOriginY = 0.0;
		std::int64_t CellX = 0;
		std::int64_t CellY = 0;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		double WorldUnitsPerPixel = 0.0;
	};

	/** Stores one global world-space XYZ triangle; destination implementations use XY for 2D output. */
	struct SolidTriangle
	{
		std::array<std::array<double, 3>, 3> Vertices{};
	};

	/** Borrows a contiguous triangle span and its sRGB 0xRRGGBB color plus straight 8-bit opacity. */
	struct SolidTriangleBatch
	{
		std::span<const SolidTriangle> Triangles;
		std::uint32_t Rgb = 0;
		std::uint8_t Opacity = 255;
	};

	/** Selects how normalized texture coordinates address values outside the source region. */
	enum class TextureAddressMode : std::uint8_t
	{
		Clamp,
		Repeat
	};

	/** Stores one global world-space XYZ vertex with its interpolated texture coordinate. */
	struct TexturedTriangleVertex
	{
		/** Global world-space location; two-dimensional destinations rasterize XY. */
		std::array<double, 3> Position{};
		/** Per-vertex source coordinates interpolated barycentrically across the triangle. */
		std::array<double, 2> TextureCoordinates{};
	};

	/** Stores one textured world-space triangle in deterministic vertex order. */
	struct TexturedTriangle
	{
		std::array<TexturedTriangleVertex, 3> Vertices{};
	};

	/** Borrows textured triangles and immutable RGBA8 pixels for one submission call. */
	struct TexturedTriangleBatch
	{
		/** Borrowed triangle order consumed during one synchronous submission or advancement. */
		std::span<const TexturedTriangle> Triangles;
		/** Borrowed immutable RGBA8 pixels, dimensions, and selected half-open region.
		 * Pixel storage remains caller-owned and is valid only for this synchronous submission or advancement.
		 */
		Rgba8ImageSource Source;
		/** Multiplies canonical surface UVs before addressing. */
		std::array<double, 2> UvScale{1.0, 1.0};
		/** Adds to canonical surface UVs before addressing. */
		std::array<double, 2> UvOffset{};
		/** U axis sampling behavior outside the source region. */
		TextureAddressMode AddressU = TextureAddressMode::Repeat;
		/** V axis sampling behavior outside the source region. */
		TextureAddressMode AddressV = TextureAddressMode::Clamp;
		/** Linear-light modulation source supplied as sRGB 0xRRGGBB. */
		std::uint32_t ModulationRgb = 0xFFFFFF;
		/** Straight 8-bit opacity multiplied by sampled texture alpha. */
		std::uint8_t Opacity = 255;
	};

	/** Defines synchronous and incremental image, solid-triangle, and textured-triangle destinations. */
	class IImageDestination
	{
	public:
		/** Stores only type-erased state owned by one implementation-specific operation cursor. */
		class CursorState
		{
		public:
			virtual ~CursorState() = default;
			/** Reports whether the implementation has reached a reset-safe terminal state. */
			virtual bool IsTerminal() const noexcept = 0;
			/** Clears terminal implementation progress so this allocation can serve a later operation. */
			virtual void ResetTerminal() noexcept = 0;
		};

		/** Resumes one logical submission without exposing implementation-specific raster state. */
		class SubmissionCursor final
		{
		public:
			SubmissionCursor() noexcept = default;
			SubmissionCursor(const SubmissionCursor&) = delete;
			SubmissionCursor& operator=(const SubmissionCursor&) = delete;
			SubmissionCursor(SubmissionCursor&&) = delete;
			SubmissionCursor& operator=(SubmissionCursor&&) = delete;
			/** Resets a fresh or terminal cursor; active work remains owned until its cursor is destroyed. */
			void Reset() noexcept
			{
				if (state_ && state_->IsTerminal())
				{
					state_->ResetTerminal();
				}
			}

		private:
			friend class IImageDestination;
			std::unique_ptr<CursorState> state_;
		};

		/** Resumes complete-image validation and ownership transfer without exposing destination state. */
		class FinalizationCursor final
		{
		public:
			FinalizationCursor() noexcept = default;
			FinalizationCursor(const FinalizationCursor&) = delete;
			FinalizationCursor& operator=(const FinalizationCursor&) = delete;
			FinalizationCursor(FinalizationCursor&&) = delete;
			FinalizationCursor& operator=(FinalizationCursor&&) = delete;
			/** Resets a fresh or terminal cursor; active completion remains retained. */
			void Reset() noexcept
			{
				if (state_ && state_->IsTerminal())
				{
					state_->ResetTerminal();
				}
			}

		private:
			friend class IImageDestination;
			std::unique_ptr<CursorState> state_;
		};

		/** Reports work consumed and the terminal outcome, if any, for one submission slice. */
		struct SubmissionSliceResult
		{
			std::size_t WorkUnitsConsumed = 0;
			std::optional<ImageSubmission> CompletedOutcome;
		};

		/** Classifies one incremental completed-image pass. */
		enum class FinalizationStepStatus : std::uint8_t
		{
			Pending,
			Complete,
			Failed
		};

		/** Reports complete pixels inspected and transparency only after successful completion. */
		struct FinalizationSliceResult
		{
			FinalizationStepStatus Status = FinalizationStepStatus::Pending;
			std::size_t PixelsResolved = 0;
			bool FullyTransparent = false;
		};

		/** Supports polymorphic destruction through the public destination boundary. */
		virtual ~IImageDestination() = default;

		/** Creates the selected numerical destination without exposing its concrete renderer type.
		 * @param grid Format-neutral Section grid and pixel dimensions.
		 * @param initializeImmediately Whether the destination is ready for drawing when created.
		 * @return The destination interface owner, or empty when allocation fails.
		 */
		static std::unique_ptr<IImageDestination> Create(const ImageGrid& grid, bool initializeImmediately) noexcept;

		/** Makes the already transparent-initialized destination available using the caller's deadline. */
		virtual bool StepInitializePixels(std::chrono::steady_clock::time_point deadline) noexcept = 0;
		/** Reports allocation, initialization, or raster failure without exposing partial output. */
		virtual bool IsFailed() const noexcept = 0;

		/** Submits immutable RGBA8 pixels and their global world-space quad synchronously.
		 * Implementations may report unavailable resources, invalid input, clipping, or rejection.
		 * The command and pixel view are borrowed for this call. The producer must keep the payload alive
		 * until the call returns, and the consumer must not retain the view afterward.
		 */
		virtual ImageSubmission SubmitImage(const ImageSubmissionCommand& command) = 0;

		/** Submits a contiguous span of global world-space XYZ triangles synchronously.
		 * RGB is sRGB in 0xRRGGBB form and opacity is straight 8-bit alpha.
		 * The span is borrowed and must not be retained.
		 */
		virtual ImageSubmission SubmitSolidTriangles(const SolidTriangleBatch& batch) = 0;

		/** Submits textured world-space triangles synchronously with per-vertex UVs and borrowed immutable pixels.
		 * UV scale and offset are applied before U/V addressing; RGB modulation is sRGB 0xRRGGBB and opacity is straight alpha.
		 * Triangle and pixel spans are borrowed; producers keep their storage alive through the call and consumers do not retain them.
		 */
		virtual ImageSubmission SubmitTexturedTriangles(const TexturedTriangleBatch& batch) = 0;

		/** Advances one image quad with fresh borrowed source data until its deadline or completion.
		 * The cursor and equivalent logical command must be reused; pixel spans remain call-local.
		 */
		virtual SubmissionSliceResult StepSubmitImage(
			const ImageSubmissionCommand& command,
			SubmissionCursor& cursor,
			std::chrono::steady_clock::time_point deadline) noexcept = 0;
		/** Advances ordered solid triangles until the deadline or completion. */
		virtual SubmissionSliceResult StepSubmitSolidTriangles(
			const SolidTriangleBatch& batch,
			SubmissionCursor& cursor,
			std::chrono::steady_clock::time_point deadline) noexcept = 0;
		/** Advances ordered textured triangles using only this call's borrowed source data.
		 * The same cursor and logical batch must be reused with fresh valid spans on continuation.
		 */
		virtual SubmissionSliceResult StepSubmitTexturedTriangles(
			const TexturedTriangleBatch& batch,
			SubmissionCursor& cursor,
			std::chrono::steady_clock::time_point deadline) noexcept = 0;
		/** Transfers a complete straight-alpha sRGB RGBA8 image synchronously; partial output is never exposed. */
		virtual bool Finalize(std::vector<std::uint8_t>& output) noexcept = 0;
		/** Validates image completion in deadline-bounded slices and transfers only after full completion. */
		virtual FinalizationSliceResult StepFinalize(
			std::vector<std::uint8_t>& output,
			FinalizationCursor& cursor,
			std::chrono::steady_clock::time_point deadline) noexcept = 0;

	protected:
		/** Lets destination implementations access opaque cursor state without exposing it to consumers. */
		static CursorState* GetCursorState(SubmissionCursor& cursor) noexcept
		{
			return cursor.state_.get();
		}
		/** Installs implementation progress only when the submission cursor has no retained state. */
		static void SetCursorState(SubmissionCursor& cursor, std::unique_ptr<CursorState> state) noexcept
		{
			if (!cursor.state_)
			{
				cursor.state_ = std::move(state);
			}
		}
		/** Lets destination implementations access opaque completion state without exposing it to consumers. */
		static CursorState* GetCursorState(FinalizationCursor& cursor) noexcept
		{
			return cursor.state_.get();
		}
		/** Installs implementation completion progress only when the cursor has no retained state. */
		static void SetCursorState(FinalizationCursor& cursor, std::unique_ptr<CursorState> state) noexcept
		{
			if (!cursor.state_)
			{
				cursor.state_ = std::move(state);
			}
		}
	};
}
