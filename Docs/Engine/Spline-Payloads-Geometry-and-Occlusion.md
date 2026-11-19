# Native Spline Payloads, Geometry, and Occlusion

## Current implementation

[BuildingSplineSource](../../Src/Map/Buildings/BuildingSplineSource.h) copies raw Mass fragment/payload facts using validated runtime offsets and point-array stride. `BuildingCollectorV2` acquires initialized Mass curves first and uses the typed AP fallback only under its existing eligibility rules. Malformed initialized/multi-point input is not silently replaced. Native payload construction/destruction is scoped; only owned numerical values escape acquisition.

Neutral `SplineGeometry` converts keyed points to cubic Bézier segments and supports linear and constant interpolation. Constant spans do not generate bridging ribbons. Adaptive de Casteljau sampling uses a fixed reference density of 0.08 px/cm, 1 px XY error and 10 cm height error, with default depth cap 8. These are map-surface tolerances, not recovered native mesh topology.

Authored profile alternatives retain their source order. Surface projection combines sampled centreline/tangents with supported lateral bounds and transforms. Ribbon edges use centreline Z; numerical UVs use cumulative sampled XYZ distance and cross-ribbon position. The scheduler selects the first numerically drawable alternative under its existing failure and terminal-omission rules.

Current solid policies are black DroneRail, cyan `80EBDD` zipline and `1F1E1C` walkway, at full opacity. DroneRail is a **15 cm total** synthetic ribbon (7.5 cm half-width), not recovered native gauge. The retired renderer's 10 cm rail width and walkway inset bands are not the current service policy. Generic numerical textured representations do not establish an authored textured walkway feature; the GPU route uses solid tint through the shared white texture.

Ordinary source quads and capture top heights supply downward-prism occluders before spline preparation, even when the sprite contributes no drawable Section triangles. Height-aware subtraction precedes XY Section clipping and preserves attributes at cuts. It is not an alpha-aware sprite mask, native mesh intersection, terrain occlusion or spline-to-spline visibility. Failed preparation is distinct from confirmed full occlusion. Cross-Section occluder completeness and final pixel fidelity are not guaranteed by conservative admission.

Current building pass order remains ordinary sprites, splines, then overlay-last sprites, including Base Core. Section scenes retain numerical geometry; immutable GPU owners retain derived world triangles and revision-matched atlas bindings. Draw uses the current projection without acquisition, painter sorting or mutable service access. See [Map Service Architecture](../Architecture/Map-Service-Architecture.md) for publication and resource lifetimes and [Compatibility and Version Resolution](../ModLoader/Compatibility-and-Version-Resolution.md) for version boundaries.

## Historical native and renderer evidence

The remaining sections preserve CL-127004 native findings and the **earlier renderer reconciliation**. Names such as `BuildingSplinePayload`, `BuildingGeometry`, the old atlas-submission occluder rule and inset bands refer to retired source. They are retained as historical evidence, not descriptions of current V2 behavior. Native layouts/RVAs remain build-specific.

## Scope and version basis

This reference consolidates spline-specific native payload, geometry, and occlusion evidence for MiniMap maintenance and related rendering work. It documents evidence; it does not specify a new renderer or establish runtime behavior on later builds.

Native ABI evidence is primarily for StarRupture **CL-127004 Client**, executable SHA-256 b2b9188700ec64dfaf721f969063f969d5a89f8f0b9dc8b6f7135a914f3e5308, matched PDB GUID 41cbaea9-7ac3-99da-b947-a1db9c1021ed, age 1. Historical reports refer to Unreal Engine 5.6/5.6.1. These qualifications belong to their evidence only; server parity and later-build ABI are not established. Old RVAs are historical references, never binding instructions. Interface 70 is historical context, not a universal loader requirement.

