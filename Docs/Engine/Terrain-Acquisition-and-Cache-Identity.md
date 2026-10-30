# Terrain Acquisition and Persistent Cache Identity

## Scope and version basis

This reference separates historical native research from the current MiniMap source state. It is about the game's built-in map terrain and MiniMap's CPU/disk acquisition path; it does not specify a new renderer.

Historical findings come from the 25 October 2026 terrain provenance, terrain acquisition, and graphics revision investigations. The terrain acquisition report associates its runtime log with StarRupture CL-127004 and AlienX ModLoader v1.22.0/interface 70, but states that the plugin DLL was sourceRevision-unstamped. Its code observations came from an earlier source checkpoint. The terrain provenance investigation gives no game build and is an implementation-era report. The graphics revision investigation describes an earlier cache identity source state.

Current reconciliation is a static inspection of Src/Map/Terrain/{Terrain,TerrainSource,TerrainCache,TerrainChunks}.*, Src/CacheIdentity.h, Src/Native/{Fingerprints,NativeApi,TextureAccess}.*, Src/Native/MapMenuDevSettingsLookup.h, Src/plugin.cpp, and Src/Map/MapTransform.h. No current executable, PDB, native disassembly, runtime, or loader binary was checked. Current source therefore establishes what this checkout expresses, not that a current game build accepts every native binding.

Evidence labels used below: **CONFIRMED** means visible in identified current source or retained primary evidence; **OBSERVED** means a specific historical runtime record; **INFERRED** means a reasoned interpretation; **UNKNOWN** means evidence is absent; **SUPERSEDED IMPLEMENTATION** marks an old implementation state; **REJECTED** marks a contradicted hypothesis. Report-only statements are called out as such.

## Executive technical summary

- **CONFIRMED (current source):** `TerrainSource` finds the unique `CrMapMenuDevSettings` class-default object through the Objects service, synchronously resolves its `TerrainData` soft field, reads terrain origin/segment size/segment records, maps a world position to a terrain grid, selects the ordinary or Radiation2 brush, and asks the native brush helper for a `UTexture2D*`.
- These are separate states: a soft reference/brush is **discovered**; `LoadSynchronous` may make the terrain-data UObject **loaded**; brush extraction may return a **texture object**; platform and mip metadata may be **accessible**; BulkData may be copied and validated; only then have **pixels been acquired**. Neither an object pointer nor a streaming wait proves CPU bytes exist or contain a complete pixel image.
- The present acquisition consumes **mip zero**, requires the game's terrain source to be an 8×8 arrangement of 256-pixel chunks (2048×2048 source), pixel format identifier 5 (BC1/DXT1 in this implementation), non-virtual-texture data, and the exact expected BulkData byte count. It extracts compressed block rows into individual chunks, decodes each chunk to tightly packed RGBA8, then calls the loader texture API. Current source does not select among mips by quality or inspect resident/allowed/total mip counts.
- **CONFIRMED (current source):** `GetCopy` output, when non-null, is treated as a native allocation and released through the paired native `FMemory::Free` binding. The copied buffer is temporary. The original BulkData/mip/platform pointers are borrowed. This checkout does not establish every engine-side precondition for a successful copy or make the copy atomic against concurrent asset mutation.
- Terrain disk files are named by radiation and segment grid, and contain 64 BC1 chunks plus a 60-byte header. The current header validates dimensions, format, sRGB and chunk layout, but does not contain a terrain asset or brush variant identity, a source-byte digest, or a payload checksum. Exact file length and full reads are checked; same-sized payload corruption can still pass those checks.
- Root `CacheInfo` currently gates persistent cache access using a SHA-256 digest over validated executable `RT_VERSION` resources plus a manually maintained graphics revision. That is plugin cache authorization and broad game-version compatibility, not terrain pixel identity. There is no session/world identity in that record. Terrain cache clear is part of full cache clearing; graphics-revision-only invalidation clears building cache alone.
- **Correction:** A later historical implementation describes a later disk format v2 with a selected-variant byte and exact current-source compressed-byte comparison before resident/disk reuse. That is **SUPERSEDED IMPLEMENTATION** relative to the inspected current checkout: `TerrainCache.cpp` defines v1 and its API has no source-pixel proof parameter; `Terrain.cpp` resident matching uses radiation/grid/chunk coordinates. Do not treat that historical validation or its reported test results as behavior of current source.

