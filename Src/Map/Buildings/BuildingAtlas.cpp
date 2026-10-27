#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingAtlas.h"
#include "BuildingAtlasAlienX.h"
#include "BuildingScheduler.h"
#include "Map/Map.h"
#include "plugin_helpers.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <optional>
#include <utility>

namespace MiniMapBuildingAtlas
{
    namespace C = MiniMapBuildingAtlasCore;
    namespace X = MiniMapBuildingAtlasAlienX;
    namespace S = MiniMapSpriteStatus;

    struct Revision
    {
        X::Texture Texture;
        std::shared_ptr<const C::Layout> Pixels;
        std::shared_ptr<Revision> NextRetained; // Explicit uncertain-release ownership only.
    };
    struct SourceBounds
    {
        std::string Identifier;
        MiniMapBuildingCache::SpatialBounds Bounds;
        std::array<double, 4> SourceRect;
    };
    struct Session
    {
        uint64_t Serial{};
        bool Active = true, Busy = false, Retained = false;
        bool Dirty = false, UploadPending = false;
        unsigned PackAttempts = 0, UploadAttempts = 0;
        bool CapacityWaitReported = false, BatchExceptionReported = false;
        uint32_t DrawDiagnostics = 0; // Once per invalid published-entry category per session.
        uint64_t NextAttempt = 0, CandidateRevision = 0, PublishedRevision = 0;
        C::Atlas Core;
        X::Backend Backend;
        std::vector<SourceBounds> Bounds; // Representation metadata, never class-status truth.
        std::shared_ptr<Revision> Published, RetainedTextures;
        std::shared_ptr<Session> NextRetained;
    };
}

namespace
{
    template<class... Args> void SpriteDiagnostic(const char* format, Args... args) noexcept
    {
        try { if (auto* self = GetSelf(); self && self->logger && self->logger->Warn) self->logger->Warn(self, format, args...); }
        catch (...) {} // Reporting must not change atlas publication/ownership.
    }
    namespace A = MiniMapBuildingAtlas;
    namespace C = MiniMapBuildingAtlasCore;
    namespace X = MiniMapBuildingAtlasAlienX;
    namespace S = MiniMapSpriteStatus;
    S::Dictionary* g_statuses = nullptr;
    IPluginImGuiTextures* g_textures = nullptr;
    bool g_interfacesAlive = false;
    // Uses the existing LifecycleMutex. Teardown waits with that mutex released;
    // Busy covers the entire batch, including backend calls, releases and PNG export.
    std::condition_variable g_serviceFinished;
    bool g_stopping = false;
    std::shared_ptr<A::Session> g_session, g_uncertain;

