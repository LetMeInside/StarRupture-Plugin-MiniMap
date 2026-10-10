# R2.4b bounded representation-eligible selection

## Baseline and result

Initial branch: `develop`. Initial HEAD: `e0e5d34433279d65e5b7a72bcb6f2e205198756b`
(`Add native Smelter capture and bounded representation diagnostics`). The initial
working tree was clean. HEAD was preserved.

Confirmed offline: the diagnostic now checks resident representation eligibility
before selecting a building, prefers eligible Smelter definitions, and separates
selection coverage from component-traversal status. Actual Smelter residency and
metadata remain unobserved until a separately authorized runtime test.

## Files changed

- `Src/Experiments/RepresentationSelectionCore.h`: bounded CPU selection policy,
  shared native-work accounting, selection JSON and bounded document fallback.
- `Src/Experiments/BuildingRepresentationDiagnostic.cpp`: eligibility probes,
  candidate provenance, schema 2 reporting, and one selected full traversal.
- `Src/Map/BuildingCollector.h` and `Src/Map/BuildingCollector.cpp`: diagnostic-only
  accessor result enum distinguishing lifecycle, entity and parameter failures.
  Production collection behavior is unchanged.
- `Src/MiniMap.vcxproj`: register the new header; normal configurations preserved.
- `Tests/RepresentationSelectionTests.cpp`: synthetic selection and JSON tests.
- `Docs/StageR24b-RepresentationSelection.md`: this report.

## Access gate and evidence

The gate passed for the existing resident cached-configuration route only. No new
native function, generated SDK call, signature or weak-reference resolver was
introduced. `Tests/VerifyRepresentationAbi.py` passed against the installed client
executable and matching PDB, GUID `41cbaea9-7ac3-99da-b947-a1db9c1021ed`, age 1.
The verification transcript is
`C:\Modding\StarRupture\MiniMapR1-Analysis\R24-Offline\R24b-NativeAbi.txt`.

Installed PDB evidence includes UObject identity fields (`InternalIndex +0x0C`,
class `+0x10`, FName `+0x18`), FUObjectItem stride `0x18`, placement EntityType
`+0x120`, BuildingID `+0x170`, and EntityType cached configuration `+0x30`.
The script verifies configuration parent/trait layouts, visualization actor-class
and mesh-descriptor fields, ordinary array headers, element layouts and component
metadata contracts. Array headers are 16 bytes: allocation pointer at 0, signed
Num at 8, signed Max at 12. PE signatures remain unique and unwind-validated;
the object-array anchor resolves to installed GUObjectArray RVA `0x0E359230`.
These RVAs are evidence, not new hardcoded runtime addresses.

Existing collector APIs revalidate current world generation, Mass index/serial
and parameter availability. The synchronous visitor then checks placement
identity/type/extent, acquires existing strong retention, and compares copied
object index, FName comparison index/number and building ID before field access.
Resident objects use the established targeted object-array identity contract,
class validation and lifetime checks. Correct alignment/readability alone is
never treated as UObject validation. Existing native retention and game-thread
contracts are reused; no additional internally enumerating native API is called.

The optional authored soft `ActorClass` and `EntityConfig` read/resolution
contracts were not independently established. Both are explicitly `unavailable`.
A null transient cache is recorded as `cached_config_null`, with authored state
unknown; it is not reported as missing authored configuration. This preserves a
safe route through populated, validated resident caches without inventing a
fallback. Placement actor-class preference is modeled in the policy tests but
is deliberately unsupported by the runtime implementation.

## Selection and limits

The collector publishes an immutable shared CPU snapshot. Selection reads that
snapshot without widening collection radius, changing ordering or traversing
native objects for every record. Existing query handle ceilings are 1,000,000
per query; the off-grid/spline cache union can reach 2,000,000 and the local
indexed input 1,000,000, yielding a derived pre-deduplication envelope of
3,000,000. There is no separate final published-vector cap. These are bounds,
not expected save sizes; the previous runtime snapshot contained 348 records.

The CPU pass examines at most 4,096 records and reports total versus examined
counts and whether coverage is complete. It counts definition-valid records,
distinct copied definition identities and Smelter presence. Beyond the cap,
Smelter absence remains unknown. Absence from a complete snapshot means absence
from the current spatially filtered snapshot, not from the world/save.

Definition keys contain object index and FName comparison index/number, never
only building ID or raw pointer. The pass retains at most 16 candidate
definitions and two deterministic instance representatives per definition.
Ordering uses Smelter preference, building ID, definition identity, entity serial
and index. Only these bounded candidate containers are sorted. The distinct-key
set is bounded by the 4,096 record visits. Temporary insertion capacity is at
most 17 definitions and three instances before pruning.

At most eight native instance probes are made. A stale instance permits one
alternate instance for that definition within the same overall probe budget.
Other failures do not trigger an instance retry loop. Probes never traverse
component templates, materials or transforms.

