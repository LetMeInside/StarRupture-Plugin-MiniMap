# Story-Item Identity and Collection State

## Scope and version basis

This reference preserves bounded historical findings from an initial investigation and two follow-ups for actor-based Abandoned Base datapads, the Abandoned Base wall computer, and interactive ground audiologs. It documents authored actor identity and the reported native story-state paths; it does not define a current MiniMap feature contract.

The reports are dated 28 October 2026. Their game version/build label is **UNKNOWN**. The initial investigation and second follow-up record the inspected client executable's matching PDB identity as GUID `41cbaea9-7ac3-99da-b947-a1db9c1021ed`, age 1; the reports do not state that this is CL-127004. The second follow-up also records installed package/UCAS observations, but no game CL. Do not attach the separate CL-127004 qualification from the persistent-ID investigation to these findings. Continued validity on later game builds is **UNKNOWN**.

All asset, native, and runtime-state conclusions below are **REPORT-BACKED** by the original reports. Their cited raw cooked package decoding and offline native inspection were not re-run for this restoration. The archive contained the three reports, but no separate raw package extracts or native disassembly artifacts. No runtime Mass query or gameplay verification was performed.

## Executive technical summary

The historical reports establish three actor families in an inspected `AB_S_12` Abandoned Base: a wall-mounted computer, two ordinary Personal datapads, and one interactive ground audiolog. A POI story-assignment list associates their distinct actor GUIDs with Mission Logs progress. The actor instance, its full Blueprint class identity, its story-item GUID, collection status, read-entry state, playback state, world position, and any runtime Mass entity are different identity/state domains.

The wall computer and ordinary Personal datapads derive through `BP_DatapadBase_C` from native `ACrDatapadBase : AActor`. The interactive audiolog is a separate `ACrAudiologBase : AActor` family. An actor's class identifies its authored type, not a unique placed instance. The GUID provides the reported per-story-item association; it is not a Mass handle, building ID, UObject address, or location.

For the identified examples, story collection advances through the base's per-GUID story status. Datapad read progress is stored separately by datapad GUID and entry name. Audiolog start/played state is a third path. Collection, reading, and audio playback completion must not be treated as equivalent.

## Actor-based story-item discovery

The initial investigation established an actor-based datapad/email system and a separate Abandoned Base story-progress path, while leaving the exact wall screen identity unresolved. A follow-up then identified one placed Abandoned Base computer by its cooked Blueprint class, import identity, actor GUID, content rows, and POI assignment. A second follow-up documented two ordinary Personal datapads and one interactive audiolog in the same generated cell and tied all four actors to the POI's story-item assignment list.

For the inspected examples, the placed actor is the spatial/interactable object; its authored content is associated through data-table rows and GUID-keyed state. The reports did not establish a runtime Mass proxy for any of these actors. They also did not establish that every similar-looking asset or all subclasses belong to this Abandoned Base story set.

## Full Blueprint class identity

Use the full package and generated class identity recorded by the reports:

- Wall computer: `/Game/Chimera/Environment/Datapads/BP_DatapadComputer.BP_DatapadComputer_C`
- Ordinary Personal datapad: `/Game/Chimera/Environment/Datapads/BP_DatapadPersonal.BP_DatapadPersonal_C`
- Interactive audiolog: `/Game/Chimera/Environment/Audiologs/BP_Audiolog.BP_Audiolog_C`

The computer and Personal classes are siblings below `BP_DatapadBase_C`, which derives from native `ACrDatapadBase`. The interactive Environment audiolog derives from native `ACrAudiologBase`; it is not an `ACrDatapadBase`.

A short class/export name is insufficient to identify the interactive audiolog. The decorative asset `/Game/Chimera/Buildings/AbandonedBases/Furniture/Meshes/BP_Audiolog` has the same short Blueprint name and, in the report's cooked comparison, the same local export hash, but belongs to a different package and derives from plain `AActor`. Full package identity and native ancestry avoid classifying decorative furniture as an interactive story actor. The same rule applies to runtime object classification and asset searches: do not use basename matching.

