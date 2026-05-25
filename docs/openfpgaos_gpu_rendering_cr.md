# Change Request: openfpgaOS GPU fixes needed by PocketQuake

## Summary

PocketQuake can now use the openfpgaOS GPU param-span-list path for world color and Quake-compatible z writes, but three OS/SDK issues still block fully correct and fast rendering:

1. `CMD_DRAW_PARAM_SPAN_LIST` perspective needs stronger Quake regression coverage and may still have precision/correctness gaps with realistic Quake planes.
2. GPU alias/sprite acceleration needs a real z-tested path against Quake's 16-bit `d_pzbuffer`; current triangle rendering ignores z.
3. The triangle texture-coordinate ABI is inconsistent: the SDK exposes 32-bit 16.16 `s/t`, while the RTL currently consumes only the upper 16 bits.

Quake must not force any new capability bits. Every new path needs an explicit runtime cap and a CPU fallback.

## Current Quake behavior

Quake currently:

- Uses `OF_HW_GPU_PARAM_SPAN_LIST` for world surface color spans.
- Uses `OF_HW_GPU_PARAM_SPAN_Z` only with `z_mode = OF_GPU_PARAM_Z_WRITE_ZI` for opaque world spans.
- Forces a GPU finish before CPU alias/sprite rendering reads `d_pzbuffer`.
- Keeps world aliases CPU-z-tested because current GPU triangles cannot test against `d_pzbuffer`.
- Uses GPU affine spans for the viewmodel/no-z path only, because it renders last and deliberately skips z.

The code paths involved are:

- `src/quake/engine/d_scan.c`: world param spans and z-write setup.
- `src/quake/engine/d_polyse.c`: alias CPU/GPU paths.
- `src/quake/engine/d_sprite.c`: sprite CPU/GPU paths.
- `src/quake/engine/vid_of.c`: openfpgaOS GPU bridge and runtime caps.
- `src/sdk/include/of_gpu.h` and `src/sdk/include/of_caps.h`: SDK ABI/caps.

## CR-1: Param-span perspective correctness and precision tests

### Problem

World perspective has been observed wrong in Quake, especially when looking away from straight ahead. Quake reduced the error by submitting param spans with local surface-relative `u/v` coordinates and rebased framebuffer/z/attribute origins, which avoids large absolute screen-coordinate products. That is a useful mitigation, but the OS should still guarantee that `CMD_DRAW_PARAM_SPAN_LIST` produces the same pixels as the older perspective span path for valid absolute and rebased coordinates.

Relevant RTL behavior in `../openfpgaOS/src/fpga/common/gpu_core.v`:

- `param_launch_attr_mul` multiplies record `u/v` by `attr_du/attr_dv`.
- `S_PARAM_CAPTURE` computes:
  - `attr = attr_origin + u * attr_du + v * attr_dv`
  - `fb = fb_base + u/v framebuffer steps`
  - optional z address from `z_base + u/v z steps`.

### Required OS work

- Add acceptance tests comparing `CMD_DRAW_PARAM_SPAN_LIST` against `CMD_DRAW_PERSP_SPAN_GROUP` and a CPU reference for realistic Quake inputs.
- Include both absolute screen records and rebased local records that should produce identical pixels.
- Test non-zero `sadjust/tadjust`, non-zero `d_sdivzstepv/d_tdivzstepv/d_zistepv`, camera yaw/pitch-like cases, and large surfaces near screen edges.
- Test with `z_mode = OF_GPU_PARAM_Z_NONE` and `z_mode = OF_GPU_PARAM_Z_WRITE_ZI`.
- If tests fail, fix the RTL arithmetic width, sign handling, rounding, or pipeline ordering so param spans match the reference.

### Acceptance criteria

- Param-span output matches the CPU reference and 0x46 perspective span output for the same Quake surface within exact paletted pixel equality.
- Rebased local-coordinate submissions and absolute-coordinate submissions produce identical framebuffer and z-buffer results.
- `CMD_FENCE` / `of_gpu_finish()` guarantees all param-span color and z writes are visible to the CPU.

## CR-2: Add z-tested GPU path for aliases and sprites

### Problem

The current GPU triangle path cannot correctly render Quake aliases behind walls. The triangle payload includes a z word, but the RTL comments and implementation indicate triangle depth is ignored:

- `../openfpgaOS/src/fpga/common/gpu_core.v`: triangle load comment says `word 1: {z, --} (depth is ignored)`.
- Triangle span emission sets `sp_z_write_enable <= 1'b0`.
- Quake's `of_emit_depth_test()` bridge is currently a no-op because there is no generic depth-test command exposed by the active SDK/API.

As a result, GPU-drawn enemies can render through walls even though the world param-span path has populated `d_pzbuffer`.

### Required OS work

Provide one of these GPU paths, with a new explicit capability bit:

Option A, preferred for alias models:

- Add z-test/z-write support to `CMD_DRAW_TRIANGLES`.
- Use the existing vertex z field or a documented replacement field.
- Support Quake-compatible 16-bit z values.
- Support `GEQUAL` compare semantics for Quake aliases: draw if incoming z is greater than or equal to the current z-buffer value, then write incoming z.
- Use a z-buffer base pointer and byte stride compatible with `d_pzbuffer`.
- Do not write color or z for discarded/keyed transparent pixels.

Option B, likely simpler and still useful:

