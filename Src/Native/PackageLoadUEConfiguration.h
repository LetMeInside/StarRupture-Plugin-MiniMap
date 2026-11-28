// Isolates the installed checked delegate layout from the shared readback configuration.
// Only PackageLoadOperation.cpp may include this before genuine UE headers.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <Windows.h>
// Parse before UE's WindowsSystemIncludes.h temporarily maps static to inline (MSVC >= 1940).
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
#define IS_MONOLITHIC 1
#define IS_PROGRAM 0
#define STATS 0
#define UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_6 0
#define DO_CHECK 1
#define DO_ENSURE 0
#define DO_GUARD_SLOW 0
#define UE_DETECT_DELEGATES_RACE_CONDITIONS 1
#define NUM_DELEGATE_INLINE_BYTES 0
#define UE_AUTORTFM 0
#define CORE_API
#define COREUOBJECT_API
#define ENGINE_API
#define APPLICATIONCORE_API
#define COREONLINE_API
#define NETCORE_API
#define NETCOMMON_API
#define TRACELOG_API
