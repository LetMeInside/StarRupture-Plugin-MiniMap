#pragma once

#include "Services/RequiredScanner.h"

struct IPluginSelf;
struct IPluginHookScanner;

namespace MiniMap::Adapters::AlienX
{
    // Bootstrap-local borrow. The loader table and identity must stay valid
    // throughout OnPluginLoadHooks; no instance may outlive that callback.
    class RequiredScanner final : public Services::IRequiredScanner
    {
        IPluginSelf& Self;
        IPluginHookScanner& Scanner;
        bool Failed = false;

    public:
        RequiredScanner(IPluginSelf& self, IPluginHookScanner& scanner) noexcept;
        RequiredScanner(const RequiredScanner&) = delete;
        RequiredScanner& operator=(const RequiredScanner&) = delete;

        Services::Result<std::uintptr_t> Resolve(const Services::RequiredScan&) override;
        void Fail(std::string_view name, std::string_view reason) override;
        bool HasFailures();
    };
}