## Built-in terrain source discovery

The current source route is:

```text
TerrainSource::TryResolveSourceTile(world position)
  -> cached CrMapMenuDevSettings CDO (Objects service class-default lookup)
  -> NativeApi.asset.loadSynchronous(&CDO->TerrainData)
  -> UCrMapMenuTerrainData fields and TerrainSegmentsData
  -> world position -> (grid X,Y) and local (U,V)
  -> matching FCrTerrainSegmentData
  -> NativeApi.texture.getBrushTexture(terrain brush)
  -> selected UTexture2D* returned in a borrowed SourceTile

Terrain::LoadChunkTexture(SourceTile)
  -> waitForPendingInitOrStreaming(texture, true, true)
  -> TextureAccess::QueryPlatformData / QueryMipData(0)
  -> optional validated cache read, otherwise GetCopy of BulkData
  -> BC1 chunk extraction and RGBA decode
  -> ImGuiTextures->LoadFromRGBA
```

Current `TerrainSource.cpp` makes no widget instance, `UWidgetBlueprintLibrary`, map-menu screen, or displayed-brush lookup. It reads `TerrainSegmentsData` directly from the terrain data UObject. On this source path, opening or displaying the map UI is not a coded prerequisite for acquiring a brush resource. Whether every segment brush is initialized before the UI is shown is **UNKNOWN**; no runtime test was made. The source stores the settings CDO pointer only until source `Shutdown`/`Initialize` reset and stores no native terrain object as a long-lived cache. `SourceTile::Texture` is a raw borrowed pointer scoped to synchronous acquisition; current code does not retain/root it across world transitions.

The desired condition ladder is intentionally strict:

| Condition | What current source/evidence establishes | Does not establish |
|---|---|---|
| Asset reference discovered | The settings CDO has `TerrainData`; segment structs contain ordinary and Radiation2 brush fields. | A loaded UObject or a usable brush resource. |
| Asset loaded | Current code calls `LoadSynchronous` on the `TerrainData` field and checks a non-null return. | Texture object, texture platform data, CPU BulkData, or pixels. |
| Texture object available | Current brush helper returns a non-null `UTexture2D*`. | Valid platform/mip pointers, resident data, complete CPU bytes, or safe lifetime beyond this call. |
| Texture data accessible | Current helper obtains platform/mip metadata and a positive-size/non-null BulkData pointer after its checks. | That BulkData is currently resident in RAM or that GetCopy will succeed. |
| Pixels acquired | A non-null copied buffer is then checked against expected BC1 byte size and read by extraction. | That metadata and payload were an atomic snapshot if the underlying asset changed during the operation. |

## Map-menu settings and soft object references

Current `Src/Native/MapMenuDevSettingsLookup.h` calls `Services::TryObjects()->ClassDefaults("CrMapMenuDevSettings", records)`, accepts exactly one result with non-null object, and casts it to `SDK::UCrMapMenuDevSettings*`. This is not a raw object-array scan in `TerrainSource`. If lookup is unavailable, not ready, ambiguous, or empty, it logs and fails; it does not guess a settings object.

`TerrainSource::LoadTerrainData` passes the address of `g_devSettings->TerrainData` to `NativeApi.asset.loadSynchronous` as `void*`. The typed current function pointer is `SDK::UObject* (*)(void*)`; the source casts the returned pointer to `UCrMapMenuTerrainData*` after null checking. On Microsoft x64, the code uses the platform's ordinary x64 function ABI; the typedef has no explicit `__fastcall` annotation. This is the current binding declaration, not independent proof of a particular executable's native ABI.

The current settings field and segment members are accessed through generated SDK types. This restoration did not inspect a matching SDK declaration excerpt, PDB, or native call site that verifies the `TerrainData` member offset, `FSoftObjectPtr` internal layout, concrete `LoadSynchronous` native target/RVA, or reference-count/GC ownership after return. Those remain **UNKNOWN**. Do not interpret a successful synchronous load as a guarantee that the texture's CPU BulkData is available. The selected historical reports provide source-path descriptions, but not primary native call-body evidence for those details.

## Native terrain loading

