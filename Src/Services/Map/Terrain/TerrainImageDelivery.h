// Defines a passive canonical BC1 delivery envelope, separating source identity from transient generations.
// This value never advances acquisition, retires callbacks or owns native/GPU resources.
#pragma once

#include "TerrainImage.h"
#include <cstdint>
#include <memory>
#include <string>

namespace Services::Map::Terrain
{
    /** Carries shared immutable BC1 data and availability evidence, without implementing acquisition. */
    struct TerrainImageDelivery
    {
        /** Describes CPU data availability, not GPU readiness or native-work retirement. */
        enum class Availability
        {
            Pending,
            Ready,
            Unavailable,
            Failed
        };

        /** Identifies a transient delivery independently of persistent source/content identity. */
        struct Stamp
        {
            std::string WorldId;
            std::uint64_t SessionGeneration = 0;
            std::uint64_t SelectionRevision = 0;
            std::uint64_t RequestGeneration = 0;
        };

        Availability State = Availability::Unavailable;
        Stamp Context;
        std::string RegionId;
        TerrainImage::Identity Source;
        std::shared_ptr<const TerrainImage> Image;
        std::string Error;

        /** Reports a matching Ready BC1 payload, without implying presentation or native retirement. */
        bool HasReadyImage() const noexcept
        {
            return State == Availability::Ready && Image && Source == Image->GetIdentity();
        }
    };
}