    bool Current(const A::Batch& batch)
    {
        // Caller holds LifecycleMutex. Neither Core nor mutable Backend state is
        // inspected here while its game-thread service runs unlocked.
        return batch && batch == g_session && batch->Active && !g_uncertain &&
            g_interfacesAlive && !g_stopping && MiniMapMap::GameplayReady() &&
            MiniMapBuildingSchedulerInternal::ActiveSession() == batch->Serial;
    }
    bool Admitted(const A::Batch& batch)
    {
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        return Current(batch);
    }
    void Retain(const A::Batch& batch, std::shared_ptr<A::Revision>& revision)
    {
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        if (revision)
        {
            revision->NextRetained = std::move(batch->RetainedTextures);
            batch->RetainedTextures = std::move(revision);
        }
        if (!batch->Retained)
        {
            batch->Retained = true;
            batch->NextRetained = std::move(g_uncertain);
            g_uncertain = batch;
        }
        // These records survive session reset/shutdown. No blind Free retry and
        // no native call from destruction, even when no handle was returned.
        LOG_ERROR("MiniMap: atlas ownership uncertain; further atlas uploads/session admission blocked");
    }
    void Release(const A::Batch& batch, std::shared_ptr<A::Revision>& revision)
    {
        if (!revision) return;
        if (revision->Texture.HasOwnership())
        {
            bool available;
            {
                std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
                available = g_interfacesAlive;
            }
            if (!available || batch->Backend.Release(revision->Texture) == X::ReleaseResult::OwnershipUncertain)
            { Retain(batch, revision); return; }
        }
        revision.reset();
    }
    bool SameImage(const C::Layout& a, const C::Layout& b)
    {
        if (a.Width != b.Width || a.Height != b.Height || a.Images.size() != b.Images.size() || a.Rgba != b.Rgba) return false;
        for (size_t i = 0; i < a.Images.size(); ++i)
        {
            const auto& x = a.Images[i]; const auto& y = b.Images[i];
            if (x.Identifier != y.Identifier || x.Crop.X != y.Crop.X || x.Crop.Y != y.Crop.Y ||
                x.Crop.Width != y.Crop.Width || x.Crop.Height != y.Crop.Height ||
                x.Content.X != y.Content.X || x.Content.Y != y.Content.Y ||
                x.Content.Width != y.Content.Width || x.Content.Height != y.Content.Height) return false;
        }
        return true; // Revision/deferred-list changes alone do not change GPU pixels or UVs.
    }
    uint64_t Later(uint64_t tick)
    {
        constexpr auto maximum = (std::numeric_limits<uint64_t>::max)();
        return tick > maximum - 60 ? maximum : tick + 60;
    }
    void Stop(bool shutdown)
    {
        A::Batch session;
        std::shared_ptr<A::Revision> old;
        std::unique_lock<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        // Serialize EndSession with Shutdown, including their unlocked releases.
        // Otherwise a concurrent Shutdown could close the borrowed API while an
        // earlier EndSession was still using it to release the published texture.
        g_serviceFinished.wait(lock, [] { return !g_stopping; });
        g_stopping = true;
        session = g_session;
        if (session) session->Active = false; // Deny publication/new batches first.
        g_serviceFinished.wait(lock, [&] { return !session || !session->Busy; });
        // EndBatch has now returned from Create/GetSize AND all candidate/old
        // releases. No batch can cross another backend boundary during teardown.
        g_session.reset();
        if (session) old = std::move(session->Published);
        if (g_statuses)
            for (auto& [name, status] : *g_statuses)
                if (status.Where == S::Location::Atlas) { status.Atlas = {}; status.Where = S::Location::FileCache; }
        lock.unlock();
        if (session) Release(session, old);
        lock.lock();
        if (shutdown)
        {
            g_interfacesAlive = false;
            g_textures = nullptr; g_statuses = nullptr;
        }
        g_stopping = false;
        lock.unlock();
        g_serviceFinished.notify_all();
    }
}

