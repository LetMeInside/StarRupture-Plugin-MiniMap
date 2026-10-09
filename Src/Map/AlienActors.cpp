#if defined(MODLOADER_CLIENT_BUILD)

#include "AlienActors.h"
#include "FogOfWar.h"
#include "../Native/NativeApi.h"
#include "../plugin_helpers.h"
#include "SDK/MassEntity_structs.hpp"
#include "SDK/MassAIPrototypeEnemyRuntime_structs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace
{
    constexpr float kPollIntervalSeconds = 0.2f;
    constexpr auto kDiagnosticInterval = std::chrono::seconds(5);
    constexpr auto kSlowPollCooldown = std::chrono::seconds(5);
    constexpr double kSlowPollMilliseconds = 20.0;
    using DiagnosticClock = std::chrono::steady_clock;
    constexpr double kMovementMarginWorldUnits = 500.0; // 5 meters
    constexpr float kDotRadiusPixels = 2.5f;
    constexpr uint32_t kHostileColor = 0xF04040FFu; // ImGui ABGR
    constexpr std::size_t kMaximumRecords = 512;
    constexpr int32_t kMaximumReasonableHandleCount = 1000000;
    constexpr std::ptrdiff_t kManagerOffset = 0x38;
    constexpr std::ptrdiff_t kStateOffset = 0x01;
    constexpr std::ptrdiff_t kTranslationOffset = 0x20;
    constexpr std::size_t kQuerySize = 0x350;

    struct AlienRecord
    {
        uint64_t WorldGeneration = 0;
        MiniMapNative::MassEntityHandle Id = {};
        double WorldX = 0.0;
        double WorldY = 0.0;
        float Visibility = 0.0f;
    };

    IPluginSelf* g_alienActorsSelf = nullptr;
    bool g_initialized = false;
    bool g_experienceReady = false;
    bool g_queryConstructed = false;
    bool g_queryFailureLogged = false;
    bool g_wasTruncated = false;
    float g_pollAccumulator = 0.0f;
    DiagnosticClock::time_point g_nextDiagnostic = {};
    DiagnosticClock::time_point g_nextSlowPollWarning = {};
    double g_maxPollMilliseconds = 0.0;
    SDK::UWorld* g_queryWorld = nullptr;
    SDK::UMassEntitySubsystem* g_subsystem = nullptr;
    const void* g_manager = nullptr;
    alignas(0x10) std::array<std::byte, kQuerySize> g_queryStorage = {};

    std::mutex g_snapshotMutex;
    uint64_t g_worldGeneration = 0;
    double g_acquisitionRadius = 0.0;
    std::vector<AlienRecord> g_records;

    void ResetDiagnostics()
    {
        g_nextDiagnostic = DiagnosticClock::now() + kDiagnosticInterval;
        g_nextSlowPollWarning = {};
        g_maxPollMilliseconds = 0.0;
        g_wasTruncated = false;
    }

    struct PollMeasurement
    {
        DiagnosticClock::time_point Started = DiagnosticClock::now();
        int32_t Matching = -1; // No native handle query completed yet.
        std::size_t Active = 0;
        std::size_t Nearby = 0;
        std::size_t Visible = 0;
        std::size_t Retained = 0;
        bool Capped = false;

        ~PollMeasurement()
        {
            // Declared first in Poll, so timing includes publication, native
            // result-array freeing, and temporary snapshot cleanup on all exits.
            const auto finished = DiagnosticClock::now();
            const double milliseconds =
                std::chrono::duration<double, std::milli>(finished - Started).count();
            g_maxPollMilliseconds = (std::max)(g_maxPollMilliseconds, milliseconds);
            if (milliseconds > kSlowPollMilliseconds && finished >= g_nextSlowPollWarning)
            {
                LOG_WARN("MiniMap: AlienActors: slow poll=%.3fms matching=%d retained=%zu capped=%d",
                    milliseconds, Matching, Retained, Capped ? 1 : 0);
                g_nextSlowPollWarning = finished + kSlowPollCooldown;
            }
            if (finished >= g_nextDiagnostic)
            {
                LOG_INFO("MiniMap: AlienActors: matching=%d active=%zu nearby=%zu visible=%zu retained=%zu capped=%d poll=%.3fms max=%.3fms",
                    Matching, Active, Nearby, Visible, Retained, Capped ? 1 : 0,
                    milliseconds, g_maxPollMilliseconds);
                g_maxPollMilliseconds = 0.0;
                g_nextDiagnostic = finished + kDiagnosticInterval;
            }
        }
    };

    void ClearSnapshot()
    {
        std::scoped_lock lock(g_snapshotMutex);
        g_records.clear();
    }

    void DestroyQuery()
    {
        ResetDiagnostics();
        if (g_queryConstructed)
        {
            const auto* native = MiniMapNative::Get();
            if (native != nullptr && native->mass.queryDestruct != nullptr)
            {
                native->mass.queryDestruct(g_queryStorage.data());
            }
            g_queryStorage.fill(std::byte{ 0 });
            LOG_DEBUG("MiniMap: Alien Actors query destroyed");
        }
        g_queryConstructed = false;
        g_queryWorld = nullptr;
        g_subsystem = nullptr;
        g_manager = nullptr;
    }

    bool EnsureQuery(const MiniMapNative::NativeApi& native)
    {
        auto* world = MiniMapMap::GetWorld();
        if (world == nullptr)
        {
            DestroyQuery();
            return false;
        }
        auto* subsystem = native.mass.getMassEntitySubsystem(world);
        const void* manager = nullptr;
        if (subsystem != nullptr)
        {
            std::memcpy(&manager,
                reinterpret_cast<const uint8_t*>(subsystem) + kManagerOffset,
                sizeof(manager));
        }
        if (g_queryConstructed && world == g_queryWorld &&
            subsystem == g_subsystem && manager == g_manager)
        {
            return true;
        }
        DestroyQuery();
        {
            std::scoped_lock lock(g_snapshotMutex);
            g_records.clear();
            ++g_worldGeneration;
        }
        if (manager == nullptr)
        {
            return false;
        }
        const auto* enemyTag = native.mass.enemyTagStaticStruct();
        const auto* neutralTag = native.mass.neutralTagStaticStruct();
        if (enemyTag == nullptr || neutralTag == nullptr)
        {
            return false;
        }
        // Same shared-manager constructor and requirements base as Foundables.
        native.mass.queryConstruct(g_queryStorage.data(),
            reinterpret_cast<const uint8_t*>(subsystem) + kManagerOffset);
        g_queryConstructed = true;
        native.mass.addTransformRequirement(g_queryStorage.data(),
            static_cast<uint8_t>(SDK::EMassFragmentAccess::ReadOnly),
            static_cast<uint8_t>(SDK::EMassFragmentPresence::All));
        native.mass.addEnemyStateRequirement(g_queryStorage.data(),
            SDK::EMassFragmentAccess::ReadOnly, SDK::EMassFragmentPresence::All);
        native.mass.addTagRequirement(g_queryStorage.data(), enemyTag,
            SDK::EMassFragmentPresence::All);
        native.mass.addTagRequirement(g_queryStorage.data(), neutralTag,
            SDK::EMassFragmentPresence::None);
        g_queryWorld = world;
        g_subsystem = subsystem;
        g_manager = manager;
        LOG_DEBUG("MiniMap: Alien Actors query constructed");
        return true;
    }

    void Poll()
    {
        PollMeasurement measurement;
        const auto* native = MiniMapNative::Get();
        if (native == nullptr || !EnsureQuery(*native))
        {
            ClearSnapshot();
            if (!g_queryFailureLogged)
            {
                LOG_DEBUG("MiniMap: Alien Actors query unavailable; will retry");
                g_queryFailureLogged = true;
            }
            return;
        }
        g_queryFailureLogged = false;

        double radius = 0.0;
        uint64_t generation = 0;
        {
            std::scoped_lock lock(g_snapshotMutex);
            radius = g_acquisitionRadius;
            generation = g_worldGeneration;
        }
        MiniMapMap::PlayerPose pose = {};
        if (radius <= 0.0 || !MiniMapMap::TryGetPlayerPose(pose) ||
            !std::isfinite(pose.WorldX) || !std::isfinite(pose.WorldY))
        {
            ClearSnapshot();
            return;
        }
        auto* transformType = native->mass.transformFragmentStaticStruct();
        auto* stateType = native->mass.enemyStateFragmentStaticStruct();
        if (transformType == nullptr || stateType == nullptr)
        {
            ClearSnapshot();
            return;
        }

        MiniMapNative::MassEntityHandleArray handles = {};
        native->mass.getMatchingEntityHandles(g_queryStorage.data(), &handles);
        measurement.Matching = handles.Num;
        // The native TArray allocation must always be freed with the native allocator.
        struct HandleArrayOwner
        {
            void* Data;
            MiniMapNative::TextureApi::MemoryFreeFn Free;
            ~HandleArrayOwner() { if (Data != nullptr) Free(Data); }
        } owner{ handles.Data, native->texture.memoryFree };
        if (handles.Num < 0 || handles.Max < handles.Num ||
            handles.Num > kMaximumReasonableHandleCount ||
            (handles.Num != 0 && handles.Data == nullptr))
        {
            ClearSnapshot();
            LOG_DEBUG("MiniMap: Alien Actors received invalid handle array");
            return;
        }

        const double radiusSquared = radius * radius;
        MiniMapMap::Transform visibilityTransform = {};
        visibilityTransform.PlayerWorldX = pose.WorldX;
        visibilityTransform.PlayerWorldY = pose.WorldY;
        visibilityTransform.Valid = true;
        std::vector<AlienRecord> next;
        next.reserve((std::min)(static_cast<std::size_t>(handles.Num), kMaximumRecords));
        for (int32_t i = 0; i < handles.Num; ++i)
        {
            const auto id = handles.Data[i];
            const auto* state = static_cast<const uint8_t*>(
                native->mass.getFragmentDataPtr(g_manager, id, stateType));
            if (state == nullptr || state[kStateOffset] !=
                static_cast<uint8_t>(SDK::EMassEnemyInitializationState::Active))
            {
                continue;
            }
            ++measurement.Active;
            const auto* transform = static_cast<const uint8_t*>(
                native->mass.getFragmentDataPtr(g_manager, id, transformType));
            if (transform == nullptr)
            {
                continue;
            }
            AlienRecord record = {};
            record.WorldGeneration = generation;
            record.Id = id;
            std::memcpy(&record.WorldX, transform + kTranslationOffset, sizeof(double));
            std::memcpy(&record.WorldY, transform + kTranslationOffset + sizeof(double), sizeof(double));
            const double dx = record.WorldX - pose.WorldX;
            const double dy = record.WorldY - pose.WorldY;
            if (!std::isfinite(record.WorldX) || !std::isfinite(record.WorldY) ||
                dx * dx + dy * dy > radiusSquared)
            {
                continue;
            }
            ++measurement.Nearby;
            record.Visibility = MiniMapFogOfWar::GetRenderedVisibilityAtWorldPosition(
                record.WorldX, record.WorldY, visibilityTransform);
            if (record.Visibility <= 0.0f)
            {
                continue;
            }
            ++measurement.Visible;
            next.push_back(record);
        }
        const std::size_t eligible = next.size();
        const bool truncated = eligible > kMaximumRecords;
        measurement.Capped = truncated;
        const bool diagnosticDue = DiagnosticClock::now() >= g_nextDiagnostic;
        if (truncated)
        {
            // No ordering work for the usual <=512 case.
            std::nth_element(next.begin(), next.begin() + kMaximumRecords, next.end(),
                [&pose](const AlienRecord& a, const AlienRecord& b)
                {
                    const double ax = a.WorldX - pose.WorldX;
                    const double ay = a.WorldY - pose.WorldY;
                    const double bx = b.WorldX - pose.WorldX;
                    const double by = b.WorldY - pose.WorldY;
                    return ax * ax + ay * ay < bx * bx + by * by;
                });
            next.resize(kMaximumRecords);
            if (!g_wasTruncated || diagnosticDue)
            {
                LOG_WARN("MiniMap: Alien Actors cap hit: eligible=%zu retained=%zu (nearest)",
                    eligible, next.size());
            }
        }
        g_wasTruncated = truncated;
        {
            std::scoped_lock lock(g_snapshotMutex);
            if (generation == g_worldGeneration)
            {
                g_records = std::move(next);
                measurement.Retained = g_records.size();
            }
        }
    }

    void OnTick(float deltaSeconds)
    {
        if (!g_experienceReady || !MiniMapMap::HasWorld())
        {
            return;
        }
        g_pollAccumulator += deltaSeconds;
        if (g_pollAccumulator >= kPollIntervalSeconds)
        {
            g_pollAccumulator = 0.0f;
            Poll();
        }
    }
}

