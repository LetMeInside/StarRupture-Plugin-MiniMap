// Builds the five application-owned Forgotten Engine capture definitions from accepted cooked residency metadata.
#pragma once

#include "Services/Map/Terrain/ITerrainDataProvider.h"
#include <vector>

namespace SDK { class UWorld; }
namespace MiniMap::Services { class INativeHooks; }
namespace Services::Map::Terrain { class TerrainService; }

namespace MiniMapTerrainSource
{
    /** Supplies StarRupture cooked-world inputs for the generic terrain capture batch. */
    class ForgottenEngineCaptureConfiguration final
    {
    public:
        /** Creates the five independent outputs defined by the fixed capture configuration. */
        static std::vector<Services::Map::Terrain::ITerrainDataProvider::CaptureDefinition>
            CreateDefinitions();
#if defined(MODLOADER_CLIENT_BUILD)
        /** Submits one fixed capture batch; returns false when admission is unavailable. */
        static bool RequestBatch(
            ::Services::Map::Terrain::TerrainService& terrainService,
            SDK::UWorld* gameplayWorld,
            MiniMap::Services::INativeHooks* hooks);
#endif

    private:
        using CaptureDefinition = Services::Map::Terrain::ITerrainDataProvider::CaptureDefinition;

        /** Copies the common cooked residency, XY registration, and output settings into one slab definition. */
        static CaptureDefinition CreateDefinition(
            const char* regionId,
            double minimumZ,
            double maximumZ);
    };
}