Current source obtains the active world from `MiniMapMap`, reads `world->GameState`, casts to `ACrGameStateBase`, then reads `MapMenuDataReplicationHelper->CurrentRadiationLevelReplicated`. Null at any step fails the source resolution. The terrain data supplies `MapTerrainTopLeftPivotPoint`, `MapTerrainSegmentSize`, and an array of `TerrainSegmentsData`; each segment supplies grid index and ordinary/Radiation2 brushes.

The grid mapping implemented in `TerrainSource::TryResolveSourceTile` is, with world coordinates in Unreal centimeters and segment sizes converted to centimeters:

```text
worldTileWidth  = 100 * MapTerrainSegmentSize.X
worldTileHeight = 100 * MapTerrainSegmentSize.Y
horizontalTile  = (worldPosition.X - pivot.X) / worldTileWidth
verticalTile    = (worldPosition.Y - pivot.Y) / worldTileHeight
gridX = floor(verticalTile)
gridY = floor(horizontalTile)
localU = horizontalTile - gridY
localV = verticalTile   - gridX
```

It linearly scans segment records for exact `(TerrainSegmentGridIndex.X,Y) == (gridX,gridY)`. Negative coordinates use `floor`, so local fractions remain in `[0,1)` for finite inputs. Current code checks positive segment world sizes and missing segment records, but the conversion to `int` has no explicit representable-range guard in the inspected source. It does not clamp to an inferred world rectangle; missing grid records fail.

Variant rule is explicit: at replicated radiation level 1, ask the Radiation2 brush for a texture; if the helper returns null, use the ordinary brush. At other radiation levels, use ordinary. A non-null Radiation2 texture selected at level 1 that later fails metadata/copy/decode/upload does not switch to ordinary; source-selection fallback is only for missing Radiation2 brush texture. `SourceTile` returns radiation, grid, per-position local UV, selected variant and borrowed texture pointer.

## Texture2D acquisition

The current function-pointer APIs are housed in the centralized mandatory NativeApi chain. `Fingerprints.cpp` resolves mandatory scans into `ResolvedAddresses`; `NativeApi::Initialize` assigns typed subsystem pointers. Texture consumers do not own independent scan tables and do not store historical addresses. Current signatures visible in `NativeApi.h` include:

| Operation | Current typed signature shape | Current path usage |
|---|---|---|
| Synchronous soft load | `UObject* (*)(void*)` | `TerrainSource::LoadTerrainData`; input is address of the settings field. |
| Brush texture | `UTexture2D* (*)(const FSlateBrush&)` | Called on segment ordinary/Radiation2 fields. |
| Pending-init/stream wait | `void (*)(UStreamableRenderAsset*, bool, bool)` | Called once per nonresident chunk with `(true,true)` before metadata access, including cache-hit attempts. |
| Platform data | `void* (*)(UTexture2D*)` | Returns an opaque platform-data pointer. |
| Bulk size / disk capability | `int64_t (*)(const void*)`, `bool (*)(const void*)` | Reads metadata from a mip's embedded BulkData object. `CanLoadFromDisk` is recorded but current terrain code does not branch on it. |
| Bulk copy | `void (*)(void*, void**, bool)` | Copy called with `discardInternalCopy=false`; caller adopts the output if non-null. |
| Free | `void (*)(void*)` | Paired release of the copied allocation. |

The internal native field is named `getBrushTexture`; historical research/inventory also calls the target `UWidgetBlueprintLibrary::GetBrushResourceAsTexture2D`. This restoration found no matching retained PDB/native body excerpt proving that exact exported/mangled name, callsite RVA, or argument/return convention for the game's executable. Current pointer type and consumer behavior are **CONFIRMED current source**; exact executable native name/RVA/signature and behavior are **UNKNOWN** from selected evidence. Do not hardcode a historical RVA as a recommendation.

## Texture platform-data layout

Current `Src/Native/TextureAccess.cpp` uses a private, manually described layout rather than generated SDK members:

| Current local layout | Offset / size | Interpretation in current helper |
|---|---:|---|
| Platform `SizeX`, `SizeY`, packed data, format | `SizeX +0`, `SizeY +4`, packed +8, format byte +0xC | `FTexturePlatformData`-like metadata. |
| Mip pointer container | `+0x18` | Exposes mip pointer array and mip count/capacity. Local layout total `0x38`. |
| VTData / CPUCopy | `+0x28` / `+0x30` | Opaque pointers copied to output. |
| Mip BulkData storage | mip `+0x20`, size fields at `+0x48` | Mip local layout size `0x50`; `SizeX/Y/Z` are 16-bit at +0x48/+0x4A/+0x4C. |