Evidence labels: **CONFIRMED** = identifiable native/PDB/SDK or earlier-source evidence; **OBSERVED** = recorded runtime/log/screenshot; **INFERRED** = plausible but unproved; **UNKNOWN** = unavailable; **SUPERSEDED IMPLEMENTATION** = historical code not current authority; **REJECTED** = contradicted by inspected evidence. Report-only deductions are identified. General UObject/GC, capture-resource, and Mass identity material is cross-referenced to [Loading and World Transitions](../Lifecycle/Loading-and-World-Transitions.md), [Rendering and Asset Capture](Rendering-and-Asset-Capture.md), and [Mass Entity Identity and Building Discovery](Mass-Entity-Identity-and-Building-Discovery.md).

## Executive technical summary

- **CONFIRMED, CL-127004 Client:** UAuActorPlacementFunctionLibrary::GetSplineActorData(const FAuAPPayloadDataHandle&) returns FAuAPSplineActorPayloadData. Win64 ABI: caller result storage in RCX, borrowed const handle in RDX, result address in RAX. Historical RVA 0x0668DEA0.
- **CONFIRMED, CL-127004 Client:** typed GetValue copies the numerical point array into a separately owned value. The non-deleting destructor at historical RVA 0x0668A750 frees that array and owned path strings. Destroy once through that native function; do not shallow-copy or CRT-free it.
- **CONFIRMED, earlier source:** getter, destructor and payload StaticStruct are mandatory fingerprint resolutions distributed through NativeApi. BuildingSplinePayload uses aligned scoped storage and outputs owned numeric geometry. Collector reads Mass spline data first and uses AP payload only when a Mass spline fragment exists but its source is the specifically incomplete zero/one-point case. A missing Mass spline fragment does not trigger AP fallback in that earlier Collector path.
- **CONFIRMED:** supported two-point data uses world XYZ positions/tangents and CIM_CurveUser on keys 0 and 1. For key span h, Bezier controls are B0=P0, B1=P0+hT0/3, B2=P1-hT1/3, B3=P1. This payload/initializer rule does not limit initialized Mass curves, whose supported segments and modes are preserved.
- **CONFIRMED:** MiniMap projection transforms tracking-relative XY into screen coordinates; Z is not a screen displacement. Z remains relevant to world-space ribbon and occlusion preparation.
- **CONFIRMED, earlier source:** ordinary building atlas sprites are submitted first and become ordinary footprint/top-prism occluders; visible spline triangles follow; Base Core is submitted last. Geometric clipping is not an alpha-aware sprite mask and does not guarantee final pixel occlusion.
- **UNKNOWN:** ABI compatibility beyond the matched client, all actor-to-Mass relationships, all category mesh recipes, same-identity geometry mutation/revision, joins/caps, and exact effective material coverage.

## Native spline identity

| Domain | Evidence and meaning | Limit |
|---|---|---|
| Mass entity | Matched-client evidence identifies FAuSplineConnectionFragment inside a Mass entity. [Mass Entity Identity and Building Discovery](Mass-Entity-Identity-and-Building-Discovery.md) documents manager-scoped Index+SerialNumber identity and generation behavior. | Fragment presence, index, serial, BuildingID, save ID and actor pointer are distinct. See [Mass Entity Identity and Building Discovery](Mass-Entity-Identity-and-Building-Discovery.md). |
| UObject actor | ACrBuildingSplineActorBase and subclasses are presentation/geometry consumers in matched native traces. Matched-client identity analysis supports generic UObject slot/index/serial semantics. | It establishes no persistent spline actor key, universal actor-to-Mass association, pooling or pointer stability. Never use a raw pointer as persistent spline identity. |
| AP payload handle | FAuAPPayloadDataHandle is an engine-managed reference within the AP Mass fragment. The native assignment path increments source reference control before replacing/releasing the destination; network serialization constructs typed storage. MiniMap borrows the handle for the getter. | Opaque handle words are not MiniMap-owned and must not be manually copied or released. Payload revision is not entity identity. |
| Geometry value | Native getter returns an owning value; MiniMap then retains owned SplineGeometry numbers after destruction. | A copied value is not guaranteed current after entity removal, world transition, or same-entity mutation. |

