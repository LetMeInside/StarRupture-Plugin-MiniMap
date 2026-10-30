# Unreal Asset Readiness and Rendering Resource Lifetime

## Scope and version basis

This reference consolidates selected historical MiniMap research about Unreal asset availability, StaticMesh render data, renderer/capture readiness, and native capture-resource ownership. It is a documentation synthesis, not a current-runtime validation or an implementation specification.

Evidence labels follow the restored MiniMap documentation convention:

- **CONFIRMED** — supported by an identifiable primary artifact inspected in the archive, such as a matched PDB type/symbol, disassembly/decompiler excerpt, SDK declaration, engine source excerpt, or asset structure. Build-specific native evidence is explicitly qualified.
- **OBSERVED** — supported by a recorded runtime log, image, or measurement in a named historical run.
- **INFERRED** — a reasoned interpretation whose missing premise is stated.
- **UNKNOWN** — not established by the available evidence.
- **SUPERSEDED IMPLEMENTATION** — describes an earlier MiniMap source path that no longer governs the current architecture.
- **REJECTED** — contradicted by the inspected evidence.

Historical identity groups must not be merged by date alone. The matched client PE/PDB identity is image base 0x140000000, executable size 250,504,264 bytes, PDB size 2,533,404,672 bytes, PDB GUID 41cbaea9-7ac3-99da-b947-a1db9c1021ed, age 1, executable SHA-256 B2B9188700EC64DFAF721F969063F969D5A89F8F0B9DC8B6F7135A914F3E5308, and byte-ordered RSDS/PDB GUID a9aecb41c37ada99b947a1db9c1021ed, age 1; the GUID spellings represent the same bytes. The identified client is ++Earth20+Neon-CL-127004. This identity does not assign CL-127004 to unrelated observations or make an RVA stable.

The inspected public Unreal Engine reference source identifies Unreal Engine 5.6.1, CL 44394996, branch ++UE5+Release-5.6. It is Epic reference source, not proof that every source line matches StarRupture’s private P_Neon branch or build configuration. The dedicated server has a separate executable/ABI and is outside these client-native findings. The AlienX texture-copy evidence is separately qualified to interface 70 and its recorded local loader source; it is not an Unreal resource contract.

## Executive technical summary

| Finding | Evidence | Practical boundary |
|---|---|---|
| A reflected or resident UObject is not proof of asset streaming completion, render-resource initialization, CPU geometry retention, selected render representation, or completed capture. | **CONFIRMED** by distinct compiled predicates and source paths in the renderer-readiness investigation; missing conjunction remains **UNKNOWN**. | Keep ASSET LOCATED, ASSET LOADED, CPU DATA AVAILABLE, RENDER RESOURCE INITIALIZED and CAPTURE READY separate. No universal readiness state machine was established. |
| StaticMesh render data has LOD resources, sections, vertex buffers and an index buffer. The client PDB exposes concrete layouts missing from the generated SDK. | **CONFIRMED**, matched Client PDB type records and selected shipping instructions. | Offsets, allocator details, ABI and RVAs are specific to this client image/PDB. They are not public UE APIs or safe arbitrary SDK overlays. |
| Cooked CPU geometry may be discarded after upload when CPU retention is disabled. UObject retention and LOD references do not lock vertex/UV/index storage. | **CONFIRMED** for inspected UE 5.6.1 source and corresponding client instructions; actual Walkway owner-array state is **UNKNOWN**. | A CPU copy needs a valid, non-mutating access window plus live per-buffer owners and logical bytes. bAllowCPUAccess=false is a policy signal, not a direct live-array measurement; true alone is not a lease. |
| A queued capture call, a render command/fence, GPU readback completion and owned CPU pixels are different milestones. | Capture call contracts **CONFIRMED**; historical C0/C1/C2 outputs **OBSERVED**; a complete readiness predicate remains **UNKNOWN**. | CaptureScene returning is not render completion. A scene fence does not identify the view’s geometry/material choice. Readback verification is evidence for copied pixels from that job only. |
| Native capture components, target UObjects, RHI resources, readback owners, CPU pixel arrays and AlienX texture handles have separate owners and retirement rules. | **CONFIRMED** in current source boundaries and qualified historical ownership research. | Releasing a UObject wrapper or finishing one fence does not prove all asynchronous users/resources are synchronously retired. The historical renderer-extension/unload contract is incomplete. |
| A historical Nanite scene-capture assertion occurred in one CL-127004 run. | Assertion and stack **OBSERVED**; nearby UE source explains the condition; attribution to the nearest Blueprint capture is **INFERRED**. | This is a bounded failure case, not a general Nanite capture prohibition or verified universal defect. |

## Asset loading and residency

The inspected evidence supports these distinct states; it does not establish that every asset passes through one global sequence or that one predicate covers all of them.