namespace MiniMapAlienActors
{
    bool Initialize(IPluginSelf* self)
    {
        if (g_initialized) return true;
        if (self == nullptr || self->hooks == nullptr || self->hooks->Engine == nullptr)
        {
            return false;
        }
        g_alienActorsSelf = self;
        Reset();
        // AlienX dispatches these callbacks after the original engine tick.
        self->hooks->Engine->RegisterOnTick(&OnTick);
        g_initialized = true;
        return true;
    }

    void Reset()
    {
        g_experienceReady = false;
        {
            std::scoped_lock lock(g_snapshotMutex);
            ++g_worldGeneration;
            g_records.clear();
            g_acquisitionRadius = 0.0;
        }
        DestroyQuery();
        g_pollAccumulator = 0.0f;
        g_queryFailureLogged = false;
        g_wasTruncated = false;
        LOG_DEBUG("MiniMap: Alien Actors reset");
    }

    void OnExperienceLoadComplete()
    {
        if (g_initialized)
        {
            g_experienceReady = true;
            g_pollAccumulator = 0.0f;
        }
    }

    void SetViewport(const MiniMapMap::Transform& transform)
    {
        // PixelsPerWorldUnit is pixels/centimeter; this is the viewport's
        // circumscribed world radius plus a small movement margin.
        double radius = 0.0;
        if (transform.Valid && transform.Width > 0.0f && transform.Height > 0.0f &&
            std::isfinite(transform.PixelsPerWorldUnit) && transform.PixelsPerWorldUnit > 0.0)
        {
            radius = 0.5 * std::hypot(transform.Width, transform.Height) /
                transform.PixelsPerWorldUnit + kMovementMarginWorldUnits;
        }
        std::scoped_lock lock(g_snapshotMutex);
        g_acquisitionRadius = std::isfinite(radius) ? radius : 0.0;
    }

