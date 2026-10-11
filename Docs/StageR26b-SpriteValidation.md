# R2.6b — Generic resident representation validation

Implemented 2026-10-11. No deployment or game execution was performed.

## Baseline and preserved work

Branch `develop`, HEAD and local `origin/develop` were both
`4929b7f61dd4c5dfbdf4dddf5c9c1d8552f4696a`. The initial working tree contained
the expected R2.6a changes: `plugin.cpp`, `MiniMap.props`, `MiniMap.vcxproj`,
the three SpriteDiscovery files, its test and documentation. These were preserved.
There were no unrelated changes. The installed R2.6a DLL and the initial local DLL
had identical SHA256 `F73A085C9BF79EAE9862E3203F8DA0C4CD57CD60DE808141360B61C64004E1B5`.
The installed SpriteDiscovery.json contained the reported complete generation-4
snapshot: 39 instances, seven groups, no sprites generated.

R2.6b changes `BuildingRepresentationDiagnostic.cpp/.h`, the existing uncommitted
`SpriteDiscovery.cpp` and `MiniMap.props`. It adds `GenericRepresentationAudit.inl`,
`SpriteValidationCore.h`, `SpriteValidationTests.cpp`,
`VerifySpriteValidationAbi.py` and this document. R2.6a's existing project/plugin
integration is retained; no new translation unit is required.

## Scheduling and identity

The existing SpriteDiscovery owner owns one validation scheduler. It consumes
only complete published discovery registries and coherent collector snapshots,
outside UI drawing. Candidate order is the published registry order. Every current
key gets one audit opportunity per world generation; there is no three-candidate
limit and failures advance to the next key. Registry counts remain observational.

Keys retain R2.6a's provisional generation/definition/FName/copied visual-state and
tint identity. Results are not persistent asset keys. Two representative copied
records per key are retained; the second is attempted only when the first native
Mass entity is stale. An immutable CPU snapshot protects incremental cursors from
publisher replacement. No native cursor or UObject survives a synchronous audit.
Removed keys are marked non-current in bounded same-generation history; additions
get an opportunity. Generation changes clear everything. Same-key disappearance
and return does not cause automatic retries.

Limits: 256 queued/history keys, 128 copied records per preparation tick,
65,536 record ceiling, two representatives, one candidate audit per tick.
An oversized source gets a reported bounded prefix and is released after that
opportunity, rather than being held across ticks. Incomplete discovery does not
start native audits. Queue exhaustion is explicitly deferred/truncated.

## Native contracts and authored audit

No new native callable, function fingerprint, weak-pointer resolution, field
offset or name conversion is introduced. The implementation reuses the matching
build guard, exact native class lookup, game-thread check, strong retain/release,
Mass entity index/serial revalidation, placement identity and cached configuration
route already used by R2.4b/R2.5a.

`VerifySpriteValidationAbi.py` reruns the representation ABI verifier against the
installed CL-127004 executable/PDB (GUID
`41cbaea9-7ac3-99da-b947-a1db9c1021ed`, age 1). Verified reads include UObject
index/class/name/outer, object-array identity, class inheritance/CDO slot,
Blueprint/SCS arrays and templates, stored materials and scene transforms.
Existing CDO/root contracts are `UClass +0x110`, `AActor +0x1B8`, and, only after
validating CrBuildingActorBase inheritance, main components `+0x2D8/+0x2E0`.
The existing skeletal getter is uniquely resolved at offline RVA `0x048F6190`;
the targeted object-array anchor at `0x017B2740` resolves GUObjectArray
`0x0E359230`. RVAs are verifier evidence, never runtime addresses.

Each candidate gets a fresh context with the existing shared 32,768 identity-work,
8,192 graph-visit and 4,096 assembly-audit ceilings. Existing configuration depth
16, trait array cap 256, Mass mesh cap 64 and component traversal caps remain.
Selected-class assembly collection is capped at 32 SCS nodes, 16 parent levels,
16 static and 16 skeletal instances, and 32 material slots per array.

High-detail, low-detail and Mass alternatives remain distinct. Selected-class SCS
inputs are audited using the existing positive-scale UE QST composition and native
parent matching/ancestry checks. Repeated mesh instances and per-component
nonnull material overrides remain separate. Existing root transforms define local
reference space; no placement transform or Mass transform composition is added.
Visibility is authored metadata only. Inherited templates/override records and
child contributions are observed but prevent unconditional eligibility when their
effective assembly is unresolved. All audit references retire before JSON I/O.

Null cached configuration means `cached_config_null`, not absent authored config.
Authored soft config and placement ActorClass remain explicitly unexamined because
their safe resident resolution contract is unavailable. Native default component
enumeration, effective inherited resolver, unsafe material getters, construction
scripts and asset loading remain excluded.

