#pragma once

// Transitional include for existing loader-specific subsystem declarations.
// Services headers remain neutral; migrate these signatures in their own stages.
#include "Adapters/AlienX/Compatibility.h"

namespace MiniMap
{
    bool Initialize();
    void Shutdown();
}
