# Compatibility and Version Resolution

## Requirements and evidence

MiniMap requires **AlienX ModLoader 1.25.0 or newer**, providing **plugin interface 78 or newer**. It cannot load with an older plugin interface.

The current client renderer uses AlienX ModLoader's `DL_AddTexturedTriangles`, introduced in **ModLoader 1.25.0 / plugin interface 78**. [Bootstrap](../../Src/Adapters/AlienX/Bootstrap.cpp) declares `PLUGIN_INTERFACE_VERSION` from the selected Plugin SDK. The [viewport adapter](../../Src/Adapters/AlienX/Viewport.cpp) requires the bulk callback; rejected submissions fail that draw rather than selecting a fallback renderer. Interface 70 is the historical texture-API baseline, not the current MiniMap requirement.

The original Game SDK targets **CL-127004**. StarRupture **Update 3 is CL-130045**. A compiled SDK type and the corresponding runtime reflected type can have different field offsets or sizes. Build-specific RVAs, offsets and PDB observations in the engine references remain historical evidence for their identified executable.

This document describes source-level compatibility handling. It does not certify every game/loader combination or report a new runtime test.

## Resolution and admission

[Bootstrap](../../Src/Adapters/AlienX/Bootstrap.cpp) declares `PLUGIN_MAP_MIGRATED` for the client and resolves required native functions and reflected contracts through ModLoader's version-map mechanism. [The generated map](../../Src/Adapters/AlienX/version_map.generated.h) supplies build-specific resolution data; it is not a portable address table for consumers. [MiniMap.props](../../Src/MiniMap.props) derives the compiled Game SDK build identity from the selected SDK metadata or an explicit build property.

Required native dependencies follow one chain:

```text
Native/Fingerprints.cpp -> MiniMapFingerprints::ResolvedAddresses
  -> NativeApi::Initialize -> typed NativeApi interfaces -> consumers
```

Bootstrap resolves consumed reflected fields and class/element extents, checks their required sizes, bounds, alignment and bit masks as applicable, then publishes validated access contracts. [RuntimeFieldAccess.h](../../Src/Map/RuntimeFieldAccess.h) provides typed field, bitfield and runtime-stride access without exposing loader mapping types to map acquisition.

Resolution allows supported fields to move; it does not relax every native ABI requirement. Fixed query storage, required native layouts and other explicitly checked contracts still gate admission. Missing functions, rejected fields or incompatible extents cause initialization/admission failure. A successful pattern match alone is not proof that a native call's full ABI is compatible.

## Foundables GameState contract

[Foundables](../../Src/Map/Foundables/Foundables.cpp) uses the resolved `UWorld::GameState` and `ACrGameStateBase::AbandonBaseInfoReplicator` offsets. Bootstrap supplies the resolved `ACrGameStateBase` class extent and verifies that the replicator member fits inside it.

Foundables resolves `/Script/Chimera.CrGameStateBase` using that runtime extent and checks its native class metadata against the resolved size. It also verifies that a live GameState's class has sufficient extent before accessing the replicator. This replaces the earlier assumption that the reflected base class must have the compiled SDK's exact size. Other actor, world and field checks remain in place; this is a bounded correction, not a general exemption from validation.

The replicated story arrays also use resolved element contracts. A validation failure is unavailable data, not evidence that an Abandoned Base has no remaining story items.

## Reporting compatibility problems

Include the game CL, ModLoader version, MiniMap version, session type, and relevant initialization or contract-failure log entries. Keep native-resolution failure, unavailable live data, and rendering/submission failure distinct. Historical timing and capture results in these references do not establish Update 3 runtime behavior.