**CONFIRMED, CL-127004 Client:** server AP payload publication enters replicated placement data; native serializers transmit spline points and associated values; client add/change callbacks assign the received AP handle into the fragment. This is a value/payload route, not transmission of a borrowed server pointer. This account is report-backed for the identified client image.

**UNKNOWN:** universal actor-to-Mass mapping. GetSplineActorData consumes an AP handle; it is not an actor lookup. This generic transient UObject identity evidence is not a persistent spline actor contract. Do not infer Mass identity from actor naming or inheritance.

## Native payload acquisition and ABI

The following is qualified to the CL-127004 Client PE/PDB. This is report-backed to the identified client image and PDB.

| Item | Confirmed contract | Qualification |
|---|---|---|
| Function | UAuActorPlacementFunctionLibrary::GetSplineActorData(const FAuAPPayloadDataHandle&) -> FAuAPSplineActorPayloadData; historical RVA 0x0668DEA0. | Static native x64 function; declaration in the generated SDK and matched-client call-site evidence. |
| ABI | Hidden result buffer RCX; const handle address RDX; result storage address returned RAX. | Do not declare as an ordinary by-value call that adds a hidden argument. |
| Value layout | FAuAPSplineActorPayloadData size 0xE8/alignment 8; Data +0x68; embedded point array +0x70. Point size 0x40; Position +0x10, Tangent +0x28. | Matched-client PDB evidence establishes the layout; the generated SDK corroborates the layout but not ownership. |
| Type metadata | Payload StaticStruct is resolved and warmed during NativeApi initialization for the typed getter. Historical initializer RVA 0x06687390. | Exact pattern and shared cache are build-specific; no later binary checked. |
| Earlier route | Required Fingerprints -> ResolvedAddresses -> NativeApi::Initialize -> typed SplinePayloadApi -> BuildingSplinePayload consumer. | Confirmed in earlier source. No optional scan, hard-coded address, fallback or consumer-side binding table. |

These are one-build ABI facts, not stable Unreal/cross-version guarantees. Dedicated-server compatibility is UNKNOWN. Historical unique-pattern checks were not rerun.

## Payload ownership and destruction

Keep these states separate:

1. **BORROWED DATA:** source fragment and FAuAPPayloadDataHandle are borrowed for the valid current Mass read scope.
2. **COPIED VALUE:** native GetValue type-checks and copies point data and other fields into a returned value.
3. **OWNED NATIVE ALLOCATION:** returned value owns its point array and path storage under native type rules.
4. **DESTROYED PAYLOAD:** non-deleting FAuAPSplineActorPayloadData destructor, historical RVA 0x0668A750, releases nested allocations but not caller result storage.

**CONFIRMED, matched native evidence:** GetValue retains source during copying, deep-copies the point array, then releases its temporary source reference. Missing/mismatched type yields a default empty value. A returned value is not necessarily a renderable curve.

**CONFIRMED, earlier source:** BuildingSplinePayload.cpp uses non-copyable/non-movable 8-byte-aligned 0xE8 storage. It marks construction only after the getter returns and invokes the native destructor once on scope exit, including empty results and conversion early returns. It copies numbers before destruction. Pre-call failures do not destroy unconstructed storage. Collector does not retain the payload or fragment pointer.

**UNKNOWN:** complete native exception/allocation-failure behavior during partially completed result construction. Do not assume destructor idempotence, ownership transfer from a shallow structure copy, or permission to free nested arrays with CRT.

See [Loading and World Transitions](../Lifecycle/Loading-and-World-Transitions.md) for general UObject GC and [Rendering and Asset Capture](Rendering-and-Asset-Capture.md) for native capture/readback resource lifetime; these are separate contracts.

