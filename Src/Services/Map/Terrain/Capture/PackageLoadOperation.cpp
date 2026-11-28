// Uses genuine UE 5.6.1 checked delegates in one configuration-isolated translation unit.
// Engine-mandated bridge definitions dispatch to typed native bindings, including the engine's own TLS.
#if defined(MODLOADER_CLIENT_BUILD)
#include "Native/PackageLoadUEConfiguration.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackagePath.h"
#include "PackageLoadOperation.h"
#include "PreloadWorldTypeAssociation.h"
#include "Native/NativeApi.h"
#include <cstdlib>
#include <exception>
#include <type_traits>
#include <utility>
#include <algorithm>

using Services::Map::Terrain::Capture::PackageLoadOperation;

// Select the exact genuine overload only after UE has defined its enum and checked delegate.
using NativePackageLoad = decltype(static_cast<int32 (*)(const FPackagePath&, FName,
    FLoadPackageAsyncDelegate, EPackageFlags, int32, TAsyncLoadPriority,
    const FLinkerInstancingContext*, uint32)>(&::LoadPackageAsync));

static_assert(sizeof(FName) == 8 && alignof(FName) == 4);
static_assert(sizeof(FPackagePath) == 12 && alignof(FPackagePath) == 4);
static_assert(sizeof(FDelegateAllocation) == 16 && alignof(FDelegateAllocation) == 8);
static_assert(offsetof(FDelegateAllocation, DelegateSize) == 8);
static_assert(sizeof(FMRSWRecursiveAccessDetector) == 8);
static_assert(sizeof(TDelegateAccessHandlerBase<FNotThreadSafeDelegateMode>) == 8);
static_assert(sizeof(FMRSWRecursiveAccessDetector::FDestructionSentinel) == 24);
static_assert(offsetof(FMRSWRecursiveAccessDetector::FDestructionSentinel, Accessor) == 8);
static_assert(offsetof(FMRSWRecursiveAccessDetector::FDestructionSentinel, bDestroyed) == 16);
static_assert(sizeof(TDelegateBase<FNotThreadSafeDelegateMode>) == 24);
static_assert(sizeof(FLoadPackageAsyncDelegate) == 24 && alignof(FLoadPackageAsyncDelegate) == 8);
static_assert(sizeof(FDelegateHandle) == 8 && sizeof(EAsyncLoadingResult::Type) == 1);
static_assert(std::is_same_v<FDefaultDelegateUserPolicy::FThreadSafetyMode, FNotThreadSafeDelegateMode>);
static_assert(sizeof(EPackageFlags) == sizeof(uint32));
static_assert(WITH_CASE_PRESERVING_NAME == 0 && UE_FNAME_OUTLINE_NUMBER == 0);
static_assert(UE_WITH_REMOTE_OBJECT_HANDLE == 0);
static_assert(UE_ENABLE_ARRAY_SLACK_TRACKING == 0);

/** Implements the actual UE-declared allocation symbol; native allocation owns its allocator bookkeeping. */
void* UE::Core::Private::DelegateAllocate(size_t size, FDelegateAllocation& allocation)
{
    return PackageLoadOperation::RequireNativeBindings().allocate(size, allocation);
}

/** Generates native-global unique delegate IDs rather than maintaining a plugin-local counter. */
uint64 FDelegateHandle::GenerateNewID()
{
    return PackageLoadOperation::RequireNativeBindings().generateHandle();
}

/** Uses the exact engine-owned TLS container; no private substitute TLS stack is constructed. */
auto FMRSWRecursiveAccessDetector::GetReadersTls() -> FReadersTls&
{
    static_assert(sizeof(FReadersTls) == 80 && alignof(FReadersTls) == 8);
    static_assert(sizeof(FReadersTls::ElementType) == 16);
    return *static_cast<FReadersTls*>(PackageLoadOperation::RequireNativeBindings().readersTls());
}

/** Shares destruction sentinels across plugin-inline and native delegate operations. */
auto FMRSWRecursiveAccessDetector::GetDestructionSentinelStackTls() -> FDestructionSentinelStackTls&
{
    static_assert(sizeof(FDestructionSentinelStackTls) == 48 && alignof(FDestructionSentinelStackTls) == 8);
    return *static_cast<FDestructionSentinelStackTls*>(
        PackageLoadOperation::RequireNativeBindings().destructionTls());
}

