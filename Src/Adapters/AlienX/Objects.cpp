#include "Objects.h"

#include "Compatibility.h"
#include "plugin_helpers.h"

#include <algorithm>
#include <limits>
#include <vector>
#include <Windows.h>

namespace
{
    using namespace MiniMap::Services;

    Status Failure(Error error, const char* detail)
    {
        return {error, detail};
    }

    Result<SDK::UObject*> FailedObject(Error error, const char* detail)
    {
        return {std::nullopt, Failure(error, detail)};
    }

    Result<MiniMap::Services::ObjectMatches> FailedMatches(Error error, const char* detail)
    {
        return {std::nullopt, Failure(error, detail)};
    }

    std::size_t BoundedLength(const char* value, std::size_t capacity) noexcept
    {
        std::size_t length = 0;
        while (length < capacity && value[length] != '\0') ++length;
        return length;
    }

    bool InvokeFind(std::uintptr_t address, SDK::UClass* type, SDK::UObject* outer,
        const wchar_t* path, bool exact, SDK::UObject*& result) noexcept
    {
        using Find = SDK::UObject*(*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
        __try
        {
            result = reinterpret_cast<Find>(address)(type, outer, path, exact);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    bool InvokeFree(IPluginMemoryUtils* memory, void* allocation) noexcept
    {
        __try
        {
            memory->Free(allocation);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }
}

namespace MiniMap::Adapters::AlienX
{
    Objects& GetObjects() noexcept
    {
        static Objects instance;
        return instance;
    }

    bool Objects::Ready() const noexcept
    {
        try
        {
            auto* self = GetSelf();
            auto* hooks = self ? self->hooks : nullptr;
            auto* engine = hooks ? hooks->Engine : nullptr;
            auto* walker = hooks ? hooks->ObjectWalker : nullptr;
            return engine && engine->GetStaticFindObjectByNameAddress &&
                walker && walker->IsReady && walker->FindObjectsByClassNameInto && walker->InvokeResolvedUFunction &&
                engine->GetStaticFindObjectByNameAddress() != 0;
        }
        catch (...) { return false; }
    }

    Result<SDK::UObject*> Objects::FindResident(SDK::UClass* type, SDK::UObject* outer,
        std::wstring_view path, bool exactClass)
    {
        if (path.empty() || path.find(L'\0') != std::wstring_view::npos)
            return FailedObject(Error::InvalidArgument, "resident object path is empty or contains NUL");
        try
        {
            auto* self = GetSelf();
            auto* engine = self && self->hooks ? self->hooks->Engine : nullptr;
            if (!engine || !engine->GetStaticFindObjectByNameAddress)
                return FailedObject(Error::Unavailable, "AlienX resident lookup is unavailable");
            const auto address = engine->GetStaticFindObjectByNameAddress();
            if (!address) return FailedObject(Error::Unavailable, "AlienX resident lookup address is unavailable");
            std::wstring ownedPath(path);
            SDK::UObject* object = nullptr;
            if (!InvokeFind(address, type, outer, ownedPath.c_str(), exactClass, object))
                return FailedObject(Error::BackendFailure, "resident lookup invocation was interrupted");
            return {object, {}};
        }
        catch (...)
        {
            return FailedObject(Error::BackendFailure, "resident lookup failed");
        }
    }

    Result<ObjectMatches> Objects::ClassDefaults(std::string_view className,
        std::span<ObjectRecord> output)
    {
        if (className.empty() || className.find('\0') != std::string_view::npos ||
            output.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
            return FailedMatches(Error::InvalidArgument, "class-default lookup arguments are invalid");
        auto* self = GetSelf();
        auto* walker = self && self->hooks ? self->hooks->ObjectWalker : nullptr;
        if (!walker || !walker->IsReady || !walker->FindObjectsByClassNameInto)
            return FailedMatches(Error::Unavailable, "AlienX object walker is unavailable");
        try
        {
            if (!walker->IsReady()) return FailedMatches(Error::Unavailable, "AlienX object walker is not ready");
            std::string ownedName(className);
            std::vector<PluginObjectInfo> native(output.size());
            const int capacity = static_cast<int>(native.size());
            const int total = walker->FindObjectsByClassNameInto(ownedName.c_str(), PluginObjectLookup_CDOOnly,
                native.empty() ? nullptr : native.data(), capacity);
            if (total < 0) return FailedMatches(Error::BackendFailure, "AlienX object walker returned a negative count");
            const auto written = (std::min)(static_cast<std::size_t>(total), output.size());
            for (std::size_t i = 0; i < written; ++i)
            {
                output[i].object = static_cast<SDK::UObject*>(native[i].object);
                output[i].objectName.assign(native[i].objectName,
                    BoundedLength(native[i].objectName, sizeof(native[i].objectName)));
                output[i].className.assign(native[i].className,
                    BoundedLength(native[i].className, sizeof(native[i].className)));
            }
            return {ObjectMatches{static_cast<std::size_t>(total)}, {}};
        }
        catch (...)
        {
            return FailedMatches(Error::BackendFailure, "AlienX class-default lookup failed");
        }
    }

    Status Objects::Invoke(SDK::UObject* object, SDK::UFunction* function, void* parameterBuffer)
    {
        if (!object || !function) return Failure(Error::InvalidArgument, "reflection invocation requires object and function");
        auto* self = GetSelf();
        auto* walker = self && self->hooks ? self->hooks->ObjectWalker : nullptr;
        if (!walker || !walker->IsReady || !walker->InvokeResolvedUFunction)
            return Failure(Error::Unavailable, "AlienX reflection invocation is unavailable");
        try
        {
            if (!walker->IsReady()) return Failure(Error::Unavailable, "AlienX object walker is not ready");
            return walker->InvokeResolvedUFunction(object, function, parameterBuffer)
                ? Status{} : Failure(Error::BackendFailure, "AlienX reflection invocation was rejected");
        }
        catch (...)
        {
            return Failure(Error::BackendFailure, "AlienX reflection invocation failed");
        }
    }

    Status Objects::FreeEngineAllocation(void* allocation) noexcept
    {
        if (!allocation) return {};
        try
        {
            auto* self = GetSelf();
            auto* memory = self && self->hooks ? self->hooks->Memory : nullptr;
            if (!memory || !memory->Free || !memory->IsAllocatorAvailable)
                return {Error::Unavailable, {}};
            if (!memory->IsAllocatorAvailable())
                return {Error::Unavailable, {}};
            if (!InvokeFree(memory, allocation))
                return {Error::OwnershipUncertain, {}};
            return {};
        }
        catch (...)
        {
            return {Error::OwnershipUncertain, {}};
        }
    }
}
