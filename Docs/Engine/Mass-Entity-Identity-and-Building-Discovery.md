# Mass Entity Identity and Authoritative Building Discovery

## Scope and version basis

This reference consolidates historical Mass identity, building-definition, query, fragment, grid and save-ID research with a bounded read-only reconciliation of current MiniMap source. It is descriptive. It does not specify a replacement collector or validate the present executable at runtime.

Evidence labels follow the other restored MiniMap references: **CONFIRMED** means an identifiable primary artifact or current source declaration supports the claim; **OBSERVED** means a retained runtime observation supports it; **INFERRED** means interpretation beyond the direct evidence; **UNKNOWN** means the inspected material does not establish it; **REJECTED** means evidence contradicts it; **SUPERSEDED IMPLEMENTATION** marks historical MiniMap mechanics that are not current source authority. When an underlying native artifact was not retained, the claim is explicitly report-only.

Matched-client evidence identifies the matched StarRupture client executable as CL-127004, SHA-256 b2b9188700ec64dfaf721f969063f969d5a89f8f0b9dc8b6f7135a914f3e5308, with PDB GUID 41cbaea9-7ac3-99da-b947-a1db9c1021ed, age 1. The reported RVAs apply only to that client image. Persistent-ID findings concern StarRupture 0.3.5 / Hotfix 0.3.5 and matched CL-127004 client/server pairs. The stock Unreal Engine comparison identifies UE 5.6.1, CL 44394996; it explains stock Mass semantics but does not prove private P_Neon source or ABI parity. A historical grid performance observation does not state a game build; its runtime and performance observations are not assigned CL-127004 here.

This document does not repeat general UObject/GC or world-transition contracts. See [Loading and World Transitions](../Lifecycle/Loading-and-World-Transitions.md) and [Rendering and Asset Capture](Rendering-and-Asset-Capture.md) for those separate lifetime boundaries.

## Executive technical summary

| Topic | Established result | Boundary |
|---|---|---|
| Mass handle | **CONFIRMED:** the handle is an 8-byte pair of signed 32-bit Index and SerialNumber. The full pair is the Mass lifetime key in inspected collector paths. | Index alone is not a unique identity; the pair is manager/session scoped, not a persistent save key. |
| Validity | **CONFIRMED:** IsEntityValid checks a known handle against a manager; stock UE source says storage index and serial must match. IsEntityBuilt is a separate Created-state check and expects a valid handle. | Neither check discovers membership or locks storage. Their result is a momentary manager observation. No cross-call or cross-tick lease was found. |
| Building identity | **CONFIRMED for the configured definition:** FCrBuildingParameters is a const-shared Mass fragment whose PlacementData points to the configured placement definition; later matched-client ownership tracing traces manager GC reference collection and matched game reads. | Custom instance payload and configured definition can differ; the report did not prove their universal equality. BuildingToRestore is not a universal current-type field. |
| Query | **CONFIRMED:** current source owns an opaque aligned query tied to a UMassEntitySubsystem manager, requiring the CrMassBuildingTag and optional read-only transform; GetMatchingEntityHandles returns an allocated handle array that is freed. | Current generated SDK declarations do not expose all query internals. The native bindings and layouts are game/version-specific. |
| Fragment data | **CONFIRMED:** regular fragment, const-shared fragment and tag access have different storage/meaning. Current collector copies values synchronously and does not publish Mass pointers. | Stable handle identity does not establish stable fragment addresses. No safe raw-pointer cross-tick contract was found. |
| Grid | **CONFIRMED at report level:** UCrEntityGridSubsystem radius traversal visits spatial cells; the retained interface is synchronous with a borrowed TFunctionRef-style callback. Collector applies strict XY admission separately, while traversal includes all Z and disables entity-radius filtering in the cited historical path. | Grid membership is spatial scope, not proof of built state, Building tag, current definition or authoritative building status. Native callback return meaning, ordering and cell-boundary details remain unresolved from retained primary artifacts. |
| Save identity | **CONFIRMED:** FCrMassPersistentEntityID is a 16-byte save/reference value with uint32 ID at +0 and a transient cached FMassEntityHandle at +8. | Its numeric ID is not Mass Index, Mass SerialNumber, Actor pointer, UObject identity or ECrBuildingID. No relationship to a Mass entity serial was established. |

