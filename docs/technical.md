# Technical notes

Notes on the parts of this port that were genuinely hard. Written for anyone doing the same thing to another Xbox 360 title — most of what follows is not specific to Lost Odyssey.

---

## 1. Rendering above 720p

### The constraint

The Xbox 360's GPU does not render into main memory. It renders into 10 MB of embedded DRAM (EDRAM) attached to the GPU daughter die, and only resolves finished tiles out to RAM. That 10 MB is the hard budget for colour plus depth, simultaneously.

EDRAM is addressed in tiles of 80×16 samples at 32 bits per pixel — 5120 bytes each. 10 MB is exactly **2048 tiles**.

At 1280×720:

```
16 tiles across × 45 down = 720 tiles
colour + depth            = 1440 tiles   ✓ fits
```

At 1920×1080:

```
24 tiles across × 68 down = 1632 tiles
colour + depth            = 3264 tiles   ✗ needs 16 MB
```

This is *why* Lost Odyssey renders at 720p. It is not an engine limitation or an artistic choice; 1080p does not physically fit in the console. Every 360 game that renders at 1080p either uses a single buffer format that fits, or renders in predicated tiles.

Under emulation, the EDRAM is just a buffer in host memory, so the limit is gone. There are two ways to use that, and this port has done both.

### First attempt: make the game itself render 1080p (retired)

The first version of this port did the obvious thing: it made the game render a real 1920×1080 frame. It worked, it shipped in the port for a month, and it has been removed. It is worth describing because the reasons it failed apply to any 360 game.

It took three pieces.

**A bigger EDRAM.** The emulated EDRAM went from 2048 to 16384 tiles, and the render-target base fields in `RB_COLOR_INFO` / `RB_DEPTH_INFO` from the console's 11 bits to 13.

**Resolve shaders that did not know about it.** The GPU plugin ships precompiled compute shaders that copy finished tiles out of EDRAM. They take the source tile as an 11-bit constant and wrap addresses with a hardcoded `2048 tiles` literal. The port bound a sliding 2048-tile window over the big buffer so the shaders kept their 11-bit view, and binary-patched the literal — recomputing the DXBC container checksum, and rewriting the one `OpConstant` in the SPIR-V.

**A canvas pin.** Enlarge the render target and the 3D scene fills it, because the game's projection matrices are resolution-independent. The 2D layer does not move. Menus, HUD and subtitles are authored against a fixed 1280×720 canvas, with orthographic projections that bake those numbers in. So every draw had to be inspected, classified as 2D or not, and rewritten: viewport, scissor, projection constants. That grew to seven rules in the GPU plugin and around eighty-five hooks in the game code.

**Why it was retired.** The pin worked on everything that was a plain orthographic draw. But part of this game's interface is laid out on the CPU against the design canvas, and only then handed to the GPU as finished vertices: a tutorial box sized to its text, a callout line from a label to a point on a 3D target, letters that fly in one at a time. Each of those needed its own rule, each rule risked breaking another screen, and after weeks there was always one more.

The lesson, for anyone about to start a native-resolution patch: if the game's interface is computed against a design canvas, do not move the canvas. Scale the whole render instead.

### What replaced it: a fractional render scale

Xenia-derived GPU plugins already have a *draw resolution scale*. The game keeps rendering 1280×720 as far as it knows. The host multiplies everything by an integer N: render targets, viewports, scissors, and the textures the game resolves its frames into, which are kept in a parallel address space with N×N host texels per guest texel. The interface is exact, because the game is not told anything changed.

The catch is "integer". With N = 2 you render 1440p. If your monitor is 1080p, you render 1440p and throw almost half of those pixels away.

The port extends that mechanism to quarter steps. The scale is `q / 4`: 5 is ×1.25 (900p), 6 is ×1.5 (1080p), 9 is ×2.25 (1620p), 10 is ×2.5 (1800p). Quarters are not arbitrary. An EDRAM tile is 80×16 samples and resolves work in blocks of 8 pixels, and both have to stay whole numbers after scaling; a quarter step keeps them whole, a finer one does not.

**The storage does not change.** This is the decision that made it tractable. Scaled textures keep the emulator's layout, with a storage factor `S = ceil(q / 4)`. At ×1.5, a guest texel still owns a 2×2 block of storage; only some of those sub-texels are filled. Addresses, buffers and paging stay as they were, and the integer scales keep running the original code path untouched.