namespace MiniMapBuildingAtlas
{
    bool Initialize(S::Dictionary& statuses, IPluginImGuiTextures* textures)
    {
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        if (g_session || g_uncertain || g_stopping) return false;
        if (!textures || !textures->LoadFromRGBA || !textures->FreeTexture || !textures->GetSize || !textures->GetFreeSlotCount) return false;
        g_statuses = &statuses; g_textures = textures; g_interfacesAlive = true;
        return true;
    }
    void ObservePublication(uint64_t& session,uint64_t& revision) noexcept
    {
        session=g_session?g_session->Serial:0;
        revision=g_session?g_session->PublishedRevision:0;
    }
    bool InitializeSession(uint64_t serial)
    {
        // Backend.Initialize only validates function pointers and makes a GUID;
        // it performs no GPU call. Keep that borrowed-interface access within
        // the lifecycle scope, so Shutdown cannot overtake initialization either.
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        if (!serial || !g_interfacesAlive || !g_statuses || g_uncertain || g_stopping ||
            MiniMapBuildingSchedulerInternal::ActiveSession() != serial) return false;
        if (g_session) return g_session->Active && g_session->Serial == serial;
        std::shared_ptr<Session> session;
        try
        {
            session = std::make_shared<Session>();
            session->Serial = serial;
            if (!session->Backend.Initialize(g_textures)) return false; // No GPU operation.
        }
        catch (...) { return false; }
        g_session = std::move(session);
        return true;
    }
    Batch BeginBatch(uint64_t serial)
    {
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        if (!Current(g_session) || g_session->Serial != serial || g_session->Busy) return {};
        g_session->Busy = true;
        return g_session;
    }
    AddResult Add(const Batch& batch, const std::string& name, const MiniMapBuildingCache::Sprite& sprite) noexcept
    {
        if (!batch)
        {
            // Scheduler calls Add at most three times for each decoded payload.
            SpriteDiagnostic("MiniMap: atlas admission unavailable: canonical=%s reason=no admitted atlas batch", name.c_str());
            return AddResult::Unavailable;
        }
        try
        {
            if (!Admitted(batch)) return AddResult::Unavailable;
            for (const auto& bounds : batch->Bounds)
                if (bounds.Identifier == name) return AddResult::Retained;
            if (!MiniMapBuildingCache::ValidSpatial(sprite.Width, sprite.Height, sprite.Bounds, sprite.Mapping))
            {
                SpriteDiagnostic("MiniMap: atlas spatial metadata rejected: canonical=%s reason=invalid processed dimensions/capture mapping", name.c_str());
                return AddResult::Invalid;
            }
            for (size_t i = 0; i < 3; ++i)
                if (!std::isfinite(sprite.Bounds.Min[i]) || !std::isfinite(sprite.Bounds.Max[i]) ||
                    sprite.Bounds.Max[i] < sprite.Bounds.Min[i])
                {
                    SpriteDiagnostic("MiniMap: atlas bounds rejected: canonical=%s axis=%zu reason=nonfinite/reversed bounds", name.c_str(), i);
                    return AddResult::Invalid;
                }
            if (!(sprite.Bounds.Max[0] > sprite.Bounds.Min[0]) || !(sprite.Bounds.Max[1] > sprite.Bounds.Min[1]))
            {
                SpriteDiagnostic("MiniMap: atlas bounds rejected: canonical=%s reason=nonpositive XY extent", name.c_str());
                return AddResult::Invalid;
            }
            // Allocate representation identity before Add can acquire retained pixels.
            batch->Bounds.push_back({name, sprite.Bounds,
                {double(sprite.Mapping.X), double(sprite.Mapping.Y), sprite.Mapping.X + sprite.Width * sprite.Mapping.Step,
                 sprite.Mapping.Y + sprite.Height * sprite.Mapping.Step}});
            const auto result = batch->Core.Add(name, sprite.Width, sprite.Height, sprite.Rgba);
            if (result != C::Result::Ok)
            {
                batch->Bounds.pop_back();
                const auto reason = result == C::Result::UnsupportedDimensions ? "unsupported processed image dimensions" :
                    result == C::Result::InvalidBuffer ? "RGBA byte length rejected" : result == C::Result::EmptyAlpha ? "no alpha occupancy" :
                    result == C::Result::AllocationFailure ? "CPU allocation failed" :
                    result == C::Result::Overflow ? "size arithmetic overflow" : result == C::Result::DuplicateIdentifier ? "duplicate source identifier" :
                    result == C::Result::EmptyIdentifier ? "empty source identifier" : "core rejected source (see result)";
                SpriteDiagnostic("MiniMap: atlas Add rejected '%s' (core result %u, reason=%s)", name.c_str(), unsigned(result), reason);
                if (result == C::Result::AllocationFailure || result == C::Result::Overflow || result == C::Result::InternalError)
                    return AddResult::Unavailable;
                return AddResult::Invalid;
            }
            batch->Dirty = true;
            batch->PackAttempts = 0;
            batch->NextAttempt = 0;
            return AddResult::Retained;
        }
        catch (...) { SpriteDiagnostic("MiniMap: atlas source retention exception: canonical=%s; valid cache remains FileCache", name.c_str()); return AddResult::Unavailable; }
    }
    void EndBatch(const Batch& batch, uint64_t tick) noexcept
    {
        if (!batch) return;
        const bool sourceAdmission = batch->Dirty;
        std::shared_ptr<Revision> candidate, old;
        std::shared_ptr<const C::Layout> previousPixels;
        std::shared_ptr<const C::Layout> diagnosticPixels;
        try
        {
            {
                std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
                if (batch->Published) previousPixels = batch->Published->Pixels;
            }
            // This lambda can return without bypassing Busy cleanup below.
            [&]
            {
                if (!Admitted(batch) || tick < batch->NextAttempt) return;
                if (batch->Dirty)
                {
                    if (batch->PackAttempts >= 3) return;
                    ++batch->PackAttempts;
                    // Every retained cache source belongs to this candidate. Never
                    // publish a fitting subset or evict an existing allocation.
                    // Repack commits only a complete 512/1024 CPU layout.
                    const auto result = batch->Core.Repack();
                    if (result != C::Result::Ok)
                    {
                        batch->NextAttempt = Later(tick);
                        if (result == C::Result::Capacity)
                            SpriteDiagnostic("MiniMap: atlas physical packing capacity exhausted: retained=%zu supported=512/1024 max=1024 gutter=2 stable allocations; complete candidate not published, previous atlas and FileCache sources preserved", batch->Core.Sources().size());
                        SpriteDiagnostic("MiniMap: atlas complete-set repack unavailable (%u, attempt %u/3)", unsigned(result), batch->PackAttempts);
                        for (const auto& source : batch->Core.Sources())
                            SpriteDiagnostic("MiniMap: atlas repack candidate includes canonical=%s result=%u attempt=%u/3; previous representation preserved",
                                source.Identifier.c_str(), unsigned(result), batch->PackAttempts);
                        return;
                    }
                    batch->Dirty = false;
                    batch->CandidateRevision = batch->Core.Current().Revision;
                    batch->UploadAttempts = 0;
                    batch->CapacityWaitReported = false;
                    batch->UploadPending = !previousPixels || !SameImage(batch->Core.Current(), *previousPixels);
                }
                if (!batch->UploadPending || batch->UploadAttempts >= 3 || !Admitted(batch)) return;
                batch->NextAttempt = Later(tick);
                if (!batch->Backend.HasCapacity())
                {
                    if (!batch->CapacityWaitReported)
                    {
                        batch->CapacityWaitReported = true;
                        SpriteDiagnostic("MiniMap: atlas publication waiting: candidateRevision=%llu reason=backend HasCapacity returned false (slot capacity, prerequisites or observation unavailable); candidate retained, no upload attempt consumed",
                            static_cast<unsigned long long>(batch->CandidateRevision));
                        for (const auto& image : batch->Core.Current().Images)
                            SpriteDiagnostic("MiniMap: atlas waiting candidate includes canonical=%s", image.Identifier.c_str());
                    }
                    return; // Keep the candidate; do not reread or repack.
                }
                ++batch->UploadAttempts; // Also bound failed CPU snapshot/publication preparation.

                const auto& layout = batch->Core.Current();
                // Allocate every fallible CPU publication object BEFORE acquiring a GPU reference.
                if (batch->PublishedRevision == (std::numeric_limits<uint64_t>::max)()) return;
                candidate = std::make_shared<Revision>();
                auto pixels = std::make_shared<C::Layout>(layout);
                // Core revisions identify proposals. Public/PNG revisions count
                // only committed GPU generations, including after failed uploads.
                pixels->Revision = batch->PublishedRevision + 1;
                candidate->Pixels = std::move(pixels);
                std::vector<std::pair<std::string, S::AtlasEntry>> entries;
                entries.reserve(layout.Images.size());
                for (const auto& image : layout.Images)
                {
                    const auto bounds = std::find_if(batch->Bounds.begin(), batch->Bounds.end(),
                        [&](const SourceBounds& b) { return b.Identifier == image.Identifier; });
                    if (bounds == batch->Bounds.end())
                    { SpriteDiagnostic("MiniMap: atlas publication rejected: canonical=%s reason=source bounds missing; attempt=%u/3", image.Identifier.c_str(), batch->UploadAttempts); return; }
                    S::AtlasEntry entry;
                    entry.Region = {image.Corners[0].U, image.Corners[0].V, image.Corners[2].U, image.Corners[2].V};
                    entry.UVs = S::PreparedAtlasUV::FromRegion(entry.Region);
                    entry.SourceRect = bounds->SourceRect;
                    entry.Min = bounds->Bounds.Min; entry.Max = bounds->Bounds.Max;
                    entries.emplace_back(image.Identifier, entry);
                }
                if (!Admitted(batch)) return; // Session may have ended during CPU preparation.
                const auto result = batch->Backend.Create(layout.Rgba, layout.Width, layout.Height, candidate->Texture);
                if (result == X::TransferResult::OwnershipUncertain)
                {
                    for (const auto& image : layout.Images)
                        SpriteDiagnostic("MiniMap: atlas publication ownership uncertain: candidate includes canonical=%s; existing ownership retention applies", image.Identifier.c_str());
                    Retain(batch, candidate); return;
                }
                if (result != X::TransferResult::Complete)
                {
                    SpriteDiagnostic("MiniMap: atlas transfer unavailable (%u, attempt %u/3); previous GPU revision preserved",
                        unsigned(result), batch->UploadAttempts);
                    for (const auto& image : layout.Images)
                        SpriteDiagnostic("MiniMap: atlas transfer candidate not published: canonical=%s result=%u attempt=%u/3; previous representation preserved",
                            image.Identifier.c_str(), unsigned(result), batch->UploadAttempts);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
                    if (!Current(batch) || MiniMapBuildingSchedulerInternal::ActiveSession() != batch->Serial ||
                        layout.Revision != batch->CandidateRevision) return;
                    // Validate all dictionary entries before changing any publication.
                    for (const auto& entry : entries)
                    {
                        const auto it = g_statuses->find(entry.first);
                        if (it == g_statuses->end() ||
                            (it->second.Where != S::Location::FileCache && it->second.Where != S::Location::Atlas))
                        {
                            SpriteDiagnostic("MiniMap: atlas publication rejected: canonical=%s reason=expected FileCache/Atlas dictionary state missing/changed; attempt=%u/3", entry.first.c_str(), batch->UploadAttempts);
                            return;
                        }
                    }
                    old = std::move(batch->Published);
                    batch->Published = candidate;
                    for (const auto& entry : entries)
                    {
                        auto& status = g_statuses->find(entry.first)->second;
                        status.Atlas = entry.second;
                        status.Atlas.Texture = candidate->Texture.Binding();
                        status.Where = S::Location::Atlas;
                    }
                    batch->PublishedRevision = candidate->Pixels->Revision;
                    batch->UploadPending = false;
                    // Freeze the actually published pixels, not Core.Current().
                    // Retain through retirement, commit reporting and optional PNG export.
                    diagnosticPixels = batch->Published->Pixels;
                    candidate.reset(); // Current publication now owns the acquired reference.
                }
            }();
        }
        catch (...)
        {
            if (!batch->BatchExceptionReported)
            {
                batch->BatchExceptionReported = true;
                SpriteDiagnostic("MiniMap: atlas batch allocation/service exception: candidateRevision=%llu packAttempt=%u uploadAttempt=%u; previous GPU publication preserved; further exception diagnostics suppressed for session",
                    static_cast<unsigned long long>(batch->CandidateRevision), batch->PackAttempts, batch->UploadAttempts);
                for (const auto& bounds : batch->Bounds)
                    SpriteDiagnostic("MiniMap: atlas exception affected batch includes canonical=%s", bounds.Identifier.c_str());
            }
        }
        if(diagnosticPixels)
        {
                    SpriteDiagnostic("MiniMap: atlas committed: revision=%llu sources=%zu size=%ux%u candidateRevision=%llu kind=%s reason=%s",
                        static_cast<unsigned long long>(batch->PublishedRevision), diagnosticPixels->Images.size(),
                        unsigned(diagnosticPixels->Width), unsigned(diagnosticPixels->Height), static_cast<unsigned long long>(batch->CandidateRevision),
                        batch->PublishedRevision==1?"initial-available-batch":"incremental",
                        sourceAdmission?"source-admission":"pending-upload-retry");
        }
        // Never invoke the loader inside LifecycleMutex, including rejected/stale uploads.
        Release(batch, candidate);
        Release(batch,old);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (diagnosticPixels)
            MiniMapBuildingCache::WriteAtlasDiagnosticPng(diagnosticPixels->Rgba,
                diagnosticPixels->Width, diagnosticPixels->Height, diagnosticPixels->Revision);
#endif
        // Keep synchronous export inside Busy as well as backend operations.
        {
            std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
            batch->Busy = false;
        }
        g_serviceFinished.notify_all();
    }