## Mass entity handles and identity

The current NativeApi declaration represents MiniMap’s FMassEntityHandle as:

| Field | Type | Meaning supported by evidence |
|---|---|---|
| Index | int32 | Manager slot/index used to locate an entity record. It may be reused. |
| SerialNumber | int32 | Serial discriminator checked with the index to distinguish the current slot occupant from a stale handle. |

The pair is 8 bytes. Stock UE 5.6.1 source checks the storage index and matching serial in IsEntityValid. Matched client/server evidence distinguishes this handle from persistent IDs; the persistent-ID layout is described below. **CONFIRMED:** code that uses only Index collapses distinct entity lifetimes. The historical collector key packs both 32-bit fields, with world/manager generation surrounding that key.

| Identity domain | Representation and known relation |
|---|---|
| Mass entity handle | Index + SerialNumber, interpreted by one FMassEntityManager. A valid pair in one manager is not automatically meaningful in another manager or world. |
| UObject identity | Unreal object identity/lifetime follows UObject allocation, serial and GC rules documented in [Loading and World Transitions](../Lifecycle/Loading-and-World-Transitions.md). Not equivalent to a Mass handle. |
| Actor pointer | An optional representation/presentation object associated with an entity by engine systems. The association can be absent or change; it is not the Mass identity. |
| Game building ID | ECrBuildingID / BuildingID value on placement definition data, used as configured building type identity. It is not an entity slot or serial. |
| Persistent save ID | FCrMassPersistentEntityID numeric ID used by save/reference maps. Its CachedHandle is transient and embedded as a cache, not a serialization of the persistent numeric ID. |

**UNKNOWN:** no inspected evidence establishes that a Mass handle survives save/load, manager recreation, world replacement or network remapping. Normal save loading creates/registers entities in a manager and resolves persistent IDs through a separate map. **CONFIRMED:** same index with a new serial is a different entity lifetime; a new world/manager is a separate ownership context even if numeric fields happen to match. Do not use index, handle, pointer or persistent ID as a cross-domain substitute.

## Entity validity and lifetime

Treat these as separate conditions:

| Condition | What it establishes | What it does not establish |
|---|---|---|
| Structurally formed handle | Index and serial fields are present. | That either value exists in a manager. |
| Currently registered / valid | The selected manager currently recognizes the index/serial pair. | Built/Created state, tag membership, required fragment presence, or an access lock. |
| Built | The manager reports the entity has reached its Created state. | That every requested fragment exists or that the entity is a building. |
| Required fragment present | A fragment lookup or query requirement supplies that fragment at this observation. | Pointer stability after archetype/chunk movement, mutation or entity removal. |
| Safe to access now | Requires an appropriate manager/world context, thread/phase, validity and state checks, then a synchronous borrow/copy discipline. | Any stronger lifetime than the actual engine read scope provides. No independent Mass lock was identified in MiniMap’s exposed contract. |

Stock UE 5.6.1 source distinguishes IsEntityValid from IsEntityBuilt: the latter checks Created state and expects a valid handle. The matched-client report describes the target IsEntityBuilt body as calling IsEntityValid internally, but the separate valid-before-built check is retained because the built call asserts on invalid input. These predicates are not interchangeable. Query membership also does not substitute for checking a handle at later use.

Current MiniMap BuildingCollector checks validity and built state before initial fragment reads, rechecks after definition preparation, and captures world, manager and generation around membership reconciliation. It validates that context before removing missing entities. These are current-source behaviors, not a concurrency lock. The collector acquires data on its admitted game thread and publishes owned snapshots; no evidence establishes protection from all concurrent manager changes. Treat validity as a momentary observation and reacquire or validate at use boundaries.

An invalid or destroyed entity can leave an old handle value in plugin data; serial mismatch prevents it from becoming the current occupant’s identity. Entity recreation may reuse an index with a different serial. **UNKNOWN:** exact serial wrap/reuse limits and lifetime across all native allocator implementations were not closed. Fragment access through a stale handle must not be inferred safe merely because an earlier validity check succeeded.