**The mapping.** A length of `n` guest pixels becomes `(q·n + 1) >> 2` host pixels. Host pixel `h` belongs to guest pixel `(4h + 2) / q`. So at ×1.5 guest pixels are alternately one and two host pixels wide, and a tile of 80 samples is exactly 120.

Vertically that is all there is to it: guest row `g` owns a run of host rows, stored in the first rows of its storage block.

Horizontally there is a constraint. The 360's tiled texture layout keeps short runs of texels — 8, 4, 2 or 1, depending on the format — contiguous in memory, and the compute shaders copy whole runs at a time. So the horizontal mapping is done in groups of `4 / gcd(q, 4)` of those units, chosen so that a group is a whole number of host runs. Run `r` of the group goes into storage slot `r`. Every slot then holds a contiguous, aligned run of host pixels, and the shaders can still copy a run at a time.

**What had to change.** The viewport becomes fractional. Scissors, point sizes, polygon offset and occlusion sample counts follow the new formula. The EDRAM tile seen by the pixel shaders becomes `20q × 4q` samples. The resolve, clear and texture-load compute shaders learn the mapping. The shader translators scale their per-pixel address arithmetic. It only exists on the exact EDRAM paths — pixel-shader interlock on Direct3D 12, fragment-shader interlock on Vulkan — which are the ones this game needs anyway (section 3).

**What it costs.** 1080p is 56% of the pixels of 1440p, and that is what the GPU now shades. The resolved textures still use the ×2 storage, so video memory sits between the two. Dropping the enlarged EDRAM of the first attempt gave back roughly 290 MB.

### Building the Xenos shaders from source

None of that was possible while the resolve and texture-load shaders existed only as compiled blobs. That is how the SDK ships them, and it is why the first attempt patched binaries.

The sources are in Xenia, written in XeSL — a thin macro layer that compiles as both HLSL and GLSL. The port carries a copy under Xenia's BSD licence and a build script: `fxc` for DXBC, `glslang` followed by `spirv-opt` for SPIR-V.

Before changing a line, the script was proven against the blobs it was replacing. All 109 DXBC shaders came out byte-identical to the SDK's. The SPIR-V came out equivalent, differing only in how a newer optimizer numbers its IDs. Only then were the shaders edited.

### Three traps with odd scales

×1.5 worked first, and it hid three bugs. With an even `q`, a lot of things line up by accident. At ×1.25 and ×2.25 they do not, and the screen filled with bars.

- **A difference of two addresses, held in an unsigned integer.** The texture loader reads two runs per thread, and computes the second as an offset from the first, shifted down to index a buffer. In the 360's tiled layout, runs are *not* in increasing memory order along X on half the rows. With an odd scale, the second run can sit before the first. The difference goes negative, the shift mangles it, and the result is 4-pixel bars every 40 pixels on alternate groups of rows. The fix is to shift both addresses before subtracting.
- **Assuming neighbouring pixels are neighbours in EDRAM.** A scaled tile is `20q` samples wide. With an odd `q` that is not a multiple of 8, so a run of 8 samples crosses into the next tile, which lives somewhere else. Worse, the depth half of a tile is `10q` samples, not a multiple of 4, and the fast depth resolve indexed four pixels at a time — so every depth resolve was off by two pixels. Under a fractional scale the resolves now read EDRAM one pixel at a time. They are cheap, and it ends the problem for every format at once.
- **Groups are aligned to the destination.** A resolve that starts in the middle of a group would leave the first partial run unwritten. It has not been seen in Lost Odyssey; the port logs a warning if it ever happens.

### A model that catches them in seconds

Each of those bugs looks the same in the game: stripes. What separated them was a small Python model of the whole path — EDRAM, resolve, scaled storage, texture load — using the same integer arithmetic as the shaders, with every host pixel carrying its own coordinates as its value. Run the path, and check that each pixel of the loaded texture holds the coordinates it should.

It reports exactly which columns are wrong, for which format, at which scale. From a report of "some thin lines in the 3D", it pinned down the depth offset and the tile crossing without another test run.

The other half of the method is measuring the artifact. Take a screenshot, average each column, subtract a running median, and look at which period carries the energy. A period of 40 host pixels at ×1.25 is 32 guest pixels, which is a texture tile: the bug is in the texture loader. A period of 100 is `20q`: the bug is in EDRAM addressing.

---

## 2. A build trap worth knowing about

Cost: four failed launches and a crash that appeared in code that had not been touched.

