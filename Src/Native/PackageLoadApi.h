// Carries native dependencies for the isolated checked-delegate bridge.
// The package-load entry is transported without redeclaring UE's non-forward-declarable flags enum.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <cstdint>
#include <cstddef>
#include <cstdarg>

class FName;
class FPackagePath;
struct FDelegateAllocation;
struct FNameEntryId;

namespace MiniMapNative
{
    /** Borrows all checked-delegate/load dependencies from the single NativeApi instance.
     * The owner must finish native callbacks and their destruction before clearing these bindings.
     */
    struct PackageLoadApi final
    {
        using Allocate = void*(std::size_t, FDelegateAllocation&);
        using GenerateHandle = std::uint64_t();
        using NativeTls = void*();
        using Reallocate = void*(void*, std::size_t, std::uint32_t);
        using QuantizeSize = std::size_t(std::size_t, std::uint32_t);
        // x64 member ABI: the unchanged genuine allocator object is the first argument.
        using ResizeHeapAllocation = void(void*, std::int32_t, std::int32_t, std::size_t);
        using ResizeAlignedHeapAllocation = void(void*, std::int32_t, std::int32_t, std::size_t, std::uint32_t);
        using CheckFailure = bool(const char*, const char*, std::int32_t, const wchar_t*, std::va_list);
        using InvalidArrayCount = void(unsigned long long);
        using HashNameEntry = std::uint32_t(FNameEntryId);
        using MakePath = bool(FName, FPackagePath&);
        using FlushRequest = void(std::int32_t);
        using LoadingSuspended = bool();

        Allocate* allocate = nullptr;
        GenerateHandle* generateHandle = nullptr;
        NativeTls* readersTls = nullptr;
        NativeTls* destructionTls = nullptr;
        Reallocate* reallocate = nullptr;
        QuantizeSize* quantizeSize = nullptr;
        ResizeHeapAllocation* resizeHeapAllocation = nullptr;
        ResizeAlignedHeapAllocation* resizeAlignedHeapAllocation = nullptr;
        CheckFailure* checkFailure = nullptr;
        InvalidArrayCount* invalidArrayCount = nullptr;
        InvalidArrayCount* invalidSetCount = nullptr;
        HashNameEntry* hashNameEntry = nullptr;
        void* worldTypePreloadMap = nullptr; // Restored to its genuine TMap type only in the UE bridge.
        MakePath* makePath = nullptr;
        // Transport only: the isolated UE bridge restores the genuine overload type.
        std::uintptr_t load = 0;
        FlushRequest* flushRequest = nullptr;
        LoadingSuspended* const* loadingSuspended = nullptr;

        /** Requires the whole dependency set, including error/destruction paths. */
        bool IsComplete() const noexcept
        {
            return allocate && generateHandle && readersTls && destructionTls && reallocate &&
                quantizeSize && resizeHeapAllocation && resizeAlignedHeapAllocation &&
                checkFailure && invalidArrayCount && invalidSetCount && hashNameEntry && worldTypePreloadMap &&
                makePath && load && flushRequest && loadingSuspended;
        }
    };
}
#endif