## Mass subsystem ownership

The reported client path obtains UMassEntitySubsystem from the current UWorld and accesses its manager shared-pointer storage. Current BuildingCollector reads manager storage at subsystem offset +0x38, constructs its query with that storage, remembers both world and manager identity, and destroys the query on session clearing. The +0x38 detail is current-source/layout evidence with build-specific native support; it is not a public Unreal ABI.

The query and manager belong to the subsystem/world lifetime. Current source clears entity records, pending keys, definitions, query state, manager/world fields and published snapshot when world or manager identity changes; it advances an internal generation. This matches the world/session invalidation boundary described in [Loading and World Transitions](../Lifecycle/Loading-and-World-Transitions.md). **UNKNOWN:** a subsystem pointer is safe beyond the owning world/session or across teardown. Do not retain it across world transitions.

The historical native subsystem getter is identified as UWorld::GetSubsystem<UMassEntitySubsystem>. Reported ABI: UMassEntitySubsystem* (__fastcall*)(const UWorld*). Query constructor receives opaque query storage and manager shared-pointer storage; query destructor receives query storage. NativeApi carries these typed function pointers after the mandatory fingerprint-resolution chain. The exact private query size, native symbols and signatures are version-bound; they are not generated-SDK public Mass APIs.

## Query construction and execution

### Current MiniMap building query

Current source constructs one aligned 0x350-byte opaque query per active world/manager session. It registers CrMassBuildingTag with presence All. It also registers TransformFragment as ReadOnly/Optional. The query is not spatially limited: grid traversal and view-region selection are later collector work. Optional transform means query membership alone does not guarantee a readable transform.

| Operation | Retained native ABI / behavior |
|---|---|
| Get subsystem | UMassEntitySubsystem* (__fastcall*)(const UWorld* world) |
| Construct query | void* (__fastcall*)(void* queryStorage, const void* managerSharedPtrStorage) |
| Destruct query | void (__fastcall*)(void* queryStorage) |
| Add tag requirement | void (__fastcall*)(void* requirements, const UScriptStruct* tag, EMassFragmentPresence presence) |
| Add fragment requirement | void* (__fastcall*)(void* requirements, uint8 access, uint8 presence); exact instantiated native requirement types are fingerprint-specific |
| Get matching handles | MassEntityHandleArray* (__fastcall*)(void* query, MassEntityHandleArray* returnStorage) |
| Handle array | pointer + int32 Num + int32 Max, 16 bytes in the retained MiniMap ABI declaration |
| IsEntityValid / IsEntityBuilt | bool (__fastcall*)(const void* manager, MassEntityHandle entity) |
| InternalGetFragmentDataPtr | void* (__fastcall*)(const void* manager, MassEntityHandle entity, const UScriptStruct* fragmentType) |
| InternalGetConstSharedFragmentPtr | const void* (__fastcall*)(const void* manager, MassEntityHandle entity, const UScriptStruct* fragmentType) |

The query constructor, destructor, requirement helpers and GetMatchingEntityHandles are private native bindings resolved through the existing fingerprint chain. Current NativeApi and Fingerprints are the authority for current consumers. Names recorded by the fingerprints identify the target functions; a matching byte pattern does not make the ABI public or transferable to another game build.

The returned handle array is an allocated result, not a retained chunk cursor. Current collector checks that the function returns the caller’s result descriptor and that Num/Max/Data are structurally consistent. It frees non-null Data through the paired native free wrapper. Membership reconciliation is global for the query’s Building-tag composition; failure or malformed output is not interpreted as an empty world. A completed scan deduplicates index+serial keys and removes unseen keys only after successful enumeration.

Stock UE 5.6.1 source shows GetMatchingEntityHandles creating an array and exporting matching archetype/chunk handles. Those source details explain the stock implementation only. The SDK generated for StarRupture leaves much of FMassEntityQuery opaque; it does not expose every query builder, iterator or fragment API that exists in engine source. Do not assume all Mass APIs can be called through the generated SDK.