## Spline point and segment representation

The matched client PDB reports these layouts:

- FAuAPSplinePointData size 0x40: Index +0x08, Position +0x10, Tangent +0x28.
- FAuAPSplineData size 0x50: point array +0x08, SplineLocationWS +0x18, SplineRotationWS +0x30, SplineRealLength +0x48.
- FAuSplineConnectionFragment size 0xE0: Data +0x00, StartEntity +0x50, EndEntity +0x60, SplineCurves +0x70, initialized +0xD8.

**CONFIRMED, native InitSplineCurves, historical RVA 0x0668DEC0:** for an uninitialized fragment it requires exactly two source points, uses array order rather than Index, assigns keys 0/1 and mode 3 (CIM_CurveUser), copies Position and identical arrive/leave Tangent, updates curves, then marks initialized. This is a report-backed finding for the identified client image. This is not a universal limit on initialized Mass curves.

Earlier BuildingGeometry::CopySpline walks adjacent initialized Position points, preserves supported keys/modes and applies key span h to cubic tangents. Linear and Constant remain distinct; Constant is discontinuous, not bridged. Looped curves, malformed arrays, invalid keys, unsupported modes, nonfinite and oversized geometry are explicit failures. StartEntity/EndEntity and AP Start/End helper fields do not prove curve point order or a universal connection graph; MiniMap does not currently join them.


## Geometry interpolation and coordinate transforms

For a cubic segment on keys [k0,k1], h=k1-k0, u=(k-k0)/h, endpoint positions P0/P1, leave tangent T0 and arrive tangent T1:

    C(u) = (2u^3-3u^2+1)P0 + (u^3-2u^2+u)hT0
         + (-2u^3+3u^2)P1 + (u^3-u^2)hT1
    B0=P0; B1=P0+hT0/3; B2=P1-hT1/3; B3=P1

For supported AP/uninitialized two-point data h=1. The formula is backed by matched InitSplineCurves/evaluation bodies and reconciled against BuildingGeometry.h. MiniMap's sampling is not claimed bit-identical to Unreal arc-length reparameterization.

**CONFIRMED, CL-127004 native coordinate getter:** GetLocationAtSplineInputKey evaluates stored Position curve. World argument 1 returns that value directly; Local converts using SplineLocationWS and SplineRotationWS. Do not apply those transforms or the Mass entity transform again to already-world-space centerline points. Reported local conversion is q^-1(C_world-L); direction conversion omits translation. This is a report-backed coordinate contract for the identified client image.

**Limit:** centerline world coordinates do not prove how entity FTransformFragment relates to profile cross-section, mesh frame, offsets or spline-mesh component transform. The earlier envelope supports a bounded set of validated profile cases; unsupported coherent profiles remain unsupported. Full native segment construction was not reconstructed.

## Walkway geometry

**CONFIRMED native provenance, CL-127004 Client:** ACrModularRampWalkway::CreateSplineMeshesBatch (historical RVA 0x074E4980) uses SplineMesh, desired/actual section length, section count and local spline position/tangent samples. It normalizes and clamps tangent magnitude to section length before configuring sections. SDK/PDB fields include LadderAngle, StairsAngle, narrow flag, DesiredSectionLength, SectionLength and section count. This is a report-backed native provenance finding for the identified client image.

**CONFIRMED, earlier MiniMap:** validated Walkway ID is distinct from RailingWalkway. Both are in broad Walkways category; only exact Walkway receives earlier inset bands. Earlier body color is 0x1F1E1C, inset 0x080808; two side bands each use 5% of ring width. These are MiniMap presentation choices, not native mesh facts.

**UNKNOWN:** complete Blueprint-selected ladder/stair/narrow construction, topology, joins/caps, final material and live overrides. CDO/template defaults do not establish every live section. RailingWalkway is separate in native inheritance evidence; do not assume its path matches.

