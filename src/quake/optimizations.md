# Quake openfpgaOS - Rendering Pipeline Review & Optimization Plan

Reviewed against the current source in `src/quake/engine` and `src/sdk/include/of_gpu.h`.

Target: Analogue Pocket, VexiiRiscv rv32imafc @ 100 MHz, custom asynchronous span/triangle GPU with a 16 KB M10K ring, I8 texture path, SRAM/SDRAM framebuffer writes, colormap LUT, and GEQUAL depth testing.

Framebuffer and z-buffer are 320x240. The 3D view rect is dynamic: default `viewsize 90` with the full status bar renders about 288x192; `viewsize 100` renders 320x192; `viewsize 110` renders 320x216; fullscreen/intermission can use 320x240.

## 1. Current build-time switches

| Area | Current default | Source | Notes |
|------|-----------------|--------|-------|
| World spans | `D_USE_GPU_PERSP=1` | `d_scan.c` | One GPU perspective span per scanline. |
| Alias models | `D_USE_GPU_ALIAS=2` | `d_polyse.c` | Hardware triangle path, not CPU alias spans. |
| Alias perspective | `D_ALIAS_PERSP=1` | `d_polyse.c` | Per-vertex `w` is submitted from Quake 1/z. |
| Alias lighting | `D_ALIAS_GOURAUD=1` | `d_polyse.c` | Vertex `r=g=b` gets the colormap light row from `finalvert_t.v[4] >> 8`. |
| Surface block accelerator | `HW_SURFBLOCK_ACCEL=0` | `r_surf.c` | CPU block builder is active; inactive hardware path remains as documentation. |
| Row accelerator | `HW_SURFACE_ACCEL=0` | `r_surf.c` | CPU colormap lookup is active. |
| Profiler | `pq_cycleprof=0` | `r_main.c` | `pq_cycleprof 2` draws the terminal profiler and HW counter rows. |
| Fast text placement | `PQ_FASTTEXT` expands empty | `quakedef.h` | The annotations are present but do not currently move code to a special section. |

## 2. Current pipeline

### 2.1 What the GPU does

| Work | Path | Current behavior |
|------|------|------------------|
| Startup clears | `VID_Init` -> `of_emit_clear(COLOR|DEPTH)` | Clears all three back buffers once, with an `of_emit_finish()` after each clear. |
| Per-frame flip/clear | `VID_Update` | Drains all queued GPU work, flips, rebinds the new framebuffer/z-buffer, then queues a depth-only clear. The clear is not explicitly kicked there. |
| World spans | `D_DrawSpans8` -> `of_emit_span` | Default build has `D_GPU_WORLD_LIGHT=1` (T6 simple): submits raw mip texture + `OF_EMIT_PERSP | OF_EMIT_COLORMAP` and the GPU does `colormap[light*256 + texel]` per pixel.  `pq_world_light` is set per-surface from `R_BuildLightMap`'s output via `D_GpuLightSurface` — no per-texel CPU rebuild.  Fallback path (`D_GPU_WORLD_LIGHT=0`) goes through the legacy CPU-pre-lit surface cache. |
| Alias triangles | `D_DrawNonSubdiv` -> `of_emit_triangles` | Binds one I8 skin texture per model, then emits one `DRAW_TRIANGLES` command per triangle. Vertices are screen-space 12.4 x/y, 16-bit GEQUAL z, 16.16 s/t, perspective `w`, and monochrome colormap-light bytes. |
| Colormap | `VID_Init` -> `of_emit_upload_colormap` | Uploads 64x256 bytes once at startup. |
| Depth test state | `of_emit_init` | Sets global GEQUAL depth. The SDK no longer has a `SET_SHADE` or `SET_BLEND` command. |

### 2.2 What still runs on the CPU

