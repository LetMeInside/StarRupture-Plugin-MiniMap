# R2.5a bounded authored Smelter assembly audit

## Baseline and scope

Initial branch `develop`, HEAD `e0e5d34433279d65e5b7a72bcb6f2e205198756b`.
Preexisting R2.4b changes were present in BuildingRepresentationDiagnostic.cpp,
BuildingCollector.cpp/.h, MiniMap.vcxproj, RepresentationSelectionCore.h,
RepresentationSelectionTests.cpp and StageR24b-RepresentationSelection.md.
They were preserved. No deployment, game execution or Git mutation was performed.

The existing runtime Building_336_Representation.json establishes ten static
instances/seven meshes, one skeletal instance, three other scene components and
one non-scene component across fifteen SCS nodes. These counts are evidence from
R2.4b, not a result of running R2.5a. Snapshot IDs are never hardcoded.

## Native gate

Confirmed against installed client PE/PDB GUID
`41cbaea9-7ac3-99da-b947-a1db9c1021ed`, age 1:

| Read | Installed PDB evidence | Runtime validation |
|---|---|---|
| Existing CDO | UClass +0x110 | Retained selected class; resident actor CDO identity, type and RF_ClassDefaultObject=0x10; never GetDefaultObject |
| RootComponent | AActor type 0x181e15, size 0x2a8, +0x1b8 | Validated scene component |
| MainComponent | ACrBuildingActorBase type 0x721a9a, size 0x6c8, +0x2d8 | Validated scene component |
| MainMeshComponent | Same native class, +0x2e0 | Validated static-mesh component |
| Pointer wrappers | All three fields: eight-byte TObjectPtr, FObjectPtr at +0, native pointer representation at +0 | Unsupported/unresolved handles fail pointer alignment and indexed UObject identity checks; no handle resolver |
| Native parent identity | UObject FName +0x18, comparison index +0, number +4 | Raw FName equality; deduplicate aliases of one component, reject distinct-object ambiguity |
| Scene metadata | AttachParent +0xc8, socket +0xd0, location +0x140, rotator +0x158, scale +0x170, flags +0x1a0/+0x1a1 | Existing identity/type/extent gates, local strong retention |
| SCS | Node template +0x30, socket +0x80, native parent name +0x88, native flag +0x98; existing verified arrays | Reuse existing bounded traversal and copied graph edges |
| Materials | Mesh static slots +0x160, skeletal slots +0x1a0, component overrides +0x530; strides 0x38/0x30/8 | Ordinary validated 16-byte arrays, Num +8, Max +12; cap 32; validate/retain each nonnull material |

`Tests/VerifyRepresentationAbi.py` now checks the three direct native-component
fields and their pointer wrappers in addition to the existing contracts. Its full
installed PE/PDB verification passes. No new native signature or callable was
introduced. The fixed skeletal assignment getter remains the already verified
R2.4 getter, used only by the existing traversal; the audit reuses its retained
assignment and does not call it again.

Installed CalcNewComponentToWorld_GeneralCase RVA 0x048de240 disassembly confirms
parent socket transform acquisition, signed-scale branch and the ordinary QST
path (positive branch +0x5af), followed by absolute bits 4/8/16. This is offline
evidence, never a runtime address or approved call. UE supporting source:
UnrealMath.cpp FRotator3d::Quaternion at line 483 (scalar equations 532), and
TransformNonVectorized.h Multiply at line 1311 (ordinary equations 1341–1343).
The audit uses CPU equations with normalized quaternions:

    Q = Qparent * Qrelative
    S = Srelative * Sparent                 // componentwise
    T = Rotate(Qparent, Sparent * Trelative) + Tparent

Positive nonuniform scales follow Unreal QST semantics, not arbitrary affine
matrix composition. Nonfinite, negative/degenerate scales, absolute transforms,
socket transforms, cycles, ambiguous parents, unreconciled template attachment
pointers and excessive depth fail closed. Euler angles are never added.

SetupMainMesh RVA 0x074806f0 and SetupSkeletalMesh RVA 0x071c7f50 remain offline
identified functions. Their full actor-setup effects are unverified. Neither is
called. The GetMaterial helper's ConditionalPostLoad path remains excluded.

## Collection and ownership

The audit runs inside the existing one-shot, game-thread R2.4b synchronous
context after at most one full representation snapshot. It reuses selected class,
retained assignments and SCS metadata. It requires selected building ID 336;
other selections produce `not_attempted_target_not_selected`. Current world and
collector generation are checked before and after audit. Native references are
local to Context and release in reverse order before either JSON export.

