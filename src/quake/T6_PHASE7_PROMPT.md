# T6 Phase 7 — Bypass the AET for world rendering

Drop `R_ScanEdges` from the world-rendering critical path entirely.
Drive surface emission directly from BSP traversal so triangles dispatch
as soon as a face is determined visible, not after a full edge-table
walk.  This is the change that finally turns T6-better's promised CPU
savings (~30 ms per frame on the dense reference scene) from
theoretical into measured.

## Why this is needed

Phase 5 wired `R_DrawSurfaceTris` into `D_DrawSurfaces` and saved
~5.5 ms of `D_CalcGradients` work but adds ~5 ms of per-surface CPU
triangle prep — net wash.  The actually-large bucket is the AET
maintenance:

```
ScanEdge  74-79 ms   (= 65-70 % of frame)
  Insert      3.5
  GenSpan    11.0
  StepU       4.7
  Remove      4.6
  DrwSurf    22.2     ← this stays under T6-better
  Final      23.1     ← this stays under T6-better
  SE.Other   10.3
```

Of those ~75 ms, the ~46 ms in DrwSurf+Final is the per-surface CPU
work that any path must do (vertex collection + clip + pack + dispatch).
The remaining **~30 ms (Insert + GenSpan + StepU + Remove + SE.Other)
is pure AET maintenance** — and is exactly the work this phase
eliminates.

The CPU is fully utilised today (`GPU wait = 0`, `FrameGap = 9 ms`,
`VidFlip = 0 ms`).  Any meaningful FPS jump requires either reducing
geometry, raising the clock, or removing the AET.  This phase is the
last option.

## Current state on disk

- `D_GPU_WORLD_TRIS = 1` default in `src/quake/engine/d_local.h`.
- `R_DrawSurfaceTris` in `src/quake/engine/d_scan.c` does
  collect → clip → pack → texture-bucketed-batch dispatch (Phases 2-5
  + texture batching).
- `R_FlushWorldTriBatch` declared in `d_local.h`, called from
  `D_DrawSurfaces` end.
- New profile probes live: `Remove`, `Final`, `SE.Other` (derived),
  `GPU wait`, `VidFlip`, `FrameGap`.

There IS a known visual issue (texture corruption on tiled surfaces) —
do NOT block on fixing it before this phase.  Post-Phase-7, the
working geometry path will be different and the corruption may resolve
or move.  Keep `D_GPU_WORLD_TRIS=1` as default.

## What to do

### Step 1 — Find the BSP-emit hook

`R_RecursiveWorldNode` (`src/quake/engine/r_bsp.c:527`) calls
`R_RenderFace` per visible face.  `R_RenderFace`
(`src/quake/engine/r_draw.c:396`) currently:

  1. Allocates a `surf_t` slot.
  2. Walks the face's edges, calling `R_ClipEdge` (which inserts
     edges into `r_edges[]` for the AET).
  3. Returns; the surface is later picked up by `R_ScanEdges`.

For Phase 7, when `D_GPU_WORLD_TRIS=1`, `R_RenderFace` should:

  1. Skip the edge insertion.
  2. Call directly into a new `R_DispatchSurfaceTris(msurface_t *fa)`
     that does what `R_DrawSurfaceTris` does today but takes the
     miplevel parameter inline.
  3. NOT allocate a `surf_t` slot (the slot only exists to be picked
     up by `R_ScanEdges`).

The miplevel is currently computed in `D_DrawSurfaces` from
`s->nearzi * scale_for_mip * pface->texinfo->mipadjust`.  Move that
calculation into the new direct-dispatch path.

### Step 2 — Make `R_ScanEdges` a no-op for world surfaces

Two options, pick whichever is cleaner:

(a) Skip the entire `R_ScanEdges` call from `R_EdgeDrawing`
    (`src/quake/engine/r_main.c:1255`) under `D_GPU_WORLD_TRIS=1`.

(b) Keep `R_ScanEdges` running but make `D_DrawSurfaces` a no-op
    (since surfaces have already been dispatched directly).

