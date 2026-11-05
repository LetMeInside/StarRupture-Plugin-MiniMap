#if defined(MODLOADER_CLIENT_BUILD)

// Implements independent v1 sprite validation, WIC payload decoding and atomic persistence.
#include "SpriteDiskCache.h"

#include "SpritePngCodec.h"

#include <windows.h>

#include <bit>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <system_error>

namespace Services::Map::Buildings::Cache
{
	namespace
	{
		static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
		constexpr std::size_t kHeaderBytes = 84;
		constexpr std::size_t kMaximumPngBytes = 128u * 1024u * 1024u;
		constexpr std::size_t kMaximumRgbaBytes = 64u * 1024u * 1024u;
		constexpr std::array<std::uint8_t, 8> kSpriteMagic{'M', 'M', 'B', 'S', 'P', 'R', 'T', '1'};
	}

	/** Retains the explicitly configured directory for individual sprite files. */
SpriteDiskCache::SpriteDiskCache(std::filesystem::path buildingsRoot)
		: m_buildingsRoot(std::move(buildingsRoot))
	{
	}

	/** Loads one key without an existence precheck and preserves missing/invalid/I/O outcomes. */
	SpriteDiskCache::LoadResult SpriteDiskCache::TryLoad(const OwnedSpriteKey& key) const noexcept
	{
		try
		{
			std::filesystem::path path;
			if (!TryGetPath(key, path)) return {LoadStatus::Invalid, {}, 0};
			return LoadFile(path);
		}
		catch (const std::bad_alloc&)
		{
			return {LoadStatus::Unavailable, {}, ERROR_NOT_ENOUGH_MEMORY};
		}
		catch (const std::filesystem::filesystem_error& error)
		{
			return {LoadStatus::IOError, {}, ERROR_SUCCESS, S_OK, error.code()};
		}
		catch (...)
		{
			return {LoadStatus::IOError, {}, ERROR_UNHANDLED_EXCEPTION};
		}
	}

	/** Completes PNG encoding and serialization before creating the operation-owned sibling temp file. */
	SpriteDiskCache::WriteStatus SpriteDiskCache::TryWrite(
		const OwnedSpriteKey& key,
		const ProcessedSpritePayload& payload) noexcept
	{
		try
		{
			std::filesystem::path path;
			if (!TryGetPath(key, path) || !IsPayloadMappable(payload)) return WriteStatus::Invalid;
			std::vector<std::uint8_t> png;
			const auto encoded = SpritePngCodec::Encode(payload.Pixels(), payload.Width(), payload.Height(), png);
			if (encoded.Status == SpritePngCodec::ResultStatus::Unavailable) return WriteStatus::Unavailable;
			if (encoded.Status != SpritePngCodec::ResultStatus::Complete ||
				png.empty() || png.size() > kMaximumPngBytes) return WriteStatus::Invalid;
			return SerializeAndPersist(path, payload, png);
		}
		catch (const std::bad_alloc&)
		{
			return WriteStatus::Unavailable;
		}
		catch (...)
		{
			return WriteStatus::IOError;
		}
	}

	/** Resolves the encoded basename below the caller-supplied Buildings cache root. */
	bool SpriteDiskCache::TryGetPath(const OwnedSpriteKey& key, std::filesystem::path& path) const
	{
		std::string fileName;
		if (!m_buildingsRoot.is_absolute() || !key.TryGetFileName(fileName)) return false;
		path = m_buildingsRoot / std::filesystem::path(std::wstring(fileName.begin(), fileName.end()));
		return true;
	}

	/** Opens one sprite file, validates exact length and spatial mapping, then decodes its PNG. */
	SpriteDiskCache::LoadResult SpriteDiskCache::LoadFile(const std::filesystem::path& path) const
	{
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return ClassifyOpenFailure(GetLastError());
		std::unique_ptr<void, decltype(&CloseHandle)> owner(file, &CloseHandle);
		LARGE_INTEGER length{};
		if (!GetFileSizeEx(file, &length)) return {LoadStatus::IOError, {}, GetLastError()};
		if (length.QuadPart < LONGLONG(kHeaderBytes) ||
			std::uint64_t(length.QuadPart) > kHeaderBytes + kMaximumPngBytes)
		{
			return {LoadStatus::Invalid, {}, 0};
		}
		return ReadAndDecodeFile(file, std::uint64_t(length.QuadPart));
	}

