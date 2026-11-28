// Private native streaming inputs converted from stable provider metadata; no value owns native resources.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <array>
#include <cstdint>
#include <vector>
#include <optional>
#include <string>
#include "../ITerrainDataProvider.h"

namespace Services::Map::Terrain::Capture
{
    /** Identifies an already interned name in the current native process, including its instance number. */
    struct StreamingName final
    {
        std::int32_t ComparisonIndex = 0;
        std::uint32_t Number = 0;
        /** Rejects None and invalid comparison identities. */
        bool IsValid() const noexcept { return ComparisonIndex > 0; }
        /** Compares exact current-process name identity. */
        bool operator==(const StreamingName&) const = default;
    };

    /** Names one required original cell and its distinct expected private package, independently of imagery. */
    struct RequiredStreamingCell final
    {
        StreamingName SourcePackage;
        StreamingName PrivatePackage;
    };

    /** Identifies an original cooked Data Layer asset; the session resolves its own manager's instance. */
    struct StreamingDataLayer final
    {
        StreamingName AssetPackage;
        StreamingName AssetName;
        std::uint8_t State = 2; // Activated; no live-world layer state is copied.
    };

    /** Describes one retained region independently of any output image or future capture job.
     * Conversion interns stable names and supplies a unique original-name-plus-suffix package instance set.
     * The session requires effective regional streaming and streaming-out modes on its private partition.
     */
    struct PrivateStreamingDefinition final
    {
        StreamingName SourcePackage;
        StreamingName DestinationPackage;
        std::array<double, 3> SourcePosition{};
        float Radius = 0;
        std::vector<StreamingName> TargetGrids;
        std::vector<StreamingDataLayer> DataLayers;
        std::vector<RequiredStreamingCell> RequiredCells;

        /** Converts on the admitted game thread; returns no value for unsupported streaming metadata.
         * Interns names and generates transient identities only; never loads, registers or submits work.
         * Data Layer entries require original cooked object paths (package.asset).
         */
        static std::optional<PrivateStreamingDefinition> FromCaptureDefinition(
            const ITerrainDataProvider::CaptureDefinition& definition);
    private:
        /** Interns validated UTF-8 text through the existing SDK name conversion; rejects None/overlong names. */
        static std::optional<StreamingName> ResolveName(const std::string& text);
        /** Requires a rooted long package path with nonempty segments and no object/subobject suffix. */
        static bool IsPackagePath(const std::string& text);
        /** Generates a transient GUID/counter suffix unrelated to persistent image identity. */
        static std::string CreateInstanceSuffix();
        /** Derives a finite centered enclosing sphere, rounding native float radius outward. */
        bool SetStreamingRegion(const ITerrainDataProvider::Bounds& bounds);
        /** Converts original world/generated-cell package identities using one shared native suffix. */
        bool SetPackages(const ITerrainDataProvider::CaptureDefinition& definition, const std::string& suffix);
        /** Splits a cooked object path into stable native package/object names without loading it. */
        static std::optional<StreamingDataLayer> ResolveDataLayerAsset(const std::string& path);
    };
}
#endif