No retained MiniMap contract establishes persistent iterators, caller-owned result storage, query snapshots spanning ticks, observer coverage, or a manager-wide membership revision. Stock UE 5.6.1 source does not support treating archetype versions or entity-order versions as a complete membership certificate. Query construction and cleanup are bound to one world/manager session; query destruction is part of session teardown, not a reusable global operation.

## Fragment and shared-fragment access

Ordinary fragments, shared fragments and tags have different roles:

| Kind | Use and access semantics evidenced here |
|---|---|
| Ordinary fragment | Per-entity data in Mass storage/archetype chunks. Current collector uses InternalGetFragmentDataPtr for transform, spline connection and AP fragment types. A null result means requested data is unavailable at that observation. |
| Const-shared fragment | Shared payload associated with entity composition/configuration. Current collector uses InternalGetConstSharedFragmentPtr for FCrBuildingParameters, then interprets the returned FConstSharedStruct-style wrapper and aligns its payload using UScriptStruct MinAlignment. It is not the payload bytes directly. |
| Tag | Presence/membership marker with no building record payload used here. Current query requires CrMassBuildingTag; a grid result is separately checked for the tag in historical indexed collection work. |
| Shared fragment | A shared value may be interned/reused among entities; pointer equality is not entity identity and does not mean the value is a unique entity definition. Mutation and association lifetimes remain manager-controlled. |

The strongest matched-client building-definition finding is FCrBuildingParameters::PlacementData. Matched CL-127004 evidence traces simple/full building traits copying parameter bytes into manager const-shared storage, FMassEntityManager::AddReferencedObjects traversing that storage, and reflected UObject reference collection reaching PlacementData. The game itself reads PlacementData from the same shared parameters after save load. Matched-client native evidence includes BuildTemplate RVAs 074B64F0 / 074B6070 and manager collection RVA 0443DED0. These RVAs belong only to the identified client image.

The earlier matched-client investigation documented PlacementData’s representation and current collector access, but did not establish the pointed-to UObject ownership chain; it therefore did not treat a borrowed pointer as a trusted identity. Subsequent matched-client analysis traced manager-held shared storage and GC references, supporting a synchronous typed read of the configured definition. This evidence does not prove that every instance payload matches the configured placement definition or make the pointer safe after manager/world teardown.

Historical reports also describe FAuAPMassFragment::Payload as a reference-counted handle to instance placement data; its resolver may synchronously load an asset. Do not shallow-copy that handle as if it were plugin-owned storage. It is a distinct route and was not the selected direct identity path. Tags are not fragments carrying data; presence means composition membership only.

## Fragment pointer lifetime and safety

Current BuildingCollector borrows PlacementData’s shared wrapper and payload, then copies BuildingID, UObject index, transform numbers, spline data and other owned values within the game-thread acquisition path. Its comments explicitly end Mass borrows before asset loading, and the renderer-facing Snapshot contains owned records and shared CPU-side numerical owners, not fragment pointers. This is a valuable current contract, not a universal proof about all Mass APIs.

Stock Mass stores entity fragments in archetype chunks. Entity removal/composition changes can move or invalidate chunk locations. Stock UE 5.6.1 source explains that archetype collection ranges can become stale and entities may be moved during removal/compaction. **CONFIRMED for stock source; shipping fork details remain version-limited.** Stable entity handle does not stabilize an ordinary fragment address. Shared fragments may outlive a particular entity while manager-owned shared storage still references them, but that is not permission to retain an arbitrary raw pointer across mutation, manager replacement or teardown.

No inspected evidence supplies a read lock/lease that authorizes retaining fragment pointers across ticks or asynchronous work. No generic safety conclusion follows from const access, GC reference collection, a stable entity handle, or a prior IsEntityValid result. The established cautious boundary is: use the owning manager/world and supported game-thread phase; check validity/state; access the required fragment; copy needed fields; finish the borrow before operations that may load, mutate structure, yield, publish a pointer, or cross a lifecycle boundary. The exact interval in which concurrent Mass mutation is impossible was not independently proven by the retained artifacts.

## Transform and spatial data

