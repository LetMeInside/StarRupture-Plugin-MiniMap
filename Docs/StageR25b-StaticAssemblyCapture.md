# R2.5b static-template Smelter capture

## Baseline and scope

Initial checkout: `develop`, HEAD and local `origin/develop` both
`81506e39c4e46b3376491164597f0c1adffe45ff`, clean. This is the pushed
R2.5b.3 checkpoint, immediately following `40476ed`. Its retirement tests,
ABI verifier and normal Client DLL were present. No Git history/reference
changes, deployment or game execution were performed.

This implementation captures authored static templates only. It excludes the
skeletal component, construction scripts, runtime actor setup, effective cooked
instancing and live state. It does not assert complete-building fidelity.

## Files

Modified:

- `Src/Experiments/AssemblyRecipeAudit.inl`
- `Src/Experiments/BuildingRepresentationDiagnostic.cpp`
- `Src/Experiments/BuildingRepresentationDiagnostic.h`
- `Src/Experiments/SmelterCaptureExperiment.cpp`
- `Src/Experiments/SmelterCaptureExperiment.h`
- `Src/Experiments/SmelterCaptureNativeBindings.h`
- `Src/MiniMap.props`
- `Src/MiniMap.vcxproj`

Added:

- `Src/Experiments/StaticAssemblyCaptureCore.h`
- `Src/Experiments/StaticAssemblyCapturePatterns.inl`
- `Tests/StaticAssemblyCaptureTests.cpp`
- `Tests/VerifyStaticAssemblyCaptureAbi.py`
- This report.

## Native gate

Confirmed offline against installed client executable/PDB GUID
`41cbaea9-7ac3-99da-b947-a1db9c1021ed`, age 1. RVAs here are evidence only;
runtime resolution uses unique fingerprints after the existing build gate.

| New boundary | Installed evidence and use |
|---|---|
| `UMeshComponent::SetMaterial(int32, UMaterialInterface*)` | RVA `0x048B2A60`; PDB void x64 member, two explicit arguments. Body grows stored overrides at +0x530, stores indexed material, uses normal engine material/PSO notifications. Called on a new, unregistered exact native static component on the game thread. No material getter is called by MiniMap. |
| `USceneComponent::SetWorldTransform(const FTransform&, bool, FHitResult*, ETeleportType)` | RVA `0x04911140`; PDB void x64 member, four explicit arguments. Unattached branch at +0x12A forwards the full transform to SetRelativeTransform. No sweep, null hit storage, installed TeleportPhysics enum value 1. |
| Show-only native wrapper | `execShowOnlyComponent`, RVA `0x04922E60`; writes mode 2 at `0x04922ED1`, constructs a weak index/serial pair, calls native AddUnique at `0x04922F2C` for array +0x278. The existing exact reflected dispatch is reused and its ExecFunction must match this resolved entry. |
| Mesh bounds | `UStaticMesh::ExtendedBounds` +0x240. PDB bounds size 0x38, double Origin +0, double BoxExtent +0x18, radius +0x30. Only copied stored bounds are read; no RenderData or vertex/index buffers. |
| Capture membership | Component weak array +0x278, separate actor array +0x288. FWeakObjectPtr size 8, index +0, serial +4; FUObjectItem size 0x18, serial +0x10. Read entries are compared with retained, indexed validated objects; weak pointers are never resolved. |
| Transform representation | Installed FTransform size 0x60, quaternion +0, translation +0x20, scale +0x40; SDK alignment checked at compilation. R2.5a's verified positive-scale QST reconstruction is reused. |

Three new 64-byte contextual fingerprints match unique unwind function entries.
The verifier also rechecks 40 existing capture/readback fingerprints, baseline
RHI/readback layouts and eight staging references. Existing representation and
RHI-depth fence ABI verifiers pass independently.

Reused unchanged contracts: native SpawnObject indirect subclass argument,
transient package/flags, AddRef/ReleaseUObject, mesh assignment, capture-only
visibility, component registration/unregistration/destruction, target setup,
builder submission and the complete native GPU readback adapter. Existing native
material getters with ConditionalPostLoad remain excluded from plugin calls.
Setter-internal engine notifications remain engine-owned; no new plugin render
callable or callback format is introduced.

Supporting UE source: MeshComponent.cpp:63 (setter), SceneComponent.cpp:1962
(world transform), SceneCaptureComponent.cpp:344 (AddUnique), and
SceneCaptureRendering.cpp:628/638 (separate component/actor show-only sources).
Installed evidence takes precedence over source descriptions.

## Eligibility and source ownership

