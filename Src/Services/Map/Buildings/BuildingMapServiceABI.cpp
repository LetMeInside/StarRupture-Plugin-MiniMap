// Adapts the existing CPU-image service to a polling-only C ABI. All C++ objects,
// native collector resources and temporary image borrows remain inside MiniMap.
#include "BuildingMapServiceABI.h"

#if defined(MODLOADER_CLIENT_BUILD)
#include "Map/Buildings/BuildingCollectorV2.h"
#include "Map/Map.h"
#include "Native/NativeApi.h"
#include "Services/Map/Buildings/BuildingMapService.h"
#include "Services/Map/Buildings/BuildingMapViewport.h"
#include "Services/Map/Buildings/BuildingMapSection.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>

static_assert(sizeof(void*) == 8, "BuildingMapServiceABI requires Windows x64");
static_assert(sizeof(BuildingMapABIState) == 40);
static_assert(sizeof(BuildingMapABISectionImage) == 72);
static_assert(sizeof(BuildingMapServiceABITable) == 72);
static_assert(offsetof(BuildingMapServiceABITable, QueryState) == 16);
static_assert(offsetof(BuildingMapABISectionImage, ByteCount) == 64);

namespace Services::Map::Buildings
{
    /** Retains one external association; the base interface owns the concrete collector.
     * Native shutdown must precede collector destruction. Handles share one monotonic
     * sequence so neither old sessions nor collector/viewport mixups can alias a new owner.
     */
    struct BuildingMapServiceABI::State
    {
        ::Services::Map::Buildings::BuildingMapService* Service = nullptr;
        std::unique_ptr<::Services::Map::Buildings::IBuildingCollector> Collector;
        std::unique_ptr<::Services::Map::Buildings::BuildingMapViewport> Viewport;
        uint64_t CollectorHandle = 0;
        uint64_t ViewportHandle = 0;
        uint64_t NextHandle = 1;
    };

    /** Returns storage with no allocation, registrations or acquisition until requested. */
    BuildingMapServiceABI::State& BuildingMapServiceABI::GetState() noexcept
    {
        static State state;
        return state;
    }

    /** Borrows the existing plugin service after successful startup. */
    void BuildingMapServiceABI::BindService(::Services::Map::Buildings::BuildingMapService& service) noexcept
    {
        GetState().Service = &service;
    }

