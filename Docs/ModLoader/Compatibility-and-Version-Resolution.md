# Compatibility and Version Resolution

## Requirements and evidence

MiniMap requires **AlienX ModLoader 1.25.0 or newer**, providing **plugin interface 78 or newer**. It cannot load with an older plugin interface.

The current client renderer uses AlienX ModLoader's `DL_AddTexturedTriangles`, introduced in **ModLoader 1.25.0 / plugin interface 78**. [Bootstrap](../../Src/Adapters/AlienX/Bootstrap.cpp) declares `PLUGIN_INTERFACE_VERSION` from the selected Plugin SDK. The [viewport adapter](../../Src/Adapters/AlienX/Viewport.cpp) requires the bulk callback; rejected submissions fail that draw rather than selecting a fallback renderer. Interface 70 is the historical texture-API baseline, not the current MiniMap requirement.

The original Game SDK targets **CL-127004**. StarRupture **Update 3 is CL-130045**. A compiled SDK type and the corresponding runtime reflected type can have different field offsets or sizes. Build-specific RVAs, offsets and PDB observations in the engine references remain historical evidence for their identified executable.

This document describes source-level compatibility handling. It does not certify every game/loader combination or report a new runtime test.

## Resolution and admission

[Bootstrap](../../Src/Adapters/AlienX/Bootstrap.cpp) declares `PLUGIN_MAP_MIGRATED` for the client and resolves required native functions and reflected contracts through ModLoader's version-map mechanism. [The generated map](../../Src/Adapters/AlienX/version_map.generated.h) is an empty plugin map identifying the selected Client Game-SDK CL. [MiniMap.props](../../Src/MiniMap.props) initializes the SDK include directories before importing the SDK's build properties, which prefer that directory's `build_cl.txt` over fallback build properties.

Required native dependencies follow one chain:

```text
Native/Fingerprints.cpp -> MiniMapFingerprints::ResolvedAddresses
  -> NativeApi::Initialize -> typed NativeApi interfaces -> consumers
```

Bootstrap resolves consumed reflected fields and class/element extents, checks their required sizes, bounds, alignment and bit masks as applicable, then publishes validated access contracts. [RuntimeFieldAccess.h](../../Src/Map/RuntimeFieldAccess.h) provides typed field, bitfield and runtime-stride access without exposing loader mapping types to map acquisition.

Resolution allows supported fields to move; it does not relax every native ABI requirement. Fixed query storage, required native layouts and other explicitly checked contracts still gate admission. Missing functions, rejected fields or incompatible extents cause initialization/admission failure. A successful pattern match alone is not proof that a native call's full ABI is compatible.

## Updating the Client SDK and generated map

MiniMap tracks the latest upstream Plugin SDK, its current Game-SDK submodule, and the latest default-branch `msantoro12/starrupture-ci` generator. None is pinned by this version-map procedure. After updating the SDK checkout and its submodule, regenerate from `Tools` using PowerShell 7 and Node.js:

```powershell
Unblock-File .\Update-VersionMap.ps1
.\Update-VersionMap.ps1 -PluginSDKRoot 'C:\Modding\StarRupture\StarRupture-Plugin-SDK'
```

Pass the actual Plugin SDK checkout used for compilation. For `Client Debug Experimental`, pass its separately configured SDK checkout instead. The helper reads `StarRupture SDK/Client/build_cl.txt`, requires a positive 32-bit decimal CL, downloads the latest generator with its supporting modules, and runs its `init --flavour plugin --built-cl <CL>` and format-only `check` commands. It independently checks the identity, plugin format, and empty collections before atomically replacing the header. Identical content is not rewritten; generation or validation failure preserves the existing header. Temporary files default to MiniMap's `Research` directory and are removed after the run. `-WorkDirectory` can select another temporary directory on the destination's volume. HTTPS access to GitHub is required. The helper logs the selected path, CL, available SDK revisions, generator revision, and whether the header changed.

The plugin-specific map intentionally carries no historical build rows or function/field candidates into a newly compiled binary. The eight former CL-127004 function candidates duplicated patterns still supplied by `Native/Fingerprints.cpp`. With no generated entry, `PluginVersionMap::Attach` leaves each request's pattern and transforms unchanged. Requests continue through ModLoader's central map and its existing pattern-resolution rules. A binding absent map row can still refuse a request, and required scans remain mandatory. Empty-map format validation does not certify scan coverage; the generator's `check --keys` coverage mode requires mapping rows and is not used for this empty map.

GitHub Actions runs the same helper after checking out the latest Plugin SDK and its referenced Game-SDK submodule, before Client Release compilation. It provides Node.js, passes the workflow's SDK path explicitly, and fails the job if regeneration fails. Each run resolves and logs the latest generator revision before downloading that revision as one coherent snapshot; the revision is not a configured pin. Existing packaging and publication stages follow unchanged.

Regeneration removes the generated-header identity mismatch without requiring a central ModLoader mapping update merely to compile or publish MiniMap. It does not prove runtime compatibility with a new game build: supplied patterns, native call ABIs, mandatory fields, SDK layouts, and loader admission requirements still need verification. Central mappings remain available to maintain compatibility of already published MiniMap binaries between game updates and later MiniMap releases.

## Foundables GameState contract

[Foundables](../../Src/Map/Foundables/Foundables.cpp) uses the resolved `UWorld::GameState` and `ACrGameStateBase::AbandonBaseInfoReplicator` offsets. Bootstrap supplies the resolved `ACrGameStateBase` class extent and verifies that the replicator member fits inside it.

Foundables resolves `/Script/Chimera.CrGameStateBase` using that runtime extent and checks its native class metadata against the resolved size. It also verifies that a live GameState's class has sufficient extent before accessing the replicator. This replaces the earlier assumption that the reflected base class must have the compiled SDK's exact size. Other actor, world and field checks remain in place; this is a bounded correction, not a general exemption from validation.

The replicated story arrays also use resolved element contracts. A validation failure is unavailable data, not evidence that an Abandoned Base has no remaining story items.

## Reporting compatibility problems

Include the game CL, ModLoader version, MiniMap version, session type, and relevant initialization or contract-failure log entries. Keep native-resolution failure, unavailable live data, and rendering/submission failure distinct. Historical timing and capture results in these references do not establish Update 3 runtime behavior.