The GPU plugin is a fork that overrides SDK headers by placing its own copies **earlier on the include path**. Adding a *new* override header creates a trap:

The already-compiled object files have dependency files (`.d`) that reference the *SDK's* header — the one that hasn't changed. Ninja checks those, sees nothing newer, and recompiles only the `.cpp` you edited.

If the new header changes the size or member layout of a class, you now have some objects using the old layout and some using the new one. In this case one translation unit allocated a render target cache using the old `sizeof`, while its constructor wrote eight descriptor sets past the end of it. The result was heap corruption, a crash in untouched code, and — the part that makes it so hard to diagnose — no Vulkan validation layer error at all, because nothing invalid was ever submitted to the API.

**The rule:** after adding a shadow header to a fork, delete the build directory and rebuild everything. Editing an existing one propagates correctly; adding a new one does not.

**The symptom to recognise:** a crash that lands between two log points separated only by code you did not modify.

---

## 3. Choosing a Vulkan render target path

The Vulkan backend inherits two strategies for emulating EDRAM:

- **Host render targets** — real framebuffers, with copies between them to emulate EDRAM aliasing.
- **Fragment shader interlock (FSI)** — exact emulation inside a storage buffer.

The default configuration falls back to host render targets. In Lost Odyssey this is the wrong choice twice over: the game's dynamic shadows round-trip through EDRAM to a texture and back, which the copy-based path renders incorrectly, and the copies cost enough performance to put a supersampled preset under 30 fps.

Forcing the FSI path fixes both. This was initially misdiagnosed as "Vulkan is just poorly optimised" — worth flagging, because the symptom (broken shadows *and* bad framerate) looks like two separate problems and is one setting.

---

## 4. Patches as hooks, not bytes

The recompilation SDK has no facility for patching bytes at runtime — the guest code is gone by then, compiled into the host binary. What it has instead are mid-instruction hooks: run host code at a given guest address, with access to the guest registers, optionally skipping or redirecting the original instruction.

This turns out to be *better* than byte patching for distributing game fixes. Each of the Xenia Canary patches becomes a hook guarded by a boolean, which means every one of them is a toggle in the options menu that takes effect on the next frame, rather than a permanent modification chosen before launch.

The catch is verification: the hook has to sit at an address that means the same thing in the recompiled output as it did in the original executable. Addresses must be checked against the generated assembly, and against the correct regional build — the first patch list tried here was for the Japanese release and pointed into unrelated code.

---

## 5. SMAA on the final frame

SMAA 1x runs where the emulator's FXAA already did: on the frame about to be presented, after the guest has finished drawing. It is the reference implementation, unmodified, compiled into three compute passes — edge detection, blending weights, neighbourhood blending — with its area and search lookup textures uploaded once. A single HLSL source produces DXBC for Direct3D 12 and SPIR-V for Vulkan.

Running pixel-shader code as compute needed two adjustments. The edge detection pass uses `discard`, which compute does not have; it is redefined to write zero weights into a target that starts cleared, which is exactly what discarding would have left there. And every texture read becomes an explicit level-0 sample, because compute has no derivatives.

The Vulkan validation layers caught a real bug that the NVIDIA driver was quietly tolerating. The presenter's gamma pass declares its storage image as `rgb10_a2`, and the first version pointed it at the FXAA source image, which is RGBA16F — undefined behaviour that happened to look correct. SMAA now has its own intermediate image in the presenter's guest output format, and the validation output is clean.

Applying SMAA to the final frame means the HUD is antialiased too. For an interface made of flat 2D art that is harmless, and it keeps the effect independent of the game's own render passes.

TAA was evaluated and not attempted yet. Doing it properly on a 360 game means injecting camera jitter into specific shaders, capturing the scene before the HUD is drawn, and having depth, history and motion vectors — a different order of work from a post-process pass.

---

## 6. Wrapping whole guest functions

Section 4 described mid-instruction hooks. Some features need to act *around* a function instead — before it runs, after it returns — and the SDK allows that too, although it is not presented as a feature: every recompiled function is emitted as a weak alias of its implementation. Defining a function with the same name in the project replaces it at link time, and the original stays callable under its implementation name.

```cpp
REX_EXTERN(__imp__sub_82B88020);  // the recompiled original

REX_EXTERN(sub_82B88020) {         // replaces it at link time
  // ...before...
  __imp__sub_82B88020(ctx, base);
  // ...after...
}
```

No generated code is edited, nothing is byte-patched, and it links without duplicate symbols. Several features in this port are built this way:

- **Save anywhere** wraps the System menu's permission setter and its menu task, so the Save row stays enabled away from save points and the game's own permission comes back when the option is turned off.
- **The settings tabs** wrap the Configuration screen's task, to know every frame whether the screen is open and interactive.
- **Disc changes** wrap the game's one call site of `XamSwapDisc`.
- **No random encounters** wraps the field controller's step counter (section 10).

---

## 7. New menu pages that look native

The settings tabs are drawn by the port, on top of the game's own Configuration screen, and are meant to be indistinguishable from it. That breaks down into three problems: knowing when to draw, taking the controller, and looking right.

**When.** The Configuration screen is a task object in the game. One of its fields reaches an "interactive" state once the opening animation has finished, and another is non-zero while a native dialog is open on top of it. Wrapping the task (section 6) reads both every frame. The tabs are only shown while the screen is interactive with nothing above it, and vanish the moment that stops being true, so they never fight the game's own transitions or dialogs.

**The controller.** The game reads the pad in two places, and both pass through one hook. While one of the port's tabs is open, the hook reads the buttons for the page and hands the game a pad at rest, so the native page underneath does not react. Buttons still held when switching back to the native page are withheld until they are released — otherwise the same B press that returns to the game's options would also close them.

**Looking right.** An imitation with a similar font was never going to pass, so the port uses the real assets, read from the player's own game data when it starts: the menu font, the title font, and the texture atlas that holds the brushed-metal panels, the curved corner of the side panel, and the cursor. Reaching them means walking a chain of formats:

1. the disc's file index, whose names are packed in base 40 against a shared dictionary;
2. an archive whose entries use a custom, bit-oriented LZ compression;
3. an Unreal Engine 3 package, big-endian;
4. inside it, `Texture2D` objects holding DXT5 data that is LZO-compressed, stored in the Xbox 360's tiled block order and byte-swapped per 16-bit word — and `Font` objects, glyph tables laid over those textures.

Every step was validated first with a throwaway Python prototype that decoded the assets to PNG files, before any of it went into the executable.

The layout was then measured from a screenshot of the native screen, pixel profile by pixel profile: panel edges and bevel colours, the inset of the selected value, the drop shadow under the highlighted row, the exact scale of each font, and the fact that the help bar uses the same font squeezed horizontally. Highlighted and inactive text are drawn with recoloured copies of the font pages: the game's glyphs are a white face with a black outline, and no multiplicative tint can turn that into the dark face with a light outline that the game uses on a highlighted row.

None of the game's assets are in this repository or in the port. If the data cannot be read, the page falls back to a plain style instead of failing.

---

## 8. Four discs

Each of Lost Odyssey's four discs carries its own copy of the executable — the same code, with a header that says "disc N of 4" — and its own set of archives. Comparing the discs file by file, five archives differ from one disc to the next: the file index, events, field data, video and sound. The rest are identical.

When the game needs another disc, it calls `XamSwapDisc` with the disc number and an event to signal once the disc is in, waits on that event, and then re-reads the file index to check it has the disc it asked for. In the SDK, `XamSwapDisc` is a stub that reports success and never signals anything.

The port wraps the game's only call site. After the original runs, it looks the requested disc up, re-points the `game:` and `d:` links of the virtual filesystem to a device over that disc, and signals the event. The game's own check then passes, and it carries on. Devices for previous discs stay registered, since the game may still hold files open on them.

Discs are identified by that executable header, never by name. The same catalogue accepts every common form, each read in place by one of the SDK's filesystem devices: an extracted folder, an XDVDFS ISO image, or a Games on Demand package. Booting from an image has one wrinkle: the SDK needs the entry executable to be a file inside a folder, so only `default.xex` (6 MB) is copied to the cache, and the image is mounted over it as soon as the runtime exists.

Two lessons from getting there:

- **Mount order matters.** Mounting a different disc early in startup crashed the game on launch. The SDK still reads `game:\default.xex` while it prepares the module, and at that moment it found another disc's executable. Anything that changes the mounted disc has to wait until the module is prepared.
- **Check what you are given.** A folder labelled `disc2` on the development machine turned out to be a second copy of disc 1: every file hashed identical. Reading the real disc 2 image straight out of its zip archive — streaming, without extracting it — is what showed which files genuinely differ, and it is also why the port trusts executable headers rather than names.