`BP_DatapadComputer_FE_C` and `BP_DatapadPersonal_FE_C` were reported as Base siblings, not subclasses of the two regular classes above. Their existence does not establish that they are members of this inspected Abandoned Base story set. No additional instances are inferred here.

## Abandoned Base wall computer

The identified placed computer is cell actor export 10 in `ChimeraMain` generated cell `8PAERCPGSTCXKBH00YF4I3X6C.umap`. Its full class identity is `/Game/Chimera/Environment/Datapads/BP_DatapadComputer.BP_DatapadComputer_C`; its actor GUID, written as the four `FGuid` words in the report, is `9A56CCAF-4329532F-BF74F591-630898D3`. It references content rows `D_SmallBase12_01` and `D_SmallBase12_02`.

In `ChimeraMain.umap`, POI export 806 (`BP_PointOfInterestMarker_C_UAID_00090FFE0001B60902`, discovered-name key `AB_S_12`) includes that exact GUID in `AbandonBaseStoryItemsAssigned`. The report records the placed actor root at approximately `(X=-234855.499604, Y=-33478.765609, Z=3556.167357)` Unreal units. Its visible meshes attach to the actor's `DefaultSceneRoot`, and the inspected component/template offsets were effectively coincident with the root. The report therefore identifies the validated actor root's world position as the appropriate spatial anchor for this instance.

That anchor is not a persistent location identifier. The serialized coordinates describe one placed example, not a universal position or offset. Use of a root location in a future context would still require a valid current actor/component and world transform; the story GUID does not encode coordinates.

The report-backed interaction path says inherited `ACrDatapadBase::NativeOnInteract` reports the actor GUID to the Abandon Base subsystem. Story status is separate from reading both content rows: the computer's collection transition does not wait for all messages to be read. Base story status is saved through the Abandoned Base save-data path; entry-read state uses GameState save data (details below).

## Personal datapads

The regular class is `/Game/Chimera/Environment/Datapads/BP_DatapadPersonal.BP_DatapadPersonal_C`. Two placed examples are actor/root exports 11/298 and 12/299 in the same generated cell:

| Actor GUID | Content row | Reported root position (Unreal units) |
|---|---|---|
| `BC03F0EF-470C4F0B-CDE66183-CD999B09` | `D_SmallBase12_03` | `(-232253.239274, -33982.921662, 1177.811380)` |
| `B68A4328-461701A4-AE4DDEB9-A3E1B6C0` | `D_SmallBase12_04` | `(-234415.120414, -32842.018736, 4357.462234)` |

Both GUIDs appear in the `AB_S_12` POI's `AbandonBaseStoryItemsAssigned` list. The report describes the root as the spatial anchor. The Personal mesh template has a small local translation; do not copy it as a world offset or assume it applies to rotated or other placed instances.

Each inspected Personal actor has one configured content row. The report traces the inherited interaction through entry-read recording and then story collection. Thus these two examples participate in both mechanisms, and the one-entry interaction normally advances both. This per-instance observation does not make collection and reading interchangeable for every datapad subclass or content configuration. For a story-item marker, the relevant reported state is the matching GUID's story `Status`, not an assumption that all entries are unread or that presentation alone identifies a story item.

The reports describe ordinary Personal datapads as actors with no authored MassAgent/inventory bridge in the inspected definitions and placed examples. That is bounded cooked/native evidence, not proof that no separate runtime entity can ever be created.

## Interactive audiologs

The inspected ground voice log uses `/Game/Chimera/Environment/Audiologs/BP_Audiolog.BP_Audiolog_C`, derived from `ACrAudiologBase : AActor`, not the datapad base. Actor export 8 in the generated cell has GUID `933DC23A-40531E55-E45E24A8-EDD5ABE9` and references `AudiologEntryDT` row `AB_S_12_01`. Its reported root position is `(-233841.127428, -36844.721219, 498.234468)` Unreal units. Meshes and animated cover attach below the actor's `DefaultSceneRoot`; the report identifies the validated root, rather than the animated cover, as the spatial anchor for this placed example.