| State | What the evidence supports | What it does not establish |
|---|---|---|
| **ASSET LOCATED** | A path/name lookup or resident object lookup can return an object identity. | That the object is loaded from disk, fully postloaded, the intended class, or safe to use through a later asynchronous operation. A failed resident lookup is not proof the asset does not exist. |
| **ASSET LOADED** | A valid loaded UObject/CDO can be synchronously retained and inspected under its own UObject/GC/world rules. A synchronous load can invoke IO/postload and other engine work. | RenderData presence, resident LODs, CPU array retention, render resource initialization, proxy creation, material shader state or capture output. |
| **CPU DATA AVAILABLE** | Per-buffer owner presence and live logical resource-array size can establish candidate CPU storage only inside a valid read scope. | GPU residency or successful rendering. Nominal counts, cached pointers and readable virtual memory are not evidence of live owned storage. |
| **RENDER RESOURCE INITIALIZED** | For StaticMesh, UStaticMesh::InitResources initiates initialization; FStaticMeshRenderData::bIsInitialized records initiation. A later queued command sets bReadyForStreaming. HasPendingRenderResourceInitialization performs a separate predicate. | GPU-idle, the intended LOD being resident/selected, CPU arrays being retained, a component proxy existing, or materials/PSOs being ready. |
| **CAPTURE READY** | No whole-capture predicate was established. Readiness would depend on the actual capture world/scene, component and proxy, view, rendering route, resources, material/shader/PSO state, scheduling and readback contract. | Any shortcut such as asset-loaded, IsFullyStreamedIn, pending=false, nonnull proxy, accepted CaptureScene, nonzero pixels, fence completion or elapsed delay as a universal certificate. |

**CONFIRMED — conventional StaticMesh streaming completion is multi-part.** In the researched client, StreamIn proceeds through IO/resource creation, then render-thread FStaticMeshStreamIn::DoFinishUpdate advances FStaticMeshRenderData::CurrentFirstLODIdx and requests cached render-state update. UStreamableRenderAsset::TickStreaming commits the requested resident count only after the update completes successfully; cancellation restores requested to resident and PendingUpdate is released. This is a report-backed matched-client source and binary observation. This is a state-publication path, not an invitation for MiniMap to call the private operations.

**CONFIRMED — predicates are narrow.** GetRenderData at client preferred-image RVA 0x024E0410 is a short borrowed-pointer getter and performs no load/wait. HasPendingInitOrStreaming(false), preferred-image VA 0x145280160 in the matched-client PDB identity group, checks PendingUpdate and initialization/transition hints without waiting; it may clear stale cached hint bits. False is compatible with absent or uninitialized render data. HasPendingRenderResourceInitialization, 0x145241530, is also false if RenderData is absent or uninitialized. The caller must establish resource existence and initialization separately. IsFullyStreamedIn uses a biased cached streaming target; true can include invalid/nonstreaming cached state and does not prove conventional authored LOD 0, proxy creation, Nanite readiness or material selection. False may reflect optional LOD policy rather than recoverable loading.

**CONFIRMED — game-thread and rendering-thread observations differ.** Streaming request creation is constrained to the game-thread timeline in the inspected paths; async/render work completes later. Render-side LOD selection requires the actual FStaticMeshSceneProxy and capture FSceneView and is view-dependent. Material resources also have distinct game-thread and rendering-thread state. The reports did not establish a safe MiniMap bridge that snapshots the relevant proxy/view/material state and protects its lifetime through observation. Do not dereference render-owned state on the game thread or retain borrowed native pointers across ticks.

A historical texture-specific investigation separately documents synchronous asset lookup and a borrowed UTexture2D result. That lookup behavior is texture-specific evidence and does not turn loading into capture readiness; the investigation explicitly notes that loading can cause IO/postload and does not by itself establish gameplay/world/GC admission or a retained lease.

## StaticMesh render-data structures

**CONFIRMED for the matched StarRupture client PDB.** Generated SDK declarations expose UStaticMesh::StaticMaterials and bAllowCPUAccess, while hiding RenderData and native render classes in padding. The offline PDB-derived type inventory and selected EXE instructions provide these client-specific offsets and sizes:

| Type / relation | Client-specific layout from matched PDB evidence |
|---|---|
| UStaticMesh, size 0x290 | RenderData TUniquePtr at +0xE8; bAllowCPUAccess +0x1D5 bit 1; ExtendedBounds +0x240. |
| FStaticMeshRenderData, size 0x98 | LODResources +0x00; NaniteResourcesPtr +0x40; bounds +0x50; bReadyForStreaming +0x8E; NumInlinedLODs +0x8F; CurrentFirstLODIdx +0x90; bIsInitialized +0x92. |
| FStaticMeshLODResources, size 0x2C8 | Sections +0x10; SourceMeshBounds +0x58; StreamingBulkData +0xA0; VertexBuffers +0xC8; IndexBuffer +0x1D0; DepthOnlyIndexBuffer +0x210. |
| FStaticMeshSection, size 0x1C | MaterialIndex +0x00; FirstIndex +0x04; NumTriangles +0x08; MinVertexIndex +0x0C; MaxVertexIndex +0x10; flags +0x14 through +0x18. |
| FStaticMeshVertexBuffers, size 0x108 | StaticMeshVertexBuffer +0x00; PositionVertexBuffer +0x88; color buffer follows. |
| FPositionVertexBuffer, size 0x40 | VertexData owner +0x20; cached Data +0x28; Stride +0x30; NumVertices +0x34; bNeedsCPUAccess +0x38. |
| FStaticMeshVertexBuffer, size 0x88 | TexcoordData owner +0x50; cached TexcoordDataPtr +0x68; TexcoordStride +0x74; NumTexCoords +0x78; NumVertices +0x7C; full-precision flag +0x80; NeedsCPUAccess +0x82. |
| FRawStaticIndexBuffer, size 0x40 | IndexStorage +0x18; CachedNumIndices +0x38; b32Bit +0x3C. |
| FIndexArrayView, size 0x10 | data +0x00; count +0x08; 32-bit flag +0x0C. |

