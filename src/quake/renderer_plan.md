# Quake Renderer: Pragmatic GPU Migration Plan

Status: draft rewrite of `~/quake.md`.

Goal: make the Quake renderer faster and simpler by moving more real
rendering work onto the GPU, without pretending the current software
renderer can be deleted in one jump.

Target: stable 30 FPS at the normal 320-wide viewport on openfpgaOS.
Higher frame rates are a bonus, not a planning assumption.

## 1. Summary

The right long-term direction is still triangle-first rendering. The
software AET/span path exists because original Quake rendered into a CPU
framebuffer. This platform now has a useful triangle rasterizer, a command
ring, command-stream DMA, perspective attributes, Gouraud light, colormap
lookup, and GPU-routed HUD blits. We should use those.

The pragmatic change is phasing:

1. Stabilize the world-triangle path that already exists.
2. Restore or replace depth in a way that lets entities and sprites be
   occluded without `D_DrawZSpans`.
3. Only then remove `R_ScanEdges` from the default world path.
4. Move particles, sprites, water, warp, and lightmaps after the core world
   path is measured and correct.

This is not a one-kick whole-frame renderer in the first version. The current
SDK mixed command stream batch is capped at 2560 words, about 10 KB. A full
frame command list can be built as a CPU-side concept, but submission must be
chunked at command boundaries unless the SDK/RTL grows a larger streaming
mode.

## 2. Current facts from the repo

These are the constraints the plan starts from:

- `D_GPU_WORLD_LIGHT` is the normal path. It already avoids the old per-texel
  pre-lit surface rebuild for many world surfaces by using GPU colormap
  lookup with one light row per surface.
- `R_DrawSurfaceTris` is now the unconditional normal-world surface path. It
  tessellates surfaces, clips near-plane geometry, handles repeated texture
  tiles, batches consecutive surfaces by texture, and currently submits those
  batches as one-triangle GPU commands until large-count triangle commands are
  proven on the deployed RTL.
- `R_ScanEdges` still runs. Even if world pixels are drawn by triangles,
  the AET/span path is still used to discover visible spans and to feed
  `D_DrawZSpans`.
- GPU depth was retired in the lean Phase 2.3 path. `of_emit_depth_test`
  is currently a no-op, and the public span depth bits are reserved.
- The CPU-side z buffer still exists and is still important for alias/sprite
  occlusion.
- `Turbulent8`, `D_WarpScreen`, and `D_DrawParticle` still contain CPU pixel
  work.
- The SDK command ring is 16 KB, and mixed command-stream DMA uses a 16 KB
  scratch split into about 10 KB command stream slots. The current code is
  designed for many small batches, not one 256 KB raw DMA stream.

The main opportunity is still real: remove the CPU edge/span renderer from
the hot path. The sequencing has to respect the depth and command-stream
constraints.

## 3. Non-goals for the first shipping version

These should not block the first useful renderer:

- Pixel-identical software Quake lighting.
- GPU-side true lightmap sampling.
- GPU-side turbulent water.
- One DMA submission for the entire frame.
- Maintaining a parallel old-renderer fallback.
- 60 FPS as a commitment.

The first useful target is simpler: world triangles on by default, no visual
show-stoppers, CPU frame time comfortably under 33 ms on the dense test scene,
and no reliance on undefined GPU behavior.

## 4. What this design is not

These are intentional boundaries for the RTL side of the plan:

- no edge-walk coprocessor; the triangle rasterizer is the edge walker;
- no RGB565 texture path for Quake; the game is paletted end to end;
- no bilinear filtering; at 320-wide resolution it is not worth the timing
  and area risk;
- no RGB blend path for Quake; use paletted translucency/lookup behavior;
- no full software-Quake pixel parity requirement for the first shipping
  renderer.

If one of these becomes attractive later, it should have its own benchmark and
RTL budget. It should not slip into the renderer migration as incidental work.

## 5. Architecture target

The end-state renderer should look like this:

```text
CPU:
  - PVS and BSP traversal
  - build ordered surface/entity/effect batches
  - transform vertices
  - emit GPU commands into chunked command buffers
  - submit chunks at command boundaries

GPU:
  - clear color/depth
  - rasterize world triangles, writing color and depth
  - rasterize alias/sprite/particle triangles, testing depth
  - draw HUD/console/menu blits
  - flip at the normal video boundary
```

