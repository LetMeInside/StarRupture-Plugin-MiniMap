# StarRupture Native Loading and World Transitions

## Scope and version basis

This reference preserves the native loading, transition, and UObject-lifetime findings in the MiniMap historical archive. It does not specify a loading observer or implement a lifecycle service. The current MiniMap source remains authoritative for current behavior; historical observer code and reports are not a current design contract.

The principal native investigations inspected the Client executable whose SHA-256 is `b2b9188700ec64dfaf721f969063f969d5a89f8f0b9dc8b6f7135a914f3e5308`, paired with PDB GUID `41cbaea9-7ac3-99da-b947-a1db9c1021ed`, age 1. A separate native investigation also identifies a dedicated Server executable (SHA-256 `3a0f160767b24a621a73b75dd6d43ae262ceef56127b340fef30860ef8041757`) and matching PDB GUID `59289f85-a90d-7c1d-3814-69c4c54fd05c`, age 1. The reports do not assign a release label such as 0.3.5 to every loading finding; no such label is inferred here. A separate identity investigation identifies its matched binary as CL-127004. RVAs below are offline image RVAs for the cited evidence and are not runtime addresses or portable bindings.

Evidence labels used below:

- **CONFIRMED** — supported by an identified native, PDB, SDK, asset, or source artifact inspected in the historical investigation.
- **OBSERVED** — reported from a specific historical runtime log or user observation. It is not a general engine guarantee.
- **INFERRED** — reasoned interpretation with its premise stated.
- **UNKNOWN** — the research did not close the question.
- **SUPERSEDED IMPLEMENTATION** — a historical MiniMap observer or design that is not current source authority.
- **REJECTED** — a hypothesis contradicted by inspected evidence.

## Executive technical summary

| Finding | Classification | Engineering boundary |
|---|---|---|
| `ULoadingScreenManager` is a `UGameInstanceSubsystem` with map-load delegates, a manager-owned viewport widget, loading-reason aggregation, a display-state byte, and update-driven show/hide paths. | CONFIRMED | The manager byte, its delegate, viewport operations, MoviePlayer, and engine preload screen are distinct observations/owners. A request or callback is not necessarily completed visible presentation. |
| `PreLoadMap` sets the manager's in-map state and can trigger an update; `PostLoadMap` clears that state but does not itself dismiss the screen. | CONFIRMED | A later update evaluates remaining reasons and hold policy. |
| The project startup preload is a separate Slate `FCommonPreLoadScreen`, type `EngineLoadingScreen` (2), with its own active lifetime and terminal wait/stop path. | CONFIRMED | It is not the configured initial UMG widget. Manager Hidden can coexist with preload presentation. |
| The default MoviePlayer may hold the same Slate widget supplied by the manager. `StopMovie`, its playing getter, and the temporary loss of its synchronization mechanism do not prove completed removal. | CONFIRMED | Full-path completion occurs later in `WaitForMovieToFinish`; early/no-op wait paths must be distinguished. |
| Teleport movement can set a shared “waiting for world streaming” input consumed by the loading manager. | CONFIRMED | This establishes a teleport-related loading reason, not that all teleports show a screen or perform full map travel. |
| The user-visible teleport freeze's root cause was not established. | UNKNOWN | The measured Collector callback completed; available logs lacked freeze-time stacks and a stopped-operation marker. |
| UObject slot equality and flags are current membership evidence; Index+Serial identifies a slot incarnation across reuse. AddRef provides a GC-rooting effect but does not validate an arbitrary pointer or make all operations safe through teardown. | CONFIRMED | Borrowing, validation, retention, and operation-specific safety remain separate. |
| The historical loading-observer implementation is not a complete, approved lifecycle contract. | SUPERSEDED IMPLEMENTATION | Later observer and native-contract reviews document lifetime/performance blockers and NO-GO decisions. The current MiniMap source no longer contains that observer. |

Keep **LOADING PRESENTATION** separate from **WORLD/SESSION LIFETIME**. Dismissing a Slate widget or completing a MoviePlayer wait does not certify that a destination world, Mass population, terrain, definitions, or every actor is ready. World callbacks, experience completion, loading UI, and same-world teleport are separate mechanisms.

## Native loading mechanisms

### `ULoadingScreenManager`

**CONFIRMED.** The matched Client PDB and offline disassembly identify `ULoadingScreenManager final : UGameInstanceSubsystem` (SDK size `0xC8`). Its relevant matched native layout is:

| Member | Offset | Evidenced role |
|---|---:|---|
| `LoadingScreenVisibilityChanged` | `+0x38` | Multicast delegate; manager passes `true`/`false` at different points in show/hide. |
| `LoadingScreenWidget` | `+0x58` | Retained Slate widget; viewport insert/removal is a separate operation. |
| `InputPreProcessor` | `+0x68` | Input-blocking ownership. |
| `ExternalLoadingProcessors` | `+0x78` | Additional request participants. |
| debug reason | `+0x88` | Reason text; not a readiness contract. |
| shown/last-dismissed timing | `+0x98` / `+0xA0` | Native timing/hold state. |
| `bCurrentlyInLoadMap` | `+0xB0` | Map callback state. |
| `bCurrentlyShowingLoadingScreen` | `+0xB1` | Manager's own display-state byte. |

Matched Client methods (Windows x64 member ABI; `this` in RCX unless noted):

| RVA | Signature | Finding |
|---:|---|---|
| `0x070D2A00` | `void ULoadingScreenManager::Initialize(FSubsystemCollectionBase&)` | Registers `PreLoadMap` and `PostLoadMap` delegates. |
| `0x070D28D0` | `void ULoadingScreenManager::Deinitialize()` | Stops input, removes its widget/delegates, ends ticking; it does not issue the usual false notification/clear sequence. |
| `0x070D2D40` | `bool ShouldCreateSubsystem(const UObject*) const` | Rejects the dedicated-server GameInstance for this local presentation subsystem. |
| `0x070D29A0` | `void HandlePreLoadMap(const FWorldContext&, const FString&)` | Checks the matching GameInstance, sets in-map state, may update. |
| `0x070D2960` | `void HandlePostLoadMap(UWorld*)` | Checks the matching GameInstance and clears in-map state; does not itself hide. |
| `0x070D3C70` | `void UpdateLoadingScreen()` | Aggregates native reasons and hold state; normal manager show and hide control point. |
| `0x070D3080` | `void ShowLoadingScreen()` | Guarded show operation. Entry and return do not by themselves prove a new widget was inserted. |
| `0x070D2C00` | `void RemoveWidgetFromViewport()` | Manager-owned removal helper; actual viewport call depends on widget and viewport presence. |
| `0x070D2F20` | `bool ShouldShowLoadingScreen()` | Evaluates desired state/hold policy; not a pure status query. |
| `0x070D1E30` | `bool CheckForAnyNeedToShowLoadingScreen()` | Aggregates reasons including loading-process participants; not a display-state getter. |
| `0x070D3A20` | `void Tick(float)` | Adjusts `this` by `-0x30` before update; do not confuse tickable-subobject ABI with the base manager ABI. |
| `0x076944F0` | `bool UCrExperienceManagerComponent::ShouldShowLoadingScreen(FString&) const` | One loading-process predicate; false is not proof all manager reasons cleared. |

`UCommonLoadingScreenSettings` provides regular and initial widget classes, hold time and performance settings. Its configuration does not certify readiness. `ULoadingProcessTask` registration participates in a request; creating/registering a task is not a read-only presentation observation. The generated SDK exposes only a narrow portion of the manager; the native member layout and methods above are report-backed from the matched PDB/disassembly, not generated field declarations.

### Startup preload / MoviePlayer

**CONFIRMED.** In the matched Client evidence, `EPreLoadScreenTypes` maps `0=CustomSplashScreen`, `1=EarlyStartupScreen`, `2=EngineLoadingScreen`. Cooked `CommonStartupLoadingScreen` is registered as `ClientOnly/PreLoadingScreen`. Its module creates `FCommonPreLoadScreen`, which owns `SCommonPreLoadingScreenWidget`; this startup Slate screen is distinct from both configured manager UMG classes.

The engine startup path creates `FPreLoadScreenManager` during `FEngineLoop::PreInitPreStartupScreen`, starts the project preload in startup phases, marks engine loading complete in `FEngineLoop::Init`, waits for type-2 completion, and later transfers the window to the game viewport and destroys the preload manager during the first-tick handoff. `SetEngineLoadingComplete(bool)` at RVA `0x03903980` is an input, not dismissal. `WaitForEngineLoadingScreenToFinish()` at `0x039040C0` waits for the type-2 completion state, stops/joins the preload synchronization thread and reaches `HandleStopPreLoadScreen()` (`0x038FEE70`). That helper clears the active owner/index and removes window content. Post-original return is a control-side completion boundary for this type-2 path, not a pixel scan-out fence. `StopPreLoadScreen()` is not an unconditional equivalent for every type.

