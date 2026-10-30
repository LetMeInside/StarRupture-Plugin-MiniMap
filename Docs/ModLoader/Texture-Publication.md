# AlienX Texture Publication and Resource Lifetime

## Scope and version basis

This reference documents the interface-70 texture contract, bounded historical observations, and MiniMap's current use of that interface. AlienX Mod Loader is MiniMap's active supported backend. Historical findings are version-bound; the current MiniMap comparison below comes from focused, read-only source inspection.

The preferred atlas investigation records **StarRupture CL-127004, AlienX Mod Loader v1.22.0, interface 70**, dated 2026-10-19. Its loader implementation evidence came from a local source checkout whose exact identity with the installed DLL was not proven. Public API signatures and interface-70 behavior are attributed to the [AlienX StarRupture Plugin SDK interface header](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/include/plugin_interface.h). The main URL is mutable and does not identify the historical local checkout or installed DLL; implementation observations remain separately qualified below. A separate terrain upload observation dated 2026-10-26 does not record a game build or loader version; its local loader checkout was not proven identical to the installed DLL and it lists a supplied GTX 1650 SUPER without measured device/driver characterization. The cold-capture observation records a 2026-10-19 log interval but does not identify game build or loader version. Do not transfer the preferred investigation’s version tags to those observations.

A bounded inspection of the public SDK interface header confirms interface min/max/current **70** and the texture function signatures below. This verifies the checked-in SDK declaration, not the installed loader binary, game build, or runtime behavior. No game or loader binaries were inspected.

## Executive technical summary

- Interface 70 offers named image loads from file, encoded memory, raw RGBA, and `SDK::UTexture2D`; it returns an opaque `PluginTextureHandle`. It has `FreeTexture`, dimension/capacity queries, and ImGui image/draw-list functions. It exposes no texture update, region upload, external-resource registration, upload ticket, or completion-poll operation. A same-name live load shares the first registered pixels instead of replacing them.
- The SDK says all `Load*` calls block until the GPU copy completes and must run in a game-thread callback outside the ImGui render callback. This establishes a blocking API boundary, not permanent latency or proof that a frame has presented the image when the call returns.
- In interface 70, each successful load is a reference balanced by one `FreeTexture`. `FreeTexture` immediately withdraws the handle from drawing and defers resource/descriptor release until GPU frames that could use it complete. The header does not establish plugin DLL-unload safety or callback quiescence.
- MiniMap still uses the relevant AlienX calls, but manages complete atlas revisions: CPU candidate, synchronous `LoadFromRGBA`, dimension validation, atomic texture/UV publication, then old-owner release. This is consumer-level whole-texture replacement, not an AlienX in-place update.
- An AlienX `PluginTextureHandle` is an opaque ImGui identity, not an Unreal `UTexture2D` or UMG/Slate brush. `LoadFromUTexture2D` copies an engine texture into a separate loader-owned resource; it does not convert a loader handle into an engine resource.

## Verified AlienX texture APIs

The signatures below are attributed to the [public AlienX SDK interface header](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/include/plugin_interface.h). `PluginTextureHandle` is declared `void*` and documented as opaque; `nullptr` means invalid/failed. `IPluginImGuiTextures` is client-only and unavailable on server/generic interfaces.

| API | Signature | Established behavior |
|---|---|---|
| `LoadFromFile` | `PluginTextureHandle (*)(const char* utf8_path, const char* name)` | Loads WIC formats (PNG/JPG/BMP/GIF/TIFF); blocks through GPU upload. |
| `LoadFromMemory` | `PluginTextureHandle (*)(const void* data, size_t size, const char* name)` | Decodes the same formats; input need be valid only for the call; blocks through upload. |
| `LoadFromRGBA` | `PluginTextureHandle (*)(const unsigned char* rgba, int width, int height, const char* name)` | Four-byte RGBA, top-left row-major; input need be valid only for the call; blocks through upload. |
| `LoadFromUTexture2D` | `PluginTextureHandle (*)(SDK::UTexture2D* texture, const char* name)` | Copies to a loader-owned GPU resource; blocks through copy; returned handle persists until freed. Not a live binding. |
| `FreeTexture` | `void (*)(PluginTextureHandle handle)` | In v70, safe from any thread; withdraws drawing immediately and defers GPU resource/slot release until prior GPU uses finish. |
| `GetSize` | `void (*)(PluginTextureHandle handle, int* out_width, int* out_height)` | Queries natural dimensions; either output pointer may be null. |
| `GetFreeSlotCount` | `int (*)()` | Reports free slots; retiring slots remain occupied. An observation, not a reservation. |
| `GetCapacity` | `int (*)()` | Reports total texture-slot capacity. |
| `Image`, `ImageButton` | See SDK: `void (*)(PluginTextureHandle, float, float)`; `bool (*)(const char*, PluginTextureHandle, float, float)` | Draw inside an active plugin render callback/ImGui frame. |
| `IModLoaderImGui::DL_AddImage`, `DL_AddImageQuad`, `DL_AddImageRounded` | Signatures take `PluginTextureHandle` | Draw-list image calls require a handle obtained from `IPluginImGuiTextures`; this is not a raw ImGui texture ID. |