/** Preserves the native allocator on checked detector TLS heap growth as well as delegate destruction. */
void* FMemory::Realloc(void* pointer, SIZE_T size, uint32 alignment)
{
    return PackageLoadOperation::RequireNativeBindings().reallocate(pointer, size, alignment);
}

/** Uses the installed allocator's quantization policy for genuine TLS container slack calculations. */
SIZE_T FMemory::QuantizeSize(SIZE_T size, uint32 alignment)
{
    return PackageLoadOperation::RequireNativeBindings().quantizeSize(size, alignment);
}

// UE declares this specialization extern template after defining its primary member inline.
// Supply its unresolved MSVC x64 linkage through a C ABI thunk, avoiding a conflicting specialization
// or local instantiation of the allocator implementation. The name is verified in the Client PDB.
#pragma comment(linker, "/alternatename:?ResizeAllocation@ForAnyElementType@?$TSizedHeapAllocator@$0CA@UFMemory@@@@QEAAXHH_K@Z=MiniMapPackageLoadResizeHeapAllocation")

/** Forwards the genuine allocator object and all three arguments using the verified Windows x64 ABI. */
extern "C" void MiniMapPackageLoadResizeHeapAllocation(void* allocator, int32 currentCount,
    int32 newCapacity, SIZE_T bytesPerElement)
{
    static_assert(sizeof(TSizedHeapAllocator<32, FMemory>::ForAnyElementType) == 8);
    static_assert(alignof(TSizedHeapAllocator<32, FMemory>::ForAnyElementType) == 8);
    static_assert(std::is_same_v<int32, std::int32_t> && std::is_same_v<SIZE_T, std::size_t>);
    PackageLoadOperation::RequireNativeBindings().resizeHeapAllocation(
        allocator, currentCount, newCapacity, bytesPerElement);
}

// Preserve the exact four-argument extern-template symbol independently of the three-argument alias.
#pragma comment(linker, "/alternatename:?ResizeAllocation@ForAnyElementType@?$TSizedHeapAllocator@$0CA@UFMemory@@@@QEAAXHH_KI@Z=MiniMapPackageLoadResizeAlignedHeapAllocation")

/** Borrows the genuine allocator and forwards counts, element size, and alignment to native reallocation. */
extern "C" void MiniMapPackageLoadResizeAlignedHeapAllocation(void* allocator, int32 currentCount,
    int32 newCapacity, SIZE_T bytesPerElement, uint32 alignmentOfElement)
{
    using NativeAllocator = TSizedHeapAllocator<32, FMemory>::ForAnyElementType;
    using NativeResize = void (NativeAllocator::*)(int32, int32, SIZE_T, uint32);
    static_assert(std::is_same_v<decltype(static_cast<NativeResize>(&NativeAllocator::ResizeAllocation)),
        NativeResize>);
    static_assert(std::is_same_v<uint32, std::uint32_t>);
    static_assert(std::is_same_v<decltype(MiniMapPackageLoadResizeAlignedHeapAllocation),
        MiniMapNative::PackageLoadApi::ResizeAlignedHeapAllocation>);
    PackageLoadOperation::RequireNativeBindings().resizeAlignedHeapAllocation(
        allocator, currentCount, newCapacity, bytesPerElement, alignmentOfElement);
}

/** Forwards genuine header checks through the native va_list entry, including failure-path diagnostics. */
bool FDebug::CheckVerifyFailedImpl2(const ANSICHAR* expression, const ANSICHAR* file,
    int32 line, const TCHAR* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const bool result = PackageLoadOperation::RequireNativeBindings().checkFailure(
        expression, file, line, format, arguments);
    va_end(arguments);
    return result;
}

/** Implements the UE-mandated invalid-array entry required by genuine TArray inline operations. */
void UE::Core::Private::OnInvalidArrayNum(unsigned long long count)
{
    PackageLoadOperation::RequireNativeBindings().invalidArrayCount(count);
    std::abort();
}

