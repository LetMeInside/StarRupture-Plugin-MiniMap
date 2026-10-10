#pragma once
#include <cstdint>
namespace MiniMapSmelterCapture
{
    struct Bindings
    {
        uintptr_t Spawn, Transient, Flags, Retain, Release, Register, Unregister, DestroyComponent;
        uintptr_t InitTarget, TargetResource, SetMesh, CaptureOnly;
        uintptr_t BuilderConstruct, BuilderExecute, BuilderDestroy, UpdateCapture, Transition, GameThread;
        uintptr_t RetirementConstruct, RetirementDestroy, RetirementBegin, RetirementPoll;
    };
    const Bindings& GetBindings();
}