Probably (a) is cleanest since `R_ScanEdges` itself is the bulk
(`Insert`, `GenSpan`, `StepU`, `Remove` all happen inside it).

### Step 3 — Replace `D_DrawZSpans`

`D_DrawZSpans` populates the CPU z-buffer (`d_pzbuffer`) for
sprite/alias CPU-side depth tests.  It's currently called from
`R_ScanEdges` (line ~`r_edge.c:1413` final-scan area) per scanline of
each surface span.

Without `R_ScanEdges`, no `D_DrawZSpans`.  Sprites and alias models
will have wrong depth.

Two options:
- **Quick path:** rasterise z per-face directly during the BSP walk,
  using the same per-vertex `1/z` Phase 4 already computes.  CPU-side
  z-fill, but per-face instead of per-scanline-span.  Probably faster
  than the current per-span-AET-z-fill since you skip the AET.
- **Architectural:** make sprites/aliases use BSP-back-to-front
  ordering for visibility instead of z-buffer occlusion (they already
  are clipped against the BSP for view-frustum culling — extending
  that to occlusion is non-trivial but doable).

Start with the quick path.

### Step 4 — Keep correctness invariants

- BSP back-to-front order MUST be preserved for transparent surfaces
  (water, lava) and for occlusion correctness on world.
- Bmodels (doors, lifts, etc.) live in `R_DrawBEntitiesOnList` which
  also runs `R_RenderFace`.  Same dispatch path applies.
- The texture-bucketed batch (`R_FlushWorldTriBatch`) must still flush
  before any state-change boundary.  Since BSP traversal is depth-first
  and emits faces in deterministic order, the existing flush-at-
  D_DrawSurfaces-end logic transitions to flush-at-RenderWorld-end.

### Step 5 — Measure

Before changes: capture `pq_cycleprof 2` on the AET-846 dense scene.
Should match the most recent capture (~114 ms, `Insert+GenSpan+StepU+
Remove+SE.Other = ~30 ms`).

After changes: same scene capture.  Target: `Insert + GenSpan +
StepU + Remove + SE.Other ≈ 0 ms` (these probes still exist but
won't fire if `R_ScanEdges` is skipped).  `Total` should drop from
~114 to ~80-85 ms.  Expected FPS jump: 9 → 12-13.

If `Total` doesn't drop by ~30 ms, something is still routing
through the AET.  Bisect.

## What NOT to do

- Don't touch the SDK / `of_gpu.h` / gateware.  This is purely engine.
- Don't try to fix the texture corruption first.  See above.
- Don't remove `R_ScanEdges` source — gate it on `D_GPU_WORLD_TRIS`
  so the legacy path remains buildable.
- Don't restructure `R_RecursiveWorldNode` itself.  The dispatch
  point is downstream of it.

## Reference files

- `src/quake/engine/r_bsp.c:527` — `R_RecursiveWorldNode`
- `src/quake/engine/r_draw.c:396` — `R_RenderFace`
- `src/quake/engine/r_main.c:1210` — `R_EdgeDrawing` (calls
  `R_ScanEdges`)
- `src/quake/engine/r_edge.c:1273` — `R_ScanEdges` (the loop to skip)
- `src/quake/engine/r_edge.c:1411` — final-scan `D_DrawSurfaces` call
- `src/quake/engine/d_edge.c:183` — `D_DrawSurfaces` (per-surface
  dispatch — extract miplevel calc from here)
- `src/quake/engine/d_scan.c` — `R_DrawSurfaceTris` and texture-batch
  state
- `src/quake/optimizations.md` §6 T6 — phased plan (this is Phase 7)

## Hardware testing required

After Step 4, test on hardware in at least:

- A simple corridor (head-on walls).
- A room with oblique walls (Block-2 stress, but should be fine
  post-gateware-fix).
- A scene with multiple bmodels (doors, lifts).
- A scene with sprites/aliases visible (z-buffer correctness).

If any look wrong, post a screenshot and the `pq_cycleprof 2` output.