    void Render(IModLoaderImGui* ui, const MiniMapMap::Transform& transform)
    {
        if (ui == nullptr || !transform.Valid) return;
        const auto drawList = ui->GetWindowDrawList();
        if (drawList == nullptr) return;
        const float left = transform.CenterX - transform.Width * 0.5f;
        const float top = transform.CenterY - transform.Height * 0.5f;
        const float right = left + transform.Width;
        const float bottom = top + transform.Height;

        // Hold the snapshot lock through drawing so a reset cannot invalidate
        // a copied old-world snapshot between the generation check and draw.
        std::scoped_lock lock(g_snapshotMutex);
        if (g_records.empty()) return;
        ui->DL_PushClipRect(drawList, left, top, right, bottom, true);
        for (const auto& record : g_records)
        {
            MiniMapMap::ScreenPoint point = {};
            if (record.WorldGeneration != g_worldGeneration ||
                !transform.WorldToScreen(record.WorldX, record.WorldY, point) ||
                point.X + kDotRadiusPixels < left || point.X - kDotRadiusPixels > right ||
                point.Y + kDotRadiusPixels < top || point.Y - kDotRadiusPixels > bottom)
            {
                continue;
            }
            const uint32_t alpha = static_cast<uint32_t>(std::lround(
                static_cast<float>(kHostileColor >> 24) * record.Visibility));
            if (alpha == 0) continue;
            const uint32_t color = (kHostileColor & 0x00FFFFFFu) | (alpha << 24);
            ui->DL_AddCircleFilled(drawList, point.X, point.Y, kDotRadiusPixels, color, 12);
        }
        ui->DL_PopClipRect(drawList);
    }

    void Shutdown()
    {
        if (!g_initialized) return;
        g_experienceReady = false;
        g_alienActorsSelf->hooks->Engine->UnregisterOnTick(&OnTick);
        Reset(); // Destruct while NativeApi is still available.
        g_alienActorsSelf = nullptr;
        g_initialized = false;
    }
}

#endif