**Container relationships matter.** LODResources is a TIndirectArray (0x10-byte container) containing pointers to separately allocated LOD records, not a flat array of structures. UStaticMesh::GetNumSections (client RVA 0x0523D990) follows that pointer array and selects the LOD entry. Sections use FStaticMeshSectionArray, a 0x30-byte TArray with TInlineAllocator<1>; inline section storage begins at +0, secondary allocator state at +0x20, count +0x28 and capacity +0x2C. Do not treat its first word as an unconditional heap pointer. IndexStorage is a TResourceArray with a memory-image allocator; FRawStaticIndexBuffer::GetArrayView (client RVA 0x05004950) decodes 16/32-bit storage and tagged relative pointers. It is not a plain pointer/count cast, and its hidden aggregate-return ABI must be respected by any binding. Other relevant client entries are UStaticMesh::GetRenderData (RVA 0x024E0410), FStaticMeshLODResources::SerializeBuffers (0x0524EB30), FStaticMeshLODResources::DiscardCPUData (0x0523A050), and FStaticMeshStreamOut::CheckReferencesAndDiscardCPUData (0x0526E4A0). They are offline navigation identifiers for this matched client, not runtime addresses or portable bindings.

PDB field layout evidence is **CONFIRMED for that binary**, not a stable Unreal public interface. The renderer-readiness and follow-up extension investigations additionally provide selective streaming/render-data offsets for a different group of native objects; they do not amount to complete type layouts. No historical source finding grants authority to make hardcoded-offset fallbacks or reuse a client offset against a server or later client build.

## Mesh sections, vertex buffers and UV data

**CONFIRMED representation; actual Walkway values UNKNOWN.** A section identifies a material slot and a range in that LOD’s main index stream. For section s, the triangle-index interval is FirstIndex through FirstIndex + 3*NumTriangles, with checked arithmetic. The decoded indices address LOD-global render vertices. MinVertexIndex/MaxVertexIndex describe the referenced range; they are not an extra index base. The section table does not provide per-section bounds. Depth-only/reversed index buffers are not the authored main-surface stream. MaterialIndex must be checked against that mesh’s material slots.

The selected shipping symbols/disassembly establish the relationship and getter behavior, but the research did not read actual Walkway sections, triangles, vertex bytes, UV coordinates, or section-to-upper-deck assignment. Three material slots do not imply three sections or a stable section order across LODs. The CDO’s material/texture metadata does not authenticate a section mapping. No general geometry-extraction method was proved safe for runtime use.

**CONFIRMED from UE 5.6.1 source excerpt; binary-layout interpretation remains build-specific.** Static mesh UVs are vertex-major. NumTexCoords is the channel count. Full-precision UVs store two float32 values per vertex/channel; default precision stores two half values. Decode only after confirming precision, stride, vertex count, owners and live byte length are mutually consistent. Positions have their own buffer. Index elements may be 16 or 32 bits. Preserve duplicated render vertices at seams: same positions can have distinct UVs/materials. FMeshUVChannelInfo density values are not UV coordinates and do not select a channel.

Section/vertex/material association is per render LOD; geometry interpretation must keep each section’s material identity with the triangles it indexes. Authored geometry is distinct from transformed geometry. In particular, SplineMesh deformation maps the mesh’s source-forward coordinate into spline parameter space for positions, while ordinary UV attributes travel separately with their vertices. The engine shader code cited in the matched-client mesh-layout investigation does not turn ordinary UVs into arc-length coordinates automatically. Module boundaries may restart authored UVs; continuity depends on actual endpoints, material expressions, placement and addressing. No specific Walkway tiling/repetition policy was established.

Spline deformation also prevents equating the historical MiniMap ribbon triangles with native StaticMesh triangles. Current spline geometry/occlusion keys identify plugin-owned input ribbon triangles, not UStaticMesh triangle IDs; the current adapter’s endpoint/centerline Z is not an authenticated authored top-surface Z. Barycentric UV reconstruction would only apply if each input triangle had an authenticated, single affine UV/material field; the research found no proof that a ribbon triangle crossing mesh seams, material boundaries or module resets meets that condition. This records a limitation, not a restored rendering algorithm.

