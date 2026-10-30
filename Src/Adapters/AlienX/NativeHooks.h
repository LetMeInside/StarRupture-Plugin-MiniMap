#pragma once

#include "Services/NativeHooks.h"
#include "plugin_interface.h"
#include <list>

namespace MiniMap::Adapters::AlienX
{
    class NativeHooks final : public Services::INativeHooks
    {
        struct Entry
        {
            Services::Hook Token;
            std::uintptr_t Target = 0;
            void* Detour = nullptr;
            HookHandle Backend = nullptr;
            void* Original = nullptr;
            Services::HookInstallState State = Services::HookInstallState::BackendUnknown;
            bool RemovalRequested = false;
            Services::RegistryRemoval Registry = Services::RegistryRemoval::NotRequested;
        };
        // Installation/removal retain the existing lifecycle serialization.
        std::list<Entry> Entries;
        std::uint64_t NextToken = 1;
    public:
        Services::HookInstallation Install(const Services::NativeDetour&) override;
        Services::Removal Remove(Services::Hook) override;
    };
}
