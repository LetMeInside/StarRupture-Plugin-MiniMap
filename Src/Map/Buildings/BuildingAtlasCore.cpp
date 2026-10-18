#include "BuildingAtlasCore.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace MiniMapBuildingAtlasCore
{
    namespace
    {
        static_assert(std::is_nothrow_move_constructible_v<CroppedSource> &&
            std::is_nothrow_move_assignable_v<CroppedSource>);
        bool AddSize(std::size_t a, std::size_t b, std::size_t& out) noexcept
        {
            if (b > (std::numeric_limits<std::size_t>::max)() - a) return false;
            out = a + b; return true;
        }
        bool Multiply(std::size_t a, std::size_t b, std::size_t& out) noexcept
        {
            if (a && b > (std::numeric_limits<std::size_t>::max)() / a) return false;
            out = a * b; return true;
        }
        bool ImageBytes(std::size_t width, std::size_t height, std::size_t& bytes) noexcept
        {
            std::size_t pixels{};
            return width && height && Multiply(width, height, pixels) && Multiply(pixels, 4, bytes);
        }
        bool Pixel(std::size_t x, std::size_t y, std::size_t width, std::size_t height, std::size_t bytes, std::size_t& offset) noexcept
        {
            std::size_t row{}, index{}, end{};
            return x < width && y < height && Multiply(y, width, row) && AddSize(row, x, index) &&
                Multiply(index, 4, offset) && AddSize(offset, 4, end) && end <= bytes;
        }
        bool BytesLess(const std::string& a, const std::string& b) noexcept
        {
            return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                [](char x, char y) { return static_cast<unsigned char>(x) < static_cast<unsigned char>(y); });
        }
        bool Inside(const Rect& r, std::uint16_t side) noexcept
        {
            return r.Width && r.Height && std::uint32_t(r.X) + r.Width <= side && std::uint32_t(r.Y) + r.Height <= side;
        }
        bool Overlaps(const Rect& a, const Rect& b) noexcept
        {
            return a.X < std::uint32_t(b.X) + b.Width && b.X < std::uint32_t(a.X) + a.Width &&
                a.Y < std::uint32_t(b.Y) + b.Height && b.Y < std::uint32_t(a.Y) + a.Height;
        }
        Result Pack(const std::vector<CroppedSource>& sources, std::uint16_t side, Layout& candidate)
        {
            std::vector<const CroppedSource*> order;
            order.reserve(sources.size());
            for (const auto& source : sources)
            {
                const auto& crop = source.Crop;
                std::size_t bytes{}, width{}, height{};
                if (!crop.Width || !crop.Height || std::uint32_t(crop.X) + crop.Width > SourceSize ||
                    std::uint32_t(crop.Y) + crop.Height > SourceSize) return Result::InternalError;
                if (!ImageBytes(crop.Width, crop.Height, bytes) ||
                    !AddSize(crop.Width, 2 * Gutter, width) || !AddSize(crop.Height, 2 * Gutter, height)) return Result::Overflow;
                if (bytes != source.Rgba.size()) return Result::InternalError;
                if (width > side || height > side) return Result::Capacity;
                order.push_back(&source);
            }
            std::sort(order.begin(), order.end(), [](const CroppedSource* a, const CroppedSource* b)
            {
                // Bounded by SourceSize + 2*Gutter; products fit std::uint32_t.
                const std::uint32_t aw = a->Crop.Width + 2 * Gutter, ah = a->Crop.Height + 2 * Gutter;
                const std::uint32_t bw = b->Crop.Width + 2 * Gutter, bh = b->Crop.Height + 2 * Gutter;
                const auto ak = std::tuple{(std::max)(aw, ah), aw * ah, ah, aw};
                const auto bk = std::tuple{(std::max)(bw, bh), bw * bh, bh, bw};
                return ak != bk ? ak > bk : BytesLess(a->Identifier, b->Identifier);
            });
            std::vector<Rect> free{{0, 0, side, side}};
            candidate.Width = candidate.Height = side;
            candidate.Images.reserve(sources.size());
            for (const auto* source : order)
            {
                const std::uint16_t width = std::uint16_t(source->Crop.Width + 2 * Gutter);
                const std::uint16_t height = std::uint16_t(source->Crop.Height + 2 * Gutter);
                std::size_t best = free.size();
                std::tuple<std::uint16_t, std::uint16_t, std::uint16_t, std::uint16_t> score{};
                for (std::size_t i = 0; i < free.size(); ++i)
                {
                    const auto& r = free[i];
                    if (!Inside(r, side)) return Result::InternalError;
                    if (width > r.Width || height > r.Height) continue;
                    const std::uint16_t dx = std::uint16_t(r.Width - width), dy = std::uint16_t(r.Height - height);
                    const auto next = std::tuple{(std::min)(dx, dy), (std::max)(dx, dy), r.Y, r.X};
                    if (best == free.size() || next < score) { best = i; score = next; }
                    // Equal scores keep the first free-list entry.
                }
                if (best == free.size()) return Result::Capacity;
                const Rect slot = free[best];
                free.erase(free.begin() + best);
                const std::uint16_t dx = std::uint16_t(slot.Width - width), dy = std::uint16_t(slot.Height - height);
                const Rect allocation{slot.X, slot.Y, width, height};
                if (!Inside(allocation, side)) return Result::InternalError;
                for (const auto& placed : candidate.Images)
                    if (Overlaps(allocation, placed.Allocation)) return Result::InternalError;
                // Report's split rule; append right then bottom, retaining stable list order.
                const Rect right{std::uint16_t(slot.X + width), slot.Y, dx, dx > dy ? slot.Height : height};
                const Rect bottom{slot.X, std::uint16_t(slot.Y + height), dx > dy ? width : slot.Width, dy};
                if (right.Width && right.Height) free.push_back(right);
                if (bottom.Width && bottom.Height) free.push_back(bottom);
                const Rect content{std::uint16_t(slot.X + Gutter), std::uint16_t(slot.Y + Gutter), source->Crop.Width, source->Crop.Height};
                if (!Inside(content, side)) return Result::InternalError;
                const double u0 = double(content.X) / side, v0 = double(content.Y) / side;
                const double u1 = double(std::uint32_t(content.X) + content.Width) / side;
                const double v1 = double(std::uint32_t(content.Y) + content.Height) / side;
                candidate.Images.push_back({source->Identifier, source->Crop, allocation, content,
                    {{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}}});
            }
            std::size_t bytes{};
            if (!ImageBytes(side, side, bytes)) return Result::Overflow;
            candidate.Rgba.assign(bytes, 0);
            for (std::size_t i = 0; i < order.size(); ++i)
            {
                const auto& source = *order[i];
                const auto& allocation = candidate.Images[i].Allocation;
                for (std::size_t y = 0; y < allocation.Height; ++y)
                    for (std::size_t x = 0; x < allocation.Width; ++x)
                    {
                        const std::size_t sx = x < Gutter ? 0 : (std::min)(x - Gutter, std::size_t(source.Crop.Width - 1));
                        const std::size_t sy = y < Gutter ? 0 : (std::min)(y - Gutter, std::size_t(source.Crop.Height - 1));
                        std::size_t from{}, to{};
                        if (!Pixel(sx, sy, source.Crop.Width, source.Crop.Height, source.Rgba.size(), from) ||
                            !Pixel(std::size_t(allocation.X) + x, std::size_t(allocation.Y) + y, side, side, bytes, to))
                            return Result::InternalError;
                        std::memcpy(candidate.Rgba.data() + to, source.Rgba.data() + from, 4);
                    }
            }
            return Result::Ok;
        }
    }

    Result Atlas::Add(const std::string& identifier, std::uint32_t width, std::uint32_t height,
        std::span<const std::uint8_t> rgba) noexcept
    {
        try
        {
            if (width != SourceSize || height != SourceSize) return Result::UnsupportedDimensions;
            std::size_t bytes{};
            if (!ImageBytes(width, height, bytes)) return Result::Overflow;
            if (rgba.size() != bytes) return Result::InvalidBuffer;
            if (identifier.empty()) return Result::EmptyIdentifier;
            const auto position = std::lower_bound(Retained.begin(), Retained.end(), identifier,
                [](const CroppedSource& source, const std::string& key) { return BytesLess(source.Identifier, key); });
            if (position != Retained.end() && position->Identifier == identifier) return Result::DuplicateIdentifier;
            if (Retained.size() >= MaximumSources) return Result::SourceLimit;
            std::size_t x0 = width, y0 = height, x1 = 0, y1 = 0;
            for (std::size_t y = 0; y < height; ++y)
                for (std::size_t x = 0; x < width; ++x)
                {
                    std::size_t offset{};
                    if (!Pixel(x, y, width, height, bytes, offset)) return Result::InternalError;
                    if (!rgba[offset + 3]) continue;
                    x0 = (std::min)(x0, x); y0 = (std::min)(y0, y);
                    x1 = (std::max)(x1, x + 1); y1 = (std::max)(y1, y + 1);
                }
            if (!x1 || !y1) return Result::EmptyAlpha;
            if (x0) --x0;
            if (y0) --y0;
            if (x1 < width) ++x1;
            if (y1 < height) ++y1;
            CroppedSource source;
            source.Identifier = identifier;
            source.Crop = {std::uint16_t(x0), std::uint16_t(y0), std::uint16_t(x1 - x0), std::uint16_t(y1 - y0)};
            std::size_t cropBytes{}, rowBytes{};
            if (!ImageBytes(source.Crop.Width, source.Crop.Height, cropBytes) ||
                !Multiply(source.Crop.Width, 4, rowBytes)) return Result::Overflow;
            source.Rgba.resize(cropBytes);
            for (std::size_t y = 0; y < source.Crop.Height; ++y)
            {
                std::size_t from{}, to{}, fromEnd{}, toEnd{};
                if (!Pixel(x0, y0 + y, width, height, bytes, from) ||
                    !Pixel(0, y, source.Crop.Width, source.Crop.Height, cropBytes, to) ||
                    !AddSize(from, rowBytes, fromEnd) || fromEnd > bytes ||
                    !AddSize(to, rowBytes, toEnd) || toEnd > cropBytes) return Result::InternalError;
                std::memcpy(source.Rgba.data() + to, rgba.data() + from, rowBytes);
            }
            // String/vector moves are noexcept; insertion has the strong failure guarantee.
            Retained.insert(position, std::move(source));
            return Result::Ok;
        }
        catch (const std::bad_alloc&) { return Result::AllocationFailure; }
        catch (const std::length_error&) { return Result::Overflow; }
        catch (...) { return Result::InternalError; }
    }

    Result Atlas::Repack() noexcept
    {
        try
        {
            if (Retained.empty()) return Result::EmptySources;
            if (Packed.Revision == (std::numeric_limits<std::uint64_t>::max)()) return Result::Overflow;
            for (std::uint16_t side : {std::uint16_t(512), std::uint16_t(1024)})
            {
                Layout candidate;
                const auto result = Pack(Retained, side, candidate);
                if (result == Result::Capacity) continue;
                if (result != Result::Ok) return result;
                candidate.Revision = Packed.Revision + 1;
                // Only no-throw swaps publish the complete candidate. Failed attempts
                // never touch the previous canvas, metadata or revision.
                std::swap(Packed.Width, candidate.Width);
                std::swap(Packed.Height, candidate.Height);
                std::swap(Packed.Revision, candidate.Revision);
                Packed.Images.swap(candidate.Images);
                Packed.Rgba.swap(candidate.Rgba);
                return Result::Ok;
            }
            return Result::Capacity;
        }
        catch (const std::bad_alloc&) { return Result::AllocationFailure; }
        catch (const std::length_error&) { return Result::Overflow; }
        catch (...) { return Result::InternalError; }
    }
}
