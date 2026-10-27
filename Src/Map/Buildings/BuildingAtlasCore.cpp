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
        void Occupy(std::vector<Rect>& free, const Rect& used)
        {
            std::vector<Rect> remaining;
            for (const auto& r : free)
            {
                if (!Overlaps(r, used)) { remaining.push_back(r); continue; }
                const unsigned right = unsigned(r.X) + r.Width, bottom = unsigned(r.Y) + r.Height;
                const unsigned usedRight = unsigned(used.X) + used.Width, usedBottom = unsigned(used.Y) + used.Height;
                auto append = [&](unsigned x, unsigned y, unsigned w, unsigned h)
                { if (w && h) remaining.push_back({std::uint16_t(x), std::uint16_t(y), std::uint16_t(w), std::uint16_t(h)}); };
                // Maximal free rectangles can overlap each other, never occupied
                // pixels. Keep full strips so a later image can span old split
                // boundaries; subtract each new allocation from ALL free space.
                if (used.X > r.X) append(r.X, r.Y, used.X - r.X, r.Height);
                if (usedRight < right) append(usedRight, r.Y, right - usedRight, r.Height);
                if (used.Y > r.Y) append(r.X, r.Y, r.Width, used.Y - r.Y);
                if (usedBottom < bottom) append(r.X, usedBottom, r.Width, bottom - usedBottom);
            }
            for (std::size_t i = 0; i < remaining.size();)
            {
                const auto& a = remaining[i];
                bool contained = false;
                for (std::size_t j = 0; j < remaining.size(); ++j)
                {
                    if (i == j) continue;
                    const auto& b = remaining[j];
                    if (a.X >= b.X && a.Y >= b.Y && unsigned(a.X) + a.Width <= unsigned(b.X) + b.Width &&
                        unsigned(a.Y) + a.Height <= unsigned(b.Y) + b.Height)
                    { contained = true; break; }
                }
                if (contained) remaining.erase(remaining.begin() + i);
                else ++i;
            }
            free.swap(remaining);
        }
        Result Pack(const std::vector<const CroppedSource*>& sources, const Layout& previous, std::uint16_t side,
            Layout& candidate, bool buildPixels = true)
        {
            std::vector<const CroppedSource*> order;
            order.reserve(sources.size());
            for (const auto* source : sources)
            {
                if (!source) return Result::InternalError;
                const auto& crop = source->Crop;
                std::size_t bytes{}, width{}, height{};
                if (!crop.Width || !crop.Height || std::uint32_t(crop.X) + crop.Width > SourceSize ||
                    std::uint32_t(crop.Y) + crop.Height > SourceSize) return Result::InternalError;
                if (!ImageBytes(crop.Width, crop.Height, bytes) ||
                    !AddSize(crop.Width, 2 * Gutter, width) || !AddSize(crop.Height, 2 * Gutter, height)) return Result::Overflow;
                if (bytes != source->Rgba.size()) return Result::InternalError;
                if (width > side || height > side) return Result::Capacity;
                order.push_back(source);
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
            // Reconstruct free space around immutable retained allocations. Split
            // every intersecting free rectangle into maximal strips; this also
            // exposes the new right/bottom area when the canvas grows.
            for (const auto& image : previous.Images)
            {
                const auto source = std::find_if(order.begin(), order.end(),
                    [&](const CroppedSource* s) { return s->Identifier == image.Identifier; });
                if (source == order.end() || !Inside(image.Allocation, side) ||
                    image.Content.X != image.Allocation.X + Gutter || image.Content.Y != image.Allocation.Y + Gutter ||
                    image.Content.Width != (*source)->Crop.Width || image.Content.Height != (*source)->Crop.Height ||
                    image.Allocation.Width != image.Content.Width + 2 * Gutter ||
                    image.Allocation.Height != image.Content.Height + 2 * Gutter) return Result::InternalError;
                for (const auto& placed : candidate.Images)
                    if (Overlaps(image.Allocation, placed.Allocation)) return Result::InternalError;
                Occupy(free, image.Allocation);
                candidate.Images.push_back(image);
                order.erase(source); // Only previously unplaced sources enter the allocator below.
            }
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
                const Rect allocation{slot.X, slot.Y, width, height};
                if (!Inside(allocation, side)) return Result::InternalError;
                for (const auto& placed : candidate.Images)
                    if (Overlaps(allocation, placed.Allocation)) return Result::InternalError;
                Occupy(free, allocation);
                const Rect content{std::uint16_t(slot.X + Gutter), std::uint16_t(slot.Y + Gutter), source->Crop.Width, source->Crop.Height};
                if (!Inside(content, side)) return Result::InternalError;
                const double u0 = double(content.X) / side, v0 = double(content.Y) / side;
                const double u1 = double(std::uint32_t(content.X) + content.Width) / side;
                const double v1 = double(std::uint32_t(content.Y) + content.Height) / side;
                candidate.Images.push_back({source->Identifier, source->Crop, allocation, content,
                    {{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}}});
            }
            // Recompute ALL UVs, including retained images after canvas growth.
            for (auto& image : candidate.Images)
            {
                const auto& r = image.Content;
                const double u0 = double(r.X) / side, v0 = double(r.Y) / side;
                const double u1 = double(unsigned(r.X) + r.Width) / side;
                const double v1 = double(unsigned(r.Y) + r.Height) / side;
                image.Corners = {{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};
            }
            // Subset fit probes use the same placement algorithm, but only the
            // final selection needs a complete RGBA canvas.
            if (!buildPixels) return Result::Ok;
            std::size_t bytes{};
            if (!ImageBytes(side, side, bytes)) return Result::Overflow;
            candidate.Rgba.assign(bytes,0);
            for (const auto& image : candidate.Images)
            {
                const auto found = std::find_if(sources.begin(), sources.end(),
                    [&](const CroppedSource* s) { return s->Identifier == image.Identifier; });
                if (found == sources.end()) return Result::InternalError;
                const auto& source = **found;
                const auto& allocation = image.Allocation;
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

        void Commit(Layout& published, Layout& candidate) noexcept
        {
            // Single-owner publication: no allocation or throwing operation can
            // occur after the first swap. The displaced layout dies in the caller.
            std::swap(published.Width, candidate.Width);
            std::swap(published.Height, candidate.Height);
            std::swap(published.Revision, candidate.Revision);
            published.Images.swap(candidate.Images);
            published.Rgba.swap(candidate.Rgba);
            published.DeferredIdentifiers.swap(candidate.DeferredIdentifiers);
        }
    }

    Result Atlas::Add(const std::string& identifier, std::uint32_t width, std::uint32_t height,
        std::span<const std::uint8_t> rgba) noexcept
    {
        try
        {
            if (!width || !height || width > SourceSize || height > SourceSize) return Result::UnsupportedDimensions;
            std::size_t bytes{};
            if (!ImageBytes(width, height, bytes)) return Result::Overflow;
            if (rgba.size() != bytes) return Result::InvalidBuffer;
            if (identifier.empty()) return Result::EmptyIdentifier;
            const auto position = std::lower_bound(Retained.begin(), Retained.end(), identifier,
                [](const CroppedSource& source, const std::string& key) { return BytesLess(source.Identifier, key); });
            if (position != Retained.end() && position->Identifier == identifier) return Result::DuplicateIdentifier;
            bool coverage = false;
            for (std::size_t i = 3; i < rgba.size(); i += 4) coverage |= rgba[i] != 0;
            if (!coverage) return Result::EmptyAlpha;
            CroppedSource source;
            source.Identifier = identifier;
            source.Crop = {0, 0, std::uint16_t(width), std::uint16_t(height)};
            source.Rgba.assign(rgba.begin(), rgba.end());
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
            std::vector<const CroppedSource*> sources;
            sources.reserve(Retained.size());
            for (const auto& source : Retained) sources.push_back(&source);
            for (std::uint16_t side : {std::uint16_t(512), std::uint16_t(1024)})
            {
                if (side < Packed.Width || side < Packed.Height) continue;
                Layout candidate;
                const auto result = Pack(sources, Packed, side, candidate);
                if (result == Result::Capacity) continue;
                if (result != Result::Ok) return result;
                candidate.Revision = Packed.Revision + 1;
                Commit(Packed, candidate);
                return Result::Ok;
            }
            return Result::Capacity;
        }
        catch (const std::bad_alloc&) { return Result::AllocationFailure; }
        catch (const std::length_error&) { return Result::Overflow; }
        catch (...) { return Result::InternalError; }
    }

    Result Atlas::RepackSubset() noexcept
    {
        try
        {
            if (Retained.empty()) return Result::EmptySources;
            if (Packed.Revision == (std::numeric_limits<std::uint64_t>::max)()) return Result::Overflow;
            std::vector<bool> admitted(Retained.size(), false);
            std::vector<const CroppedSource*> selection;
            selection.reserve(Retained.size());

            // Protect the current CPU layout, not a caller-supplied identity set.
            // Validate that every mandatory image still has one retained source.
            for (const auto& image : Packed.Images)
            {
                const auto source = std::lower_bound(Retained.begin(), Retained.end(), image.Identifier,
                    [](const CroppedSource& retained, const std::string& key)
                    { return BytesLess(retained.Identifier, key); });
                if (source == Retained.end() || source->Identifier != image.Identifier)
                    return Result::InternalError;
                const auto index = std::size_t(source - Retained.begin());
                if (admitted[index]) return Result::InternalError;
                admitted[index] = true;
            }
            for (std::size_t i = 0; i < Retained.size(); ++i)
                if (admitted[i]) selection.push_back(&Retained[i]);

            std::uint16_t minimumSide = (std::max)(Packed.Width, Packed.Height);
            auto fit = [&]() -> Result
            {
                for (std::uint16_t side : {std::uint16_t(512), std::uint16_t(1024)})
                {
                    if (side < minimumSide) continue;
                    Layout probe;
                    const auto result = Pack(selection, Packed, side, probe, false);
                    if (result == Result::Capacity) continue;
                    if (result != Result::Ok) return result;
                    minimumSide = side;
                    return Result::Ok;
                }
                return Result::Capacity;
            };
            if (!selection.empty())
            {
                const auto result = fit();
                if (result != Result::Ok) return result; // Never defer a mandatory image.
            }

            // Add preserves unsigned-byte identifier ordering in Retained. Each
            // probe includes every mandatory and previously accepted source.
            for (std::size_t i = 0; i < Retained.size(); ++i)
            {
                if (admitted[i]) continue;
                selection.push_back(&Retained[i]);
                const auto result = fit();
                if (result == Result::Ok) admitted[i] = true;
                else
                {
                    selection.pop_back();
                    if (result != Result::Capacity) return result;
                }
            }
            if (selection.empty()) return Result::Capacity;

            Layout candidate;
            const auto result = Pack(selection, Packed, minimumSide, candidate);
            if (result != Result::Ok) return result;
            candidate.DeferredIdentifiers.reserve(Retained.size() - selection.size());
            for (std::size_t i = 0; i < Retained.size(); ++i)
                if (!admitted[i]) candidate.DeferredIdentifiers.push_back(Retained[i].Identifier);
            candidate.Revision = Packed.Revision + 1;
            Commit(Packed, candidate);
            return Result::Ok;
        }
        catch (const std::bad_alloc&) { return Result::AllocationFailure; }
        catch (const std::length_error&) { return Result::Overflow; }
        catch (...) { return Result::InternalError; }
    }
}
