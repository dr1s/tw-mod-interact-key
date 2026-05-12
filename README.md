# ![logo](https://raw.githubusercontent.com/azerothcore/azerothcore.github.io/master/images/logo-github.png) AzerothCore

# Interact Key for AzerothCore

## Overview

A server-side "interact with target" key for AzerothCore (3.3.5a). Approximates retail WoW's interact key without touching client files.

The module adds one chat command, `.interact`. When invoked, the server looks around the player for the most useful nearby target (lootable corpse, quest giver, vendor, gather node, door, etc.) and triggers the right interaction as if the player had right-clicked it.

Players bind `.interact` to a key via the in-game macro system, so a single keypress feels like retail's V/F interact key.

## About this module

This module was developed collaboratively between [Talotan](https://github.com/Talotan) and [Claude Code](https://claude.com/claude-code) (Anthropic's CLI for Claude). The C++, configuration, documentation, and CI scaffolding were drafted by Claude under iterative direction and testing against a live AzerothCore + Playerbots server. Code, design choices, and final review are the maintainer's responsibility.

## Issues and support

If you hit a bug, want a feature, or have a question, please **[open an issue on this repository](https://github.com/Talotan/mod-interact-key/issues)** in the first instance rather than reaching out elsewhere. Issue templates are provided for bug reports and feature requests.

## How to install

1. Clone this repository into your AzerothCore `modules/` folder. You should now have `mod-interact-key` there.
2. Re-run cmake to regenerate the build files.
3. Re-build your worldserver.
4. After building, copy `mod_interact_key.conf.dist` from `configs/modules/` to `configs/modules/mod_interact_key.conf` in your server's base directory (or just leave the `.dist`; the server will fall back to defaults).
5. Restart the worldserver.

## How players use it

### One-time setup, in-game

1. Open the macro UI (`/macro` or click the chat dropdown → Macros).
2. Create a new macro. Pick any icon. Macro body:
   ```
   .interact
   ```
3. Drag the macro icon onto an action bar slot.
4. Open `Key Bindings`, find the action bar slot you used, and bind a key — `V` is unused by default in 3.3.5a and is the retail interact key.

### Usage

Press the key.

- **Standing next to a vendor:** vendor window opens.
- **Standing next to a quest giver with `!` or `?`:** quest dialog opens.
- **Looking at a dead mob you tagged:** loot window opens.
- **Standing on a herb/ore/chest:** the gather/open animation triggers.
- **In front of a door:** the door opens.
- **Tab-targeted a mob and pressed interact:** you start auto-attacking it (if hostile) or open dialog (if friendly).

## Target selection rules

The module ranks candidates within range and picks the best one. Lower number = higher priority; distance breaks ties within a priority bucket.

| Priority | Target |
|---|---|
| (Always wins) | Current target, if it's a creature in range and otherwise interactable |
| 1 | Lootable corpse you (or your group) tapped |
| 2 | Quest giver (flag-based) |
| 3 | Service NPC: vendor, trainer, banker, flightmaster, innkeeper, auctioneer, repair, stable, battlemaster, spirit healer/guide, tabard designer |
| 4 | Plain gossip NPC |
| 5 | Usable GameObject: door, button, chest, binder, chair, goober, spellcaster, meeting stone, mailbox, guard post, barber chair, guild bank, fishing hole |
| 6 | Hostile creature (only if `TargetHostile = 1`) |

Targets must be within `InteractKey.Range` yards AND, by default, within `InteractKey.ConeDegrees` degrees of the player's facing direction. The current-target rule bypasses the cone check so Tab + interact always works.

## Configuration

The full set of options lives in `conf/mod_interact_key.conf.dist`. Highlights:

| Key | Default | Purpose |
|---|---|---|
| `InteractKey.Enable` | `1` | Master switch |
| `InteractKey.Range` | `5.5` | Search radius in yards |
| `InteractKey.ConeDegrees` | `180.0` | Field-of-view filter; 360 or 0 = omnidirectional |
| `InteractKey.CooldownMs` | `250` | Per-player throttle, prevents key-repeat spam |
| `InteractKey.PreferCurrentTarget` | `1` | Tab-target overrides the search |
| `InteractKey.TargetHostile` | `0` | Whether the auto-search picks hostile mobs |
| `InteractKey.AutoAttackHostile` | `1` | Start auto-attack when the picked target is hostile |
| `InteractKey.PrintFailMessage` | `1` | Tell the player if nothing was nearby |
| `InteractKey.AnnounceTarget` | `0` | Debug echo on every successful interact |

After tweaking the config, reload it with `.reload config` or restart the worldserver.

## Build

Drop the module under `modules/` next to your other AzerothCore modules and rebuild:

```bash
cd build
cmake .. -DSCRIPTS=static -DMODULES=static -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc)
make install
```

Or, for the Docker-based setup used by this server:

```bash
docker compose build ac-worldserver
docker compose up -d ac-worldserver
```

## Known limitations

- **No reticle.** Without client-side modification we cannot tell what's literally under the player's cursor. The cone filter is the closest approximation — narrow `ConeDegrees` to ~90 if you want a tighter "what I'm looking at" feel.
- **No tradeskill auto-cast on nodes.** When a player interacts with an herb/ore GameObject, the standard `GameObject::Use` path runs, which performs the same checks as a right-click: the skill check still applies. The module won't bypass skill requirements.
- **GameObject filtering is type-based, not display-based.** Decorative objects of types like `GENERIC` or `SPELL_FOCUS` are excluded by design; this is conservative and may occasionally skip a niche interactable. If you find one that should work, add its `GameObjectType` to `IsInteractableGameObjectType()` in `InteractKey.cpp`.
- **Range is server-distance.** The server's 3D distance check may differ slightly from the client's. If players report "I'm clearly next to it but it says nothing nearby," bump `Range` to 6.0 or 7.0.

## Files

- `src/InteractKey.cpp` — module logic, command handler, player-script for logout cleanup
- `src/interact_key_loader.cpp` — script loader glue
- `conf/mod_interact_key.conf.dist` — config defaults
- `CMakeLists.txt` — registers the two source files and the config

## License

GPL-2.0, matching AzerothCore.