The report traces the collection notification through `ServerOnAudiologStarted` to the Abandon Base story subsystem. For this inspected path, collection is marked at the start notification, not at playback completion. An interaction that does not reach that notification is not shown to collect. This does not establish repeat-play behavior, nor does it make the start notification equivalent to hearing the complete recording.

Audiolog playback state is separately represented by replicated `bWasPlayed` and GameState `PlayedAudiologs`, which the report says is persisted in `FGameStateSaveData`. The audio actor has no datapad `ReadEntries` list. Neither `bWasPlayed` nor membership in `PlayedAudiologs` should be interpreted as proof that playback completed.

## Decorative versus interactive actors

The decorative `/Game/Chimera/Buildings/AbandonedBases/Furniture/Meshes/BP_Audiolog` asset is distinct from the interactive `/Game/Chimera/Environment/Audiologs/BP_Audiolog.BP_Audiolog_C` actor. Their short names coincide; the report also notes coincident local export hashes. Their package locations and superclass identities differ. A basename-only scan can therefore create false positives in runtime actor classification, map markers, and asset discovery.

## Story GUID relationships

The report identifies four GUIDs in the `AB_S_12` POI story list:

| Story actor | GUID | Associated content |
|---|---|---|
| Wall computer | `9A56CCAF-4329532F-BF74F591-630898D3` | `D_SmallBase12_01`, `D_SmallBase12_02` |
| Personal datapad 1 | `BC03F0EF-470C4F0B-CDE66183-CD999B09` | `D_SmallBase12_03` |
| Personal datapad 2 | `B68A4328-461701A4-AE4DDEB9-A3E1B6C0` | `D_SmallBase12_04` |
| Interactive audiolog | `933DC23A-40531E55-E45E24A8-EDD5ABE9` | `AudiologEntryDT: AB_S_12_01` |

These are per-item semantic/save associations as recorded by the historical reports. They are not actor addresses, UObject indices, Mass entity handles, persistent building IDs, or spatial anchors. A class path identifies the authored class; a GUID identifies the reported story item; neither alone identifies the actor's current lifetime or world placement.

## Collection, read and playback state

Keep these states separate:

1. **Story collection status:** `FCrAbandonBaseData::StoryItemsCollectedStatus` contains GUID/status pairs. For the inspected wall computer and Personal actors, the reported interaction path reaches `OnStoryItemCollected` and changes the matching status to true. The MissionLogs UI consumes the collected/total story-item count.
2. **Datapad entry-read state:** actor `ReadEntries` contains entry names. GameState `ReadDatapadEntries` is keyed by datapad GUID and stores read entry names. The reports describe per-entry additions, read visuals, and save/load separately from story collection.
3. **Audiolog playback/start state:** the audiolog uses `bWasPlayed` and GameState `PlayedAudiologs`; story collection is separately forwarded on the start notification. No playback-ended/completion event was established as the collection trigger.

For the wall computer, the reports state that story collection may occur while its two email rows remain unread. For each inspected Personal example, one configured entry is read and collection is reported in the same interaction, but this is an instance-specific path, not a general equivalence. For the audiolog, start notification is the story collection trigger in the inspected path; completion of playback is not.

## Story save/load relationships

The reports describe three distinct persistence paths:

- **Abandoned Base story progress:** `SaveAbandonBaseSaveData` copies GUID/status pairs into `FCrAbandonBaseSaveData`; `LoadAbandonBaseSaveData` restores or merges statuses by GUID. This is the collection state used by Mission Logs.
- **Datapad reads:** `FGameStateSaveData::ReadDatapadEntries` stores the GameState's GUID-keyed map of entry names. A follow-up report specifically describes load restoring saved names, including partial lists, into each actor's `ReadEntries`. The initial account had not established partial restoration; the later evidence supersedes that earlier uncertainty for its inspected build/evidence basis.
- **Audiolog played state:** GameState `PlayedAudiologs` stores GUIDs and is copied through GameState save/load; actor `bWasPlayed` is restored to update played-state visuals. This remains distinct from Abandoned Base story collection and does not prove playback completion.

