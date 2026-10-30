#pragma once

#include "plugin_helpers.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
#include "SDK/ChimeraUI_classes.hpp"

#include <array>

namespace MiniMapNative
{
    inline SDK::UCrMapMenuDevSettings* FindMapMenuDevSettingsCDO(bool diagnostics)
    {
        auto* objects = MiniMap::Services::TryObjects();
        if (objects == nullptr)
        {
            if (diagnostics) LOG_ERROR("MiniMap: ObjectWalker is unavailable");
            return nullptr;
        }
        std::array<MiniMap::Services::ObjectRecord, 8> records{};
        const auto matches = objects->ClassDefaults("CrMapMenuDevSettings", records);
        if (!matches.value)
        {
            if (diagnostics)
            {
                if (matches.status.detail.find("not ready") != std::string::npos)
                    LOG_ERROR("MiniMap: ObjectWalker is not ready");
                else if (matches.status.detail.find("unavailable") != std::string::npos)
                    LOG_ERROR("MiniMap: ObjectWalker is unavailable");
                else
                    LOG_ERROR("MiniMap: class-default lookup failed: %s", matches.status.detail.c_str());
            }
            return nullptr;
        }
        const auto count = matches.value->total;

        if (diagnostics)
        {
            LOG_INFO("MiniMap: found %zu CrMapMenuDevSettings CDO(s)", count);
            const auto loggedCount = count < records.size() ? count : records.size();
            for (std::size_t i = 0; i < loggedCount; ++i)
            {
                LOG_INFO(
                    "MiniMap: DevSettings CDO [%d]: "
                    "object=%p name=%s class=%s",
                    static_cast<int>(i),
                    records[i].object,
                    records[i].objectName.c_str(),
                    records[i].className.c_str());
            }
        }

        if (count != 1 || records[0].object == nullptr)
        {
            if (diagnostics)
            {
                LOG_WARN(
                    "MiniMap: expected exactly one "
                    "CrMapMenuDevSettings CDO");
            }

            return nullptr;
        }

        return static_cast<SDK::UCrMapMenuDevSettings*>(records[0].object);
    }
}