	/** Reads and checks header and bounded PNG bytes from the already-open file handle. */
	SpriteDiskCache::LoadResult SpriteDiskCache::ReadAndDecodeFile(HANDLE file, std::uint64_t fileBytes) const
	{
		std::array<std::uint8_t, kHeaderBytes> headerBytes{};
		DWORD read = 0;
		if (!ReadFile(file, headerBytes.data(), DWORD(headerBytes.size()), &read, nullptr))
		{
			return {LoadStatus::IOError, {}, GetLastError()};
		}
		SpriteHeaderFields header;
		if (read != headerBytes.size() || !DecodeHeader(headerBytes.data(), header) || !ValidateHeader(header) ||
			fileBytes != kHeaderBytes + std::uint64_t(header.PngBytes))
		{
			return {LoadStatus::Invalid, {}, 0};
		}
		return ReadPngAndBuildPayload(file, header, header.PngBytes);
	}

	/** Reads the exact declared PNG length, decodes it, and transfers pixels into immutable ownership. */
	SpriteDiskCache::LoadResult SpriteDiskCache::ReadPngAndBuildPayload(
		HANDLE file, const SpriteHeaderFields& header, std::uint32_t pngBytes) const
	{
		std::vector<std::uint8_t> png(pngBytes);
		DWORD read = 0;
		if (!ReadFile(file, png.data(), pngBytes, &read, nullptr)) return {LoadStatus::IOError, {}, GetLastError()};
		if (read != pngBytes) return {LoadStatus::Invalid, {}, 0};
		std::vector<std::uint8_t> rgba;
		const auto decoded = SpritePngCodec::Decode(png, header.Width, header.Height, rgba);
		if (decoded.Status == SpritePngCodec::ResultStatus::Unavailable)
		{
			return {LoadStatus::Unavailable, {}, ERROR_SUCCESS, decoded.Error};
		}
		if (decoded.Status != SpritePngCodec::ResultStatus::Complete)
		{
			return {LoadStatus::Invalid, {}, ERROR_SUCCESS, decoded.Error};
		}
		std::unique_ptr<const ProcessedSpritePayload> payload;
		if (!BuildPayload(header, std::move(rgba), payload)) return {LoadStatus::Invalid, {}, 0};
		return {LoadStatus::Loaded, std::move(payload), 0};
	}

	/** Classifies open errors so absence remains distinct from I/O failure. */
	SpriteDiskCache::LoadResult SpriteDiskCache::ClassifyOpenFailure(DWORD error) const noexcept
	{
		const bool missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
		return {missing ? LoadStatus::Missing : LoadStatus::IOError, {}, error};
	}

	/** Decodes exact packed v1 fields from explicit little-endian byte offsets. */
	bool SpriteDiskCache::DecodeHeader(const std::uint8_t* bytes, SpriteHeaderFields& header) const noexcept
	{
		if (!std::equal(kSpriteMagic.begin(), kSpriteMagic.end(), bytes) || ReadUInt32(bytes + 8) != 1) return false;
		header.Width = ReadUInt32(bytes + 12);
		header.Height = ReadUInt32(bytes + 16);
		header.PngBytes = ReadUInt32(bytes + 20);
		for (std::size_t axis = 0; axis < 3; ++axis)
		{
			header.Bounds.Minimum[axis] = ReadDouble(bytes + 24 + axis * 8);
			header.Bounds.Maximum[axis] = ReadDouble(bytes + 48 + axis * 8);
		}
		header.SourceX = std::uint16_t(bytes[72]) | (std::uint16_t(bytes[73]) << 8);
		header.SourceY = std::uint16_t(bytes[74]) | (std::uint16_t(bytes[75]) << 8);
		header.SourceStep = ReadDouble(bytes + 76);
		return header.Width && header.Height && header.PngBytes;
	}

	/** Applies legacy dimension, allocation, bounds, source-coordinate and sampling-footprint validation. */
	bool SpriteDiskCache::ValidateHeader(const SpriteHeaderFields& header) const noexcept
	{
		if (header.Width > 256 || header.Height > 256 ||
			std::uint64_t(header.Width) * header.Height * 4 > kMaximumRgbaBytes ||
			header.PngBytes > kMaximumPngBytes || header.SourceX >= 256 || header.SourceY >= 256 ||
			!std::isfinite(header.SourceStep) || header.SourceStep < 1.0)
		{
			return false;
		}
		return ValidateSpatialBounds(header) && ValidateSamplingFootprint(header);
	}

