# R2.4 bounded representation metadata snapshot

Implemented 2026-10-10. Offline validation passed; no deployment or game execution performed. This diagnostic inventories resident metadata and does not establish a complete rendered building.

## Discovery and lifetime

The existing collector must publish a complete, nonempty snapshot. On the game thread, the diagnostic considers at most 64 records, prefers the Smelter building ID if present in that window, otherwise selects the first definition-valid record. This preference contains no Blueprint, component, mesh or material path. Only one building is attempted per process; unavailable selected metadata does not trigger asset loading or discovery fallbacks.

The synchronous collector accessor revalidates world/generation and Mass entity index/serial, then follows the existing building parameters fragment to placement data. The diagnostic independently validates placement identity, copied definition index/FName/building ID, entity configuration, configuration parents and visualization traits. High-detail and low-detail actor class candidates and Mass descriptors remain separate. The first resident validated high-detail class is preferred, then low-detail; otherwise validated Mass mesh metadata can be selected. Candidate equivalence is never assumed.

Roots use the existing R2 native retain/release facilities. Object references live only inside the synchronous snapshot context and retire before filesystem export. No persistent component pointers, actor creation, CDO creation, registration, construction scripts, forced loading or mutation are introduced.

## Enumerated and excluded sources

| Source | Treatment |
|---|---|
| Blueprint component templates, SCS roots/all nodes/children | Bounded resident traversal, expected type and object identity validation |
| Inherited override handler records | Raw bounded records and key/template relationships; effective override precedence not evaluated |
| Existing CDO pointer | Identity/type/class-default-object flag checked; never created |
| Native default components | **not_attempted**: sparse-set traversal bound not yet independently verified |
| K2_GetComponentsByClass / OwnedComponents | Excluded completely |
| GetActualComponentTemplate | Not called: internal enumeration lacks a diagnostic bound; raw overrides stay distinct |
| Child actor class/template | Resident relationships and bounded class-template recursion; no live child traversal or spawning |
| Static mesh assignment | Verified direct field and authored static material slots |
| Skeletal mesh assignment | Optional verified fixed native getter after validating both input objects and its class-chain array |
| Other skinned component assignment | Raw verified asset inputs; effective getter not attempted |
| Materials | Authored mesh defaults and component overrides with slot indexes; virtual effective-material enumeration not attempted |
| Scene metadata | Relative location/rotation/scale, parent, raw socket FName, absolute flags, visibility/hidden/registration bits |
| Mass metadata | Ordered mesh references, authored material overrides, significance ranges, raw descriptor/local/TransformOffsets inputs |

No transforms are composed. No animation, reference pose, construction-script changes, live-instance changes, LOD/residency or Nanite state are evaluated. Records from inherited, child and representation sources are not declared additive geometry.

Readable native name conversion is not newly verified and is not invoked. Objects use snapshot-local `oN` IDs, class IDs and raw FName comparison index/number. Readable names explicitly remain unavailable. Live addresses are not serialized.

## Installed ABI evidence

Evidence is against the installed `StarRuptureGameSteam-Win64-Shipping.exe` and matching PDB under the Steam game's `StarRupture/Binaries/Win64` directory. RSDS GUID `41cbaea9-7ac3-99da-b947-a1db9c1021ed`, age 1. Runtime direct reads require this exact identity. UE source is supporting context only, not a substitute for installed evidence.

`Tests/VerifyRepresentationAbi.py` checks the PE/PDB match, PDB types/field offsets, array allocator/header layouts, component flags, object flags, object-pointer storage and the fixed getter's member-function return contract. Its output is in `C:\Modding\StarRupture\MiniMapR1-Analysis\R24-Offline\NativeAbi.txt`.

| Boundary | Installed evidence |
|---|---|
| Blueprint/SCS | ComponentTemplates +220, SCS +268, override handler +270; RootNodes +28, AllNodes +38; node template +30, children +A0; handler records +28, stride 78 |
| UObject identity | Object index +C, class +10; GU chunked array +10, counts +20/+24/+28/+2C; object item stride 18 and exact slot-pointer equality; lifecycle flags checked |
| Class reads | Existing CDO +110; UStruct base-chain +30, superclass +40; bounded class-chain inputs verified before native getter |
| Scene/mesh | Relative fields +140/+158/+170, attachment +C8/+D0, flags +1A0/+1A1; static assignment +588; overrides +530; static materials +160, skeletal materials +1A0 |
| Child template | Class +250, template +260; live child +258 is not traversed |
| Mass | Trait descriptor +30, high/low classes +D0/+D8; descriptor meshes +8 and TransformOffsets +80; mesh stride A0, transform stride 60 |
| Object-array anchor | Unique masked installed signature at RVA 017B2740, unwind entry verified; RIP reference resolves GUObjectArray RVA 0E359230; function is not invoked |
| Skeletal getter | Unique installed signature/unwind entry at RVA 048F6190; native x64 member, RCX this, no arguments, borrowed USkeletalMesh* in RAX; no return-array allocator or sret |

