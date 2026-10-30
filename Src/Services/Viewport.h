#pragma once
#include "ServiceTypes.h"
#include "Textures.h"
#include <array>
#include <string_view>

namespace MiniMap::Services
{
    struct Widget { std::uint64_t id = 0; };
    struct Vec2 { float x, y; };
    using Quad = std::array<Vec2, 4>;
    using Color = std::uint32_t; // 0xAABBGGRR
    enum class DrawLayer { Window, Foreground };
    // Borrowed only for the synchronous RenderFn invocation. Never retain it.
    class ICanvas
    {
    public:
        virtual ~ICanvas() = default;
        virtual Vec2 WindowPosition() const = 0;
        virtual Vec2 WindowSize() const = 0;
        virtual Vec2 ContentAvailable() const = 0;
        virtual void ReserveContent(Vec2 size) = 0;
        virtual Vec2 MeasureText(std::string_view utf8, bool hideAfterHashHash, float wrapWidth) const = 0;
        virtual void PushClip(DrawLayer, Vec2 min, Vec2 max, bool intersect) = 0;
        virtual void PopClip(DrawLayer) = 0;
        virtual void Image(DrawLayer, Texture, Vec2 min, Vec2 max, Vec2 uvMin, Vec2 uvMax, Color tint) = 0;
        virtual void ImageQuad(DrawLayer, Texture, const Quad& positions, const Quad& uv, Color tint) = 0;
        virtual void FilledRect(DrawLayer, Vec2 min, Vec2 max, Color, float rounding) = 0;
        virtual void Rect(DrawLayer, Vec2 min, Vec2 max, Color, float rounding, float thickness) = 0;
        virtual void FilledCircle(DrawLayer, Vec2 center, float radius, Color, int segments) = 0;
        virtual void Triangle(DrawLayer, const std::array<Vec2, 3>&, Color, float thickness) = 0;
        virtual void FilledTriangle(DrawLayer, const std::array<Vec2, 3>&, Color) = 0;
        virtual void Text(DrawLayer, Vec2 position, Color, std::string_view utf8) = 0;
    };
    using RenderFn = void (*)(void* user, ICanvas& frame);
    struct OverlaySpec
    {
        std::string_view persistentName;
        Vec2 firstUseSize;
        std::optional<Vec2> firstUsePosition;
    };
    class IViewport
    {
    public:
        virtual ~IViewport() = default;
        virtual RegistrationReceipt<Widget> CreateOverlay(const OverlaySpec&, RenderFn, void* user) = 0;
        virtual Status SetVisible(Widget, bool visible) = 0;
        virtual Removal Remove(Widget) = 0;
    };
}