Loads implicitly create/register or share a loader-managed texture. **No separate `Create`, `Update`, `UpdateRegion`, `RegisterExternalTexture`, `UnregisterTexture`, or texture-table `Shutdown` function appears in the inspected interface.** `FreeTexture` is the texture removal/release operation. A slot query cannot reserve the slot for a later load.

The SDK documents `std::out_of_range` when no slots are available and `nullptr` for loader-not-ready or load failure. A case-insensitive matching live name reuses its resource and increments a reference count; first registration's pixels win. Every successful call, including a name hit, carries exactly one matching `FreeTexture` obligation. `LoadFromMemory` and `LoadFromRGBA` explicitly limit input-buffer validity to the call. File and memory decode costs are not separated in the public contract.

## Texture creation and publication

**CONFIRMED CONTRACT.** SDK interface 70 states that `Load*` blocks until the GPU copy completes, must be invoked from a game-thread callback, and must not run inside the ImGui render callback. Rendering functions belong inside an active plugin UI callback. Successful-call return is the API completion boundary; no separate readiness token or completion poll is exposed.

**OBSERVED BEHAVIOR, not a guarantee for every loader build.** The historical local-source inspection describes the corresponding local loader source path: new RGBA names were copied into loader-managed GPU storage, submitted on a private DIRECT queue, and followed by an infinite CPU event wait before record return. The report says the texture mutex covered upload and publication. Exact local-source-to-installed-binary identity was unproven. Treat private queue and record details as source observations; the public blocking/thread rule is the API contract.

The historical CPU-side evidence is a per-call timing scope around `LoadFromRGBA`, not isolated GPU execution time. It describes local source copying input rows into staging before return; the public contract only requires the input pointer to remain valid for the call. On a same-name hit, the historical source inspection found no new upload and no pixel replacement. A successful call return does not prove that a draw command was submitted or that a frame displayed the image.

**UNKNOWN.** The API exposes no scheduling queue, descriptors, fences, mutable CPU backing store, external-resource import, asynchronous publication, or independent way to observe upload completion. Native game render/readback access does not establish a bridge into the loader's ImGui registry.

## Update and replacement behavior

There is no public in-place whole-texture or subregion update operation. Reusing a live name shares its existing image; changed pixels with that name do not replace it. A unique name can acquire another loader-owned texture, but replacement and retirement sequencing belong to the caller. `LoadFromUTexture2D` is a blocking copy into loader ownership, not a changing view of its source.

MiniMap uses **consumer-managed replacement**: form a full CPU atlas candidate, load it under a unique name, validate returned dimensions, atomically switch the published texture and matching UV/status entries, then release the old owner. The historical account documents this pattern. Current `Src/Map/Buildings/BuildingAtlas.cpp` still performs it; `Src/Map/Buildings/BuildingAtlasAlienX.cpp` generates a GUID-plus-serial name and calls `LoadFromRGBA`. This is not an AlienX update feature.

## Resource ownership and lifetime