- Add a z-tested affine span-list command for CPU edge-walked aliases/sprites.
- Header carries framebuffer/z/texture state.
- Records carry `fb_addr`, `z_addr`, `count`, `s/t`, `s/t step`, `zi`, `zi step`, light, flags.
- Support z test and optional z write.
- Support transparent-key discard before z write for sprites/masked spans.

Suggested caps:

- `OF_HW_GPU_TRI_Z` for z-tested triangle rendering.
- `OF_HW_GPU_AFFINE_SPAN_Z` or equivalent for z-tested affine span rendering.
- Do not overload `OF_HW_GPU_TRIANGLE`; current triangle hardware is not sufficient for Quake occlusion.

### Acceptance criteria

- Enemy behind an opaque wall does not render through the wall.
- Enemy in front of a wall renders and updates z consistently.
- Viewmodel/no-z rendering can still bypass z test/write.
- Transparent sprite pixels do not update z.
- CPU and GPU alias/sprite renders match on a small deterministic scene.

## CR-3: Resolve triangle texture-coordinate ABI mismatch

### Problem

The SDK vertex ABI says triangle `s` and `t` are 32-bit 16.16 fixed-point values:

- `of_gpu_vertex_t.s`
- `of_gpu_vertex_t.t`

The RTL triangle payload load currently consumes only `ring_rd_data[31:16]` for `v_s` and `v_t`. That makes the effective ABI a 16-bit integer/high-half field, not 16.16. This likely contributes to noisy or incorrect alias textures when the GPU triangle path is enabled.

### Required OS work

- Decide and document the actual triangle `s/t` ABI.
- Preferred: keep the SDK ABI as 32-bit 16.16 and update RTL to consume all 32 bits.
- If the hardware intentionally only supports 16-bit integer `s/t`, update `of_gpu.h`, docs, tests, and downstream callers so they do not submit 16.16 values.
- Add tests using Quake-style alias skins, including non-power-of-two dimensions such as 224x64 and seam-fixup coordinates.

### Acceptance criteria

- SDK documentation, firmware API, and RTL payload decode agree.
- A textured triangle with fractional 16.16 coordinates samples the same texels as the documented reference.
- Non-power-of-two alias skins render without random/noisy texels.

## CR-4: Define triangle texture bounds/wrap behavior

### Problem

Triangle rasterization currently sets texture masks to `16'hFFFF`, effectively no POT wrap/clamp, while the SDK texture state only carries width/height. For Quake aliases this may work only if interpolation never steps slightly outside the skin bounds. In practice, edge rounding and seam fixups can expose out-of-bounds reads as noisy texels.

### Required OS work

- Define whether triangle sampling clamps, wraps, or requires caller-guaranteed in-bounds coordinates.
- Prefer explicit texture state for wrap/clamp mode, or clamp to `[0, width - 1]` and `[0, height - 1]` for indexed alias textures.
- Add tests for edge pixels on non-power-of-two textures.

### Acceptance criteria

- Triangle texture sampling never reads outside the bound texture allocation for documented in-range input.
- Edge and seam triangles on Quake alias skins are stable and deterministic.

## CR-5: SDK/capability cleanup

### Problem

The SDK/API needs clearer caps for the actual depth functionality:

- `OF_HW_GPU_PARAM_SPAN_Z` means param-span z write only.
- It does not imply generic depth test, triangle z test, or affine span z test.
- Old tests or comments mention `CMD_SET_DEPTH_FUNC` / `CMD_SET_ZB`, but the active firmware SDK does not expose a working generic depth path for Quake.

The SDK should also expose stable constants for perspective span group lane limits:

- `OF_GPU_PERSP_SPAN_GROUP_MAX_NATIVE_LANES = 4`
- `OF_GPU_PERSP_SPAN_GROUP_MAX_LANES = 8`

### Required OS work

- Add distinct caps for every new z-tested primitive.
- Keep `OF_HW_GPU_PARAM_SPAN_Z` scoped to param-span z writes unless the implementation changes.
- Ensure docs, `of_caps.h`, `of_gpu.h`, runtime caps, and RTL feature bits agree.
- Add the perspective span lane constants to the firmware SDK API.

### Acceptance criteria

- Quake can select all GPU paths exclusively from runtime caps.
- No app needs to force a capability bit to reach a path.
- SDK headers compile with static asserts for command opcodes, payload sizes, lane limits, and capability bit positions.

## Suggested OS test scenes

1. World perspective regression:
   - Slanted floor/wall with checker texture.
   - Camera straight ahead, yawed, pitched up/down.
   - Compare CPU reference, 0x46, and 0x48 param spans.

2. World z-write regression:
   - Draw opaque world spans with `OF_GPU_PARAM_Z_WRITE_ZI`.
   - Fence.
   - CPU reads `d_pzbuffer` and verifies Quake-compatible 16-bit values.

3. Alias occlusion:
   - Draw wall z first.
   - Draw alias triangle/span behind wall.
   - Verify no color or z changes behind wall.
   - Move alias in front.
   - Verify color and z update.

4. Transparent sprite z behavior:
   - Draw keyed sprite in front of wall.
   - Verify key pixels do not update color or z.
   - Verify opaque pixels pass/fail z correctly.

5. Alias texture ABI:
   - Render a triangle using 16.16 `s/t` with fractional endpoints.
   - Render a non-power-of-two skin, e.g. 224x64.
   - Verify no out-of-range texture reads and no random/noisy texels.

