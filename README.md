# Lost Odyssey HD Remaster

**A native PC build of Lost Odyssey (Xbox 360), produced by static recompilation of the original game code. Not an emulator.**

*[Léeme en español](README.es.md)*

![Lost Odyssey HD Remaster](media/hero.png)

> **Status:** in development · playable · source not public yet · no downloads
> This repository is a progress window, not a release. See the [FAQ](docs/faq.md).

---

## What this is

Lost Odyssey shipped for the Xbox 360 in 2007 and never came to PC. This project turns it into a real PC executable.

Static recompilation translates the game's original PowerPC machine code, function by function, into x86-64 source code, which is then compiled into a normal Windows binary. There is no CPU emulation at runtime, no interpreter, no JIT: the game's logic runs as native code on your processor. Only the parts the game asks of the console — the GPU command stream, the filesystem, saves, audio, controller input — are reimplemented on the host.

The practical difference is that the game stops behaving like a console game running under emulation and starts behaving like a PC game. It can be modified. Its renderer can be changed. Its resolution is not fixed by what fits in a 2007 GPU's memory.

Built on the [ReXGlue](https://github.com/rexglue/rexglue-sdk) recompilation SDK (0.10.0), with a heavily modified fork of its Xenos GPU plugin.

---

## Current status

| | |
|---|---|
| **Boots and plays** | Yes — main menu, saves, achievements, cutscenes, long sessions |
| **Known crashes** | **Gone** — the three this game is known for under emulation, and the port's own |
| **Renderers** | Direct3D 12 and Vulkan, both in one plugin, switchable in-game |
| **Resolutions** | Seven presets from 720p to 4K, on both renderers |
| **Interface at every resolution** | Exact — same layout as the console, drawn at the chosen resolution |
| **SMAA** | Working on both |
| **Texture replacement** | Working on both |
| **Settings in the game's own menu** | Working |
| **All four discs** | Disc changes handled automatically — extracted folders, ISO or Games on Demand |
| **Startup** | Seconds, once a resolution has been used once |
| **Linux** | Not built yet — the SDK supports it, including arm64 |
| **Android** | Not supported by the SDK |

Honest caveat: "playable" means it boots, runs, saves and has been played for extended sessions, including real disc changes into discs 2 and 3. Every known crash is fixed, but the game has not been verified start-to-finish across all four discs, so there may be others nobody has met yet. The in-between presets (900p, 1620p, 1800p) are the newest work and have only been lightly tested.

---

## The known crashes are gone

Anyone who has played Lost Odyssey under emulation knows the list: three places where the game hangs or crashes, with workarounds passed around in guides — drop to 30 fps here, do not skip this cutscene, do not ram that door twice. **None of them happen in this port, at 60 fps, with no workaround.**

| Known problem | In this port |
|---|---|
| **Grand Staff prison cell** (disc 1) — crash when talking to Jansen after the dream, or when ramming the cell door more than once | **Does not happen.** Both triggers tested at 60 fps, door rammed repeatedly with two characters, no cutscene skipped. |
| **Cutscene after the first boss** (disc 1) — crash unless the game is dropped to 30 fps | **Fixed.** It did crash here at first; the cause was found and removed. Validated three runs out of three at 60 fps. |
| **Frozen train** (disc 3) — the game freezes during the train sequence | **Does not happen.** The train section was played through in a 41-minute session with no freeze. |

The port's own problems of the same kind are fixed too:

| Problem | Status |
|---|---|
| Crash after about 27 minutes of play | **Fixed** |
| Memory leak that ended any session after 4 to 20 minutes | **Fixed** — 26 minutes of play with the affected memory flat |
| Crash when listing saves copied from an emulator | **Fixed** — missing save headers are rebuilt at startup |
| Interface out of place at 1080p (tutorial boxes, callout lines, animated text) | **Fixed** — the interface is now exact at every resolution |
| One to two minutes of shader compilation on every launch | **Fixed** — compiled once per resolution, then kept on disk |
| Dotted bars and thin lines at 900p and 1620p | **Fixed** |

If the port does crash, it writes a report that names the original Xbox 360 function it was in. [How the hardest two were found →](docs/technical.md#13-two-crashes-worth-writing-down)

---

## What it adds over the Xbox 360 release

### Any resolution, with the interface exactly where it belongs

The 360 renders Lost Odyssey at 1280×720 because that is what fits in the console's 10 MB of EDRAM — colour and depth buffers together.

The port leaves the game believing it is still rendering 720p, and scales every draw it makes — scene, HUD, menus, text — inside the GPU plugin. The scale goes in quarter steps, so it does not have to be a whole number: 1080p is ×1.5, not a 1440p image shrunk down. You pay for the pixels of the resolution you chose and nothing more.

| Preset | Internal render | Scale |
|---|---|---|
| 720p | 1280×720 | ×1 — original console mode |
| 900p | 1600×900 | ×1.25 |
| **1080p** | 1920×1080 | ×1.5 |
| 1440p | 2560×1440 | ×2 |
| 1620p | 2880×1620 | ×2.25 |
| 1800p | 3200×1800 | ×2.5 |
| 4K | 3840×2160 | ×3 |

On top of the preset: 1×/2×/3× SSAA, post-process antialiasing (FXAA, FXAA extreme or SMAA 1x), and a presentation filter (bilinear, CAS or FSR).

Because the game's own canvas never changes, the interface is correct by construction: dialogue boxes, tutorial panels, target callouts and animated text sit exactly where the console put them, only sharper.

This replaced an earlier approach. The first version made the game itself render a 1920×1080 frame, which meant enlarging the emulated EDRAM eightfold and rewriting the game's 2D projection draw by draw. It worked for most of the game and never quite closed the interface — a tutorial box spilling off-screen here, a callout line pointing at nothing there. It was retired, and the code removed. [Both approaches, and why the second one won →](docs/technical.md#1-rendering-above-720p)

### Fast startup

Xbox 360 emulation translates the game's shaders and asks the graphics driver to compile about two thousand pipelines before the first frame. When the driver's own cache misses, that takes one to two minutes — on every launch, and every time the resolution changes.

The port now keeps the compiled pipelines on disk, as modern PC games do. The first launch at a given resolution still compiles them. After that, startup takes a few seconds. [How →](docs/technical.md#9-keeping-compiled-pipelines-on-disk)

| Measured on an RTX 3080 | First launch | Later launches |
|---|---|---|
| Direct3D 12 | about 2 min | about 3 s |
| Vulkan | about 1 min | about 7 s |

### SMAA

Subpixel Morphological Antialiasing — the reference implementation, unmodified — running as three compute passes on the final frame, in both renderers. Cleaner edges than FXAA, without FXAA's softening of the whole image. [How it fits in →](docs/technical.md#5-smaa-on-the-final-frame)

TAA was considered and deliberately left out for now: done properly it needs per-shader camera jitter, the scene before the HUD, depth, history and motion vectors.

### Settings inside the game's own menu

Open the game's **Configuration** screen and press **RB**. Next to the original page there are four new tabs — **Graphics**, **Patches**, **Extras** and **Textures** — that look as if they shipped with the game, because they are drawn with the game's own font, metal panels and cursor.

Those assets are read at runtime from your own copy of the game. Nothing from the game is part of this project.

It works like the native page: up and down to move, left and right to change a value, **LB/RB** to switch tabs, **B** to go back to the game's own options. Changes that can apply immediately do. Those that need a restart are saved, and the page offers to restart the game — pressing A twice, so an accidental press never costs you unsaved progress. [How the tabs are built →](docs/technical.md#7-new-menu-pages-that-look-native)

The old **F2** overlay still exists during development and is on its way out.

### Game patches, toggleable at runtime

The community patches from Xenia Canary (original patch work by **boma**) are reimplemented as recompiler hooks rather than byte patches, so each one is a switch you can flip while playing:

60 fps · character flicker fix · disable occlusion queries · post-process upscale fix · disable depth of field · disable motion blur · 16× anisotropic filtering · disable dynamic shadows

### No random encounters

An optional toggle that stops random battles while you explore. Scripted fights — bosses, story battles — are untouched.

It does not edit any game data. The port found the one native function that counts your steps towards the next encounter, and holds its counter back while the option is on. [How it was found →](docs/technical.md#10-finding-the-random-encounter-check)

### Save anywhere

An optional toggle that enables **Save** in the System menu away from save points. It uses the game's own save flow — the same slot screen, the same save files — instead of faking a save point.

Meant for exploration. The game was never designed to be saved in the middle of an event or a cutscene, so that is best avoided.

### Four discs, no swapping

Lost Odyssey spans four discs, and asks for the next one as the story moves on. On the 360 the console handles that. Here the port does: when the game asks for a disc, the port finds it, mounts it and lets the game continue. No prompt, no menu.

Discs are recognised by the header of their own executable ("disc N of 4"), so file and folder names do not matter. The intended layout is one folder per disc next to the executable:

```
Lost Odyssey\
├── lostodyssey.exe
└── data\
    ├── disc1\    default.xex, LO.fpi, xenon_*.fpd ...
    ├── disc2\
    ├── disc3\
    └── disc4\
```

Extracted folders are the recommended form, but **ISO images** and **Games on Demand** packages work as well, read where they are without extracting or copying anything, and so does pointing at a disc's `default.xex`. If the disc the game asks for cannot be found, the port says so and waits, as the console would, so it can be added without closing the game. [How disc changes work →](docs/technical.md#8-four-discs)

### Texture replacement

Replace any texture with a PNG and reload the pack in place with **F7** — no restart, no repacking.

Textures are matched by a hash of their contents rather than by memory address, so a pack keeps working across sessions and save files.

There are two ways to get the originals. The port can dump textures as the game uses them. Or it can read all four discs directly and write out every texture in the game — more than sixteen thousand of them, in about a minute and a half — without visiting a single area. The names it writes carry the same hashes the pack uses, so a whole pack can be prepared offline. Colour textures, normal maps and lightmaps go to separate folders. [How →](docs/technical.md#11-every-texture-without-playing-the-game)

### Sharp text

The game's fonts are texture atlases drawn for a 720p screen, and they look it at higher resolutions. Upscaling them makes them bigger, not cleaner.

Instead, the port's tooling identifies the typeface each atlas was made from, fits its size, weight and outline to the original glyphs, and redraws every glyph from the vector outlines at four times the resolution. Same letters, same positions, same metrics — drawn again rather than enlarged. [How →](docs/technical.md#12-rebuilding-the-fonts-instead-of-upscaling-them)

### DualSense button prompts

The game's button glyph atlas is one of those replaceable textures, so the on-screen prompts can show PlayStation glyphs instead of the Xbox ones the 2007 release hardcoded. No patching, no separate build — it ships as part of the texture pack.

### Turbo

Fast-forward from 1.5× up to 8×, as hold or toggle, bindable to a controller button (**F6** on keyboard). Useful for a 2007 JRPG's long corridors and battle animations.

---

## Screenshots

### 720p vs 1080p

The same spot, two presets. On the left, the console's 1280×720. On the right, the same frame rendered at 1920×1080 — scene, minimap and text alike.

| 720p — original console mode | 1080p |
|---|---|
| ![720p](media/comparison-720p.png) | ![1080p](media/comparison-1080p.png) |

### The interface, exactly where it belongs

Battle screens are where a resolution patch usually gives itself away: a target box whose line points at nothing, a menu a few pixels off its panel. Here the callout line lands on the enemy and every panel sits where the console put it, because the game's own canvas is never touched.

![Target info with its callout line](media/target-info.png)

![Battle menu](media/battle-menu.png)

### Sharp text

The same line of dialogue, enlarged. Above, the game's original font atlas. Below, the same font redrawn from vector outlines at four times the resolution.

![Original font above, redrawn font below](media/fonts-comparison-zoom.png)

### Settings inside the game

The game's Configuration screen with the port's tabs. The font, the brushed-metal panels, the cursor and the layout are the game's own, read from its data at runtime; the pages are new.

The Graphics tab, with the seven resolutions:

![Graphics tab inside the game's own Configuration screen](media/in-game-settings.png)

The Extras tab — save anywhere, random battles and turbo:

![Extras tab](media/extras-tab.png)

### DualSense button prompts

The game's own settings screen, with PlayStation glyphs in place of the Xbox buttons the 2008 release hardcoded.

![DualSense glyphs](media/dualsense-glyphs.png)

### Title screen

The "HD Remaster" subtitle is not in the original game. It is a replaced texture — the texture pack at work on the very first thing you see — and it doubles as a way to tell at a glance which build you are running.

![Title screen](media/title-screen.png)

### The F2 overlay

The development overlay that came first: patches, turbo, cheats and texture tools in one panel. Now that the settings live in the game's own Configuration screen, it is being retired.

![F2 options overlay](media/options-menu.png)

---

## Documentation

- **[Technical notes](docs/technical.md)** — rendering above 720p (the native-resolution attempt, and the fractional render scale that replaced it), SMAA, menu pages built from the game's own assets, the four discs, the pipeline cache, the encounter toggle, texture dumping, the fonts, and two crashes worth writing down.
- **[Progress log](docs/progress.md)** — what changed and when.
- **[FAQ](docs/faq.md)** — including where the source is and why, and what you will need to play.

---

## Credits

Built and maintained by **[FaliGame](https://github.com/FaliGame)**.

Standing on other people's work:

- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)** — the static recompilation SDK this port is built on, and the Xenos GPU plugin this project forks.
- **[Xenia](https://xenia.jp/)** — the emulator whose GPU research underpins essentially all Xbox 360 graphics work, this project included. The plugin's compute shaders are built from Xenia's shader sources, under their BSD licence.
- **boma** — the original Xenia Canary patch set for Lost Odyssey, reimplemented here as runtime hooks.
- **re:Blue** — the Blue Dragon recompilation, which showed how a finished port on this SDK should look.
- **[SMAA](https://github.com/iryoku/smaa)** — by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez; used unmodified under its MIT licence.
- **[lzokay](https://github.com/jackoalan/lzokay)** — LZO decompression (MIT), used to read the game's textures.
- **[stb](https://github.com/nothings/stb)** — PNG writing (public domain), used by the texture dump.

---

## Legal

This repository contains **no game code, no game assets, and no executables** — only documentation and screenshots.

The in-game settings read the game's font and menu textures from the player's own copy at runtime; none of them are stored in this repository or in the port.

Lost Odyssey is © Microsoft / Mistwalker / Feelplus. This is an unaffiliated, non-commercial preservation and porting effort. Nothing here will ever distribute the game: any future release would require you to supply your own legally obtained copy.

Documentation in this repository is © its author. All rights reserved.
