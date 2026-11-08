# Update notes

## 1.2.0-gss.5

- MiniMap now takes every game-specific location (offsets, sizes, patterns and functions) from the mod loader's version map, so the same DLL runs on every supported game build.
- If the map lacks an entry for a game build, only the layer that needs it turns off and the rest of MiniMap keeps running. The camera follow of the build drone is one such layer: without its entry the minimap tracks your character.
- Building pictures on a newly released game build need a mod loader whose map covers that build.
- Needs mod loader 1.23.1 or newer.