These are **CONFIRMED current helper assumptions and compile-time assertions**, not newly verified matching game-build structure layouts. The local helper also reads an sRGB flag byte at UTexture offset `0x106`, mask `0x01`. It requires non-null texture, platform-data return, mip pointer container, selected mip pointer and native function pointers. `QueryMipData` bounds the mip index against reported MipCount, then calls native size and disk-capability getters. Terrain caller separately requires source metadata 2048×2048, pixel format 5, no `VTData`, positive mip count, mip pointers, mip-zero dimensions equal platform dimensions, nonzero `SizeZ`, non-null BulkData and exact byte size.

Current evidence does **not** show that a valid platform pointer guarantees resident bytes; platform, mip, and BulkData addresses are borrowed. The caller copies bytes before later decode, but [Rendering and Asset Capture](Rendering-and-Asset-Capture.md) discusses the broader distinction between asset metadata readiness and CPU storage lifetime. This document does not duplicate its general render-data findings.

## Streaming and mip residency

Current `NativeApi` declares `SetForceMipLevelsToBeResident`, `WaitForStreaming`, `GetNumResidentMips`, `GetNumMipsAllowed`, `GetNumMips`, `StreamIn`, and `WaitForPendingInitOrStreaming`. The current terrain acquisition path uses only `WaitForPendingInitOrStreaming(texture,true,true)`, then requests mip index 0. It does not call explicit force-resident, `StreamIn`, resident-mip, allowed-mip, or total-mip functions. It does not explicitly poll a streaming result or enforce a timeout; the wait function returns `void` in the binding.

Therefore the states remain distinct:

1. **Requested residency:** no direct force-resident or StreamIn request occurs in the current terrain path.
2. **Reported residency:** resident/allowed/total mip APIs exist in NativeApi but are unused here.
3. **CPU data available:** established only by a usable BulkData object/size and successful non-null copy, not by wait return.
4. **Valid pixels copied:** current size/format/dimension checks and chunk extraction must also succeed.

The wait's exact `bool` semantics, blocking guarantees, timeout/failure behavior, and whether `(true,true)` means full or merely pending-operation completion are **UNKNOWN** from selected primary evidence. The historical acquisition account says its recorded calls were synchronous and could block, but explicitly treats this as report-level API/source interpretation rather than a phase measurement. Do not infer that the wait establishes full mip residency or CPU availability; do not move it or add game-thread waits based on these reports. No selected terrain runtime diagnostic conclusively documented a “ready but no CPU mip” case.

## BulkData access and pixel ownership

Current ownership path:

```text
borrowed mip->BulkData object
  -> GetBulkDataSize / CanLoadFromDisk metadata
  -> GetCopy(bulk, &out, false)
  -> require non-null out pointer
  -> validate reported size before reading expected bytes
  -> extract requested/all tile chunks into plugin-owned vectors/files
  -> FMemory::Free(out) through NativeApi.texture.memoryFree
```

`GetBulkDataSize` is typed as signed 64-bit return; terrain checks equality with an expected 64-bit BC1 calculation before copy and extraction. The expected source byte calculation is `(width/4)*(height/4)*8`, with width and height fixed by preceding checks to 2048. `CanLoadFromDisk` is queried but its boolean is not used as a precondition or fallback. `GetCopy` has `void` return and reports success only indirectly through a non-null output. Current source checks a null output and releases non-null allocations through the native memory-free function. It does not call CRT `free`, `delete[]`, or `FMemory::Free` from a separate allocator.

The exact native declaration/body contract (whether GetCopy may fail by exception, how it reports I/O errors, what the third argument means, allocation internals, behavior for locked/unavailable disk data, and whether it can partially initialize output) is **UNKNOWN** from the selected native evidence. The current wrapper initializes the destination to null before invocation; its success condition is solely non-null output. Do not generalize this to a guaranteed copy. A source-size mismatch, null output, unsupported format, or failed extraction returns failure; the path does not manufacture zero pixels.

The native copy is freed before decode/upload on the current miss route. Extracted compressed and RGBA vectors are plugin-owned CPU memory, and loader upload consumes the RGBA pointer only during the call under the existing loader interface contract documented in [Texture Publication](../ModLoader/Texture-Publication.md). This does not make engine `UTexture2D`, platform data, mip, or BulkData pointers owned or safe across a world transition. World/session lifetime rules are covered generally in [Loading and World Transitions](../Lifecycle/Loading-and-World-Transitions.md).