The default MoviePlayer is another independent presentation owner. `GetMoviePlayer()` (`0x043E6FC0`) returns an existing implementation on inspected branches; it can return a null implementation and is not evidence of creation. On the default branch it returns an `IGameMoviePlayer` subobject at complete-object `+0x10`. Virtual functions receive the interface pointer:

| RVA | Signature | Contract evidence |
|---:|---|---|
| `0x043E9320` | `bool PlayMovie(IGameMoviePlayer*)` | Successful return follows setup and playback-started broadcast; attempted call alone is not a start. |
| `0x043EB2F0` | `void StopMovie(IGameMoviePlayer*)` | Sets a user-finish request/last-play state; does not remove content or complete cleanup. |
| `0x043EB520` | `void WaitForMovieToFinish(IGameMoviePlayer*, bool)` | Full path stops/joins Slate work, clears widget content, playing state and performs cleanup before finished broadcast/return; early/no-op paths exist. |
| `0x017B15E0` | `bool IsMovieCurrentlyPlaying(IGameMoviePlayer*) const` | Tests presence of the synchronization mechanism. Wait clears it before all cleanup is complete. |
| `0x043E8EE0` | `bool IsLoadingFinished(IGameMoviePlayer*) const` | Producer completion state, not presentation removal. |
| `0x043EA810` | `void Shutdown(IGameMoviePlayer*)` | Presentation-owner retirement/resource boundary, not normal dismissal and not all allocation destruction. |

The manager passes its same Slate widget through `SetupLoadingScreen`/`PlayMovie` after viewport insertion on the matching branch; both owners can overlap. A full active Wait's finished delegate occurs after widget-content removal and playing-state clear. A nested/idle early Wait can return without finished broadcast. `IsMovieCurrentlyPlaying()==false`, `StopMovie`, `IsLoadingFinished`, Wait entry, and every void Wait return are therefore insufficient individually. The static findings establish branch behavior, not whether a configured branch was taken in a particular runtime session.

## Loading presentation versus world lifetime

The manager boolean describes the manager's own state, not all visible loading presentation. Show has early guards; a type-2 preload can divert before regular widget creation. On a normal branch it sets `+0xB1`, broadcasts true before loading/constructing the widget and calling the viewport insertion virtual. A later native-contract analysis further establishes a path that can set the byte and then leave without ordinary manager widget insertion when preload is active. After no loading reason and no remaining native hold period, hide stops input blocking, requests engine garbage collection at `0x070D3F2F`, calls the removal helper at `0x070D3F37`, restores performance settings, broadcasts false at `0x070D3F4C`, then clears the byte at `0x070D3F9F`. The GC request is part of the native ordering; it does not mean collection has completed before the hide path continues. The helper can return without invoking viewport removal when no widget or viewport exists. The manager's own display byte can clear while preload remains active. Deinitialize removes its owner without the normal false broadcast/clear path.

Therefore:

- **CONFIRMED:** request, native state change, delegate broadcast, widget insertion/removal, MoviePlayer completion, preload completion, and physical pixel presentation are distinct boundaries.
- **CONFIRMED:** manager Hidden does not imply preload/movie presentation ended; manager retirement is not a successful hide event.
- **CONFIRMED:** native extra hold time is intended to allow texture streaming. Expiry of that hold is not proof global streaming or all destination actors are complete.
- **UNKNOWN:** exact compositor/scan-out timing and whether every native completion aligns with the user's visible frame in all branches.
- **UNKNOWN:** exact branch use and overlap in any future game build or particular save/teleport run unless runtime-correlated.

The existing texture publication reference documents AlienX UI/GPU texture acquisition and release: [Texture-Publication.md](../ModLoader/Texture-Publication.md). Its resource-lifetime findings do not establish UObject, loading-manager, or plugin-DLL lifetime.

## Lifecycle events and ordering

No universal callback sequence is established. The verified relationships below are narrower:

| Context | Established ordering | Status and limit |
|---|---|---|
| Engine startup preload | Engine creates preload manager; loading-screen module registers project type-2 preload; engine startup phases play it; engine init signals completion; wait/stop removes the preload content; first Tick hands off window/initializes MoviePlayer as needed and destroys preload manager. | CONFIRMED for the inspected matched Client startup path. Exact timing relative to AlienX `PluginInit` is UNKNOWN. |
| Loading-manager lifetime | GameInstance subsystem collection creates/initializes eligible subsystem; manager `Initialize` registers PreLoadMap/PostLoadMap; GameInstance shutdown deinitializes the subsystem collection; manager `Deinitialize` removes its delegates/widget and stops ticking. | CONFIRMED normal ownership path. Not proof that every possible UObject destruction/reconstruction route calls manager Deinitialize. |
| Map travel | Manager's matching `PreLoadMap` sets `bCurrentlyInLoadMap` and can update; `PostLoadMap` clears it. Subsequent manager Update re-evaluates remaining reasons and hold. | CONFIRMED. These callbacks do not establish the exact order of experience completion or visible dismissal. |
| Experience initialization | Native experience component predicate contributes a loading request. MiniMap current source admits services on ModLoader `ExperienceLoadComplete`, subject to current world/generation and setup succeeding. | Native predicate CONFIRMED; current source order CONFIRMED by source. Relative order to Movie/preload and final visible dismissal is not universal/UNKNOWN. |
| Return to menu and next world | A runtime-correction log from one historical session observed the same manager/GameInstance identities across menu and a later save, with world/experience generations changing. The exact manager dismissal in the second load fell within an admission gap. | OBSERVED in that run only; no new manager per world can be assumed. |
| Same-world teleport | Teleport subsystem request leads to character movement streaming state; no new UWorld/GameInstance is required by this observed path. | CONFIRMED native dataflow for inspected path; coverage does not include every teleporter/travel family. |
| Garbage collection | In `IncrementalPurgeGarbage` at RVA `0x016415A0`, PostPurge and GarbageCollectComplete delegates run before `GObjIncrementalPurgeIsInProgress` is cleared at `0x01641722`. | CONFIRMED for inspected body. A completion listener may still be inside an admission-excluded interval; completion broadcast is not yet post-purge admission. |
| Plugin start/shutdown | Current MiniMap prepares native dependencies before lifecycle callbacks, registers callbacks, then performs an explicit active-world check (Src/plugin.cpp:748–760). Shutdown closes gameplay admission, tears services down in dependency order, unregisters world/tick callbacks, and shuts down NativeApi last (Src/plugin.cpp:774–845). Src/Map/Map.cpp:114–126 stores world identity and advances the MiniMap generation. | CONFIRMED current source order only. This is not a guarantee of callback quiescence or physical DLL unload safety. |

Callbacks must be described by their actual evidence. The native manager’s PreLoadMap/PostLoadMap delegate bodies and MoviePlayer/preload calls are statically identified. One saved historical runtime log records observer samples, not pixels; some records were drained long after capture and the historical account reconstructs approximate capture time using QPC/age. It contains identity-fault gaps. It does not prove exact screen timing, full callback coverage, or a universal order with SaveLoaded/experience callbacks.

**OBSERVED, one historical runtime-correction session only:** hooks installed after owners already existed; the initial authenticated manager sample was Visible while preload and MoviePlayer samples were Hidden. The same manager index/serial and GameInstance were later seen across menu return and a second save. During the second load, MiniMap was shown before a later authenticated all-Hidden sample; manager admission faults left a gap, so the actual dismissal operation/time is not known. Printed log time was not the event time for queued records; QPC/age-based approximate capture times are required. The report-backed account records 133 manager identity/admission fault events and no pixel observations.

## World identity and ownership

The engine relationships visible in the inspected evidence are:

- `UGameEngine` owns its current `GameInstance`; the GameInstance has a WorldContext and a subsystem collection. `FSubsystemCollectionBase` constructs subsystem instances with the collection's GameInstance Outer, holds subsystem references, and deinitializes/clears them through collection removal/shutdown.
- The loading manager is a GameInstance subsystem. Its lifetime can span multiple `UWorld` lifetimes. Historical same-manager observations across menu/world transitions corroborate that a world generation is not a manager generation.
- `FWorldContext` / GameInstance and `UWorld` / owning GameInstance associations are useful ownership/context checks, but a copied address alone does not retain either object. The reviewed reports do not establish a complete ownership model for every temporary/travel world or every context replacement path.
- In current MiniMap, `MiniMapMap::SetWorld` invalidates gameplay readiness, stores the world pointer, and increments a MiniMap world generation. That generation is MiniMap session identity; it is not the engine UObject serial and does not represent every same-world teleport.