## Rail geometry

Here “rail” means DroneRail T1–T5, distinct from ZipRail and ordinary Walkway.

**CONFIRMED native provenance, CL-127004 Client:** ACrDronePathPointConnection::BuildSplineMesh/CreateSplineMeshesBatch (historical RVAs 0x07635E50/0x07638680) use desired segment length, spline distance/location/tangent sampling, segment-start state and developer RailMeshVisualExtraLength. T1–T5 inheritance does not prove identical tier assets/settings. This is report-backed native provenance for the identified client image.

**CONFIRMED, earlier MiniMap:** exact DroneRail definitions use a synthetic 10 cm full-width representation (half-width 5 Unreal units) and black representative color. This is MiniMap policy, not recovered native gauge. Toggle and styling are exact-ID policy.

**UNKNOWN:** faithful segmentation, endpoint extension/caps/joins, tier-specific mesh widths/materials, and live overrides. The reported extension value is an input, not a complete endpoint formula. Historical radial mesh bounds were a conservative envelope, not a measured rendered width.

## Zipline geometry

“Zipline” includes several building IDs and systems. Earlier category also includes supports, old ZipLine, height variants and ZipRail; category alone does not imply a record owns a spline fragment or is a cable connection.

**CONFIRMED:** ACrZipRail derives from ACrModularRampWalkway in inspected SDK and matched vtable evidence selects walkway batch construction. This does not prove its Blueprint profile/material equals a Walkway. BP_Zipline supports/endpoints and the old Niagara connection system are distinct candidates; a travel proxy is not proof of rendered cable. Matched-client inheritance/vtable evidence establishes the construction route; Blueprint profile and material correspondence remain unknown.

**CONFIRMED, earlier MiniMap:** ZipRail keeps its own ID/color policy (0x80EBDD), native profile treatment and the common curve/occlusion path. It does not inherit DroneRail's synthetic width. Centerline Z is retained for world-space clipping; map projection drops it.

**OBSERVED, one uncontrolled historical screenshot session:** screenshots showed partial cyan route visibility and dark branches, but positions/orientations were uncontrolled and no Mass identity, source completeness, Z, zoom, or pixel-to-section mapping was recorded. These are symptoms, not causal proof.

**UNKNOWN:** which zipline system supplies a given route, topology, sag, actual cable profile, height frame, deformation/material, joins, or support association. Do not conflate elevated ziplines with ground infrastructure or infer a cable component from names.

## World-to-map projection

MapTransform::WorldToScreen computes:

    d = (worldX, worldY) - (TrackingWorldX, TrackingWorldY)
    r = (cos(theta)*dx - sin(theta)*dy, sin(theta)*dx + cos(theta)*dy)
    screen = (CenterX, CenterY) + PixelsPerWorldUnit*r

PixelsPerWorldUnit is 1/(100*MetersPerPixel), converting Unreal centimetres to map metres/pixel. Z is not projected. XYZ survives earlier profile/ribbon and occlusion preparation. UI conversion uses float screen coordinates and viewport clipping; tiny projected triangles can collapse/reject.

Native centerline coordinates and MiniMap presentation coordinates are separate. Entity/profile transforms are relevant to cross-section construction, not a second transform for the native world-space centerline.

## Spline clipping and building occlusion

**Earlier algorithm, CONFIRMED in source:** only successfully submitted ordinary building atlas quads enter Request::Ordinary. Their transformed world quad gives an XY footprint; the maximum Z of transformed assembly-bounds corners supplies one constant TopZ, forming a downward prism. This is not a native mesh surface, per-pixel depth/alpha map, terrain occluder, or spline-to-spline visibility system.

For each input spline triangle, the planner applies AABB broad phase then clips against four XY half-planes of the convex footprint and the height half-plane z <= TopZ. Equality is hidden. On a crossing edge with signed distances da/db, t=da/(da-db), and each coordinate is interpolated; cut Z is set exactly to TopZ. There is no intentional epsilon, height bias, padding or area threshold. Positive-area fragments survive. Multiple occluders successively subtract from remaining fragments. Numerical/allocation failure is not evidence of full occlusion.