The offline-authored-mesh decoding idea in the matched-client mesh-layout investigation was a proposed proof only. A later historical investigation records that it was not carried forward after resident acquisition was preferred. No asset extraction or offline decoder is part of the current source or this restoration.

## CPU buffer access and lifetime

The CPU owners identified by UE source and client disassembly are:

| Payload | CPU owner | Relevant release/replacement behavior |
|---|---|---|
| Positions | FPositionVertexBuffer::VertexData, backed by FPositionVertexData/TStaticMeshVertexData and a TResourceArray. | CleanUp deletes the owner; AllocateData replaces it; upload can discard/empty its resource array and release the owner. |
| UVs / tangents | FStaticMeshVertexBuffer::TexcoordData and tangent owner, backed by typed TStaticMeshVertexData arrays. | CleanUp deletes owners; allocate/conversion can replace them; upload can discard CPU storage. |
| Main indices | Embedded FRawStaticIndexBuffer::IndexStorage TResourceArray<uint8>. | Upload/Discard empties elements; SetIndices/serialization/rebuild can replace storage. The containing index-buffer object may remain after its bytes are gone. |
| LOD object | FStaticMeshRenderData::LODResources owns LOD records through TIndirectArray references. | LOD references protect LOD object lifetime and participate in stream-out reader accounting; they do not pin each CPU array. |

**CONFIRMED for inspected UE 5.6.1 cooked path.** UStaticMesh::bAllowCPUAccess is propagated as a retention choice during cooked buffer serialization. Position, tangent/UV and index data can be deserialized to create render resources, then RHI upload may discard CPU arrays when CPU access is not retained. The client’s SerializeBuffers instruction stream (RVA 0x0524EB30) checks the owner CPU-access bit. D3D12RHI source and the corresponding client RHI initializer show initial upload followed by Discard. This does not prove which precise runtime path any particular mesh took.

**CRITICAL lifetime restriction.** bAllowCPUAccess=false means post-upload CPU retention is not guaranteed and strongly predicts unavailable CPU arrays for an ordinary cooked, initialized non-retaining mesh. It is not a live sample of a particular array’s owner/length. Conversely, bAllowCPUAccess=true describes policy, not a lock against release/reallocation or proof that every needed buffer exists. The reports did not measure the actual Walkway arrays, so “Walkway CPU data was discarded” is **INFERRED**, not confirmed.

Do not authorize reads using cached Data, TexcoordDataPtr, NumVertices or CachedNumIndices alone. Cleanup may delete owners without clearing every cached pointer/count; CachedNumIndices can describe GPU geometry even when current CPU storage is empty. Non-null, nonzero, readable-page and SEH observations are not ownership/lifetime evidence. For indexes, GetArrayView is borrowed; its validity requires CPU access or not-yet-initialized resources and no later SetIndices. GetCopy/copy initializers allocate owned output but consume existing input bytes; they cannot recreate discarded storage. UKismetProceduralMeshLibrary::GetSectionFromStaticMesh warns when CPU access is disabled, still reads CPU buffers, and its cited helper copies UV0 only; it does not provide a streaming-safe lease or restore missing data.

**No unconditional safe copy window was established.** A historical matched-client investigation reasons about a possible short, synchronous game-thread quiescence scope for an already loaded, noncompiling mesh after resource initialization, bReadyForStreaming, and HasPendingInitOrStreaming(false), followed by owner and live logical-size checks. This is explicitly conditional, not a validated lock or implementation-ready protocol. The exact callback/parallel-update exclusion, game-specific resource replacement paths, and per-owner metadata-query ABI were still unresolved. HasPendingInitOrStreaming(false) is not literally bitwise read-only and does not lease CPU storage. Render-thread command ordering is not a global CPU-array lock; FRenderAssetUpdate::DoLock can block and is not a general reader lease; Tick(TT_None) executes update work rather than returning a retained read lock.

Thus the practical distinction is:

- **ASSET METADATA AVAILABLE:** object/LOD/section/nominal count/stride metadata may remain after payload storage is unavailable.
- **CPU RENDER DATA ACCESSIBLE:** requires a valid lifetime/quiescence scope and independently present, sized and retained position/UV/index backing arrays for the exact representation. Copy to plugin-owned memory before leaving that scope. A supported way to establish the scope and ABI was not verified.

Nothing here generalizes the Walkway observation to all StarRupture meshes.

## Renderer and scene-capture readiness

The renderer-readiness investigation separates asset and renderer conditions. For a conventional mesh, proxy construction checks compile state, RenderData, initialization, available LODs/vertices, PSO strategy and material eligibility. Nanite route selection includes platform/component/material audit. A proxy can be delayed, null, use default-material fallback, take conventional fallback, use Nanite, or be absent. The generated SDK does not expose all relevant native render-state structures.