**CONFIRMED identity distinction:** `UObjectBase::IsValidLowLevel()` at RVA `0x017B2740` reaches `FUObjectArray::IsValid()` (`0x017B2510`), which checks index range, slot presence, pointer equality and validity conditions but not a previously observed serial. `FUObjectArray::AllocateSerialNumber()` (`0x017A5560`) lazily assigns a positive serial when a weak identity is formed. A zero serial does not alone mean a live UObject is invalid. Matched-client UObject identity evidence distinguishes transient synchronous validation from persistent identity: do not require a serial merely to traverse a validated object synchronously, but require a positive Index+Serial token when publishing/persisting identity under that contract.

The report-backed lifetime analysis adds that current source's generic `ValidateObjectIdentity` checks object index, selected slot's pointer equality, flags and readability, but does not compare a previously captured serial. `QuietGameThread()` is a sampled exclusion predicate, not a documented global object-array lock. Treat current registry evidence and lifetime across time as separate questions.

## UObject retention and GC boundaries

Use these terms precisely:

| Term | What evidence supports | What it does not establish |
|---|---|---|
| **Borrowed pointer** | A current engine lookup, iterator result, world field, or component/Outer relationship can supply a pointer for immediate synchronous use under the relevant thread/GC/world-transition admission. | No ownership extension, no cross-callback cache, no protection from reentry or owner replacement. |
| **Validated current object** | Object address/extent, index, current FUObjectItem pointer equality, flags, class/Outer/world association as applicable have been checked for a specific operation. | Does not retain the allocation, pin a page, or prove future validity. A validation boolean is not a handle. |
| **Retained/rooted reference** | `UObjectBase::AddRef()` (`0x016205F0`) increments item RefCount and establishes the RefCounted/root path; `ReleaseRef()` (`0x0168F6F0`) decrements and may clear root membership at zero. A reachable GameInstance's subsystem collection is also traversed by `AddReferencedObjects`. | Does not validate arbitrary input, guarantee class/readiness/child-buffer validity, assign serial, or authorize calls through world teardown. It does not immediately destroy on release. |
| **Safe for a specific operation** | Requires the operation's thread, current owner/identity, GC/purge conditions, native API contract, and no unsupported transition/reentrant invalidation across the operation. | Cannot be inferred from numeric pointer stability or rooting alone. |

The matched `FUObjectItem` layout is `0x18` bytes: Object `+0`, internal flags `+8`, SerialNumber `+0x10`, RefCount `+0x14`; UObject Index is `+0x0C`; array chunk size is 65,536. Garbage/Unreachable internal flags and ordinary UObject flags are separate checks. Index plus pointer correspondence establishes current slot membership; a matching captured serial distinguishes reuse of that slot across observations. A pointer that remains numerically identical can be destroyed/reconstructed or reused; address equality is not continuity.

**CONFIRMED cleanup order:** `ConditionalBeginDestroy` marks logical retirement before allocation reclamation. `ConditionalFinishDestroy` resets serial before delete-listener notification; `FreeUObjectIndex` clears slot flags/serial/object and sets index to `-1`. A delete listener can therefore be too late to serve as the sole pre-invalidation boundary, and FreeUObjectIndex has separate callers. The direct caller searches are bounded, not an exhaustive proof that every writer/retirement route was covered.

**CONFIRMED subsystem retention:** the GameInstance's subsystem collection holds strong UObject references during normal reachable ownership. Collection deinitialization invokes subsystem retirement and clears the collection. This does not root GameInstance indefinitely. The loading manager's scalar-deleting destructor was inspected and does not itself call manager `Deinitialize`; normal collection ownership is strong evidence for the ordinary path, not proof every destruction path is covered.

**Incremental GC:** `PostPurge` and `GarbageCollectComplete` broadcast before the incremental-purge-in-progress flag clears. A callback during those broadcasts may still fail the project's normal admission predicate. Repeatedly authenticating each frame, skipping GC safeguards, or treating temporary unavailability as Hidden was rejected by the historical research.

### Native ownership and cleanup evidence

`AddRef`/`ReleaseRef` manipulate root/refcount state; Release is not immediate destruction. The existing ReleaseUObject wrapper also skips ReleaseRef during exit purge. Historical MiniMap code uses an “uncertain ownership” latch/quarantine policy when a native retain/release outcome cannot be trusted. That is a conservative implementation strategy to avoid dropping a potentially live root; it is not evidence that Unreal requires that exact policy in every integration.