Earlier walkway inset bands are clipped from already visible body and remain body-owned. A record is FullyOccluded only when every section completed successfully and has no visible triangle; planner failure is distinct.

Distinguish:

- **Geometric intersection:** XY overlap and Z/half-plane relation under the prism approximation.
- **Visual occlusion:** final draw-list pixels, including transparent sprite texels, AA, fog and later layers.
- **Alpha composition:** color/alpha blending of ordered layers. Geometric subtraction does not compute translucent composition.

Flattened atlas sprites cannot recover hidden layers or surface heights. Transparent openings can be covered by the footprint prism. Bounds do not establish native mesh alignment. The earlier occlusion was an approximation for the selected prism model.

## Alpha compositing and layering

The earlier renderer used full vertex alpha for spline body and inset submissions. Its historical alpha discussion does not change the current solid policy described above.

Ordinary sprites are submitted with captured RGBA, then clipped spline bodies/insets, then Base Core. The exact Base Core canopy alpha intentionally composes over rails. That renderer used this order:

1. Ordinary building sprites, excluding Base Core.
2. Clipped spline bodies and eligible walkway insets.
3. Base Core overlay.

This order does not mean every sprite texel is opaque or that height ordering is correct. Texture transparency, AA, fog and later map overlays affect visible pixels.

**REJECTED:** reducing spline alpha to mimic occlusion under glass. A half-alpha spline drawn over an already glass-composited sprite does not equal placing the spline beneath glass. Correct translucent composition needs ordered source layers or recomposition.

## Base Core overlay ordering

The earlier renderer excludes canonical Base Core path /Game/Chimera/Buildings/BaseCore/BP_BaseCore.BP_BaseCore_C from ordinary occluders and draws it in the final building pass after splines. Source comment records the reason: captured canopy alpha composes over rails. The pass remains when spline rendering is disabled or planning fails.

This is a specific authored overlay behavior, not a universal depth solution. Historical zipline investigations preserve the final Base Core pass while discussing spline painter-order gaps. No ordering change is authorized here.

## Lifetime and invalidation

- Fragment/AP handle and native payload array are scoped to current game-thread acquisition; owned SplineGeometry has no engine pointers.
- Earlier collector records are tied to Mass identity and world/session generation. Removal or session reset discards records; replacement identity is acquired separately.
- **UNKNOWN:** same-index/serial geometry revision tracking for mutations during an entity's lifetime is not established by inspected source/research. Successful copy does not prove indefinite freshness.
- Visual/occlusion plans bind snapshot/generation, atlas session/revision and spline/ordinary inputs. UI draw retains immutable plan through submission. This is separate from native temporary lifetime and GPU/capture resource retirement described in [Rendering and Asset Capture](Rendering-and-Asset-Capture.md).
- No cross-session native pointer is retained by the spline adapter.

## Historical failures and corrections

| Historical issue or claim | Correction |
|---|---|
| An earlier investigation left network-client Zipline/Walkway payload availability unknown. | Follow-up client tracing identified serialization, AP fragment add/change assignment and typed value extraction. Earlier source now uses the typed path for incomplete Mass data. |
| The typed adapter checkpoint preceded Collector integration. | Historical checkpoint; earlier source has required binding and Collector integration. |
| Two points treated as universal spline count. | Two-point rule applies to native initializer/AP payload path. Initialized Mass curve can contain multiple segments. |
| Screen-width/radial-envelope approach implied native gauge/material fidelity. | Not a native geometry contract. Blueprint research found live section/material evidence gaps; the earlier DroneRail width was explicitly synthetic. |
| Whole-route origin-Z painter order caused missing sections. | Historical screenshot analysis identified possible local overlap where later walkway can paint over earlier cable. Screenshots did not prove causal pixels; the earlier source had no spline-to-spline resolver. |
| A proposed upper-envelope splitter was a fix. | Design only; not implemented. |
| Bounds were treated as true visible width/alpha silhouette. | Rejected: admission bound/prism are not an alpha mask or actual mesh top surface. |
| Alpha scaling substitutes for glass ordering. | Rejected: flattened composited pixels lose layer information. |
| Missing segment proves acquisition regression. | Rejected: candidate causes include incomplete source, unsupported profile/frame/cell, prism clipping, later painter order, subpixel projection, fog and overlays. The historical screenshot evidence was uncontrolled. |

