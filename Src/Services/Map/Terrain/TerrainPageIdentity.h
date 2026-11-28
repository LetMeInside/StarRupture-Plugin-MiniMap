// Defines provider-supplied logical page identity with an optional safe storage filename stem.
#pragma once
#include <cstdint>
#include <string>

namespace Services::Map::Terrain
{
    /** Identifies one provider-owned page and its optional storage filename stem; registration defines placement.
     * Providers issue stable, distinct collection keys and indices, including (0, 0) for standalone images.
     * FilenameStem, when set, is a safe generic filename component and is not part of serialized page identity.
     * Keys contain 1..128 ASCII letters/digits/underscores/hyphens/periods, with no leading/trailing
     * period, consecutive periods or Windows device names. Keys differing only in ASCII case share
     * one persistence namespace. This value supplies no path, extension, backend or IO capability.
     */
    struct TerrainPageIdentity final
    {
        std::string CollectionKey;
        std::int32_t X = 0;
        std::int32_t Y = 0;
        std::string FilenameStem;

        /** Compares exact provider evidence for inspection/retrieval revalidation, including spelling. */
        bool operator==(const TerrainPageIdentity&) const = default;
    };
}