Similarly, a failure after AddRef can leave ownership ambiguous. Do not blindly retry a non-idempotent retain/release or release an object after its identity/ownership outcome is uncertain. Preserve the distinction between the engine's verified reference-count behavior and a plugin's chosen conservative quarantine. No native evidence here establishes that a successful root makes calls safe during subsystem deinitialization or world teardown.

## Transition and teleportation findings

### Native teleport path

**CONFIRMED, matched Client only.** `UCrTeleportingSubsystem::TeleportPlayer(APlayerController*, FCrMassEntityReplicationHelper, FCrMassEntityReplicationHelper)` at RVA `0x07832330` resolves source/destination Mass entities/transforms and initiates relocation/state work. `UCrCharacterMovementComponent::UpdateTeleportState(bool)` at `0x075557E0` acquires a WorldPartition subsystem and calls `UWorldPartitionSubsystem::IsStreamingCompleted` at `0x07555E32`; it also performs relocation/collision/state operations. The movement state machine writes the shared `ULoadingScreenManager::bPlayerWaitingForWorldStreaming` storage: sets true at `0x0755719F`, clears on observed branches at `0x07556185`, `0x07556DA6`, `0x07556F07`, and `0x07557117`. The manager reads that state and emits “Player controller is waiting for world streaming.”

This proves a real teleport-streaming-to-manager reason path. It does not prove every teleport makes the manager widget visible, that every teleport follows this exact function, or that every teleport creates/replaces a world. The exact intermediate dispatch chain and ordering against every relocation branch remain unresolved. `ACrMegaTeleporter` symbols include additional fade/execution/completion functions but that family was not traced to a complete lifecycle.

### Historical freeze / regression evidence

**OBSERVED.** The initial incident investigation records a user-observed destination appearing without the usual screen, building up, then a system session where the game would not respond to Alt+F4 or permit reaching Task Manager; Windows sign-out eventually terminated it. A separate follow-up regression analysis records two reproducible user freezes. The precise game DLL/source identity was not proven (sourceRevision=unstamped). The initial investigation inspected two historical source revisions and logs; it found a completed Collector callback with a maximum near 858 ms and later game/UI diagnostic progress. The completed callback is not the indefinitely blocked operation.

The follow-up regression analysis separated changes: the earlier source revision advanced/coalesced grid and object-array validation; a later revision replaced some array-storage page probes with scoped access while preserving individual UObject extent probes. The reports identify temporal-admission differences and unsafe-read surfaces worth isolating, but no runtime invalidation, native nonreturn, deadlock cycle, GPU/driver fault, or source defect was established as the freeze cause. Teleport locality can trigger substantial synchronous MiniMap collection work without changing UWorld. A log’s last line or a large query count cannot identify a stopped operation. Hypotheses about array storage, validation timing, fragment borrows, terrain streaming, UI/native synchronization and GPU/display behavior remained conditional.

**REJECTED:** attributing the freeze to the completed 857 ms Collector pass as an operation that never returned. **UNKNOWN:** actual freeze root cause. No Unreal-wide rule should be inferred from this historical regression.

## Plugin initialization and hot-loading

The native trace from the preload/bootstrap closure investigation gives a non-actor bootstrap route: resolve GEngine storage, authenticate UGameEngine, read its GameInstance (UGameEngine +0x12C8 in the matched layout), authenticate GameInstance/WorldContext, then use the exact UGameInstance::GetSubsystem<ULoadingScreenManager>() getter at RVA 0x075DEC40 and verify manager Outer matches that GameInstance. This is evidence that an existing manager can be found without enumerating actors. It does **not** prove it exists at PluginInit, authorize creating a missing subsystem, or establish a race-free reusable bootstrap API.

**AlienX provenance limitation:** the statement that `UWorld::GetWorld()` during `PluginInit` can reveal an active `ChimeraMain` world is separately supplied context. It is not evidence of the wider native loading, manager, or DLL-lifetime contract. Current MiniMap source independently uses `SDK::UWorld::GetWorld()` during initialization and, if its name is `ChimeraMain`, routes through its existing world-begin and experience-complete handlers. This current path detects an already active gameplay world for MiniMap session admission. It does not authenticate the loading manager or report loading-screen state.

Current source also comments that lifecycle registration replays events only during loader startup, not runtime reload. It registers world begin/end and experience-complete callbacks, then performs the explicit active-world check. This means it has a current already-active-**world** path, but no authoritative loading-screen bootstrap. Current source does not contain the archived `LoadingAccess`/`LoadingObserver` system or native hooks for PreLoadMap, MoviePlayer, or preload transitions.