## Unresolved questions and negative findings

- **UNKNOWN:** getter/destructor/layout/StaticStruct match beyond CL-127004 Client; dedicated server parity.
- **UNKNOWN:** native exception/allocation-failure ownership guarantees.
- **UNKNOWN:** actor-to-Mass mapping, pooling/reuse, payload mutation revision and all producer/restore writers.
- **UNKNOWN:** initialized curve limits outside evidenced payload wire path; full category mesh/material/join/cap/deformation rules.
- **UNKNOWN:** exact atlas alpha coverage, AA, fog and final pixel result.
- **REJECTED:** raw actor pointer as persistent identity; Mass index, serial, BuildingID, save ID and AP handle as interchangeable; shallow copy as ownership transfer; assumed destructor idempotence; second transform on world centerline; AP count limit on initialized Mass path; bounds as alpha mask; alpha reduction as glass ordering; screenshots as causal proof; the proposed splitter as current behavior.

## Historical evidence basis and current references

Native spline identity, payload ABI/ownership, matched-client layouts, historical geometry observations, and screenshot/probe results are **REPORT-BACKED** unless the text explicitly identifies earlier MiniMap source. The original raw artifacts for some screenshot/probe and native claims are unavailable for independent reinspection; the report classifications, synthetic-profile boundaries, alpha fact, and unknown causal relationships remain qualified. The current implementation notes above, rather than the retired source reconciliation, describe implemented geometry and occlusion behavior.

The permanent [Mass identity and building discovery](Mass-Entity-Identity-and-Building-Discovery.md) reference covers general entity identity and manager-lifetime distinctions. This document focuses on spline payloads, geometry, and rendering policy.
### Current source references

- [BuildingSplineSource](../../Src/Map/Buildings/BuildingSplineSource.cpp) — Native fragment/payload copying and scoped ownership.
- [BuildingCollectorV2](../../Src/Map/Buildings/BuildingCollectorV2.cpp) — Regional acquired facts and neutral Section publications.
- [BuildingDefinitionAcquisition](../../Src/Map/Buildings/BuildingDefinitionAcquisition.cpp) — Authored profiles and exact-ID policy acquisition.
- [SplineGeometry](../../Src/Services/Map/Buildings/Visuals/SplineGeometry.h) — Keyed conversion and adaptive sampling.
- [SplineSurfaceGeometry](../../Src/Services/Map/Buildings/Visuals/SplineSurfaceGeometry.h), [SplineOcclusionGeometry](../../Src/Services/Map/Buildings/Visuals/SplineOcclusionGeometry.h) — Surface projection and height-aware subtraction.
- [SectionScheduler](../../Src/Services/Map/Buildings/Scheduling/SectionScheduler.cpp), [BuildingGeometryPreparation](../../Src/Services/Map/Buildings/Visuals/BuildingGeometryPreparation.cpp) — Retained numerical preparation and omission outcomes.
- [BuildingVisuals](../../Src/Services/Map/Buildings/Visuals/BuildingVisuals.cpp), [BuildingMapGpuViewport](../../Src/Services/Map/Buildings/BuildingMapGpuViewport.cpp) — Numerical and GPU presentation consumers.
