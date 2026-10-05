#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

struct IPluginSelf;

namespace MiniMapFogOfWar
{
    bool Initialize(
        IPluginSelf* self);

    void Reset();

    void Shutdown();
}

#endif