| Classification | Finding |
|---|---|
| **CONFIRMED CONTRACT** | `PluginTextureHandle` is opaque. Every successful load acquires one reference; balance it with one `FreeTexture`. Name sharing is case-insensitive and first-registration pixels win. Caller RGBA/memory bytes need only remain valid through the call. |
| **CONFIRMED CONTRACT (v70)** | `FreeTexture` can be called from any thread. It makes the handle non-drawable immediately and retains the GPU resource and descriptor slot until frames that could reference them have completed. Retiring slots are not immediately free. |
| **OBSERVED BEHAVIOR (local source; binary match unproven)** | Preferred report describes pointer-like handles to loader registry records, fixed slots, and render-fence retirement. It says `FreeTexture` withdraws the record and later render-thread reclamation releases its resource/descriptor after the loader render fence. Do not depend on pointer representation or internal fence values. |
| **REASONABLE INFERENCE** | Since the SDK limits input validity to the call and defines completion by return, a caller can release temporary input after a successful return. This is not evidence that the loader owns the caller's CPU buffer. |
| **UNKNOWN** | The SDK does not specify plugin DLL-unload safety, whether all callbacks are quiescent before physical unload, or a complete shutdown handshake for outstanding callbacks/loads. It does not make a freed handle valid for later drawing; stop using it once released. |

V70 retirement is the loader's resource-lifetime contract. It does not transfer ownership of the loader fence or let a plugin retire arbitrary resources through it. There is no public release-completion callback or retirement token. A one-time successful cleanup observation would not extend the documented contract.

## Shutdown and retirement

The SDK's example lifecycle is load during initialization/game-thread work, draw during render callbacks, and call `FreeTexture` from `PluginShutdown`. It declares no texture-table shutdown function. Release each known acquired reference exactly once while the borrowed interface is valid, then stop using the handle/interface. This is an API-use pattern, not proof that loader shutdown waits for callbacks or that physical DLL unload cannot race them.

A separate historical local-source analysis reports that the inspected local loader source removed hooks, waited up to 500 ms on a render fence, then cleaned D3D12 resources; it also notes unchecked timeout/error and active-load quiescence limitations. The source checkout was not proven to match the installed DLL. Treat this as a historical implementation warning, not v70 contract or current loader behavior. The same section reports unchecked `Close`, `Signal`, `SetEventOnCompletion`, and wait results in its upload path. This qualifies confidence in source-path failure handling; it is not evidence of a runtime contradiction under normal successful calls.

**UNKNOWN:** safe plugin self-unload during an in-flight texture call, exact callback-unregister completion semantics, device-loss recovery for the registry, and whether failed GPU waits make resource release safe. No new loader teardown or runtime investigation was performed.

## Performance observations

Figures are historical wall-clock milliseconds, not latency promises. No GPU timestamp queries or confidence intervals were reported.

| Run/workload | Measurement | Result | Interpretation/limits |
|---|---|---:|---|
| Preferred atlas observation; 2026-10-19; StarRupture CL-127004, AlienX 1.22.0/interface 70; 44 cold-capture atlas revisions; log SHA-256 `b17cb8ffadad0b734e588dc608fb32831d589c9d475b63775d4d3b4ac5613eaf`; game thread 24456, UI thread 9336 | Existing per-call wall-clock scopes; exact `LoadFromRGBA` scope, 44 successful calls | Median **25.445**, max **223.191** | Max is revision 43 (Hub), inside the API call; includes any call work/wait, not measured GPU execution. |
| Same run, revisions 43 and 44 | Both 1024×1024 RGBA (4 MiB); same upload scope | **223.191** vs **25.447** | Later same-sized call was far shorter; size alone does not explain the outlier. No component attribution. |
| Same run; 44 per-revision publication scopes | Existing inclusive wall-clock timers | Median **43.295**, max **260.557** | Includes work beyond upload; nested durations must not be summed. |
| Same run; candidate preparation / atlas PNG export | Existing wall-clock scopes, 44 samples each | Prepare median **3.059**, max **7.208**; export median **14.746**, max **32.388** | Export follows commit in the reported implementation; encoding, file I/O and scheduling are not separated. |
| Separate terrain observation; 2026-10-26; GTX 1650 SUPER listed as supplied environment | Terrain texture aggregates at five timestamps; 16 new textures | `LoadFromRGBA` total **407.6**, mean **25.475** per call; a three-texture batch upload **66.3** | Different workload. Game build, loader version, individual sample IDs and internal phase timings absent. Kept separate, not pooled. |

The preferred atlas observation gives no basis for attributing the 223.191 ms outlier to allocation, mutex wait, row copy, driver scheduling, GPU work, fence wait, or teardown. Commit lock and old-texture retirement intervals were short, but occur outside the earlier load-call timer. A separate cold-capture analysis cautions that log-event brackets and cumulative callback maxima cannot establish per-frame causality.

