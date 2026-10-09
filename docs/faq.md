# FAQ

### Where is the source code?

Here. The port's own code is open under the MIT licence: the game executable (`project/`), the GPU plugin (`xenos_fork/`), the tools (`tools/`) and the developer notes (`docs/dev/`). See [Building from source](building.md).

What is **not** here, on purpose: the game's code and assets, the recompiled code generated from your disc (`generated/`), and the HD texture pack (encrypted, downloaded by the game and only opened by your discs).

### Will there be a download?

Yes: test builds of the port itself under [Releases](../../releases) — never the game.

A release needs your own legally obtained copy of Lost Odyssey: the installer asks for your discs, checks them and copies their files. No game code and no game assets are ever distributed here. That is not only a legal position, it's the practical one: it is what keeps projects like this online.

### What will I need to play it?

Your own copy of the game — all four discs.

The intended layout is one folder per disc next to the executable, `data\disc1` to `data\disc4`, each holding that disc's extracted files. ISO images and Games on Demand packages also work, read where they are without extracting anything. The game changes discs by itself when the story needs it.

### Does it modify my game files?

No. Game data is only ever read. The one file the port copies is the disc 1 executable, into its own cache, and only when booting from an ISO image or a Games on Demand package. The in-game settings read the game's font and menu textures from your copy each time; they are never stored anywhere.

### Why not just use Xenia?

Xenia is an excellent emulator and this project would not exist without the work behind it. They solve different problems.

An emulator translates the game's code while it runs. A static recompilation translates it *once*, ahead of time, into a normal PC executable. The result runs the game's logic as native x86-64 with no interpreter or JIT in the way — but more importantly for this project, it makes the game *modifiable* in ways an emulator cannot easily match:

- Fixes become toggles in an options menu instead of external patch files.
- The renderer is part of the build, so it can be changed — which is how a game hard-limited to 720p by console memory ends up rendering at anything up to 4K, with DLSS, with its interface intact and sharp.
- The output is a single executable that behaves like a PC game.

The cost is that the work is per-title and substantial. Xenia runs thousands of games; this runs one.

### Does it run better than the console?

Yes, in the ways you would expect from native code and a modern GPU: 60 fps, higher resolutions, ultrawide, DLSS, anisotropic filtering, and no loading from optical media.

### Does it have the crashes this game is known for under emulation?

No. The three well-known ones — the Grand Staff prison cell, the cutscene after the first boss, and the frozen train on disc 3 — do not happen here, at 60 fps and with no workaround. The second one did crash in this port at first; its cause was found and fixed. The [feature list](features.md#the-known-crashes-are-gone) has the full table, including the port's own bugs and their status.

The game has not yet been played start to finish, so there may be problems nobody has met. If the port crashes, it writes a report naming the original game function it was in.

### Why does the first launch take a while?

On the first launch the port reads your discs and prepares every shader the game will need, so that nothing stutters later. You can prepare all four discs or only the part you are playing, and you can start playing right away with **Enter** while it finishes. It takes from a few seconds to about a quarter of an hour, depending on what your graphics driver already has cached.

It happens once per renderer. After that, startup takes two or three seconds, at any resolution and any 3D scale.

The cache takes roughly 230 MB on Direct3D 12 and 30 MB on Vulkan. It is safe to delete; the next launch rebuilds it.

### Which resolution should I pick?

For **Resolution**, the one your monitor has. The interface is drawn at that resolution, always sharp.

**3D scale** is where the cost is. ×1.5 is 1080p, ×2 is 1440p, ×3 is 4K; matching your output is the natural choice, and going above it is supersampling. On an RTX card, DLSS Quality or DLAA lets you keep a lighter 3D scale with a sharp result.

### Do I need an NVIDIA card?

No. DLSS needs a GeForce RTX card; everything else works on any GPU with Direct3D 12 or Vulkan. Without DLSS there is SMAA, FXAA and a 3D sharpness setting.

### Linux? Steam Deck? Android?

Linux is planned and is the reason the Vulkan renderer exists — the recompilation SDK supports Linux, including arm64. It has not been built yet. The Steam Deck's 1280×800 is already one of the output resolutions.

Android is not supported by the SDK, so it is not on the table.

### Can I help / can I test it?

Not yet, but this is worth asking again later. Watch the repository for updates.

### Is this affiliated with Microsoft, Mistwalker or Feelplus?

No. This is an unaffiliated, non-commercial preservation effort. Lost Odyssey is © its respective rights holders.
