#if defined(MODLOADER_CLIENT_BUILD)

// Implements independent RGBA8 image, solid-triangle, and textured-triangle rasterization for the Building Map library.
#include "Rgba8ImageDestination.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>
#include <new>

namespace Services::Map::Buildings::Rendering
{
	/** Creates one validated global grid and allocates its zero-initialized RGBA8 buffer. */
	Rgba8ImageDestination::Rgba8ImageDestination(ImageGrid grid, bool initializeImmediately) noexcept
		: grid_(grid)
	{
		if (!IsValidGrid(grid_, worldBounds_))
		{
			failed_ = true;
			return;
		}
		try
		{
			const std::size_t pixels = std::size_t(grid_.Width) * grid_.Height;
			pixels_.resize(pixels * ChannelsPerPixel);
			if (initializeImmediately)
			{
				initialized_ = true;
			}
		}
		catch (...)
		{
			pixels_.clear();
			failed_ = true;
		}
	}

	/** Makes the already zero-initialized RGBA8 allocation available within the caller's deadline. */
	bool Rgba8ImageDestination::StepInitializePixels(
		std::chrono::steady_clock::time_point deadline) noexcept
	{
		if (failed_ || finalized_)
		{
			return false;
		}
		if (!initialized_)
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			initialized_ = true;
		}
		return true;
	}

	/** Reports whether construction or later raster work made the destination unavailable. */
	bool Rgba8ImageDestination::IsFailed() const noexcept
	{
		return failed_;
	}

	/** Starts or validates the sole active submission while keeping cursor state opaque. */
	bool Rgba8ImageDestination::BeginSubmission(
		SubmissionCursor& cursor,
		SubmissionProgress::OperationKind kind,
		SubmissionProgress*& progress,
		SubmissionSliceResult& output) noexcept
	{
		progress = dynamic_cast<SubmissionProgress*>(GetCursorState(cursor));
		if (GetCursorState(cursor) != nullptr && progress == nullptr)
		{
			output.CompletedOutcome = ImageSubmission::Rejected;
			return false;
		}
		if (progress == nullptr)
		{
			try
			{
				auto state = std::make_unique<SubmissionProgress>();
				progress = state.get();
				SetCursorState(cursor, std::move(state));
			}
			catch (...)
			{
				output.CompletedOutcome = ImageSubmission::Unavailable;
				return false;
			}
		}
		const std::uintptr_t destinationIdentity = reinterpret_cast<std::uintptr_t>(this);
		if (progress->DestinationIdentity != 0 && progress->DestinationIdentity != destinationIdentity)
		{
			progress->Terminal = true;
			progress->TerminalOutcome = ImageSubmission::Rejected;
			progress->OperationId = 0;
			output.CompletedOutcome = ImageSubmission::Rejected;
			return false;
		}
		if (progress->Terminal)
		{
			output.CompletedOutcome = progress->Kind == kind && progress->DestinationIdentity == destinationIdentity
				? progress->TerminalOutcome
				: ImageSubmission::Rejected;
			return false;
		}
		if (failed_ || finalized_ || !initialized_)
		{
			CompleteSubmission(*progress, ImageSubmission::Unavailable, output);
			return false;
		}
		if (progress->Kind != SubmissionProgress::OperationKind::None &&
			(progress->Kind != kind || progress->DestinationIdentity != destinationIdentity))
		{
			RejectSubmissionProtocol(*progress, output);
			return false;
		}
		if (progress->Kind == SubmissionProgress::OperationKind::None)
		{
			progress->Kind = kind;
			progress->DestinationIdentity = destinationIdentity;
			if (activeSubmissionId_ != 0 || activeFinalizationId_ != 0)
			{
				RejectSubmissionProtocol(*progress, output);
				return false;
			}
			progress->OperationId = nextOperationId_++;
			if (nextOperationId_ == 0) nextOperationId_ = 1;
			activeSubmissionId_ = progress->OperationId;
			return true;
		}
		if (progress->OperationId == 0 || activeSubmissionId_ != progress->OperationId || activeFinalizationId_ != 0)
		{
			RejectSubmissionProtocol(*progress, output);
			return false;
		}
		return true;
	}

	/** Records a terminal outcome and releases this progress object's destination slot. */
	void Rgba8ImageDestination::CompleteSubmission(
		SubmissionProgress& progress,
		ImageSubmission outcome,
		SubmissionSliceResult& output) noexcept
	{
		if (progress.OperationId != 0 && activeSubmissionId_ == progress.OperationId) activeSubmissionId_ = 0;
		progress.Terminal = true;
		progress.TerminalOutcome = outcome;
		progress.OperationId = 0;
		output.CompletedOutcome = outcome;
	}

	/** Poison-protects partial destination bytes after cursor protocol misuse. */
	void Rgba8ImageDestination::RejectSubmissionProtocol(
		SubmissionProgress& progress,
		SubmissionSliceResult& output) noexcept
	{
		progress.DestinationIdentity = reinterpret_cast<std::uintptr_t>(this);
		failed_ = true;
		activeSubmissionId_ = 0;
		activeFinalizationId_ = 0;
		progress.Terminal = true;
		progress.TerminalOutcome = ImageSubmission::Rejected;
		progress.OperationId = 0;
		output.CompletedOutcome = ImageSubmission::Rejected;
	}

	/** Discards partial RGBA8 bytes and permanently terminates this finalization progress. */
	Rgba8ImageDestination::FinalizationSliceResult Rgba8ImageDestination::FailFinalization(
		std::vector<std::uint8_t>& output,
		FinalizationProgress& progress,
		FinalizationSliceResult result) const noexcept
	{
		const std::uint64_t operationId = progress.OperationId;
		output.clear();
		progress.Terminal = true;
		progress.TerminalStatus = FinalizationStepStatus::Failed;
		progress.OperationId = 0;
		if (activeFinalizationId_ == operationId)
		{
			activeFinalizationId_ = 0;
		}
		result.Status = FinalizationStepStatus::Failed;
		return result;
	}

	/** Validates call-local image data and retains only its world-space quad values. */
	bool Rgba8ImageDestination::InitializeImageSubmission(
		const ImageSubmissionCommand& command,
		SubmissionProgress& progress,
		SubmissionSliceResult& output) noexcept
	{
		if (!progress.ImageInitialized)
		{
			if (!IsValidImage(command))
			{
				CompleteSubmission(progress, ImageSubmission::InvalidInput, output);
				return false;
			}
			std::array<RasterVertex, 4> corners{};
			constexpr std::array<std::array<double, 2>, 4> uv{{{{0.0, 0.0}}, {{1.0, 0.0}}, {{1.0, 1.0}}, {{0.0, 1.0}}}};
			for (std::size_t index = 0; index < corners.size(); ++index)
			{
				const auto& position = command.Geometry.Corners[index];
				if (!std::isfinite(position[0]) || !std::isfinite(position[1]))
				{
					CompleteSubmission(progress, ImageSubmission::InvalidInput, output);
					return false;
				}
				corners[index] = {position[0], position[1], uv[index][0], uv[index][1]};
			}
			if (!IntersectsGrid(command.Geometry))
			{
				CompleteSubmission(progress, ImageSubmission::Culled, output);
				return false;
			}
			progress.ImageCorners = corners;
			BuildExactImageMapping(command, corners, progress.ImageMapping);
			progress.ImageInitialized = true;
			progress.SubmittedAny = true;
		}
		return true;
	}

	/** Validates and prepares one solid triangle without retaining its borrowed source record. */
	Rgba8ImageDestination::RasterSetupResult Rgba8ImageDestination::PrepareSolidRasterTriangle(
		const SolidTriangleBatch& batch,
		std::size_t triangleIndex,
		SubmissionProgress& progress,
		RasterProgress& rasterProgress) noexcept
	{
		const SolidTriangle& triangle = batch.Triangles[triangleIndex];
		if (!HasFiniteTriangleXY(triangle))
		{
			return RasterSetupResult::InvalidInput;
		}
		if (!IntersectsGrid(triangle))
		{
			return RasterSetupResult::Skipped;
		}
		progress.SubmittedAny = true;
		std::array<RasterVertex, 3> vertices{};
		for (std::size_t index = 0; index < vertices.size(); ++index)
		{
			vertices[index] = {triangle.Vertices[index][0], triangle.Vertices[index][1], 0.0, 0.0};
		}
		progress.SolidColor = {
			float(SrgbToLinear(std::uint8_t((batch.Rgb >> 16) & 0xFF))),
			float(SrgbToLinear(std::uint8_t((batch.Rgb >> 8) & 0xFF))),
			float(SrgbToLinear(std::uint8_t(batch.Rgb & 0xFF))),
			float(batch.Opacity) / 255.0f};
		return PrepareRasterProgress(vertices, rasterProgress)
			? RasterSetupResult::Ready
			: RasterSetupResult::Rejected;
	}

	/** Validates one textured record and preserves the established UV transform arithmetic. */
	Rgba8ImageDestination::RasterSetupResult Rgba8ImageDestination::PrepareTexturedRasterTriangle(
		const TexturedTriangleBatch& batch,
		std::size_t triangleIndex,
		SubmissionProgress& progress,
		RasterProgress& rasterProgress) noexcept
	{
		const TexturedTriangle& triangle = batch.Triangles[triangleIndex];
		if (!HasFiniteTexturedTriangle(triangle))
		{
			return RasterSetupResult::InvalidInput;
		}
		if (!IntersectsGrid(SolidTriangle{{triangle.Vertices[0].Position, triangle.Vertices[1].Position,
			triangle.Vertices[2].Position}}))
		{
			return RasterSetupResult::Skipped;
		}
		progress.SubmittedAny = true;
		std::array<RasterVertex, 3> vertices{};
		for (std::size_t index = 0; index < vertices.size(); ++index)
		{
			const auto& source = triangle.Vertices[index];
			vertices[index] = {source.Position[0], source.Position[1],
				source.TextureCoordinates[0] * batch.UvScale[0] + batch.UvOffset[0],
				source.TextureCoordinates[1] * batch.UvScale[1] + batch.UvOffset[1]};
		}
		return PrepareRasterProgress(vertices, rasterProgress)
			? RasterSetupResult::Ready
			: RasterSetupResult::Rejected;
	}

	/** Validates all borrowed textured-batch metadata without retaining its source span. */
	bool Rgba8ImageDestination::IsValidTexturedBatch(const TexturedTriangleBatch& batch) noexcept
	{
		ImageSubmissionCommand sourceValidation;
		sourceValidation.Source = batch.Source;
		return IsValidImage(sourceValidation) && std::isfinite(batch.UvScale[0]) &&
			std::isfinite(batch.UvScale[1]) && std::isfinite(batch.UvOffset[0]) &&
			std::isfinite(batch.UvOffset[1]) &&
			(batch.AddressU == TextureAddressMode::Clamp || batch.AddressU == TextureAddressMode::Repeat) &&
			(batch.AddressV == TextureAddressMode::Clamp || batch.AddressV == TextureAddressMode::Repeat);
	}

	/** Starts or resumes one finalization identity and prepares its caller-owned output storage. */
	bool Rgba8ImageDestination::BeginFinalization(
		std::vector<std::uint8_t>& output,
		FinalizationProgress& progress,
		FinalizationSliceResult& result) noexcept
	{
		if (!progress.Started)
		{
			return StartFinalization(output, progress, result);
		}
		if (progress.DestinationIdentity != reinterpret_cast<std::uintptr_t>(this) ||
			progress.OutputIdentity != reinterpret_cast<std::uintptr_t>(&output) ||
			activeFinalizationId_ != progress.OperationId || activeSubmissionId_ != 0 || failed_)
		{
			FailFinalization(output, progress, result);
			return false;
		}
		if (!output.empty() || finalized_ || pixels_.empty())
		{
			FailFinalization(output, progress, result);
			return false;
		}
		return true;
	}

	/** Captures destination/output identities without allocating a duplicate image. */
	bool Rgba8ImageDestination::StartFinalization(
		std::vector<std::uint8_t>& output,
		FinalizationProgress& progress,
		FinalizationSliceResult& result) noexcept
	{
		progress.Started = true;
		progress.DestinationIdentity = reinterpret_cast<std::uintptr_t>(this);
		progress.OutputIdentity = reinterpret_cast<std::uintptr_t>(&output);
		if (activeSubmissionId_ != 0 || activeFinalizationId_ != 0)
		{
			FailFinalization(output, progress, result);
			return false;
		}
		progress.OperationId = nextOperationId_++;
		if (nextOperationId_ == 0)
		{
			nextOperationId_ = 1;
		}
		activeFinalizationId_ = progress.OperationId;
		output.clear();
		const std::size_t width = grid_.Width;
		if (!initialized_ || failed_ || finalized_ || width == 0 ||
			std::size_t(grid_.Height) > (std::numeric_limits<std::size_t>::max)() / width)
		{
			FailFinalization(output, progress, result);
			return false;
		}
		const std::size_t pixels = width * grid_.Height;
		if (pixels > (std::numeric_limits<std::size_t>::max)() / ChannelsPerPixel ||
			pixels_.size() != pixels * ChannelsPerPixel)
		{
			FailFinalization(output, progress, result);
			return false;
		}
		return true;
	}
	/** Transfers the complete buffer once after the pixel validation pass succeeds. */
	bool Rgba8ImageDestination::TransferCompletedImage(std::vector<std::uint8_t>& output) noexcept
	{
		if (failed_ || !initialized_ || finalized_ || output.size() != 0 ||
			pixels_.size() != std::size_t(grid_.Width) * grid_.Height * ChannelsPerPixel)
		{
			return false;
		}
		output = std::move(pixels_);
		finalized_ = true;
		return true;
	}

	/** Prepares one triangle's exact synchronous setup and initializes Y-major raster progress. */
	bool Rgba8ImageDestination::PrepareRasterProgress(
		std::array<RasterVertex, 3> vertices,
		RasterProgress& progress) const noexcept
	{
		progress = {};
		for (const auto& vertex : vertices)
		{
			if (!std::isfinite(vertex.X) || !std::isfinite(vertex.Y) ||
				!std::isfinite(vertex.U) || !std::isfinite(vertex.V))
			{
				return false;
			}
		}
		progress.SourceVertices = vertices;
		double area = Edge(vertices[0], vertices[1], vertices[2].X, vertices[2].Y);
		if (!std::isfinite(area))
		{
			return false;
		}
		progress.SetupComplete = true;
		if (area == 0.0)
		{
			return true;
		}
		progress.Swapped = area < 0.0;
		if (progress.Swapped)
		{
			std::swap(vertices[1], vertices[2]);
			area = -area;
		}
		bool hasPixels = false;
		if (!TryGetRasterBounds(vertices, progress.Bounds, hasPixels))
		{
			return false;
		}
		progress.Vertices = vertices;
		progress.Area = area;
		progress.HasPixels = hasPixels;
		if (!hasPixels)
		{
			return true;
		}
		progress.TopLeftEdges = {
			IsTopLeftEdge(vertices[1], vertices[2]),
			IsTopLeftEdge(vertices[2], vertices[0]),
			IsTopLeftEdge(vertices[0], vertices[1])};
		progress.NextPixelX = progress.Bounds[0];
		progress.NextPixelY = progress.Bounds[1];
		return true;
	}

	/** Advances one retained pixel coordinate in row-major raster order. */
	void Rgba8ImageDestination::AdvanceRasterPixel(RasterProgress& progress) noexcept
	{
		if (progress.NextPixelX < progress.Bounds[2])
		{
			++progress.NextPixelX;
			return;
		}
		progress.NextPixelX = progress.Bounds[0];
		++progress.NextPixelY;
		if (progress.NextPixelY > progress.Bounds[3])
		{
			progress.HasPixels = false;
		}
	}

	/** Consumes one triangle-setup work unit when deadline allows. */
	template <typename PrepareFunction>
	Rgba8ImageDestination::RasterSetupResult Rgba8ImageDestination::StepRasterTriangleSetup(
		SubmissionProgress& progress,
		std::chrono::steady_clock::time_point deadline,
		SubmissionSliceResult& result,
		PrepareFunction&& prepare) noexcept
	{
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return RasterSetupResult::DeadlineReached;
		}
		++result.WorkUnitsConsumed;
		return prepare(progress.TriangleIndex, progress.Raster);
	}

	/** Visits one destination pixel center as one non-splittable work unit. */
	template <typename ShadeFunction>
	Rgba8ImageDestination::RasterPixelStepResult Rgba8ImageDestination::StepRasterPixel(
		SubmissionProgress& progress,
		std::chrono::steady_clock::time_point deadline,
		SubmissionSliceResult& result,
		ShadeFunction&& shade) noexcept
	{
		if ((result.WorkUnitsConsumed % 32 == 0) &&
			std::chrono::steady_clock::now() >= deadline)
		{
			return RasterPixelStepResult::DeadlineReached;
		}
		++result.WorkUnitsConsumed;
		if (!RasterPixel(
			progress.Raster.Vertices,
			progress.Raster.NextPixelX,
			progress.Raster.NextPixelY,
			progress.Raster.Area,
			progress.Raster.Swapped,
			progress.Raster.TopLeftEdges,
			pixels_,
			shade))
		{
			return RasterPixelStepResult::Rejected;
		}
		AdvanceRasterPixel(progress.Raster);
		return RasterPixelStepResult::Advanced;
	}

	/** Advances one ordered triangle by one setup or complete-pixel work unit. */
	template <typename PrepareFunction, typename ShadeFunction>
	Rgba8ImageDestination::RasterSequenceStepResult
	Rgba8ImageDestination::StepRasterTriangle(
		SubmissionProgress& progress,
		std::chrono::steady_clock::time_point deadline,
		SubmissionSliceResult& result,
		PrepareFunction&& prepare,
		ShadeFunction&& shade) noexcept
	{
		if (!progress.Raster.SetupComplete)
		{
			const RasterSetupResult setupResult = StepRasterTriangleSetup(
				progress,
				deadline,
				result,
				prepare);
			if (setupResult == RasterSetupResult::DeadlineReached)
			{
				return RasterSequenceStepResult::DeadlineReached;
			}
			if (setupResult == RasterSetupResult::InvalidInput)
			{
				return RasterSequenceStepResult::InvalidInput;
			}
			if (setupResult == RasterSetupResult::Rejected)
			{
				return RasterSequenceStepResult::Rejected;
			}
			if (setupResult == RasterSetupResult::Skipped)
			{
				++progress.TriangleIndex;
				progress.Raster = {};
				return RasterSequenceStepResult::Progressed;
			}
		}
		if (progress.Raster.HasPixels)
		{
			const RasterPixelStepResult pixelResult = StepRasterPixel(
				progress,
				deadline,
				result,
				shade);
			if (pixelResult == RasterPixelStepResult::DeadlineReached)
			{
				return RasterSequenceStepResult::DeadlineReached;
			}
			if (pixelResult == RasterPixelStepResult::Rejected)
			{
				return RasterSequenceStepResult::Rejected;
			}
			if (progress.Raster.HasPixels)
			{
				return RasterSequenceStepResult::Progressed;
			}
		}
		++progress.TriangleIndex;
		progress.Raster = {};
		return RasterSequenceStepResult::Progressed;
	}

	/** Consumes setup and complete-pixel work units for one ordered triangle sequence. */
	template <typename PrepareFunction, typename ShadeFunction>
	Rgba8ImageDestination::SubmissionSliceResult
	Rgba8ImageDestination::StepRasterTriangleSequence(
		SubmissionProgress& progress,
		std::size_t triangleCount,
		std::chrono::steady_clock::time_point deadline,
		PrepareFunction&& prepare,
		ShadeFunction&& shade) noexcept
	{
		SubmissionSliceResult result;
		while (progress.TriangleIndex < triangleCount)
		{
			const RasterSequenceStepResult stepResult = StepRasterTriangle(progress, deadline, result, prepare, shade);
			if (stepResult == RasterSequenceStepResult::DeadlineReached) break;
			if (stepResult == RasterSequenceStepResult::InvalidInput)
			{
				CompleteSubmission(progress, ImageSubmission::InvalidInput, result);
				return result;
			}
			if (stepResult == RasterSequenceStepResult::Rejected)
			{
				failed_ = true;
				CompleteSubmission(progress, ImageSubmission::Rejected, result);
				return result;
			}
		}
		if (progress.TriangleIndex == triangleCount)
		{
			CompleteSubmission(progress,
				progress.SubmittedAny ? ImageSubmission::Submitted : ImageSubmission::Culled, result);
		}
		return result;
	}

	/** Resumes one image quad's source-ordered pair of triangles using call-local pixels. */
	Rgba8ImageDestination::SubmissionSliceResult Rgba8ImageDestination::StepSubmitImage(
		const ImageSubmissionCommand& command,
		SubmissionCursor& cursor,
		std::chrono::steady_clock::time_point deadline) noexcept
	{
		SubmissionSliceResult result;
		SubmissionProgress* progress = nullptr;
		if (!BeginSubmission(cursor, SubmissionProgress::OperationKind::Image, progress, result)) return result;
		if (!InitializeImageSubmission(command, *progress, result)) return result;
		auto prepare = [this, progress](std::size_t triangleIndex, RasterProgress& rasterProgress)
		{
			const auto& corners = progress->ImageCorners;
			const std::array<RasterVertex, 3> vertices = triangleIndex == 0
				? std::array<RasterVertex, 3>{corners[0], corners[1], corners[2]}
				: std::array<RasterVertex, 3>{corners[0], corners[2], corners[3]};
			return PrepareRasterProgress(vertices, rasterProgress)
				? RasterSetupResult::Ready
				: RasterSetupResult::Rejected;
		};
		auto shade = [this, &command, progress](double first, double second, double third,
			int pixelX, int pixelY, LinearPixel& output)
		{
			const ImageShadeContext context{
				command, progress->ImageCorners, progress->ImageMapping, progress->TriangleIndex != 0};
			return ShadeImage(context, first, second, third, pixelX, pixelY, output);
		};
		return StepRasterTriangleSequence(*progress, 2, deadline, prepare, shade);
	}

	/** Resumes ordered solid triangles without retaining their borrowed span. */
	Rgba8ImageDestination::SubmissionSliceResult Rgba8ImageDestination::StepSubmitSolidTriangles(
		const SolidTriangleBatch& batch,
		SubmissionCursor& cursor,
		std::chrono::steady_clock::time_point deadline) noexcept
	{
		SubmissionSliceResult result;
		SubmissionProgress* progress = nullptr;
		if (!BeginSubmission(cursor, SubmissionProgress::OperationKind::SolidTriangles, progress, result)) return result;
		if (batch.Triangles.empty())
		{
			CompleteSubmission(*progress, ImageSubmission::Culled, result);
			return result;
		}
		auto prepare = [this, &batch, progress](std::size_t triangleIndex, RasterProgress& rasterProgress)
		{
			return PrepareSolidRasterTriangle(batch, triangleIndex, *progress, rasterProgress);
		};
		auto shade = [progress](double, double, double, int, int, LinearPixel& output)
		{
			output = progress->SolidColor;
			return true;
		};
		return StepRasterTriangleSequence(*progress, batch.Triangles.size(), deadline, prepare, shade);
	}

	/** Resumes ordered textured triangles while borrowing pixels only for this call. */
	Rgba8ImageDestination::SubmissionSliceResult Rgba8ImageDestination::StepSubmitTexturedTriangles(
		const TexturedTriangleBatch& batch,
		SubmissionCursor& cursor,
		std::chrono::steady_clock::time_point deadline) noexcept
	{
		SubmissionSliceResult result;
		SubmissionProgress* progress = nullptr;
		if (!BeginSubmission(cursor, SubmissionProgress::OperationKind::TexturedTriangles, progress, result)) return result;
		if (!progress->TexturedBatchValidated && !IsValidTexturedBatch(batch))
		{
			CompleteSubmission(*progress, ImageSubmission::InvalidInput, result);
			return result;
		}
		progress->TexturedBatchValidated = true;
		if (batch.Triangles.empty())
		{
			CompleteSubmission(*progress, ImageSubmission::Culled, result);
			return result;
		}
		auto prepare = [this, &batch, progress](std::size_t triangleIndex, RasterProgress& rasterProgress)
		{
			return PrepareTexturedRasterTriangle(batch, triangleIndex, *progress, rasterProgress);
		};
		auto shade = [this, &batch, progress](double first, double second, double third,
			int, int, LinearPixel& output)
		{
			const TextureShadeContext context{batch};
			return ShadeTexture(context, progress->Raster.SourceVertices, first, second, third, output);
		};
		return StepRasterTriangleSequence(*progress, batch.Triangles.size(), deadline, prepare, shade);
	}

	/** Validates the image command, then rasterizes its two source-ordered quad triangles. */
	ImageSubmission Rgba8ImageDestination::SubmitImage(const ImageSubmissionCommand& command)
	{
		if (!initialized_ || failed_ || finalized_) return ImageSubmission::Unavailable;
		if (activeSubmissionId_ != 0 || activeFinalizationId_ != 0)
		{
			failed_ = true;
			activeSubmissionId_ = 0;
			activeFinalizationId_ = 0;
			return ImageSubmission::Rejected;
		}
		if (!IsValidImage(command)) return ImageSubmission::InvalidInput;

		std::array<RasterVertex, 4> corners{};
		constexpr std::array<std::array<double, 2>, 4> uv{{{{0.0, 0.0}}, {{1.0, 0.0}}, {{1.0, 1.0}}, {{0.0, 1.0}}}};
		for (std::size_t index = 0; index < corners.size(); ++index)
		{
			const auto& position = command.Geometry.Corners[index];
			if (!std::isfinite(position[0]) || !std::isfinite(position[1])) return ImageSubmission::InvalidInput;
			corners[index] = {position[0], position[1], uv[index][0], uv[index][1]};
		}
		if (!IntersectsGrid(command.Geometry)) return ImageSubmission::Culled;

		const std::size_t bufferSize = pixels_.size();
		ExactImageMapping imageMapping;
		BuildExactImageMapping(command, corners, imageMapping);
		ImageShadeContext first{command, corners, imageMapping, false};
		ImageShadeContext second{command, corners, imageMapping, true};
		const bool firstComplete = RasterTriangle({corners[0], corners[1], corners[2]}, pixels_,
			[&](double a, double b, double c, int pixelX, int pixelY, LinearPixel& output)
			{
				return ShadeImage(first, a, b, c, pixelX, pixelY, output);
			});
		const bool complete = firstComplete && RasterTriangle({corners[0], corners[2], corners[3]}, pixels_,
			[&](double a, double b, double c, int pixelX, int pixelY, LinearPixel& output)
			{
				return ShadeImage(second, a, b, c, pixelX, pixelY, output);
			});
		if (!complete || pixels_.size() != bufferSize)
		{
			failed_ = true;
			return ImageSubmission::Rejected;
		}
		return ImageSubmission::Submitted;
	}

	/** Submits solid triangles synchronously in span order and poisons finalization after raster failure. */
	ImageSubmission Rgba8ImageDestination::SubmitSolidTriangles(const SolidTriangleBatch& batch)
	{
		if (!initialized_ || failed_ || finalized_) return ImageSubmission::Unavailable;
		if (activeSubmissionId_ != 0 || activeFinalizationId_ != 0)
		{
			failed_ = true;
			activeSubmissionId_ = 0;
			activeFinalizationId_ = 0;
			return ImageSubmission::Rejected;
		}
		if (batch.Triangles.empty()) return ImageSubmission::Culled;

		bool submitted = false;
		for (const auto& triangle : batch.Triangles)
		{
			if (!HasFiniteTriangleXY(triangle)) return ImageSubmission::InvalidInput;
			if (!IntersectsGrid(triangle)) continue;
			const std::size_t bufferSize = pixels_.size();
			if (!RasterSolidTriangle(triangle, batch.Rgb, batch.Opacity) || pixels_.size() != bufferSize)
			{
				failed_ = true;
				return ImageSubmission::Rejected;
			}
			submitted = true;
		}
		return submitted ? ImageSubmission::Submitted : ImageSubmission::Culled;
	}

	/** Rasterizes textured triangles synchronously and poisons finalization after a raster failure. */
	ImageSubmission Rgba8ImageDestination::SubmitTexturedTriangles(const TexturedTriangleBatch& batch)
	{
		if (!initialized_ || failed_ || finalized_) return ImageSubmission::Unavailable;
		if (activeSubmissionId_ != 0 || activeFinalizationId_ != 0)
		{
			failed_ = true;
			activeSubmissionId_ = 0;
			activeFinalizationId_ = 0;
			return ImageSubmission::Rejected;
		}
		ImageSubmissionCommand sourceValidation;
		sourceValidation.Source = batch.Source;
		if (!IsValidImage(sourceValidation) || !std::isfinite(batch.UvScale[0]) ||
			!std::isfinite(batch.UvScale[1]) || !std::isfinite(batch.UvOffset[0]) ||
			!std::isfinite(batch.UvOffset[1]) ||
			(batch.AddressU != TextureAddressMode::Clamp && batch.AddressU != TextureAddressMode::Repeat) ||
			(batch.AddressV != TextureAddressMode::Clamp && batch.AddressV != TextureAddressMode::Repeat))
		{
			return ImageSubmission::InvalidInput;
		}
		if (batch.Triangles.empty()) return ImageSubmission::Culled;

		bool submitted = false;
		for (const auto& triangle : batch.Triangles)
		{
			if (!HasFiniteTexturedTriangle(triangle)) return ImageSubmission::InvalidInput;
			if (!IntersectsGrid(SolidTriangle{{triangle.Vertices[0].Position, triangle.Vertices[1].Position,
				triangle.Vertices[2].Position}})) continue;
			const std::size_t bufferSize = pixels_.size();
			if (!RasterTexturedTriangle(triangle, batch) || pixels_.size() != bufferSize)
			{
				failed_ = true;
				return ImageSubmission::Rejected;
			}
			submitted = true;
		}
		return submitted ? ImageSubmission::Submitted : ImageSubmission::Culled;
	}

	/** Transfers the complete RGBA8 buffer after validating its completion state. */
	bool Rgba8ImageDestination::Finalize(std::vector<std::uint8_t>& output) noexcept
	{
		output.clear();
		if (activeSubmissionId_ != 0 || activeFinalizationId_ != 0 || failed_ || !initialized_)
		{
			output.clear();
			return false;
		}
		return TransferCompletedImage(output);
	}

	/** Validates pixels in bounded slices and transfers output only after completion. */
	Rgba8ImageDestination::FinalizationSliceResult Rgba8ImageDestination::StepFinalize(
		std::vector<std::uint8_t>& output,
		FinalizationCursor& cursor,
		std::chrono::steady_clock::time_point deadline) noexcept
	{
		FinalizationSliceResult result;
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return result;
		}
		FinalizationProgress* progress = dynamic_cast<FinalizationProgress*>(GetCursorState(cursor));
		if (GetCursorState(cursor) != nullptr && progress == nullptr)
		{
			result.Status = FinalizationStepStatus::Failed;
			return result;
		}
		if (progress == nullptr)
		{
			try
			{
				auto state = std::make_unique<FinalizationProgress>();
				progress = state.get();
				SetCursorState(cursor, std::move(state));
			}
			catch (...)
			{
				result.Status = FinalizationStepStatus::Failed;
				return result;
			}
		}
		if (progress->DestinationIdentity != 0 && progress->DestinationIdentity != reinterpret_cast<std::uintptr_t>(this))
		{
			result.Status = FinalizationStepStatus::Failed;
			return result;
		}
		if (progress->Started && progress->OutputIdentity != reinterpret_cast<std::uintptr_t>(&output))
		{
			return FailFinalization(output, *progress, result);
		}
		if (progress->Terminal)
		{
			const std::size_t expectedPixelCount = std::size_t(grid_.Width) * grid_.Height;
			const bool expectedOutput = progress->TerminalStatus == FinalizationStepStatus::Complete &&
				expectedPixelCount <= (std::numeric_limits<std::size_t>::max)() / ChannelsPerPixel &&
				output.size() == expectedPixelCount * ChannelsPerPixel;
			if (progress->TerminalStatus == FinalizationStepStatus::Complete && !expectedOutput)
			{
				return FailFinalization(output, *progress, result);
			}
			if (progress->TerminalStatus == FinalizationStepStatus::Failed && !output.empty())
			{
				return FailFinalization(output, *progress, result);
			}
			result.Status = progress->TerminalStatus;
			result.FullyTransparent = progress->FullyTransparent;
			return result;
		}
		if (progress->Started && !output.empty()) return FailFinalization(output, *progress, result);
		if (!BeginFinalization(output, *progress, result))
		{
			result.Status = progress->TerminalStatus;
			return result;
		}
		const std::size_t pixelCount = pixels_.size() / ChannelsPerPixel;
		while (progress->NextPixel < pixelCount)
		{
			if (result.PixelsResolved % 32 == 0 && std::chrono::steady_clock::now() >= deadline) break;
			const std::size_t alphaOffset = progress->NextPixel * ChannelsPerPixel + 3;
			if (pixels_[alphaOffset] != 0) progress->FullyTransparent = false;
			++progress->NextPixel;
			++result.PixelsResolved;
		}
		if (progress->NextPixel == pixelCount)
		{
			if (!TransferCompletedImage(output)) return FailFinalization(output, *progress, result);
			progress->Terminal = true;
			progress->TerminalStatus = FinalizationStepStatus::Complete;
			activeFinalizationId_ = 0;
			result.Status = progress->TerminalStatus;
			result.FullyTransparent = progress->FullyTransparent;
		}
		return result;
	}

	/** Validates grid dimensions, allocation arithmetic, scale, and signed-cell world bounds. */
	bool Rgba8ImageDestination::IsValidGrid(
		const ImageGrid& grid,
		std::array<long double, 4>& worldBounds) noexcept
	{
		if (!grid.Width || !grid.Height || grid.Width > INT_MAX || grid.Height > INT_MAX ||
			!std::isfinite(grid.WorldOriginX) || !std::isfinite(grid.WorldOriginY) ||
			!std::isfinite(grid.WorldUnitsPerPixel) || grid.WorldUnitsPerPixel <= 0.0)
		{
			return false;
		}

		const std::size_t pixels = std::size_t(grid.Width) * std::size_t(grid.Height);
		if (pixels / grid.Width != grid.Height || pixels > (std::numeric_limits<std::size_t>::max)() / ChannelsPerPixel)
		{
			return false;
		}
		return TryGetGridBounds(grid, worldBounds);
	}

	/** Calculates finite half-open world bounds using widened signed-cell arithmetic. */
	bool Rgba8ImageDestination::TryGetGridBounds(
		const ImageGrid& grid,
		std::array<long double, 4>& bounds) noexcept
	{
		const long double left = static_cast<long double>(grid.WorldOriginX) +
			static_cast<long double>(grid.CellX) * grid.Width * grid.WorldUnitsPerPixel;
		const long double bottom = static_cast<long double>(grid.WorldOriginY) +
			static_cast<long double>(grid.CellY) * grid.Height * grid.WorldUnitsPerPixel;
		const long double right = left + static_cast<long double>(grid.Width) * grid.WorldUnitsPerPixel;
		const long double top = bottom + static_cast<long double>(grid.Height) * grid.WorldUnitsPerPixel;
		if (!std::isfinite(left) || !std::isfinite(bottom) || !std::isfinite(right) || !std::isfinite(top)) return false;
		bounds = {left, bottom, right, top};
		return true;
	}

	/** Validates the borrowed image view, dimensions, exact RGBA8 byte count, and the half-open pixel region. */
	bool Rgba8ImageDestination::IsValidImage(const ImageSubmissionCommand& command) noexcept
	{
		const auto& source = command.Source;
		if (source.Pixels.empty() || !source.Width || !source.Height) return false;
		const std::size_t pixels = std::size_t(source.Width) * std::size_t(source.Height);
		if (pixels / source.Width != source.Height || pixels > (std::numeric_limits<std::size_t>::max)() / 4 ||
			source.Pixels.size() != pixels * 4)
		{
			return false;
		}
		for (double edge : source.PixelRegion)
		{
			if (!std::isfinite(edge)) return false;
		}
		return source.PixelRegion[0] >= 0.0 && source.PixelRegion[1] >= 0.0 &&
			source.PixelRegion[2] > source.PixelRegion[0] && source.PixelRegion[3] > source.PixelRegion[1] &&
			source.PixelRegion[2] <= source.Width && source.PixelRegion[3] <= source.Height;
	}

	/** Returns true when the world-space image quad overlaps or touches the destination grid. */
	bool Rgba8ImageDestination::IntersectsGrid(const ImageWorldQuad& quad) const noexcept
	{
		double minX = quad.Corners[0][0];
		double maxX = minX;
		double minY = quad.Corners[0][1];
		double maxY = minY;
		for (const auto& corner : quad.Corners)
		{
			minX = (std::min)(minX, corner[0]);
			maxX = (std::max)(maxX, corner[0]);
			minY = (std::min)(minY, corner[1]);
			maxY = (std::max)(maxY, corner[1]);
		}
		return !(static_cast<long double>(maxX) < worldBounds_[0] || static_cast<long double>(minX) > worldBounds_[2] ||
			static_cast<long double>(maxY) < worldBounds_[1] || static_cast<long double>(minY) > worldBounds_[3]);
	}

	/** Returns true when the triangle XY bounds overlap or touch the destination grid. */
	bool Rgba8ImageDestination::IntersectsGrid(const SolidTriangle& triangle) const noexcept
	{
		long double minX = triangle.Vertices[0][0];
		long double maxX = minX;
		long double minY = triangle.Vertices[0][1];
		long double maxY = minY;
		for (const auto& vertex : triangle.Vertices)
		{
			minX = (std::min)(minX, static_cast<long double>(vertex[0]));
			maxX = (std::max)(maxX, static_cast<long double>(vertex[0]));
			minY = (std::min)(minY, static_cast<long double>(vertex[1]));
			maxY = (std::max)(maxY, static_cast<long double>(vertex[1]));
		}
		return !(maxX < worldBounds_[0] || minX > worldBounds_[2] || maxY < worldBounds_[1] || minY > worldBounds_[3]);
	}

	/** Rejects non-finite XY while deliberately leaving Z unused by 2D rasterization. */
	bool Rgba8ImageDestination::HasFiniteTriangleXY(const SolidTriangle& triangle) noexcept
	{
		for (const auto& vertex : triangle.Vertices)
		{
			if (!std::isfinite(vertex[0]) || !std::isfinite(vertex[1])) return false;
		}
		return true;
	}

	/** Validates all position and texture coordinates before raster work begins. */
	bool Rgba8ImageDestination::HasFiniteTexturedTriangle(const TexturedTriangle& triangle) noexcept
	{
		for (const auto& vertex : triangle.Vertices)
		{
			for (const double value : vertex.Position)
			{
				if (!std::isfinite(value)) return false;
			}
			for (const double value : vertex.TextureCoordinates)
			{
				if (!std::isfinite(value)) return false;
			}
		}
		return true;
	}

	/** Recognizes a cardinal one-to-one quad aligned to source texels and destination pixels. */
	void Rgba8ImageDestination::BuildExactImageMapping(
		const ImageSubmissionCommand& command,
		const std::array<RasterVertex, 4>& corners,
		ExactImageMapping& output) const noexcept
	{
		constexpr double tolerance = 1e-8;
		std::array<std::array<long double, 2>, 4> pixels{};
		for (std::size_t index = 0; index < corners.size(); ++index)
		{
			pixels[index][0] = (static_cast<long double>(corners[index].X) - grid_.WorldOriginX) /
				grid_.WorldUnitsPerPixel - static_cast<long double>(grid_.CellX) * grid_.Width;
			pixels[index][1] = (static_cast<long double>(corners[index].Y) - grid_.WorldOriginY) /
				grid_.WorldUnitsPerPixel - static_cast<long double>(grid_.CellY) * grid_.Height;
			if (!std::isfinite(pixels[index][0]) || !std::isfinite(pixels[index][1])) return;
		}
		const long double uX = pixels[1][0] - pixels[0][0];
		const long double uY = pixels[1][1] - pixels[0][1];
		const long double vX = pixels[3][0] - pixels[0][0];
		const long double vY = pixels[3][1] - pixels[0][1];
		const bool uHorizontal = std::abs(uX) > tolerance && std::abs(uY) <= tolerance;
		const bool uVertical = std::abs(uY) > tolerance && std::abs(uX) <= tolerance;
		const bool vHorizontal = std::abs(vX) > tolerance && std::abs(vY) <= tolerance;
		const bool vVertical = std::abs(vY) > tolerance && std::abs(vX) <= tolerance;
		if (!((uHorizontal && vVertical) || (uVertical && vHorizontal))) return;
		if (std::abs(pixels[2][0] - pixels[1][0] - pixels[3][0] + pixels[0][0]) > tolerance ||
			std::abs(pixels[2][1] - pixels[1][1] - pixels[3][1] + pixels[0][1]) > tolerance) return;
		const int uAxis = uHorizontal ? 0 : 1;
		const int vAxis = vHorizontal ? 0 : 1;
		const long double sourceWidth = command.Source.PixelRegion[2] - command.Source.PixelRegion[0];
		const long double sourceHeight = command.Source.PixelRegion[3] - command.Source.PixelRegion[1];
		const long double uSpan = std::abs(uAxis == 0 ? uX : uY);
		const long double vSpan = std::abs(vAxis == 0 ? vX : vY);
		const long double uEdge = std::round(pixels[0][std::size_t(uAxis)]);
		const long double vEdge = std::round(pixels[0][std::size_t(vAxis)]);
		const auto aligned = [](long double value)
		{
			return std::abs(value - std::round(value)) <= 1e-8L;
		};
		if (!aligned(command.Source.PixelRegion[0]) || !aligned(command.Source.PixelRegion[1]) ||
			!aligned(command.Source.PixelRegion[2]) || !aligned(command.Source.PixelRegion[3]) ||
			!aligned(pixels[0][std::size_t(uAxis)]) || !aligned(pixels[0][std::size_t(vAxis)]) ||
			std::abs(uSpan - sourceWidth) > tolerance || std::abs(vSpan - sourceHeight) > tolerance ||
			uEdge < std::numeric_limits<int>::min() || uEdge > std::numeric_limits<int>::max() ||
			vEdge < std::numeric_limits<int>::min() || vEdge > std::numeric_limits<int>::max()) return;
		output.DestinationAxes = {{uAxis, vAxis}};
		output.DestinationEdges = {{int(std::llround(pixels[0][std::size_t(uAxis)])),
			int(std::llround(pixels[0][std::size_t(vAxis)]))}};
		output.SourceStarts = {{std::int64_t(std::llround(command.Source.PixelRegion[0])),
			std::int64_t(std::llround(command.Source.PixelRegion[1]))}};
		output.Forward = {{(uAxis == 0 ? uX : uY) > 0.0L, (vAxis == 0 ? vX : vY) > 0.0L}};
		output.Enabled = true;
	}

	/** Reads one texel's original bytes without decoding or re-encoding its sRGB color. */
	bool Rgba8ImageDestination::FetchOriginalPixel(
		const Rgba8ImageSource& source,
		std::int64_t pixelX,
		std::int64_t pixelY,
		LinearPixel& output) noexcept
	{
		if (pixelX < 0 || pixelY < 0 || pixelX >= source.Width || pixelY >= source.Height) return false;
		const std::size_t offset = (std::size_t(pixelY) * source.Width + std::size_t(pixelX)) * ChannelsPerPixel;
		if (offset > source.Pixels.size() || source.Pixels.size() - offset < ChannelsPerPixel) return false;
		const auto* bytes = source.Pixels.data() + offset;
		output = {0.0f, 0.0f, 0.0f, float(bytes[3]) / 255.0f,
			{bytes[0], bytes[1], bytes[2], bytes[3]}, true};
		return true;
	}

	/** Samples the nearest source texel at the normalized coordinate within the half-open region. */
	bool Rgba8ImageDestination::SampleRgba8(
		const Rgba8ImageSource& source,
		double u,
		double v,
		TextureAddressMode addressU,
		TextureAddressMode addressV,
		LinearPixel& output) noexcept
	{
		if (source.Pixels.empty() || source.Width == 0 || source.Height == 0 || !std::isfinite(u) || !std::isfinite(v))
		{
			return false;
		}
		const double regionWidth = source.PixelRegion[2] - source.PixelRegion[0];
		const double regionHeight = source.PixelRegion[3] - source.PixelRegion[1];
		if (!std::isfinite(regionWidth) || !std::isfinite(regionHeight) || regionWidth <= 0.0 || regionHeight <= 0.0)
		{
			return false;
		}
		u = addressU == TextureAddressMode::Repeat ? u - std::floor(u) : (std::clamp)(u, 0.0, 1.0);
		v = addressV == TextureAddressMode::Repeat ? v - std::floor(v) : (std::clamp)(v, 0.0, 1.0);
		const std::int64_t minimumX = static_cast<std::int64_t>(std::ceil(source.PixelRegion[0] - 0.5));
		const std::int64_t minimumY = static_cast<std::int64_t>(std::ceil(source.PixelRegion[1] - 0.5));
		const std::int64_t maximumX = static_cast<std::int64_t>(std::ceil(source.PixelRegion[2] - 0.5)) - 1;
		const std::int64_t maximumY = static_cast<std::int64_t>(std::ceil(source.PixelRegion[3] - 0.5)) - 1;
		const auto pixelX = static_cast<std::int64_t>(std::floor(source.PixelRegion[0] + u * regionWidth));
		const auto pixelY = static_cast<std::int64_t>(std::floor(source.PixelRegion[1] + v * regionHeight));
		return FetchPixel(source, pixelX, pixelY, minimumX, minimumY, maximumX, maximumY,
			TextureAddressMode::Clamp, TextureAddressMode::Clamp, output);
	}

	/** Reads an addressed texel and retains its original straight-alpha RGBA8 bytes. */
	bool Rgba8ImageDestination::FetchPixel(
		const Rgba8ImageSource& source,
		std::int64_t pixelX,
		std::int64_t pixelY,
		std::int64_t minX,
		std::int64_t minY,
		std::int64_t maxX,
		std::int64_t maxY,
		TextureAddressMode addressU,
		TextureAddressMode addressV,
		LinearPixel& output) noexcept
	{
		const std::int64_t boundedMinimumX = (std::max)(std::int64_t(0), minX);
		const std::int64_t boundedMinimumY = (std::max)(std::int64_t(0), minY);
		const std::int64_t boundedMaximumX = (std::min)(std::int64_t(source.Width) - 1, maxX);
		const std::int64_t boundedMaximumY = (std::min)(std::int64_t(source.Height) - 1, maxY);
		if (boundedMinimumX > boundedMaximumX || boundedMinimumY > boundedMaximumY) return false;
		pixelX = AddressPixel(pixelX, boundedMinimumX, boundedMaximumX, addressU);
		pixelY = AddressPixel(pixelY, boundedMinimumY, boundedMaximumY, addressV);
		if (pixelX < 0 || pixelY < 0) return false;
		const std::size_t offset = (std::size_t(pixelY) * source.Width + std::size_t(pixelX)) * 4;
		if (offset > source.Pixels.size() || source.Pixels.size() - offset < 4) return false;
		const auto* bytes = source.Pixels.data() + offset;
		output = {0.0f, 0.0f, 0.0f, float(bytes[3]) / 255.0f,
			{bytes[0], bytes[1], bytes[2], bytes[3]}, true};
		return true;
	}

	/** Applies clamp or positive modulo within the validated source-region pixel range. */
	std::int64_t Rgba8ImageDestination::AddressPixel(
		std::int64_t pixel,
		std::int64_t minimum,
		std::int64_t maximum,
		TextureAddressMode addressMode) noexcept
	{
		if (addressMode == TextureAddressMode::Clamp) return (std::clamp)(pixel, minimum, maximum);
		const std::int64_t count = maximum - minimum + 1;
		const std::int64_t offset = (pixel - minimum) % count;
		return minimum + (offset < 0 ? offset + count : offset);
	}

	/** Returns one precomputed linear-light value for an 8-bit sRGB channel. */
	double Rgba8ImageDestination::SrgbToLinear(std::uint8_t channel) noexcept
	{
		static const std::array<double, 256> lookup = []
		{
			std::array<double, 256> values{};
			for (std::size_t index = 0; index < values.size(); ++index)
			{
				const double value = double(index) / 255.0;
				values[index] = value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
			}
			return values;
		}();
		return lookup[channel];
	}

	/** Uses exact integer mapping when available and nearest-center sampling otherwise. */
	bool Rgba8ImageDestination::ShadeImage(
		const ImageShadeContext& context,
		double firstWeight,
		double secondWeight,
		double thirdWeight,
		int pixelX,
		int pixelY,
		LinearPixel& output) noexcept
	{
		const auto& corners = context.Corners;
		const auto& sampling = context.ExactMapping;
		if (sampling.Enabled)
		{
			const int uCoordinate = sampling.DestinationAxes[0] == 0 ? pixelX : pixelY;
			const int vCoordinate = sampling.DestinationAxes[1] == 0 ? pixelX : pixelY;
			const int uOffset = sampling.Forward[0]
				? uCoordinate - sampling.DestinationEdges[0] : sampling.DestinationEdges[0] - uCoordinate - 1;
			const int vOffset = sampling.Forward[1]
				? vCoordinate - sampling.DestinationEdges[1] : sampling.DestinationEdges[1] - vCoordinate - 1;
			const auto sourceX = sampling.SourceStarts[0] + uOffset;
			const auto sourceY = sampling.SourceStarts[1] + vOffset;
			return FetchOriginalPixel(context.Command.Source, sourceX, sourceY, output);
		}
		const std::size_t first = 0;
		const std::size_t second = context.OtherTriangle ? 2 : 1;
		const std::size_t third = context.OtherTriangle ? 3 : 2;
		const double u = corners[first].U * firstWeight + corners[second].U * secondWeight + corners[third].U * thirdWeight;
		const double v = corners[first].V * firstWeight + corners[second].V * secondWeight + corners[third].V * thirdWeight;
		return SampleRgba8(context.Command.Source, u, v, TextureAddressMode::Clamp,
			TextureAddressMode::Clamp, output);
	}

	/** Samples barycentric UVs and decodes RGB only when modulation or opacity requires it. */
	bool Rgba8ImageDestination::ShadeTexture(
		const TextureShadeContext& context,
		const std::array<RasterVertex, 3>& vertices,
		double firstWeight,
		double secondWeight,
		double thirdWeight,
		LinearPixel& output) noexcept
	{
		const auto& batch = context.Batch;
		const double u = vertices[0].U * firstWeight + vertices[1].U * secondWeight + vertices[2].U * thirdWeight;
		const double v = vertices[0].V * firstWeight + vertices[1].V * secondWeight + vertices[2].V * thirdWeight;
		if (!SampleRgba8(batch.Source, u, v, batch.AddressU, batch.AddressV, output)) return false;
		if (batch.ModulationRgb != 0xFFFFFF || batch.Opacity != 255)
		{
			output.PreserveOriginal = false;
			output.R = float(SrgbToLinear(output.Original[0]));
			output.G = float(SrgbToLinear(output.Original[1]));
			output.B = float(SrgbToLinear(output.Original[2]));
		}
		if (batch.ModulationRgb != 0xFFFFFF)
		{
			output.R *= float(SrgbToLinear(std::uint8_t((batch.ModulationRgb >> 16) & 0xFF)));
			output.G *= float(SrgbToLinear(std::uint8_t((batch.ModulationRgb >> 8) & 0xFF)));
			output.B *= float(SrgbToLinear(std::uint8_t(batch.ModulationRgb & 0xFF)));
		}
		if (batch.Opacity != 255)
		{
			output.A *= float(batch.Opacity) / 255.0f;
		}
		return true;
	}

	/** Rasterizes one triangle with shared-edge top-left ownership and the established pixel-center sampling. */
	template <typename ShadeFunction>
	bool Rgba8ImageDestination::RasterTriangle(
		std::array<RasterVertex, 3> vertices,
		PixelBuffer& pixels,
		ShadeFunction&& shade) const noexcept
	{
		for (const auto& vertex : vertices)
		{
			if (!std::isfinite(vertex.X) || !std::isfinite(vertex.Y) ||
				!std::isfinite(vertex.U) || !std::isfinite(vertex.V)) return false;
		}
		double area = Edge(vertices[0], vertices[1], vertices[2].X, vertices[2].Y);
		if (!std::isfinite(area)) return false;
		if (area == 0.0) return true;
		const bool swapped = area < 0.0;
		if (swapped)
		{
			std::swap(vertices[1], vertices[2]);
			area = -area;
		}
		std::array<int, 4> bounds{};
		bool hasPixels = false;
		if (!TryGetRasterBounds(vertices, bounds, hasPixels)) return false;
		if (!hasPixels) return true;
		const std::array<bool, 3> topLeftEdges{{
			IsTopLeftEdge(vertices[1], vertices[2]),
			IsTopLeftEdge(vertices[2], vertices[0]),
			IsTopLeftEdge(vertices[0], vertices[1])}};
		return RasterizePixels(vertices, bounds, area, swapped, topLeftEdges, pixels, shade);
	}

	/** Calculates clamped integer pixel bounds while preserving the reference's one-pixel conservative expansion. */
	bool Rgba8ImageDestination::TryGetRasterBounds(
		const std::array<RasterVertex, 3>& vertices,
		std::array<int, 4>& bounds,
		bool& hasPixels) const noexcept
	{
		const long double sideX = grid_.Width;
		const long double sideY = grid_.Height;
		long double minX = std::numeric_limits<long double>::infinity();
		long double minY = minX;
		long double maxX = -minX;
		long double maxY = -minX;
		for (const auto& vertex : vertices)
		{
			const long double pixelX = (static_cast<long double>(vertex.X) - grid_.WorldOriginX) / grid_.WorldUnitsPerPixel -
				static_cast<long double>(grid_.CellX) * sideX;
			const long double pixelY = (static_cast<long double>(vertex.Y) - grid_.WorldOriginY) / grid_.WorldUnitsPerPixel -
				static_cast<long double>(grid_.CellY) * sideY;
			minX = (std::min)(minX, pixelX);
			maxX = (std::max)(maxX, pixelX);
			minY = (std::min)(minY, pixelY);
			maxY = (std::max)(maxY, pixelY);
		}
		if (!std::isfinite(minX) || !std::isfinite(maxX) || !std::isfinite(minY) || !std::isfinite(maxY)) return false;
		if (maxX < 0.0L || maxY < 0.0L || minX > sideX || minY > sideY)
		{
			hasPixels = false;
			return true;
		}
		bounds = {LowPixel(minX, grid_.Width), LowPixel(minY, grid_.Height),
			HighPixel(maxX, grid_.Width), HighPixel(maxY, grid_.Height)};
		hasPixels = bounds[0] <= bounds[2] && bounds[1] <= bounds[3];
		return true;
	}

	/** Computes the reference conservative lower raster bound. */
	int Rgba8ImageDestination::LowPixel(long double value, std::uint32_t extent) noexcept
	{
		const long double clipped = (std::min)(value, static_cast<long double>(extent));
		return int((std::max)(0.0L, std::floor(clipped) - 1.0L));
	}

	/** Computes the reference conservative upper raster bound. */
	int Rgba8ImageDestination::HighPixel(long double value, std::uint32_t extent) noexcept
	{
		const long double clipped = (std::max)(value, 0.0L);
		return int((std::min)(static_cast<long double>(extent - 1), std::ceil(clipped) + 1.0L));
	}

	/** Visits each candidate pixel center, applies top-left ownership, and blends source-over. */
	template <typename ShadeFunction>
	bool Rgba8ImageDestination::RasterizePixels(
		const std::array<RasterVertex, 3>& vertices,
		const std::array<int, 4>& bounds,
		double area,
		bool swapped,
		const std::array<bool, 3>& topLeftEdges,
		PixelBuffer& pixels,
		ShadeFunction&& shade) const noexcept
	{
		for (int pixelY = bounds[1]; pixelY <= bounds[3]; ++pixelY)
		{
			for (int pixelX = bounds[0]; pixelX <= bounds[2]; ++pixelX)
			{
				if (!RasterPixel(vertices, pixelX, pixelY, area, swapped, topLeftEdges, pixels, shade)) return false;
			}
		}
		return true;
	}

	/** Tests one pixel center and shades it directly in the RGBA8 destination. */
	template <typename ShadeFunction>
	bool Rgba8ImageDestination::RasterPixel(
		const std::array<RasterVertex, 3>& vertices,
		int pixelX,
		int pixelY,
		double area,
		bool swapped,
		const std::array<bool, 3>& topLeftEdges,
		PixelBuffer& pixels,
		ShadeFunction&& shade) const noexcept
	{
		const std::size_t pixel = std::size_t(pixelY) * grid_.Width + std::size_t(pixelX);
		const double worldX = GlobalPixel(grid_.CellX, std::size_t(pixelX),
			grid_.WorldOriginX, grid_.Width, grid_.WorldUnitsPerPixel);
		const double worldY = GlobalPixel(grid_.CellY, std::size_t(pixelY),
			grid_.WorldOriginY, grid_.Height, grid_.WorldUnitsPerPixel);
		if (!std::isfinite(worldX) || !std::isfinite(worldY)) return false;
		return ShadeCoveredPixel(vertices, worldX, worldY, pixelX, pixelY, area, swapped, topLeftEdges,
			pixels.data() + pixel * ChannelsPerPixel, shade);
	}

	/** Applies edge ownership, interpolates one covered pixel, and blends directly into its RGBA8 bytes. */
	template <typename ShadeFunction>
	bool Rgba8ImageDestination::ShadeCoveredPixel(
		const std::array<RasterVertex, 3>& vertices,
		double worldX,
		double worldY,
		int pixelX,
		int pixelY,
		double area,
		bool swapped,
		const std::array<bool, 3>& topLeftEdges,
		std::uint8_t* destination,
		ShadeFunction&& shade) const noexcept
	{
		const double firstEdge = Edge(vertices[1], vertices[2], worldX, worldY);
		const double secondEdge = Edge(vertices[2], vertices[0], worldX, worldY);
		const double thirdEdge = Edge(vertices[0], vertices[1], worldX, worldY);
		if (IsOutsideTriangle(firstEdge, secondEdge, thirdEdge, topLeftEdges)) return true;
		LinearPixel color;
		const bool shaded = swapped
			? shade(firstEdge / area, thirdEdge / area, secondEdge / area, pixelX, pixelY, color)
			: shade(firstEdge / area, secondEdge / area, thirdEdge / area, pixelX, pixelY, color);
		if (!shaded) return false;
		CompositePixel(color, destination);
		return true;
	}

	/** Applies strict edge and top-left tests to determine whether a pixel center belongs to a triangle. */
	bool Rgba8ImageDestination::IsOutsideTriangle(
		double firstEdge,
		double secondEdge,
		double thirdEdge,
		const std::array<bool, 3>& topLeftEdges) noexcept
	{
		return firstEdge < 0.0 || (firstEdge == 0.0 && !topLeftEdges[0]) ||
			secondEdge < 0.0 || (secondEdge == 0.0 && !topLeftEdges[1]) ||
			thirdEdge < 0.0 || (thirdEdge == 0.0 && !topLeftEdges[2]);
	}

	/** Converts one world-space solid triangle to zero-UV raster vertices and shades it in linear light. */
	bool Rgba8ImageDestination::RasterSolidTriangle(
		const SolidTriangle& triangle,
		std::uint32_t rgb,
		std::uint8_t opacity) noexcept
	{
		std::array<RasterVertex, 3> vertices{};
		for (std::size_t index = 0; index < vertices.size(); ++index)
		{
			vertices[index] = {triangle.Vertices[index][0], triangle.Vertices[index][1], 0.0, 0.0};
		}
		const LinearPixel color{0.0f, 0.0f, 0.0f, float(opacity) / 255.0f,
			{std::uint8_t((rgb >> 16) & 0xFF), std::uint8_t((rgb >> 8) & 0xFF),
			std::uint8_t(rgb & 0xFF), opacity}, true};
		return RasterTriangle(vertices, pixels_,
			[color](double, double, double, int, int, LinearPixel& output)
			{
				output = color;
				return true;
			});
	}

	/** Applies the batch UV transform and uses the shared single-buffer rasterizer. */
	bool Rgba8ImageDestination::RasterTexturedTriangle(
		const TexturedTriangle& triangle,
		const TexturedTriangleBatch& batch) noexcept
	{
		std::array<RasterVertex, 3> vertices{};
		for (std::size_t index = 0; index < vertices.size(); ++index)
		{
			const auto& source = triangle.Vertices[index];
			vertices[index] = {source.Position[0], source.Position[1],
				source.TextureCoordinates[0] * batch.UvScale[0] + batch.UvOffset[0],
				source.TextureCoordinates[1] * batch.UvScale[1] + batch.UvOffset[1]};
		}
		const TextureShadeContext context{batch};
		return RasterTriangle(vertices, pixels_,
			[&](double first, double second, double third, int, int, LinearPixel& output)
			{
				return ShadeTexture(context, vertices, first, second, third, output);
			});
	}

	/** Maps signed section and local pixel indices to global coordinates with widened arithmetic. */
	double Rgba8ImageDestination::GlobalPixel(
		std::int64_t cell,
		std::size_t pixel,
		double origin,
		std::uint32_t side,
		double worldUnitsPerPixel) noexcept
	{
		const long double index = static_cast<long double>(cell) * side +
			static_cast<long double>(pixel) + 0.5L;
		return static_cast<double>(static_cast<long double>(origin) + index * worldUnitsPerPixel);
	}

	/** Computes the oriented edge value at one world-space pixel center. */
	double Rgba8ImageDestination::Edge(
		const RasterVertex& first,
		const RasterVertex& second,
		double x,
		double y) noexcept
	{
		return (second.X - first.X) * (y - first.Y) - (second.Y - first.Y) * (x - first.X);
	}

	/** Selects inclusive top-left edges for deterministic shared-edge coverage. */
	bool Rgba8ImageDestination::IsTopLeftEdge(const RasterVertex& first, const RasterVertex& second) noexcept
	{
		const double dx = second.X - first.X;
		const double dy = second.Y - first.Y;
		return dy < 0.0 || (dy == 0.0 && dx > 0.0);
	}

	/** Converts normalized linear light to the nearest cached 8-bit sRGB channel. */
	std::uint8_t Rgba8ImageDestination::LinearToSrgb(double channel) noexcept
	{
		static const std::array<std::uint8_t, 4096> lookup = []
		{
			std::array<std::uint8_t, 4096> values{};
			for (std::size_t index = 0; index < values.size(); ++index)
			{
				const double linear = double(index) / double(values.size() - 1);
				const double srgb = linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
				values[index] = std::uint8_t(std::lround((std::clamp)(srgb, 0.0, 1.0) * 255.0));
			}
			return values;
		}();
		const double bounded = (std::clamp)(channel, 0.0, 1.0);
		const std::size_t index = std::size_t(std::lround(bounded * double(lookup.size() - 1)));
		return lookup[index];
	}

	/** Composites one source pixel in linear light and stores straight-alpha sRGB bytes. */
	void Rgba8ImageDestination::CompositePixel(const LinearPixel& source, std::uint8_t* destination) noexcept
	{
		const double sourceAlpha = (std::clamp)(double(source.A), 0.0, 1.0);
		if (sourceAlpha <= 0.0)
		{
			if (destination[3] == 0)
			{
				destination[0] = 0;
				destination[1] = 0;
				destination[2] = 0;
				destination[3] = 0;
			}
			return;
		}
		if (sourceAlpha >= 1.0)
		{
			if (source.PreserveOriginal)
			{
				std::copy(source.Original.begin(), source.Original.end(), destination);
			}
			else
			{
				destination[0] = LinearToSrgb(source.R);
				destination[1] = LinearToSrgb(source.G);
				destination[2] = LinearToSrgb(source.B);
				destination[3] = 255;
			}
			return;
		}
		if (source.PreserveOriginal && destination[3] == 0)
		{
			if (source.Original[3] == 0)
			{
				destination[0] = 0;
				destination[1] = 0;
				destination[2] = 0;
				destination[3] = 0;
			}
			else
			{
				std::copy(source.Original.begin(), source.Original.end(), destination);
			}
			return;
		}
		CompositeOverlappingPixel(source, sourceAlpha, destination);
	}

	/** Computes straight-alpha source-over in linear light for two visible pixels. */
	void Rgba8ImageDestination::CompositeOverlappingPixel(
		const LinearPixel& source,
		double sourceAlpha,
		std::uint8_t* destination) noexcept
	{
		const double destinationAlpha = double(destination[3]) / 255.0;
		const double outputAlpha = sourceAlpha + destinationAlpha * (1.0 - sourceAlpha);
		const double destinationRed = SrgbToLinear(destination[0]);
		const double destinationGreen = SrgbToLinear(destination[1]);
		const double destinationBlue = SrgbToLinear(destination[2]);
		const double destinationWeight = destinationAlpha * (1.0 - sourceAlpha);
		const double sourceRed = source.PreserveOriginal ? SrgbToLinear(source.Original[0]) : source.R;
		const double sourceGreen = source.PreserveOriginal ? SrgbToLinear(source.Original[1]) : source.G;
		const double sourceBlue = source.PreserveOriginal ? SrgbToLinear(source.Original[2]) : source.B;
		const double outputRed = (sourceRed * sourceAlpha + destinationRed * destinationWeight) / outputAlpha;
		const double outputGreen = (sourceGreen * sourceAlpha + destinationGreen * destinationWeight) / outputAlpha;
		const double outputBlue = (sourceBlue * sourceAlpha + destinationBlue * destinationWeight) / outputAlpha;
		const auto alphaByte = std::uint8_t(std::lround(outputAlpha * 255.0));
		if (alphaByte == 0)
		{
			destination[0] = 0;
			destination[1] = 0;
			destination[2] = 0;
			destination[3] = 0;
			return;
		}
		destination[0] = LinearToSrgb(outputRed);
		destination[1] = LinearToSrgb(outputGreen);
		destination[2] = LinearToSrgb(outputBlue);
		destination[3] = alphaByte;
	}

}

namespace Services::Map::Buildings
{
	/** Constructs the selected RGBA8 numerical implementation behind the neutral interface. */
	std::unique_ptr<IImageDestination> IImageDestination::Create(
		const ImageGrid& grid,
		bool initializeImmediately) noexcept
	{
		return std::unique_ptr<IImageDestination>(
			new (std::nothrow) Rendering::Rgba8ImageDestination(grid, initializeImmediately));
	}
}

#endif // MODLOADER_CLIENT_BUILD