Actual conventional LOD selection is view-dependent. ForcedLodModel=1 requests LOD 0 but can be clamped by mesh/component/platform MinLOD, stripped or empty LODs, streaming CurrentFirstLODIdx, global forced LOD and view policy. Automatic mode depends on view/projection, screen size, scalability and dither. Static relevance can be constrained by available batches. GetLOD alone is not a final universal selection observation; GetLODMask and renderer selection use the actual view/proxy on the render thread. A game-thread UObject pointer is not the correct render-owned receiver.

PSO and material readiness are distinct. IsPSOPrecaching() compares tracked component job counters; false does not prove every capture-pass shader/PSO is ready or no recreation is pending. DelayUntilPSOPrecached can produce no proxy; UseDefaultMaterialUntilPSOPrecached can substitute a default. UMaterialInterface lookup is feature/quality-specific and yields a game-thread resource distinct from render-thread FMaterial state. The renderer can fall back from the intended material depending on render-thread shader-map completeness. Checking original material UObject or non-null shader-map pointer does not prove actual section material, shader completeness, required PSO or texture residency. The full capture-pass PSO/resource contract was not established.

Nanite validity is also layered. HasValidNaniteData checks built page-state metadata, not initialization, root upload, detailed page residency, route selection or the capture view’s selected cluster cut. Registration/runtime resource ID can precede root-page upload. Detail requests, IO, page installation, GPU upload and view selection are separate. The Nanite view-readiness investigation found no reliable view-specific Nanite completion ticket in the inspected UE 5.6/client contracts. UE source exposes prefetch of the first 16 pages and explicit page requests without per-view completion callbacks; the shipping renderer request-recording getter returned -1 and setter was a no-op. This bounded search does not prove no private mechanism exists elsewhere.

The renderer-readiness investigation’s generic completion proposal was not completed: a true per-component Ready state would require the actual representation, selection, material and resource prerequisites. It classified renderer state as Unknown, and no safe capture-view/proxy/material snapshot bridge was demonstrated. The earlier readiness review’s numeric retry cadence was superseded because a missing native observation contract does not become available by waiting.

Scene-capture object configuration is not itself a readiness certificate. The component must belong to a live initialized world/scene; target object and underlying render resource must exist; the renderer must schedule and execute the capture for its view; and readback must complete before CPU pixels are usable. The historical capture code configured source, target and flags, registered the component and obtained the render-target resource, but those preconditions only prepared a capture job.

## Capture execution and readback completion

Use these milestone names precisely:

| Milestone | Evidence it represents | It does not mean |
|---|---|---|
| **CAPTURE REQUEST ACCEPTED** | The public/reflected CaptureScene invocation returns or a capture operation is submitted. The investigated public path flushes deferred end-of-frame updates before dispatch. | The render thread successfully executed the view, the capture used intended geometry/materials, GPU work completed, or output pixels were read. |
| **CAPTURE EXECUTED** | Evidence would need to identify the matching renderer/view job through the relevant render work. A general command fence can establish ordering/completion only at its documented depth. | Correct scene membership/view/LOD/material or GPU-to-CPU completion unless the evidence includes those results. |
| **GPU OUTPUT AVAILABLE** | Render target output has been written and a relevant GPU dependency has completed. | CPU mapped pixels exist or have been copied to owned storage. |
| **CPU PIXELS AVAILABLE** | The matching readback reached its ready state, map/copy/unmap completed, owned CPU bytes passed validation, and native readback/RHI owners are accounted for. | That the intended high-detail geometry/material was rendered; it only certifies this capture job’s copied output under its own checks. |

**OBSERVED, historical experiment.** The repeated-capture experiment records six PNG captures and matching logs: a Foundation capture kept the same alpha mask across C0/C1/C2; a Smelter capture changed between C0 and C1 and C1/C2 matched. This proves image output changed across that experiment’s capture sequence; it does not identify Nanite page demand, prove C0 caused the transition, or establish a universal tick delay. The Nanite view-readiness investigation preserves the limitations and confirms no page-identity completion signal was associated with those images.

**Current-source reconciliation.** Current MiniMap BuildingSpriteRenderer owns a private capture world, actor/component/target references, request state, NativeApi operations and a retirement fence. It records CaptureSubmitted before CaptureScene, then performs external GPU readback through ReadbackNativeAdapter. Adapter states distinguish readback-ready, copied pixels, verified result and cleanup-complete; it verifies GPU prerequisites and retains/quarantines uncertain ownership. The current renderer waits for cleanup/readback/native work to retire before deleting the readback job, then releases its fence and destroys/drops UObject references. This establishes source-level ownership policy, not current runtime correctness or a guarantee that CaptureScene produced the intended view.

## Native rendering resource ownership

The relevant ownership domains are separate:

| Resource | Owner / evidence | Boundary |
|---|---|---|
| Capture world, package, persistent level, world settings, subsystem references | Current MiniMap NativeWorldOwner holds individually tracked native UObject references and validates its isolated world. | A successful Cleanup/release of references does not prove immediate UObject destruction or independent renderer-scene retirement. Current code explicitly does not manually free its observed Scene pointer. |
| Actor and SceneCaptureComponent2D | Current active request holds native UObject references; unregister/destroy is performed through engine operations after world/capture admission checks. | DestroyActor/DestroyComponent and dropping references do not by themselves prove queued scene/proxy deletion has finished. |
| UTextureRenderTarget2D wrapper | Request holds target UObject and initializes it before obtaining the render-target resource. | UObject wrapper and RHI texture resource are distinct owners. |
| Render-target/RHI texture resource | Readback job acquires/retains RHI texture ownership in the native adapter. | Releasing target UObject or capture component is not a synchronous RHI retirement certificate. |
| FRHIGPUTextureReadback, staging resources, GPU fence and queued callable | Readback adapter owns these; report/phase/outstanding/callable state gates destroy. | Fence/queue uncertainty retains or quarantines ownership; CPU job completion alone does not prove RHI owners retired. |
| Capture scene work | Separate MiniMap render fence is inserted after detachment when capture was submitted. | The code comment says CaptureScene may enqueue scene work independently of the readback Job. The scene fence does not replace readback-job cleanup checks. |
| CPU float/RGBA pixels and converted sprite | Owned CPU arrays in the request/cache path. | Pixel lifetime is not RHI texture lifetime and not AlienX texture-handle lifetime. |
| AlienX PluginTextureHandle | Loader-owned opaque texture registry/resource reference; described in [Texture Publication](../ModLoader/Texture-Publication.md). | It is not UTexture2D*, render-target resource, RHI texture pointer or CPU image buffer. |

**Historical capture-extension evidence is partial.** The compiled SceneCaptureComponent2D layout places the capture-local weak-array data/count/capacity at +0xBC8/+0xBD0/+0xBD4, with 16-byte weak entries. The shipping capture component promotes live weak refs into the actual capture view family and calls registered callbacks. A global factory also registers extensions with the engine-global repository, so ‘capture-local’ collection alone does not establish isolation. Callback timing and verified vtable slots are partial; the follow-up ownership analysis leaves semantic slots 0x10, 0x18, 0x48, 0x50, 0x58, 0x68 and 0x70 unresolved. A complete DLL-compatible object/factory remains unresolved. The follow-up ownership analysis established asynchronous renderer Delete task behavior in the inspected build: FSceneRenderProcessor::End body at preferred-image VA 0x1434A56C0 / thunk 0x1434AD920 can dispatch Delete callable body 0x14349EE90 / thunk 0x1434AD7F0 after Cleanup/prior Delete prerequisites; Cleanup and Delete event references occupy 0x14E432228 and 0x14E432230 in that image. These are offline identifiers only. The report did not establish a nonblocking end-to-end retirement boundary safe for module unload.

Do not collapse wrapper ownership and underlying rendering ownership. A render fence only covers the exact preceding work and synchronization depth for which it was constructed/inserted. The reports do not prove that a fence alone guarantees SceneCapture proxy/material/renderer teardown, pending extension callbacks, GPU readback cleanup, or DLL-unload safety.

## Resource retirement and unresolved safety limits

**CONFIRMED, current source policy.** Capture retirement detaches the capture component first; if capture was submitted, inserts an RHI-depth fence behind capture work, waits nonblocking across service ticks, separately checks readback phase/callables/outstanding work and RHI owners, destroys the readback only after those conditions, releases the fence, then destroys/drops capture objects and target references. Uncertain state is quarantined; cancellation is not treated as retirement. The request retains CPU data until dependent native owners are retired. These are source behavior findings, not runtime validation.

**UNKNOWN, native extension retirement.** The decisive gap is completion/retirement of renderer Delete plus proof that no extension/controller/callable can enter MiniMap code before DLL unload. A callback or destructor setting a done flag in its body is insufficient because its epilogue may still execute in the DLL. Observing a shared strong/weak count at zero is also insufficient if destruction is still in progress; a final strong owner is not a completion certificate without proving ownership closure and preventing new promotion/raw borrows. Historical reports did not establish that PluginShutdown or callback unregister waits for all in-flight native work. Do not use this partial extension evidence as a general hot-unload contract.

**UNKNOWN, asset CPU-array retirement.** No general StaticMesh lock-and-copy API or unconditional CPU-data lifetime pin was found. The exact integration phase that excludes async mutation and replacement, and validated metadata-query ABI for concrete position, UV and index owners, remain open. A transient before/after state comparison does not replace a proven exclusion mechanism.

## Historical capture failure case studies

### Nanite scene-capture assertion

**OBSERVED, one bounded client run.** On 2026-10-17, the historical game log recorded Assertion failed: PrimitiveCount > 0 in NaniteCullRaster.cpp:3982. The installed client was ++Earth20+Neon-CL-127004, executable SHA-256 B2B9188700EC64DFAF721F969063F969D5A89F8F0B9DC8B6F7135A914F3E5308. The PDB-resolved render-thread stack runs through Nanite::FRenderer::AddPass_PrimitiveFilter (module RVA 0x030D5DE6), DrawGeometry, FDeferredShadingSceneRenderer::RenderNanite, UpdateSceneCaptureContent_RenderThread, FScene::UpdateSceneCaptureContents and FSceneRenderProcessor::Execute. A normal game-thread CaptureScene return about 2 ms earlier was therefore not successful render completion.