Validated so far: a disc change forced by booting with disc 2 mounted, where the game immediately asked for disc 1, got it, and continued; a full boot from an ISO image, including the in-game settings reading their assets from it; and real changes during play — the game asked for disc 2 after an early boss and for disc 3 from a later save, and both were mounted without a prompt. Not yet exercised: the change to disc 4, a change between ISO images, and Games on Demand packages.

---

## 9. Keeping compiled pipelines on disk

The plugin inherits a persistent store from Xenia: one file of translated shaders, and one file of pipeline *descriptions* — which shaders, which blend state, which formats. At startup it reads the descriptions and asks the driver to create every pipeline again, so nothing stutters later.

That leaves the expensive step, compiling, to the driver's own cache. For Lost Odyssey that is about 2,100 pipelines. In the logs of this port the same step took either 0.3 seconds or between 73 and 134 seconds, depending only on whether the driver's cache hit. It missed after every resolution change, because each render scale produces different shader code, and sometimes it missed with nothing changed at all.

So the port stores the compiled result itself:

- **Direct3D 12:** an `ID3D12PipelineLibrary`. Each pipeline is looked up by name before it is created, and stored after. The name is a hash of the checksums of its shaders plus its description, so a pipeline whose shaders changed simply is not found and gets compiled fresh.
- **Vulkan:** a `VkPipelineCache`, serialised to disk and handed to every `vkCreateGraphicsPipelines` call. Its header is checked against the device and driver before it is trusted.

There is one file per title and render path — until October it was also one per scale; section 17 explains how that went away. It is written right after the startup batch, not only on exit, so a crash does not throw the work away, and it is written to a temporary file and renamed. If most lookups miss — after a change to the shader translator, say — the library is rebuilt from the pipelines that are alive, so stale entries do not pile up.

The cost is disk: roughly 230 MB on Direct3D 12, where the exact-EDRAM pixel shaders are large, and 30 MB on Vulkan. It is a cache. Deleting it costs one slow launch.

This store only holds pipelines the game has already drawn. Section 16 is how the port fills it before you play.

---

## 10. Finding the random-encounter check

The aim was a toggle for random battles that leaves scripted ones alone. There was no patch to port; it had to be found.

**Start from the effect.** Wrapping the functions that set up a battle and logging the chain of game functions above them gave the same call stack for a boss fight and for a random encounter. The battle is not built where it is decided: something leaves a request, and a later tick picks it up.

**Follow the request.** That led to a state machine in the battle manager, to the one function that starts it, and from there to a native function called from the game's Unreal script. Reading the engine's name table made the script side legible: the caller is the battle HUD's start function. That is still a consequence of the battle, not its cause.

**Log everything the script does.** A ring buffer of the last 65,536 script calls, dumped when a battle is requested, showed what happens in the seconds before one. A battle is a different map, and the game *travels* to it. In a random encounter, the last script call is an ordinary walking update — and twenty milliseconds later the field map is already being torn down. No script call in between decides anything.

**So the decision is native.** Instrumenting the queue of pending map travels found who asks for the trip: a function on the field controller that adds up the distance walked, compares it with a threshold for the zone, rolls, and requests the battle map.

The toggle wraps that function (section 6). Before the original runs it sets the accumulated distance to a very large negative number, and afterwards it puts the real value back. The threshold is never reached. Scripted battles request their map through a different function and are not affected.

The general method: when the effect and its cause are separated by a queue, stop following the call stack and instrument the queue.

---

## 11. Every texture, without playing the game

A texture pack needs the original textures. The plugin can dump them as the game loads them, but that means walking through every area of a four-disc RPG.

The port already had a reader for the game's data, written for the settings tabs (section 7). Generalised, it walks all four discs: 9,197 packages, read in about ninety seconds on six threads, yielding 16,276 distinct textures — 11,123 colour, 4,502 normal maps and 651 lightmaps, written to separate folders, since only the first kind is worth upscaling.

What makes this useful rather than merely complete is the file name. The pack identifies a texture by a hash of its top mip level as it sits in guest memory. The package stores that mip in the same tiled, byte-swapped form the game later hands to the GPU, so the hash can be computed straight from the disc. Of 1,503 textures dumped the slow way, in-game, 1,487 have exactly the hash the disc reader predicts. The rest are mostly things that never came from a package: video frames and render targets.

So a complete pack can be prepared offline, named correctly, and picked up by the game the first time it loads each texture.

---

## 12. Rebuilding the fonts instead of upscaling them