| Work | Where | Notes |
|------|-------|-------|
| BSP traversal | `R_RecursiveWorldNode` | Still CPU. Has SoA visibility/flags helpers and profiler coverage under `RndWorld` / `RndFace`. |
| Edge insertion and AET walk | `R_ScanEdges`, `R_GenerateSpans_*`, `R_StepActiveU_Array`, `R_InsertNewEdges_Array` | Still CPU. Profiler reports `Insert`, `GenSpan`, `StepU`, and `DrwSurf`. |
| Surface cache build | `D_GpuLightSurface` (default) / `D_CacheSurface` -> `R_DrawSurfaceBlock8_mipN` (legacy) | T6 simple (`D_GPU_WORLD_LIGHT=1`) skips the per-texel rebuild — `D_GpuLightSurface` runs `R_BuildLightMap` and returns the per-surface light row that `D_DrawSpans8` puts in the span's `light` byte.  Legacy path builds pre-lit cache bytes texel-by-texel. |
| Lightmap build | `R_BuildLightMap` | CPU combines static and dynamic light into `blocklights[]`. |
| Alias setup | `R_AliasTransformAndProjectFinalVerts` | CPU transforms/project vertices and computes per-vertex light. GPU rasterizes only after projected vertices are ready. |
| Turbulent water/lava | `Turbulent8` / `D_DrawTurbulent8Span` | CPU sine-offset addressing and palette lookup. |
| Sky | `D_DrawSkyScans8` | CPU two-layer sky compositing. |
| Sprites | `D_DrawSprite` / `D_SpriteDrawSpans` | CPU. `d_sprite.c` includes `of_emit.h`, but the active sprite path emits no GPU commands. |
| Particles | `D_DrawParticle` | CPU transform, projection, z-test, and pixel writes. |
| 2D overlay | `draw.c`, `r_draw.c`, status/console/menu code | CPU writes to the framebuffer. |
| Audio drain | `snd_of.c` | The 1 kHz ISR drain was reverted. Current code drains synchronously from `SNDDMA_Submit()` and `SNDDMA_FillRing()` calls inside `R_EdgeDrawing`. |

## 3. Synchronization model

### 3.1 Ring mechanics

- `of_gpu.h` owns `_gpu_wrptr`, `_gpu_fence_next`, and the ring write state as file-static data.
- Only `vid_of.c` includes `of_gpu.h`; other engine files must go through `of_emit.h`.
- CPU writes command words through `GPU_RING_DATA`.
- The GPU only sees new work when `GPU_RING_WRPTR` is written.
- `of_gpu_finish()` submits a fence, uses the verified kick path, then waits with a bounded spin that traps on timeout.
- `_gpu_ring_ensure()` now publishes the write pointer if the ring is full before spinning for space, preventing the old producer deadlock.

### 3.2 Current publish/wait points

| Location | Call | Effect | Keep? |
|----------|------|--------|-------|
| `VID_Update` | `of_emit_finish()` before `of_video_flip()` | Required: the displayed buffer must not flip before queued rendering completes. | Yes. |
| `VID_Update` | `of_emit_clear(DEPTH)` + `of_emit_kick()` after rebind | Queues next frame's z clear and publishes the wrptr so the GPU can start while CPU does next-frame setup.  Kick added (T1). | Yes. |
| `D_DrawSpans8` | `of_emit_cache_clean(pbase, ...)` | Only fires on `D_GPU_WORLD_LIGHT=0` (legacy CPU-pre-lit cache).  Default path skips this — `D_GpuLightSurface` doesn't need a flush since it only writes lightmap data the GPU doesn't read. | Yes for legacy path. |
| `D_DrawNonSubdiv` | `of_emit_cache_clean(pskin, ...)` once per skin change | Required for GPU reads of alias skins.  Single owner of skin coherency (T3 dedupe shipped). | Yes. |

### 3.3 Idle hole — addressed by T1

`of_emit_kick()` is now exposed (`vid_of.c`, included via `of_emit.h`) and called after every meaningful submission boundary: `VID_Update`'s depth clear, per-surface in `D_DrawSpans8`, per-batch in `D_DrawNonSubdiv`.  Combined with the span batch accumulator (`vid_of.c`'s `flush_span_batch()`), CPU/GPU overlap during the frame is now the default — the GPU starts consuming each surface's spans while the CPU moves on to the next surface.

## 4. Rendering correctness status — gateware bugs (FIXED)

The three previously-blocking gateware bugs are all resolved in `openfpgaOS/src/fpga/common/gpu_core.v` and the deployed `runtime/bitstream.rbf_r` is byte-identical to the build that contains the fixes (md5 match against `openfpgaOS/build/Cores/ThinkElastic.openfpgaOS/bitstream.rbf_r`, both timestamped 2026-04-27 15:18).

