#pragma once

// Declares copied building acquisition geometry values shared by the collector and geometry helpers.
#if defined(MODLOADER_CLIENT_BUILD)
#include <array>
#include <cstdint>
#include <vector>

namespace MiniMapBuildingCollector
{
    /** A 2D axis-aligned rectangle used by the existing collector and geometry helpers. */
    struct Bounds
    {
        double MinX = 0;
        double MinY = 0;
        double MaxX = 0;
        double MaxY = 0;
        bool Valid = false;
    };

    /** Classifies why copied spline geometry could not be represented. */
    enum class GeometryIssue : uint8_t
    {
        None,
        MissingSource,
        Uninitialized,
        Looped,
        InvalidArray,
        InvalidCurve,
        UnsupportedMode,
        NonFinite,
        Oversized,
        InvalidTransform
    };

    /** A copied XYZ point in the existing geometry helper's numerical representation. */
    using Point = std::array<double, 3>;

    /** A 3D axis-aligned bound for copied local geometry values. */
    struct LocalBounds
    {
        Point Min = {};
        Point Max = {};
        bool Valid = false;

        /** Compares all stored local-bound values. */
        bool operator==(const LocalBounds&) const = default;
    };

    /** One copied world-space cubic segment with its native interpolation keys and mode. */
    struct CurveSegment
    {
        std::array<Point, 4> Controls = {}; // Copied world-space XYZ cubic Bezier, Unreal units; no entity transform applied.
        // Initialized and reconstructed controls retain this verified world-space basis.
        double StartKey = 0;
        double EndKey = 1;
        uint8_t Mode = 0; // Native EInterpCurveMode; constant jumps stay discontinuous.

        /** Compares all copied segment values. */
        bool operator==(const CurveSegment&) const = default;
    };

    /** Owns copied spline segments and preserves the current extraction outcome. */
    struct SplineGeometry
    {
        std::vector<CurveSegment> Segments;
        /** Transitional derived control-hull result, not a native bound or complete rendering envelope; separate it from clean facts in a later structural migration. */
        Bounds CenterlineBounds;
        GeometryIssue Issue = GeometryIssue::None;
    };
}
#endif