The game's fonts are texture atlases: a white glyph with a dark outline, compressed, drawn for 720p. An upscale came first. A careful filter chain made the text sharper but not clean, because the compression had already turned the edges into steps. The fonts were made from outlines once; the better idea was to go back to them.

**Identify the typeface.** For each font in the game, its glyphs are compared against the same characters rendered from the typefaces installed on the machine, scoring how much the silhouettes overlap. The matches were unambiguous — ordinary system typefaces, a humanist sans for the dialogue and menus and a grotesque for the small labels.

**Fit it.** Five parameters per font — size, baseline, stroke weight, outline radius and outline opacity — are fitted by numerical optimisation until the rendered glyphs line up with the originals.

**Redraw.** Every glyph is rendered from the vector outline at four times the resolution, through a distance field so that the outline has an even thickness, and placed by its centroid in the same cell of the atlas. The glyph table, the metrics and the spacing are the game's own; only the pixels are new.

The result goes through the texture pack like any other replacement. Nothing is patched. The atlas of button icons is not text, so it keeps a smooth upscale, and the credits font is the weakest fit of the set.

---

## 13. Two crashes worth writing down

Both of these are the kind that look like something else.

### A leak that ends every session

Sessions died after anywhere from four to twenty minutes with a failed allocation. A watcher that logged the emulated heaps every few seconds showed one region growing in a straight line — 205 MB in a little over three minutes, sitting idle in the intro, never once going down.

The game creates and destroys short-lived threads constantly, thousands of them per session. In the SDK, a finished thread could not release its own last reference from inside itself, and nothing else released it either. Every one of them kept its memory.

The fix parks finished threads on a list with a timestamp, and another thread frees them a couple of seconds later. Afterwards the same region stayed between 1 and 8 MB through 26 minutes of real play, with the count of threads released tracking the count created.

### A race in the audio system

The game crashed during the loading screen after an early boss, reading an address just above `0x10000000` that nothing had ever allocated.

The crash reporter showed two threads. One was loading a sound bank. The other, the audio engine's own thread, was walking the table of banks. While a bank is loading, its slot in that table briefly holds a small number that is not yet a pointer, and the engine followed it. On the console this presumably lands on readable memory; here it landed on a hole.

The fixes that intervened all failed. Rejecting the bad access stopped one crash and exposed the next reader of the same slot. Correcting the table from outside produced the game's own "disc read error".

What worked was to change nothing the game does: reserve that small range of addresses, filled with zeros. A reader that follows the half-written slot now finds a bank with zero entries and skips it. When the load finishes, the slot holds the real pointer.

A watcher confirmed it was the fix and not luck. In three consecutive runs through that loading screen it logged the race happening, with the very addresses that used to crash, and the game carrying on.

The lesson: when a race cannot be removed, make the losing side read something harmless.

---

*More to come as the port progresses.*

---

## 14. A sharp interface over a scaled 3D

The fractional scale of section 1 scales *everything* the game draws, interface included. That keeps the interface exact, but it ties its sharpness to the cost of the 3D: text at 4K means a 3D at 4K.

The port now splits the two.

**Telling the interface apart.** Lost Odyssey draws its 2D against a 1280×720 design canvas, with an orthographic projection that carries those numbers. The plugin recognises that projection in the vertex shader constants of each draw. That is the one signal that held across menus, HUD, dialogue, the battle interface and text — far more reliable than guessing from render state.

**A second target.** Interface draws are redirected away from the emulated EDRAM to an ordinary RGBA8 render target at the output resolution, drawn the conventional way, without pixel-shader interlock. The 3D keeps going through the EDRAM at the 3D scale.

**Compositing.** At the end of the frame a compute pass upscales the 3D to the output — or hands it to DLSS (section 15) — and blends the interface layer over it.

What made it hard is that the game does not keep the two apart. Some of it works with full-screen images:

- **Menu backgrounds and the orb screen** are copies of the scene that the game resolves out of the EDRAM and draws back as a full-screen quad. Moving those to the interface layer corrupted them. The rule that settled it: an image the size of the screen stays in the EDRAM with the 3D.
- **Crossfades between scenes** blend the previous frame, interface included, over the new one. A frame split in two layers cannot reproduce that, so there is an option for a clean cut instead. It only skips the faded layer while its opacity is below one.

On screens that are not 16:9 the interface layer is a centred 16:9 box. Still open: the gamma ramp is not applied to the interface layer, and markers attached to 3D positions may drift outside the 16:9 area.

---

