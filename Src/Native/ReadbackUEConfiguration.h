#pragma once

// Supplies the installed UE 5.6.1 Shipping configuration for genuine templates,
// readback declarations and private Mass implementation in client-only TUs.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <Windows.h>
#include <strsafe.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#undef Yield

#define UBT_COMPILED_PLATFORM Windows
#define UBT_COMPILED_TARGET Game
#define PLATFORM_WINDOWS 1
#define PLATFORM_64BITS 1
#define UE_BUILD_SHIPPING 1
#define WITH_EDITOR 0
#define WITH_EDITORONLY_DATA 0
#define WITH_ENGINE 1
#define WITH_COREUOBJECT 1
#define WITH_SERVER_CODE 1
#define WITH_UNREAL_DEVELOPER_TOOLS 0
#define WITH_PLUGIN_SUPPORT 0
#define WITH_PERFCOUNTERS 0
#define WITH_MASSENTITY_DEBUG 0
#define UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_6 0
#define IS_MONOLITHIC 1
#define IS_PROGRAM 0
#define STATS 0
#define CORE_API
#define COREUOBJECT_API
#define ENGINE_API
#define INPUTCORE_API
#define SLATECORE_API
#define SOCKETS_API
#define APPLICATIONCORE_API
#define COREONLINE_API
#define RHI_API
#define PHYSICSCORE_API
#define DEVELOPERSETTINGS_API
#define STRUCTUTILS_API
#define STATESTREAM_API
#define RENDERCORE_API
#define NETCORE_API
#define NETCOMMON_API
#define TRACELOG_API
#define MASSENTITY_API
#define CHAOS_API
#define CHAOSCORE_API

#include "Templates/Function.h"
// StatsSystemTypes alone omits TStatId when STATS=0; Stats.h supplies the
// shipping one-byte declaration. Its object initialization is inspected below.
#include "Stats/Stats.h"
