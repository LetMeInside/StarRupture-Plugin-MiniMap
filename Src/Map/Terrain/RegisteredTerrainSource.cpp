// Maintains registered terrain selection and lazily decodes embedded FE floor resources.
#include "RegisteredTerrainSource.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainSource.h"
#include "TerrainReconciliation.h"
#include "Map/Map.h"
#include "Services/Map/Buildings/Cache/SpritePngCodec.h"
#include "SDK/CoreUObject_structs.hpp"
#include "resource.h"
#include "plugin_helpers.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace MiniMapTerrainSource
{
    namespace
    {
        enum class EmbeddedFloorState { Unloaded, Ready, Failed };

        struct EmbeddedFloorResource
        {
            double ElevationCm;
            int ResourceId;
            const char* SourceId;
            const char* FloorId;
            EmbeddedFloorState State = EmbeddedFloorState::Unloaded;
            RegisteredTerrainImage Image;
        };

        std::array<EmbeddedFloorResource, 3> g_embeddedFloors{{
            {5074.529, IDR_FE_FLOOR_CANDIDATE_5074, "ForgottenEngine.CandidateZ5074.529cm",
                "CandidateZ5074.529cm"},
            {6274.529, IDR_FE_FLOOR_CANDIDATE_6274, "ForgottenEngine.CandidateZ6274.529cm",
                "CandidateZ6274.529cm"},
            {7474.529, IDR_FE_FLOOR_CANDIDATE_7474, "ForgottenEngine.CandidateZ7474.529cm",
                "CandidateZ7474.529cm"}
        }};

        std::atomic<int> g_selectedForgottenEngineFloor{-1};
        int g_embeddedResourceModuleAnchor = 0;
        std::optional<RegisteredTerrainImage> g_image;
        uint64_t g_selectionRevision = 0;
        uint64_t g_selectionWorld = 0;
    }

    std::optional<RegisteredTerrainImage> RegisteredTerrainSource::Snapshot()
    {
        return g_image;
    }

    uint64_t RegisteredTerrainSource::Revision()
    {
        return g_selectionRevision;
    }

    void RegisteredTerrainSource::Clear()
    {
        g_selectedForgottenEngineFloor.store(-1, std::memory_order_release);
        if (!g_image)
        {
            return;
        }
        g_image.reset();
        ++g_selectionRevision;
        g_selectionWorld = 0;
        TerrainReconciliation::WithdrawSelection();
    }

    bool RegisteredTerrainSource::SynchronizeWorld()
    {
        if (!g_image || g_selectionWorld == MiniMapMap::GetWorldGeneration()) return false;
        Clear();
        return true;
    }

    bool RegisteredTerrainSource::Resolve(const SDK::FVector& position, SourceTile& tile)
    {
        tile = {};
        if (!g_image) return TryResolveSourceTile(position, tile);
        const auto image = *g_image;
        if (g_selectionWorld != MiniMapMap::GetWorldGeneration() ||
            !image.ContainsWorld({position.X, position.Y})) return false;
        RegisteredTerrainImage::Point uv;
        if (!image.WorldToImage({position.X, position.Y}, uv)) return false;
        tile.GridX = image.Reference.GridX;
        tile.GridY = image.Reference.GridY;
        tile.RadiationLevel = image.Reference.RadiationLevel;
        tile.SelectedVariant = static_cast<Variant>(image.Reference.Variant);
        tile.LocalU = uv.X;
        tile.LocalV = uv.Y;
        tile.RegisteredImage = image;
        return true;
    }

    bool RegisteredTerrainSource::ResolveTexture(const RegisteredTerrainImage& image, SourceTile& tile)
    {
        tile = {};
        if (!image.IsValid() || image.Type != RegisteredTerrainImage::SourceType::GameNativeBC1) return false;
        SDK::FVector position{};
        position.X = image.Reference.SampleWorld.X;
        position.Y = image.Reference.SampleWorld.Y;
        if (!TryResolveSourceTile(position, tile)) return false;
        if (tile.GridX != image.Reference.GridX || tile.GridY != image.Reference.GridY ||
            tile.RadiationLevel != image.Reference.RadiationLevel ||
            static_cast<int>(tile.SelectedVariant) != image.Reference.Variant)
        {
            tile = {};
            return false;
        }
        tile.RegisteredImage = image;
        return tile.Texture != nullptr;
    }

    bool RegisteredTerrainSource::Select(const RegisteredTerrainImage& image)
    {
        if (!image.IsValid() || !MiniMapMap::GameplayReady()) return false;
        if (g_image && g_selectionWorld == MiniMapMap::GetWorldGeneration() &&
            g_image->ResidentIdentity() == image.ResidentIdentity()) return true;
        g_selectedForgottenEngineFloor.store(-1, std::memory_order_release);
        g_image = image;
        g_selectionWorld = MiniMapMap::GetWorldGeneration();
        ++g_selectionRevision;
        TerrainReconciliation::WithdrawSelection();
        return true;
    }

    bool RegisteredTerrainSource::SynchronizeForgottenEngine(bool inForgottenEngine,
        double characterWorldZ)
    {
        if (!inForgottenEngine)
        {
            if (SelectedForgottenEngineFloor() >= 0) Clear();
            return false;
        }
        const int selectedFloor = SelectedForgottenEngineFloor();
        const int floorIndex = ClassifyForgottenEngineFloor(characterWorldZ);
        if (floorIndex < 0)
        {
            if (selectedFloor >= 0) Clear();
            return false;
        }
        if (floorIndex == selectedFloor && g_image) return true;
        g_selectedForgottenEngineFloor.store(-1, std::memory_order_release);
        RegisteredTerrainImage image;
        if (!LoadForgottenEngineImage(floorIndex, image) || !Select(image))
        {
            if (selectedFloor >= 0) Clear();
            return false;
        }
        g_selectedForgottenEngineFloor.store(floorIndex, std::memory_order_release);
        return true;
    }

    int RegisteredTerrainSource::ClassifyForgottenEngineFloor(double worldZ)
    {
        if (!std::isfinite(worldZ)) return -1;
        const double lowerBoundary =
            (g_embeddedFloors[0].ElevationCm + g_embeddedFloors[1].ElevationCm) * 0.5;
        const double upperBoundary =
            (g_embeddedFloors[1].ElevationCm + g_embeddedFloors[2].ElevationCm) * 0.5;
        if (worldZ < lowerBoundary) return 0;
        if (worldZ < upperBoundary) return 1;
        return 2;
    }

    int RegisteredTerrainSource::SelectedForgottenEngineFloor()
    {
        const int selectedFloor =
            g_selectedForgottenEngineFloor.load(std::memory_order_acquire);
        if (selectedFloor < 0 || selectedFloor >= static_cast<int>(g_embeddedFloors.size())) return -1;
        return selectedFloor;
    }

    bool RegisteredTerrainSource::LoadForgottenEngineImage(int floorIndex,
        RegisteredTerrainImage& image)
    {
        if (floorIndex < 0 || floorIndex >= static_cast<int>(g_embeddedFloors.size())) return false;
        auto& resource = g_embeddedFloors[floorIndex];
        if (resource.State == EmbeddedFloorState::Ready)
        {
            image = resource.Image;
            return true;
        }
        if (resource.State == EmbeddedFloorState::Failed) return false;
        resource.State = EmbeddedFloorState::Failed;
        try
        {
            if (!DecodeForgottenEngineResource(floorIndex, resource.Image)) return false;
        }
        catch (...)
        {
            FailForgottenEngineResource(floorIndex, "embedded PNG decode or allocation failed");
            return false;
        }
        resource.State = EmbeddedFloorState::Ready;
        image = resource.Image;
        return true;
    }

    bool RegisteredTerrainSource::DecodeForgottenEngineResource(int floorIndex,
        RegisteredTerrainImage& image)
    {
        auto& resource = g_embeddedFloors[floorIndex];
        HMODULE module = nullptr;
        const DWORD moduleFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (!GetModuleHandleExW(moduleFlags,
            reinterpret_cast<LPCWSTR>(&g_embeddedResourceModuleAnchor), &module))
        {
            FailForgottenEngineResource(floorIndex, "plugin module lookup failed");
            return false;
        }
        const HRSRC resourceInfo = FindResourceW(module, MAKEINTRESOURCEW(resource.ResourceId), RT_RCDATA);
        if (!resourceInfo)
        {
            FailForgottenEngineResource(floorIndex, "embedded RCDATA is missing");
            return false;
        }
        const DWORD resourceSize = SizeofResource(module, resourceInfo);
        const HGLOBAL loadedResource = LoadResource(module, resourceInfo);
        const auto* pngBytes = loadedResource ? static_cast<const uint8_t*>(LockResource(loadedResource)) : nullptr;
        if (!pngBytes || resourceSize == 0)
        {
            FailForgottenEngineResource(floorIndex, "embedded RCDATA is missing or empty");
            return false;
        }
        return DecodeForgottenEnginePng(floorIndex, pngBytes, resourceSize, image);
    }

    bool RegisteredTerrainSource::DecodeForgottenEnginePng(int floorIndex, const uint8_t* pngBytes,
        uint32_t pngByteCount, RegisteredTerrainImage& image)
    {
        std::vector<uint8_t> rgba;
        const std::span<const uint8_t> png(pngBytes, pngByteCount);
        const auto result = ::Services::Map::Buildings::Cache::SpritePngCodec::Decode(png, 1800, 1800, rgba);
        if (result.Status != ::Services::Map::Buildings::Cache::SpritePngCodec::ResultStatus::Complete ||
            rgba.size() != 1800u * 1800u * 4u)
        {
            FailForgottenEngineResource(floorIndex, "embedded PNG decode or RGBA size validation failed");
            return false;
        }
        const auto& resource = g_embeddedFloors[floorIndex];
        RegisteredTerrainImage decoded;
        decoded.SourceId = resource.SourceId;
        decoded.ContentRevision = "Final4.StructuralGeometry.v1";
        decoded.EnvironmentId = "ForgottenEngine";
        decoded.FloorId = resource.FloorId;
        decoded.Type = RegisteredTerrainImage::SourceType::CpuOwnedPixels;
        decoded.WidthPixels = 1800;
        decoded.HeightPixels = 1800;
        decoded.PixelFormat = RegisteredTerrainImage::Format::RGBA8;
        decoded.ByteCount = rgba.size();
        decoded.RowStrideBytes = 1800u * 4u;
        decoded.SRGB = true;
        decoded.WorldOrigin = {-999.0343550357065, 452279.79522340785};
        decoded.WorldU = {21749.973382667064 - (-999.0343550357065), 0.0};
        decoded.WorldV = {0.0, 429530.78748570505 - 452279.79522340785};
        decoded.CpuPixels = std::make_shared<const CpuTerrainImagePixels>(std::move(rgba));
        if (!decoded.IsValid())
        {
            FailForgottenEngineResource(floorIndex, "compiled registration or image metadata is invalid");
            return false;
        }
        image = std::move(decoded);
        return true;
    }

    void RegisteredTerrainSource::FailForgottenEngineResource(int floorIndex, const char* reason)
    {
        const auto& resource = g_embeddedFloors[floorIndex];
        LOG_ERROR("MiniMap: Forgotten Engine floor candidate Z=%.3f cm disabled: %s",
            resource.ElevationCm, reason);
    }
}
#endif
