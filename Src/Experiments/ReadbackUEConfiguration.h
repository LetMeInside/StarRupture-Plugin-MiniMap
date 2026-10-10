#pragma once

// Only included by the opt-in R1 translation unit. Use Epic's genuine callable
// template, without pulling in RHI headers with engine static initializers.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <Windows.h>
#include <strsafe.h>
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
#define IS_MONOLITHIC 1
#define IS_PROGRAM 0
#define STATS 0
#define CORE_API
#define TRACELOG_API

#include "Templates/Function.h"
// StatsSystemTypes alone omits TStatId when STATS=0; Stats.h supplies the
// shipping one-byte declaration. Its object initialization is inspected below.
#include "Stats/Stats.h"