These are the actual supported paths recorded in the reports, not a complete save-schema inventory. Cooked asset observations describe authored classes, placement, GUIDs, and content references; they are not themselves runtime save-state observations.

## World placement and spatial anchors

For each listed placed example, the reports use the actor's validated root component/world location as the spatial anchor. They provide no evidence that a story GUID is a coordinate, a persistent world-position key, or a universal actor-root offset. The listed coordinates belong only to the inspected generated-cell instances. Do not infer additional placements from class names or GUID patterns.

## Foundable registry limitations

A separate historical registry investigation records a **NO-GO** for using historical `FoundableEntitiesWithBlueprintsInside` registry membership as authoritative story-item or current reward classification. It describes that registry as local Mass handles enrolled for blueprint-replacement processing, with no supported SDK/NativeApi read interface; the member is not shown as a replicated property, and save restoration was not shown to rebuild membership. Its enrollment semantics do not classify the story-actor GUIDs or story collection state. These limitations are report-backed and version-specific to that registry investigation; registry enrollment must not be promoted to a stable read contract.

## Runtime Mass-proxy status

**UNKNOWN — RUNTIME MASS-PROXY RELATIONSHIP.** The second follow-up found no qualifying Mass proxy in the examined cooked definitions, native initialization paths, placed examples, or nearby authored spawners. It did not inspect a live Mass population. Actor-based discovery, actor placement, shared story GUIDs, or the absence of an obvious handle do not prove that a runtime proxy is absent. Conversely, none establishes that a proxy exists. No runtime query is documented here.

## Historical corrections and negative findings

- The follow-up resolved the initial uncertainty about the wall-terminal class, discriminator, anchor, and state for the identified placed Abandoned Base computer; the initial investigation remains useful for its explanation of the actor/read/story data model.
- The initial uncertainty about partial `ReadEntries` restoration was narrowed by a follow-up report, which describes restoration of partial name lists during GameState load.
- The second follow-up adds ordinary Personal and interactive audiolog examples. Its conclusion about absent authored proxy representations remains explicitly bounded and does not close live runtime Mass membership.
- `BP_DatapadPersonal_FE_C` and `BP_DatapadComputer_FE_C` were reported as siblings of the regular variants; no inclusion in the `AB_S_12` story list is established here.
- A similarly named decorative audiolog is not the interactive actor.
- Historical Foundable registry enrollment is not authoritative current classification.
- The reports do not establish whether these actor identities or state semantics remain unchanged in later game versions.

## Unresolved questions

- **UNKNOWN:** whether any of the reported story actors also have runtime Mass proxies or a separate dynamically created counterpart.
- **UNKNOWN:** whether the reported class paths, GUID assignments, save-state behavior, and placement remain valid in a later game build.
- **UNKNOWN:** behavior for uninspected datapad subclasses, other bases, repeated audiolog interactions, or actors outside the recorded placed examples.
- **UNKNOWN:** whether a playback-started state is ever reset/replayed or corresponds to playback completion; the reports do not establish that contract.

## Evidence basis and internal reference

The actor classes, GUIDs, placed examples, state/save paths, and registry limitations in this reference are **REPORT-BACKED** and bounded to the examined game version and examples. The raw cooked packages and selected native excerpts were not retained for independent reinspection. The historical reports did not inspect a live Mass population; no runtime query or Mass-proxy conclusion is claimed. Version stability and uninspected subclasses remain unknown as stated above.

For general Mass identity and persistent-ID/handle distinctions, see [Mass Entity Identity and Building Discovery](Mass-Entity-Identity-and-Building-Discovery.md). That reference does not establish story-item runtime membership.