	/** Validates finite ordered capture bounds and the positive projected footprint. */
	bool SpriteDiskCache::ValidateSpatialBounds(const SpriteHeaderFields& header) const noexcept
	{
		for (std::size_t axis = 0; axis < 3; ++axis)
		{
			if (!std::isfinite(header.Bounds.Minimum[axis]) || !std::isfinite(header.Bounds.Maximum[axis]) ||
				header.Bounds.Maximum[axis] < header.Bounds.Minimum[axis]) return false;
		}
		if (!(header.Bounds.Maximum[0] > header.Bounds.Minimum[0] &&
			header.Bounds.Maximum[1] > header.Bounds.Minimum[1])) return false;
		const double extentX = header.Bounds.Maximum[0] * 0.5 - header.Bounds.Minimum[0] * 0.5;
		const double extentY = header.Bounds.Maximum[1] * 0.5 - header.Bounds.Minimum[1] * 0.5;
		const double orthographicWidth = double(float(2.0 * (std::max)(extentX, extentY) * 1.1));
		return std::isfinite(orthographicWidth) && orthographicWidth > 0 &&
			std::isfinite((orthographicWidth / 256.0) * header.SourceStep);
	}

	/** Preserves ceil-rounded spatial margins while requiring the last texel start inside 256. */
	bool SpriteDiskCache::ValidateSamplingFootprint(const SpriteHeaderFields& header) const noexcept
	{
		const double lastX = header.SourceX + (header.Width - 1) * header.SourceStep;
		const double lastY = header.SourceY + (header.Height - 1) * header.SourceStep;
		const double edgeX = header.SourceX + header.Width * header.SourceStep;
		const double edgeY = header.SourceY + header.Height * header.SourceStep;
		return lastX < 256 && lastY < 256 && std::isfinite(edgeX) && std::isfinite(edgeY);
	}

	/** Reconstructs original capture dimensions and the original-coordinate projection mapping. */
	bool SpriteDiskCache::BuildPayload(
		const SpriteHeaderFields& header,
		std::vector<std::uint8_t>&& rgba,
		std::unique_ptr<const ProcessedSpritePayload>& output) const
	{
		const Visuals::SpriteProjectionRectangle projection{
			double(header.SourceX), double(header.SourceY),
			header.SourceX + header.Width * header.SourceStep,
			header.SourceY + header.Height * header.SourceStep};
		return ProcessedSpritePayload::TryAdopt(std::move(rgba), header.Width, header.Height,
			{256, 256}, header.Bounds, projection, output);
	}

	/** Verifies payload projection represents the single-step mapping the v1 header can store. */
	bool SpriteDiskCache::IsPayloadMappable(const ProcessedSpritePayload& payload) const noexcept
	{
		const auto& projection = payload.Projection();
		const double stepX = (projection.Right - projection.Left) / payload.Width();
		const double stepY = (projection.Bottom - projection.Top) / payload.Height();
		const double stepTolerance = (std::max)(std::abs(stepX), std::abs(stepY)) *
			(std::numeric_limits<double>::epsilon)() * 8.0;
		return payload.CaptureSize().Width == 256 && payload.CaptureSize().Height == 256 &&
			projection.Left >= 0 && projection.Top >= 0 && projection.Left < 256 && projection.Top < 256 &&
			projection.Left <= 65535 && projection.Top <= 65535 &&
			std::floor(projection.Left) == projection.Left && std::floor(projection.Top) == projection.Top &&
			std::isfinite(stepX) && stepX >= 1 && std::abs(stepX - stepY) <= stepTolerance &&
			ValidatePayloadSpatial(payload, std::uint16_t(projection.Left), std::uint16_t(projection.Top), stepX);
	}

	/** Revalidates serialized bounds and source mapping against version-1 file constraints. */
	bool SpriteDiskCache::ValidatePayloadSpatial(
		const ProcessedSpritePayload& payload,
		std::uint16_t sourceX,
		std::uint16_t sourceY,
		double sourceStep) const noexcept
	{
		SpriteHeaderFields header;
		header.Width = payload.Width();
		header.Height = payload.Height();
		header.Bounds = payload.Bounds();
		header.SourceX = sourceX;
		header.SourceY = sourceY;
		header.SourceStep = sourceStep;
		header.PngBytes = 1;
		return ValidateHeader(header);
	}

	/** Encodes valid payload metadata and PNG bytes into a packed version-1 header and body. */
	SpriteDiskCache::WriteStatus SpriteDiskCache::SerializeAndPersist(
		const std::filesystem::path& path,
		const ProcessedSpritePayload& payload,
		const std::vector<std::uint8_t>& png)
	{
		std::array<std::uint8_t, kHeaderBytes> header{};
		if (!EncodeHeader(payload, std::uint32_t(png.size()), header.data())) return WriteStatus::Invalid;
		return PersistFile(path, header.data(), png);
	}

