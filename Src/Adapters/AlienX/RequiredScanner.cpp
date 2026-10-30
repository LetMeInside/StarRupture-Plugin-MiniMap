#include "RequiredScanner.h"
#include "Compatibility.h"

#include <limits>
#include <string>

namespace MiniMap::Adapters::AlienX
{
    RequiredScanner::RequiredScanner(IPluginSelf& self, IPluginHookScanner& scanner) noexcept
        : Self(self), Scanner(scanner) {}

    bool RequiredScanner::HasFailures()
    {
        Failed = Failed || Scanner.HasFailures(&Self);
        return Failed;
    }

    Services::Result<std::uintptr_t> RequiredScanner::Resolve(const Services::RequiredScan& scan)
    {
        using namespace Services;
        if (HasFailures())
            return {std::nullopt, {Error::BackendFailure, "required scan session has failed"}};

        // Views are borrowed. Own NUL-terminated copies for the synchronous SDK
        // call; reject embedded NULs rather than silently scanning a prefix.
        if (scan.name.empty() || scan.idaPattern.empty() ||
            scan.name.find('\0') != std::string_view::npos ||
            scan.idaPattern.find('\0') != std::string_view::npos)
        {
            Fail(scan.name, "invalid required scan name or pattern");
            return {std::nullopt, {Error::InvalidArgument, "invalid required scan name or pattern"}};
        }

        PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
        const std::string name(scan.name);
        const std::string pattern(scan.idaPattern);
        request.hookName = name.c_str();
        request.pattern = pattern.c_str();
        switch (scan.kind)
        {
        case AddressKind::FunctionEntry: request.kind = PLUGIN_SCAN_FUNCTION_START; break;
        case AddressKind::Code: request.kind = PLUGIN_SCAN_CODE; break;
        default:
            Fail(scan.name, "invalid required scan address kind");
            return {std::nullopt, {Error::InvalidArgument, "invalid required scan address kind"}};
        }
        request.resultOffset = scan.resultOffset;
        if (scan.followRel32At)
        {
            // The approved neutral field is unsigned; AlienX uses signed int32.
            // Refuse an unrepresentable request instead of wrapping its offset.
            if (*scan.followRel32At > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()))
            {
                Fail(scan.name, "required scan rel32 offset exceeds AlienX int32 range");
                return {std::nullopt, {Error::InvalidArgument, "unrepresentable rel32 offset"}};
            }
            request.flags = PLUGIN_SCAN_FLAG_FOLLOW_REL32;
            request.followRel32At = static_cast<std::int32_t>(*scan.followRel32At);
        }

        // Main executable and required flags come from REQUEST_INIT. The host
        // owns uniqueness, transform/kind validation and detailed failure reports.
        const auto address = Scanner.Resolve(&Self, &request);
        if (!address)
        {
            Failed = true; // Resolve already recorded the named required failure.
            return {std::nullopt, {Error::BackendFailure, "required scan failed; see loader scan report"}};
        }
        if (HasFailures())
            return {std::nullopt, {Error::BackendFailure, "loader reports a failed required scan session"}};
        return {address, {}};
    }

    void RequiredScanner::Fail(std::string_view name, std::string_view reason)
    {
        Failed = true;
        const std::string ownedName(name);
        const std::string ownedReason(reason);
        Scanner.ReportFailure(&Self, ownedName.c_str(), ownedReason.c_str());
    }
}
