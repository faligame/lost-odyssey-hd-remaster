# Features in detail

*[Leer en español](features.es.md)* · [Back to the front page](../README.md)

Everything the port does, explained. For how each piece works inside, see the [technical notes](technical.md).

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

If the port does crash, it writes a report that names the original Xbox 360 function it was in. [How the hardest two were found →](technical.md#13-two-crashes-worth-writing-down)

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

This replaced an earlier approach. The first version made the game itself render a 1920×1080 frame, which meant enlarging the emulated EDRAM eightfold and rewriting the game's 2D projection draw by draw. It worked for most of the game and never quite closed the interface — a tutorial box spilling off-screen here, a callout line pointing at nothing there. It was retired, and the code removed. [Both approaches, and why the second one won →](technical.md#1-rendering-above-720p)

### Fast startup

Xbox 360 emulation translates the game's shaders and asks the graphics driver to compile about two thousand pipelines before the first frame. When the driver's own cache misses, that takes one to two minutes — on every launch, and every time the resolution changes.

The port now keeps the compiled pipelines on disk, as modern PC games do. The first launch at a given resolution still compiles them. After that, startup takes a few seconds. [How →](technical.md#9-keeping-compiled-pipelines-on-disk)

| Measured on an RTX 3080 | First launch | Later launches |
|---|---|---|
| Direct3D 12 | about 2 min | about 3 s |
| Vulkan | about 1 min | about 7 s |

### SMAA

Subpixel Morphological Antialiasing — the reference implementation, unmodified — running as three compute passes on the final frame, in both renderers. Cleaner edges than FXAA, without FXAA's softening of the whole image. [How it fits in →](technical.md#5-smaa-on-the-final-frame)

TAA was considered and deliberately left out for now: done properly it needs per-shader camera jitter, the scene before the HUD, depth, history and motion vectors.

### Settings inside the game's own menu

Open the game's **Configuration** screen and press **RB**. Next to the original page there are four new tabs — **Graphics**, **Patches**, **Extras** and **Textures** — that look as if they shipped with the game, because they are drawn with the game's own font, metal panels and cursor.

Those assets are read at runtime from your own copy of the game. Nothing from the game is part of this project.

It works like the native page: up and down to move, left and right to change a value, **LB/RB** to switch tabs, **B** to go back to the game's own options. Changes that can apply immediately do. Those that need a restart are saved, and the page offers to restart the game — pressing A twice, so an accidental press never costs you unsaved progress. [How the tabs are built →](technical.md#7-new-menu-pages-that-look-native)

The old **F2** overlay still exists during development and is on its way out.

### Game patches, toggleable at runtime

The community patches from Xenia Canary (original patch work by **boma**) are reimplemented as recompiler hooks rather than byte patches, so each one is a switch you can flip while playing:

60 fps · character flicker fix · disable occlusion queries · post-process upscale fix · disable depth of field · disable motion blur · 16× anisotropic filtering · disable dynamic shadows

### No random encounters

An optional toggle that stops random battles while you explore. Scripted fights — bosses, story battles — are untouched.

It does not edit any game data. The port found the one native function that counts your steps towards the next encounter, and holds its counter back while the option is on. [How it was found →](technical.md#10-finding-the-random-encounter-check)

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

Extracted folders are the recommended form, but **ISO images** and **Games on Demand** packages work as well, read where they are without extracting or copying anything, and so does pointing at a disc's `default.xex`. If the disc the game asks for cannot be found, the port says so and waits, as the console would, so it can be added without closing the game. [How disc changes work →](technical.md#8-four-discs)

### Texture replacement

Replace any texture with a PNG and reload the pack in place with **F7** — no restart, no repacking.

Textures are matched by a hash of their contents rather than by memory address, so a pack keeps working across sessions and save files.

There are two ways to get the originals. The port can dump textures as the game uses them. Or it can read all four discs directly and write out every texture in the game — more than sixteen thousand of them, in about a minute and a half — without visiting a single area. The names it writes carry the same hashes the pack uses, so a whole pack can be prepared offline. Colour textures, normal maps and lightmaps go to separate folders. [How →](technical.md#11-every-texture-without-playing-the-game)

### Sharp text

The game's fonts are texture atlases drawn for a 720p screen, and they look it at higher resolutions. Upscaling them makes them bigger, not cleaner.

Instead, the port's tooling identifies the typeface each atlas was made from, fits its size, weight and outline to the original glyphs, and redraws every glyph from the vector outlines at four times the resolution. Same letters, same positions, same metrics — drawn again rather than enlarged. [How →](technical.md#12-rebuilding-the-fonts-instead-of-upscaling-them)

### DualSense button prompts

The game's button glyph atlas is one of those replaceable textures, so the on-screen prompts can show PlayStation glyphs instead of the Xbox ones the 2007 release hardcoded. No patching, no separate build — it ships as part of the texture pack.

### Turbo

Fast-forward from 1.5× up to 8×, as hold or toggle, bindable to a controller button (**F6** on keyboard). Useful for a 2007 JRPG's long corridors and battle animations.