## 15. DLSS on an emulated GPU

DLSS wants five things: a jittered low-resolution colour image, depth, motion vectors, the jitter offset, and the scene before the interface goes on top. A 2007 console game provides none of them. Each had to be manufactured.

- **The scene before the interface** is the 3D target of section 14. Without that split there would have been nothing to feed it.
- **Depth** lives only in the emulated EDRAM, in the console's 24-bit floating point format, laid out in tiles. A compute shader reads it back into a normal depth texture right after the game resolves the main surface, following the EDRAM addressing of section 1, fractional scale included.
- **Motion vectors.** The game's view-projection matrix is in a fixed range of vertex shader constants. With the current matrix and the previous one, the same compute pass reconstructs each pixel's position from depth and projects it into the previous frame. That gives exact camera motion. Objects that move on their own — characters, particles — get no vectors of their own. An experimental pass that captures per-character motion exists, and is off by default, because DLSS already handles them well.
- **Jitter.** A Halton (2,3) sub-pixel offset is added to the viewport of 3D draws only: depth-tested, full-screen, scene-sized. Jittering the interface would make it shimmer.
- **Calling it.** DLSS is driven through NVIDIA's NGX from inside the plugin's deferred command list, with the descriptor heaps and pipeline state rebound afterwards. On Vulkan it needs a few extensions the SDK did not enable, which a small patch to the SDK adds.

**The flag that mattered.** The first working build left trails behind everything that moved. The cause was not the motion vectors. Lost Odyssey uses **inverted depth**, with the near plane at 1 and the far plane at 0, and DLSS has to be told so. One flag removed the trails.

Two things were tried and dropped. A negative mip bias, recommended for DLSS, turned the 3D almost black on Vulkan, so it is off. A choice of DLSS model was removed; NGX picks one per mode.

---

## 16. Preparing every shader from the discs

Section 9 keeps compiled pipelines on disk, but a pipeline only gets there after the game has drawn it once, and that first draw is a stutter. A cache seeded from someone else's play would be the usual answer, and it would be game data. This port builds its own from the user's discs.

Lost Odyssey is an Unreal Engine 3 game, and UE3 packages carry a **ShaderCache** export: the compiled Xbox 360 shaders for every material in the package. The port wraps the game's file read function, recognises packages as they are read and parses what it needs:

- the shader cache itself, keyed by material;
- which materials each package asks for — imported materials, exported material instances, and materials attached to meshes.

That gives pairs of vertex and pixel shaders, which go to the plugin to be translated and turned into pipelines ahead of time.

The remaining piece is render state — vertex layouts, blend modes, formats — which does not live in the packages. A small seed file holds just those descriptions, about 33 KB, with no code and no shaders in it. Each discovered pair is combined with the state descriptions that fit it.

**All of the disc at once.** Waiting for the game to read packages only covers where you have been. So on the first launch the port reads every package on the mounted disc itself — disc 1 is 6,253 packages, parsed in about three seconds on several threads, giving 11,648 shader pairs — and hands the whole batch over.

**Never in the way.** The player chooses all four discs or only the current one. While the queue is working, whatever the area being loaded needs jumps to the front. Pipelines are created at the normal pace and not flat out; flat out dragged the game to 20 fps. On a warm driver cache the 16,000 pipelines of the full game take about 12 seconds. On a cold one, the first time, it can take up to a quarter of an hour.

---

## 17. One pipeline set for every scale

Until October the 3D scale was compiled into the translated shaders, because the EDRAM addressing of section 1 depends on it. Every scale was therefore a different set of about two thousand pipelines, with its own file on disk and its own slow first launch.

The scale is now a **runtime constant**. The shader translators — DXBC for Direct3D 12, SPIR-V for Vulkan — read it from a constant buffer instead of baking it in. The places that needed it:

- pixel-shader parameter generation;
- memory export from pixel shaders;
- the interlocked EDRAM output, where the tile size is `20q × 4q`;
- texture fetches from scaled resolves.

The awkward part is division. With a baked-in scale, dividing by `q` compiles to a shift or to a constant multiply. At run time it would be an integer division per pixel. The port precomputes the "magic number" multiplier and shift for each `q` on the host and passes them along, so the shaders still multiply, and the result is exact for every value they can see.

The outcome is one pipeline library per renderer instead of one per scale. Moving the 3D slider costs nothing but a restart.

---

## 18. Where the frame time went