The available UE 5.6.1 CL 44394996 source has the same assertion at that line. Its surrounding logic reaches the check when Nanite primitive filtering is active and the persistent primitive-index count is zero. Under that source, with PrimitiveCount zero the filter-flags condition is false, so an active filter branch implies a nonempty hidden-primitive or show-only list. This source interpretation is **CONFIRMED for the inspected stock source excerpt**; the game’s private P_Neon branch may differ nearby, and the exact runtime counts/filter set are absent.

The nearest newly submitted MiniMap operation was Blueprint BP_Smelter C0, actor id 2/building 336, using a private actor capture and actor-level show-only path. Its call returned normally before the assertion. The static-template Smelter C0 was submitted about 32 ms after the assertion, so it is not the nearest submitted cause. The stack does not name a capture component, target or world; attribution to Blueprint C0 is **INFERRED**, not proven. No matching CrashContext XML/minidump or runtime primitive counts were available. The evidence does not establish why the persistent primitive count was zero, prove the Blueprint had no geometry, or prove concurrent captures were involved.

The precise negative result is: one logged Nanite primitive-filter assertion occurred during render-thread scene-capture processing under the recorded run. It does not establish that Nanite scene capture is generally unsupported. The Nanite show-flag experiment’s NaniteMeshes=false change was build-verified but not runtime-tested; its effective flag value was not independently read back. The Nanite resource/detail investigation records image/camera correlations without page/view readiness proof. The repeated-capture experiment records later image change, but not a Nanite causal metric. The Nanite view-readiness investigation finds no reliable view-specific readiness signal in its bounded inspection. These are distinct evidence, not one universal rule.

### Other failed or limited capture approaches

- **Static capture readiness retry proposal — SUPERSEDED IMPLEMENTATION / REJECTED as a readiness proof.** The earlier readiness review proposed timed numeric retries; the renderer-readiness investigation rejected that inference because missing native observation contracts are not time-dependent. It explicitly left full renderer state Unknown and production publication disabled in that historical diagnostic source. That source state no longer describes current MiniMap.
- **Nanite show-flag isolation — SUPERSEDED IMPLEMENTATION.** The show-flag experiment temporarily set NaniteMeshes=false only for one public static route; no runtime capture or effective-value check was done. Current source’s SceneCaptureConfiguration does not reproduce that experimental conditional setting. No inference about current effective flags follows.
- **Repeated retained-scene capture — OBSERVED but causally unresolved.** The repeated-capture experiment’s Smelter alpha changed from C0 to C1 and stabilized at C2, while Foundation remained stable. The Nanite view-readiness investigation states that C0-induced page requests, elapsed ticks, PSO/proxy/material preparation or another renderer state were not distinguished.
- **Capture-local observer extension — INCOMPLETE contract.** The initial and follow-up investigations verified portions of the engine mechanism but did not establish construction, complete ABI, safe ownership/retirement and DLL-unload guarantees. It was not implemented and is not a failed runtime experiment.
- **Texture import into AlienX — distinct direction and resource class.** A historical texture-interface investigation documents AlienX LoadFromUTexture2D as copying from an Unreal texture resource into a loader-owned texture. That does not import an AlienX handle into an Unreal UMG brush and does not furnish an isolated mesh-rendering service.

## MiniMap implementation boundaries

These findings describe boundaries present in the current implementation.

- UObject lookup/retention does not establish StaticMesh CPU buffers, renderer readiness or a capture completion contract. The renderer-readiness investigation notes its query candidates are native/game-specific and require correct thread, receiver, identity and build validation.
- MiniMap keeps owned CPU images, AlienX opaque handles and engine UTexture2D/RHI resources as distinct resource types. The documented LoadFromUTexture2D copy direction is Unreal-to-loader. The texture-publication reference documents interface-70 ownership/release; it does not expose a native capture/readback bridge.
- Capture and readback use StarRupture engine objects, render-thread command scheduling, RHI ownership, GPU readback, native fences and explicit retirement. Generic UObject/reflection access does not replace MiniMap’s game-specific NativeApi capture/readback path.
- Reuse only the fact that the current source centralizes required native functions in NativeApi and fingerprints and keeps capture/readback ownership in game-native modules. Existing current code is not evidence that new readiness probes are safe or that old PDB offsets remain valid.
- Do not use a MiniMap UI texture lifetime rule to retire native capture resources. A capture/readback job can have independent RHI and callable owners after CPU publication or UI use has ended.

## Corrections and superseded conclusions