Current collector reads FTransformFragment through the typed SDK and copies Translation, Scale3D and normalized quaternion values. It verifies finite coordinates and finite nonzero quaternion norm. The earlier Mass-only implementation reported mass spline curves as world-space controls in that acquisition path, but that applies to those specific reports and source revision. **UNKNOWN:** every transform/curve producer uses a synchronized coordinate space or is updated atomically with grid membership at every instant.

Spatial discovery has multiple representations: Mass transform, spline curve/bounds copied from fragments or AP payload, entity-grid membership and presentation actor/component transforms. They are not evidence of one universally synchronized snapshot. One historical collector observation records that a valid/built entity can still lack transform, curve, definition or profile facts; the report-described log showed a fixed worldwide membership count while the local published/drawable counts grew at different times. That is **OBSERVED** for that historical client run, not a current-build guarantee.

Current source applies player-centered region selection in XY, uses a grid callback to prioritize pending records, and separately obtains authoritative building membership through a Mass query. Thus a grid-derived coordinate or cell match does not replace the Mass transform/fragment acquisition needed for record data.

## Entity-grid layout and traversal

The historical MiniMap native interface represents FCrGridEntityHandle as a Mass handle plus opaque thread-safe shared-pointer words; its report-declared size is 0x18, with shared payload/controller at +0x08/+0x10. The callback wrapper is represented as a TFunctionRef-style pair: callable at +0, object/context at +8. The reported function type is:

    void (__fastcall*)(void* subsystem, const FVector& center, float radius,
                        const VisitorRef& visitor, bool testEntityRadius)

The source-side visitor ABI is bool (__fastcall*)(void* context, const GridEntityHandle&). Available report-backed evidence describes callback invocation as synchronous and borrowed: the native traversal does not take ownership of the callback object. The callback’s bool meaning (continue/stop/accept) is **UNKNOWN** because the raw native traversal body was not retained; historical current source always returned true. These interface sizes and signatures are report-level evidence, build-bound, and not a generated public API.

In one captured MiniMap grid-performance observation, the query uses an outward-rounded diagonal radius over an enlarged rectangle; it traverses all Z and passes testEntityRadius=false. The visitor separately checks validity, built state, Building tag, and—where needed—finite transform and strict XY inclusion. These are distinct filters. The report explicitly states grid-query timing includes native traversal and visitor work, so it does not isolate a native cell-walk cost. The grid performance figures and exact source line numbers belong to that historical collector state.

Current BuildingCollector calls ForEachCellInRadius with the current presentation sample, radius rounded outward, and false for entity-radius testing. It visits only to prioritize pending keys. Current authoritative membership comes from the separate Building-tag query. Current callback does not make grid traversal the collector’s all-building enumeration source.

| Grid property | Evidence result |
|---|---|
| Hierarchy and cell layout | **UNKNOWN** in the retained report set; no complete cell structure/layout was promoted. |
| Cell selection dimensionality | **CONFIRMED at report level for the cited traversal path:** all-Z traversal; XY inclusion is a subsequent collector predicate. Do not call this a 3D spherical selection. |
| Radius/distance test | The interface has radius and testEntityRadius inputs. In the cited path the flag is false; whether another internal cell-level boundary test uses a different shape is **UNKNOWN** absent the native body. |
| Callback timing/ownership | Report says synchronous borrowed TFunctionRef callback. No callback storage may outlive the call. |
| Callback order | **UNKNOWN.** No stable cell or entity ordering guarantee was found. |
| Boundary inclusivity/coordinate convention | **UNKNOWN.** Exact behavior on grid boundaries and world/local conversion were not established by available primary artifacts. |
| Entity validity/build/tag | Separate predicates; traversal membership alone proves none of these. |
| Built/authoritative building | Grid membership does not prove entity is currently built, carries CrMassBuildingTag, has PlacementData, or is an authoritative building record. |

The reports found no grid-membership certificate that replaces worldwide Mass reconciliation. No traversal algorithm is proposed here.

## Authoritative building state

