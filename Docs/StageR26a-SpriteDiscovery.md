# R2.6a collector-integrated diagnostic sprite registry

## Baseline and outcome

Initial checkout was clean on `develop`, HEAD and local `origin/develop`
`4929b7f61dd4c5dfbdf4dddf5c9c1d8552f4696a`. The required R2.5b checkpoint
`b143d1dd1a0c20ba822849d618f92566a1594b52` is an ancestor. Existing R2.5b
implementation, tests and report were present. No Git mutations, deployment or
game execution occurred.

This stage adds discovery only. No new sprites, rendering objects, native
representation probes, GPU work or minimap drawing changes are introduced.
The normal Client Release enables the diagnostic; Server compiles it out.

## Files

Modified:

- `Src/plugin.cpp`: register/unregister the diagnostic consumer.
- `Src/MiniMap.props`: normal Client Release feature property, false otherwise.
- `Src/MiniMap.vcxproj`: compile the new consumer as inspectable ordinary COFF.

Added:

- `Src/Experiments/SpriteDiscovery.h`
- `Src/Experiments/SpriteDiscovery.cpp`
- `Src/Experiments/SpriteDiscoveryCore.h`
- `Tests/SpriteDiscoveryTests.cpp`
- This report.

## Collector integration and source contract

The consumer registers one existing ModLoader engine-tick callback; it is not
called by ImGui and does not change `BuildingCollector`. Existing hooks run
plugin callbacks after the original engine tick:
`StarRupture-ModLoader-Core/hooks/game/engine_tick/engine_tick.cpp`.
It uses only `BuildingCollector::GetSnapshot()` and copied C++ data.
No newly resolved game function, generated SDK wrapper or UObject read is used.

Relevant existing source:

- `Src/Map/BuildingCollector.h`: `Record`, `DefinitionGeometry`, `Snapshot`.
- `BuildingCollector.cpp:634`: publication copies spatially admitted records,
  sorts by entity index/serial, deduplicates instances, then publishes an
  immutable `shared_ptr<const Snapshot>` under a mutex.
- `BuildingCollector.cpp:401`: reset clears publication and increments generation.
- `BuildingCollector.cpp:677`: service; local publication cadence is 200 ms.
- `BuildingCollector.cpp:836`: mutex-protected snapshot acquisition.
- `BuildingCollector.cpp:355`: copied definition index and FName pair.
- `BuildingCollector.cpp:225`: copied visual-class index is populated for a
  validated spline-width proxy, not a generic selected representation class.
- `BuildingCollector.cpp:370` onward: copied visual customization tint.

Collector queries validate at most one million handles per individual source:
grid, off-grid and indexed splines. The latter two feed the reconciled cache.
Publication has no separate small record cap; the source bounds imply up to
three million records before publication deduplication/filtering. This is not
a promise of a small or visually complete snapshot. R2.6a neither changes those
queries nor traverses unrelated entities.

## Registry owner, identity and confidence

There is one explicit `Session<Snapshot>` owner, constructed during diagnostic
initialization. It contains a published registry and a staging buffer, each
with at most 256 entries. No native object is retained. The held publication
contains only existing CPU-owned records/shared copied geometry.

A session-local key contains:

- Collector world generation.
- Building ID and copied definition UObject index plus FName index/number.
- Copied spline-proxy class and mesh identity fields, when present.
- Exact finite copied RGBA customization bits; signed zero is canonicalized.
- Representation-tier tag, currently `Unknown` because this consumer does not
  probe representations.
- Whether the instance uses spline geometry.

The entity index/persistent ID is not a sprite key. Raw UObject addresses,
display names and cross-session identities are not stored or exported. Placement
position/orientation is not a visual recipe identity. Different definitions,
observed tints, copied visual identities or future tier tags remain separate.

These are **provisional definition/metadata groups**, not proven equivalent
representations. Unobserved materials, configuration, animation and live variants
can require later splitting. Rebuilding discards old grouping; there is no
irreversible merge or disk-cache identity. The original collector's definition
validation is recorded as publication-time evidence, never current residency.

## Reconciliation and lifecycle

Each scan starts fresh counts against one immutable publication. Repeated
service calls advance its cursor without rereading already-counted records.
Same-generation replacement of the collector's current publication cannot
change that cursor's storage. Once finished, the staging result replaces the
published registry atomically within the game-thread owner; its source reference
is released. Removals drop from the next observed registry; they are not evidence
that the representation asset itself has become invalid or nonresident.

A new scan starts no sooner than ten seconds after completion. It reacquires
the latest snapshot, including incomplete snapshots and empty publications.
Counts always describe that publication, not an instantaneous world census.

A changed generation aborts the old cursor and clears identities/counts before
new processing. Before-EndPlay for the currently published gameplay world
(comparison only, no world dereference) immediately clears the owner and blocks the
retired generation; the next service publishes an empty `Invalidated` diagnostic.
The old generation cannot repopulate it while its snapshot remains visible.
Missing world/publication also clears retained CPU state. No gameplay-world or
UObject pointer is cached or dereferenced.

## Bounds and performance

- 256 entries per registry; two fixed buffers in one owner, not two registries.
- At most 128 examined records and 16 new entries per tick.
- At most 256 key comparisons per examined valid record; no unbounded sort,
  instance-ID history or unique-definition set.
- At most 65,536 records across an ordinary incremental scan. Thus at most
  528 service ticks suffice for one accepted publication under these budgets.
- Publications larger than 65,536 records are **not held across ticks**. Only
  one bounded prefix tick is examined, the rest is explicitly deferred and the
  result is truncated. This avoids retaining a huge old factory snapshot.
