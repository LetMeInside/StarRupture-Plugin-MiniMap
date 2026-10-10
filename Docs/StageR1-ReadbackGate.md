# Stage R1.2b: one-shot asynchronous native readback experiment

Implementation/build checkpoint only: no deployment, game launch or GPU test
has been performed. The experiment remains default-OFF. Enabled builds now
submit one job on a normal post-engine tick; this is no longer the R1.2a blocked
preflight. Stage C visuals retain their independent default-OFF switch.

## Scope and integration

- ReadbackNativeAdapter.cpp owns the opaque Job, genuine UE callable, native
  texture/readback operations, completion polling, CPU copy and retirement.
- AsyncReadbackExperiment.cpp owns the game-thread scheduler and concise logs.
- ReadbackNativeResolution.cpp retains the 13 native functions and five global
  anchors already checked against CL-127004. No new native resolutions.
- plugin.cpp forwards world begin/end cancellation and final plugin shutdown.
- A separate engine-shutdown event stops submissions earlier in teardown.
- No UObject, actor, Mass, placement, save, image codec or atlas work.

Initialization publishes the CPU job through a release/acquire started flag.
Native resources are constructed only by the queued render command. Failure of
preflight installs no experimental tick callback. The default Client and Server
compile out the experimental subsystem.

## State machine

Disabled / PreflightFailed are compile/configuration/initialization outcomes.
The job records both current phase and a monotonic visited-phase bitset:

ReadyToSubmit -> CommandQueued -> CopySubmitted -> CopyPending
-> ReadbackReady -> PixelsCopied -> Verified
-> CleanupPending -> CleanupComplete.

Failed and Cancelled are separate terminal outcomes; cleanup phase does not
erase them. Cancellation of a queued/pending job suppresses success publication,
then ordinary engine ticks continue retirement. Cancellation before submission
completes without GPU work. No automatic retry after failure.

The scheduler reserves one outstanding command and checks the owning callable
count before another poll. A pointer-only callable retains its Job through moves
and destruction. Polls occur on subsequent ticks, never in a loop and never in
the initial copy command.

ModLoader's EngineTick detour invokes callbacks after the original engine tick.
It iterates a live vector, so OnTick does NOT unregister itself during dispatch:
after success/failure it stays inert until PluginShutdown unregisters it.
No render/Present/ImGui callback is used as an Unreal render thread.

## Ordered native work

The initial command obtains the native immediate list and verifies that it
matches the list supplied by the pipe. PDB base offsets Immediate -> List ->
Base are zero; classes stay incomplete to avoid broad command-list headers.

One ordered native sequence:
1. Create 37x23 PF_B8G8R8A8 with genuine FResourceBulkDataArrayView.
2. Native initial-data upload copies bytes into engine-owned upload storage.
3. Native creation establishes requested CopySrc access.
4. Construct the actual native readback, validate its fence, enqueue its copy.
5. Publish CopySubmitted/CopyPending and return without mapping.

Descriptor: one mip/sample/array element, RenderTargetable|ShaderResource,
no SRGB, initial CopySrc, exactly 3404 source bytes.
Split x=13/y=7; RGBA values:
(17,67,131,193), (29,151,223,109), (241,83,37,157), (101,211,59,239).
The source buffer stores BGRA and remains owned by Job.

Native EnqueueCopy returns void. Its return is CPU submission, not proof of GPU
completion/success. Subsequent checks validate mask/staging/fence, and pixel
verification is the final correctness evidence.

## Readiness and pixels

On the render thread, Check requires:
- valid supported initialized/threaded RHI and unchanged GPU count;
- a non-null fence with the verified no-argument D3D12 Poll virtual target;
- a valid copy mask and staging references for all participating GPUs;
- zero NumPendingWriteCommands AND successful no-argument Poll().

Only then is Lock invoked, with the full predicate checked again immediately
before mapping. No GPU-mask Poll substitute, ReadPixels, flush, fence wait,
busy-wait or synchronous staging map before readiness is used.

Pitch must be 37..16384 pixels and buffer height at least 23. Copy exactly 148
bytes from each of 23 pitched rows to MiniMap-owned CPU memory, then Unlock.
Only after Unlock does the existing CPU verifier inspect all 851 pixels.
Metrics are immutable after DataReady release publication. Game-thread readers
acquire DataReady before reading them; native containers/pointers are not exposed.

## Ownership and cancellation

