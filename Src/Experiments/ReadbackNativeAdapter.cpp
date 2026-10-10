#if defined(MODLOADER_CLIENT_BUILD) && (defined(MINIMAP_ASYNC_READBACK_EXPERIMENT) || defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE))
#include "ReadbackUEConfiguration.h"
#define RHI_API
#define RENDERCORE_API
#define WITH_MGPU 1
#define WITH_RHI_BREADCRUMBS 0
#define RHI_ENABLE_RESOURCE_INFO 0
#include "RHIResources.h"
#include "Containers/ResourceArray.h"
#include "ReadbackNativeAdapter.h"
#include "ReadbackNativeBindings.h"
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <new>
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
#include "RHITransition.h"
#include "SmelterCaptureNativeBindings.h"
#include <vector>
class FRHIComputeCommandList;
#endif

class FRHICommandListBase;
class FRHICommandList;
class FRHICommandListImmediate;
class FRHIGPUTextureReadback; // Native owns its construction and all eight slots.

// Header-inline descriptor defaults require this global. This is a validated
// snapshot of the native global, never a substituted single-GPU constant.
uint32 GNumExplicitGPUsForRendering = 0;

namespace MiniMapReadbackNative
{
    using RenderCallable = TUniqueFunction<void(FRHICommandListImmediate&)>;
    static_assert(sizeof(RenderCallable) == 0x30 && alignof(RenderCallable) == 16);
    static_assert(NUM_TFUNCTION_INLINE_BYTES == 24 && sizeof(TStatId) == 1);
    static_assert(sizeof(FResourceBulkDataInterface) == 0x08);
    static_assert(sizeof(FResourceBulkDataArrayView) == 0x18);
    static_assert(offsetof(FResourceBulkDataArrayView, Data) == 0x08);
    static_assert(offsetof(FResourceBulkDataArrayView, SizeInBytes) == 0x10);
    static_assert(sizeof(FRHITextureDesc) == 0x38);
    static_assert(sizeof(FRHITextureCreateDesc) == 0x60);
    static_assert(offsetof(FRHITextureCreateDesc, InitialState) == 0x38);
    static_assert(offsetof(FRHITextureCreateDesc, DebugName) == 0x40);
    static_assert(offsetof(FRHITextureCreateDesc, BulkData) == 0x48);
    static_assert(sizeof(FTextureRHIRef) == 8 && sizeof(FName) == 8);
    static_assert(sizeof(FNameEntryId) == 4 && sizeof(FRHIResource) == 0x10);
    static_assert(uint32(PF_B8G8R8A8) == 2);
    static_assert(uint32(ERHIAccess::CopySrc) == (1u << 7));
    static_assert(sizeof(FRHIGPUFence) == 0x20);
    static_assert(offsetof(FRHIGPUFence, NumPendingWriteCommands) == 0x10);

    // Inspection schema ONLY, not a substitute C++ owning readback object.
    // CL-127004 PDB + ctor/dtor: eight references, reverse-order destruction.
    // Native construction starts the actual object's lifetime in native malloc
    // storage; no local constructor/destructor touches its members.
    struct ReadbackLayout
    {
        void* VTable;
        FRHIGPUFence* Fence;
        uint32 CopyMask, LockIndex;
        FRHITexture* Staging[8];
    };
    static_assert(sizeof(ReadbackLayout) == 0x58);
    static_assert(offsetof(ReadbackLayout, Fence) == 0x08);
    static_assert(offsetof(ReadbackLayout, CopyMask) == 0x10);
    static_assert(offsetof(ReadbackLayout, LockIndex) == 0x14);
    static_assert(offsetof(ReadbackLayout, Staging) == 0x18);
    static_assert(offsetof(ReadbackLayout, Staging) + 7 * sizeof(void*) == 0x50);