RVAs are offline references, never runtime hardcoded addresses. Runtime resolution uses existing MiniMap native signature facilities. No new reflected invocation or generated SDK function call is introduced.

## Bounds and output

Every traversed array validates nonnegative Num/Max, Num <= Max, capacity multiplication and address arithmetic, storage alignment/readability and bounded inspected storage. Every UObject element separately validates indexed identity/lifetime and expected class. Pointer readability alone never establishes object identity. Graphs have cycle/repeat handling and a shared work budget.

Limits: 128 component records, 256 unique SCS nodes and per-array SCS elements, 32 material slots per array, 4 child-template depth, 32 graph/class depth, 16 configuration parents, 256 traits per array, 64 Mass meshes/offsets per array, 8,192 total visits, 1 MiB output. JSON sections also have byte limits: sources/Mass 128 KiB each, components 512 KiB, candidates 32 KiB, relationships 64 KiB. Truncation is explicit; final overflow produces valid partial JSON rather than incomplete text.

Schema version 1 root fields include build/stage, building ID and copied transform input, selection/provenance/status, candidates, sources, components, mass_representations, relationships, limits and truncation_summary. `complete_visual_building`, `transforms_composed` and `component_records_are_additive_geometry` are false. Source entries include source, owner_id, status, reason and structured details. Statuses distinguish present, absent, invalid, unavailable/not resident, not_attempted, unsupported, cycles/repeats and truncated. Component records carry local IDs/provenance, mesh inputs, material slots/overrides, relative/attachment/visibility metadata and limitations.

The existing plugin-directory resolver produces:

`<MiniMap DLL directory>\MiniMap\Cache\Buildings\Diagnostics\Building_<id>_Representation.json`

For selected Smelter ID 336: `Building_336_Representation.json`. In the normal build directory, this is `C:\Modding\StarRupture\MiniMap\Solution\bin\x64\Client Release\plugins\MiniMap\Cache\Buildings\Diagnostics\Building_336_Representation.json`. Runtime deployment location determines the actual cache root. A temporary file and atomic publication are used once; ordinary ticks do not repeatedly export.

## Files changed by this stage

- New: `Src/Experiments/BuildingRepresentationDiagnostic.cpp`, `.h`, `RepresentationMetadataCore.h`, `RepresentationMetadataPatterns.h`.
- Modified: `Src/Map/BuildingCollector.cpp`, `.h` (synchronous diagnostic accessor), `Src/Map/TerrainCache.cpp`, `.h` (existing cache resolver exposure), `Src/plugin.cpp` (guarded lifecycle), `Src/MiniMap.vcxproj` (new source/header entries and inspectable COFF object).
- New: `Tests/RepresentationMetadataTests.cpp`, `Tests/VerifyRepresentationAbi.py`, this report.

Preexisting dirty capture files, props and documentation deletions were preserved. SHA256 checks of all 12 baseline capture/readback/configuration/conversion/export files matched their pre-implementation hashes.

## Validation and readiness

- Standard VS2022 `msbuild MiniMap.sln /m /p:Configuration="Client Release" /p:Platform=x64`: passed. Normal `bin/x64/Client Release/plugins/MiniMap.dll` and matching PDB produced; no special property override or DLL copies.
- Existing R1 CPU, RGBA16F decoder and PNG conversion/WIC export tests: passed.
- Metadata synthetic tests: passed for exact shared object identity gate, malformed arrays, overflow/alignment, graph cycles/repeats/depth/budget, byte truncation, escaping and nonfinite numbers. Independent Python JSON parse: passed.
- Installed PE/PDB ABI checks: passed.
- New diagnostic COFF: no `.CRT` sections or dynamic initializer symbols found.
- `git diff --check`: passed (existing line-ending warnings only).
- Server separation: source/header/accessor/lifecycle guarded by client and R2 macros; server configurations do not define the R2 macro. Server build was not run under the latest Client Release-only workflow instruction.

Ready for a separately authorized controlled runtime diagnostic. Runtime residency, actual selected candidate, exported metadata and any remaining visual gaps have not been observed. Missing native defaults, effective inherited selection/material state, construction-script changes and live state prevent a completeness claim. Existing successful R2.3.1 capture/export behavior is byte-preserved but was not rerun in-game.