    void EndSession()
    {
        Stop(false);
    }
    void Shutdown()
    {
        Stop(true);
    }
    const S::AtlasEntry* Find(const std::string& name) noexcept
    {
        if (!Current(g_session) || !g_statuses || !g_session->Published) return nullptr;
        const auto it = g_statuses->find(name);
        if (it == g_statuses->end() || it->second.Where != S::Location::Atlas) return nullptr;
        if (!it->second.Atlas.Texture || it->second.Atlas.Texture != g_session->Published->Texture.Binding())
        {
            if (!(g_session->DrawDiagnostics & 1))
            {
                g_session->DrawDiagnostics |= 1;
                SpriteDiagnostic("MiniMap: atlas entry unavailable: canonical=%s reason=Atlas status has missing/stale texture binding; further binding diagnostics suppressed for session", name.c_str());
            }
            return nullptr;
        }
        return &it->second.Atlas;
    }
    bool SubmitQuad(IModLoaderImGui* ui, PluginDrawList list, const S::AtlasEntry& entry,
        const std::array<X::Point, 4>& positions, uint32_t color) noexcept
    {
        if (!Current(g_session) || !g_session->Published || !entry.Texture ||
            entry.Texture != g_session->Published->Texture.Binding()) return false;
        // Default/compatibility entries retain submission-time preparation.
        // Published entries never take this branch; Region is immutable there.
        std::optional<S::PreparedAtlasUV> local;
        const auto* uv = &entry.UVs;
        if (uv->Status() == S::PreparedAtlasUV::State::Unprepared)
        { local.emplace(S::PreparedAtlasUV::FromRegion(entry.Region)); uv = &*local; }
        if (uv->Status() == S::PreparedAtlasUV::State::InvalidRegion)
        {
            if (!(g_session->DrawDiagnostics & 2))
            {
                g_session->DrawDiagnostics |= 2;
                SpriteDiagnostic("MiniMap: atlas quad rejected: reason=UV nonfinite/outside unit interval; further UV diagnostics suppressed for session");
            }
            return false;
        }
        if (uv->Status() == S::PreparedAtlasUV::State::InvalidRectangle)
        {
            if (!(g_session->DrawDiagnostics & 4))
            {
                g_session->DrawDiagnostics |= 4;
                SpriteDiagnostic("MiniMap: atlas quad rejected: reason=empty/reversed UV rectangle; further rectangle diagnostics suppressed for session");
            }
            return false;
        }
        return g_session->Backend.SubmitPreparedQuad(ui, list, g_session->Published->Texture, positions, *uv, color);
    }
    std::shared_ptr<const C::Layout> PublishedImage()
    {
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        return g_session && g_session->Active && g_session->Published ? g_session->Published->Pixels : nullptr;
    }
}

#endif