    using SubmitFn = void(*)(void*, const TCHAR*, uint32&, TStatId, RenderCallable&&);
    using ImmediateFn = FRHICommandListImmediate&(*)();
    // Explicit MSVC x64 member-function indirect result: this, sret, list, desc.
    using CreateFn = FTextureRHIRef*(*)(void*, FTextureRHIRef*, FRHICommandListBase*, const FRHITextureCreateDesc*);
    using ConstructFn = FRHIGPUTextureReadback*(*)(FRHIGPUTextureReadback*, FName);
    using DestroyFn = void(*)(FRHIGPUTextureReadback*);
    using CopyFn = void(*)(FRHIGPUTextureReadback*, FRHICommandList&, FRHITexture*, const FIntVector&, uint32, const FIntVector&);
    using LockFn = void*(*)(FRHIGPUTextureReadback*, int32&, int32*);
    using UnlockFn = void(*)(FRHIGPUTextureReadback*);
    using PollFn = bool(*)(FRHIGPUFence*);

    // Only the render-command state machine below may touch native resources.
    namespace Operations
    {
        bool Prerequisites()
        {
            const auto& b = GetBindings();
            if (!b.Create || !b.GpuCount || !b.DynamicRHI || !b.Threaded || !b.Multithreaded) return false;
            // PDB FRHIGlobals: IsRHIInitialized +0, SupportsMultithreading +0x10.
            if (!*reinterpret_cast<const bool*>(b.Multithreaded - 0x10)) return false;
            if (!*reinterpret_cast<const bool*>(b.Threaded) || !*reinterpret_cast<const bool*>(b.Multithreaded)) return false;
            const auto count = *reinterpret_cast<const uint32*>(b.GpuCount);
            if (count == 0 || count > 8 || count != GNumExplicitGPUsForRendering) return false;
            auto* rhi = *reinterpret_cast<void**>(b.DynamicRHI);
            if (!rhi) return false;
            // Verified native virtual slot used by EnqueueCopy. Reject another RHI.
            return (*reinterpret_cast<uintptr_t**>(rhi))[0x1A0 / 8] == b.Create;
        }

        bool Submit(const TCHAR* persistentName, uint32& persistentSpec, RenderCallable&& callable)
        {
            if (!Prerequisites()) return false;
            const auto& b = GetBindings();
            reinterpret_cast<SubmitFn>(b.Submit)(reinterpret_cast<void*>(b.Pipe), persistentName,
                persistentSpec, TStatId{}, MoveTemp(callable));
            return true;
        }

        FRHICommandListImmediate& Immediate()
        {
            return reinterpret_cast<ImmediateFn>(GetBindings().Immediate)();
        }

        FTextureRHIRef Create(FRHICommandListBase& list, const void* pixels, uint32 bytes)
        {
            FTextureRHIRef result;
            if (!Prerequisites() || !pixels || bytes != 3404) return result;
            FResourceBulkDataArrayView bulk(pixels, bytes);
            auto desc = FRHITextureCreateDesc::Create2D(TEXT("MiniMapR1"), 37, 23, PF_B8G8R8A8);
            desc.SetNumMips(1).SetNumSamples(1).SetArraySize(1)
                .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
                .SetInitialState(ERHIAccess::CopySrc).SetBulkData(&bulk);
            const auto& b = GetBindings();
            // Native InitializeTextureData copies bulk bytes into an engine-owned
            // upload allocation before returning; its queued GPU work owns that
            // allocation. InitialState drives its final CopySrc barrier.
            reinterpret_cast<CreateFn>(b.Create)(*reinterpret_cast<void**>(b.DynamicRHI), &result, &list, &desc);
            return result;
        }

        FRHIGPUTextureReadback* Construct()
        {
            if (!Prerequisites()) return nullptr;
            const auto& b = GetBindings();
            auto* storage = static_cast<FRHIGPUTextureReadback*>(
                reinterpret_cast<void*(*)(SIZE_T, uint32)>(b.Malloc)(sizeof(ReadbackLayout), 8));
            return storage ? reinterpret_cast<ConstructFn>(b.Construct)(storage, FName{}) : nullptr;
        }

        void Copy(FRHIGPUTextureReadback* readback, FRHICommandList& list, FRHITexture* texture)
        {
            reinterpret_cast<CopyFn>(GetBindings().Copy)(readback, list, texture,
                FIntVector(0, 0, 0), 0, FIntVector(37, 23, 1));
        }