The first implementation does not need to literally store the whole frame as
one monolithic command stream. A per-frame builder can be used internally, but
it must flush chunks that fit the current `OF_GPU_COMMAND_STREAM_BATCH_WORDS`
limit.

## 6. Depth strategy

Depth is the hard dependency. Without a replacement for `D_DrawZSpans`, the
renderer cannot delete `R_ScanEdges` without breaking sprites, alias models,
and particles near walls.

### Option A: keep CPU z temporarily

This is the current state. It is useful while validating world triangles, but
it preserves the AET/span dependency and keeps a costly CPU pixel pass.

Use this only for Phase 1 validation.

### Option B: painter-sort entities, no z

This would let us bypass AET sooner, but it will produce visible artifacts in
doorways, railings, corpses, gibs, and interpenetrating models.

Keep this as a debug experiment only. It is not the recommended shipping path.

### Option C: restore GPU depth

Recommended.

Restore enough GPU depth support for:

- world triangles: depth write on, depth compare optional or always pass;
- alias models: depth test and depth write on;
- sprites and particles: depth test on, depth write usually off;
- translucent effects: sorted where needed, depth write off.

The first prototype should use SDRAM depth, not M10K depth. At 320x200, an
8-bit depth buffer is about 64 KB per buffer; at 320x240 it is about 75 KB.
A 16-bit depth buffer doubles that. SDRAM memory is not the problem; bandwidth
and RTL simplicity are.

Start with 8-bit depth if the RTL is materially simpler, but keep the API
versioned so 16-bit depth can replace it if artifacts show up. The old CPU
z buffer is 16-bit, so 8-bit depth needs real hardware screenshots before it
is declared good enough.

Depth is the first RTL feature that matters. Do not spend RTL budget on water,
lightmap sampling, or larger command streams before this is settled.

The read arbiter needed for depth RMW is also the arbiter needed by the Tier 3
alpha-blend path described in `~/Repos/gpu.md`; charge that RTL cost once
across both efforts, not once per feature.

## 7. Command submission reality

The old document assumed a 256-512 KB command stream submitted in one kick.
That is not the current SDK.

Current constraints:

- ring buffer: 16 KB;
- mixed command stream batch: 2560 words, about 10 KB;
- batch scratch: 16 KB total;
- helpers can submit triangle batches directly, but they still write through
  the ring/batch machinery.

Practical plan:

- build batches by state: texture, mip, flags, depth mode;
- flush whenever the next command would exceed the current batch limit;
- never split inside a command;
- keep per-surface/per-texture batching local and simple;
- measure whether command overhead is actually a bottleneck before adding a
  larger long-stream DMA mode.

A larger stream path may be useful later, but it is not required to prove the
renderer.

## 8. Lighting plan

The first renderer should use the existing GPU colormap path plus per-surface
or per-vertex light. It will not be pixel-identical to software Quake, but it
is enough to validate the architecture.

Important detail: a 16 KB Quake colormap is 64 light rows x 256 palette
entries. A true `light_value[0..255] x diffuse[0..255]` table would be
64 KB, not 16 KB. Any GPU lightmap design must quantize the sampled lightmap
value to the existing colormap row range or change the table layout.

Phase 1 lighting:

- keep `D_GpuLightSurface`;
- use the current per-surface light row where acceptable;
- for world triangles, use per-vertex light sampled from the already-built
  blocklights data;
- add subdivision only where long surfaces show visible banding.

Later lightmap sampling:

- pack Quake lightmaps into an atlas at level load;
- interpolate lightmap UVs per triangle;
- sample lightmap, quantize to a colormap row, then sample diffuse and
  colormap.

This is likely a multi-cycle fragment path unless the texture/cache design
grows another read port. Do not assume it remains one pixel per cycle.

## 9. Phased implementation

### Phase 0: make the current state measurable

Scope: no renderer rewrite, no RTL.

Tasks:

- keep a startup log line and profiler marker for the active renderer mode;
- capture the same dense and sparse scenes in both modes;
- record CPU buckets, GPU stall counters, and screenshots;
- verify whether `of_emit_triangles_batch` really works for more than one
  triangle on hardware, not just in the SDK header.

Exit criteria:

- the active renderer mode is unambiguous;
- world-triangle mode has hardware screenshots;
- current regressions are listed as bugs, not guessed.

### Phase 1: ship world triangles inside the existing pipeline

Scope: use `R_DrawSurfaceTris`, but keep `R_ScanEdges` and CPU z.

Cost warning: during this phase the renderer may do extra work. `R_ScanEdges`
still runs for visibility/depth, and `R_DrawSurfaceTris` also emits triangles.
CPU frame time may temporarily increase or only improve modestly until Phase 3
removes AET from the default path.

This phase does not delete AET. It replaces per-surface span drawing with
triangle drawing while leaving the old visibility/depth machinery in place.
That is less glamorous, but it is the safest way to validate projection,
clipping, texture coordinates, tiled surfaces, bmodels, animated textures, and
lighting.

Tasks:

- harden `R_DrawSurfaceTris` for all id1 maps;
- keep texture batching bounded and easy to reason about;
- add asserts or counters for skipped/clamped over-budget surfaces;
- compare screenshots against the span path;
- profile the dense scene with world triangles on by default;
- capture hardware screenshots and fix visible regressions directly.

Expected result:

- `D_CalcGradients` and `D_DrawSpans8` stop being the main world pixel path;
- `R_ScanEdges` and `D_DrawZSpans` still cost real time;
- dense scene should move materially toward 30 FPS, but not to a 3 ms CPU
  frame.

### Phase 2: restore GPU depth

Scope: RTL plus firmware/API.

Tasks:

- define depth buffer format and address binding;
- restore depth clear;
- restore depth test/write flags for triangle/span commands as needed;
- make `of_emit_depth_test` real again;
- make world triangles write depth;
- make alias models test/write depth;
- make sprites and particles test depth without usually writing it;
- keep CPU z until GPU depth screenshots pass.

Exit criteria:

- alias models and sprites occlude correctly against world triangles;
- `D_DrawZSpans` can be disabled in a test build without obvious artifacts;
- GPU depth bandwidth is measured, not guessed.

### Phase 3: remove AET from the default world path

Scope: engine architecture, minimal RTL beyond Phase 2.

Tasks:

- build a world surface list directly from BSP traversal in back-to-front or
  otherwise correct order;
- emit world triangles from that list without calling `R_ScanEdges`;
- ensure brush models are transformed and ordered correctly;
- use GPU depth, not CPU spans, for dynamic model occlusion;
- remove default dependence on the old span renderer instead of carrying a
  parallel world-renderer switch.

Exit criteria:

- default frame path does not call `R_ScanEdges` for normal world rendering;
- `D_DrawZSpans` is not used by the default renderer;
- dense scene CPU frame time is under the 33 ms budget with margin.

This is the real architectural win.

### Phase 4: particles and sprites as triangles

Scope: firmware/engine, probably no RTL if depth exists.

Tasks:

- convert sprites to textured quads with skip-color;
- convert particles to small screen-facing quads or a tiny dot texture;
- depth-test them against the GPU depth buffer;
- avoid per-particle CPU framebuffer writes;
- sort translucent effects only where needed.

Exit criteria:

- no renderer CPU writes directly to the framebuffer for normal particles;
- explosions, smoke, gibs, and projectiles look acceptable in hardware
  screenshots.

### Phase 5: command builder cleanup

Scope: SDK/engine cleanup after the renderer shape is proven.

Tasks:

- introduce a small command-buffer builder that chunks at 10 KB boundaries;
- flush at command boundaries only;
- keep texture/depth/colormap state changes explicit;
- measure ring waits and DMA waits;
- only consider a larger DMA stream mode if chunk overhead is visible.

Exit criteria:

- renderer submission code is centralized;
- command overflow is impossible by construction;
- no hidden mid-command flushes exist.

### Phase 6: water, warp, and other CPU pixel paths

Scope: optional RTL or lower-risk approximations.

Do this after the world/AET/depth work. Water and warp are visible, but they
are not the largest structural blocker.

Options:

- keep CPU `Turbulent8` temporarily if it is not the frame bottleneck;
- approximate water with animated affine/perspective UVs and no sine LUT;
- add `OF_GPU_SPAN_TURB` or a triangle turb mode if profiling justifies RTL;
- handle screen warp as a separate fullscreen effect only if the source
  framebuffer can be sampled safely by the GPU.

