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
| Enemies frozen in place, or "exploding" into spikes, in some battles | **Fixed** — caused by one of the Xenia patches; see below |
| A hitch on entering every area once an HD pack was installed | **Fixed** — HD textures now load in the background |
| A character briefly wearing another character's HD textures after a scene change | **Fixed** |
| Corrupted menu backgrounds and crossfades with the sharp-interface layer | **Fixed** |

If the port does crash, it writes a report that names the original Xbox 360 function it was in. If it stops drawing frames for more than a few seconds, it writes down what every thread was doing. [How the hardest two were found →](technical.md#13-two-crashes-worth-writing-down)

---

## What it adds over the Xbox 360 release

### Output resolution and 3D scale, chosen separately

The 360 renders Lost Odyssey at 1280×720 because that is what fits in the console's 10 MB of EDRAM — colour and depth buffers together.

The port leaves the game believing it is still rendering 720p, and scales its 3D inside the GPU plugin. The **Graphics** tab now has two sliders where there used to be one list of presets:

- **Resolution** — what reaches your screen: 720p, Steam Deck (1280×800), 900p, 1080p, ultrawide 1080 (2560×1080), 1440p, 1620p, ultrawide 1440 (3440×1440), 1800p or 4K.
- **3D scale** — how many pixels the 3D is rendered with, from ×1 (the console's 1280×720) to ×7, in quarter steps. The menu shows it as a percentage of your output. ×1.5 is 1080p, ×2 is 1440p, ×3 is 4K.

The quarter steps matter: 1080p is a real ×1.5, not a 1440p image shrunk down. You pay for the pixels you choose and nothing more. [How a fractional scale works →](technical.md#1-rendering-above-720p)

The old supersampling setting is gone, because it is now simply a 3D scale above your output. Post-process antialiasing (FXAA, FXAA extreme or SMAA 1x) is still there, plus a **3D sharpness** setting (off, low, medium, high).

### An interface that is always sharp

Menus, HUD, dialogue and text are no longer rendered at the 3D scale. They are drawn on a layer of their own at the full output resolution and composited over the 3D. Play at 4K with the 3D at 1080p and the text is still 4K text.

Because the game's own canvas never changes, the interface is also correct by construction: dialogue boxes, tutorial panels, target callouts and animated text sit exactly where the console put them, only sharper. [How the interface is split from the 3D →](technical.md#14-a-sharp-interface-over-a-scaled-3d)

This replaced an earlier approach. The first version made the game itself render a 1920×1080 frame, which meant enlarging the emulated EDRAM eightfold and rewriting the game's 2D projection draw by draw. It worked for most of the game and never quite closed the interface — a tutorial box spilling off-screen here, a callout line pointing at nothing there. It was retired, and the code removed. [Both approaches, and why the second one won →](technical.md#1-rendering-above-720p)

### DLSS

NVIDIA DLSS for the 3D, on both Direct3D 12 and Vulkan, on GeForce RTX cards: **DLAA**, **Quality**, **Balanced**, **Performance** and **Ultra Performance**. The interface is not touched by it; it stays on its own sharp layer.

Choosing a mode sets the 3D scale for you, and moving the 3D scale by hand turns DLSS off. The 3D never goes below the console's own 720p, so modes that would land on the same scale as a better one are greyed out — at 1080p output, Balanced, Performance and Ultra Performance would all be 720p, so only Quality and DLAA are offered. With DLSS on, SMAA and FXAA are switched off.

A game from 2007 gives DLSS none of what it needs, so the port builds it: depth read back out of the emulated EDRAM, motion vectors from the game's own camera matrices, and a sub-pixel camera jitter applied only to the 3D draws. [How →](technical.md#15-dlss-on-an-emulated-gpu)

### Ultrawide and Steam Deck

The two 21:9 resolutions and the Deck's 16:10 widen the 3D view instead of stretching it, and remove the black bars of the in-engine cutscenes. The interface stays in a centred 16:9 box, as it was designed.

Still to do: pre-rendered videos are stretched to fill a 21:9 screen, and full-screen fades only cover the centre.

### No shader stutter, and a fast start

Xbox 360 emulation translates the game's shaders as it meets them, and asks the graphics driver to compile a pipeline the first time each one is drawn. That is the hitch everyone knows: an object that appears a moment late, a stutter the first time a spell is cast.

The port does that work before you play. On the first launch it reads your discs, finds every material the game can draw, and prepares their pipelines on a screen built from the game's own fonts and textures. You choose whether to prepare **all four discs** at once or **only the part you are playing**; whatever the area being loaded needs always goes to the front of the queue, and **Enter** lets you start playing right away while the rest finishes in the background. [How →](technical.md#16-preparing-every-shader-from-the-discs)

The result is kept on disk, and since early October it is one set for every 3D scale: changing the scale no longer means compiling anything again. [How →](technical.md#17-one-pipeline-set-for-every-scale)

| Measured on an RTX 3080 | |
|---|---|
| Preparing all four discs, the first time | from about 12 seconds to about a quarter of an hour, depending on what the driver already has cached |
| Later launches, Direct3D 12 | about 3 s |
| Later launches, Vulkan | about 2 s |

### SMAA

Subpixel Morphological Antialiasing — the reference implementation, unmodified — running as three compute passes on the final frame, in both renderers. Cleaner edges than FXAA, without FXAA's softening of the whole image. [How it fits in →](technical.md#5-smaa-on-the-final-frame)

TAA was considered and deliberately left out for now: done properly it needs per-shader camera jitter, the scene before the HUD, depth, history and motion vectors.

### Settings inside the game's own menu

Open the game's **Configuration** screen and press **RB**. Next to the original page there are four new tabs — **Graphics**, **Patches**, **Extras** and **Textures** — that look as if they shipped with the game, because they are drawn with the game's own font, metal panels and cursor.

Those assets are read at runtime from your own copy of the game. Nothing from the game is part of this project.

It works like the native page: up and down to move, left and right to change a value, **LB/RB** to switch tabs, **B** to go back to the game's own options. Changes that can apply immediately do. Those that need a restart are saved, and the page offers to restart the game — pressing A twice, so an accidental press never costs you unsaved progress. [How the tabs are built →](technical.md#7-new-menu-pages-that-look-native)

The **Extras** tab also holds the language, the shader preparation choice and a restart button. The old **F2** overlay still exists during development, now with an fps counter, and is on its way out.

### Game patches, toggleable at runtime

The community patches from Xenia Canary (original patch work by **boma**) are reimplemented as recompiler hooks rather than byte patches, so each one is a switch you can flip while playing:

60 fps · disable depth of field · disable motion blur · disable dynamic shadows · **fade between scenes** (new: a clean cut instead of a crossfade that the sharp-interface layer cannot reproduce)

Some are no longer switches, because there is only one right answer: the post-process upscale fix and 16× anisotropic filtering are always on. The occlusion-query patch is gone for a reason worth knowing: it froze enemies. [Why →](technical.md#20-a-patch-that-froze-the-enemies)

### Softer shadows

The game's dynamic shadows had a hard, stepped edge and some flicker on characters. The port gives them a linear penumbra and filters the shadow map, with a softness setting.

The shadow map itself is a fixed 864×864 in the game, sized to the console's memory. A higher 3D scale is what makes shadows sharper.

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

Replace any texture with a PNG or a DDS (BC1, BC3 or BC7) and reload the pack in place with **F7** — no restart, no repacking.

HD textures are loaded in the background: an area appears with its original textures for a moment and switches to HD as each one is ready, instead of stopping the game while they load. [How →](technical.md#19-hd-textures-without-hitches)

An upscaled pack for the whole game — 8,245 textures of characters, places, objects and battles, about 32 GB in BC7 — is in testing. Like everything made from the game's data, it will not be distributed here.

Textures are matched by a hash of their contents rather than by memory address, so a pack keeps working across sessions and save files.

There are two ways to get the originals. The port can dump textures as the game uses them. Or it can read all four discs directly and write out every texture in the game — more than sixteen thousand of them, in about a minute and a half — without visiting a single area. The names it writes carry the same hashes the pack uses, so a whole pack can be prepared offline. Colour textures, normal maps and lightmaps go to separate folders. [How →](technical.md#11-every-texture-without-playing-the-game)

### Sharp text

The game's fonts are texture atlases drawn for a 720p screen, and they look it at higher resolutions. Upscaling them makes them bigger, not cleaner.

Instead, the port's tooling identifies the typeface each atlas was made from, fits its size, weight and outline to the original glyphs, and redraws every glyph from the vector outlines at four times the resolution. Same letters, same positions, same metrics — drawn again rather than enlarged. [How →](technical.md#12-rebuilding-the-fonts-instead-of-upscaling-them)

### DualSense button prompts

The game's button glyph atlas is one of those replaceable textures, so the on-screen prompts can show PlayStation glyphs instead of the Xbox ones the 2007 release hardcoded. No patching, no separate build — it ships as part of the texture pack.

### Six languages

The game's text in English, French, German, Italian, Spanish or Japanese, chosen from the **Extras** tab. The port's own menus and screens follow the same language. Voices are chosen, as on the console, in the game's own Configuration.

### Saves next to the game

Saves live in a plain `SAVE\` folder next to the executable, one folder per slot. Saves from earlier builds are moved there automatically on the first launch.

### Turbo

Fast-forward from 1.5× up to 8×, as hold or toggle, bindable to a controller button (**F6** on keyboard). Useful for a 2007 JRPG's long corridors and battle animations.