        enum class Readiness { Pending, Complete, Invalid, Unavailable };
        bool ValidFence(FRHIGPUTextureReadback* readback)
        {
            if (!readback) return false;
            FRHIGPUFence* fence = nullptr;
            std::memcpy(&fence, reinterpret_cast<const unsigned char*>(readback) + offsetof(ReadbackLayout, Fence), sizeof(fence));
            return fence && (*reinterpret_cast<uintptr_t**>(fence))[3] == GetBindings().Poll;
        }
        Readiness Check(FRHIGPUTextureReadback* readback)
        {
            if (!Prerequisites()) return Readiness::Unavailable;
            if (!ValidFence(readback)) return Readiness::Invalid;
            ReadbackLayout view;
            std::memcpy(&view, readback, sizeof(view));
            if (!view.CopyMask || (view.CopyMask >> GNumExplicitGPUsForRendering)) return Readiness::Invalid;
            for (uint32 i = 0; i < 8; ++i)
                if ((view.CopyMask & (1u << i)) && !view.Staging[i]) return Readiness::Invalid;
            if (view.Fence->NumPendingWriteCommands.GetValue() != 0) return Readiness::Pending;
            return reinterpret_cast<PollFn>(GetBindings().Poll)(view.Fence)
                ? Readiness::Complete : Readiness::Pending;
        }

        void* Map(FRHIGPUTextureReadback* readback, int32& pitch, int32& height)
        {
            if (Check(readback) != Readiness::Complete) return nullptr;
            return reinterpret_cast<LockFn>(GetBindings().Lock)(readback, pitch, &height);
        }
        void Unmap(FRHIGPUTextureReadback* readback)
        {
            reinterpret_cast<UnlockFn>(GetBindings().Unlock)(readback);
        }
        // Caller must prove no queued command/copy/map still references it.
        // Cancellation or a timeout is NOT such proof.
        void DestroyCompleted(FRHIGPUTextureReadback* readback)
        {
            if (!readback) return;
            const auto& b = GetBindings();
            reinterpret_cast<DestroyFn>(b.Destroy)(readback);
            reinterpret_cast<void(*)(void*)>(b.Free)(readback);
        }
        void ReleaseCompleted(FTextureRHIRef& texture) { texture.SafeRelease(); }
    }

    bool Preflight()
    {
        const auto& b = GetBindings();
        if (!b.GpuCount) return false;
        GNumExplicitGPUsForRendering = *reinterpret_cast<const uint32*>(b.GpuCount);
        const bool ready = Operations::Prerequisites();
        if (!ready) SetStatus("RHI initialization/threading/GPU-count/backend prerequisite unavailable");
        return ready;
    }

    namespace Px = MiniMapAsyncReadbackExperiment::Pixels;
    struct Job
    {
        std::atomic<Phase> Current{Phase::ReadyToSubmit};
        std::atomic<Outcome> Result{Outcome::Pending};
        std::atomic<uint32> Visited{1u << uint32(Phase::ReadyToSubmit)};
        std::atomic<uint32> Callables{0};
        std::atomic<bool> Outstanding{false}, Quarantined{false}, DataReady{false};
        std::atomic<const char*> Failure{nullptr};
        std::atomic<uint64_t> Frame{0}, StartFrame{0};
        std::atomic<int64_t> StartNs{0};
        // Only game-thread submission touches these trace IDs. Native callable
        // names are literals in this DLL, and Job persists through destruction.
        uint32 SubmitSpec = 0, PollSpec = 0;
        std::array<uint8_t, Px::ByteCount> Initial{}, Cpu{};
        // Render-thread-only until DataReady release publication. Immutable then.
        Px::Result Verification{};
        int Pitch = 0, Height = 0;
        double CompletionMs = 0;
        uint64_t CompletionFrames = 0;
        bool HasPixels = false;
        // Render-thread-only: no UObject/world/component references.
        FTextureRHIRef Texture;
        FRHIGPUTextureReadback* Readback = nullptr;
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
        bool External = false;
        void* TargetResource = nullptr;
        uint32 Width = 0, ImageHeight = 0;
        std::vector<uint8_t> FloatCpu;
        MiniMapFloatPixels::Stats FloatStatistics;
#endif
    };