An eligibility probe follows:

1. Revalidate world, entity and copied placement identity.
2. Read and validate the resident cached entity configuration.
3. Traverse at most 16 configuration parents, detecting repeats/cycles.
4. Inspect bounded trait arrays (256 entries per array) and validated visual
   trait objects.
5. Validate resident high/low actor classes, or descriptor mesh assignments
   (64 descriptors per array) through the existing configuration/trait route.

Eligibility requires a retained validated actor-derived class or a retained
validated static mesh assignment. A configuration alone is insufficient. Actor
metadata outranks Mass-only metadata, and any eligible Smelter outranks another
building. The runtime stops when the best supported preference has been
established, preserving work for the full snapshot. It never claims unprobed
fallback candidates were tested.

The selection and full traversal share 32,768 validation/gate operations and
8,192 graph/array element visits. Each probe entry consumes a validation/gate
operation, in addition to object checks. Existing full traversal caps remain:
128 component records, 256 SCS nodes, 32 material slots per array, child-template
depth 4, graph/class depth 32. Exhaustion produces explicit partial statuses.

## Snapshot, ownership and JSON

One process-wide attempt starts only on the verified game thread after a
complete, nonempty collector snapshot is published. At most one eligible
candidate receives the full existing metadata traversal, with another entity
and definition revalidation before that traversal. A failure at that point does
not start a second full snapshot. Existing strong roots stay local to the
synchronous context and retire before filesystem export. Serialization retains
only CPU-owned strings and values. No pointer is exported as a reusable identity.

Schema version 2 retains the existing detailed root-level `sources`,
`candidates`, `components`, `mass_representations` and `relationships` records.
It adds:

- `selection`: snapshot/scan counts, distinct definitions, Smelter coverage,
  probe counts, eligible/rejected counts, selected building/route and outcome.
- `candidate_attempts`: copied identities, tested Mass/placement states, cached
  configuration state, unavailable authored-reference states, trait/mesh counts,
  high/low class availability, eligibility and specific rejection reasons.
- `selection_truncation`: independent CPU, definition, probe, native traversal,
  work and attempt-output limits.
- `representation`: whether a full traversal was attempted and its status;
  active renderer tier remains unknown and completeness remains false.
- `representation_truncation` and `native_work`: independent component/graph/
  output exhaustion and selection versus full traversal accounting.

Selection-window truncation alone does not turn the full component traversal's
status into `truncated`. Unknown, absent, invalid, unsupported and unexamined
states remain distinct. No representation is claimed complete; transforms are
recorded as inputs, never composed, and high/low/Mass sources remain separate.

Output is bounded to 1 MiB. Existing per-section JSON buffers retain their
ceilings, with candidate attempts capped at 64 KiB. Oversized documents become
a small valid schema-2 truncation document rather than incomplete JSON. Existing
cache resolution and temporary-file atomic publication are reused.

Expected paths, relative to the normal plugin DLL directory:

- Eligible Smelter: `MiniMap\Cache\Buildings\Diagnostics\Building_336_Representation.json`.
- Other selected building: `MiniMap\Cache\Buildings\Diagnostics\Building_<id>_Representation.json`.
- No eligible candidate: `MiniMap\Cache\Buildings\Diagnostics\Representation_Selection.json`.

## Verification and remaining limitations

Confirmed offline results:

- Normal `Client Release | x64` build passed; normal DLL and matching PDB remain
  under `bin\x64\Client Release\plugins`.
- Normal `Server Release | x64` build passed.
- Existing R1 pixel, RGBA16F decoder, WIC PNG conversion/export and representation
  metadata tests passed.
- New tests passed: Smelter beyond record 64, deterministic ordering, definition
  deduplication, bounded stale alternate, cache-null/authored-unknown reporting,
  actor/Mass route policy, no eligible candidate, CPU/probe/work limits,
  preference early-stop, separate truncation and valid no-selection JSON.
- Installed executable/PDB ABI verification passed.
- Independent Python JSON parser accepted synthetic selection and metadata
  documents, including output-limit fallback and escaping cases.
- Client COFF inspection found no unwanted CRT/dynamic initializer entries;
  Server COFF contained no client diagnostic implementation symbols.
- `git diff --check` passed. Protected SceneCapture/readback/decoder/PNG source
  files and existing feature configuration were unchanged from HEAD.

Native default component enumeration, unbounded inherited-template helper calls,
CDO creation, actor spawning, Blueprint execution, asset loading, soft-reference
resolution and streaming-sensitive render-data traversal remain excluded.
Supported resident SCS/inherited/child-template relationships remain conditional
on validation and existing traversal bounds. Eligibility establishes access to
some representation metadata, not complete visual assembly, active distance
tier, construction-script changes or live-instance material state.

The implementation is ready for a separately authorized controlled runtime test.
No deployment or game execution was performed; no commit or push was made.
