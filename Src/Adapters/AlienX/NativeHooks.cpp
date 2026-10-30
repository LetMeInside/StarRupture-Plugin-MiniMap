#include "NativeHooks.h"
#include "Compatibility.h"

#if defined(MODLOADER_CLIENT_BUILD)
namespace MiniMap::Adapters::AlienX
{
    namespace S = Services;

    S::HookInstallation NativeHooks::Install(const S::NativeDetour& detour)
    {
        if (!detour.target || !detour.entry || !detour.publishOriginal)
            return {{}, S::HookInstallState::Rejected, {S::Error::InvalidArgument, {}}};
        auto* self = GetSelf();
        auto* hooks = self && self->hooks ? self->hooks->Hooks : nullptr;
        if (!hooks || !hooks->Install || !hooks->Remove)
            return {{}, S::HookInstallState::Rejected, {S::Error::Unavailable, {}}};
        if (!NextToken) return {{}, S::HookInstallState::Rejected, {S::Error::Capacity, {}}};
        // Reserve bookkeeping before the potentially mutating call. Exceptions
        // propagate to existing bootstrap cleanup; no new recovery/transaction.
        Entries.push_back({{NextToken}, detour.target, detour.entry});
        ++NextToken;
        auto& entry = Entries.back();
        void* original = nullptr;
        entry.Backend = hooks->Install(detour.target, detour.entry, &original);
        entry.Original = original;
        if (!entry.Backend || !original)
            return {entry.Backend ? entry.Token : S::Hook{}, S::HookInstallState::BackendUnknown,
                {S::Error::OwnershipUncertain, "AlienX Install returned an incomplete handle/original pair"}};
        // Compatibility publication only: the entry may already be reachable.
        detour.publishOriginal(detour.owner, original);
        entry.State = S::HookInstallState::Active;
        return {entry.Token, entry.State, {}};
    }

    S::Removal NativeHooks::Remove(S::Hook token)
    {
        if (!token.id) return {false, S::RegistryRemoval::NotRequested, S::CoreDrain::Unknown, {}};
        Entry* entry = nullptr;
        for (auto& candidate : Entries) if (candidate.Token.id == token.id) { entry = &candidate; break; }
        if (!entry) return {false, S::RegistryRemoval::NotRequested, S::CoreDrain::Unknown, {S::Error::InvalidArgument, {}}};
        if (entry->RemovalRequested)
            return {false, entry->Registry, S::CoreDrain::Unknown,
                {entry->Registry == S::RegistryRemoval::Unknown ? S::Error::OwnershipUncertain : S::Error::None, {}}};
        auto* self = GetSelf();
        auto* hooks = self && self->hooks ? self->hooks->Hooks : nullptr;
        if (!hooks || !hooks->Remove)
            return {false, S::RegistryRemoval::NotRequested, S::CoreDrain::Unknown, {S::Error::Unavailable, {}}};
        entry->RemovalRequested = true;
        entry->Registry = S::RegistryRemoval::Unknown;
        hooks->Remove(entry->Backend);
        entry->Registry = S::RegistryRemoval::RequestReturned;
        // Neither the legacy void return nor retained thunks prove restoration,
        // logical native admission closure, core drain or executable lifetime.
        return {false, entry->Registry, S::CoreDrain::Unknown, {}};
    }
}
#endif