Only the three direct existing-CDO fields are considered as native parent
candidates. Raw declared SCS parent FName is compared dynamically. Native parent
chains must reach the validated CDO RootComponent within sixteen levels. The
assembly reference space is that root component: its own placement/external
attachment is excluded explicitly. No missing parent is replaced by identity.

Every static instance remains distinct, even with shared mesh identity. The
separate hierarchy table includes non-mesh intermediate nodes. Effective root
transforms are emitted only when the entire chain validates. Raw input vectors
remain available when composition is blocked. Nonfinite values serialize as null.

Mesh-authored slots and component overrides remain separate. A conditional
proposed mapping applies nonnull validated overrides by slot, otherwise the
authored input. No virtual material getter or postload function is invoked.
Null/unreadable/truncated inputs prevent a positive material-readiness result.

Skeletal output records the retained mesh, authored transform/hierarchy,
visibility and stored material inputs. Animation mode/class, pose support and
streaming-sensitive bounds/render data are explicitly not attempted or null.

Mass mesh references are CPU-matched by snapshot mesh identity to every matching
ordinal, with raw local transforms, offsets, significance and material inputs.
Repeated instances are not paired by list order. The complete separate Mass
descriptor remains available; no renderer-equivalent transform or material
equivalence is claimed and no actor/Mass geometry is combined.

## Optional bridges and readiness

The exact private R2 capture asset is not exposed in this diagnostic context.
No verified simultaneous access or copied generation-token bridge is established.
`identity_bridge_unavailable` is emitted; no relookup, retention extension or R2
integration change was introduced. No exact main-body asset match is claimed.

Alpha sharing is `unsupported`: immutable CPU readback sharing has not been
established. No changes were made to capture scheduling, readback ownership or
PNG conversion/export. Existing alpha observations remain R2.3.1 evidence.

Readiness is deliberately conservative. Static hierarchy success is separate
from runtime actor-setup/cooked-instancing equivalence, effective inherited
override selection, skeletal completeness and future multi-component cleanup.
`static_only_diagnostic_capture` remains false while these essential capture
prerequisites are unresolved. `complete_visual_building` is always false here.
This audit is ready for a separately authorized controlled runtime diagnostic;
it does not authorize or establish readiness for multi-component rendering.

## Output and bounds

Output through the existing plugin-relative resolver and temporary-file atomic
publication:

    <MiniMap.dll directory>\MiniMap\Cache\Buildings\Diagnostics\Smelter_AssemblyRecipe.json

Schema version 1, stage R2.5a. Main fields: game_build, building_id, status,
representation_source, reference_space, identity_bridge, native_parent,
hierarchy_nodes, static_instances, skeletal_components, material_resolution,
mass_comparison, alpha_summary, readiness, truncation and limitations.
Native IDs are snapshot-local strings; raw addresses are never exported.

Caps: sixteen static instances, thirty-two selected-class SCS nodes, sixteen
skeletal instances, sixteen parent levels, thirty-two slots per material array,
4,096 additional audit identity/graph visits; existing overall R2.4b work budget
also applies. CPU Mass comparison retains at most sixty-four references and
sixty-four raw offsets from the already bounded R2.4 traversal. Array arithmetic,
allocation storage/alignment, identity, class and residency are validated.
Per-section JSON caps and a 256 KiB final document cap report truncation; final
overflow emits a valid small JSON failure document with readiness false.
No per-frame retry or logging is introduced. Failed publication leaves native
references already released and does not affect capture cleanup.

## Files and verification

Added AssemblyRecipeCore.h, AssemblyRecipeAudit.inl, AssemblyRecipeTests.cpp and
this document. Modified BuildingRepresentationDiagnostic.cpp, MiniMap.vcxproj
(header registration only) and VerifyRepresentationAbi.py. R2.4b collector and
selection-policy files were not changed by this stage.

Validation: normal x64 Client Release and Server Release builds pass; new CPU
tests pass for trusted UE quaternion/QST vectors, nonuniform scales, unsupported
inputs, hierarchy/cycles/depth/shared budget, native-parent aliasing/ambiguity,
repeated instances, distinct overrides, partial skeletal metadata and JSON
escaping/overflow. Existing readback, RGBA16F, WIC PNG, representation-metadata
and R2.4b selection tests pass. Independent Python JSON parsing passes for the
synthetic assembly, selection and metadata documents. Installed ABI verification,
COFF static-initializer/server-isolation inspection and git diff --check pass.
Actual R2.5a native-parent values and recipe results remain unobserved until the
authorized runtime test. No complete Smelter has been reconstructed or rendered.