**UNKNOWN:** whether `PluginInit` always occurs before/after GameInstance/subsystem/preload/MoviePlayer setup; whether lifecycle callbacks are missed during runtime plugin reload beyond the explicit world check; whether unregister waits for in-flight callbacks; and whether unloading the plugin DLL during native callback execution is safe. The eight-hook historical observer's controlled-shutdown handling and retained thunks were implementation choices, not a loader guarantee. Do not claim `Unregister` proves physical DLL unload safety.

## Shutdown and cleanup

Current MiniMap's shutdown path stops input/config and CommonUI hooks, marks world-ending and gameplay unavailable, ends the building session, shuts down atlas/scheduler resources, unregisters world lifecycle callbacks, tears down map services and UI, clears identity state, then calls `MiniMapNative::Shutdown()` last. This is the observed source order in `Src/plugin.cpp`; it is not a proof of native callback quiescence or loader DLL-unload semantics.

Native manager `Deinitialize` removes its own delegate/widget resources and disables ticking, but omits normal false-display notification/clear. MoviePlayer `Shutdown` retires presentation resources; `DestroyMoviePlayer` clears the singleton and releases shared ownership, and destruction may be delayed by remaining shared refs. An uninitialized destructor can bypass Shutdown. Preload public Destroy retires the manager; engine process-exit cleanup has a separate destructor path. These are separate ownership boundaries; no single “hidden” callback covers them all.

No current or historical source finding establishes arbitrary concurrent hot unload as safe. Retained native references, callbacks into plugin code, queued diagnostics, and module code lifetime require a loader guarantee not present in this archive. Historical no-go decisions remain controlling context; no partial hook set should be treated as a complete lifecycle solution.

## Historical observer approaches and corrections

| Report | What it added | Correction/limit retained |
|---|---|---|
| Initial manager and teleport trace | Found manager class, show/update/hide/map hooks and teleport waiting flag; proposed post-original Update observation. | Native discovery was real, but initial preload/MoviePlayer/bootstrap and ownership boundaries were still open. Its candidate was not a complete contract. |
| Preload and MoviePlayer closure | Closed type-2 preload and MoviePlayer branches, startup call graph and non-actor GI/subsystem lookup. | GO applied only to an observation-only proposal; NO-GO for suspension until runtime presentation correlation. Runtime branch use remained to be observed. |
| Historical eight-hook observer prototype | Historical eight-hook Client observer with separate manager/preload/movie owner state and a 128-event store. | **SUPERSEDED IMPLEMENTATION.** Positive native bootstrap, actual hook lifecycle, pixel correspondence, reentrancy and gameplay behavior were unverified. Standalone fixtures did not validate native plugin build or gameplay. |
| Runtime observation correction | Corrected idle Wait noise, fault semantics, delayed log timestamps and reconciliation behavior; analyzed one runtime session. | It observed identity gaps and an unobserved manager dismissal gap. The first/second MiniMap appearance preceded known-hidden samples. No pixel capture; exact second dismissal unresolved. |
| Observer hot-path measurement | One matching run reported 1,995 Manager Update calls (15,126.560 ms summed observer segments; 7.582 ms mean) and 2,090 Movie Wait calls (4,238.180 ms; 2.028 ms mean). Source-derived successful-layout lower bounds were 584 region queries/idle Update and 24/idle Wait; those are not measured OS-query counts. | **NO-GO** for the observer's production hot path/performance validation; caching owner pointers without a stronger lifetime contract would fail same-address/changed-serial checks. Original native function time was excluded from measured segments; phase attribution and causal FPS loss were not established. |
| Native transition contract follow-up | Added manager delegate ABI, subsystem collection/GC retention, serial/free/reclamation and MoviePlayer/preload completion evidence. | Even with those functions identified, complete receipt semantics, reentrant ordering, lifetime invalidation, bootstrap and hot-path contract remained incomplete. Explicit **NO-GO** for complete replacement observer and loading suspension. |

The chronology is not a monotonic GO ladder. The preload/bootstrap closure added native coverage but did not overrule its runtime condition. The initial observer implementation was corrected after the runtime observation; the hot-path performance investigation then showed that suppressing duplicate logs did not remove repeated authentication. The later native lifecycle/ownership analysis closed useful subcontracts but expressly kept the complete-observer NO-GO. The presence of a function, delegate ABI or standalone fixture does not reverse that decision.