## Texture formats and pixel conversion

Current supported terrain source is strictly pixel format numeric identifier **5**, interpreted by current code as BC1/DXT1. Virtual-texture data is rejected. Other formats are unsupported in this path; no generic conversion exists. No row pitch is consumed: BC1 data is treated as tightly packed blocks. Expected mip-zero byte count is `2048/4 * 2048/4 * 8 = 2,097,152` bytes.

The tile is divided into 8×8 chunks, each 256×256 pixels. A BC1 chunk contains 64×64 blocks ×8 bytes = 32,768 bytes. Extraction copies 64 consecutive eight-byte blocks per block row, for 64 block rows, at source offset `((startBlockY + blockRow) * sourceBlocksPerRow + startBlockX) * 8`. Chunk coordinates are `floor(localU*8)` and `floor(localV*8)`, accepted only when each is in `[0,8)`.

Decoder behavior in current `TerrainChunks.cpp`:

- RGB565 endpoints are little-endian; 5/6-bit channels expand with `(value*255+15)/31` and `(value*255+31)/63`.
- If endpoint0 > endpoint1, colors 2 and 3 are integer weighted `(2*c0+c1)/3` and `(c0+2*c1)/3`; otherwise color2 is average and color3 is transparent black `(0,0,0,0)`.
- The 32-bit selector word is little-endian; each pixel selects two bits at index `y*4+x`, and pixels write byte order R,G,B,A.
- Output is a tightly packed 256×256 RGBA8 buffer with stride 1024 bytes. No row-pitch input, vertical flip, channel swap, or explicit alpha postprocess is present in this decoder.

This establishes the MiniMap conversion routine, not a separate native confirmation of cooked asset orientation or cross-platform block layout. The selected historical research contains no independently retained terrain screenshot/byte fixture validating top-versus-bottom orientation, tile mirroring, or color-space appearance. There is no known supported uncompressed source format in current terrain code. The old runtime account reports 2048 BC1 mip-zero terrain and 256-pixel chunks; its report-only/runtime confidence should not be read as a new observation in this restoration.

## Terrain tiles, bounds and projection

Source layout and presentation layout are separate concepts. The game source records segments by grid index and defines a top-left pivot plus per-segment world size. The present code proves a coordinate-to-grid lookup and per-tile UV/chunk math, but not a complete finite terrain arrangement. The code obtains segment count dynamically from `TerrainSegmentsData.Num()`; no fixed world-wide tile count, guaranteed rectangular arrangement, tile overlap/gutter, or edge-clipping contract was established in selected evidence.

