#pragma once

#include "ServiceTypes.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace MiniMap::Services
{
    enum class AddressKind { FunctionEntry, Code };

    struct RequiredScan
    {
        std::string_view name, idaPattern;
        AddressKind kind;
        std::int32_t resultOffset = 0;
        std::optional<std::uint32_t> followRel32At;
        // True when name is a key in the loader's shared version map: the map's
        // row for the running game build answers first and idaPattern is the
        // fallback. False when the name only labels a scan of MiniMap's own.
        bool inMap = true;
    };

    // Borrowed only during load admission; never retained in Runtime.
    // Main executable, exactly one match. Follow E8/E9 first (when requested),
    // then apply resultOffset and validate the target kind. Every failure is
    // fatal to admission, including semantic validation reported through Fail.
    class IRequiredScanner
    {
    public:
        virtual ~IRequiredScanner() = default;
        virtual Result<std::uintptr_t> Resolve(const RequiredScan&) = 0;
        virtual void Fail(std::string_view name, std::string_view reason) = 0;
    };
}