The best documented configured definition association is the current entity’s FCrBuildingParameters::PlacementData const-shared payload, under a valid synchronous Mass acquisition scope. It supplies the configured placement data including BuildingID and the soft ActorClass path. The collector can copy these values without requiring a spawned presentation actor or loading a Blueprint merely to read the soft path. Subsequent matched-client ownership tracing supports the typed shared UObject reference through native and GC evidence; exact custom-payload versus configured-definition equality remains **UNKNOWN**.

BuildingToRestore is an ECrBuildingID-sized SaveGame field at FAuAPMassFragment +0x48. The earlier matched-client investigation established that the field defaults to NONE, is preserved by copy and restoration, and has a replacement writer. Inspected ordinary spawn paths did not universally populate it, and payload replacement does not update it automatically. It is therefore an optional replacement/restoration-carried value, not a universal current BuildingID source. Waiting until an entity is built does not repair the contract.

The developer-settings placement registry is an ID-to-definition lookup that can synchronously load on cache miss; it does not accept a Mass handle or provide an inverse entity-to-ID lookup. The live actor GetBuildingID accessor exists for suitable initialized actors, but the earlier matched-client investigation did not establish universal Mass-to-actor coverage or an actor-independent lifetime contract. Neither route supersedes the configured shared-fragment association.

Distinguish these stages: Building-tag membership, registered Mass entity, built state, configured placement definition, instance payload, construction/presentation actor, and renderer-ready visual state. **CONFIRMED:** One historical collector observation had records admitted before BuildingID/profile readiness and later type/draw readiness changes without membership count changes. **UNKNOWN:** a single authoritative “settled building” predicate spanning all those states. Historical actor-first collection cannot be treated as complete because actor representation may be absent, delayed, pooled or presentation-only. Conversely, missing actor representation does not prove that the Mass record is absent.

An earlier Mass-only implementation documented actor/component observation and private capture as historical implementation dependencies for particular spline/sprite flows and warned that those observations were not always direct draw gates. Those paths are **SUPERSEDED IMPLEMENTATION** for current MiniMap source. Do not infer that all visible actors are authoritative records or that all authoritative Mass entities already have an actor.

## Persistent building identifiers

Matched client/server PDB and generated-SDK comparison identifies FCrMassPersistentEntityID as a 16-byte struct, alignment 8: uint32 ID at +0, padding +4..+7, and transient FMassEntityHandle CachedHandle at +8. The cached handle has index and serial at its own +0/+4. The field-level evidence is report-backed from client/server PDB and generated-SDK comparison. Numeric ID is the persistent map/save identity; cached handle is an acceleration/cache field marked transient. Copy only the numeric ID when treating this as saved identity.

This is distinct from ECrBuildingID/BuildingID on placement data, Mass handle Index and SerialNumber, UObject index/serial, actor pointer, and FMassNetworkID. No evidence equates these domains. **UNKNOWN:** a persistent ID does not directly identify a particular Mass handle outside manager lookup/reconstruction. No shared allocation counter or serial semantics between persistent IDs and Mass entity serials were established.

Matched-client persistent-ID analysis reports a high-water MaxID restored with save data, allocator registration through ID/handle maps, and native invalid ID sentinel 0xFFFFFFFF. Its CL-127004 / 0.3.5 findings show persistent numeric associations may exist separately from a saved entity identity, including custom-name keys, storage-transfer keys, stability/connectivity state and offline player floor references. An entity rejected during load may leave references whose later numeric lookup could resolve differently if the number is reused. This is a historically identified mechanism and safety boundary, not evidence that a user’s current save has a collision.

The persistent-ID reuse analysis does not establish a universally safe minimum reusable-ID set. It explicitly leaves some delayed reference cleanup and registration timing unresolved. Persistent-ID reporting here is descriptive and does not recommend counter edits, reindexing, or a reuse algorithm. In particular, do not infer Mass serial reuse from persistent-ID high-water behavior.

## Mass entities versus UObject presentation

A Mass handle addresses manager-owned entity storage. A shared PlacementData reference is a UObject reachable through manager-owned shared fragment storage under the documented ownership chain. A presentation actor is a separate UObject/world instance and may be associated through Mass actor/representation systems. These identities and owners must remain distinct:

| Observation | Correct interpretation |
|---|---|
| Valid Mass handle | That manager currently recognizes this index/serial. It does not imply actor existence. |
| PlacementData pointer in const-shared parameters | Configured definition UObject reference, supported during the synchronous manager-owned borrow and its UObject lifetime. It is not the entity handle or the instance payload. |
| Actor pointer or actor-manager association | Representation/presentation linkage at an observation. It does not prove universal building membership or persist across pooling/replacement. |
| Blueprint ActorClass soft path | Authored configured class identity; reading/copying its path is not proof that an actor was spawned or that visual resources are ready. |
| World-visible mesh/component | Presentation state. Absence, delayed spawn, hidden state or actor replacement does not alone determine Mass entity existence or save identity. |

An actor getter considered in the earlier matched-client investigation was rejected as a universal source because the required actor coverage was not established. Later matched-client ownership tracing established the shared PlacementData GC chain and direct configured-definition path. For general UObject identity and GC limits, see [Loading and World Transitions](../Lifecycle/Loading-and-World-Transitions.md); for rendering and capture resource lifetimes, see [Rendering and Asset Capture](Rendering-and-Asset-Capture.md). No broad duplicate is needed here.

## Historical failure cases and corrections

| Case | What evidence shows | What remains unresolved |
|---|---|---|
| Index-only identity | The historical collector key uses index+serial and generation; the stock UE 5.6.1 comparison describes serial validation and index reuse. | Exact serial wrap and all-client/server allocator details. |
| BuildingToRestore treated as type | The earlier matched-client investigation found that the field defaults to NONE, is copied during restoration, has a replacement writer, and is not updated automatically by SetPayload. | Full writer/callback coverage and semantics in every family. The “strictly restoration-only” hypothesis is contradicted by replacement write. |
| Registry reverse lookup treated as entity identity | Native getter takes ECrBuildingID and returns a definition; no Mass handle inverse or complete catalog/alias proof. | Complete authored registry contents and all late registrations. |
| PlacementData rejected as untrusted pointer | The earlier matched-client investigation lacked ownership proof; subsequent analysis traced manager shared-fragment GC traversal and game reads. | This correction covers configured placement association; custom payload equivalence and post-teardown use remain unresolved. |
| Grid result treated as authoritative membership | Historical evidence separates spatial visit, validity, built, tag and transform filters; current collector uses grid only for pending priority. | Full native traversal internals, callback order/return behavior and exact edge inclusivity. |
| Slow/staggered observations interpreted as late entity creation | A historical collector observation recorded fixed worldwide counts while local published/drawable counts changed; one logged example lacked definition owner. | Per-entity failed predicate and world/grid insertion timing were not logged; no single root cause was proven. |
| Safe ID reuse inferred from primary identity maps only | Matched-client persistent-ID analysis inspected retained numeric reference categories and rejected map-only absence as universal proof. | A closed minimum reference set and cleanup in every load/listener path. No runtime collision was demonstrated. |
| Reused saved handle / pointer presumed stable | Stock UE 5.6.1 collections move on composition/removal; current query result is materialized. | No retained lock, tick-spanning fragment pointer contract or universal shipping-fork concurrency guarantee. |

Rejected or superseded claims: entity index is not globally unique; a persistent ID is not a Mass serial; BuildingToRestore is not universally current type; a grid hit is not a built authoritative building; actor presence is not required to establish Mass membership; and a pointer returned from one successful fragment lookup is not a cross-tick lease. These are domain distinctions, not proposed implementation changes.

## Implications for MiniMap

Current source reconciliation is limited to BuildingCollector, BuildingBlueprintIdentity, NativeApi and Fingerprints:

- Current building query requires Building tag All and optional read-only Transform; query and manager are session-owned and destroyed/reset with the world/manager generation.
- Current membership key is packed Index+SerialNumber. Reconciliation does not prune based on a partial or malformed handle array.
- Acquisition checks IsEntityValid then IsEntityBuilt before fragment reads, copies configured BuildingID from const-shared PlacementData, transform and spline/AP data, and returns owned record values. It validates identity again after definition preparation that can reenter engine code.
- Grid traversal is performed only to prioritize pending local acquisition, not as the sole source of authoritative building membership.
- BuildingBlueprintIdentity converts the configured soft ActorClass reference into an owned path string; the renderer does not use that string as a Mass handle.
- NativeApi continues to hold typed native function pointers populated from ResolvedAddresses. The mandatory chain remains Fingerprints.cpp → MiniMapFingerprints::ResolvedAddresses → NativeApi::Initialize → typed APIs → consumers. The current mass functions are private bindings; the generated SDK is not treated as a complete Mass API.

