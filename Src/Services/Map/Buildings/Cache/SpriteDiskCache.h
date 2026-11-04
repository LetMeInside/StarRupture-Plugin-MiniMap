// Independently loads and transactionally persists compatible version-1 sprite files.
#pragma once

#include "OwnedSpriteKey.h"
#include "ProcessedSpritePayload.h"

#include <windows.h>

#include <filesystem>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <system_error>
#include <vector>
#include <string>

namespace Services::Map::Buildings::Cache
{
	/** Owns configured cache-root access and performs independent compatible sprite file operations. */
	class SpriteDiskCache final
	{
	public:
		/** Outcomes for individual sprite file loads. */
		enum class LoadStatus { Loaded, Missing, Invalid, IOError, Unavailable };
		/** Outcomes for completed persistence, distinct from successful processing. */
		enum class WriteStatus { Persisted, Invalid, IOError, Unavailable };
		/** Returns one validated payload or a distinct file-operation outcome. */
		struct LoadResult
		{
			LoadStatus Status = LoadStatus::Invalid;
			std::shared_ptr<const ProcessedSpritePayload> Payload;
			/** Native Win32 error returned by file operations, when applicable. */
			DWORD Win32Error = ERROR_SUCCESS;
			/** Original HRESULT returned by the WIC codec, when applicable. */
			HRESULT CodecError = S_OK;
			/** Portable filesystem error retained from a standard-library exception. */
			std::error_code FileSystemError;
		};

		/** Uses the explicitly configured Buildings sprite directory.
		 * @param buildingsRoot Absolute configured MiniMap/Cache/Buildings directory.
		 */
		SpriteDiskCache(std::filesystem::path buildingsRoot);

		/** Loads one owned canonical sprite key.
		 * @param key Owned bytes identifying one sprite file.
		 * @return Decoded payload or distinct file/codec outcome.
		 */
		LoadResult TryLoad(const OwnedSpriteKey& key) const noexcept;

		/** Encodes completely and atomically replaces one key's sibling .tmp file on success.
		 * @param key Owned bytes identifying the destination sprite file.
		 * @param payload Processed immutable pixels and original capture geometry to persist.
		 * @return Persisted only after the complete temporary file was closed and published.
		 */
		WriteStatus TryWrite(const OwnedSpriteKey& key, const ProcessedSpritePayload& payload) noexcept;

	private:
		/** Holds validated version-1 metadata decoded from the packed 84-byte header. */
		struct SpriteHeaderFields
		{
			std::uint32_t Width = 0;
			std::uint32_t Height = 0;
			std::uint32_t PngBytes = 0;
			Visuals::SpriteCaptureBounds Bounds{};
			std::uint16_t SourceX = 0;
			std::uint16_t SourceY = 0;
			double SourceStep = 0;
		};

		/** Resolves only the already-owned, safely encoded compatible filename. */
		bool TryGetPath(const OwnedSpriteKey&, std::filesystem::path&) const;
		/** Reads one file handle, validates its header/length, then decodes its PNG payload. */
		LoadResult LoadFile(const std::filesystem::path&) const;
		/** Reads the sprite header and checks exact file and declared PNG lengths. */
		LoadResult ReadAndDecodeFile(HANDLE, std::uint64_t) const;
		/** Reads declared PNG bytes, decodes them, and adopts their pixel allocation. */
		LoadResult ReadPngAndBuildPayload(HANDLE, const SpriteHeaderFields&, std::uint32_t) const;
		/** Maps the file-open error to missing or I/O status. */
		LoadResult ClassifyOpenFailure(DWORD) const noexcept;
		/** Decodes exact 84-byte little-endian version-1 fields. */
		bool DecodeHeader(const std::uint8_t*, SpriteHeaderFields&) const noexcept;
		/** Validates dimensions, bounds and projection before any pixel allocation. */
		bool ValidateHeader(const SpriteHeaderFields&) const noexcept;
		/** Validates bounds and positive capture-space XY extents. */
		bool ValidateSpatialBounds(const SpriteHeaderFields&) const noexcept;
		/** Checks last-sample starts and ceil-rounded spatial edges. */
		bool ValidateSamplingFootprint(const SpriteHeaderFields&) const noexcept;
		/** Checks that payload mapping reduces exactly to the v1 source origin/step fields. */
		bool IsPayloadMappable(const ProcessedSpritePayload&) const noexcept;
		/** Reuses file-format validation for output mapping and capture bounds. */
		bool ValidatePayloadSpatial(const ProcessedSpritePayload&, std::uint16_t, std::uint16_t, double) const noexcept;
		/** Serializes validated metadata to the exact packed 84-byte legacy representation. */
		bool EncodeHeader(const ProcessedSpritePayload&, std::uint32_t, std::uint8_t*) const noexcept;
		/** Reconstructs original-coordinate projection metadata from the version-1 header. */
		bool BuildPayload(const SpriteHeaderFields&, std::vector<std::uint8_t>&&,
			std::shared_ptr<const ProcessedSpritePayload>&) const;
		/** Joins encoded PNG and header bytes before any persistent file is opened. */
		WriteStatus SerializeAndPersist(const std::filesystem::path&, const ProcessedSpritePayload&,
			const std::vector<std::uint8_t>&);
		/** Writes header and PNG bytes to a sibling temporary and atomically publishes it. */
		WriteStatus PersistFile(const std::filesystem::path&, const std::uint8_t*, std::span<const std::uint8_t>);
		/** Checks complete WriteFile length and successful temporary handle closure. */
		bool WriteAndClose(HANDLE, const std::uint8_t*, std::span<const std::uint8_t>) noexcept;
		/** Removes a temporary path created by this cache operation. */
		void CleanupTemporary(const std::filesystem::path&) noexcept;
		/** Encodes a 16-bit integer in little-endian order. */
		static void WriteUInt16(std::uint8_t*, std::uint16_t) noexcept;
		/** Encodes a 32-bit integer into its exact little-endian field representation. */
		static void WriteUInt32(std::uint8_t*, std::uint32_t) noexcept;
		/** Decodes a 32-bit little-endian header field. */
		static std::uint32_t ReadUInt32(const std::uint8_t*) noexcept;
		/** Encodes an IEEE 754 double in the existing little-endian field representation. */
		static void WriteDouble(std::uint8_t*, double) noexcept;
		/** Decodes an IEEE 754 double from the existing little-endian field representation. */
		static double ReadDouble(const std::uint8_t*) noexcept;

		std::filesystem::path m_buildingsRoot;
		mutable std::mutex m_operationMutex;
	};
}