Observer callbacks in those reports were synchronous hooks around synchronous native functions. Manager `ShowLoadingScreen` can tick Slate and reenter; MoviePlayer Wait can tick the engine and trigger nested work. The reports did not prove all callbacks are non-reentrant or always on the game thread. Observer sample/detour ordering must not be generalized into guaranteed gameplay event ordering.

## Implications for MiniMap

These findings establish lifecycle boundaries for current MiniMap integrations:

- Treat a world/session generation, manager/GameInstance identity, preload owner, MoviePlayer owner, and presentation state as distinct identity/lifetime facts. Same-world teleport can change locality/readiness while manager/world identity persists.
- Lifecycle consumers use source-level interfaces while native bindings remain behind the existing mandatory fingerprint chain. Historical RVAs/patterns are evidence for a particular executable only.
- World/session generation guards remain useful for rejecting stale MiniMap publications; they do not authenticate UObject allocation lifetime or track loading UI.
- A confirmed loading dismissal may end a presentation interval only. It must not stand in for source-specific readiness, entity population, terrain/texture readiness, or identity validation.
- Cleanup decisions must distinguish borrowed pointer, validated current object, retained reference, and operation-specific safety. Do not remove current validity/GC boundaries based on historical “quiet” or root assumptions.
- Do not restore the historical loading observer or assume a safe cached-owner hot path; the hot-path and native lifecycle/ownership investigations found that owner lifetime and repeated admission remained blockers.

No interface shape, generation scheme, hook list, production suspension gate, or fallback scanner is prescribed here.

## Unresolved questions and negative findings

- **UNKNOWN:** exact alignment of plugin initialization with GameInstance, manager, startup preload and MoviePlayer creation; current source only checks for an active `ChimeraMain` world.
- **UNKNOWN:** actual visible-frame timing vs manager broadcasts, viewport operations, preload `HandleStop`, or MoviePlayer completion in current builds.
- **UNKNOWN:** all loading/reason/preload branches used on every save-load, return-to-menu, teleporter family, and later executable version.
- **UNKNOWN:** full manager/GameInstance/UObject allocation-reclamation coverage across every replacement/reconstruction path; the native caller/reclamation searches were bounded.
- **UNKNOWN:** complete MoviePlayer singleton creation/destruction and delegate subscription/removal semantics for every initialization path; Shutdown alone is not universal destruction coverage.
- **UNKNOWN:** complete PreLoadScreen activation/terminal coverage beyond the traced type-2 startup route and cited wait/destroy paths.
- **UNKNOWN:** callback thread guarantees, complete reentrancy/nesting behavior, safe observer detach, unregister quiescence, and physical plugin-DLL unload safety.
- **UNKNOWN:** cause of the historical teleport-freeze incidents; freeze-time thread/driver/OS evidence was absent.
- Historical authored-widget package/export inspection found no local Fade/Hide/dismissal graph in the extracted relevant exports, but inherited or complete Blueprint behavior was not ruled out.
- The configured hold duration in cooked config and native constructor defaults do not prove effective runtime CVar/CDO values in all runs.
- No evidence says every teleport is equivalent to full `PreLoadMap`/`PostLoadMap` travel.

## Evidence basis and current-source references

Historical native transition findings, observer results, and teleport observations are **REPORT-BACKED**, version-bounded, and classified in the body. The raw freeze stacks and runtime pixel captures were unavailable; the observer measurements describe one historical run and do not establish causal frame loss or general performance. Searches were bounded and do not establish universal event ordering, bootstrap, callback quiescence, or physical DLL-unload safety. Current MiniMap lifecycle behavior below was checked against repository source only.

| Current repository source | Relevance |
|---|---|
| `Src/plugin.cpp` | Active-world bootstrap, world generation, event registration and shutdown order. |
| `Src/Map/Map.cpp`, `Src/Map/Map.h` | Current world/session lifecycle consumers and map teardown. |
| `Src/Native/NativeApi.cpp`, `Src/Native/NativeApi.h` | Current native binding and lifecycle interfaces. |
| `Src/Map/Buildings/BuildingCollector.cpp` and focused lifecycle consumers | Current session admission/teardown behavior; does not implement the historical loading observer. |
| [Texture Publication](../ModLoader/Texture-Publication.md) | Narrow AlienX texture-reference retirement boundary; not a UObject or DLL-lifetime contract. |
