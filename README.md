# Interact Key for Tortoise-WoW

## Overview

A server-side "interact with target" key for Tortoise-WoW. It approximates retail WoW's interact key with an optional small client addon helper for corpse looting.

The module adds one chat command, `.interact`. When invoked, the server looks around the player for the most useful nearby target (lootable corpse, quest giver, vendor, gather node, door, etc.) and triggers the right interaction as if the player had right-clicked it.

Players bind `.interact` to a key via the in-game macro system, so a single keypress feels like retail's V/F interact key.

## How to install

### Server

1. Clone this repository into your Tortoise-WoW `modules/` folder as `tw-mod-interact-key`.
2. Re-run cmake to regenerate the build files.
3. Re-build your worldserver.
4. After building, copy `conf/tw-mod-interact-key.conf.dist` to your server's config directory as `tw-mod-interact-key.conf` (or leave the `.dist`; the server will fall back to defaults).
5. Restart the worldserver.

### Client addon (optional for corpse looting)

By default the server can loot corpses entirely on its own with `InteractKey.ServerSideAutoLoot = 1`. This works with every 1.12 client, including stock clients that do not expose `C_Loot.LootUnit`.

If you prefer the classic loot-window experience (or your players want to see the loot before taking it), enable the client addon:

**Requirements:**
- A client that exposes `C_Loot.LootUnit(guid)` (e.g. clients with the ClassicAPI plugin). The addon uses this function to initiate looting.
- If your client does **not** expose `C_Loot.LootUnit`, the addon still works, but corpse looting falls back to targeting the corpse so you can press your normal Interact key.

**Installation:**
1. Copy the `client/TWInteractKey` folder into your WoW `Interface/AddOns/` directory.
2. Restart the game client (or `/reload`).
3. You should see "TW Interact Key loaded." in chat.

## How players use it

### One-time setup, in-game

The addon creates a small on-screen button and a macro named `TWInteract` automatically. The floating button is hidden by default.

1. Type `/interact show` to display the floating button.
2. Drag the floating button onto an action bar slot (or drag the `TWInteract` macro from `/macro`).
3. Open `Key Bindings`, find the action bar slot you used, and bind a key.
4. Type `/interact hide` to hide the floating button again.
5. To move the floating button, hold Shift and drag it.
6. To reset its position, type `/interact reset`.
7. `/interact` with no arguments triggers the interaction directly (same as the macro).

### Usage

Press the key (or click the button).

- **Standing next to a vendor:** vendor window opens.
- **Standing next to a quest giver with `!` or `?`:** quest dialog opens.
- **Standing on a dead mob you tagged:** corpse is looted instantly (with `ServerSideAutoLoot`) or a loot window opens (via the client addon).
- **Standing on a herb/ore/chest:** the gather/open animation triggers.
- **In front of a door:** the door opens.
- **Tab-targeted a mob and pressed interact:** you start auto-attacking it (if hostile) or open dialog (if friendly).

## Target selection rules

The module ranks candidates within range and picks the best one. Lower number = higher priority; distance breaks ties within a priority bucket.

| Priority | Target |
|---|---|
| (Always wins) | Current target, if it's a creature or usable game object in range |
| 1 | Lootable corpse you (or your group) tapped |
| 2 | Quest giver (flag-based) |
| 3 | Service NPC: vendor, trainer, banker, flightmaster, innkeeper, auctioneer, repair, stable, battlemaster, spirit healer/guide, tabard designer |
| 4 | Plain gossip NPC |
| 5 | Usable GameObject: door, button, chest, binder, chair, goober, spellcaster, meeting stone, mailbox, guard post, fishing hole |
| 6 | Hostile creature (only if `TargetHostile = 1`) |

Targets must be within `InteractKey.Range` yards AND, by default, within `InteractKey.ConeDegrees` degrees of the player's facing direction. The current-target rule bypasses the cone check so Tab + interact always works.

## Configuration

The full set of options lives in `conf/tw-mod-interact-key.conf.dist`. Highlights:

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
| `InteractKey.AnnounceTarget` | `0` | Chat echo on every successful interact |
| `InteractKey.Debug` | `0` | Verbose server console logging |
| `InteractKey.ServerSideAutoLoot` | `0` | Loot corpses server-side instead of opening a client loot window |

After tweaking the config, reload it with `.reload config` or restart the worldserver.

## Build

Drop the module under `modules/` next to your other Tortoise-WoW modules and rebuild:

```bash
cd build
cmake ..
make -j$(nproc)
```

## Known limitations

- **Corpse looting can work without the client addon.** When `InteractKey.ServerSideAutoLoot = 1`, the server loots corpses directly. If you disable it, the 1.12 client only opens a loot window when it has sent `CMSG_LOOT` itself, so the addon (or `C_Loot.LootUnit`) is required for a visible loot window.
- **No reticle.** Without client-side modification we cannot tell what's literally under the player's cursor. The cone filter is the closest approximation — narrow `ConeDegrees` to ~90 if you want a tighter "what I'm looking at" feel.
- **No tradeskill auto-cast on nodes.** When a player interacts with an herb/ore GameObject, the standard `GameObject::Use` path runs, which performs the same checks as a right-click: the skill check still applies. The module won't bypass skill requirements.
- **GameObject filtering is type-based, not display-based.** Decorative objects of types like `GENERIC` or `SPELL_FOCUS` are excluded by design; this is conservative and may occasionally skip a niche interactable. If you find one that should work, add its `GameObjectType` to `IsInteractableGameObjectType()` in `InteractKey.cpp`.
- **Range is server-distance.** The server's 3D distance check may differ slightly from the client's. If players report "I'm clearly next to it but it says nothing nearby," bump `Range` to 6.0 or 7.0.

## Files

- `src/InteractKey.cpp` — module logic, command handler, world-script config loading
- `src/interact_key_loader.cpp` — script loader glue
- `conf/tw-mod-interact-key.conf.dist` — config defaults
- `data/sql/world/tw_mod_interact_key_strings.sql` — module string locale data
- `client/TWInteractKey/` — client addon for corpse looting
- `tw-mod-interact-key.cmake` — CMake registration

## License

GPL-2.0, matching AzerothCore.