Job is a normal heap C++ object with genuine FTextureRHIRef, opaque native
readback pointer, CPU arrays, atomic state and persistent trace IDs.
Native readback storage uses native Malloc, constructor, exactly one
non-deleting destructor, then native Free. Its eight staging references and fence
are destroyed by native code only. Texture release uses reference-counting and
MarkForDelete, never direct deletion.

Successful or mapped-but-invalid-pixels paths unmap, destroy the completed
readback and release the source texture on the render thread. CleanupComplete
is logged only once the callable count and outstanding-command flag are zero.
CPU diagnostics stay owned until safe PluginShutdown; they are not reallocated
each frame. Cancellation races with verification via a Pending->terminal atomic
CAS; cancellation wins over a later Verified publication.

If texture creation already queued an upload but readback/fence construction
fails, or readiness/mapping becomes invalid, retirement cannot be proved.
The job is quarantined: no retry, no unsafe destructor/free, one failure log,
and resources retained. Device loss/hung fences are not made safe by timeouts.
C++ exceptions quarantine the job; native fatal checks/SEH/device faults are not
claimed recoverable. A never-signalling valid fence remains pending, with bounded
one-command-per-frame nonblocking polling and no fabricated completion.

## Shutdown limitation (mandatory runtime-review gate)

Engine shutdown stops new scheduling and cancels publication. The scheduling
gate is nonblocking: an already accepted/in-progress operation is pending work,
not something shutdown can safely interrupt. No shutdown callback queues a drain,
flushes rendering or waits for GPU completion.

Completed jobs with destroyed callables can release their CPU storage. Otherwise
the job remains allocated; queued callbacks and native resources are NOT freed
underneath pending work. World changes are safe while the plugin stays loaded:
the test has no world-owned objects and later ticks can retire cancelled work.

This does NOT certify FreeLibrary safety for queued/executing MiniMap callbacks.
The current host cannot be told to defer unload, and cancellation cannot remove
an already-owned UE callable. Normal engine shutdown overlapping such callbacks
also lacks a proven quiescence barrier. The implementation logs this explicitly.
No DLL pinning, host change, helper DLL or blocking drain was added. Do not
hot-reload this experimental build. Review this limitation before runtime
authorization; a timeout is never permission to unload callback code.

## ABI and static initialization

R1.2a assertions remain: callable0x30/align16/inline24/TStatId1;
bulk interface0x08/view0x18; desc0x38/create desc0x60; native readback PDB schema
0x58 with staging refs+0x18..+0x50; FRHIResource0x10/FRHIGPUFence0x20.
Texture creation uses this/sret/list/desc in RCX/RDX/R8/R9.
FromValidEName uses explicit result storage as verified in CL-127004.

Genuine narrow UE headers only: Function.h, Stats.h (shipping TStatId),
RHIResources.h and ResourceArray.h. No RHIGPUReadback.h, DynamicRHI.h or
RHICommandList.h. No global owning UE objects. Native-boundary TUs remain
ordinary inspectable COFF, with real allocator/name/release forwarding.

## Validation and later controlled test

Passed: enabled/default Client Release, Server Release even with the experiment
property requested, existing offline pixel tests, and git diff --check.
All 18 signatures remain unique with valid unwind starts/data targets. All ABI
assertions compile. All three experimental COFF objects have zero CRT$XCU or
generated initializers, no UnifiedError chain and no unresolved UE dependencies.
The default Client and Server adapter objects contain no experimental symbols.
External inspection files are under C:\Modding\StarRupture\MiniMapR1-Analysis.

After separate deployment/runtime authorization:
1. Exit the game; manually install the enabled R1-Enabled DLL and matching PDB.
   Keep Stage C visuals OFF and do not hot-reload.
2. Launch to the main menu and remain there; no save needs to be loaded.
3. Expect enabled/preflight, submission, copy submitted, VERIFIED and native
   cleanup complete. All 851 pixels must match, pitch>=37, height>=23,
   mismatches=0, and cleanup must show no outstanding callable.
4. Wait for cleanup complete before normal exit. The experiment must not repeat.
5. Send all MiniMap R1 lines, nearby errors, game/ModLoader versions and any hitch.
   Preflight failure is a useful fail-closed result, not GPU success.
   If completion is absent, do not hot-reload/redeploy; report the pending log.
   Pending-operation shutdown remains unverified.

Natural world transitions can exercise cancellation in a separate authorized
run, but this tiny one-shot job may finish before a transition. No UI trigger,
deliberate delay, save modification or forced unsafe unload was introduced.