namespace Services::Map::Terrain::Capture
{
    /** Carries callback-owned state only; it never captures the operation or a TerrainService pointer. */
    struct PackageLoadOperation::State final
    {
        FName Destination;
        PreloadWorldTypeAssociation Preload;
        SDK::UPackage* Package = nullptr;
        SDK::UWorld* World = nullptr;
        MiniMapNative::AssetApi::ReleaseObjectFn Release = nullptr;
        Result Outcome = Result::Pending;
        bool Cancelled = false;
        bool Completed = false;

        /** Releases the callback's retained package only after the owning operation acknowledges native retirement. */
        ~State()
        {
            if (World)
                Release(reinterpret_cast<SDK::UObject*>(World));
            if (Package)
                Release(reinterpret_cast<SDK::UObject*>(Package));
        }

        /** Consumes a game-thread completion and retains a late package even for cancelled logical work. */
        void Complete(const FName& name, UPackage* package, EAsyncLoadingResult::Type result)
        {
            const auto* api = MiniMapNative::Get();
            if (!api || !api->player.isGameThread())
                std::abort();
            Package = reinterpret_cast<SDK::UPackage*>(package);
            if (Package)
            {
                api->asset.retainObject(reinterpret_cast<SDK::UObject*>(Package));
                World = api->captureAdmission.findWorldInPackage(Package);
                if (World)
                    api->asset.retainObject(reinterpret_cast<SDK::UObject*>(World));
            }
            Outcome = result == EAsyncLoadingResult::Canceled ? Result::Cancelled :
                (result == EAsyncLoadingResult::Succeeded && Package && name == Destination
                    ? Result::Succeeded : Result::Failed);
            Completed = true;
        }
    };

    /** Keeps all native bridge calls tied to the single complete feature binding lifetime. */
    const MiniMapNative::PackageLoadApi& PackageLoadOperation::RequireNativeBindings()
    {
        const auto* api = MiniMapNative::Get();
        if (!api || !api->packageLoad.IsComplete())
            std::abort();
        return api->packageLoad;
    }

    /** Requires game-thread, nonreentrant use; no native-memory probing occurs. */
    bool PackageLoadOperation::CanEnterNative() const noexcept
    {
        const auto* api = MiniMapNative::Get();
        return !insideNativeCall_ && api && api->player.isGameThread && api->player.isGameThread() &&
            api->packageLoad.IsComplete() && api->captureAdmission.findWorldInPackage &&
            api->asset.retainObject && api->asset.releaseObject;
    }

    /** Moves a genuine checked delegate into the verified custom-name native package overload. */
    bool PackageLoadOperation::Submit(std::int32_t sourceComparisonIndex, std::uint32_t sourceNumber,
        std::int32_t instanceComparisonIndex, std::uint32_t instanceNumber)
    {
        if (state_ || !CanEnterNative() ||
            (sourceComparisonIndex == instanceComparisonIndex && sourceNumber == instanceNumber) ||
            sourceComparisonIndex <= 0 || instanceComparisonIndex <= 0)
            return false;
        const auto& bindings = RequireNativeBindings();
        const FName sourceName = FName::CreateFromDisplayId(
            FNameEntryId::FromUnstableInt(sourceComparisonIndex), sourceNumber);
        const FName destinationName = FName::CreateFromDisplayId(
            FNameEntryId::FromUnstableInt(instanceComparisonIndex), instanceNumber);
        FPackagePath path;
        if (!bindings.makePath(sourceName, path))
            return false;
        state_ = std::make_shared<State>();
        state_->Destination = destinationName;
        state_->Release = MiniMapNative::Get()->asset.releaseObject;
        if (!state_->Preload.Acquire(instanceComparisonIndex, instanceNumber))
        {
            state_->Outcome = Result::Failed;
            return false;
        }
        auto completion = FLoadPackageAsyncDelegate::CreateLambda(
            [state = state_](const FName& name, UPackage* package, EAsyncLoadingResult::Type result)
            {
                state->Complete(name, package, result);
            });
        nativeRetired_ = false;
        insideNativeCall_ = true;
        const auto loadPackage = reinterpret_cast<NativePackageLoad>(bindings.load);
        requestId_ = loadPackage(path, destinationName, MoveTemp(completion), PKG_None,
            INDEX_NONE, 0, nullptr, LOAD_None);
        insideNativeCall_ = false;
        if (requestId_ <= 0 && !state_->Completed)
            state_->Outcome = Result::Failed;
        return requestId_ > 0;
    }