The normal Client Release property `MiniMapStageR25bStaticAssembly` enables a
dedicated client-only experimental gate alongside R2. R1/Stage C defaults and
Server behavior are preserved. No command-line override is needed.

The existing one-shot R2.5a audit builds an additional bounded CPU recipe while
its validated selected-class/CDO/SCS/mesh/material roots are still held. Only a
selected Smelter high-detail SCS representation is eligible. The handoff requires:

- Complete current collector snapshot and matching world/generation.
- Verified native root and all root-relative transforms.
- Exactly ten distinct component instances and seven distinct mesh objects.
- Supported finite positive transforms, visible and not hidden inputs.
- Complete nonnull stored material mapping within 32 slots per instance and
  256 assignments overall.
- Valid stored bounds and all additional native bindings.
- No upstream invalid/truncated representation or audit inputs.

`OfferStaticAssembly` independently acquires native strong references to the
validated selected class, unique source meshes and every effective material before Context releases its local
roots. The retained bridge uses fixed arrays, copied QST/bounds and a generation
token. It retains no SCS template/component pointer. Native pointer keys are
private owning handles, never exported as reusable identities.

The legacy R2.5a JSON readiness field remains false with its existing conservative
policy and schema. Separate `experimental_static_template_eligibility` diagnostics
describe this experiment. Rejection consumes the offer; there is no per-frame
rediscovery or capture retry.

## Instances and materials

Runtime associations come from the current validated SCS assignments, never
from JSON aliases. The previous runtime evidence's expected associations are:

| Previous component alias | Previous mesh alias |
|---|---|
| o62 | o10 |
| o73 | o23 |
| o75 | o24 |
| o78 | o26 |
| o84 | o27 |
| o90 | o23 |
| o92 | o26 |
| o94 | o24 |
| o99 | o12 |
| o101 | o17 |

These aliases exist only in prior evidence and synthetic fixtures. They are not
runtime dependencies. Every instance remains distinct, including repeated meshes
and coincident translations. The selected recipe's map order is not interpreted
as correspondence with Mass instance ordinals.

Each stored slot uses a validated nonnull component override if present, otherwise
the mesh-authored material. Each effective slot is applied to the corresponding
new component through the verified setter. Thus the two repeated-mesh instances
with different slot-zero overrides remain different. An initial empty override
array is required. Final count, capacity, storage and every actual material slot
are revalidated; a mismatch prevents capture and logs instance/slot. Materials
are not edited, synthesized or loaded. Source identity validation excludes
pending load/postload/destruction flags.

## Transforms, bounds and isolation

Reference space remains the selected CDO RootComponent. R2.5a supplies complete
verified root-relative QST values. The temporary anchor is explicitly
`(0,0,200000)` in gameplay-world coordinates, with identity orientation/scale.
Only this translation is added; no player, live-building or Mass placement
transform is composed. All ten temporary components are unattached. Native
world-transform assignment receives full quaternions and nonuniform scales.
Stored location, rotation-equivalent quaternion and scale are checked afterward,
including finite-value checks and quaternion sign equivalence.

The footprint is the union of eight transformed stored-bound corners for every
instance. Local bounds, scales, output corners and aggregate dimensions must be
finite and nondegenerate. Orthographic width is 1.1 times the larger aggregate
XY dimension. Camera XY is the aggregate center; camera Z is anchor Z plus
aggregate top Z plus 1000. Camera rotation stays (-90,-90,0): forward -Z,
image right +X, image down +Y. The conservative bounds calculation does not
prove actual runtime clipping or renderer residency.

After configuration, all ten instances must register into the current world.
Registration is recorded from the verified +0x94 bit 0 and +0xA8 world slot.
The capture uses the original orthographic settings, 256x256 PF_FloatRGBA,
show flags, capture source, color/alpha conversion and capture-only visibility.
Collision, overlap, tick, navigation and the same lighting/reflection
contributions as baseline are disabled for temporary components.

Before each native show-only append, the existing prefix is validated: expected
count, Num/Max/storage, capacity at most 64 and exact index/serial membership.
The actor show-only array must be empty. Native AddUnique therefore scans at
most nine existing component entries. Final membership must contain exactly the
ten distinct registered temporary components and no actors. No gameplay
visibility is changed.

## Sequencing, retirement and failure handling

The original single-mesh R2 attempt executes first. Assembly submission waits
until its successful, noncancelled readback, callable/job retirement and UObject
cleanup finish. It then rechecks world/generation, starts one new external job
and uses a separate noncopyable fence owner with the same verified R2.5b.3
mechanism. There is one active job/target/capture at a time; no concurrent
scheduler or second GPU pipeline exists.

