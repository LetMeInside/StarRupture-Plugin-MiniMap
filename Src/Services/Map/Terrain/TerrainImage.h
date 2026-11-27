// Defines immutable canonical BC1 storage, content identity and affine world XY registration.
// This value has no acquisition, session, file-cache, GPU or native resource ownership.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Services::Map::Terrain
{
    /** Owns validated BC1 mip-zero bytes and placement; copies share immutable block storage. */
    class TerrainImage final
    {
    public:
        /** Identifies source content independently of service sessions and requests. */
        struct Identity
        {
            std::string SourceId;
            std::string ContentRevision;

            /** Requires explicit nonempty source and content revision identifiers. */
            bool IsValid() const noexcept
            {
                return !SourceId.empty() && !ContentRevision.empty();
            }

            /** Compares content identifiers without comparing pixel owners or bytes. */
            bool operator==(const Identity&) const = default;
        };

        /** Represents native world XY or normalized image coordinates, according to the operation. */
        struct Point
        {
            double X = 0.0;
            double Y = 0.0;

            /** Reports whether both coordinate values can participate in finite mapping. */
            bool IsFinite() const noexcept
            {
                return std::isfinite(X) && std::isfinite(Y);
            }
        };

        /** Maps upper-left image-edge coordinates to world XY; U points right and V points down. */
        struct Registration
        {
            Point Origin;
            Point U;
            Point V;

            /** Requires a finite invertible basis, finite inverse coefficients and finite image corners. */
            bool IsValid() const noexcept
            {
                if (!Origin.IsFinite() || !U.IsFinite() || !V.IsFinite()) return false;
                const double determinant = U.X * V.Y - V.X * U.Y;
                return std::isfinite(determinant) && determinant != 0.0 &&
                    std::isfinite(U.X / determinant) && std::isfinite(U.Y / determinant) &&
                    std::isfinite(V.X / determinant) && std::isfinite(V.Y / determinant) &&
                    ImageToWorld(1.0, 0.0).IsFinite() && ImageToWorld(0.0, 1.0).IsFinite() &&
                    ImageToWorld(1.0, 1.0).IsFinite();
            }

            /** Maps normalized image-edge u/v to world XY; extrapolation may produce nonfinite results. */
            Point ImageToWorld(double u, double v) const noexcept
            {
                return {Origin.X + u * U.X + v * V.X, Origin.Y + u * U.Y + v * V.Y};
            }

            /** Inverts a finite world point; returns no value for invalid registration or overflow. */
            std::optional<Point> WorldToImage(Point world) const noexcept
            {
                if (!IsValid() || !world.IsFinite()) return std::nullopt;
                const double dx = world.X - Origin.X;
                const double dy = world.Y - Origin.Y;
                const double determinant = U.X * V.Y - V.X * U.Y;
                Point image{(dx * V.Y - dy * V.X) / determinant,
                    (U.X * dy - U.Y * dx) / determinant};
                if (!image.IsFinite()) return std::nullopt;
                return image;
            }

            /** Tests half-open logical image coverage without defining player-selection coverage. */
            bool ContainsWorld(Point world) const noexcept
            {
                const auto image = WorldToImage(world);
                return image && image->X >= 0.0 && image->X < 1.0 &&
                    image->Y >= 0.0 && image->Y < 1.0;
            }
        };

        static constexpr std::uint32_t MaximumDimensionPixels = 16384;
        static constexpr std::size_t MaximumCompressedBytes = 64u * 1024u * 1024u;

        /** Validates content/placement and takes exclusive mutable access to the moved BC1 allocation.
         * Throws invalid_argument for invalid identity, registration, dimensions or block length.
         * The caller must relinquish mutable aliases to compressedBytes after successful transfer.
         */
        TerrainImage(Identity identity, std::uint32_t widthPixels, std::uint32_t heightPixels,
            Registration registration, bool sRgb, std::vector<std::uint8_t>&& compressedBytes)
            : identity_(std::move(identity))
            , widthPixels_(widthPixels)
            , heightPixels_(heightPixels)
            , registration_(registration)
            , sRgb_(sRgb)
        {
            const auto requiredBytes = CalculateByteCount(widthPixels, heightPixels);
            if (!identity_.IsValid() || !registration_.IsValid() || !requiredBytes ||
                compressedBytes.size() != *requiredBytes)
            {
                throw std::invalid_argument("Terrain image requires valid identity, registration and exact BC1 storage.");
            }
            storage_ = std::make_shared<const Storage>(std::move(compressedBytes));
        }

        /** Copies metadata and shares the same immutable compressed allocation. */
        TerrainImage(const TerrainImage&) = default;
        /** Replaces this value with another valid image, sharing its compressed allocation. */
        TerrainImage& operator=(const TerrainImage&) = default;

        /** Calculates exact packed BC1 length, rejecting invalid dimensions or memory-bound overflow. */
        static std::optional<std::size_t> CalculateByteCount(
            std::uint32_t widthPixels, std::uint32_t heightPixels) noexcept
        {
            if (widthPixels == 0 || heightPixels == 0 ||
                widthPixels > MaximumDimensionPixels || heightPixels > MaximumDimensionPixels)
            {
                return std::nullopt;
            }
            const std::size_t blocksX = (static_cast<std::size_t>(widthPixels) + 3u) / 4u;
            const std::size_t blocksY = (static_cast<std::size_t>(heightPixels) + 3u) / 4u;
            const std::size_t rowBytes = blocksX * 8u;
            if (blocksY > MaximumCompressedBytes / rowBytes) return std::nullopt;
            return rowBytes * blocksY;
        }

        /** Returns source/content identity, borrowed for this image value's lifetime. */
        const Identity& GetIdentity() const noexcept { return identity_; }
        /** Returns logical pixel width, excluding block padding. */
        std::uint32_t GetWidthPixels() const noexcept { return widthPixels_; }
        /** Returns logical pixel height, excluding block padding. */
        std::uint32_t GetHeightPixels() const noexcept { return heightPixels_; }
        /** Returns the tightly packed byte stride between BC1 block rows. */
        std::size_t GetBlockRowBytes() const noexcept
        {
            return ((static_cast<std::size_t>(widthPixels_) + 3u) / 4u) * 8u;
        }
        /** Borrows compressed bytes while this image or a copy retains their shared allocation. */
        std::span<const std::uint8_t> GetCompressedBytes() const noexcept { return storage_->Bytes; }
        /** Returns validated world registration, borrowed for this image value's lifetime. */
        const Registration& GetRegistration() const noexcept { return registration_; }
        /** Reports the image's color interpretation without changing its BC1 storage format. */
        bool IsSRgb() const noexcept { return sRgb_; }

    private:
        /** Holds the sole immutable allocation shared by copies of an image. */
        struct Storage
        {
            /** Moves the transferred allocation without retaining mutable access to its bytes. */
            explicit Storage(std::vector<std::uint8_t>&& bytes) : Bytes(std::move(bytes)) {}
            const std::vector<std::uint8_t> Bytes;
        };

        Identity identity_;
        std::uint32_t widthPixels_;
        std::uint32_t heightPixels_;
        Registration registration_;
        bool sRgb_;
        std::shared_ptr<const Storage> storage_;
    };
}