    /** Cancels delivery without touching native async-loading queues or package requests. */
    void PackageLoadOperation::Cancel()
    {
        if (!state_)
            return;
        if (!CanEnterNative())
            std::abort();
        if (state_)
            state_->Cancelled = true;
    }

    /** Establishes a native return boundary and proves that no engine delegate copy retains callback state. */
    bool PackageLoadOperation::WaitForNativeRetirement()
    {
        if (PollNativeRetirement())
            return true;
        if (!CanEnterNative())
            return false;
        const auto& bindings = RequireNativeBindings();
        const auto suspended = *bindings.loadingSuspended;
        if (!suspended || suspended())
            return false;
        if (requestId_ > 0)
        {
            insideNativeCall_ = true;
            bindings.flushRequest(requestId_);
            insideNativeCall_ = false;
        }
        nativeRetired_ = state_.use_count() == 1 && (requestId_ <= 0 || state_->Completed);
        return nativeRetired_;
    }

    /** Exposes only the explicit post-flush acknowledgement, never a completion-only guess. */
    bool PackageLoadOperation::IsNativeRetired() const noexcept
    {
        return nativeRetired_;
    }

    /** Completion plus exclusive callback-state ownership acknowledges that all native delegate copies died. */
    bool PackageLoadOperation::PollNativeRetirement() noexcept
    {
        if (!nativeRetired_ && CanEnterNative() && state_ && (requestId_ <= 0 || state_->Completed) &&
            state_.use_count() == 1)
            nativeRetired_ = true;
        return nativeRetired_;
    }

    /** Borrows the extra retained package until the owner destroys this retired operation. */
    SDK::UPackage* PackageLoadOperation::GetRetainedPackage() const noexcept
    {
        return nativeRetired_ && state_ ? state_->Package : nullptr;
    }

    /** A package root alone does not promise cooked export residency; completion retained this world explicitly. */
    SDK::UWorld* PackageLoadOperation::GetRetainedWorld() const noexcept
    {
        return nativeRetired_ && state_ ? state_->World : nullptr;
    }

    /** Adopts the existing native reference, avoiding a second package owner after callback destruction. */
    SDK::UPackage* PackageLoadOperation::TransferRetainedPackage(std::vector<SDK::UObject*>& nativeOwners)
    {
        if (!nativeRetired_ || !CanEnterNative() || !state_ || !state_->Package)
            return nullptr;
        auto* package = state_->Package;
        auto* object = reinterpret_cast<SDK::UObject*>(package);
        auto* worldObject = reinterpret_cast<SDK::UObject*>(state_->World);
        nativeOwners.reserve(nativeOwners.size() + 2);
        if (std::find(nativeOwners.begin(), nativeOwners.end(), object) == nativeOwners.end())
            nativeOwners.push_back(object);
        else
            state_->Release(object);
        state_->Package = nullptr;
        if (worldObject)
        {
            if (std::find(nativeOwners.begin(), nativeOwners.end(), worldObject) == nativeOwners.end())
                nativeOwners.push_back(worldObject);
            else
                state_->Release(worldObject);
            state_->World = nullptr;
        }
        return package;
    }

    /** Keeps cancellation/failure distinct from native callback and delegate lifetime. */
    PackageLoadOperation::Result PackageLoadOperation::GetResult() const noexcept
    {
        if (!state_)
            return Result::Pending;
        return state_->Cancelled ? Result::Cancelled : state_->Outcome;
    }

    /** Forbids destroying code/state with native debt; successful destruction releases the package once. */
    PackageLoadOperation::~PackageLoadOperation()
    {
        if (!nativeRetired_ || (state_ && !CanEnterNative()))
            std::terminate();
        if (state_ && !state_->Preload.Release())
            std::terminate();
    }
}
#endif