    namespace
    {
        int64_t NowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        void Enter(Job* job, Phase phase)
        {
            job->Current.store(phase, std::memory_order_release);
            job->Visited.fetch_or(1u << uint32(phase), std::memory_order_release);
        }
        void Fail(Job* job, const char* reason, bool quarantine)
        {
            job->Failure.store(reason, std::memory_order_release);
            if (quarantine) job->Quarantined.store(true, std::memory_order_release);
            auto pending = Outcome::Pending;
            job->Result.compare_exchange_strong(pending, Outcome::Failed);
            Enter(job, Phase::Failed);
            if (quarantine)
            {
                Enter(job, Phase::CleanupPending);
            }
        }
        void Cleanup(Job* job)
        {
            Enter(job, Phase::CleanupPending);
            Operations::DestroyCompleted(job->Readback);
            job->Readback = nullptr;
            Operations::ReleaseCompleted(job->Texture);
            Enter(job, Phase::CleanupComplete);
        }

        void Execute(Job* job, FRHICommandListImmediate& provided)
        {
            if (job->Current.load(std::memory_order_acquire) == Phase::CommandQueued)
            {
                if (job->Result.load() == Outcome::Cancelled
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
                    && !job->External
#endif
                    ) { Cleanup(job); return; }
                if (!Operations::Prerequisites())
                {
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
                    if(job->External){Fail(job,"native prerequisites changed after capture submission",true);return;}
#endif
                    Fail(job, "native prerequisites changed before creation", false); Cleanup(job); return;
                }
                auto& immediate = Operations::Immediate();
                if (&immediate != &provided)
                {
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
                    if(job->External){Fail(job,"capture callback immediate-list mismatch",true);return;}
#endif
                    Fail(job, "render callback did not supply the native immediate list", false); Cleanup(job); return;
                }
                // PDB: Immediate -> CommandList -> Base each has base offset 0.
                // Keep the classes incomplete; do not include command-list headers.
                auto& base = reinterpret_cast<FRHICommandListBase&>(immediate);
                auto& list = reinterpret_cast<FRHICommandList&>(immediate);
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
                if (job->External)
                {
                    // CL PDB: FRenderTarget base at +0x50. Native no-argument
                    // getter slot +0x10 returns the reference at subobject +8.
                    const auto* target = static_cast<const uint8_t*>(job->TargetResource) + 0x50;
                    auto getter = (*reinterpret_cast<const uintptr_t* const*>(target))[2];
                    const unsigned char expected[]{0x48,0x8d,0x41,0x08,0xc3};
                    if (std::memcmp(reinterpret_cast<const void*>(getter),expected,sizeof(expected)))
                    { Fail(job,"unverified render-target getter; capture retirement unproven",true); return; }
                    using Getter = const FTextureRHIRef&(*)(const void*);
                    job->Texture = reinterpret_cast<Getter>(getter)(target);
                    job->TargetResource = nullptr;
                    if (!job->Texture) { Fail(job,"capture RHI texture unavailable; retirement unproven",true); return; }
                    const auto& desc = job->Texture->GetDesc();
                    if (desc.Format != PF_FloatRGBA || desc.NumSamples != 1 || desc.Extent.X != int32(job->Width) || desc.Extent.Y != int32(job->ImageHeight))
                    { Fail(job,"capture RHI descriptor mismatch; retirement unproven",true); return; }
                }
                else
#endif
                job->Texture = Operations::Create(base, job->Initial.data(), uint32(job->Initial.size()));
                if (!job->Texture)
                { Fail(job, "native texture creation returned null", false); Cleanup(job); return; }
                job->Readback = Operations::Construct();
                // Texture creation has already queued upload work. Without a
                // usable copy fence, do NOT pretend that early release is safe.
                if (!job->Readback || !Operations::ValidFence(job->Readback))
                { Fail(job, "readback construction/fence invalid; upload retirement unproven", true); return; }
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
                if (job->External)
                {
                    using TransitionFn = void(*)(FRHIComputeCommandList*, TArrayView<const FRHITransitionInfo>, ERHITransitionCreateFlags);
                    static_assert(sizeof(FRHITransitionInfo)==0x30 && sizeof(TArrayView<const FRHITransitionInfo>)==0x10);
                    static_assert(sizeof(FRHISubresourceRange)==6 && sizeof(FRHITexture)==0x60);
                    static_assert(offsetof(FRHITransitionInfo,Texture)==8 && offsetof(FRHITransitionInfo,AccessAfter)==0x18);
                    const auto transition=reinterpret_cast<TransitionFn>(MiniMapSmelterCapture::GetBindings().Transition);
                    const FRHITransitionInfo before(job->Texture.GetReference(),ERHIAccess::SRVMask,ERHIAccess::CopySrc);
                    transition(reinterpret_cast<FRHIComputeCommandList*>(&immediate),MakeArrayView(&before,1),ERHITransitionCreateFlags::None);
                    reinterpret_cast<CopyFn>(GetBindings().Copy)(job->Readback,list,job->Texture.GetReference(),FIntVector(0,0,0),0,FIntVector(job->Width,job->ImageHeight,1));
                    const FRHITransitionInfo after(job->Texture.GetReference(),ERHIAccess::CopySrc,ERHIAccess::SRVMask);
                    transition(reinterpret_cast<FRHIComputeCommandList*>(&immediate),MakeArrayView(&after,1),ERHITransitionCreateFlags::None);
                }
                else
#endif
                Operations::Copy(job->Readback, list, job->Texture.GetReference());
                Enter(job, Phase::CopySubmitted);
                Enter(job, Phase::CopyPending);
                return; // Never map in the submission command.
            }

            const auto ready = Operations::Check(job->Readback);
            if (ready == Operations::Readiness::Pending) return;
            if (ready != Operations::Readiness::Complete)
            { Fail(job, "copy readiness fence/staging/backend invalid; retirement unproven", true); return; }
            Enter(job, Phase::ReadbackReady);
            if (job->Result.load() == Outcome::Cancelled) { Cleanup(job); return; }

            auto* mapped = static_cast<const uint8_t*>(Operations::Map(job->Readback, job->Pitch, job->Height));
            if (!mapped)
            {
                job->CompletionMs = double(NowNs() - job->StartNs.load()) / 1e6;
                job->CompletionFrames = job->Frame.load() - job->StartFrame.load();
                job->DataReady.store(true, std::memory_order_release);
                Fail(job, "native Lock returned null after readiness; mapping state unproven", true);
                return;
            }
            bool dimensions = false;
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
            if (job->External)
            {
                size_t row=0,total=0,source=0;
                dimensions=job->Pitch>0 && job->Pitch<=16384 && job->Height>0 && job->Height<=16384 &&
                    MiniMapFloatPixels::Size(job->Width,job->ImageHeight,size_t(job->Pitch),size_t(job->Height),row,total,source);
                if (dimensions) dimensions=MiniMapFloatPixels::CopyRows(mapped,source,job->Width,job->ImageHeight,job->Pitch,job->Height,job->FloatCpu);
            }
            else
#endif
            {
                dimensions=job->Pitch>=Px::Width && job->Pitch<=16384 && job->Height>=Px::Height;
                if (dimensions) for(int y=0;y<Px::Height;++y)
                    std::memcpy(job->Cpu.data()+size_t(y)*Px::Width*4,mapped+size_t(y)*size_t(job->Pitch)*4,Px::Width*4);
            }
            Operations::Unmap(job->Readback);
            job->CompletionMs = double(NowNs() - job->StartNs.load()) / 1e6;
            job->CompletionFrames = job->Frame.load() - job->StartFrame.load();
            if (dimensions)
            {
                Enter(job, Phase::PixelsCopied);
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
                if (job->External) dimensions=MiniMapFloatPixels::Analyze(job->FloatCpu.data(),job->FloatCpu.size(),job->Width,job->ImageHeight,job->FloatStatistics);
                else
#endif
                job->Verification = Px::CopyAndVerify(job->Cpu, Px::Width, Px::Height);
                job->HasPixels = true;
            }
            job->DataReady.store(true, std::memory_order_release);
            if (!dimensions) Fail(job, "invalid staging pitch/height (unmapped)", false);
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
            else if (job->External && job->FloatStatistics.Nonfinite) Fail(job,"capture contains nonfinite channels",false);
#endif
            else if (job->Verification.Failure != Px::Error::None || job->Verification.Mismatches)
                Fail(job, "pixel verification mismatch", false);
            else
            {
                auto pending = Outcome::Pending;
                if (job->Result.compare_exchange_strong(pending, Outcome::Verified)) Enter(job, Phase::Verified);
            }
            Cleanup(job);
        }

