// Owns stable registered terrain metadata and optionally shares immutable CPU pixels.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace MiniMapTerrainSource
{
    /** Immutable pixel storage shared by selected descriptors and in-flight work. */
    struct CpuTerrainImagePixels
    {
        /** Takes ownership of bytes moved from the caller; no input buffer is borrowed. */
        explicit CpuTerrainImagePixels(std::vector<uint8_t>&& bytes)
            : Bytes(std::move(bytes))
        {
        }

        const std::vector<uint8_t> Bytes;
    };

    /** Registered pixel coverage and affine world mapping for one terrain image. */
    struct RegisteredTerrainImage
    {
        /** Native XY centimetres, or normalized UV when returned by WorldToImage. */
        struct Point
        {
            double X = 0.0;
            double Y = 0.0;
            bool operator==(const Point&) const = default;
            /** Reports whether both coordinates are finite. */
            bool IsFinite() const { return std::isfinite(X) && std::isfinite(Y); }
        };

        /** Specifies whether pixels are re-resolved from the game or owned on the CPU. */
        enum class SourceType { GameNativeBC1, CpuOwnedPixels };
        /** Identifies byte interpretation independently from game-engine enum values. */
        enum class Format { BC1, RGBA8, BGRA8 };

        /** Re-resolves a native brush through game-owned terrain data. */
        struct GameTileReference
        {
            Point SampleWorld;
            int GridX = 0;
            int GridY = 0;
            int RadiationLevel = 0;
            int Variant = 0;
            bool operator==(const GameTileReference&) const = default;
        };

        /** Resident key metadata; it intentionally does not retain CPU pixel storage. */
        struct Identity
        {
            std::string SourceId;
            std::string ContentRevision;
            std::string EnvironmentId;
            std::optional<std::string> FloorId;
            GameTileReference Reference;
            SourceType Type = SourceType::GameNativeBC1;
            int WidthPixels = 2048;
            int HeightPixels = 2048;
            Format PixelFormat = Format::BC1;
            size_t ByteCount = 0;
            size_t RowStrideBytes = 0;
            bool SRGB = false;
            Point WorldOrigin;
            Point WorldU;
            Point WorldV;
            bool operator==(const Identity&) const = default;
        };

        static constexpr int kMaximumDimensionPixels = 16384;
        static constexpr size_t kMaximumCpuImageBytes = 64u * 1024u * 1024u;

        std::string SourceId;
        std::string ContentRevision;
        std::string EnvironmentId;
        std::optional<std::string> FloorId;
        GameTileReference Reference;
        SourceType Type = SourceType::GameNativeBC1;
        int WidthPixels = 2048;
        int HeightPixels = 2048;
        Format PixelFormat = Format::BC1;
        size_t ByteCount = 0;
        size_t RowStrideBytes = 0;
        bool SRGB = false;
        Point WorldOrigin;
        Point WorldU;
        Point WorldV;
        std::shared_ptr<const CpuTerrainImagePixels> CpuPixels;

        /** Copies stable identity metadata without copying or retaining pixel bytes. */
        Identity ResidentIdentity() const
        {
            return {SourceId, ContentRevision, EnvironmentId, FloorId, Reference, Type,
                WidthPixels, HeightPixels, PixelFormat, ByteCount, RowStrideBytes, SRGB,
                WorldOrigin, WorldU, WorldV};
        }

        /** Validates common identity, storage interpretation and invertible world registration. */
        bool IsValid() const
        {
            if (SourceId.empty() || ContentRevision.empty() || EnvironmentId.empty() ||
                (FloorId && FloorId->empty()) || !IsValidStorage() || !IsValidRegistration()) return false;
            return ImageToWorld(1.0, 0.0).IsFinite() && ImageToWorld(0.0, 1.0).IsFinite() &&
                ImageToWorld(1.0, 1.0).IsFinite();
        }

        /** Maps normalized image-edge coordinates to native world XY. */
        Point ImageToWorld(double u, double v) const
        {
            return {WorldOrigin.X + u * WorldU.X + v * WorldV.X,
                WorldOrigin.Y + u * WorldU.Y + v * WorldV.Y};
        }

        /** Inverts registration; fails for invalid inputs or nonfinite results. */
        bool WorldToImage(Point world, Point& image) const
        {
            image = {};
            if (!IsValid() || !world.IsFinite()) return false;
            const double dx = world.X - WorldOrigin.X;
            const double dy = world.Y - WorldOrigin.Y;
            const double determinant = WorldU.X * WorldV.Y - WorldV.X * WorldU.Y;
            image = {(dx * WorldV.Y - dy * WorldV.X) / determinant,
                (WorldU.X * dy - WorldU.Y * dx) / determinant};
            return image.IsFinite();
        }

        /** Tests half-open image coverage; out-of-coverage positions have no fallback. */
        bool ContainsWorld(Point world) const
        {
            Point image;
            return WorldToImage(world, image) && image.X >= 0.0 && image.X < 1.0 &&
                image.Y >= 0.0 && image.Y < 1.0;
        }

        /** Returns valid source pixel bounds for one 256-pixel chunk. */
        bool GetChunkPixelBounds(int chunkX, int chunkY, int& pixelX, int& pixelY,
            int& widthPixels, int& heightPixels) const
        {
            pixelX = pixelY = widthPixels = heightPixels = 0;
            if (!IsValid() || chunkX < 0 || chunkY < 0) return false;
            const size_t startX = static_cast<size_t>(chunkX) * 256u;
            const size_t startY = static_cast<size_t>(chunkY) * 256u;
            if (startX >= static_cast<size_t>(WidthPixels) || startY >= static_cast<size_t>(HeightPixels))
                return false;
            pixelX = static_cast<int>(startX);
            pixelY = static_cast<int>(startY);
            widthPixels = (std::min)(256, WidthPixels - pixelX);
            heightPixels = (std::min)(256, HeightPixels - pixelY);
            return true;
        }

        /** Returns TL,TR,BR,BL world corners for the valid pixels in a chunk. */
        std::array<Point, 4> ChunkCorners(int chunkX, int chunkY) const
        {
            int x0 = 0;
            int y0 = 0;
            int width = 0;
            int height = 0;
            if (!GetChunkPixelBounds(chunkX, chunkY, x0, y0, width, height)) return {};
            const double u0 = static_cast<double>(x0) / WidthPixels;
            const double v0 = static_cast<double>(y0) / HeightPixels;
            const double u1 = static_cast<double>(x0 + width) / WidthPixels;
            const double v1 = static_cast<double>(y0 + height) / HeightPixels;
            return {ImageToWorld(u0, v0), ImageToWorld(u1, v0),
                ImageToWorld(u1, v1), ImageToWorld(u0, v1)};
        }

        /** Returns conservative inclusive chunk bounds intersecting a world-radius square. */
        std::array<int, 4> VisibleChunks(Point center, double radius) const
        {
            std::array<int, 4> empty{0, 0, -1, -1};
            if (!IsValid() || !center.IsFinite() || !std::isfinite(radius) || radius <= 0.0) return empty;
            Point minimum{1.0, 1.0};
            Point maximum{0.0, 0.0};
            for (int corner = 0; corner < 4; ++corner)
            {
                Point image;
                if (!WorldToImage({center.X + ((corner & 1) ? radius : -radius),
                    center.Y + ((corner & 2) ? radius : -radius)}, image)) return empty;
                minimum.X = (std::min)(minimum.X, image.X);
                minimum.Y = (std::min)(minimum.Y, image.Y);
                maximum.X = (std::max)(maximum.X, image.X);
                maximum.Y = (std::max)(maximum.Y, image.Y);
            }
            return PixelIntersection(minimum, maximum);
        }

    private:
        /** Verifies that source type, format and owned storage metadata agree. */
        bool IsValidStorage() const
        {
            if (WidthPixels <= 0 || HeightPixels <= 0 ||
                WidthPixels > kMaximumDimensionPixels || HeightPixels > kMaximumDimensionPixels) return false;
            if (Type == SourceType::GameNativeBC1)
                return IsValidNativeStorage();
            if (Type == SourceType::CpuOwnedPixels) return IsValidCpuStorage();
            return false;
        }

        /** Preserves the existing native source's 2048-square BC1 contract. */
        bool IsValidNativeStorage() const
        {
            return WidthPixels == 2048 && HeightPixels == 2048 && PixelFormat == Format::BC1 &&
                !CpuPixels && ByteCount == 0 && RowStrideBytes == 0 && Reference.SampleWorld.IsFinite() &&
                Reference.RadiationLevel >= 0 && Reference.Variant >= 0 && Reference.Variant <= 1;
        }

        /** Checks immutable CPU ownership, row layout and a bounded exact byte count. */
        bool IsValidCpuStorage() const
        {
            if (!CpuPixels || PixelFormat == Format::BC1 ||
                (PixelFormat != Format::RGBA8 && PixelFormat != Format::BGRA8)) return false;
            const size_t rowBytes = static_cast<size_t>(WidthPixels) * 4u;
            if (RowStrideBytes < rowBytes || RowStrideBytes > kMaximumCpuImageBytes ||
                static_cast<size_t>(HeightPixels) > kMaximumCpuImageBytes / RowStrideBytes) return false;
            const size_t requiredBytes = RowStrideBytes * static_cast<size_t>(HeightPixels);
            return requiredBytes == ByteCount && CpuPixels->Bytes.size() == ByteCount;
        }

        /** Validates the affine basis and all image-edge world coordinates. */
        bool IsValidRegistration() const
        {
            if (!WorldOrigin.IsFinite() || !WorldU.IsFinite() || !WorldV.IsFinite()) return false;
            const double determinant = WorldU.X * WorldV.Y - WorldV.X * WorldU.Y;
            return std::isfinite(determinant) && determinant != 0.0 &&
                std::isfinite(WorldU.X / determinant) && std::isfinite(WorldU.Y / determinant) &&
                std::isfinite(WorldV.X / determinant) && std::isfinite(WorldV.Y / determinant);
        }

        /** Converts normalized image bounds to clamped half-open chunk coordinates. */
        std::array<int, 4> PixelIntersection(Point minimum, Point maximum) const
        {
            std::array<int, 4> empty{0, 0, -1, -1};
            minimum.X = (std::max)(0.0, minimum.X);
            minimum.Y = (std::max)(0.0, minimum.Y);
            maximum.X = (std::min)(1.0, maximum.X);
            maximum.Y = (std::min)(1.0, maximum.Y);
            if (minimum.X >= maximum.X || minimum.Y >= maximum.Y) return empty;
            const int columns = (WidthPixels + 255) / 256;
            const int rows = (HeightPixels + 255) / 256;
            const int firstX = static_cast<int>(std::floor(minimum.X * WidthPixels / 256.0));
            const int firstY = static_cast<int>(std::floor(minimum.Y * HeightPixels / 256.0));
            const int lastX = static_cast<int>(std::ceil(maximum.X * WidthPixels / 256.0)) - 1;
            const int lastY = static_cast<int>(std::ceil(maximum.Y * HeightPixels / 256.0)) - 1;
            return {(std::min)(firstX, columns - 1), (std::min)(firstY, rows - 1),
                (std::min)(lastX, columns - 1), (std::min)(lastY, rows - 1)};
        }
    };
}