For a terrain segment, current source maps world X to `gridY`/local U and world Y to `gridX`/local V (the grid axes are transposed by the game's existing representation). Within a tile, local UV maps to `chunkX=floor(U*8)`, `chunkY=floor(V*8)`. Global presentation chunk coordinates in `Terrain.cpp` are derived as `globalChunkX = gridY*8 + localU*8` and `globalChunkY = gridX*8 + localV*8`; reverse mapping uses mathematical floor division and positive modulo so negative world grid values map to nonnegative local chunk indices.

Each chunk represents 125 world meters per axis in current code, while StarRupture positions are centimeters, hence `kChunkWorldUnits=125*100=12,500 cm`. The viewport computes its circular broad-phase radius in chunk units from viewport width/height, meters-per-pixel and half extents; candidate global chunks map to center world position by:

```text
targetWorldX = playerWorldX + (globalChunkX + 0.5 - playerGlobalChunkX) * 12,500
targetWorldY = playerWorldY + (globalChunkY + 0.5 - playerGlobalChunkY) * 12,500
```

The current MiniMap viewport then projects copied world XY through `MapTransform`: subtract tracking-world origin, rotate by the configured map rotation, multiply by `1/(100*metersPerPixel)` pixels per world centimeter, and translate to viewport center. It does not use terrain asset pixel coordinates as screen coordinates directly. This formula alone does not establish map image orientation or a general display-layout policy.

## Persistent terrain cache identity

Current `TerrainCache` root is `<plugin module directory>\MiniMap\Cache\Terrain`; per-radiation directory is `R<n>` and segment file is `Terrain_<gridX>_<gridY>.terrain`. The root identity record is passed as `<plugin module directory>\MiniMap\Cache\CacheInfo` by current plugin initialization.

The current terrain file header is packed to 60 bytes and has: magic `MMTCACHE`; uint32 format version 1; signed grid X, grid Y, radiation; uint32 source width/height, pixel format, chunks-per-axis, chunk width/height, bytes-per-chunk; one sRGB byte; seven reserved zero-initialized bytes. It is followed by 64 chunks in row-major order (`chunkY*8+chunkX`), each 32,768 bytes. Exact expected total is 60 + 64×32,768 = 2,097,212 bytes. Current writer checks source dimensions, format 5, exact whole-mip byte size, extracts every chunk, writes a `.tmp`, closes/checks the stream and publishes using `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`; write/rename failure is non-authoritative to acquisition. The current reader checks identity-ready gate, key, file existence and exact file length, validates all described header metadata against current live width/height/format/sRGB, seeks to the requested chunk, reads the exact-sized payload and treats any failure as a miss. It does not check reserved bytes, content checksum, or source variant.

Current persistent identity layers:

| Concept | Current basis | Limitation |
|---|---|---|
| Cache key / namespace | Root plugin path; terrain radiation directory; grid and chunk coordinates; graphics-independent terrain filenames. | A path/key is not proof that the stored pixels came from the currently selected brush. |
| Cache content | Raw compressed BC1 chunks, not decoded RGBA or GPU handles. | No payload digest/checksum; same-size payload corruption is not detected by length/header checks. |
| Per-file validity | Format/key/dimensions/pixel format/chunk geometry/bytes per chunk/sRGB, exact file size and complete stream read. | No selected variant, asset path/object identity, texture revision, exact source-byte comparison, or per-payload corruption detection. |
| Root authorization | 48-byte packed `CacheInfo`: `MMCACHE1`, format version 1, 32-byte SHA-256 of validated executable RT_VERSION resources, uint32 graphics revision 1. | This hashes version resources, not the executable image or terrain pixels; it is not a world/session or asset identity. |
| Cache lifetime | Plugin-owned files; current startup compatibility logic and optional exit clear. | No world pointer/generation in persistent identity. Root compatibility is global to plugin directory. |

No cryptographic identity for terrain content is implied by the SHA-256 root record. It is a digest over a selected executable-version resource set. The root has no session identity. Whether every game build change changes that resource digest is **UNKNOWN**; compatibility is tied to those version-resource bytes and manually bumped graphics revision, not explicitly to a full build ID, terrain asset path, or source texture content.

Runtime resident cache in current `Terrain.cpp` is separate from the disk cache. `ChunkIdentity` consists of radiation, grid X/Y, chunk X/Y. Resident matching compares those values; it does not compare brush variant, texture address, or bytes. Runtime `UWorld*` and map world-generation checks guard presentation/reconciliation operations and are not persisted. Resident handles are plugin-loader texture references, retained and retired according to render generations, not engine texture ownership.

## Cache validation and invalidation

`CacheIdentity::Initialize` executes before cache consumers. It hashes validated executable VERSIONINFO resources and reads the exact-size root record. Missing/malformed root record, forced clear, or executable digest mismatch invokes full cache clearing (terrain and buildings); graphics revision mismatch with matching digest clears building cache only. A successful clear is followed by atomic-ish temporary-file `CacheInfo` publication; cache authorization is not enabled on clear or metadata-write failure. If fingerprint acquisition fails, current code clears both caches, removes the root record, then authorizes this run without a persistent identity record; next startup cannot treat an absent record as a compatibility hit. Exit clear only runs when configured, closes/drains file access, and attempts the known terrain/building clears.

Terrain cache per-file mismatch, missing file, wrong size, failed open/read, bad header, or failed chunk read returns a miss/failure to acquisition. The reader does not delete a bad file. Successful miss acquisition obtains current mip bytes and tries to replace the full tile file. Temporary write failures remove the temporary path in the ordinary error paths; atomic replacement is the publication mechanism. The task's current-source inspection did not exercise interrupted clearing or filesystem failure at runtime.

The root `GraphicsRevision` is manually incremented for building graphics semantic changes, per the current comment. Current terrain format version is its own `kCacheVersion=1`; no automatic link from terrain behavior to graphics revision exists. No local cache-only source asset revision is recorded. `ClearCache` configuration drives full startup clear and optional exit clear; existing clearing owns the cache trees rather than arbitrary files in plugin root.

## World transitions and cache lifetime

Runtime source pointers (CDO, terrain UObject, brush-derived texture, platform data, mip and BulkData) are not persistent identities and are not serialized. Current `TerrainSource::Shutdown` nulls its cached CDO pointer. Terrain presentation invalidation and world generation are handled in runtime map/lifecycle code; these controls do not alter disk cache namespace. A later session re-resolves settings/terrain source and can use the same disk path if root authorization and per-file metadata validation pass.

This creates a compatibility boundary: world pointer/generation protects current in-process presentation; radiation/grid/chunk key locates a file; executable VERSIONINFO digest and graphics revision authorize broad persistent cache use; neither proves same-world/same-terrain/same-brush/same-bytes. [Loading and World Transitions](../Lifecycle/Loading-and-World-Transitions.md) and [Rendering and Asset Capture](Rendering-and-Asset-Capture.md) cover generic lifetime/readiness contracts. This restoration does not claim native texture UObject retention or safe pointer use after a transition.

## Historical failures and corrections

| Case | Evidence/status | Reconciliation |
|---|---|---|
| Settings CDO or terrain data null | Current source logs for missing/unavailable/ambiguous settings CDO or null `LoadSynchronous` return. **CONFIRMED current failure branches.** | No fallback settings object is selected. Native reason and runtime frequency **UNKNOWN**. |
| World, GameState, replication helper or segment missing | Current source null-checks each dependency and exact grid record, then returns failure. | This is not evidence of a terrain asset defect. Exact failure prevalence **UNKNOWN**. |
| Brush texture absent | Current source logs null brush result; Radiation2 absence at radiation level 1 selects Ordinary. | A found non-null Radiation2 texture failing subsequent texture extraction does not trigger Ordinary fallback. |
| Unsupported/missing mip data | Current source rejects invalid size/format/VT/mip pointers/BulkData size and fails load. | No selected runtime artifact conclusively ties a “ready but CPU bytes unavailable” symptom to one cause. Do not diagnose with mips alone. |
| Cache entries can describe another source | The historical acquisition account reports the older header omitted SelectedVariant/source brush identity; current source still omits it. A later historical implementation describes a variant-aware exact-pixel proof design. | Current code confirms the omission. The historical proposed/then-implemented proof does not exist in current source. No cause or fix is inferred here. |
| Earlier exact-pixel provenance and v2 header | Historical source-state description and reported 39 focused checks / 15 preservation checks. | **SUPERSEDED IMPLEMENTATION** relative to current source (version 1, no provenance proof type/API). Historical test claims are report-only and not re-run. |
| Historical acquisition stall | Historical runtime log on CL-127004/ModLoader interface 70; sourceRevision unstamped. Report observed 19 cache hits and callback timings, but phase boundaries mixed logging/stream teardown/decode/upload/scheduling. | **OBSERVED report-only:** delays clustered after HIT records; no exact dominant phase proven. Not current runtime behavior. Its optimization ideas are excluded, not reinstated. |
| Orientation, bounds, incomplete copies, failed cache clear/publication | No primary terrain image fixture, exact native failure trace, or current runtime reproduction inspected for this restoration. | Cause and current presence **UNKNOWN**. Current source checks stream/copy sizes and writes via temporary replacement, but source inspection is not runtime validation. |

No recovered evidence justifies restoring a widget-display dependency, source-pointer cache, mip-residency bypass, blocking wait change, or historical cache provenance patch. The historical acquisition account explicitly treated wait bypass and cache-probe reordering as unsupported by evidence. The alternate historical source state is evidence of an alternate historical source state, not authority for present architecture.

## Implications for MiniMap

- Keep the current architecture distinction between selection metadata, borrowed engine pointers, owned compressed bytes, decoded RGBA, and loader-owned GPU texture handles.
- Source reconciliation shows the cache identity limit clearly: current disk/header and resident key can collide across ordinary/Radiation2 source changes if radiation/grid/chunk and metadata coincide. This is a description of current semantics, not authorization to change format or renderer.
- Any later terrain cache work needs separate authorization and a contract for asset/source identity, bytes and versioning; this restoration does not prescribe or implement one. Do not treat file names or the root CacheInfo digest as content fingerprints.
- Native bindings remain mandatory and centralized through Fingerprints → ResolvedAddresses → NativeApi → typed API → consumer. The inspected source adds no evidence for optional scans or hard-coded addresses.
- Core pixel and cache processing uses owned CPU data and explicit plugin-side identity. Native source resolution and native texture bindings remain StarRupture-specific.

## Unresolved questions and negative findings

- Matching native executable identity, PDB symbols, RVAs and concrete signatures for LoadSynchronous, brush extraction, wait, platform-data getter, BulkData methods and FMemory::Free: **UNKNOWN** in the selected native evidence. Current source has mandatory fingerprints and typed declarations, but this restoration did not inspect a retained relevant native body/callsite artifact.
- `UCrMapMenuDevSettings::TerrainData` exact member offset and internal `FSoftObjectPtr` representation/ownership: **UNKNOWN**.
- Whether map-menu widget display is needed to initialize particular brushes: **UNKNOWN**; current consumer doesn't require a widget instance.
- Native wait semantics, whether it blocks and for what readiness state, whether the booleans request flush/streaming behavior, and any timeout: **UNKNOWN** beyond the report-backed source/runtime interpretation.
- `GetCopy` error contract, disk-load prerequisites, allocation edge cases, thread contract, and atomicity of pixel snapshot: **UNKNOWN**. Current code uses non-null output as the success indicator and the paired native free.
- Valid-but-empty/unavailable CPU data after apparently completed streaming: no verified terrain-specific example inspected; **UNKNOWN**.
- Full native terrain segment count/arrangement, exact outer bounds, neighboring edge continuity, gutters/overlap, image orientation and mirroring: **UNKNOWN**.
- Exact game-build or asset revision invalidation for terrain pixels: **UNKNOWN**; current root digest is VERSIONINFO-resource based and per-file header has no asset ID or source byte digest.
- Current-runtime effect of cache mismatch, interrupted clear, disk permission issue, corrupted-but-correct-size payload, and source-variant transition: **UNKNOWN**; source branches only were inspected.
- No selected evidence proves platform metadata pointer ownership or pointer validity past a streaming/GC/world boundary.

## Evidence basis and current-source references

Native acquisition, ABI, timing, and historical cache/format findings are **REPORT-BACKED** unless the text explicitly identifies current source or retained primary evidence. Raw native callsite artifacts and some timing/fixture evidence were unavailable for independent inspection; unknown wait semantics, native ownership, fixture behavior, and historical format limitations remain as stated. Current cache identity and implementation behavior below are based on inspected MiniMap source and do not establish runtime results.
### Current-source anchors

The following repository-relative source paths identify current MiniMap implementation:

- `Src/Map/Terrain/TerrainSource.cpp`, `Src/Map/Terrain/TerrainSource.h`, `Src/Native/MapMenuDevSettingsLookup.h` — CDO lookup, synchronous terrain soft-field resolution, radiation/grid/variant selection, brush texture pointer.
- `Src/Map/Terrain/Terrain.cpp` — readiness wait, metadata validation, mip-zero copy/cache/decode/upload, viewport chunks and resident identity.
- `Src/Native/TextureAccess.cpp`, `Src/Native/TextureAccess.h`, `Src/Native/NativeApi.h`, `Src/Native/NativeApi.cpp`, `Src/Native/Fingerprints.cpp`, `Src/Native/Fingerprints.h` — local layouts, typed signatures, required address resolution and initialization.
- `Src/Map/Terrain/TerrainChunks.cpp`, `Src/Map/Terrain/TerrainChunks.h` — BC1 extraction, chunk constants and decoder.
- `Src/Map/Terrain/TerrainCache.cpp`, `Src/Map/Terrain/TerrainCache.h` — v1 format, file path/header validation/read/write/clear.
- `Src/CacheIdentity.h`, `Src/plugin.cpp` — root CacheInfo, VERSIONINFO digest/revision authorization, startup/exit clear coordination.
- `Src/Map/MapTransform.h` — MiniMap world-to-screen formula.

For related lifetime and publication boundaries, see [Texture Publication](../ModLoader/Texture-Publication.md), [Loading and World Transitions](../Lifecycle/Loading-and-World-Transitions.md), and [Rendering and Asset Capture](Rendering-and-Asset-Capture.md).
