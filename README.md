# MiniMap

A client-side minimap plugin for **StarRupture**, built for the **AlienX Mod Loader**.

MiniMap adds a persistent, resizable HUD map so you can keep track of the surrounding terrain, exploration state, and important map markers without repeatedly opening the full in-game map.

> **Beta**
>
> MiniMap is already fully usable, but it is still under active development. Additional features and configuration options are planned for later releases.

## Features

- Live terrain minimap centered on the player.
- Heading-up map rotation: the direction you are facing stays at the top.
- Player position marker.
- Compass indicators.
- Native Fog of War support using the game's exploration data.
- Landmark markers, including caves, antennas, obelisks, abandoned bases, Forgotten Engine, and other native landmark POIs.
- Completed Abandoned Base marker support.
- Drone markers.
- Dead Body markers.
- Native map-filter integration: if a marker category is disabled on the in-game map, it is also hidden on MiniMap.
- Fog of War rules are respected for POIs and foundables.
- Adjustable zoom.
- Resizable and movable HUD window.
- Position and size are persisted by the Mod Loader UI.

## Usage

MiniMap works automatically after loading into a world.

- Press **F4** to show or hide the minimap.
- Hold **Left Ctrl** and use the **mouse wheel** to zoom in or out.
- Move or resize the minimap through the Mod Loader UI as you would any other plugin window.

No save conversion, setup process, or manual map initialization is required.

## Marker Behaviour

MiniMap intentionally follows the game's own map visibility rules where applicable.

For example:

- disabling Drone or Dead Body markers on the native StarRupture map also hides them on MiniMap;
- POIs that are hidden by unexplored Fog of War remain hidden;
- looted Drone and Dead Body markers disappear automatically when they are no longer eligible to be shown.

MiniMap reads live game state. It does not modify your save file.

## Requirements

- **StarRupture**
- **AlienX StarRupture Mod Loader 1.22.0 or newer**
- Plugin interface **v70 or newer**

MiniMap is a **client-side plugin**.

## Installation

1. Install the AlienX StarRupture Mod Loader.
2. Install MiniMap using the normal Mod Loader/Nexus Mods plugin installation method.
3. Start StarRupture and load a world.
4. Press **F4** if you want to hide or show the minimap.

## Current Beta Limitations

The current beta focuses on the core minimap experience.

Planned work includes:

- nearby hostile alien indicators;
- additional configuration options;
- further UI and usability refinements.

Alien Attack Vector support has also been investigated, but is not currently included.

## Safety and Save Compatibility

MiniMap is a display-only client plugin.

It does **not**:

- reindex entities;
- rewrite save data;
- repair saves;
- alter persistent IDs;
- modify world progression.

The plugin reads runtime game state and renders a HUD overlay.

## Development

Source code:

https://github.com/LetMeInside/StarRupture-Plugin-MiniMap

MiniMap is developed against the AlienX StarRupture Plugin SDK and uses a mixture of generated SDK access and narrowly resolved native functions where the generated SDK does not expose the required runtime behaviour.

## Reporting Issues

When reporting a problem, please include:

- your StarRupture game version/build;
- your AlienX Mod Loader version;
- whether the issue occurs in single-player, local multiplayer, remote multiplayer, or a dedicated-server session;
- what you were doing when the problem occurred;
- relevant Mod Loader log output if available.

For marker problems, it is also useful to mention whether the corresponding marker is visible and enabled on the normal in-game map.

## License

See [LICENSE](LICENSE).

Copyright (c) 2026 Kian369 (LetMeInside on GitHub)

All rights reserved.