The plugin gained a GPU profiler — timestamp queries on both renderers, grouped by kind of work and by shader — and an fps counter. What it showed, in order of what it was worth:

- **Re-uploading memory that had not changed.** The SDK marked every page the CPU had written as dirty every frame, and re-uploaded it whole. Turning that off took uploads from about 560 per frame, 18 MB, to about 90, under half a megabyte. On Vulkan at 720p with ×3 supersampling: from 31–33 fps to 48–49.
- **Transparent effects that add nothing.** With exact EDRAM emulation every blended fragment pays for an interlocked read-modify-write, including the ones with zero alpha, or an additive zero. Those now skip the EDRAM. Draws that do not write depth run their depth test after the shader. On Vulkan the GPU frame dropped from 22.5 to 12.8 ms in an effect-heavy scene, with effect draws two and a half to four times cheaper. On Direct3D 12 the gain was small.
- **Texture copies by compute on Vulkan.** Storage-image writes replace buffer-to-image copies: texture loading went from 4.7 to 1.95 ms per frame.
- **Depth clears.** The game clears depth by drawing rectangles, which under interlock means a full pixel pass. They now become a compute clear: 1.2 to 0.6 ms per frame.
- **Asynchronous submission.** Recording and submitting command lists moved off the thread that runs the game's GPU commands. At 4K on Direct3D 12, from 47–51 fps to about 58. At ×3 on Vulkan, from about 38 to about 50.
- **Registers as locals.** The recompiled code keeps the condition, count and exception registers as C++ locals instead of fields of the CPU context, so the compiler can keep them in host registers. The executable shrank from 88.9 to 73.4 MB. Two bolder variants of the same idea were tried and reverted: one broke video playback, the other crashed at start.

An experiment in batching uploads speculatively made vertices explode, and was dropped.

---

## 19. HD textures without hitches

The first full HD pack made every area entrance stop. A trace showed about ninety pack loads in ten seconds blocking the GPU thread for almost a second in total, decoding and uploading on the spot.

Now a texture whose HD replacement is not ready is drawn with the original. Background threads read and decode the DDS; when it is ready, the cache drops the original and creates the HD one in its place. The dropped texture is kept in a "graveyard" until the GPU has finished every frame that used it.

Three details mattered:

- **Textures used every frame** — the text atlas, button prompts — never changed to HD, because the cache never saw a moment when they were unused. They are now replaced regardless, through the same graveyard, and the pack's interface textures are loaded straight away.
- **Stale descriptors.** On Direct3D 12, replacing a texture under a cached descriptor table hung the GPU. A replacement counter now forces the tables to be rebuilt.
- **Reused slots.** The game reuses texture objects. After a scene change, a slot could keep the HD match of whatever it held before, so a character briefly wore someone else's clothes. The match is now recomputed whenever the game reloads data into an existing texture.

The pack is stored as BC7 DDS with full mip chains: a 4096² texture goes from about 85 MB uncompressed to 22 MB. The texture cache's memory budget is raised accordingly while a pack is active.

---

## 20. A patch that froze the enemies

Every Xenia Canary patch for Lost Odyssey was ported as a switch, and one of them was "Disable occlusion queries". Its purpose was to sidestep two hangs under emulation.

In this port, with it on, some battles had enemies frozen in their pose, or stretched into spikes of vertices. It looked like a skinning bug, and it was chased as one — through the CPU skinning path, the code generator (section 21) and thread priorities, which seemed to help until it happened again.

The cause was the patch. Unreal Engine 3 does not animate skeletons it believes are invisible. With occlusion queries disabled, the answer the engine got back said "not visible", so those skeletons were never updated. Occlusion queries are now always on, answered by the emulated GPU.

The lesson for anyone porting a patch list: a patch that hides a problem in an emulator may be answering a question the engine actually uses.

---

## 21. A bug in the code generator

While chasing a flicker on a character, a trace of the vertices the GPU read led back to the CPU skinning code. Every tangent came out as `0xFF000000`.

The chain was a run of vector instructions: a multiply-add, a conversion to integers, and then `vpkuwus` and `vpkuhus`, which pack with unsigned saturation. In the generated code the destination register was also the source. The generator wrote the result element by element *while still reading the source*, so X, Y and Z were overwritten before they were read, and only W survived.

The fix is to build the result in a temporary and assign it at the end. The other element-wise generators were checked and do not have the problem, because they read and write the same index. It did not turn out to be the cause of the flicker, but it was a real miscompilation in the SDK, and it is worth checking in any game built with it.