    /** Retires provider-local ownership without dispatching anything into another plugin. */
    bool BuildingMapServiceABI::EndGameSession() noexcept
    {
        auto& state = GetState();
        state.CollectorHandle = 0;
        state.ViewportHandle = 0;
        try
        {
            ReleaseResources();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    /** Stops future service access before closing the external association. */
    bool BuildingMapServiceABI::Shutdown() noexcept
    {
        GetState().Service = nullptr;
        return EndGameSession();
    }

    /** Mirrors existing viewport-before-collector native shutdown ordering. */
    void BuildingMapServiceABI::ReleaseResources()
    {
        auto& state = GetState();
        if (state.Viewport != nullptr)
        {
            state.Viewport->Close();
            state.Viewport.reset();
        }
        if (state.Collector != nullptr)
        {
            // Only CreateCollector constructs this provider-local implementation.
            auto& collector = static_cast<::MiniMap::BuildingAcquisition::BuildingCollectorV2&>(*state.Collector);
            collector.ShutdownResources();
            state.Collector.reset();
        }
    }

    /** Uses only the existing native typed predicate; performs no memory probing. */
    BuildingMapABIResult BuildingMapServiceABI::CheckThread() noexcept
    {
        if (GetState().Service == nullptr)
            return BUILDING_MAP_ABI_UNAVAILABLE;
        const auto* native = MiniMapNative::Get();
        if (native == nullptr || native->player.isGameThread == nullptr)
            return BUILDING_MAP_ABI_UNAVAILABLE;
        return native->player.isGameThread() ? BUILDING_MAP_ABI_OK : BUILDING_MAP_ABI_WRONG_THREAD;
    }

    /** Reuses gameplay admission and public service dimensions; never starts a session. */
    BuildingMapABIResult BuildingMapServiceABI::CheckSession() noexcept
    {
        const auto result = CheckThread();
        if (result != BUILDING_MAP_ABI_OK)
            return result;
        const auto& service = *GetState().Service;
        if (!MiniMapMap::GameplayReady() || service.GetMapWidthMapUnits() == 0.0 ||
            service.GetMapHeightMapUnits() == 0.0)
            return BUILDING_MAP_ABI_NO_SESSION;
        return BUILDING_MAP_ABI_OK;
    }

    /** Preserves service validation failures as argument errors; other exceptions are failures. */
    BuildingMapABIResult BuildingMapServiceABI::CurrentExceptionResult() noexcept
    {
        try
        {
            throw;
        }
        catch (const std::invalid_argument&)
        {
            return BUILDING_MAP_ABI_INVALID_ARGUMENT;
        }
        catch (const std::overflow_error&)
        {
            return BUILDING_MAP_ABI_INVALID_ARGUMENT;
        }
        catch (...)
        {
            return BUILDING_MAP_ABI_FAILED;
        }
    }

    /** Copies only the exact current ABI table; discovery neither creates nor advances resources. */
    BuildingMapABIResult BuildingMapServiceABI::GetABI(
        uint32_t version, uint32_t tableSize, BuildingMapServiceABITable* table) noexcept
    {
        if (table == nullptr || version != BUILDING_MAP_ABI_VERSION || tableSize != sizeof(*table))
            return BUILDING_MAP_ABI_INVALID_ARGUMENT;
        *table = {};
        table->StructSize = sizeof(*table);
        table->Version = BUILDING_MAP_ABI_VERSION;
        table->ContractId = BUILDING_MAP_ABI_CONTRACT_ID;
        table->QueryState = &QueryState;
        table->CreateCollector = &CreateCollector;
        table->DisposeCollector = &DisposeCollector;
        table->OpenViewport = &OpenViewport;
        table->UpdateViewport = &UpdateViewport;
        table->CopySectionImage = &CopySectionImage;
        table->CloseViewport = &CloseViewport;
        return BUILDING_MAP_ABI_OK;
    }

    /** Reports current provider/session state and the supplied handles without scheduling work. */
    BuildingMapABIResult BUILDING_MAP_ABI_CALL BuildingMapServiceABI::QueryState(
        BuildingMapCollectorHandle collector, BuildingMapViewportHandle viewport, BuildingMapABIState* output) noexcept
    {
        if (output == nullptr || output->StructSize != sizeof(*output))
            return BUILDING_MAP_ABI_INVALID_ARGUMENT;
        *output = {};
        output->StructSize = sizeof(*output);
        const auto& state = GetState();
        if (state.Service == nullptr)
            return BUILDING_MAP_ABI_OK;
        const auto result = CheckThread();
        if (result != BUILDING_MAP_ABI_OK)
            return result;
        output->ServiceAvailable = 1;
        output->SessionAvailable = CheckSession() == BUILDING_MAP_ABI_OK;
        output->CollectorValid = collector != 0 && collector == state.CollectorHandle;
        output->ViewportValid = viewport != 0 && viewport == state.ViewportHandle;
        if (output->ViewportValid)
        {
            output->VisibleGridWidth = state.Viewport->GetVisibleGridWidth();
            output->VisibleGridHeight = state.Viewport->GetVisibleGridHeight();
        }
        return BUILDING_MAP_ABI_OK;
    }

    /** Constructs one initialized implementation owned through IBuildingCollector inside MiniMap. */
    BuildingMapABIResult BUILDING_MAP_ABI_CALL BuildingMapServiceABI::CreateCollector(BuildingMapCollectorHandle* output) noexcept
    {
        if (output == nullptr)
            return BUILDING_MAP_ABI_INVALID_ARGUMENT;
        *output = 0;
        const auto result = CheckSession();
        if (result != BUILDING_MAP_ABI_OK)
            return result;
        auto& state = GetState();
        if (state.Collector != nullptr)
            return BUILDING_MAP_ABI_BUSY;
        if (state.NextHandle == 0)
            return BUILDING_MAP_ABI_FAILED;
        try
        {
            state.Collector = std::make_unique<::MiniMap::BuildingAcquisition::BuildingCollectorV2>(state.Service->GetSectionSizeMapUnits());
            auto& collector = static_cast<::MiniMap::BuildingAcquisition::BuildingCollectorV2&>(*state.Collector);
            if (!collector.InitializeResources())
            {
                ReleaseResources();
                return BUILDING_MAP_ABI_UNAVAILABLE;
            }
            state.CollectorHandle = state.NextHandle++;
            *output = state.CollectorHandle;
            return BUILDING_MAP_ABI_OK;
        }
        catch (...)
        {
            const auto failure = CurrentExceptionResult();
            EndGameSession();
            return failure;
        }
    }

    /** Rejects associated collectors so the viewport always closes first. */
    BuildingMapABIResult BUILDING_MAP_ABI_CALL BuildingMapServiceABI::DisposeCollector(BuildingMapCollectorHandle collector) noexcept
    {
        const auto result = CheckThread();
        if (result != BUILDING_MAP_ABI_OK)
            return result;
        const auto& state = GetState();
        if (collector == 0 || collector != state.CollectorHandle)
            return BUILDING_MAP_ABI_INVALID_HANDLE;
        if (state.Viewport != nullptr)
            return BUILDING_MAP_ABI_BUSY;
        return EndGameSession() ? BUILDING_MAP_ABI_OK : BUILDING_MAP_ABI_FAILED;
    }

    /** Forwards creation coordinates without interpreting or converting the service's map units. */
    BuildingMapABIResult BUILDING_MAP_ABI_CALL BuildingMapServiceABI::OpenViewport(
        BuildingMapCollectorHandle collector, double xMapUnits, double yMapUnits,
        double widthMapUnits, double heightMapUnits, BuildingMapViewportHandle* output) noexcept
    {
        if (output == nullptr)
            return BUILDING_MAP_ABI_INVALID_ARGUMENT;
        *output = 0;
        const auto result = CheckSession();
        if (result != BUILDING_MAP_ABI_OK)
            return result;
        auto& state = GetState();
        if (collector == 0 || collector != state.CollectorHandle)
            return BUILDING_MAP_ABI_INVALID_HANDLE;
        if (state.Viewport != nullptr)
            return BUILDING_MAP_ABI_BUSY;
        if (state.NextHandle == 0)
            return BUILDING_MAP_ABI_FAILED;
        try
        {
            state.Viewport = state.Service->OpenViewport(
                state.Collector.get(), xMapUnits, yMapUnits, widthMapUnits, heightMapUnits);
            if (state.Viewport == nullptr)
                return BUILDING_MAP_ABI_FAILED;
            state.ViewportHandle = state.NextHandle++;
            *output = state.ViewportHandle;
            return BUILDING_MAP_ABI_OK;
        }
        catch (...)
        {
            return CurrentExceptionResult();
        }
    }

    /** Executes exactly one existing Update, even when the rectangle is unchanged. */
    BuildingMapABIResult BUILDING_MAP_ABI_CALL BuildingMapServiceABI::UpdateViewport(
        BuildingMapViewportHandle viewport,
        double xMapUnits,
        double yMapUnits,
        double widthMapUnits,
        double heightMapUnits) noexcept
    {
        const auto result = CheckSession();
        if (result != BUILDING_MAP_ABI_OK)
            return result;
        auto& state = GetState();
        if (viewport == 0 || viewport != state.ViewportHandle)
            return BUILDING_MAP_ABI_INVALID_HANDLE;
        try
        {
            return state.Viewport->Update(
                xMapUnits, yMapUnits, widthMapUnits, heightMapUnits)
                ? BUILDING_MAP_ABI_OK
                : BUILDING_MAP_ABI_FAILED;
        }
        catch (...)
        {
            return CurrentExceptionResult();
        }
    }

    /** Bounds the borrow to this call and leaves all copied storage owned by the consumer. */
    BuildingMapABIResult BUILDING_MAP_ABI_CALL BuildingMapServiceABI::CopySectionImage(
        BuildingMapViewportHandle viewport, uint64_t localX, uint64_t localY,
        BuildingMapABISectionImage* image, uint8_t* buffer, uint64_t capacity) noexcept
    {
        if (image == nullptr || image->StructSize != sizeof(*image) || (buffer == nullptr && capacity != 0))
            return BUILDING_MAP_ABI_INVALID_ARGUMENT;
        *image = {};
        image->StructSize = sizeof(*image);
        const auto result = CheckSession();
        if (result != BUILDING_MAP_ABI_OK)
            return result;
        const auto& state = GetState();
        if (viewport == 0 || viewport != state.ViewportHandle)
            return BUILDING_MAP_ABI_INVALID_HANDLE;
        try
        {
            return CopyCurrentSection(localX, localY, *image, buffer, capacity);
        }
        catch (...)
        {
            return CurrentExceptionResult();
        }
    }

    /** Reads every cell directly, including removals that never generated an image-change event. */
    BuildingMapABIResult BuildingMapServiceABI::CopyCurrentSection(
        uint64_t localX, uint64_t localY, BuildingMapABISectionImage& image, uint8_t* buffer, uint64_t capacity)
    {
        const auto& state = GetState();
        if (!state.Viewport->GetWorldGridCoordinate(localX, localY, image.GridX, image.GridY))
            return BUILDING_MAP_ABI_INVALID_ARGUMENT;
        image.SectionSizeMapUnits = state.Service->GetSectionSizeMapUnits();
        image.XMapUnits = static_cast<double>(image.GridX) * image.SectionSizeMapUnits;
        image.YMapUnits = static_cast<double>(image.GridY) * image.SectionSizeMapUnits;
        const auto* section = state.Viewport->GetSectionAt(localX, localY);
        ::Services::Map::Buildings::BuildingMapSection::CompletedImageView borrowed;
        if (section == nullptr || !section->TryBorrowCompletedImage(borrowed))
            return BUILDING_MAP_ABI_NO_IMAGE;
        image.Width = borrowed.PixelWidth;
        image.Height = borrowed.PixelHeight;
        image.RowStride = uint64_t(image.Width) * 4;
        if (image.Width == 0 || image.Height == 0 || image.RowStride > UINT64_MAX / image.Height)
            return BUILDING_MAP_ABI_FAILED;
        image.ByteCount = image.RowStride * image.Height;
        if (image.ByteCount != borrowed.Rgba8Bytes.size())
            return BUILDING_MAP_ABI_FAILED;
        if (capacity < image.ByteCount)
            return BUILDING_MAP_ABI_BUFFER_TOO_SMALL;
        std::memcpy(buffer, borrowed.Rgba8Bytes.data(), static_cast<size_t>(image.ByteCount));
        return BUILDING_MAP_ABI_OK;
    }

    /** Closes only the matching owner; old handles cannot release a newer association. */
    BuildingMapABIResult BUILDING_MAP_ABI_CALL BuildingMapServiceABI::CloseViewport(BuildingMapViewportHandle viewport) noexcept
    {
        const auto result = CheckThread();
        if (result != BUILDING_MAP_ABI_OK)
            return result;
        if (viewport == 0 || viewport != GetState().ViewportHandle)
            return BUILDING_MAP_ABI_INVALID_HANDLE;
        return EndGameSession() ? BUILDING_MAP_ABI_OK : BUILDING_MAP_ABI_FAILED;
    }
}

/** Exposes a C-compatible table to GetProcAddress without exposing any implementation object. */
extern "C" __declspec(dllexport) BuildingMapABIResult BUILDING_MAP_ABI_CALL MiniMap_GetBuildingMapServiceABI(
    uint32_t version, uint32_t tableSize, BuildingMapServiceABITable* table)
{
    return Services::Map::Buildings::BuildingMapServiceABI::GetABI(version, tableSize, table);
}
#endif
