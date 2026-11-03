#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingComplexAbi.h"
#include "BuildingComplexWorld.h"
#include "Native/NativeApi.h"
#include "Native/ResidentMetadata.h"
#include "Map/Map.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <type_traits>

#define BC_LAYOUT(T, S) static_assert(sizeof(T)==S); static_assert(alignof(T)==8)
BC_LAYOUT(BC_Key,16); BC_LAYOUT(BC_Rect,32); BC_LAYOUT(BC_Ref,24); BC_LAYOUT(BC_Open,56);
BC_LAYOUT(BC_Grid,80); BC_LAYOUT(BC_Demand,64); BC_LAYOUT(BC_Status,192);
BC_LAYOUT(BC_Lease,40); BC_LAYOUT(BC_Policy,64); BC_LAYOUT(BC_Api,112);
#undef BC_LAYOUT
static_assert(offsetof(BC_Api,Open)==16 && offsetof(BC_Api,Service)==104);
static_assert(sizeof(void*)==8);

namespace
{
    struct Session
    {
        uint64_t Id = 0, GridId = 0;
        BC_World* World = nullptr;
        double OriginX = 0, OriginY = 0;
        bool Closing = false, Closed = false;
    };
    std::map<uint64_t, Session> Sessions;
    uint64_t NextSession = 1, NextGrid = 1;
    constexpr uint32_t Sides[] = {128, 256, 512, 1024};

