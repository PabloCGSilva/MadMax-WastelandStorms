# Mad Max – Wasteland Storms (v0.9.1-beta2)

Storms in Mad Max only roll in once every 2 to 2.5 hours of play, so most
sessions never see one. **Wasteland Storms** lets you choose how often they
come.

It uses the game's own storm command, so every storm is a real one: the same
sandstorms and thunderstorms, the same debris, the same loot in the eye of the
storm. The game's own storm cycle keeps running underneath, untouched.

**This is a beta.** Back up your saves before trying it.

## What's new in beta2

* **Steam fix.** On Steam the previous version found none of the game functions it needs: the Steam executable keeps its code encrypted until the game starts (Steam's DRM), and the mod was looking too early. It now waits for the game to start before looking. Tested by simulating that startup on the GOG version; Steam players, please send `scripts\WastelandStorms.log` if anything goes wrong.

## Presets

Press **F5** in game to cycle through them. The choice is shown on screen and
remembered for next time.

| Preset | Clear sky between storms |
|---|---|
| **Off** | vanilla only |
| **Frequent** | 4–9 minutes |
| **Uncommon** | 10–30 minutes |
| **Random** | 0–60 minutes, storms can come back to back |

## How it behaves

* **Gaps start when a storm actually ends**, not when it was triggered, so
  storms never pile up on each other. The mod reads the game's weather state
  to know when a storm is over your head and when the sky has cleared.
* **Only game time counts.** Pausing, menus and alt-tab don't bring the next
  storm closer.
* **It waits for you to be outside.** Inside strongholds, interiors, camps and
  tunnels the game never raises a storm, and Gastown and the sulfur pits have
  their own atmosphere. While you are in any of those, the countdown to the
  next storm is on hold and resumes when you walk out.
* **You can outrun a storm.** A storm is a moving front: it reaches a waiting
  player about 40 seconds after it forms and lasts about 5½ minutes. Drive
  away fast enough and it never reaches you. If a storm hasn't reached you
  after two minutes it is counted as missed and the next one is scheduled
  normally — storms are never fired on top of each other.
* **The game picks the storm type** (sandstorm or thunderstorm).
* **No game files are changed** and saves are unaffected.

The on-screen message uses the game's own UI font, read from your game's
archives at startup (no game file is shipped with the mod). It appears in the
open world only, never inside a stronghold.

## Requirements

* Mad Max (2015, PC).
* [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)
  by ThirteenAG — a copy (`dinput8.dll`) is included.

## Install

1. Copy `dinput8.dll` into the game folder, next to `AVAMain.exe`
   (skip if you already use the ASI loader, e.g. for Enhanced Convoys).
2. Copy the `scripts` folder into the game folder, so you end up with
   `<game folder>\scripts\WastelandStorms.asi` and `WastelandStorms.ini`.
3. Start the game and load a save. Once you are outside, a message shows the
   current preset.

To uninstall, delete `scripts\WastelandStorms.asi` (and the `.ini` / `.log`
next to it).

## Settings

`scripts\WastelandStorms.ini`:

```ini
[WastelandStorms]
Preset=Frequent        ; Off, Frequent, Uncommon or Random
Hotkey=0x74            ; key that cycles the presets (0x74 = F5, 0x75 = F6, ...)
StartupNotice=1        ; 0 hides the message shown when you enter the world
```

## Compatibility

* Works alongside **Enhanced Convoys** (both can be installed together).
* Texture, model and data mods: no conflict, this mod touches no game file.
* **GOG**: tested. **Steam**: the mod finds every game function it needs by
  scanning the running executable, so it should work, but it has not been
  tested on Steam yet. If it can't find something it disables itself and says
  so — the game stays safe to play. Please report either way, with
  `scripts\WastelandStorms.log`.

## Known limitations

* The storm type can't be chosen; the game decides.
* The permanent storm at the edge of the world (the Big Nothing) is separate
  and not affected.

## Troubleshooting

Everything the mod does is written to `scripts\WastelandStorms.log`
(the previous session is kept as `WastelandStorms.previous.log`). The first
lines say whether each game function was found.

## Building

Visual Studio 2022 (v143), x64 Release:

```
MSBuild WastelandStorms.vcxproj /p:Configuration=Release /p:Platform=x64
```

The output is `bin\WastelandStorms.asi`.

## Credits

* Reversr69, whose Cheat Engine table revealed the game's `storm.trigger`
  command.
* ThirteenAG for the Ultimate ASI Loader.
* MinHook (Tsuda Kageyu) and Dear ImGui (Omar Cornut) — see
  `THIRD-PARTY-NOTICES.md`.
* Everyone who tested and reported back.

## License

MIT for the Wasteland Storms code (`storms.cpp`, `notice.cpp`,
`gamefont.cpp`), see `LICENSE`. The bundled MinHook and Dear ImGui sources keep
their own licenses.