## Classification and semantic identities

Eligibility means bounded selected-class authored static inputs support **future
validation**, not safe capture, a complete building, live fidelity or a sprite.
Every result keeps `verified_safe_to_capture=false` and capture disabled.

* Eligible: resident ordinary static templates, root ancestry, transforms and
  stored material mapping verified, no relevant unresolved contribution/truncation.
* Conditional: missing root/material/transform, inherited or child ambiguity,
  other skinned contribution, or incomplete traversal/output. A derived static
  component is conservatively conditional: its base mesh does not establish
  spline/instance/procedural geometry semantics.
* Unsupported: copied spline representation, Mass-only route with unverified
  renderer-equivalent transforms, or no selected SCS static meshes.
* No resident representation: validated definition but no supported resident route.
* Invalid/stale: identity/generation/facility failure or candidate exception.

Runtime display names remain null and ownership/spawn classification remains
`unknown`; no integer-to-name lookup table is installed. Offline matching PDB
enum type `0x65AF2C` independently confirms:

| ID | PDB ECrBuildingID label | Observed instances |
|---:|---|---:|
| 220 | Hub | 1 |
| 317 | PersonalStorage_NonDeconstructible | 2 |
| 341 | StartingItemPrinter | 1 |
| 4 | Airlock_StartingHUB | 1 |
| 18 | CloningBed_Lander | 4 |
| 238 | ConnectingPlatformStability | 28 |
| 336 | Smelter | 2 |

**Confirmed:** these enum labels and snapshot counts. **Strongly supported:** ID
238 is associated with the user's foundation tiles (3×4 + 4×4 = 28), rather than
meteors. **Unresolved:** per-instance ownership, spawn origin and whether each Mass
entity corresponds to one independently placeable logical object. The collector
deduplicates entity keys; grouping counts do not establish gameplay semantics.

## Output

Plugin cache resolver → `MiniMap/Cache/Buildings/Diagnostics/SpriteValidation.json`.
The existing `SpriteDiscovery.json` remains an R2.6a document.

Schema 1 contains stage/build/generation/publication identity, discovered/processed
and five outcome counts, remaining/deferred work, completion and independent queue,
representative-scan/output truncation, native-work counters and candidate records.
Each candidate includes its session-local key and current flag, instance count,
opaque definition identity, route/high/low/Mass/config status, selected class,
component/mesh/material counts, root/transform/material verification, unresolved
feature flags and a bounded authored audit (instances, transforms, material inputs,
native candidates and alternatives). Names/ownership carry explicit unavailable
provenance. `new_sprites_generated=0`, `capture_enabled=false`.

Output is capped at 256 KiB, with 16 KiB authored detail per candidate; only whole
JSON records are admitted. Missing records/details are explicitly truncated.
Atomic temporary-file publication uses the existing resolver/MoveFileEx convention.
Intermediate changes publish at most once per two seconds; completion publishes
immediately. Later ten-second discovery publications can reconcile counts without
repeating native audits. No per-frame failure retry or log spam is introduced.

## Verification and readiness

Normal `Client Release|x64` and `Server Release|x64` builds passed. Client output
remains `bin/x64/Client Release/plugins/MiniMap.dll` with its matching normal PDB.
Eleven CPU suites passed: pixel readback, RGBA16F, WIC PNG, metadata, selection,
assembly recipe, retirement, static assembly capture, building geometry, discovery
and validation. Tests include all seven synthetic groups, failure isolation,
variable/static-plus-skeletal counts, material precedence, missing roots and
transforms, subclass/inherited/child/truncation guards, no duplicate audits,
snapshot replacement, generation changes, representative/queue bounds, escaping
and valid partial output. Synthetic outcomes are not runtime findings.

Representation, retirement and static-capture ABI/fingerprint checks passed.
Independent Python JSON parsing and PNG decoding passed; the synthetic seven-group
validation JSON was 12,584 bytes. COFF inspection found no unwanted
CRT/static initializers in the five relevant units in either configuration.
Server artifacts exclude discovery/validation/capture behavior. `git diff --check`
passed. Capture/readback/retirement/PNG/alpha/assembly-audit implementations and
collector/UI/map files were checked byte-equivalent to HEAD; only guarded metadata
collection in the shared representation diagnostic was extended.

**Conditional readiness:** all seven groups can receive one audit opportunity in
one separately authorized session, at most one per tick after preparation. Actual
resident representations and eligibility remain unobserved until that test. No
new sprites or render objects are produced. Candidate-specific unsupported paths
remain diagnostic results, and native failure remains closed. No deployment,
game execution, commit or push was performed.

ModLoader shutdown/DLL-unload behavior is a known, user-deferred issue and was
intentionally excluded from this stage.