- One shared CPU publication, at most 65,536 records, may otherwise outlive
  collector replacement. Its existing copied geometry graphs remain shared;
  the registry does not duplicate those graphs. Their storage is producer-owned,
  so registry entry limits are not a claim of a fixed total source-graph byte size.
- JSON is capped at 256 KiB. At most 256 candidate records are serialized once
  per completed scan, normally at least ten seconds apart. Lifecycle invalidation
  can publish one additional small document.

Serialization, releasing CPU snapshot storage and atomic filesystem publication
are bounded diagnostic work on the service thread. There is no hard real-time
latency guarantee for allocation/destruction or disk I/O; no synchronous native,
render-thread or GPU wait is introduced.

## States and eligibility

The design names `Discovered`, `NeedsValidation`, `EligibleCandidate`,
`Unsupported`, `Deferred`, and `Invalidated`.

Valid ordinary definition groups transition immediately from discovery to
`NeedsValidation`. They are eligible for a **future bounded validation probe**,
not capture. No native actor, configuration, high/low tier, Mass descriptor,
material or static-template audit occurs here. Thus `resident_metadata_found`
is null with `not_examined`, and `verified_safe_to_capture` is always false.
`EligibleCandidate` is reserved for later verified metadata, not synthesized
from an integer class index.

Observed spline instances are `Unsupported` for the proposed shared rigid
static-template recipe: their instance-specific curve needs a separate future
capability. This does not claim that the building has no visual representation
or could never be rendered. Invalid definition metadata, generation mismatches
or nonfinite tint are counted separately and do not create misleading keys.
Unexamined records are reported as deferred coverage; no invented candidate
entries are created for them. Lifecycle invalidation clears entries rather than
preserving an unbounded history of `Invalidated` objects.

## JSON artifact

Resolved through the existing plugin-cache directory helper:

    <MiniMap.dll directory>\MiniMap\Cache\Buildings\Diagnostics\SpriteDiscovery.json

Schema version 1, stage `R2.6a`, build `CL-127004`. Header fields record world
generation, source steady-clock publication time, source/registry completeness,
scan progress, candidate and represented-definition counts, represented instance
count, invalid/deferred/skipped records, service ticks, configured limits and
separate record/capacity/output truncation flags. `new_sprites_generated` is 0.

Per candidate: session-local key, building/definition/FName identities, copied
class provenance and customization bits, tier availability, provisional
equivalence confidence, observed count, state/reason, copied-geometry presence,
unexamined residency, future-validation eligibility and false capture readiness.
Mesh identity fields are encoded in the key; they are diagnostic integers, not
resolved asset paths or owning pointers.

When capacity is reached, existing retained keys continue accumulating their
correct observed counts; new unmatched records increase skipped coverage. The
reported distinct definition count is for retained groups only, not a census of
skipped definitions. Output truncation omits whole candidate objects while
preserving full registry counts and `candidates_emitted`; it never slices JSON.
The fixed header/footer has reserved space. Publication writes a temporary file
and uses the existing `MoveFileExW` atomic replacement convention. Export errors
are bounded warnings and do not trigger a per-frame retry.

## Verification

Normal `Client Release | x64` and `Server Release | x64` builds pass. The Client
DLL/PDB remain in their normal `bin/x64/Client Release/plugins` location.
No custom output name or command-line feature override is needed.

All nine existing standalone suites passed: R1 pixels, RGBA16F, WIC PNG,
representation metadata, selection, assembly recipe, retirement, static assembly
capture and building geometry. The new sprite-discovery suite passes grouping,
definition/tier/tint separation, signed-zero normalization, provisional state,
stale generations, same-generation publication replacement, additions/removals,
repeated polling, capacity, cursor/new-entry limits, incomplete source metadata,
spline classification, invalid metadata, oversized-source deferral, bounded
publication cadence, invalidation and JSON truncation tests. Tests use CPU
fixtures only, not a game process.

Independent Python JSON parsing passed full and truncated registry artifacts;
independent PNG decoding passed existing WIC outputs. Installed representation,
capture-retirement and static-assembly ABI verifiers all pass against matching
CL-127004 executable/PDB GUID `41cbaea9-7ac3-99da-b947-a1db9c1021ed`, age 1.
No new installed layout or callable ABI is needed by this stage.

COFF inspection of the new translation unit and four existing diagnostic/native
units in both configurations found no CRT construction section, defined dynamic
initializer or UnifiedError dependency. Server has no new registry implementation
or output-path string. New Client object has no SDK naming or capture dependency.
`git diff --check` passes. Existing experiment files, collector, inventory,
building visuals and minimap UI are byte-equivalent to HEAD. The only changes
to existing source are the feature/property, project item and consumer lifecycle
wiring listed above.

Offline QA files remain outside the repository under
`C:\Modding\StarRupture\MiniMapR1-Analysis\R25b-Offline` with `R26a` logs.

## Runtime readiness and limitations

Confirmed: implementation, builds and offline verification. Conditional: actual
runtime publication, observed candidate counts and native world-lifecycle service
ordering have not been tested by this stage. The normal Client build is ready
for a separately authorized controlled R2.6a diagnostic test.

Unknown representation residency is deliberate, not an access failure or proof
of absence. R2.6b must still obtain bounded resident representation metadata and
validate generic static recipes before capture. No automatic sprite queue,
persistent cache, high/low-tier selection, material equivalence, four-direction
rendering or production draw path is provided here. Existing enabled R2
experiments remain independent and unchanged; registry publication schedules
none of their work.

ModLoader shutdown/DLL-unload behavior is a known, user-deferred issue and was
intentionally excluded from this stage.