        // The callable stores ONLY the Job pointer. Counting its lifetime (not
        // merely Execute's return) prevents job deletion/overlapping polls while
        // native callable destruction still references it.
        struct Command
        {
            Job* State;
            explicit Command(Job* job) : State(job) { State->Callables.fetch_add(1); }
            Command(const Command& other) : Command(other.State) {}
            Command(Command&& other) noexcept : State(other.State) { other.State = nullptr; }
            ~Command() { if (State) State->Callables.fetch_sub(1, std::memory_order_release); }
            void operator()(FRHICommandListImmediate& list) const
            {
                try { Execute(State, list); }
                catch (...) { Fail(State, "C++ exception in native command; retirement unproven", true); }
                State->Outstanding.store(false, std::memory_order_release);
            }
        };
        static_assert(sizeof(Command) == sizeof(void*));
        static_assert(sizeof(UE::Core::Private::Function::TFunction_OwnedObject<Command, true, false>) <= 24);
    }

    Job* CreateJob()
    {
        auto* job = new (std::nothrow) Job;
        if (!job) return nullptr;
        for (int y = 0; y < Px::Height; ++y)
            for (int x = 0; x < Px::Width; ++x)
            {
                const auto color = Px::Expected(x, y);
                const auto i = size_t(y * Px::Width + x) * 4;
                job->Initial[i] = color.B; job->Initial[i + 1] = color.G;
                job->Initial[i + 2] = color.R; job->Initial[i + 3] = color.A;
            }
        return job;
    }

#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
    Job* CreateExternalJob(void* resource,uint32_t width,uint32_t height)
    {
        if (!resource || !width || !height || width>4096 || height>4096) return nullptr;
        auto* job=new (std::nothrow) Job;
        if (job)
        {
            try { job->FloatCpu.resize(size_t(width)*height*8); }
            catch (...) { delete job; return nullptr; }
            job->External=true; job->TargetResource=resource; job->Width=width; job->ImageHeight=height;
        }
        return job;
    }
    bool DiscardExternalBeforeCapture(Job*& job)
    {
        if (!job) return true;
        if (!job->External || job->Current.load()!=Phase::ReadyToSubmit ||
            job->Callables.load() || job->Outstanding.load()) return false;
        delete job; job=nullptr; return true;
    }
    const MiniMapFloatPixels::Stats* FloatStats(const Job* job)
    { return job && job->External && job->DataReady.load(std::memory_order_acquire) && job->HasPixels ? &job->FloatStatistics : nullptr; }
    const std::vector<uint8_t>* FloatPixels(const Job* job)
    { return FloatStats(job) ? &job->FloatCpu : nullptr; }
#endif
    void Service(Job* job, uint64_t frame)
    {
        if (!job) return;
        job->Frame.store(frame);
        if (job->Quarantined.load() || job->Callables.load(std::memory_order_acquire)) return;
        const auto phase = job->Current.load(std::memory_order_acquire);
        if (phase != Phase::ReadyToSubmit && phase != Phase::CopyPending) return;
        if (phase == Phase::ReadyToSubmit && job->Result.load() == Outcome::Cancelled
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
            && !job->External
#endif
            )
        { Enter(job, Phase::CleanupComplete); return; }
        bool available = false;
        if (!job->Outstanding.compare_exchange_strong(available, true)) return;
        const bool initial = phase == Phase::ReadyToSubmit;
        if (initial)
        {
            job->StartNs.store(NowNs()); job->StartFrame.store(frame);
            Enter(job, Phase::CommandQueued);
        }
        RenderCallable callable{Command(job)};
        if (!Operations::Submit(initial ? TEXT("MiniMapR1Submit") : TEXT("MiniMapR1Poll"),
            initial ? job->SubmitSpec : job->PollSpec, MoveTemp(callable)))
        {
            job->Outstanding.store(false, std::memory_order_release);
            Fail(job, "render-command submission prerequisites unavailable", !initial);
            if (initial)
            {
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
                if (job->External) { job->Quarantined.store(true); Enter(job,Phase::CleanupPending); }
                else
#endif
                Enter(job, Phase::CleanupComplete);
            }
        }
    }

