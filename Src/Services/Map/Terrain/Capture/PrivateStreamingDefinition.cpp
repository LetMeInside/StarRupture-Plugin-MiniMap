// Converts stable capture metadata to the existing private native streaming input contract.
// Name interning and transient package naming never change persistent output identity or execute streaming.
#if defined(MODLOADER_CLIENT_BUILD)
#include "PrivateStreamingDefinition.h"
#include "Native/NativeApi.h"
#include "SDK/Engine_classes.hpp"
#include <Windows.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <exception>
#include <iomanip>
#include <limits>
#include <sstream>

namespace Services::Map::Terrain::Capture
{
    /** Validates UTF-8 and native name capacity before the SDK interns the complete name. */
    std::optional<StreamingName> PrivateStreamingDefinition::ResolveName(const std::string& text)
    {
        if (text.empty() || text.find('\0') != std::string::npos || text.size() > 4096)
            return std::nullopt;
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), nullptr, 0);
        if (length <= 0 || length >= 1024)
            return std::nullopt;
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), wide.data(), length) != length)
            return std::nullopt;
        const auto name = SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(wide.c_str()));
        StreamingName resolved{name.ComparisonIndex, name.Number};
        return resolved.IsValid() ? std::optional<StreamingName>(resolved) : std::nullopt;
    }

    /** Restricts package spelling before native package-path parsing, without choosing any game's mount. */
    bool PrivateStreamingDefinition::IsPackagePath(const std::string& text)
    {
        return text.size() > 1 && text.front() == '/' && text.back() != '/' &&
            text.find("//") == std::string::npos && text.find('\0') == std::string::npos &&
            text.find_first_of(".\\: \t\r\n\"'|?*<>%") == std::string::npos;
    }

    /** Uses a fresh GUID plus a never-wrapped process counter; neither input is a persistent content key. */
    std::string PrivateStreamingDefinition::CreateInstanceSuffix()
    {
        static std::atomic<std::uint64_t> sequence{0};
        const auto identity = ++sequence;
        if (!identity)
            std::terminate();
        const auto guid = SDK::UKismetGuidLibrary::NewGuid();
        std::ostringstream suffix;
        suffix << "_Terrain_" << std::hex << std::setfill('0');
        for (auto part : {guid.A, guid.B, guid.C, guid.D})
            suffix << std::setw(8) << static_cast<std::uint32_t>(part);
        suffix << '_' << identity;
        return suffix.str();
    }

    /** Encloses all box corners even when the native float sphere radius needs upward rounding. */
    bool PrivateStreamingDefinition::SetStreamingRegion(const ITerrainDataProvider::Bounds& bounds)
    {
        if (!bounds.IsValid())
            return false;
        const auto& minimum = bounds.Minimum;
        const auto& maximum = bounds.Maximum;
        SourcePosition = {minimum.X * 0.5 + maximum.X * 0.5,
            minimum.Y * 0.5 + maximum.Y * 0.5, minimum.Z * 0.5 + maximum.Z * 0.5};
        const double extentX = (std::max)(maximum.X - SourcePosition[0], SourcePosition[0] - minimum.X);
        const double extentY = (std::max)(maximum.Y - SourcePosition[1], SourcePosition[1] - minimum.Y);
        const double extentZ = (std::max)(maximum.Z - SourcePosition[2], SourcePosition[2] - minimum.Z);
        const double radius = std::hypot(extentX, extentY, extentZ);
        if (!std::isfinite(radius) || radius > (std::numeric_limits<float>::max)())
            return false;
        // One outward native ULP also covers rounding in the double-precision distance calculation.
        Radius = std::nextafter(static_cast<float>(radius), (std::numeric_limits<float>::infinity)());
        return std::isfinite(Radius) && Radius > 0;
    }

    /** Uses the engine's root/cell original-path-plus-same-suffix mapping, not the root's suffixed folder. */
    bool PrivateStreamingDefinition::SetPackages(const ITerrainDataProvider::CaptureDefinition& definition,
        const std::string& suffix)
    {
        if (!IsPackagePath(definition.OriginalWorldPackage) || definition.CookedCellPackages.empty())
            return false;
        const auto source = ResolveName(definition.OriginalWorldPackage);
        const auto destination = ResolveName(definition.OriginalWorldPackage + suffix);
        if (!source || !destination)
            return false;
        SourcePackage = *source;
        DestinationPackage = *destination;
        const auto generatedFolder = definition.OriginalWorldPackage + "/_Generated_/";
        for (const auto& package : definition.CookedCellPackages)
        {
            if (!IsPackagePath(package) || !package.starts_with(generatedFolder) ||
                package.find('/', generatedFolder.size()) != std::string::npos)
                return false;
            const auto original = ResolveName(package);
            const auto instance = ResolveName(package + suffix);
            if (!original || !instance)
                return false;
            RequiredCells.push_back({*original, *instance});
        }
        return true;
    }

    /** Resolves an explicit asset object identity, leaving instance lookup to the loaded private manager. */
    std::optional<StreamingDataLayer> PrivateStreamingDefinition::ResolveDataLayerAsset(const std::string& path)
    {
        const auto separator = path.find('.');
        if (separator == std::string::npos || !IsPackagePath(path.substr(0, separator)))
            return std::nullopt;
        const auto object = path.substr(separator + 1);
        if (object.empty() || object.find_first_of("./\\: \t\r\n\"'|?*<>%") != std::string::npos)
            return std::nullopt;
        const auto packageName = ResolveName(path.substr(0, separator));
        const auto objectName = ResolveName(object);
        if (!packageName || !objectName)
            return std::nullopt;
        return StreamingDataLayer{*packageName, *objectName, 2};
    }

    /** Converts residency inputs only, deliberately ignoring output identity, capture geometry and pixel settings. */
    std::optional<PrivateStreamingDefinition> PrivateStreamingDefinition::FromCaptureDefinition(
        const ITerrainDataProvider::CaptureDefinition& definition)
    {
        const auto* api = MiniMapNative::Get();
        if (!api || !api->captureAdmission.IsComplete() || !api->player.isGameThread() ||
            api->world.isGarbageCollectingAndLocking() || api->world.isIncrementalPurgePending() ||
            api->world.isIncrementalReachabilityPending() || !api->asset.exitPurge || *api->asset.exitPurge ||
            definition.TargetGrids.empty())
            return std::nullopt;
        PrivateStreamingDefinition converted;
        if (!converted.SetStreamingRegion(definition.StreamingBounds) ||
            !converted.SetPackages(definition, CreateInstanceSuffix()))
            return std::nullopt;
        for (const auto& grid : definition.TargetGrids)
        {
            const auto name = ResolveName(grid);
            if (!name)
                return std::nullopt;
            converted.TargetGrids.push_back(*name);
        }
        for (const auto& asset : definition.ActivatedDataLayerAssets)
        {
            const auto layer = ResolveDataLayerAsset(asset);
            if (!layer)
                return std::nullopt;
            converted.DataLayers.push_back(*layer);
        }
        return converted;
    }
}
#endif