The bounded component ledger records allocation, registration and membership
for every instance. All static components are created before the capture
component. Cleanup destroys the capture, then static components in reverse
creation order, drops target/additional asset roots and finally source bridge
roots. Never-registered components are not unregistered. Detachment and release
are idempotent in the ledger.

| Failure/lifecycle point | Behavior |
|---|---|
| Source/material/transform/bounds rejection | No capture; held bridge roots released. |
| Partial construction/configuration/registration/membership | Cleanup knows actual allocated and registered entries; no builder commitment, no retry. |
| Setup exception before commitment | Assembly-only guard runs existing pre-capture cleanup. |
| Committed, native readback never enqueued | Existing strict eligibility, detach all ten components, insert one RHI-depth fence, poll nonblockingly, release only after all existing ownership predicates pass. |
| Readback/callback/GPU copy pending | Hold every component, mesh/material and target until original adapter completion permits cleanup. |
| World transition | Pending unused bridge cancelled; active set unregisters before EndPlay. Retained GPU/capture obligations continue through existing service without stale-world access. |
| PNG failure | Log output failure; native cleanup continues. |
| Native enqueue/map/backend/retirement uncertainty | Existing quarantine retained; no speculative destruction. |
| Inconsistent component world association or stale world with registered entries | Stop and retain the bounded owner; no cleanup through an uncertain world. |

The native adapter, monotonic enqueue latch, callback accounting, mapping rules,
GPU readiness gate and RHI-depth recovery are unchanged. The RHI fence proves
submission ordering, not GPU idle. Engine deferred destruction remains the
physical resource-release mechanism. No wait/flush/extra capture is added.

## Outputs and verification

New outputs, relative to the DLL directory:

    MiniMap\Cache\Buildings\Smelter_StaticAssembly.png
    MiniMap\Cache\Buildings\Smelter_StaticAssembly_Alpha.png

Original single-mesh image names and both representation/audit JSON outputs are
preserved. No new manifest is added; bounded logs report eligibility, per-instance
material counts/overrides, framing, submission, readback/output failures and
cleanup. Existing JSON schemas and CPU PNG/alpha conversion remain unchanged.

Normal x64 Client Release and Server Release builds pass. Normal Client output:

    bin\x64\Client Release\plugins\MiniMap.dll
    bin\x64\Client Release\plugins\MiniMap.pdb

Nine offline suites pass: R1 pixels, RGBA16F pixels, WIC PNG, representation
metadata, representation selection, assembly recipe, capture retirement, new
static-assembly capture tests and building geometry. New tests cover the ten/
seven fixture, repeated instances/distinct overrides, invalid inputs, rotated
nonuniform bounds, quaternion sign/anchor checks, membership and every bounded
partial allocation/registration cleanup count. Existing retirement tests cover
callbacks, no-enqueue recovery, uncertainty and world/cancellation policies.
These are synthetic policy tests, not native rendering or GPU ordering proof.
The R2.5b.3 exceptional recovery branch remains unexercised at runtime; this
stage adds no runtime verification of it or of native world transitions.

Independent Python parsing passes for three synthetic JSON documents and Pillow
decodes all WIC test PNGs. Installed representation, fence and new assembly ABI
checks pass. Four relevant production COFF objects in both configurations have
no CRT construction section, defined dynamic initializer or UnifiedError
dependency. Server objects/DLL contain no client capture implementation/output
strings; Client retains both old and new filenames. `git diff --check` passes.
QA logs and temporary tools are outside the repository in
`C:\Modding\StarRupture\MiniMapR1-Analysis\R25b-Offline`.

## Readiness and limits

Confirmed: implementation, native ABI/fingerprint checks, offline suites and
normal builds. Runtime capture of this assembly has not been performed. The
Client build is ready for a separately authorized controlled runtime test with
an eligible nearby Smelter and complete published snapshot.

Conditional fidelity: authored template state can differ from initialized live
actors; effective inherited/cooked selection, runtime changes and shader/texture/
LOD residency remain unmeasured. The fixed anchor samples gameplay lighting at
its own location. Skeletal geometry and animation are omitted. AABB framing is
conservative input geometry, not evidence that every intended surface rendered.
No complete Smelter or four-direction production sprite is claimed.

Caps: expected ten/static and seven/unique, hard cap sixteen; 32 slots/instance,
256 assignments total; one offer, one assembly submission/readback/image pair;
no scans or retries per frame. Invalid, unsupported, incomplete or stale inputs
fail closed. Uncertain asynchronous states remain quarantined.

ModLoader shutdown/DLL-unload behavior is a known, user-deferred issue and was
intentionally excluded from this implementation.
