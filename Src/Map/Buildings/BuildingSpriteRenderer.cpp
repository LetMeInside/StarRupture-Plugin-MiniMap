#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/Buildings/BuildingSpriteRenderer.h"

#include <optional>
#include <string>

namespace
{
    struct ActiveRequest
    {
        std::string BlueprintPath;
    };

    MiniMapSpriteQueues::SpriteFifo* g_fifo = nullptr;
    bool g_sessionActive = false;
    std::optional<ActiveRequest> g_activeRequest;
}

namespace MiniMapBuildingSpriteRenderer
{
    void Initialize(MiniMapSpriteQueues::SpriteFifo& fifo)
    {
        g_fifo = &fifo;
        g_sessionActive = false;
    }

    void InitializeSession()
    {
        if (!g_fifo || g_sessionActive) return;
        g_activeRequest.reset();
        g_sessionActive = true;
    }

    void Tick(float deltaTime)
    {
        (void)deltaTime;
        if (!g_fifo || !g_sessionActive) return;
    }

    void EndSession()
    {
        g_sessionActive = false;
        g_activeRequest.reset();
    }

    void Shutdown()
    {
        EndSession();
        g_fifo = nullptr;
    }
}

#endif