| Historical claim or implementation | Correction retained here |
|---|---|
| The earlier readiness review’s numeric retry cadence could turn capture Unknown into Ready. | The renderer-readiness investigation supersedes this: without a readiness query contract, time/retry is not evidence. **REJECTED** as a generic readiness rule. |
| Completion of streaming means intended detail is drawable. | The renderer-readiness investigation distinguishes completion/residency from selected view LOD, proxy, material/PSO and Nanite state. Completion is only one prerequisite. |
| A loaded UStaticMesh, non-null GetRenderData or bIsInitialized proves CPU arrays exist. | Matched-client mesh-layout and CPU-buffer-lifetime investigations show upload discard and separate ready/init flags. CPU owner/live byte presence is separate; actual Walkway state was not acquired. |
| bAllowCPUAccess alone proves all CPU buffers are present or permanently safe. | **REJECTED.** It is a retention policy, not an owner lease or live size query; owner arrays can be replaced/released, and per-buffer state matters. |
| A retained mesh or LOD reference pins its CPU vertex/index arrays. | **REJECTED.** UObject/LOD object lifetime is not buffer-array lifetime; streaming/upload can discard or replace storage. |
| The initial capture-extension investigation’s installed component-local extension is safe to implement because callback slots were partly identified. | The follow-up capture-extension investigation extends the ownership analysis and leaves several vtable slots, construction and renderer Delete/unload retirement unresolved. The interface is not implementation-ready. |
| The follow-up ownership analysis proves renderer-extension resources are safe after one fence or plugin shutdown. | **REJECTED.** The report’s explicit unload/async-delete blockers remain open; readback retirement is not extension retirement. |
| The historical stack proves Nanite is unsupported in scene captures. | **REJECTED.** It is one failure under one game build/configuration; active primitive filtering and a zero persistent primitive count are the stock-source condition, while capture attribution and cause remain unresolved. |
| The Nanite show-flag experiment’s NaniteMeshes=false build result proves the flag effectively disabled Nanite or fixed capture. | **REJECTED.** It was build-only, no runtime test, no effective renderer flag readback. Current source no longer contains that experimental conditional. |
| Old capture-pipeline statements in the earlier readiness review and subsequent Nanite experiments describe current code. | **SUPERSEDED IMPLEMENTATION.** Current BuildingSpriteRenderer has a private-world request owner and NativeApi capture/readback/retirement path. Historical publication gating, retry policy, show-flag experiment setting and archived prototypes are not current source contracts. |

## Unresolved questions and negative findings

- Actual Walkway per-LOD section values, material mapping, position/UV/index bytes, CPU owner presence and live storage sizes were never acquired.
- No general safe StaticMesh CPU-buffer lock/copy API was found. The proposed quiescent GT window still lacks complete integration exclusion and concrete owner-query ABI validation.
- It is unknown whether any StarRupture asset path or asset-specific configuration preserves relevant CPU arrays after initialization; one mesh’s bAllowCPUAccess observation cannot be generalized.
- A full capture Ready predicate tying world/scene, component/proxy, route, view, selected LOD or Nanite cut, material/shader/PSO, render-target execution and readback was not established.
- The Nanite view-readiness investigation did not find a view-specific Nanite readiness signal in bounded source/binary coverage; this is not proof no private mechanism exists anywhere.
- The historical run has no matching minidump/CrashContext, no runtime primitive/filter counts and no capture-view identity. The Blueprint attribution and underlying defect remain unresolved.
- The follow-up capture-extension investigation did not establish nonblocking renderer Delete completion and callback/code-quiescence proof sufficient for safe physical plugin unload.
- Current MiniMap source was inspected statically only; current executable behavior, game-version compatibility, source-level native ABI correctness and capture success were not validated during this review.
- No conclusion here establishes CPU-buffer availability for every cooked mesh, renderer readiness for every capture, server ABI behavior, future UE versions, or a general Nanite limitation.

## Evidence basis and current-source references

Historical native and runtime findings are **REPORT-BACKED** unless the text identifies current MiniMap source or retained primary evidence. The client-specific layouts, streaming transitions, capture observations, and crash stack are qualified to the executable/PDB identity stated above. Raw crash context, some runtime images/logs, full native layouts, and private P_Neon source were not retained for independent inspection. Missing Walkway mesh values, universal readiness, crash cause, and unload safety remain unknown as stated; no omitted artifact is treated as proof of those claims.

Current MiniMap behavior was checked read-only against repository source. This establishes inspected source behavior, not live executable compatibility.

| Current repository source | Relevance |
|---|---|
| `Src/Map/Buildings/BuildingCollector.cpp` | Authored StaticMesh use is bounds/profile metadata and spline transforms; no section/vertex/index/UV extraction. |
| `Src/Map/Buildings/BuildingSpriteRenderer.cpp` | Current private-world/request/component/target ownership, capture scheduling, readback and conservative retirement. |
| `Src/Native/ReadbackNativeAdapter.cpp` | Current native RHI/readback owners and phase/retirement checks. |
| `Src/Native/NativeApi.h`, `Src/Native/Fingerprints.cpp` | Centralized typed game-native readback/fence/texture functions; not proof that new mesh APIs exist. |
| [Texture Publication](../ModLoader/Texture-Publication.md), [Loading and World Transitions](../Lifecycle/Loading-and-World-Transitions.md) | Published explanations of the separate AlienX texture and UObject/world lifetime boundaries. |
