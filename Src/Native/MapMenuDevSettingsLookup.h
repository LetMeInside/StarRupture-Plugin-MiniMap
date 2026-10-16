#pragma once

#include "plugin_helpers.h"

namespace SDK
{
    class UCrMapMenuDevSettings;
}

namespace MiniMapNative
{
    inline SDK::UCrMapMenuDevSettings* FindMapMenuDevSettingsCDO(
        IPluginSelf* self,
        bool diagnostics)
    {
        if (self == nullptr ||
            self->hooks == nullptr ||
            self->hooks->ObjectWalker == nullptr)
        {
            if (diagnostics)
            {
                LOG_ERROR(
                    "MiniMap: ObjectWalker is unavailable");
            }

            return nullptr;
        }

        auto* walker = self->hooks->ObjectWalker;

        if (!walker->IsReady())
        {
            if (diagnostics)
            {
                LOG_ERROR(
                    "MiniMap: ObjectWalker is not ready");
            }

            return nullptr;
        }

        PluginObjectInfo objects[8] = {};

        const int count =
            walker->FindObjectsByClassNameInto(
                "CrMapMenuDevSettings",
                PluginObjectLookup_CDOOnly,
                objects,
                8);

        if (diagnostics)
        {
            LOG_INFO(
                "MiniMap: found %d CrMapMenuDevSettings CDO(s)",
                count);

            const int loggedCount = count < 8 ? count : 8;
            for (int i = 0; i < loggedCount; ++i)
            {
                LOG_INFO(
                    "MiniMap: DevSettings CDO [%d]: "
                    "object=%p name=%s class=%s",
                    i,
                    objects[i].object,
                    objects[i].objectName,
                    objects[i].className);
            }
        }

        if (count != 1 ||
            objects[0].object == nullptr)
        {
            if (diagnostics)
            {
                LOG_WARN(
                    "MiniMap: expected exactly one "
                    "CrMapMenuDevSettings CDO");
            }

            return nullptr;
        }

        return static_cast<SDK::UCrMapMenuDevSettings*>(
            objects[0].object);
    }
}