The current source represents a Mass-first building collector, but source inspection cannot prove its native fingerprints or ABI against a currently running game. Earlier implementation proposals for new caches, observer streams, incremental cursors, or replacement collectors are not current contracts and are not promoted here. Current collector behavior remains the authority for what MiniMap consumes today; historical CL-127004 evidence remains authoritative only for claims tied to its matched image.

These implementation boundaries preserve the typed NativeApi ownership chain, world/session ownership, generation/index/serial identity, borrowed-data scope and source-version qualifications. Current static NativeApi typed pointer storage is intentional; this reference describes the inspected source and does not define a new native-access interface. Acquired building facts remain distinct from region selection, sprite choice and rendering lifetime; actor presence does not prove complete Mass membership, and a persistent save ID is not a Mass handle.

## Unresolved questions and negative findings

- **UNKNOWN:** full native FMassEntityHandle serial wrap policy, manager slot reuse timing and whether any private fork changes stock validity semantics.
- **UNKNOWN:** a valid/built check’s concurrency guarantee beyond its immediate manager call; no tick-spanning structural lock/fragment lease is evidenced.
- **UNKNOWN:** complete native query APIs available in all generated SDK variants and a query execution surface supported by those declarations.
- **UNKNOWN:** full FCrGrid cell hierarchy/layout, cell-boundary inclusion, ordering, internal radius metric, coordinate transform and visitor bool semantics. The retained ABI/semantics for grid traversal are partly report-only.
- **UNKNOWN:** universal synchronization among Mass transform, spline data, grid membership and presentation actor transform.
- **UNKNOWN:** equivalence of every instance placement payload to its configured PlacementData and exhaustive authored definition/variant coverage.
- **UNKNOWN:** the complete lifecycle and reuse guarantees for persistent numeric IDs across every delayed save listener/reference category. The existing evidence is not a save-repair specification.
- **Missing raw evidence:** The matched-client ownership investigation records that key disassembly was inspected in memory, but the instruction excerpts were not retained. Its native ownership/ABI details therefore remain **REPORT-BACKED**, qualified to the identified executable/PDB. No standalone native `UCrEntityGridSubsystem::ForEachCellInRadius` body or complete grid layout is available; grid ABI/behavior remains report-backed plus current source declaration, not independently re-disassembled.
- **Missing runtime evidence:** One historical collector run was described using a frozen runtime log, but the raw log is not retained here. Quantitative observations remain **REPORT-BACKED**; no root cause is inferred.

## Evidence basis and current-source references

Historical native/runtime findings are **REPORT-BACKED** unless identified as current source or supported by retained primary evidence. Matched-client executable/PDB identity and offline RVAs are stated where relevant. The native ownership disassembly excerpts and one historical collector runtime log were not retained for independent reinspection; grid traversal ABI/layout remains partly unknown. These limitations do not change the distinctions among Mass identity, configured definition, persistent save identity and spatial grid visitation described above.

Current MiniMap behavior was reconciled read-only against repository source. This establishes inspected source behavior, not live executable compatibility.

| Current source | Use in this reference |
|---|---|
| `Src/Native/NativeApi.h`, `Src/Native/NativeApi.cpp`, `Src/Native/Fingerprints.cpp`, `Src/Native/Fingerprints.h` | Current Mass/Grid ABI typedefs, typed pointer storage, required fingerprints and grid callback wrapper. |
| `Src/Map/Buildings/BuildingCollector.cpp`, `Src/Map/Buildings/BuildingBlueprintIdentity.cpp` | Query setup/destruction, world/manager reset, handle key, valid/built checks, synchronous copies, grid priority and owned identity text. |
