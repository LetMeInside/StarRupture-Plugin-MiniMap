#include "RequiredScanner.h"
#include "Compatibility.h"

#include "SDK/Engine_classes.hpp"
#include "plugin_map.h"

#include <limits>
#include <string>

namespace MiniMap::Adapters::AlienX
{
    RequiredScanner::RequiredScanner(IPluginSelf& self, IPluginHookScanner& scanner) noexcept
        : Self(self), Scanner(scanner) {}

    // A scan the loader could not place is recorded there. Whether it refuses the
    // plugin or only turns off the feature that lists its key is the loader's
    // call, so only failures the plugin reported itself stop the resolve.
    bool RequiredScanner::HasFailures()
    {
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

        const std::string name(scan.name);
        const std::string pattern(scan.idaPattern);
        int kind = PLUGIN_SCAN_UNSPECIFIED;
        switch (scan.kind)
        {
        case AddressKind::FunctionEntry: kind = PLUGIN_SCAN_FUNCTION_START; break;
        case AddressKind::Code: kind = PLUGIN_SCAN_CODE; break;
        default:
            Fail(scan.name, "invalid required scan address kind");
            return {std::nullopt, {Error::InvalidArgument, "invalid required scan address kind"}};
        }
        uint32_t flags = PLUGIN_SCAN_FLAG_NONE;
        std::int32_t followRel32At = 0;
        if (scan.followRel32At)
        {
            // The approved neutral field is unsigned; AlienX uses signed int32.
            // Refuse an unrepresentable request instead of wrapping its offset.
            if (*scan.followRel32At > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()))
            {
                Fail(scan.name, "required scan rel32 offset exceeds AlienX int32 range");
                return {std::nullopt, {Error::InvalidArgument, "unrepresentable rel32 offset"}};
            }
            flags = PLUGIN_SCAN_FLAG_FOLLOW_REL32;
            followRel32At = static_cast<std::int32_t>(*scan.followRel32At);
        }

        // A keyed scan goes to the loader's version map first and idaPattern is
        // the fallback (PluginMap::Request sets the key flag). Main executable
        // and required flags come from REQUEST_INIT; the host owns uniqueness,
        // transform/kind validation and detailed failure reports.
        PluginScanRequest request = PluginMap::Request(name.c_str(), pattern.c_str(), kind, flags,
            scan.resultOffset, followRel32At);
        if (!scan.inMap)
            request.flags &= ~static_cast<uint32_t>(PLUGIN_SCAN_FLAG_MAP_KEY);

        const auto address = Scanner.Resolve(&Self, &request);
        if (!address)
            return {std::nullopt, {Error::BackendFailure, "required scan failed; see loader scan report"}};
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