    template<class T> bool Shape(T* value) noexcept
    {
        return value && reinterpret_cast<uintptr_t>(value) % alignof(T) == 0 && value->StructSize == sizeof(T);
    }
    template<class T> void Init(T* value) noexcept
    {
        std::memset(value, 0, sizeof(T));
        value->StructSize = sizeof(T);
    }
    bool OnGameThread() noexcept
    {
        const auto* native = MiniMapNative::Get();
        return native && native->player.isGameThread && native->player.isGameThread();
    }
    bool Admitted() noexcept { return OnGameThread() && MiniMapResidentMetadata::Available(); }
    Session* Find(uint64_t id) noexcept
    {
        const auto found = Sessions.find(id);
        return found == Sessions.end() ? nullptr : &found->second;
    }
    bool ValidRef(const BC_Ref* ref, bool needsId) noexcept
    {
        return ref && reinterpret_cast<uintptr_t>(ref) % alignof(BC_Ref) == 0 &&
            ref->StructSize == sizeof(BC_Ref) && ref->Reserved == 0 && ref->Session &&
            (needsId ? ref->Id != 0 : ref->Id == 0);
    }
    uint32_t SessionCheck(uint64_t id, Session*& session) noexcept
    {
        session = Find(id);
        if (!session) return BC_STALE_SESSION;
        if (session->Closed) return BC_CLOSED;
        if (session->Closing) return BC_BUSY;
        if (!MiniMapBuildingComplexWorld::OwnsCollectorSession(session->Id, session->World)) return BC_STALE_SESSION;
        return BC_RESULT_OK;
    }
    bool TileBounds(const Session& session, int64_t x, int64_t y, BC_Rect& bounds) noexcept
    {
        constexpr int64_t low = (std::numeric_limits<int64_t>::min)() / 4;
        constexpr int64_t high = ((std::numeric_limits<int64_t>::max)() - 3) / 4;
        if (x < low || x > high || y < low || y > high) return false;
        const long double minX = static_cast<long double>(session.OriginX) + static_cast<long double>(x) * 50000.0L;
        const long double minY = static_cast<long double>(session.OriginY) + static_cast<long double>(y) * 50000.0L;
        const long double maxX = minX + 50000.0L, maxY = minY + 50000.0L;
        bounds = {double(minX), double(minY), double(maxX), double(maxY)};
        return std::isfinite(minX) && std::isfinite(minY) && std::isfinite(maxX) && std::isfinite(maxY) &&
            std::isfinite(bounds.MinX) && std::isfinite(bounds.MinY) && std::isfinite(bounds.MaxX) &&
            std::isfinite(bounds.MaxY) && bounds.MinX < bounds.MaxX && bounds.MinY < bounds.MaxY;
    }
    void FillGrid(const Session& session, BC_Grid& grid) noexcept
    {
        grid.Version = BC_VERSION; grid.ContractId = BC_CONTRACT_ID;
        grid.Session = session.Id; grid.GridId = session.GridId;
        grid.OriginX = session.OriginX; grid.OriginY = session.OriginY;
        grid.CellSizeCm = 12500.0; grid.TileSizeCm = 50000.0;
        grid.CellPixels = 256; grid.GroupWidth = 4; grid.MaxInteriorSide = 1024;
        grid.PixelConvention = BC_PIXEL_CONVENTION;
    }
    uint32_t BC_CALL Open(const BC_Open* input, BC_Grid* output)
    {
        if (!Shape(output)) return BC_INVALID_ARGUMENT;
        Init(output);
        if (!input || reinterpret_cast<uintptr_t>(input) % alignof(BC_Open) || input->StructSize != sizeof(BC_Open)) return BC_INVALID_ARGUMENT;
        if (input->Reserved || !input->World || !std::isfinite(input->OriginX) || !std::isfinite(input->OriginY) ||
            !std::isfinite(input->CellSizeCm) || input->CellSizeCm != 12500.0 || input->CellPixels != 256 ||
            input->GroupWidth != 4 || input->GridSchema != BC_SCHEMA) return BC_INVALID_ARGUMENT;
        if (!Admitted()) return BC_REJECTED_THREAD_OR_ADMISSION;
        if (MiniMapMap::GetWorld() != input->World) return BC_UNAVAILABLE;
        for (const auto& [id, session] : Sessions) { (void)id; if (!session.Closed) return BC_BUSY; }
        if (!NextSession || NextSession == (std::numeric_limits<uint64_t>::max)() ||
            !NextGrid || NextGrid == (std::numeric_limits<uint64_t>::max)()) return BC_RESOURCE_FAILURE;
        try
        {
            Session session;
            session.Id = NextSession++; session.GridId = NextGrid++; session.World = input->World;
            session.OriginX = input->OriginX; session.OriginY = input->OriginY;
            auto [it, inserted] = Sessions.emplace(session.Id, session);
            if (!inserted) return BC_INTERNAL_FAILURE;
            if (!MiniMapBuildingComplexWorld::OpenCollectorSession(it->second.Id, it->second.World,
                it->second.OriginX, it->second.OriginY))
            {
                Sessions.erase(it);
                return BC_UNAVAILABLE;
            }
            FillGrid(it->second, *output);
            return BC_RESULT_OK;
        }
        catch (...) { return BC_RESOURCE_FAILURE; }
    }
    uint32_t BC_CALL InspectSession(const BC_Ref* input, BC_Grid* output)
    {
        if (!Shape(output)) return BC_INVALID_ARGUMENT;
        Init(output);
        if (!ValidRef(input, false)) return BC_INVALID_ARGUMENT;
        if (!Admitted()) return BC_REJECTED_THREAD_OR_ADMISSION;
        Session* session = nullptr;
        const uint32_t result = SessionCheck(input->Session, session);
        if (result != BC_RESULT_OK) return result;
        FillGrid(*session, *output);
        return BC_RESULT_OK;
    }
    uint32_t BC_CALL Submit(const BC_Demand* input, uint64_t* output)
    {
        if (!output || reinterpret_cast<uintptr_t>(output) % alignof(uint64_t)) return BC_INVALID_ARGUMENT;
        *output = 0;
        if (!input || reinterpret_cast<uintptr_t>(input) % alignof(BC_Demand) || input->StructSize != sizeof(BC_Demand) ||
            input->Reserved || input->Visible > 1 || !input->Session || !input->ClientDemandId || !input->GridId ||
            std::find(std::begin(Sides), std::end(Sides), input->InteriorSide) == std::end(Sides)) return BC_INVALID_ARGUMENT;
        if (!Admitted()) return BC_REJECTED_THREAD_OR_ADMISSION;
        Session* session = nullptr;
        const uint32_t result = SessionCheck(input->Session, session);
        if (result != BC_RESULT_OK) return result;
        if (input->GridId != session->GridId) return BC_INVALID_ARGUMENT;
        BC_Rect bounds{};
        if (!TileBounds(*session, input->TileX, input->TileY, bounds)) return BC_INVALID_ARGUMENT;
        return BC_UNAVAILABLE; // The image producer is intentionally disabled until the approved architecture is implemented.
    }
    uint32_t BC_CALL InspectUnavailable(const BC_Ref* input, BC_Status* output)
    {
        if (!Shape(output)) return BC_INVALID_ARGUMENT;
        Init(output);
        if (!ValidRef(input, true)) return BC_INVALID_ARGUMENT;
        if (!Admitted()) return BC_REJECTED_THREAD_OR_ADMISSION;
        Session* session = nullptr;
        const uint32_t result = SessionCheck(input->Session, session);
        if (result != BC_RESULT_OK) return result;
        output->State = BC_PARTIAL;
        output->Reason = BC_SOURCE_PENDING;
        output->GridId = session->GridId;
        return BC_UNAVAILABLE;
    }
    uint32_t BC_CALL InspectRequest(const BC_Ref* input, BC_Status* output) { return InspectUnavailable(input, output); }
    uint32_t BC_CALL InspectPublication(const BC_Ref* input, BC_Status* output) { return InspectUnavailable(input, output); }
    uint32_t BC_CALL Acquire(const BC_Ref* input, BC_Lease* output)
    {
        if (!Shape(output)) return BC_INVALID_ARGUMENT;
        Init(output);
        if (!ValidRef(input, true)) return BC_INVALID_ARGUMENT;
        if (!Admitted()) return BC_REJECTED_THREAD_OR_ADMISSION;
        Session* session = nullptr;
        const uint32_t result = SessionCheck(input->Session, session);
        return result == BC_RESULT_OK ? BC_UNAVAILABLE : result;
    }
    uint32_t BC_CALL Release(const BC_Ref* input)
    {
        if (!ValidRef(input, true)) return BC_INVALID_ARGUMENT;
        if (!Admitted()) return BC_REJECTED_THREAD_OR_ADMISSION;
        Session* session = nullptr;
        const uint32_t result = SessionCheck(input->Session, session);
        return result == BC_RESULT_OK ? BC_UNAVAILABLE : result;
    }
    uint32_t BC_CALL Withdraw(const BC_Ref* input)
    {
        if (!ValidRef(input, true)) return BC_INVALID_ARGUMENT;
        if (!Admitted()) return BC_REJECTED_THREAD_OR_ADMISSION;
        Session* session = nullptr;
        const uint32_t result = SessionCheck(input->Session, session);
        return result;
    }
    uint32_t BC_CALL WithdrawAll(const BC_Ref* input)
    {
        if (!ValidRef(input, false)) return BC_INVALID_ARGUMENT;
        if (!Admitted()) return BC_REJECTED_THREAD_OR_ADMISSION;
        Session* session = nullptr;
        const uint32_t result = SessionCheck(input->Session, session);
        return result;
    }
    uint32_t BC_CALL Close(const BC_Ref* input)
    {
        if (!ValidRef(input, false)) return BC_INVALID_ARGUMENT;
        if (!OnGameThread()) return BC_REJECTED_THREAD_OR_ADMISSION;
        Session* session = Find(input->Session);
        if (!session) return BC_STALE_SESSION;
        if (session->Closed) return BC_RESULT_OK;
        session->Closing = true;
        MiniMapBuildingComplexWorld::CloseCollectorSession(session->Id);
        session->Closed = true;
        session->Closing = false;
        return BC_RESULT_OK;
    }
    uint32_t BC_CALL GetPolicy(const BC_Ref* input, BC_Policy* output)
    {
        if (!Shape(output)) return BC_INVALID_ARGUMENT;
        Init(output);
        if (!ValidRef(input, false)) return BC_INVALID_ARGUMENT;
        if (!Admitted()) return BC_REJECTED_THREAD_OR_ADMISSION;
        Session* session = nullptr;
        const uint32_t result = SessionCheck(input->Session, session);
        if (result != BC_RESULT_OK) return result;
        output->PolicyId = 1; output->ClassCount = 4; output->Gutter = 1;
        std::copy(std::begin(Sides), std::end(Sides), output->Sides);
        output->DowngradeFraction = 0.40; output->MaxInteriorSide = 1024;
        return BC_RESULT_OK;
    }
    uint32_t BC_CALL Service(const BC_Ref* input)
    {
        if (!ValidRef(input, false)) return BC_INVALID_ARGUMENT;
        if (!Admitted()) return BC_REJECTED_THREAD_OR_ADMISSION;
        Session* session = nullptr;
        const uint32_t result = SessionCheck(input->Session, session);
        if (result != BC_RESULT_OK) return result;
        return BC_RESULT_OK;
    }
}

