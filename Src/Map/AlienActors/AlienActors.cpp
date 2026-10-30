#include "Map/Map.h"
#if defined(MODLOADER_CLIENT_BUILD)

#include "AlienActors.h"
#include "DiagnosticExports.h"
#include "Config/Config.h"
#include "UI/MiniMapUI.h"
#include "Map/FogOfWar/FogOfWar.h"
#include "Native/NativeApi.h"
#include "plugin_helpers.h"
#include "Services/Runtime.h"
#include "Services/RuntimeEvents.h"
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
    MiniMap::Services::Registration g_tickRegistration{};
    constexpr float kPollIntervalSeconds = 0.2f;
    using PollClock = std::chrono::steady_clock;
    constexpr auto kCapWarningInterval = std::chrono::seconds(5);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    constexpr auto kDiagnosticInterval = std::chrono::seconds(5);
    constexpr auto kSlowPollCooldown = std::chrono::seconds(5);
    constexpr double kSlowPollMilliseconds = 20.0;
#endif
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
    };

    bool g_initialized = false;
    bool g_experienceReady = false;
    bool g_queryConstructed = false;
    bool g_queryFailureLogged = false;
    bool g_wasTruncated = false;
    float g_pollAccumulator = 0.0f;
    PollClock::time_point g_nextCapWarning = {};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    PollClock::time_point g_nextDiagnostic = {};
    PollClock::time_point g_nextSlowPollWarning = {};
    double g_maxPollMilliseconds = 0.0;
#endif
    SDK::UWorld* g_queryWorld = nullptr;
    SDK::UMassEntitySubsystem* g_subsystem = nullptr;
    const void* g_manager = nullptr;
    alignas(0x10) std::array<std::byte, kQuerySize> g_queryStorage = {};

    std::mutex g_snapshotMutex;
    uint64_t g_worldGeneration = 0;
    double g_acquisitionRadius = 0.0;
    std::vector<AlienRecord> g_records;

    void ResetPollReporting()
    {
        g_nextCapWarning = PollClock::now() + kCapWarningInterval;
        g_wasTruncated = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        g_nextDiagnostic = PollClock::now() + kDiagnosticInterval;
        g_nextSlowPollWarning = {};
        g_maxPollMilliseconds = 0.0;
#endif
    }

    // Production warning cadence only: preserve the old poll-exit advancement,
    // including early returns, without depending on performance measurements.
    struct PollCapacityCadence
    {
        ~PollCapacityCadence()
        {
            const auto finished = PollClock::now();
            if (finished >= g_nextCapWarning)
                g_nextCapWarning = finished + kCapWarningInterval;
        }
    };

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    struct PollMeasurement
    {
        PollClock::time_point Started = PollClock::now();
        int32_t Matching = -1; // No native handle query completed yet.
        std::size_t Active = 0;
        std::size_t Nearby = 0;
        std::size_t Visible = 0;
        std::size_t Retained = 0;
        bool Capped = false;

        ~PollMeasurement()
        {
            // Declared before functional Poll locals, so timing includes publication, native
            // result-array freeing, and temporary snapshot cleanup on all exits.
            const auto finished = PollClock::now();
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
#endif

    void ClearSnapshot()
    {
        std::scoped_lock lock(g_snapshotMutex);
        g_records.clear();
    }

    void DestroyQuery()
    {
        ResetPollReporting();
        if (g_queryConstructed)
        {
            const auto* native = MiniMapNative::Get();
            if (native != nullptr && native->mass.queryDestruct != nullptr)
            {
                native->mass.queryDestruct(g_queryStorage.data());
            }
            g_queryStorage.fill(std::byte{ 0 });
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            LOG_DEBUG("MiniMap: Alien Actors query destroyed");
#endif
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_DEBUG("MiniMap: Alien Actors query constructed");
#endif
        return true;
    }

    void Poll(const MiniMapConfig::FogOfWarSettings& fog)
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        PollMeasurement measurement;
#endif
        PollCapacityCadence capacityCadence;
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
        MiniMapMap::PresentationSample sample{};
        if (radius <= 0.0 || !MiniMapMap::TryGetPresentationSample(sample) ||
            !std::isfinite(sample.Tracking.WorldX) || !std::isfinite(sample.Tracking.WorldY))
        {
            ClearSnapshot();
            return;
        }
        const auto& pose = sample.Tracking;
        auto* transformType = native->mass.transformFragmentStaticStruct();
        auto* stateType = native->mass.enemyStateFragmentStaticStruct();
        if (transformType == nullptr || stateType == nullptr)
        {
            ClearSnapshot();
            return;
        }

        MiniMapNative::MassEntityHandleArray handles = {};
        native->mass.getMatchingEntityHandles(g_queryStorage.data(), &handles);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        measurement.Matching = handles.Num;
#endif
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
        visibilityTransform.LocalVisibilityWorldX = sample.LocalVisibilityOrigin.WorldX;
        visibilityTransform.LocalVisibilityWorldY = sample.LocalVisibilityOrigin.WorldY;
        visibilityTransform.Valid = true;
        std::vector<AlienRecord> next;
        next.reserve((std::min)(static_cast<std::size_t>(handles.Num), kMaximumRecords));
        std::size_t eligibleVisible = 0; // Production cap evidence, not admission.
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            ++measurement.Active;
#endif
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            ++measurement.Nearby;
#endif
            if (MiniMapFogOfWar::GetRenderedVisibilityAtWorldPosition(
                record.WorldX, record.WorldY, visibilityTransform, fog) > 0.0f) ++eligibleVisible;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            measurement.Visible = eligibleVisible;
#endif
            // Keep nearby candidates, including currently fogged ones. The
            // existing visible/nearest selection is evaluated against the SAME
            // transform as the rendered fog, so a moved feather has no stale
            // poll-time exclusions or character-based visibility exceptions.
            next.push_back(record);
        }
        const std::size_t eligible = eligibleVisible;
        const bool truncated = eligible > kMaximumRecords;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        measurement.Capped = truncated;
#endif
        const bool capWarningDue = PollClock::now() >= g_nextCapWarning;
        if (truncated)
        {
            if (!g_wasTruncated || capWarningDue)
            {
                LOG_WARN("MiniMap: Alien Actors cap hit: eligible=%zu retained=%zu (nearest)",
                    eligible, kMaximumRecords);
            }
        }
        g_wasTruncated = truncated;
        {
            std::scoped_lock lock(g_snapshotMutex);
            if (generation == g_worldGeneration)
            {
                g_records = std::move(next);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                measurement.Retained = (std::min)(eligible, kMaximumRecords);
#endif
            }
        }
    }

    void OnTick(float deltaSeconds)
    {
        if(!MiniMapUI::IsVisible())return;
        if(!MiniMapMap::GameplayReady())return;
        if (!g_experienceReady || !MiniMapMap::HasWorld())
        {
            return;
        }
        const auto settings = MiniMapConfig::GetRenderSettings();
        if (!settings.ShowAlienActors) return;
        g_pollAccumulator += deltaSeconds;
        if (g_pollAccumulator >= kPollIntervalSeconds)
        {
            g_pollAccumulator = 0.0f;
            Poll(settings.Fog);
        }
    }
}