    void Cancel(Job* job)
    {
        if (!job) return;
        auto pending = Outcome::Pending;
        if (job->Result.compare_exchange_strong(pending, Outcome::Cancelled))
            job->Visited.fetch_or(1u << uint32(Phase::Cancelled), std::memory_order_release);
    }

    Report Inspect(const Job* job)
    {
        Report report;
        if (!job) return report;
        report.Current = job->Current.load(std::memory_order_acquire);
        report.Visited = job->Visited.load(std::memory_order_acquire);
        report.Callables = job->Callables.load(std::memory_order_acquire);
        report.Outstanding = job->Outstanding.load(std::memory_order_acquire);
        report.Quarantined = job->Quarantined.load(std::memory_order_acquire);
        report.Result = job->Result.load(std::memory_order_acquire);
        report.Failure = job->Failure.load(std::memory_order_acquire);
        if (job->DataReady.load(std::memory_order_acquire))
        {
            report.HasPixels = job->HasPixels;
            report.Pitch = job->Pitch; report.BufferHeight = job->Height;
            report.CompletionMs = job->CompletionMs; report.CompletionFrames = job->CompletionFrames;
            report.Mismatches = job->Verification.Mismatches;
            report.FirstX = job->Verification.FirstX; report.FirstY = job->Verification.FirstY;
            report.Expected = job->Verification.FirstExpected; report.Actual = job->Verification.FirstActual;
        }
        return report;
    }

