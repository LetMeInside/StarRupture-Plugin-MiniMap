// Defines the shared inline point-to-rectangle reach admission used by building collection.
#pragma once

namespace MiniMap::BuildingAcquisition::Internal
{
    /** Stores the XY edges of one precomputed rectangle used during building admission. */
    struct Bounds2D
    {
        double MinX = 0.0;
        double MinY = 0.0;
        double MaxX = 0.0;
        double MaxY = 0.0;
    };

    /** Applies shared broad-phase and exact circular reach checks to one building origin. */
    class BuildingSpatialAdmission final
    {
    public:
        /**
         * Accepts an origin inside the half-open query rectangle or within circular reach of it.
         * The overlapping rectangle is an inclusive broad-phase bound expanded once per query.
         * @param x Building origin X coordinate in map units.
         * @param y Building origin Y coordinate in map units.
         * @param fastAccept Original half-open query rectangle.
         * @param overlappingAccept Conservative inclusive rectangle expanded by reach.
         * @param reach Maximum XY distance from the original rectangle.
         * @return Whether the origin is admitted by the existing point-to-rectangle rule.
         */
        [[nodiscard]] static inline bool IsBuildingAccepted(
            double x,
            double y,
            const Bounds2D& fastAccept,
            const Bounds2D& overlappingAccept,
            double reach) noexcept
        {
            if (x >= fastAccept.MinX && x < fastAccept.MaxX &&
                y >= fastAccept.MinY && y < fastAccept.MaxY)
            {
                return true;
            }

            if (x < overlappingAccept.MinX || x > overlappingAccept.MaxX ||
                y < overlappingAccept.MinY || y > overlappingAccept.MaxY)
            {
                return false;
            }

            double distanceX = 0.0;
            if (x < fastAccept.MinX)
            {
                distanceX = fastAccept.MinX - x;
            }
            else if (x > fastAccept.MaxX)
            {
                distanceX = x - fastAccept.MaxX;
            }

            double distanceY = 0.0;
            if (y < fastAccept.MinY)
            {
                distanceY = fastAccept.MinY - y;
            }
            else if (y > fastAccept.MaxY)
            {
                distanceY = y - fastAccept.MaxY;
            }

            return distanceX * distanceX + distanceY * distanceY <= reach * reach;
        }
    };
}