The historical atlas model found 44 blocking calls for each modeled strategy. Smaller immutable images may lower logical bytes, but do not remove the blocking call per new texture; a time benefit was not established. No separate warm-reload timing sample is reported; same-name reuse without GPU work is the interface behavior, not a measured warm latency.

## MiniMap service and ownership boundaries

These findings describe the current boundary between MiniMap services and the AlienX-specific texture implementation:

- **AlienX adapter:** using this v70 API requires `Load*` on the documented game-thread callback and outside the ImGui render callback. A worker/UI call cannot make this API asynchronous.
- **MiniMap consumers:** atlas pixels, texture identity and matching UV/status metadata form one revision. Keep the prior valid publication until the candidate is loaded and validated; failure/cancellation must not publish a partial candidate. Current source demonstrates this consumer requirement.
- **Ownership/lifetime:** track each successful reference and release it exactly once; keep a published handle alive while draw data can use it and let the loader perform its own v70 retirement. Keep CPU image ownership, API input lifetime, published identity, recorded UI draw data and GPU retirement distinct. These boundaries keep loader-specific resource rules out of MiniMap's general rendering logic.

## Known limitations and unresolved questions

- Absence of update/import/async operations is a bounded finding about the inspected interface-70 table, not every AlienX version.
- The raw timing-analysis JSON and page-model inputs were unavailable for independent inspection. The 44-call figures remain report-backed from the documented log hash and method; they were not independently recomputed.
- Local loader-source-to-installed-binary identity is unproven. Private queue, registry, descriptor, fence and shutdown details are source observations only.
- The separate terrain aggregates omit game build, loader version and individual sample IDs; they remain contextual and separate from atlas metrics.
- Behavior after interface 70, current installed loader version, device-loss recovery, callback quiescence, and physical plugin DLL unload safety remain unresolved.
- Successful load does not prove image presentation; successful `FreeTexture` does not prove plugin-unload safety.

## Historical corrections and superseded conclusions

- An earlier investigation considered conditional loader-internal optimization or batching ideas. Later evidence did not establish those ideas as implemented functionality or demonstrate a speedup. The separate terrain upload observations and unchecked-error-path limitations remain historical context with their provenance qualifications.
- A historical MiniMap source account describes earlier source state. Its candidate/UV atomicity and old-owner-after-commit concepts remain relevant; its stage narrative and old paths are not current specifications. Current atlas source was inspected for the live mechanism.
- A separate cold-capture analysis does not isolate `LoadFromRGBA` internals or prove upload alone caused a freeze. Event brackets and cumulative callback maxima are not promoted into upload timings.
- The preferred atlas observation's “no verified nonblocking path” conclusion applies to inspected public interface 70, not every later AlienX API.

## Evidence basis and current-source references

The public SDK header supports the interface declarations and documented v70 contract cited above; its mutable `main` URL does not establish the revision inspected during the historical work or the identity of an installed DLL. Local-loader implementation details and the performance figures are **REPORT-BACKED**. The historical source checkout was not proven identical to the installed binary, and the raw timing-analysis JSON/page-model inputs were unavailable for independent recomputation. These facts do not change the stated separation between public API contract, source observations, one-run measurements, and unresolved runtime behavior.

| Reference | Use and boundary |
|---|---|
| [AlienX StarRupture Plugin SDK interface header](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/include/plugin_interface.h) | Public declarations and interface-70 contract; mutable branch, not installed-binary identity or licensing evidence. |
| `Src/Map/Buildings/BuildingAtlasAlienX.cpp`, `Src/Map/Buildings/BuildingAtlasAlienX.h` | Current adapter create/release/submit path, unique names and explicit ownership state. |
| `Src/Map/Buildings/BuildingAtlas.cpp`, `Src/Map/Buildings/BuildingAtlasCore.cpp`, `Src/Map/Buildings/BuildingScheduler.cpp` | Current whole-texture candidate publication, lifecycle serialization, packing and service batch. |
| [Rendering and Asset Capture](../Engine/Rendering-and-Asset-Capture.md) | Separate capture/readback resource ownership; not an AlienX texture bridge. |