static_assert(std::is_same_v<decltype(&Open), BC_OpenFn>);
static_assert(std::is_same_v<decltype(&InspectSession), BC_InspectSessionFn>);
static_assert(std::is_same_v<decltype(&Submit), BC_SubmitFn>);
static_assert(std::is_same_v<decltype(&InspectRequest), BC_InspectRequestFn>);
static_assert(std::is_same_v<decltype(&InspectPublication), BC_InspectPublicationFn>);
static_assert(std::is_same_v<decltype(&Acquire), BC_AcquireFn>);
static_assert(std::is_same_v<decltype(&Release), BC_ReleaseFn>);
static_assert(std::is_same_v<decltype(&Withdraw), BC_WithdrawFn>);
static_assert(std::is_same_v<decltype(&WithdrawAll), BC_WithdrawAllFn>);
static_assert(std::is_same_v<decltype(&Close), BC_CloseFn>);
static_assert(std::is_same_v<decltype(&GetPolicy), BC_GetPolicyFn>);
static_assert(std::is_same_v<decltype(&Service), BC_ServiceFn>);

extern "C" MINIMAP_BUILDING_ABI_EXPORT uint32_t MINIMAP_BUILDING_ABI_CALL MiniMap_GetBuildingComplexApi(uint32_t requestedVersion, BC_Api* outApi)
{
    if (!outApi || reinterpret_cast<uintptr_t>(outApi) % alignof(BC_Api) || outApi->StructSize != sizeof(BC_Api))
        return BC_INVALID_ARGUMENT;
    std::memset(outApi, 0, sizeof(BC_Api));
    outApi->StructSize = sizeof(BC_Api);
    if (requestedVersion != BC_VERSION) return BC_VERSION_MISMATCH;
    outApi->Version = BC_VERSION; outApi->ContractId = BC_CONTRACT_ID;
    outApi->Open = &Open; outApi->InspectSession = &InspectSession; outApi->Submit = &Submit;
    outApi->InspectRequest = &InspectRequest; outApi->InspectPublication = &InspectPublication;
    outApi->Acquire = &Acquire; outApi->Release = &Release; outApi->Withdraw = &Withdraw;
    outApi->WithdrawAll = &WithdrawAll; outApi->Close = &Close; outApi->GetPolicy = &GetPolicy;
    outApi->Service = &Service;
    return BC_RESULT_OK;
}
#endif