    bool DestroyIfRetired(Job*& job)
    {
        if (!job) return true;
        if (!job->Callables.load(std::memory_order_acquire) && !job->Outstanding.load() &&
            job->Current.load() == Phase::ReadyToSubmit && job->Result.load() == Outcome::Cancelled)
        {
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
            if (!job->External)
#endif
            Enter(job, Phase::CleanupComplete);
        }
        if (job->Callables.load(std::memory_order_acquire) || job->Outstanding.load() ||
            job->Current.load(std::memory_order_acquire) != Phase::CleanupComplete) return false;
        // Callback destruction release/acquire orders all native member accesses.
        if (job->Readback || job->Texture) return false;
        delete job;
        job = nullptr;
        return true;
    }
}

// Real UE header dependencies, dispatched only after native resolution. There
// are no global owning UE objects and no engine calls in static constructors.
void* FMemory::Malloc(SIZE_T size, uint32 alignment)
{
    const auto fn = MiniMapReadbackNative::GetBindings().Malloc;
    if (!fn) std::abort();
    return reinterpret_cast<void*(*)(SIZE_T, uint32)>(fn)(size, alignment);
}
void FMemory::Free(void* ptr)
{
    const auto fn = MiniMapReadbackNative::GetBindings().Free;
    if (!fn) std::abort();
    reinterpret_cast<void(*)(void*)>(fn)(ptr);
}
void FRHIResource::MarkForDelete() const
{
    const auto fn = MiniMapReadbackNative::GetBindings().MarkForDelete;
    if (!fn) std::abort();
    reinterpret_cast<void(*)(const FRHIResource*)>(fn)(this);
}
FNameEntryId FNameEntryId::FromValidEName(EName name)
{
    const auto fn = MiniMapReadbackNative::GetBindings().FromValidEName;
    if (!fn) std::abort();
    FNameEntryId result;
    // CL-127004 0x014B68C0: RCX=result storage, EDX=EName, RAX=result.
    reinterpret_cast<FNameEntryId*(*)(FNameEntryId*, EName)>(fn)(&result, name);
    return result;
}
#endif