- **Triangle perspective — FIXED.** `186bd37` (2026-04-25) "gpu: triangle perspective fixes (Bug A/B/C)" addressed three coupled issues: (A) `S_TRI_GRAD` writeback Q-format off-by-2^16, (B) `persp_pass` left at `PSS_PASS_TO_B` across rows (the PSS anchor handoff suspect), (C) `tri_det_sign` captured but never applied (CW-winding sign bug).  Follow-ups: `4425f12` (recip LUT 256→1024), `2666f2c` (Newton-Raphson recip refinement), `3d72ffd` (Q-format fix in `S_TRI_PERSP_PREMUL`).  Engine has `D_ALIAS_PERSP=1` enabled.

- **World `SPAN_PERSP` drift — FIXED.** `2666f2c` (2026-04-24) "gpu: add Newton-Raphson refinement to perspective recip path".  Commit body explicitly names the symptom: "Quake's world surface perspective drift on oblique surfaces.  The 1024-entry recip LUT (Phase 4b) gives only ~10-bit precision in 1/normalised; for very-small zi (1/z) values like Quake's 0.001..0.1 range, the bottom 3 bits of useful precision get lost."  Fix doubles relative precision via one N-R iteration `y1 = y0 * (2 - x*y0)`, taking 10-bit → ~20-bit (well past Q16.16's 16 fractional bits).  ~0.75 cycle/pixel overhead.

- **`sp_tex_w_mask` / `sp_tex_h_mask` state-bleed — FIXED.** `gpu_core.v:3539-3540` now resets both masks to `16'hFFFF` at triangle span emit, with a comment matching the original bug description verbatim.  Engine no longer needs to send `mask=0` defensively — POT-wrap addressing on world spans is reachable.

These fixes unblock T6 (world rendering on the GPU) and remove the constraint on POT-wrap-mask-aware optimisations.

## 5. Review findings to clean up

- `of_emit_blend()` / `of_emit_shade_gouraud()` / `of_emit_blend_t` and the matching SDK helpers are already gone — the wrappers were retired alongside the SDK's reserved blend/shade commands.
- `of_dbg_verify_cmap_row0()` and the `/* GPU up, GEQUAL, Gouraud on */` inline comment in `VID_Init` are removed (T0).
- `vid.fullbright` reads byte at `VID_GRADES * 256` with a heuristic for both `marker = start_index` (id1) and `marker = count` (this PAK's) conventions — landed already, but worth documenting that `vid.fullbright = 32` is the canonical value and any other value indicates a non-standard `colormap.lmp`.
- `sram_fill_start()` / `sram_fill_wait()` are stubs in `sys_of.c`, so the `Z-clr wt` profiler row should be effectively zero. The call sites also use the old PocketQuake argument order; if a real fill engine is reintroduced, fix them before enabling it.
- `GPU_CMD_DRAW_SPANS_BATCH` (`0x41`) is now shipped in the SDK (PocketDukeNukem-SDK uses it in production via `d3d_gpu.c`'s `span_buf[]` accumulator). `vid_of.c` accumulates spans through `of_emit_span()` and dispatches via `of_gpu_draw_spans_batch()`; flushes happen on `bind_fb`/`bind_texture`/`clear`/`triangles*`/`kick`/`finish` so command-ring ordering is preserved.
- `of_gpu_draw_triangles_batch()` exists in the SDK, but `of_emit.h` does not expose it and `D_DrawNonSubdiv()` currently emits one triangle command at a time.

## 6. Optimization catalog

Ordered by impact-per-effort for the current code.

### T0. Fix stale wrappers/comments/debug hooks — DONE

`of_emit_blend()` / `of_emit_shade_gouraud()` / `of_emit_blend_t` were removed in an earlier pass.  The 226-line one-shot `of_dbg_verify_cmap_row0()` cmap diagnostic + the stale `/* GPU up, GEQUAL, Gouraud on */` inline comment in `VID_Init` are now gone too.  Net: -4.6 KB text, -17 KB bss.

### T1. Add `of_emit_kick()` and publish draw batches — DONE

`of_emit_kick()` lives in `vid_of.c`/`of_emit.h` (`vid_of.c:316`).  Kicks fire after `VID_Update`'s depth clear (`vid_of.c:504`), after each world surface in `D_DrawSpans8` (`d_scan.c:361`), and after each alias triangle batch in `D_DrawNonSubdiv` (`d_polyse.c:376, 383`).  Surface-cache lifetime risk noted in T7 — has not bitten yet under the 2 MB cache.

### T2. Remove the two bmodel `of_emit_finish()` calls — DONE

The drains around `R_DrawBEntitiesOnList()` are gone (`r_main.c:1271-1278`).  No regressions observed on doors, lifts, or rotating brushes.

### T3. De-duplicate alias skin cache flushes — DONE

The duplicate `D_PolysetUpdateTables()` flush is removed (`d_polyse.c:521-524` documents it).  `D_DrawNonSubdiv()` is the single owner of GPU skin coherency, gated on `last_skin_flushed` so it only flushes on actual skin changes.

### T4. Batch alias triangles — DONE

`D_DrawNonSubdiv` (`d_polyse.c:309-384`) accumulates up to `BATCH_TRIS_MAX=128` triangles into `batch_buf[]` and dispatches via `of_emit_triangles_batch`, which fronts `of_gpu_draw_triangles_batch`.  Per-frame ring header / decode overhead is now amortised across the model.

### T5. Reduce surface-cache flush cost (effort: small/medium, impact: medium)

`D_DrawSpans8()` calls `OF_SVC->cache_clean_range()` once per drawn world surface. Options:

- benchmark cacheable surface-cache writes plus per-surface clean versus an uncached cache arena;
- batch cleans only if doing so does not defeat T1 overlap;
- add a cheaper user-mode cache-maintenance primitive if the runtime exposes one.

Expected win: depends on syscall cost and cache-miss rate. Measure with `pq_cycleprof 2`.

Risk: uncached writes may be slower than cached writes plus clean.

### T6. World rendering on the GPU — UNBLOCKED, simple variant SHIPPED

**Simple variant — DONE.** `D_GPU_WORLD_LIGHT=1` is the default; `D_GpuLightSurface` replaces the per-texel CPU surface-cache build with one `R_BuildLightMap` + a single colormap-row byte per surface that the GPU consumes via `OF_EMIT_COLORMAP`.  Saves the per-texel CPU lookup (~5.5 cyc/texel × ~422k texels/frame on the dense scene) — confirmed by the per-bucket numbers: `CachSrf` is now 5.4 ms (cache-management work + lightmap build), down from the texel-rebuild cost the legacy path showed.

**Better variant — DEFAULT WORLD PIXEL PATH.** Normal world surfaces now tessellate into triangles with per-vertex Gouraud-interpolated light and dispatch through the triangle path.  This replaces `D_CalcGradients` + `D_DrawSpans8` for normal world pixels. `R_ScanEdges` and `D_DrawZSpans` still run for visibility and CPU-side sprite/alias depth until GPU depth returns.

  - Eliminates now: normal-world `CalcGrd` + `D_DrawSpans8`.
  - Still costs now: `R_ScanEdges`, AET stepping, and `D_DrawZSpans`. During this phase `R_ScanEdges` and `R_DrawSurfaceTris` both run, so frame time can temporarily improve modestly or even rise until GPU depth lets the AET path go away.
  - Adds: `R_DrawSurfaceTris` cost — emit ~2-4 triangles per visible surface = ~1500 vertices/frame on the dense scene, comfortably under the existing alias triangle-batch budget.  Estimate +5-8 ms of new CPU work.
  - Target after GPU depth + AET removal: 104.8 ms → ~30 ms = ~33 FPS on the dense scene.  Sparse scene → ~50 FPS.

Phased implementation:

  - **Phase 1 — scaffolding (DONE).**  `R_DrawSurfaceTris(msurface_t *fa, int miplevel)` exists in `d_scan.c`; the compile-time world-span fallback has been removed from the normal world path.
  - **Phase 2 — vertex collection (DONE).**  `R_CollectSurfaceVerts` walks `msurface_t::firstedge .. +numedges` via `currententity->model->surfedges` + `pedges`, mirroring `R_RenderFace`'s negative-surfedge handling.  Per vertex: `VectorSubtract(world, modelorg)` + `TransformVector` → camera space; `1/z` + `xscale * x/z + xcenter` → screen space; `DotProduct(world, texinfo->vecs[0/1]) + offset` → absolute texel coords.  Output: stack-local `r_tri_vert_t poly[32]` carrying world xyz, camera xyz, screen u/v, 1/z, and absolute s/t.  Near-plane clamp inside the collector (`z = max(z, NEAR_CLIP)`) is a Phase 3 placeholder.  Flag-on build verified clean.
  - **Phase 3 — frustum-near clip (DONE).**  `R_NearClipPoly` runs Sutherland-Hodgman against camera-space `z = NEAR_CLIP`.  Walks consecutive vertex pairs, emits intersection vertices via `R_NearPlaneInterp` (linear lerp in world / cam / s / t; recomputes screen u,v,1/z fresh from the interpolated cam since perspective is nonlinear).  Output ≤ n_in+1 vertices; `< 3` returns early.  Caller passes a pair of `r_tri_vert_t[N+1]` ping-pong buffers.
  - **Phase 4 — tessellation + UV/light.**  Triangle-fan from vertex 0; per vertex compute (s,t) from `texinfo->vecs` and sample the lightmap at the equivalent luxel for the Gouraud `r=g=b` byte (mirror `R_BuildLightMap` math but per-vertex).  Pack into `of_emit_vertex_t`.
  - **Phase 5 — dispatch + wire-up (DONE).**  `R_DrawSurfaceTris` binds the surface's mip texture via `of_emit_bind_texture` (animated textures resolved through `R_TextureAnimation`), builds a triangle fan from `verts[0]`, dispatches through one-triangle commands, and is now the unconditional normal-world surface path in `d_edge.c`.  `D_DrawZSpans` still runs unconditionally to populate the CPU z-buffer (sprite/alias depth — triangle path doesn't co-write z post Phase 2.3). Large-count `of_emit_triangles_batch` remains an RTL validation item before world rendering should depend on it.
  - **Phase 6 — quality fixes.**  For surfaces where the lightmap varies sharply (long walls with multi-luxel light), the triangle-fan approximation produces visible Mach banding.  Subdivide such surfaces into a 4×4 or 8×8 grid (lightmap-luxel-aligned).  Detect via lightmap variance threshold.

Each phase is a separable commit.  Hardware visual testing is required from Phase 5 onward; the plan can't be completed in one session without that loop.

Risk: lighting correctness on highly-curved or multi-lightmap surfaces.  Quake's lightmap is per-luxel (16-pixel grid); Gouraud per-vertex tessellation approximates that.  Phase 6 covers the regression that's most likely to need addressing.

### T7. Add GPU lifetime awareness to surface-cache eviction (effort: medium, impact: correctness for T1/T6)

The current surface cache has no notion of "GPU may still read this cache block." Before becoming more aggressive with kicks or cache reuse, add one of:

- a fence/wait before reusing a cache block that has been emitted this frame;
- per-frame pinning for emitted cache entries;
- a transient render cache arena for newly built surfaces.

Expected win: mostly correctness. It also lets T1 run without relying on "2 MB is probably enough."

Risk: waiting on eviction can reintroduce stalls if the cache thrashes.

### T8. BRAM placement of hot text — TRIED, INCONCLUSIVE (reverted)

Bound `PQ_FASTTEXT` to `__attribute__((section(".app_fasttext.pq")))`, curated 20 functions into APP_BRAM.  The "regression" originally reported here (-43 ms) was a **methodology error** — the "before" capture was at AET total 151 (sparse view) and the "after" capture was at AET 846 (dense view, 5.6× denser).  Once compared on the same dense scene:

| Build                          | Time (AET 846) |
|--------------------------------|----------------|
| `-O2`, no BRAM placement       | 113.1 ms       |
| `-O2`, T8 BRAM placement       | 111.5 ms       |
| `-O3 + LTO`, no BRAM placement | 98.3 ms        |

T8 was a marginal ~1.5 ms win, not a regression — but small enough not to be worth the complexity (curation maintenance, mirror drift with the SDK, the 14 KB budget being tied up).  Reverted to keep `PQ_FASTTEXT` empty.  In-source annotations stay so future I-cache-counter-aware tuning can re-enable selectively.  `PQ_HOT` confirmed truly no-op (113.1 → 113.1 with and without).

Lesson: capture the AET / poly / fc line on every measurement; only compare frames at matching scene complexity.

### T9. Turbulent water/lava on GPU (effort: large, impact: scene-dependent)

`D_DrawTurbulent8Span()` uses sine-offset texture addressing that the current GPU does not support. This needs a gateware addressing mode or a precomputed/intermediate texture path.

Expected win: useful only in water/lava-heavy scenes.

Risk: high gateware/API complexity.

### T10. Sprites on GPU (effort: medium, impact: low)

Sprites are still CPU. A two-triangle path is not enough by itself because Quake sprite transparency uses palette index 255, while the current GPU API has no alpha-test command and only exposes span `SKIP_ZERO`.

Expected win: small; sprites are uncommon.

Risk: transparency correctness unless assets are remapped or the GPU gains an alpha-ref path.

### T11. Particles on GPU (effort: medium, impact: low)

Particles are many tiny z-tested rectangles. Submitting each as GPU geometry likely costs more ring overhead than it saves. A batched y/span path might help, but this is low priority.

## 7. Recommended order

Status: T0–T4 shipped, T6 better is now the default normal-world pixel path, T8 inconclusive (~1.5 ms win not worth the complexity).  §4 gateware bugs all fixed in deployed bitstream.  Reference captures with `-O3 + LTO`: dense scene (AET 846) 104.8 ms / ~10 FPS; sparse scene (AET 151) 68.2 ms / ~14 FPS.

Remaining ranking:

1. **GPU depth + AET removal.**  T6 better still pays `R_ScanEdges` and `D_DrawZSpans`; removing those requires GPU depth first.
2. **T5** — surface-cache flush ecall cost is on the order of hundreds of calls per frame on legacy span paths; worth re-measuring with `pq_cycleprof 2` after triangle-world hardware captures.
3. **T7** — correctness work for surface-cache GPU lifetime.  Less urgent now that normal world surfaces no longer use the legacy surface cache path.

Beyond the catalog: algorithmic improvements to `R_RecursiveWorldNode` (BSP traversal) and `R_RenderFace`'s edge-emit half are still tractable but small (~5-8 ms estimated).  Higher CPU clock (gateware-only timing-closure work) gives a flat 25% across all buckets.  Adding I-cache miss/stall counters to the fabric would unblock measured tuning so future T8-style experiments aren't speculative.

## 8. Open questions

- `pq_cycleprof 2` on hardware (`-O3 + LTO`, dense reference scene at AET 846 / poly 412 / fc 511): ~10 FPS / 98.3 ms.  EdgeDraw 89.5 ms (91%) of which ScanEdge 64.2 ms (65%) and RndWorld 24.3 ms (25%); DrwSurf inside ScanEdge 20.9 ms.  Alias 3.9 ms, Entities 6.3 ms, ViewModel 1.4 ms.  HW counters stay 0 (no fabric counters on this build).
- Sparser view (AET 151 / poly 83) on the same build runs ~14 FPS / 68.2 ms.  Frame time is overwhelmingly geometry-driven; ScanEdge cost scales roughly with AET total.  **Always capture the `AET:peak/total fc:N poly:N` line — frames at different AET totals are not comparable.**
- ~Is alias colormap/light interpolation visually correct with `D_ALIAS_GOURAUD=1`?~ **Yes** — the perspective + Gouraud + colormap path is correct on the deployed bitstream after the §4 fixes landed.  Engine has `D_ALIAS_PERSP=1` and `D_ALIAS_GOURAUD=1` enabled by default.
- Does `r_cache_thrash` ever trip during normal movement with the 2 MB surface cache? (Untested. Important for T1/T7.)
- How often does `_gpu_ring_ensure()` publish because the ring filled before a frame-end finish? (Untested. Affects T1 sizing.)
- Should the startup colormap diagnostic remain in normal builds? (No — answered, gate behind `PQ_DEBUG_CMAP`.)
- Will the gateware gain `of_gpu_draw_triangles_batch` exposed via `of_emit.h` for T4? (Currently SDK-internal.)
