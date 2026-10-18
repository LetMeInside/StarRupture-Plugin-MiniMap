#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingBlueprintIdentity.h"
#include "Native/NativeApi.h"
#include "Native/ResidentMetadata.h"
#include "plugin_helpers.h"
#include <SDK/AuActorPlacement_classes.hpp>
#include <SDK/Engine_parameters.hpp>
#include <Windows.h>
#include <array>
#include <new>
#include <string_view>
#include <type_traits>

namespace
{
    namespace N = MiniMapNative;
    namespace R = MiniMapResidentMetadata;
    using Params = SDK::Params::KismetSystemLibrary_Conv_SoftClassReferenceToString;
    static_assert(sizeof(Params) == 0x38 && offsetof(Params, ReturnValue) == 0x28);
    static_assert(std::is_trivially_copyable_v<decltype(Params::SoftClassReference)>);
    bool g_interrupted = false;

    MiniMapBuildingBlueprintIdentity::Result Rejected(MiniMapBuildingBlueprintIdentity::Issue issue,
        const char* detail, MiniMapBuildingBlueprintIdentity::Status state = MiniMapBuildingBlueprintIdentity::Status::Unavailable)
    { return {{}, state, issue, detail}; }

    template<class T> bool Read(uintptr_t address, T& value) { return N::ReadMemory(address, &value, sizeof(value)); }
    bool Invoke(IPluginObjectWalker* walker, uintptr_t cdo, uintptr_t function, Params* params) noexcept
    {
        __try { return walker->InvokeResolvedUFunction(reinterpret_cast<void*>(cdo), reinterpret_cast<void*>(function), params); }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }
    bool Free(IPluginMemoryUtils* memory, void* value) noexcept
    {
        __try { memory->Free(value); return true; }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }
    bool Allocator(IPluginMemoryUtils* memory) noexcept
    {
        __try { return memory && memory->Free && memory->IsAllocatorAvailable && memory->IsAllocatorAvailable(); }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }
    bool FindFunction(uintptr_t address, uintptr_t type, uintptr_t library, uintptr_t& function) noexcept
    {
        using Find = SDK::UObject*(*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
        __try
        {
            function = reinterpret_cast<uintptr_t>(reinterpret_cast<Find>(address)(reinterpret_cast<SDK::UClass*>(type),
                reinterpret_cast<SDK::UObject*>(library), L"Conv_SoftClassReferenceToString", true));
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }
    bool Matches(uintptr_t definition, uintptr_t placementClass, MiniMapBuildingBlueprintIdentity::DefinitionIdentity expected)
    {
        int32_t index{}, name{}, serial{}; uint32_t number{}, building{}; uintptr_t type{};
        if (!R::Validate(definition, sizeof(SDK::UAuActorPlacementData)) || !R::Validate(placementClass, sizeof(SDK::UClass)) ||
            !Read(definition + 0xc, index) || index != expected.ObjectIndex || !Read(definition + 0x18, name) || name != expected.NameIndex ||
            !R::Serial(definition, serial) || serial != expected.ObjectSerial ||
            !Read(definition + 0x1c, number) || number != expected.NameNumber ||
            !Read(definition + offsetof(SDK::UAuActorPlacementData, BuildingID), building) || building != expected.BuildingId ||
            !Read(definition + 0x10, type)) return false;
        for (unsigned depth = 0; type && depth < 64; ++depth)
        {
            if (!R::Validate(type, sizeof(SDK::UClass))) return false;
            if (type == placementClass) return true;
            uintptr_t parent{};
            if (!Read(type + offsetof(SDK::UStruct, SuperStruct), parent) || parent == type) return false;
            type = parent;
        }
        return false;
    }
    bool Canonical(std::string_view path)
    {
        // Same representation accepted by BuildingSpriteRenderer::Path. Nothing
        // is normalized or assembled from the authored FName fragments.
        if (path.size() > 1024 || !path.starts_with("/Game/") || !path.ends_with("_C")) return false;
        for (const auto marker : {"/Engine/Transient", "UEDPIE_", "REINST_", "SKEL_"})
            if (path.find(marker) != path.npos) return false;
        const auto dot = path.find('.'), slash = path.rfind('/');
        if (dot == path.npos || dot <= slash + 1 || path.find('.', dot + 1) != path.npos ||
            path.substr(slash + 1, dot - slash - 1) != path.substr(dot + 1, path.size() - dot - 3)) return false;
        for (size_t i = 1; i < path.size(); ++i)
        {
            const auto c = static_cast<unsigned char>(path[i]);
            if (c < 128 && !(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_' && c != '/' && c != '.') return false;
            if (c == '/' && (i > dot || path[i - 1] == '/')) return false;
        }
        return true;
    }
}

namespace MiniMapBuildingBlueprintIdentity
{
    Result Resolve(const SDK::UAuActorPlacementData* definition, const SDK::UClass* placementClass, DefinitionIdentity identity)
    {
        if (g_interrupted) return Rejected(Issue::ConversionDisabled, "conversion disabled after uncertain native ownership");
        if (!R::Available()) return {};
        auto* hooks = GetHooks();
        auto* memory = hooks ? hooks->Memory : nullptr;
        auto* walker = hooks ? hooks->ObjectWalker : nullptr;
        auto* engine = hooks ? hooks->Engine : nullptr;
        const auto find = engine && engine->GetStaticFindObjectByNameAddress ? engine->GetStaticFindObjectByNameAddress() : 0;
        if (!Allocator(memory) || !walker || !walker->InvokeResolvedUFunction || !find)
            return Rejected(Issue::InvocationPrerequisites, "allocator/invocation/find prerequisites unavailable");
        const auto object = reinterpret_cast<uintptr_t>(definition), placement = reinterpret_cast<uintptr_t>(placementClass);
        if (!Matches(object, placement, identity)) return Rejected(Issue::DefinitionIdentity, "definition identity observation/validation failed");
        // Only synchronous temporary roots; no native destructor, cached pointer,
        // audit context or identity registry. Unknown native ownership is leaked
        // conservatively and disables conversion until restart, with a diagnostic.
        std::array<uintptr_t, 6> roots{};
        size_t count = 0;
        Params params{};
        bool interrupted = false;
        const auto keep = [&](uintptr_t value, size_t extent)
        { if (!R::Retain(value, extent)) return false; roots[count++] = value; return true; };
        const auto metadata = [&](const wchar_t* path, size_t size)
        { const auto value = R::Acquire(path, false, size); if (value) roots[count++] = value; return value; };
        const auto convert = [&]() -> Result
        {
            if (!keep(placement, sizeof(SDK::UClass)) || !keep(object, sizeof(SDK::UAuActorPlacementData)) || !Matches(object, placement, identity))
                return Rejected(Issue::RetainDefinition, "definition retention/identity validation failed");
            // Borrowed shallow copy; the retained definition owns subpath storage.
            if (!Read(object + offsetof(SDK::UAuActorPlacementData, ActorClass), params.SoftClassReference))
                return Rejected(Issue::SoftClassRead, "ActorClass memory read failed");
            const auto& path = params.SoftClassReference.ObjectID;
            const auto* subpath = path.SubPathString.CStr();
            const auto length = path.SubPathString.Num(), capacity = path.SubPathString.Max();
            if (path.AssetPath.PackageName.ComparisonIndex < 0 || path.AssetPath.AssetName.ComparisonIndex < 0 ||
                length < 0 || capacity < length || capacity > 1024 ||
                (capacity && (!N::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(subpath), size_t(length ? length : 1) * sizeof(*subpath), alignof(decltype(*subpath))) ||
                    (length && subpath[length - 1] != 0))) || length > 1)
                return Rejected(Issue::SoftPath, "soft class path structure/subpath rejected", Status::Invalid);
            const auto functionClass = metadata(L"/Script/CoreUObject.Function", sizeof(SDK::UFunction));
            const auto library = metadata(L"/Script/Engine.KismetSystemLibrary", sizeof(SDK::UKismetSystemLibrary));
            if (!functionClass || !library) return Rejected(Issue::ConversionMetadata, "conversion function/library metadata not resident");
            uintptr_t function{}, type{}, outer{}, exec{}, cdo{}; uint32_t flags{}; uint8_t parameters{}; uint16_t bytes{};
            if (!FindFunction(find, functionClass, library, function))
            { interrupted = true; return Rejected(Issue::FunctionLookup, "conversion function lookup interrupted", Status::Interrupted); }
            if (!function || !keep(function, sizeof(SDK::UFunction)) || !Read(function + 0x10, type) || type != functionClass ||
                !Read(function + 0x20, outer) || outer != library ||
                !Read(function + offsetof(SDK::UFunction, FunctionFlags), flags) || (flags & 0x2400) != 0x2400 ||
                !Read(function + offsetof(SDK::UFunction, ExecFunction), exec) || !exec ||
                !Read(function + 0xb4, parameters) || parameters != 2 || !Read(function + 0xb6, bytes) || bytes != sizeof(Params))
                return Rejected(Issue::FunctionMetadata, "conversion function identity/signature observation rejected");
            if (!Read(library + offsetof(SDK::UClass, ClassDefaultObject), cdo) || !keep(cdo, sizeof(SDK::UKismetSystemLibrary)) ||
                !Read(cdo + 0x10, type) || type != library || !Read(cdo + 8, flags) || !(flags & 0x10) || !R::Available())
                return Rejected(Issue::LibraryDefaultObject, "conversion library CDO/admission observation rejected");
            if (!Invoke(walker, cdo, function, &params))
            { interrupted = true; return Rejected(Issue::Invocation, "soft class string conversion interrupted", Status::Interrupted); }
            const auto& value = params.ReturnValue;
            const auto* text = value.CStr();
            const auto num = value.Num(), max = value.Max();
            // A structurally valid but overlong path is an ordinary unresolved
            // identity, not uncertain ownership of the returned allocation.
            if (num < 0 || max < num || max > 65536 || (max && !text) || (!max && text) ||
                (num && (!N::IsMemoryRangeReadable(reinterpret_cast<uintptr_t>(text), size_t(num) * sizeof(wchar_t), alignof(wchar_t)) || text[num - 1] != 0)))
            { interrupted = true; return Rejected(Issue::ReturnedAllocation, "returned string allocation malformed/unreadable", Status::Interrupted); } // Cannot safely free a malformed returned allocation.
            if (num <= 1 || num > 1025) return Rejected(Issue::ReturnedLength, "returned identity empty/overlong", Status::Invalid);
            for (int32_t i = 0; i < num - 1; ++i) if (text[i] < 32 || text[i] == 127)
                return Rejected(Issue::ReturnedCharacter, "returned identity contains control characters", Status::Invalid);
            std::array<char, 1025> utf8{};
            const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, num - 1, utf8.data(), 1024, nullptr, nullptr);
            if (size <= 0 || !Canonical(std::string_view(utf8.data(), size)))
                return Rejected(Issue::CanonicalPath, "UTF-8 conversion/canonical Blueprint syntax rejected", Status::Invalid);
            if (!Matches(object, placement, identity)) return Rejected(Issue::DefinitionChanged, "definition identity revalidation failed after conversion");
            try { return {std::make_shared<const std::string>(utf8.data(), size_t(size)), Status::Resolved}; }
            catch (const std::bad_alloc&) { return Rejected(Issue::Allocation, "canonical identity CPU allocation failed"); } // CPU allocation failure still follows checked native cleanup.
        };
        auto result = convert();
        if (interrupted || (params.ReturnValue.CStr() && !Free(memory, params.ReturnValue.CStr())))
        {
            g_interrupted = true;
            LOG_ERROR("MiniMap: authored Blueprint identity native invocation/result ownership uncertain; no speculative string release or retry, remaining native roots retained, conversion disabled until restart");
            return result.State == Status::Interrupted ? result : Rejected(Issue::NativeCleanup, "returned string release interrupted", Status::Interrupted);
        }
        for (size_t i = count; i > 0; --i)
            if (!R::Release(roots[i - 1])) return Rejected(Issue::NativeCleanup, "temporary native root release failed", Status::Interrupted);
        return result;
    }
}
#endif