namespace MiniMapAlienActors
{
    bool Initialize(IPluginSelf* self)
    {
        if (g_initialized) return true;
        if (self == nullptr || self->hooks == nullptr || MiniMap::Services::TryRuntimeEvents() == nullptr)
        {
            return false;
        }
        Reset();
        // AlienX dispatches these callbacks after the original engine tick.
        const auto tickReceipt = MiniMap::Services::TryRuntimeEvents()->OnTick(
            [](void*, float delta) { OnTick(delta); }, nullptr);
        if (tickReceipt.token.id) g_tickRegistration = tickReceipt.token;
        if (tickReceipt.acceptance == MiniMap::Services::Acceptance::Rejected ||
            tickReceipt.acceptance == MiniMap::Services::Acceptance::BackendUnknown)
        {
            if (g_tickRegistration.id) MiniMap::Services::TryRuntimeEvents()->Remove(g_tickRegistration);
            return false;
        }
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_DEBUG("MiniMap: Alien Actors reset");
#endif
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

    void Render(IModLoaderImGui* ui, const MiniMapMap::Transform& transform,
        const MiniMapConfig::FogOfWarSettings& fog)
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
        struct VisibleRecord { size_t RecordIndex; float Visibility; double DistanceSquared; };
        // UI-callback-owned scratch capacity only. Membership and visibility are
        // rebuilt every frame; no actor pointers or prior-frame decisions survive.
        static std::vector<VisibleRecord> visible;
        visible.clear();
        visible.reserve(g_records.size());
        const double radiusSquared = g_acquisitionRadius * g_acquisitionRadius;
        for (size_t index = 0; index < g_records.size(); ++index)
        {
            const auto& record = g_records[index];
            const double dx = record.WorldX - transform.TrackingWorldX;
            const double dy = record.WorldY - transform.TrackingWorldY;
            const double distanceSquared = dx*dx + dy*dy;
            if (record.WorldGeneration != g_worldGeneration || distanceSquared > radiusSquared) continue;
            const float visibility = MiniMapFogOfWar::GetRenderedVisibilityAtWorldPosition(
                record.WorldX, record.WorldY, transform, fog);
            if (visibility > 0.0f) visible.push_back({index, visibility, distanceSquared});
        }
        // Preserve the existing nearest-visible cap and selection-before-clip
        // policy; only presentation visibility must be current on every draw.
        if (visible.size() > kMaximumRecords)
        {
            std::nth_element(visible.begin(), visible.begin()+kMaximumRecords, visible.end(),
                [](const VisibleRecord& a, const VisibleRecord& b)
                {
                    return a.DistanceSquared < b.DistanceSquared;
                });
            visible.resize(kMaximumRecords);
        }
        ui->DL_PushClipRect(drawList, left, top, right, bottom, true);
        for (const auto& candidate : visible)
        {
            const auto& record = g_records[candidate.RecordIndex];
            MiniMapMap::ScreenPoint point = {};
            if (!transform.WorldToScreen(record.WorldX, record.WorldY, point) ||
                point.X + kDotRadiusPixels < left || point.X - kDotRadiusPixels > right ||
                point.Y + kDotRadiusPixels < top || point.Y - kDotRadiusPixels > bottom)
            {
                continue;
            }
            const uint32_t alpha = static_cast<uint32_t>(std::lround(
                static_cast<float>(kHostileColor >> 24) *
                candidate.Visibility));
            if (alpha == 0) continue;
            const uint32_t color = (kHostileColor & 0x00FFFFFFu) | (alpha << 24);
            ui->DL_AddCircleFilled(drawList, point.X, point.Y, kDotRadiusPixels, color, 12);
        }
        ui->DL_PopClipRect(drawList);
        visible.clear();
    }

    void Shutdown()
    {
        if (!g_initialized) return;
        g_experienceReady = false;
        if (auto* events = MiniMap::Services::TryRuntimeEvents())
        {
            const auto removal = events->Remove(g_tickRegistration);
            if (removal.registry == MiniMap::Services::RegistryRemoval::RequestReturned &&
                removal.core == MiniMap::Services::CoreDrain::Quiescent) g_tickRegistration = {};
        }
        Reset(); // Destruct while NativeApi is still available.
        g_initialized = false;
    }
}

#endif