Exit criteria:

- no major CPU pixel walk remains in common gameplay;
- water/warp visual differences are documented if they are not exact.

### Phase 7: real lightmaps, if still worth it

Scope: visual quality RTL/firmware.

This should come after the core renderer is already fast. The Gouraud/subdivide
approach may be good enough at 320-wide resolution.

Tasks:

- build a lightmap atlas at level load;
- add lightmap UV attributes or derive them from existing surface data;
- sample lightmap and diffuse in a staged fragment path;
- quantize sampled light to the existing 64-row colormap unless the colormap
  format changes;
- measure the pixel throughput loss.

Exit criteria:

- visible wall-lighting banding is materially reduced;
- GPU frame time remains inside budget;
- ALM/timing impact is acceptable after hardware compile.

## 10. Rough RTL cost budget

These are planning numbers, not sign-off numbers. Each RTL phase still needs a
hardware compile and timing readback before merging.

| Phase / feature | ALMs rough | M10K | DSP | Critical-path safe? |
|---|---:|---:|---:|---|
| Phase 2: SDRAM GPU depth RMW + flags | ~170 | 0 | 0 | yes, if registered |
| Phase 6: turb addressing + sine LUT, if pursued | ~120 | 1 | 0 | yes, if staged |
| Phase 7: lightmap sample path, if pursued | ~250 | 0 | 0 | yes, only if multi-cycle/staged |

The depth row assumes the M_RD/read arbiter is shared with the paletted blend
work, not duplicated.

## 11. Deletion policy

Use git rollback instead of long-lived renderer fallbacks. Delete old default
branches as each replacement lands, while still keeping unrelated helper code
that remains needed by sky, turbulent surfaces, or CPU z until those phases
replace it too.

Suggested deletion order:

1. legacy surface-cache rebuild path, once world triangles and GPU colormap
   lighting are default and stable;
2. `D_DrawSpans8` world usage, once no default world surface uses it;
3. `D_DrawZSpans`, after GPU depth is default and validated;
4. `R_ScanEdges` from the default path, after direct BSP surface submission
   is default and validated;
5. CPU particle drawing, after GPU particle quads are default;
6. CPU turbulent/warp paths, after replacement or acceptable approximation.

Do not delete code that is still called by the current renderer. Once a phase
removes the last caller, remove the dead branch in the same change.

## 12. Metrics

Use these to decide whether a phase worked:

- dense scene CPU frame time;
- sparse scene CPU frame time;
- `R_ScanEdges` bucket time;
- `D_DrawSurfaces` bucket time;
- `D_DrawZSpans` bucket time;
- command ring waits;
- command DMA waits;
- GPU texture stalls;
- GPU framebuffer/depth stalls;
- visible artifacts in fixed screenshots;
- demo playback stability.

Initial success target:

- dense scene under 33 ms CPU frame time;
- no major correctness regressions;
- active renderer mode is visible in the profiler.

End-state success target:

- default renderer does not depend on `R_ScanEdges`;
- default renderer does not call `D_DrawZSpans`;
- normal gameplay has no CPU framebuffer pixel writes from the renderer;
- code size decreases as dead renderer branches are removed;
- 30 FPS is stable in the dense reference scene.

## 13. Practical schedule

These estimates assume a hardware screenshot/profiling loop is available.

- Phase 0: 2-3 days.
- Phase 1: 1-2 weeks.
- Phase 2: 1-2 weeks RTL/firmware, depending on depth format.
- Phase 3: 2-3 weeks.
- Phase 4: 1 week.
- Phase 5: 3-5 days.
- Phase 6: 1-2 weeks if RTL turb is needed, less for approximation.
- Phase 7: 2-4 weeks if true lightmap sampling is justified.

The useful milestone is not "all phases done." It is Phase 3: no default AET
world path and no CPU z-span fill. That is where the renderer becomes a new
architecture rather than an optimized version of the old one.

## 14. Recommended next commits

1. Capture hardware screenshots for the unconditional triangle-world path.
2. Verify multi-triangle batch execution on hardware.
3. List current triangle-world visual bugs.
4. Decide depth format and implement GPU depth prototype.

Until those are done, further clean-sheet planning is less valuable than
measurement.