	/** Encodes compatible magic, widths, bounds and v1 projection fields into exactly 84 bytes. */
	bool SpriteDiskCache::EncodeHeader(
		const ProcessedSpritePayload& payload,
		std::uint32_t pngBytes,
		std::uint8_t* bytes) const noexcept
	{
		if (!IsPayloadMappable(payload) || !pngBytes || pngBytes > kMaximumPngBytes) return false;
		std::copy(kSpriteMagic.begin(), kSpriteMagic.end(), bytes);
		WriteUInt32(bytes + 8, 1);
		WriteUInt32(bytes + 12, payload.Width());
		WriteUInt32(bytes + 16, payload.Height());
		WriteUInt32(bytes + 20, pngBytes);
		for (std::size_t axis = 0; axis < 3; ++axis)
		{
			WriteDouble(bytes + 24 + axis * 8, payload.Bounds().Minimum[axis]);
			WriteDouble(bytes + 48 + axis * 8, payload.Bounds().Maximum[axis]);
		}
		WriteUInt16(bytes + 72, std::uint16_t(payload.Projection().Left));
		WriteUInt16(bytes + 74, std::uint16_t(payload.Projection().Top));
		WriteDouble(bytes + 76, (payload.Projection().Right - payload.Projection().Left) / payload.Width());
		return true;
	}

	/** Creates the configured cache directory, writes and closes a sibling temporary, then atomically replaces. */
	SpriteDiskCache::WriteStatus SpriteDiskCache::PersistFile(
		const std::filesystem::path& path,
		const std::uint8_t* header,
		std::span<const std::uint8_t> png)
	{
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		if (error) return WriteStatus::IOError;
		const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
		const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return WriteStatus::IOError;
		if (!WriteAndClose(file, header, png))
		{
			CleanupTemporary(temporary);
			return WriteStatus::IOError;
		}
		if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			return WriteStatus::Persisted;
		}
		CleanupTemporary(temporary);
		return WriteStatus::IOError;
	}

	/** Requires a complete write and successful handle close before publication is attempted. */
	bool SpriteDiskCache::WriteAndClose(
		HANDLE file,
		const std::uint8_t* header,
		std::span<const std::uint8_t> png) noexcept
	{
		DWORD written = 0;
		const bool headerWritten = WriteFile(file, header, DWORD(kHeaderBytes), &written, nullptr) && written == kHeaderBytes;
		written = 0;
		const bool pngWritten = headerWritten && png.size() <= (std::numeric_limits<DWORD>::max)() &&
			WriteFile(file, png.data(), DWORD(png.size()), &written, nullptr) && written == png.size();
		const bool closed = CloseHandle(file) != FALSE;
		return headerWritten && pngWritten && closed;
	}

	/** Removes only a sibling temp file created by this operation. */
	void SpriteDiskCache::CleanupTemporary(const std::filesystem::path& temporary) noexcept
	{
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
	}

	/** Encodes an unsigned integer in little-endian order. */
	void SpriteDiskCache::WriteUInt16(std::uint8_t* bytes, std::uint16_t value) noexcept
	{
		bytes[0] = std::uint8_t(value);
		bytes[1] = std::uint8_t(value >> 8);
	}

	/** Encodes an unsigned integer in little-endian order. */
	void SpriteDiskCache::WriteUInt32(std::uint8_t* bytes, std::uint32_t value) noexcept
	{
		for (unsigned index = 0; index < 4; ++index) bytes[index] = std::uint8_t(value >> (index * 8));
	}

	/** Decodes a 32-bit little-endian header field. */
	std::uint32_t SpriteDiskCache::ReadUInt32(const std::uint8_t* bytes) noexcept
	{
		return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
			(std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
	}

	/** Writes IEEE 754 binary64 bits in explicit little-endian order. */
	void SpriteDiskCache::WriteDouble(std::uint8_t* bytes, double value) noexcept
	{
		const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
		for (unsigned index = 0; index < 8; ++index) bytes[index] = std::uint8_t(bits >> (index * 8));
	}

	/** Reads IEEE 754 binary64 bits from explicit little-endian bytes. */
	double SpriteDiskCache::ReadDouble(const std::uint8_t* bytes) noexcept
	{
		std::uint64_t bits = 0;
		for (unsigned index = 0; index < 8; ++index) bits |= std::uint64_t(bytes[index]) << (index * 8);
		return std::bit_cast<double>(bits);
	}
}

#endif // MODLOADER_CLIENT_BUILD
