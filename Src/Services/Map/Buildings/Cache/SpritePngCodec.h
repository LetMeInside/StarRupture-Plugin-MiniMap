// Encodes and decodes the WIC PNG representation used by compatible sprite files.
#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

namespace Services::Map::Buildings::Cache
{
	/** Provides independent WIC conversion between PNG streams and straight RGBA8 pixels. */
	class SpritePngCodec final
	{
	public:
		/** Separates malformed PNG input from an unavailable WIC/COM operation. */
		enum class ResultStatus { Complete, Invalid, Unavailable };
		/** Returns a codec outcome with the original HRESULT when one is available. */
		struct Result
		{
			ResultStatus Status = ResultStatus::Invalid;
			HRESULT Error = S_OK;
		};

		/** Decodes one PNG byte sequence to exact-size straight RGBA8 pixels. */
		static Result Decode(
			std::span<const std::uint8_t> png,
			std::uint32_t expectedWidth,
			std::uint32_t expectedHeight,
			std::vector<std::uint8_t>& rgba);

		/** Encodes straight RGBA8 pixels as a PNG without changing the caller's allocation. */
		static Result Encode(
			std::span<const std::uint8_t> rgba,
			std::uint32_t width,
			std::uint32_t height,
			std::vector<std::uint8_t>& png);

	private:
		/** Performs WIC decode after input bounds and COM initialization are validated. */
		static Result DecodeWithWic(std::span<const std::uint8_t>, std::uint32_t, std::uint32_t,
			std::vector<std::uint8_t>&);
		/** Creates a WIC imaging factory for a codec operation. */
		static HRESULT CreateFactory(Microsoft::WRL::ComPtr<IWICImagingFactory>&);
		/** Configures frame conversion to exact straight 32-bit RGBA output. */
		static HRESULT InitializeRgbaConverter(Microsoft::WRL::ComPtr<IWICFormatConverter>&,
			Microsoft::WRL::ComPtr<IWICBitmapFrameDecode>&);
		/** Copies validated decoded pixels into output and preserves it on failure. */
		static bool CopyDecodedPixels(Microsoft::WRL::ComPtr<IWICFormatConverter>&, UINT, UINT,
			std::vector<std::uint8_t>&);
		/** Performs WIC PNG encoding after input bounds and COM initialization are validated. */
		static Result EncodeWithWic(std::span<const std::uint8_t>, std::uint32_t, std::uint32_t,
			std::vector<std::uint8_t>&);
		/** Converts RGBA to WIC's accepted BGRA frame format and commits the PNG encoder. */
		static bool WriteRgbaFrame(Microsoft::WRL::ComPtr<IWICBitmapFrameEncode>&,
			Microsoft::WRL::ComPtr<IWICBitmapEncoder>&, std::span<const std::uint8_t>, std::uint32_t, std::uint32_t);
		/** Extracts the complete committed in-memory PNG stream. */
		static bool ExtractPngStream(Microsoft::WRL::ComPtr<IStream>&, std::vector<std::uint8_t>&);
	};
}
