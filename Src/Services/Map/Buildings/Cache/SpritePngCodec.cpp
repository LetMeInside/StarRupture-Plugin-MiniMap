#if defined(MODLOADER_CLIENT_BUILD)

// Implements the independent WIC PNG codec for compatible sprite cache payloads.
#include "SpritePngCodec.h"
#include "SpritePngCodecApartment.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <limits>
#include <utility>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace Services::Map::Buildings::Cache
{
	/** Decodes one PNG to RGBA8 and leaves the output unchanged on failure. */
	SpritePngCodec::Result SpritePngCodec::Decode(
		std::span<const std::uint8_t> png,
		std::uint32_t expectedWidth,
		std::uint32_t expectedHeight,
		std::vector<std::uint8_t>& rgba)
	{
		if (!expectedWidth || !expectedHeight || expectedWidth > 4096 || expectedHeight > 4096 ||
			png.empty() || png.size() > 128u * 1024u * 1024u || png.size() > (std::numeric_limits<DWORD>::max)())
		{
			return {ResultStatus::Invalid, E_INVALIDARG};
		}
		SpritePngCodecApartment apartment;
		if (!apartment.IsAvailable()) return {ResultStatus::Unavailable, apartment.InitializationError()};
		return DecodeWithWic(png, expectedWidth, expectedHeight, rgba);
	}

	/** Encodes RGBA8 pixels to PNG without copying or mutating the source allocation. */
	SpritePngCodec::Result SpritePngCodec::Encode(
		std::span<const std::uint8_t> rgba,
		std::uint32_t width,
		std::uint32_t height,
		std::vector<std::uint8_t>& png)
	{
		if (!width || !height || width > 4096 || height > 4096 ||
			std::uint64_t(width) * height * 4 > 64u * 1024u * 1024u ||
			rgba.size() != std::size_t(width) * height * 4)
		{
			return {ResultStatus::Invalid, E_INVALIDARG};
		}
		SpritePngCodecApartment apartment;
		if (!apartment.IsAvailable()) return {ResultStatus::Unavailable, apartment.InitializationError()};
		return EncodeWithWic(rgba, width, height, png);
	}

	/** Creates WIC decoder objects and validates PNG frame dimensions and output size. */
	SpritePngCodec::Result SpritePngCodec::DecodeWithWic(
		std::span<const std::uint8_t> png,
		std::uint32_t expectedWidth,
		std::uint32_t expectedHeight,
		std::vector<std::uint8_t>& rgba)
	{
		using Microsoft::WRL::ComPtr;
		ComPtr<IWICImagingFactory> factory;
		ComPtr<IWICStream> stream;
		ComPtr<IWICBitmapDecoder> decoder;
		ComPtr<IWICBitmapFrameDecode> frame;
		ComPtr<IWICFormatConverter> converter;
		HRESULT result = CreateFactory(factory);
		if (FAILED(result)) return {ResultStatus::Unavailable, result};
		result = factory->CreateStream(&stream);
		if (FAILED(result)) return {ResultStatus::Unavailable, result};
		result = stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), DWORD(png.size()));
		if (FAILED(result)) return {ResultStatus::Unavailable, result};
		result = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
		if (FAILED(result)) return {ResultStatus::Invalid, result};
		result = decoder->GetFrame(0, &frame);
		if (FAILED(result)) return {ResultStatus::Invalid, result};
		UINT width = 0;
		UINT height = 0;
		result = frame->GetSize(&width, &height);
		if (FAILED(result) || width != expectedWidth || height != expectedHeight)
		{
			return {ResultStatus::Invalid, result};
		}
		result = factory->CreateFormatConverter(&converter);
		if (FAILED(result)) return {ResultStatus::Unavailable, result};
		result = InitializeRgbaConverter(converter, frame);
		if (FAILED(result)) return {ResultStatus::Invalid, result};
		return CopyDecodedPixels(converter, width, height, rgba)
			? Result{ResultStatus::Complete, S_OK} : Result{ResultStatus::Invalid, E_FAIL};
	}

	/** Creates the WIC factory required by both independent codec operations. */
	HRESULT SpritePngCodec::CreateFactory(Microsoft::WRL::ComPtr<IWICImagingFactory>& factory)
	{
		return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	}

	/** Configures WIC conversion to the exact straight 32-bit RGBA format. */
	HRESULT SpritePngCodec::InitializeRgbaConverter(
		Microsoft::WRL::ComPtr<IWICFormatConverter>& converter,
		Microsoft::WRL::ComPtr<IWICBitmapFrameDecode>& frame)
	{
		return converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
			WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
	}

	/** Copies decoded rows only after the checked dimensions bound the destination allocation. */
	bool SpritePngCodec::CopyDecodedPixels(
		Microsoft::WRL::ComPtr<IWICFormatConverter>& converter,
		UINT width,
		UINT height,
		std::vector<std::uint8_t>& rgba)
	{
		const std::size_t bytes = std::size_t(width) * height * 4;
		if (bytes > 64u * 1024u * 1024u || bytes > (std::numeric_limits<UINT>::max)()) return false;
		std::vector<std::uint8_t> decoded(bytes);
		if (FAILED(converter->CopyPixels(nullptr, width * 4, UINT(bytes), decoded.data()))) return false;
		rgba.swap(decoded);
		return true;
	}

	/** Encodes the bounded image into an owned HGLOBAL stream and extracts its PNG bytes. */
	SpritePngCodec::Result SpritePngCodec::EncodeWithWic(
		std::span<const std::uint8_t> rgba,
		std::uint32_t width,
		std::uint32_t height,
		std::vector<std::uint8_t>& png)
	{
		using Microsoft::WRL::ComPtr;
		ComPtr<IWICImagingFactory> factory;
		ComPtr<IStream> stream;
		ComPtr<IWICBitmapEncoder> encoder;
		ComPtr<IWICBitmapFrameEncode> frame;
		HRESULT result = CreateFactory(factory);
		if (FAILED(result)) return {ResultStatus::Unavailable, result};
		result = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
		if (FAILED(result)) return {ResultStatus::Unavailable, result};
		result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
		if (FAILED(result)) return {ResultStatus::Unavailable, result};
		result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
		if (FAILED(result)) return {ResultStatus::Unavailable, result};
		result = encoder->CreateNewFrame(&frame, nullptr);
		if (FAILED(result)) return {ResultStatus::Unavailable, result};
		result = frame->Initialize(nullptr);
		if (FAILED(result)) return {ResultStatus::Unavailable, result};
		result = frame->SetSize(width, height);
		if (FAILED(result)) return {ResultStatus::Unavailable, result};
		if (!WriteRgbaFrame(frame, encoder, rgba, width, height)) return {ResultStatus::Unavailable, E_FAIL};
		return ExtractPngStream(stream, png)
			? Result{ResultStatus::Complete, S_OK} : Result{ResultStatus::Unavailable, E_FAIL};
	}

	/** Converts only the bounded encoder input to BGRA, matching the legacy WIC pixel format. */
	bool SpritePngCodec::WriteRgbaFrame(
		Microsoft::WRL::ComPtr<IWICBitmapFrameEncode>& frame,
		Microsoft::WRL::ComPtr<IWICBitmapEncoder>& encoder,
		std::span<const std::uint8_t> rgba,
		std::uint32_t width,
		std::uint32_t height)
	{
		std::vector<std::uint8_t> bgra(rgba.begin(), rgba.end());
		for (std::size_t offset = 0; offset < bgra.size(); offset += 4) std::swap(bgra[offset], bgra[offset + 2]);
		WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
		return SUCCEEDED(frame->SetPixelFormat(&pixelFormat)) &&
			IsEqualGUID(pixelFormat, GUID_WICPixelFormat32bppBGRA) &&
			SUCCEEDED(frame->WritePixels(height, width * 4, UINT(bgra.size()), bgra.data())) &&
			SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
	}

	/** Rewinds the committed PNG stream and publishes its bytes after strict size checks. */
	bool SpritePngCodec::ExtractPngStream(
		Microsoft::WRL::ComPtr<IStream>& stream,
		std::vector<std::uint8_t>& png)
	{
		STATSTG info{};
		LARGE_INTEGER start{};
		if (FAILED(stream->Stat(&info, STATFLAG_NONAME)) || !info.cbSize.QuadPart ||
			info.cbSize.QuadPart > 128u * 1024u * 1024u ||
			FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) return false;
		std::vector<std::uint8_t> encoded(std::size_t(info.cbSize.QuadPart));
		ULONG read = 0;
		if (FAILED(stream->Read(encoded.data(), ULONG(encoded.size()), &read)) || read != encoded.size()) return false;
		png.swap(encoded);
		return true;
	}
}

#endif // MODLOADER_CLIENT_BUILD
