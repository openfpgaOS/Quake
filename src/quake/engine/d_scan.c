/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
// d_scan.c — portable C scan-level rasterization.
//
// openfpgaOS port: world spans are emitted to the GPU via
// of_emit_span() from of_emit.h.  Lighting goes through one of two
// paths gated by D_GPU_WORLD_LIGHT and pq_gpu_world_light: the safe
// default pre-lights the surface cache CPU-side and submits raw bytes
// (the GPU runs no colormap so we don't double-index already-lit
// pixels), while the GPU path (=1) ships unlit cache + per-span
// shade and lets the GPU's COLORMAP lookup do the lighting.  The
// CPU-side z-buffer is populated by D_DrawZSpans (below), or by the
// param-span GPU z-write path when the runtime bitstream advertises it.

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"
#include "of_emit.h"
#include "of_cache.h"

/* Choose GPU perspective vs. CPU per-16-pixel affine. The perspective
 * path emits one span per scanline — ~16x less ring traffic. */
#ifndef D_USE_GPU_PERSP
#define D_USE_GPU_PERSP 1
#endif

/* T6 — GPU world surface lighting. When 1, world spans go to the GPU
 * with raw mip textures + OF_EMIT_COLORMAP and a per-surface light
 * byte. The CPU skips R_DrawSurfaceBlock8_mipN entirely (no per-pixel
 * texel × lightmap × colormap loop on CPU). When 0, falls back to the
 * pre-lit surface cache (the legacy path). */
#ifndef D_GPU_WORLD_LIGHT
#define D_GPU_WORLD_LIGHT 1
#endif

/* D_GPU_WORLD_TRIS is defined in d_local.h so d_edge.c's per-surface
 * dispatch sees the same value. */

/* Per-surface colormap row index 0..63. Set in d_edge.c by
 * D_GpuLightSurface() right before each surface's D_DrawSpans8 call,
 * embedded in the span's `light` field below. */
byte pq_world_light;
unsigned short pq_world_tex_w_mask;
unsigned short pq_world_tex_h_mask;
int pq_world_tex_s_offset;
int pq_world_tex_t_offset;

unsigned char	*r_turb_pbase, *r_turb_pdest;
fixed16_t		r_turb_s, r_turb_t, r_turb_sstep, r_turb_tstep;
int				*r_turb_turb;
int				r_turb_spancount;
unsigned int	pq_prof_spans8_cycles_frame;
unsigned int	pq_prof_spans8_calls_frame;
unsigned int	pq_prof_zspans_cycles_frame;
unsigned int	pq_prof_zspans_calls_frame;
extern cvar_t	pq_cycleprof;
extern cvar_t	pq_gpu_zwrite;
extern cvar_t	pq_gpu_world_light;
extern cvar_t	pq_gpu_persp;
extern cvar_t	pq_gpu_param;

int		pq_combined_z_active;  /* tells d_edge.c to skip D_DrawZSpans when 1 */
int		pq_gpu_zwrite_pending; /* GPU wrote d_pzbuffer; CPU z readers need a retire barrier */

static of_emit_param_span_record_t pq_param_span_records[OF_EMIT_PARAM_SPAN_MAX_RECORDS];

void D_DrawTurbulent8Span (void);

static inline int32_t D_SpanMad2I32 (int32_t origin,
	int32_t stepv, int v, int32_t stepu, int u)
{
	return (int32_t)((uint32_t)origin
		+ (uint32_t)stepv * (uint32_t)v
		+ (uint32_t)stepu * (uint32_t)u);
}

static inline int64_t D_ShrTrunc64 (int64_t value, unsigned shift)
{
	if (shift == 0)
		return value;
	if (value >= 0)
		return value >> shift;

	uint64_t mag = (uint64_t)(-(value + 1)) + 1u;
	return -(int64_t)(mag >> shift);
}

static inline int64_t D_FloatToScaledI64 (float value, int scale)
{
	union { float f; uint32_t u; } in = { value };
	uint32_t bits = in.u;
	uint32_t sign = bits >> 31;
	uint32_t exp = (bits >> 23) & 0xffu;
	uint32_t frac = bits & 0x7fffffu;
	uint64_t mant;
	uint64_t mag;
	int shift;

	if (exp == 0xffu)
		return sign ? INT64_MIN : INT64_MAX;
	if (exp == 0) {
		if (frac == 0)
			return 0;
		mant = frac;
		shift = -126 - 23 + scale;
	} else {
		mant = (1u << 23) | frac;
		shift = (int)exp - 127 - 23 + scale;
	}

	if (shift >= 0) {
		if (shift >= 63 || mant > (UINT64_MAX >> shift))
			mag = UINT64_MAX;
		else
			mag = mant << shift;
	} else {
		unsigned rshift = (unsigned)-shift;
		mag = rshift >= 64 ? 0 : (mant >> rshift);
	}

	if (!sign) {
		if (mag > (uint64_t)INT64_MAX)
			return INT64_MAX;
		return (int64_t)mag;
	}
	if (mag > (uint64_t)INT64_MAX + 1u)
		return INT64_MIN;
	return -(int64_t)mag;
}

static inline int32_t D_Q29FromQ45 (int64_t q45)
{
	return (int32_t)D_ShrTrunc64(q45, 16);
}

static inline int32_t D_Q29Numerator (float divz, fixed16_t adjust,
	int64_t zi_q45)
{
	int32_t adj_hi = adjust >> 16;
	uint32_t adj_lo = (uint32_t)adjust - ((uint32_t)adj_hi << 16);
	int64_t acc = D_FloatToScaledI64(divz, 45);

	acc += (int64_t)adj_hi * zi_q45;
	acc += D_ShrTrunc64((int64_t)adj_lo * zi_q45, 16);
	return D_Q29FromQ45(acc);
}

static inline int32_t D_Q16FromQ45 (int64_t q45)
{
	return (int32_t)D_ShrTrunc64(q45, 29);
}

static inline int32_t D_Q16Numerator (float divz, fixed16_t adjust,
	int64_t zi_q45)
{
	int32_t adj_hi = adjust >> 16;
	uint32_t adj_lo = (uint32_t)adjust - ((uint32_t)adj_hi << 16);
	int64_t acc = D_FloatToScaledI64(divz, 32);

	acc += D_ShrTrunc64((int64_t)adj_hi * zi_q45, 13);
	acc += D_ShrTrunc64((int64_t)adj_lo * zi_q45, 29);
	return (int32_t)D_ShrTrunc64(acc, 16);
}


/*
=============
D_WarpScreen

// this performs a slight compression of the screen at the same time as
// the sine warp, to keep the edges from wrapping
=============
*/
void D_WarpScreen (void)
{
	int		w, h;
	int		u,v;
	byte	*dest;
	int		*turb;
	int		*col;
	byte	**row;
	byte	*rowptr[MAXHEIGHT+(AMP2*2)];
	int		column[MAXWIDTH+(AMP2*2)];
	float	wratio, hratio;

	of_emit_prepare_framebuffer_for_cpu();

	w = r_refdef.vrect.width;
	h = r_refdef.vrect.height;

	wratio = w / (float)scr_vrect.width;
	hratio = h / (float)scr_vrect.height;

	for (v=0 ; v<scr_vrect.height+AMP2*2 ; v++)
	{
		rowptr[v] = d_viewbuffer + (r_refdef.vrect.y * screenwidth) +
				 (screenwidth * (int)((float)v * hratio * h / (h + AMP2 * 2)));
	}

	for (u=0 ; u<scr_vrect.width+AMP2*2 ; u++)
	{
		column[u] = r_refdef.vrect.x +
				(int)((float)u * wratio * w / (w + AMP2 * 2));
	}

	turb = intsintable + ((int)(cl.time*SPEED)&(CYCLE-1));
	dest = vid.buffer + scr_vrect.y * vid.rowbytes + scr_vrect.x;

	for (v=0 ; v<scr_vrect.height ; v++, dest += vid.rowbytes)
	{
		col = &column[turb[v]];
		row = &rowptr[v];

		for (u=0 ; u<scr_vrect.width ; u+=4)
		{
			dest[u+0] = row[turb[u+0]][col[u+0]];
			dest[u+1] = row[turb[u+1]][col[u+1]];
			dest[u+2] = row[turb[u+2]][col[u+2]];
			dest[u+3] = row[turb[u+3]][col[u+3]];
		}
	}
}


#if	!id386

/*
=============
D_DrawTurbulent8Span
=============
*/
void D_DrawTurbulent8Span (void)
{
	int		sturb, tturb;

	do
	{
		sturb = ((r_turb_s + r_turb_turb[(r_turb_t>>16)&(CYCLE-1)])>>16)&63;
		tturb = ((r_turb_t + r_turb_turb[(r_turb_s>>16)&(CYCLE-1)])>>16)&63;
		*r_turb_pdest++ = *(r_turb_pbase + (tturb<<6) + sturb);
		r_turb_s += r_turb_sstep;
		r_turb_t += r_turb_tstep;
	} while (--r_turb_spancount > 0);
}

#endif	// !id386


/*
=============
Turbulent8

Water / lava surfaces — the GPU has no turb mode, so this stays CPU.
=============
*/
void Turbulent8 (espan_t *pspan)
{
	int				count;
	fixed16_t		snext, tnext;
	float			sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float			sdivz16stepu, tdivz16stepu, zi16stepu;

	of_emit_prepare_framebuffer_for_cpu();

	r_turb_turb = sintable + ((int)(cl.time*SPEED)&(CYCLE-1));

	r_turb_sstep = 0;
	r_turb_tstep = 0;

	r_turb_pbase = (unsigned char *)cacheblock;

	sdivz16stepu = d_sdivzstepu * 32;
	tdivz16stepu = d_tdivzstepu * 32;
	zi16stepu = d_zistepu * 32;

	do
	{
		r_turb_pdest = (unsigned char *)((byte *)d_viewbuffer +
				(screenwidth * pspan->v) + pspan->u);

		count = pspan->count;

	// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;

		r_turb_s = (int)(sdivz * z) + sadjust;
		if (r_turb_s > bbextents)  r_turb_s = bbextents;
		else if (r_turb_s < 0)     r_turb_s = 0;

		r_turb_t = (int)(tdivz * z) + tadjust;
		if (r_turb_t > bbextentt)  r_turb_t = bbextentt;
		else if (r_turb_t < 0)     r_turb_t = 0;

		do
		{
			if (count >= 32) r_turb_spancount = 32;
			else             r_turb_spancount = count;

			count -= r_turb_spancount;

			if (count)
			{
				sdivz += sdivz16stepu;
				tdivz += tdivz16stepu;
				zi += zi16stepu;
				z = (float)0x10000 / zi;

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents) snext = bbextents;
				else if (snext < 16)   snext = 16;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt) tnext = bbextentt;
				else if (tnext < 16)   tnext = 16;

				r_turb_sstep = (snext - r_turb_s) >> 5;
				r_turb_tstep = (tnext - r_turb_t) >> 5;
			}
			else
			{
				spancountminus1 = (float)(r_turb_spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents) snext = bbextents;
				else if (snext < 16)   snext = 16;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt) tnext = bbextentt;
				else if (tnext < 16)   tnext = 16;

				if (r_turb_spancount > 1)
				{
					float inv = 1.0f / (float)(r_turb_spancount - 1);
					r_turb_sstep = (int)((float)(snext - r_turb_s) * inv);
					r_turb_tstep = (int)((float)(tnext - r_turb_t) * inv);
				}
			}

			r_turb_s = r_turb_s & ((CYCLE<<16)-1);
			r_turb_t = r_turb_t & ((CYCLE<<16)-1);

			D_DrawTurbulent8Span ();

			r_turb_s = snext;
			r_turb_t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}


#if	!id386

/*
=============
R_DrawSurfaceTris — T6 better

Replaces the AET/span pipeline for a single visible world surface.
Pipeline:

  1. R_CollectSurfaceVerts walks the surface's edge list and emits
     (cam-space xyz, absolute mip-0 surface s/t) per vertex.

  2. R_ClipPolyAxis (axis 0) clips against the camera-space near
     plane.  Sub-3-vertex output → fully behind the camera, drop.

  3. Three-tier dispatch by tile-straddle count:
       n_tiles == 1: fast path, single emit with texture-period-
                     aligned tile origin → tile-local s,t correctly
                     indexes the bound mip texture.  Most id1 walls
                     and ceilings hit here.
       n_tiles ≤ R_MAX_TILES_PER_SURFACE: tessellate.  Polygon is
                     clipped at multiples of (tex_w << miplevel) /
                     (tex_h << miplevel) in absolute texel space;
                     each sub-polygon's tile-local s,t stays in
                     [0, tex_w-1] / [0, tex_h-1] of the bound mip.
       n_tiles >  R_MAX_TILES_PER_SURFACE: cap fallback.  Single
                     emit with mis-aligned origin — those surfaces
                     read past the POT bounds (same artifact as the
                     legacy D_GPU_WORLD_LIGHT span path), but the
                     per-frame triangle count stays close to the
                     legacy span path's budget so the GPU ring
                     doesn't back-pressure into a hang.

  4. R_PackTriVert encodes screen pos (12.4), 1/z (16.16), tile-
     local s,t (16.16), and per-vertex light byte (sampled from
     blocklights[] via surface-local luxel coord) into
     of_emit_vertex_t — same word layout as the alias triangle path
     in d_polyse.c so the gateware sees a single verified format.

  5. R_AppendSubPolygon fan-triangulates and accumulates into a
     texture-bucketed batch; R_FlushWorldTriBatch (called from
     D_DrawSurfaces) drains pending triangles before any GPU state
     change.

  D_DrawZSpans still runs for sprite/alias CPU-side depth occlusion.
=============
*/
#if D_GPU_WORLD_TRIS

/* Maximum vertices in the source surface polygon (pre-clip).  Quake
 * id1 faces are typically 4–8 verts; cap at 32 to bound stack use. */
#define R_MAX_SURFACE_VERTS 32

/* Up to 5 axis-aligned clip planes are applied per emitted sub-poly
 * (1 near + 4 tile edges); each can add one vertex to a closed
 * polygon.  +8 slack covers the worst case with margin. */
#define R_CLIP_BUF_SIZE     (R_MAX_SURFACE_VERTS + 8)

/* Cap per-surface tessellation: surfaces straddling more than this
 * many tiles fall back to a single mis-aligned emission (textures
 * deformed on the largest surfaces only).  2 keeps the per-frame
 * vertex count within the design budget the GPU was profiled
 * against (~1500 verts/frame on the dense scene per
 * optimizations.md §6 T6) — anything over that risks ring back-
 * pressure into a hang.  Catches the common "long wall" case (one
 * straddle axis, two tiles) but not 2D tessellation. */
#define R_MAX_TILES_PER_SURFACE 2

typedef struct {
	/* Camera-space (post-VectorSubtract(modelorg) + TransformVector).
	 * cam[2] is forward depth; near-plane clip tests it.  All three
	 * components lerp linearly in s,t-space (the surface is planar),
	 * so the tile-grid clipper can lerp them alongside the surface
	 * attrs without bothering with screen-space. */
	vec3_t cam;
	/* Absolute surface texture coords in mip-0 texel units, post-
	 * vecs[][3] offset.  Lightmap sampler subtracts texturemins to
	 * index blocklights[]; tile-grid clipper splits the polygon at
	 * multiples of (tex_w << miplevel) so each sub-polygon's tile-
	 * local s lands in [0, tex_w-1] of the bound mip texture. */
	float s, t;
} r_tri_vert_t;

extern vec3_t       modelorg;
extern mvertex_t   *r_pcurrentvertbase;
extern entity_t    *currententity;
extern unsigned     blocklights[];

/* Walk fa's edge list, project each vertex into r_tri_vert_t.  Each
 * surface edge contributes one vertex (its leading endpoint in
 * surfedge order), tracing the polygon perimeter exactly once.
 * Negative surfedges flip the (v[0], v[1]) order in the underlying
 * medge_t — same handling as R_RenderFace at r_draw.c:443.  Returns
 * the number of vertices collected (≤ max_verts). */
static int R_CollectSurfaceVerts(msurface_t *fa,
                                 r_tri_vert_t *poly,
                                 int max_verts)
{
	medge_t  *pedges     = currententity->model->edges;
	int      *psurfedges = currententity->model->surfedges + fa->firstedge;
	mtexinfo_t *tex      = fa->texinfo;

	int n = fa->numedges;
	if (n > max_verts) n = max_verts;

	for (int i = 0; i < n; i++) {
		int lindex = psurfedges[i];
		int v0idx;
		if (lindex > 0)
			v0idx = pedges[lindex].v[0];
		else
			v0idx = pedges[-lindex].v[1];

		float *world = r_pcurrentvertbase[v0idx].position;
		r_tri_vert_t *v = &poly[i];

		vec3_t local;
		VectorSubtract(world, modelorg, local);
		TransformVector(local, v->cam);

		v->s = DotProduct(world, tex->vecs[0]) + tex->vecs[0][3];
		v->t = DotProduct(world, tex->vecs[1]) + tex->vecs[1][3];
	}

	return n;
}

static inline float R_VertField(const r_tri_vert_t *v, int axis)
{
	switch (axis) {
		case 0: return v->cam[2];
		case 1: return v->s;
		case 2: return v->t;
	}
	return 0.0f;
}

static void R_LerpVert(const r_tri_vert_t *a, const r_tri_vert_t *b,
                       float t, r_tri_vert_t *out)
{
	out->cam[0] = a->cam[0] + t * (b->cam[0] - a->cam[0]);
	out->cam[1] = a->cam[1] + t * (b->cam[1] - a->cam[1]);
	out->cam[2] = a->cam[2] + t * (b->cam[2] - a->cam[2]);
	out->s      = a->s      + t * (b->s      - a->s);
	out->t      = a->t      + t * (b->t      - a->t);
}

/* Sutherland-Hodgman against an axis-aligned half-plane.
 *   axis: 0 = cam[2] (near plane), 1 = s, 2 = t
 *   sign:  +1 keeps verts where field >= bound
 *          -1 keeps verts where field <= bound
 * Walks the polygon as a closed loop; per (prev, curr) edge emits 0,
 * 1, or 2 vertices to poly_out depending on which side each is on.
 * Returns n_out (≤ n_in + 1).  Caller sizes poly_out for n_in + 1. */
static int R_ClipPolyAxis(const r_tri_vert_t *poly_in, int n_in,
                          r_tri_vert_t *poly_out,
                          int axis, float bound, int sign)
{
	if (n_in <= 0) return 0;

	int n_out = 0;
	const r_tri_vert_t *prev = &poly_in[n_in - 1];
	float prev_v = R_VertField(prev, axis);
	int prev_in = (sign > 0) ? (prev_v >= bound) : (prev_v <= bound);

	for (int i = 0; i < n_in; i++) {
		const r_tri_vert_t *curr = &poly_in[i];
		float curr_v = R_VertField(curr, axis);
		int curr_in = (sign > 0) ? (curr_v >= bound) : (curr_v <= bound);

		if (curr_in) {
			if (!prev_in) {
				float t = (bound - prev_v) / (curr_v - prev_v);
				R_LerpVert(prev, curr, t, &poly_out[n_out++]);
			}
			poly_out[n_out++] = *curr;
		} else if (prev_in) {
			float t = (bound - prev_v) / (curr_v - prev_v);
			R_LerpVert(prev, curr, t, &poly_out[n_out++]);
		}

		prev = curr;
		prev_v = curr_v;
		prev_in = curr_in;
	}

	return n_out;
}

/* Sample the lightmap at the luxel covering vertex `v` of surface `fa`.
 * Always indexes via SURFACE-LOCAL coords (abs - texturemins), even
 * when the GPU's texture sampling is tile-local — the lightmap is per
 * surface, not per texture tile. */
static byte R_SampleLightAtVertex(const msurface_t *fa,
                                  const r_tri_vert_t *v)
{
	int smax = (fa->extents[0] >> 4) + 1;
	int tmax = (fa->extents[1] >> 4) + 1;

	int local_s = (int)v->s - fa->texturemins[0];
	int local_t = (int)v->t - fa->texturemins[1];
	int lx = local_s >> 4;
	int ly = local_t >> 4;

	if (lx < 0)        lx = 0;
	if (ly < 0)        ly = 0;
	if (lx >= smax)    lx = smax - 1;
	if (ly >= tmax)    ly = tmax - 1;

	int lt = (int)(blocklights[ly * smax + lx] >> 8);
	if (lt < 0)  lt = 0;
	if (lt > 63) lt = 63;
	return (byte)lt;
}

/* Pack one tile-clipped vertex into of_emit_vertex_t.  tile_s_lo /
 * tile_t_lo are in mip-0 absolute texels and aligned on a multiple of
 * (tex_w << miplevel) / (tex_h << miplevel); subtracting them from
 * v->s / v->t yields tile-local coords in [0, tile_w] / [0, tile_h]
 * which, scaled to mip-N, index directly into the bound texture's
 * [0, tex_w-1] / [0, tex_h-1] range — keeping the GPU's wrap-less
 * triangle texture-fetch in-bounds.
 *
 * u/v/lzi are derived per pack call (not stored on the vertex) because
 * Sutherland-Hodgman's linear interpolation lerps through cam space
 * but the screen projection is non-linear in z. */
static void R_PackTriVert(const msurface_t *fa, int miplevel,
                          const r_tri_vert_t *v,
                          float tile_s_lo, float tile_t_lo,
                          of_emit_vertex_t *out)
{
	byte light = R_SampleLightAtVertex(fa, v);

	float z = (v->cam[2] < NEAR_CLIP) ? NEAR_CLIP : v->cam[2];
	float lzi = 1.0f / z;
	float u  = xcenter + xscale * lzi * v->cam[0];
	float vv = ycenter - yscale * lzi * v->cam[1];

	/* Screen position: float pixel coords → 12.4 fixed.  Clamp to
	 * int16 range — clipper only ran near-plane + tile bounds, so
	 * vertices may still extend off-screen on the sides; the GPU
	 * rasteriser handles screen-side clip itself. */
	int u_q4 = (int)(u  * 16.0f + 0.5f);
	int v_q4 = (int)(vv * 16.0f + 0.5f);
	if (u_q4 < INT16_MIN) u_q4 = INT16_MIN;
	if (u_q4 > INT16_MAX) u_q4 = INT16_MAX;
	if (v_q4 < INT16_MIN) v_q4 = INT16_MIN;
	if (v_q4 > INT16_MAX) v_q4 = INT16_MAX;

	int z_q = (int)(lzi * 32768.0f);
	if (z_q < 0)        z_q = 0;
	if (z_q > 0xFFFF)   z_q = 0xFFFF;

	int w_q = (int)(lzi * 65536.0f);
	if (w_q < 1)        w_q = 1;
	if (w_q > 0xFFFF)   w_q = 0xFFFF;

	float st_scale = 65536.0f / (float)(1 << miplevel);
	float local_s  = v->s - tile_s_lo;
	float local_t  = v->t - tile_t_lo;
	/* Clamp ≥ 0: Sutherland-Hodgman boundary verts can land at
	 * −ε on the wrong side of the tile due to FP roundoff. */
	if (local_s < 0.0f) local_s = 0.0f;
	if (local_t < 0.0f) local_t = 0.0f;
	int s_q16 = (int)(local_s * st_scale);
	int t_q16 = (int)(local_t * st_scale);

	out->x   = (int16_t)u_q4;
	out->y   = (int16_t)v_q4;
	out->z   = (uint16_t)z_q;
	out->pad = 0;
	out->s   = s_q16;
	out->t   = t_q16;
	out->w   = w_q;
	out->r   = light;
	out->g   = light;
	out->b   = light;
	out->a   = 0xFF;
}

/* Texture-bucketed world triangle batch.
 *
 * R_DrawSurfaceTris is called once per visible surface; if every call
 * dispatched its own (bind_texture + triangles_batch) pair, a frame
 * with 412 surfaces would issue 412 of each.  Most consecutive
 * surfaces in BSP order share the previous one's texture (long walls
 * at one mip level use the same wall_texture animation row), so we
 * accumulate triangles into a per-texture batch and flush only when
 * the texture, mip, or batch capacity changes.
 *
 * The batch state lives at file scope so D_DrawSurfaces can call
 * R_FlushWorldTriBatch() at the end of its loop to drain pending
 * triangles before any state change (FB rebind, kick, finish). */
#define R_WORLD_BATCH_CAP   672       /* 224 tris; fits one 16 KB GPU command stream. */
static of_emit_vertex_t r_world_batch[R_WORLD_BATCH_CAP];
static int              r_world_batch_count;
static uint32_t         r_world_batch_tex_addr;
static uint16_t         r_world_batch_tex_w;
static uint16_t         r_world_batch_tex_h;

void R_FlushWorldTriBatch (void)
{
	if (r_world_batch_count == 0) return;
	of_emit_texture_t tex = {
		.addr   = r_world_batch_tex_addr,
		.width  = r_world_batch_tex_w,
		.height = r_world_batch_tex_h,
	};
	of_emit_bind_texture(&tex);
	of_emit_triangles_batch(r_world_batch, (uint32_t)r_world_batch_count);
	r_world_batch_count = 0;
}

/* Append a fan-triangulated sub-polygon to the world triangle batch,
 * flushing first if the bound texture changed or the batch would
 * overflow. */
static void R_AppendSubPolygon(uint32_t tex_addr,
                               uint16_t tex_w, uint16_t tex_h,
                               const of_emit_vertex_t *verts, int nverts)
{
	if (nverts < 3) return;

	int ntri = nverts - 2;
	int new_verts = ntri * 3;

	if (r_world_batch_count > 0 &&
	    (tex_addr != r_world_batch_tex_addr ||
	     r_world_batch_count + new_verts > R_WORLD_BATCH_CAP))
	{
		R_FlushWorldTriBatch();
	}

	if (r_world_batch_count == 0) {
		r_world_batch_tex_addr = tex_addr;
		r_world_batch_tex_w    = tex_w;
		r_world_batch_tex_h    = tex_h;
	}

	for (int i = 0; i < ntri; i++) {
		r_world_batch[r_world_batch_count++] = verts[0];
		r_world_batch[r_world_batch_count++] = verts[i + 1];
		r_world_batch[r_world_batch_count++] = verts[i + 2];
	}
}

PQ_HOT void R_DrawSurfaceTris (msurface_t *fa, int miplevel)
{
	r_tri_vert_t poly_a[R_MAX_SURFACE_VERTS];
	r_tri_vert_t poly_b[R_CLIP_BUF_SIZE];
	of_emit_vertex_t verts[R_CLIP_BUF_SIZE];

	int nverts = R_CollectSurfaceVerts(fa, poly_a, R_MAX_SURFACE_VERTS);
	if (nverts < 3) return;

	nverts = R_ClipPolyAxis(poly_a, nverts, poly_b,
	                        0, NEAR_CLIP, +1);
	if (nverts < 3) return;   /* fully behind the near plane */

	texture_t *mt = R_TextureAnimation(fa->texinfo->texture);
	uint32_t tex_addr = (uint32_t)(uintptr_t)((byte *)mt + mt->offsets[miplevel]);
	uint16_t tex_w    = (uint16_t)(mt->width  >> miplevel);
	uint16_t tex_h    = (uint16_t)(mt->height >> miplevel);

	const int tile_w = (int)tex_w << miplevel;
	const int tile_h = (int)tex_h << miplevel;
	if (tile_w <= 0 || tile_h <= 0) return;

	float s_min = poly_b[0].s, s_max = poly_b[0].s;
	float t_min = poly_b[0].t, t_max = poly_b[0].t;
	for (int i = 1; i < nverts; i++) {
		if (poly_b[i].s < s_min) s_min = poly_b[i].s;
		if (poly_b[i].s > s_max) s_max = poly_b[i].s;
		if (poly_b[i].t < t_min) t_min = poly_b[i].t;
		if (poly_b[i].t > t_max) t_max = poly_b[i].t;
	}

	int i_lo = (int)floorf(s_min / (float)tile_w);
	int i_hi = (int)floorf((s_max - 1e-3f) / (float)tile_w);
	int j_lo = (int)floorf(t_min / (float)tile_h);
	int j_hi = (int)floorf((t_max - 1e-3f) / (float)tile_h);
	if (i_hi < i_lo) i_hi = i_lo;
	if (j_hi < j_lo) j_hi = j_lo;

	int n_tiles = (i_hi - i_lo + 1) * (j_hi - j_lo + 1);

	/* Single-tile fast path: polygon fits inside one texture period.
	 * Texture-period-aligned tile origin (a multiple of tile_w in
	 * absolute mip-0 texels) → tile-local s,t lands in [0, tex_w-1],
	 * matching abs_s mod tex_w in the underlying texture.  Most id1
	 * surfaces hit this path. */
	if (n_tiles == 1) {
		float tile_s_lo = (float)(i_lo * tile_w);
		float tile_t_lo = (float)(j_lo * tile_h);
		for (int i = 0; i < nverts; i++)
			R_PackTriVert(fa, miplevel, &poly_b[i],
			              tile_s_lo, tile_t_lo, &verts[i]);
		R_AppendSubPolygon(tex_addr, tex_w, tex_h, verts, nverts);
		return;
	}

	/* Over-budget surfaces (n_tiles > cap): emit as single mis-aligned
	 * polygon — the fan is bound to be deformed on a 4×4-tile floor,
	 * but this keeps the per-frame triangle count from blowing up the
	 * GPU's ring throughput.  Same artifact as the legacy span path
	 * exhibits on those surfaces. */
	if (n_tiles > R_MAX_TILES_PER_SURFACE) {
		float tile_s_lo = (float)(i_lo * tile_w);
		float tile_t_lo = (float)(j_lo * tile_h);
		for (int i = 0; i < nverts; i++)
			R_PackTriVert(fa, miplevel, &poly_b[i],
			              tile_s_lo, tile_t_lo, &verts[i]);
		R_AppendSubPolygon(tex_addr, tex_w, tex_h, verts, nverts);
		return;
	}

	/* Multi-tile tessellation: split the polygon at tile boundaries
	 * in (s,t) space and emit one fan per tile.  Tile-local s,t inside
	 * each sub-polygon stays in the bound texture's POT bounds.  All
	 * three buffers are stack-local — bounded per the n_tiles cap so
	 * stack stays under ~5 KB even with the deepest call. */
	r_tri_vert_t row_buf[R_CLIP_BUF_SIZE];
	r_tri_vert_t scratch1[R_CLIP_BUF_SIZE];
	r_tri_vert_t scratch2[R_CLIP_BUF_SIZE];

	for (int j = j_lo; j <= j_hi; j++) {
		float tile_t_lo = (float)(j * tile_h);
		float tile_t_hi = tile_t_lo + (float)tile_h;

		int n = R_ClipPolyAxis(poly_b, nverts, scratch1,
		                       2, tile_t_lo, +1);
		if (n < 3) continue;
		n = R_ClipPolyAxis(scratch1, n, row_buf,
		                   2, tile_t_hi, -1);
		if (n < 3) continue;

		for (int i = i_lo; i <= i_hi; i++) {
			float tile_s_lo = (float)(i * tile_w);
			float tile_s_hi = tile_s_lo + (float)tile_w;

			int m = R_ClipPolyAxis(row_buf, n, scratch1,
			                       1, tile_s_lo, +1);
			if (m < 3) continue;
			m = R_ClipPolyAxis(scratch1, m, scratch2,
			                   1, tile_s_hi, -1);
			if (m < 3) continue;

			for (int k = 0; k < m; k++)
				R_PackTriVert(fa, miplevel, &scratch2[k],
				              tile_s_lo, tile_t_lo, &verts[k]);

			R_AppendSubPolygon(tex_addr, tex_w, tex_h, verts, m);
		}
	}
}
#endif

/*
=============
D_DrawSpans8

World textured spans. Prefer the unified param-span path when advertised;
fall back to older SDK helpers or CPU-side per-16-pixel affine spans.
=============
*/
PQ_FASTTEXT void D_DrawSpans8 (espan_t *pspan)
{
	unsigned char *pbase = (unsigned char *)cacheblock;
	int profiling = (int)pq_cycleprof.value;

	if (profiling)
		pq_prof_spans8_calls_frame++;

#if D_GPU_WORLD_LIGHT
	const int gpu_world_light = (int)pq_gpu_world_light.value;
#else
	const int gpu_world_light = 0;
#endif
	const int tex_s_offset = gpu_world_light ? pq_world_tex_s_offset : 0;
	const int tex_t_offset = gpu_world_light ? pq_world_tex_t_offset : 0;

	/* Pre-lit surface cache path: D_CacheSurface marks cache blocks dirty
	 * only when R_DrawSurfaceBlock8_mipN rebuilt data[]. Cache hits are
	 * already clean in SDRAM, so avoid re-flushing/re-draining those lines. */
	if (!gpu_world_light) {
		surfcache_t *surf_cache = D_SurfCacheForData(pbase);
		if (surf_cache->gpu_dirty) {
			of_emit_cache_clean(pbase, cachewidth * ((bbextentt >> 16) + 1));
			surf_cache->gpu_dirty = 0;
		}
	}

#if D_USE_GPU_PERSP
	if ((int)pq_gpu_persp.value &&
	    (int)pq_gpu_param.value &&
	    !gpu_world_light &&
	    of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_LIST)) {
		espan_t *span;
		int base_u = 0, base_v = 0;
		uint32_t total_records = 0;

		const int64_t zi_org_q45 = D_FloatToScaledI64(d_ziorigin, 45);
		const int64_t zi_stepv_q45 = D_FloatToScaledI64(d_zistepv, 45);
		const int64_t zi_stepu_q45 = D_FloatToScaledI64(d_zistepu, 45);
		const int32_t sdivz_org =
			D_Q29Numerator(d_sdivzorigin, sadjust, zi_org_q45);
		const int32_t tdivz_org =
			D_Q29Numerator(d_tdivzorigin, tadjust, zi_org_q45);
		const int32_t zi_org =
			D_Q29FromQ45(zi_org_q45);
		const int32_t sdivz_stepv =
			D_Q29Numerator(d_sdivzstepv, sadjust, zi_stepv_q45);
		const int32_t tdivz_stepv =
			D_Q29Numerator(d_tdivzstepv, tadjust, zi_stepv_q45);
		const int32_t zi_stepv =
			D_Q29FromQ45(zi_stepv_q45);
		const int32_t sdivz_stepu =
			D_Q29Numerator(d_sdivzstepu, sadjust, zi_stepu_q45);
		const int32_t tdivz_stepu =
			D_Q29Numerator(d_tdivzstepu, tadjust, zi_stepu_q45);
		const int32_t zi_stepu =
			D_Q29FromQ45(zi_stepu_q45);
		uint8_t flags = OF_EMIT_PERSP;
		if (gpu_world_light)
			flags |= OF_EMIT_COLORMAP;
		const int gpu_zwrite =
			(int)pq_gpu_zwrite.value &&
			of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_Z) &&
			d_pzbuffer != NULL && d_zwidth != 0 &&
			(flags & (OF_EMIT_SKIP_ZERO | OF_EMIT_COLUMN)) == 0;

		for (span = pspan; span != NULL; span = span->pnext) {
			if (span->count <= 0)
				continue;
			if (total_records == 0 || span->u < base_u)
				base_u = span->u;
			if (total_records == 0 || span->v < base_v)
				base_v = span->v;
			total_records++;
		}

		of_emit_param_span_list_t params;
		memset(&params, 0, sizeof(params));
		params.fb_base       = (uint32_t)(uintptr_t)
			(d_viewbuffer + screenwidth * base_v + base_u);
		params.fb_major_step = screenwidth;
		params.fb_minor_step = 1;
		params.tex_addr      = (uint32_t)(uintptr_t)pbase;
		params.tex_width     = (uint16_t)cachewidth;
		params.flags         = flags;
		params.attr_mode     = OF_EMIT_PARAM_ATTR_PERSP_Q29;
		params.span_axis     = OF_EMIT_PARAM_AXIS_X;
		params.z_mode        = gpu_zwrite ? OF_EMIT_PARAM_Z_WRITE_ZI
		                                  : OF_EMIT_PARAM_Z_NONE;
		/* Keep record coordinates local to this surface batch.  This avoids
		 * large absolute u/v products in the GPU's Q29 start calculation on
		 * steep floors, while preserving the same projected planes. */
		params.attr_origin[0] =
			D_SpanMad2I32(sdivz_org, sdivz_stepv, base_v, sdivz_stepu, base_u);
		params.attr_origin[1] =
			D_SpanMad2I32(tdivz_org, tdivz_stepv, base_v, tdivz_stepu, base_u);
		params.attr_origin[2] =
			D_SpanMad2I32(zi_org, zi_stepv, base_v, zi_stepu, base_u);
		params.attr_du[0] = sdivz_stepu;
		params.attr_du[1] = tdivz_stepu;
		params.attr_du[2] = zi_stepu;
		params.attr_dv[0] = sdivz_stepv;
		params.attr_dv[1] = tdivz_stepv;
		params.attr_dv[2] = zi_stepv;
		params.clamp_min[0] = 0;
		params.clamp_max[0] = bbextents;
		params.clamp_min[1] = 0;
		params.clamp_max[1] = bbextentt;
		if (gpu_zwrite) {
			params.z_base = (uint32_t)(uintptr_t)
				(d_pzbuffer + d_zwidth * base_v + base_u);
			params.z_major_step = (int32_t)d_zwidth * (int32_t)sizeof(short);
			params.z_minor_step = (int32_t)sizeof(short);
		}

		uint32_t batch_count = 0;
		uint32_t submitted_records = 0;
		for (span = pspan; span != NULL; span = span->pnext) {
			if (span->count > 0) {
				of_emit_param_span_record_t *rec =
					&pq_param_span_records[batch_count++];
				rec->u = (uint16_t)(span->u - base_u);
				rec->v = (uint16_t)(span->v - base_v);
				rec->count = (uint16_t)span->count;

				if (batch_count == OF_EMIT_PARAM_SPAN_MAX_RECORDS) {
					submitted_records += of_emit_param_span_list(
						&params, pq_param_span_records, batch_count);
					batch_count = 0;
				}
			}
		}

		if (batch_count != 0) {
			submitted_records += of_emit_param_span_list(
				&params, pq_param_span_records, batch_count);
		}

		pq_combined_z_active = gpu_zwrite &&
			total_records != 0 &&
			submitted_records == total_records;
		if (pq_combined_z_active)
			pq_gpu_zwrite_pending = 1;
		return;
	}

	if ((int)pq_gpu_persp.value && of_emit_supports(OF_EMIT_CAP_PERSP)) {
		/* Bake Quake's `+ sadjust` offset into the projection-space planes
		 * so GPU output stays exact across the scanline:
		 *   target: s_16.16(u) = sdivz_q(u) * 0x10000 / zi_q(u) + sadjust
		 *   GPU:    s_out(u)   = sdivz_sent(u) * 0x10000 / zi_persp_sent(u)
		 * with zi_persp_sent(u) = zi_q(u) * 0x10000, the math gives
		 *   sdivz_sent(u) = sdivz_q(u) * 0x10000 + sadjust * zi_q(u)
		 * This is still linear in screen u/v, so compute descriptor-space
		 * plane coefficients once per surface and do integer MAD per span. */
		const fixed16_t sadjust_tex = sadjust + tex_s_offset;
		const fixed16_t tadjust_tex = tadjust + tex_t_offset;
		const int64_t zi_org_q45 = D_FloatToScaledI64(d_ziorigin, 45);
		const int64_t zi_stepv_q45 = D_FloatToScaledI64(d_zistepv, 45);
		const int64_t zi_stepu_q45 = D_FloatToScaledI64(d_zistepu, 45);
		const int32_t sdivz_org =
			D_Q16Numerator(d_sdivzorigin, sadjust_tex, zi_org_q45);
		const int32_t tdivz_org =
			D_Q16Numerator(d_tdivzorigin, tadjust_tex, zi_org_q45);
		const int32_t zi_org = D_Q16FromQ45(zi_org_q45);
		const int32_t sdivz_stepv =
			D_Q16Numerator(d_sdivzstepv, sadjust_tex, zi_stepv_q45);
		const int32_t tdivz_stepv =
			D_Q16Numerator(d_tdivzstepv, tadjust_tex, zi_stepv_q45);
		const int32_t zi_stepv = D_Q16FromQ45(zi_stepv_q45);
		const int32_t sdivz_stepu =
			D_Q16Numerator(d_sdivzstepu, sadjust_tex, zi_stepu_q45);
		const int32_t tdivz_stepu =
			D_Q16Numerator(d_tdivzstepu, tadjust_tex, zi_stepu_q45);
		const int32_t zi_stepu = D_Q16FromQ45(zi_stepu_q45);
		const uint32_t fb_base = (uint32_t)(uintptr_t)d_viewbuffer;

		of_emit_span_t sp = {
			.tex_addr   = (uint32_t)(uintptr_t)pbase,
			.flags      = OF_EMIT_PERSP,
			.fb_stride  = 1,
			.tex_width  = (uint16_t)cachewidth,
			.sdivz_step = sdivz_stepu,
			.tdivz_step = tdivz_stepu,
			.zi_step    = zi_stepu,
		};
		if (gpu_world_light) {
			sp.flags |= OF_EMIT_COLORMAP;
			sp.light = pq_world_light;
			sp.tex_w_mask = pq_world_tex_w_mask;
			sp.tex_h_mask = pq_world_tex_h_mask;
		}

		do {
			const int u = pspan->u, v = pspan->v;
			sp.fb_addr  = fb_base + screenwidth*v + u;
			sp.count    = (uint16_t)pspan->count;
			sp.sdivz    = D_SpanMad2I32(sdivz_org, sdivz_stepv, v, sdivz_stepu, u);
			sp.tdivz    = D_SpanMad2I32(tdivz_org, tdivz_stepv, v, tdivz_stepu, u);
			sp.zi_persp = D_SpanMad2I32(zi_org,    zi_stepv,    v, zi_stepu,    u);
			of_emit_span(&sp);
		} while ((pspan = pspan->pnext) != NULL);

		/* Kick the GPU so it starts consuming this surface's spans while
		 * the CPU moves on to building the next surface's cache + spans.
		 * Without this, the GPU sits idle until the ring fills or
		 * of_emit_finish() is called at frame end. One MMIO write — cheap. */
		of_emit_kick();

		/* The lean GPU has no depth writer. Keep d_edge.c's CPU
		 * D_DrawZSpans pass active for alias/sprite occlusion. */
		pq_combined_z_active = 0;
		return;
	}
#endif /* D_USE_GPU_PERSP */

	/* CPU-side perspective, affine per-16-pixel GPU spans. */
	const float sdivzNstepu = d_sdivzstepu * 16;
	const float tdivzNstepu = d_tdivzstepu * 16;
	const float ziNstepu    = d_zistepu    * 16;

	do {
		unsigned char *pdest = d_viewbuffer + screenwidth*pspan->v + pspan->u;
		int count = pspan->count;

		float du = (float)pspan->u, dv = (float)pspan->v;
		float sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		float tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		float zi    = d_ziorigin    + dv*d_zistepv    + du*d_zistepu;
		float z     = (float)0x10000 / zi;

		fixed16_t s = (int)(sdivz * z) + sadjust;
		if (s > bbextents) s = bbextents; else if (s < 0) s = 0;
		fixed16_t t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt) t = bbextentt; else if (t < 0) t = 0;

		fixed16_t snext = 0, tnext = 0, sstep = 0, tstep = 0;
		do {
			int spancount = (count >= 16) ? 16 : count;
			count -= spancount;

			if (count) {
				sdivz += sdivzNstepu;
				tdivz += tdivzNstepu;
				zi    += ziNstepu;
				z = (float)0x10000 / zi;
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents) snext = bbextents;
				else if (snext < 16)   snext = 16;
				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt) tnext = bbextentt;
				else if (tnext < 16)   tnext = 16;
				sstep = (snext - s) >> 4;
				tstep = (tnext - t) >> 4;
			} else {
				float m1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * m1;
				tdivz += d_tdivzstepu * m1;
				zi    += d_zistepu    * m1;
				z = (float)0x10000 / zi;
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents) snext = bbextents;
				else if (snext < 8)    snext = 8;
				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt) tnext = bbextentt;
				else if (tnext < 8)    tnext = 8;
				if (spancount > 1) {
					float inv = 1.0f / (float)(spancount - 1);
					sstep = (int)((float)(snext - s) * inv);
					tstep = (int)((float)(tnext - t) * inv);
				}
			}

			of_emit_span_t sp = {
				.fb_addr   = (uint32_t)(uintptr_t)pdest,
				.tex_addr  = (uint32_t)(uintptr_t)pbase,
				.s = s + tex_s_offset,
				.t = t + tex_t_offset,
				.sstep = sstep,
				.tstep = tstep,
				.count     = (uint16_t)spancount,
				.flags     = 0,
				.fb_stride = 1,
				.tex_width = (uint16_t)cachewidth,
			};
			if (gpu_world_light) {
				sp.flags = OF_EMIT_COLORMAP;
				sp.light = pq_world_light;
				sp.tex_w_mask = pq_world_tex_w_mask;
				sp.tex_h_mask = pq_world_tex_h_mask;
			}
			of_emit_span(&sp);
			pdest += spancount;
			s = snext; t = tnext;
		} while (count > 0);
	} while ((pspan = pspan->pnext) != NULL);

	pq_combined_z_active = 0;
}

#endif


#if	!id386

/*
=============
D_DrawZSpans — CPU z-fill. Feeds d_pzbuffer so alias/sprite CPU
occlusion checks work even when the GPU didn't co-write z.
=============
*/
PQ_FASTTEXT void D_DrawZSpans (espan_t *pspan)
{
	int    count, izistep, izi;
	short *pdest;
	float  zi;
	float  du, dv;
	int    doublecount;
	unsigned ltemp;
	int profiling = (int)pq_cycleprof.value;

	if (profiling)
		pq_prof_zspans_calls_frame++;

	izistep = (int)(d_zistepu * 0x8000 * 0x10000);

	do
	{
		pdest = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;
		count = pspan->count;

		du = (float)pspan->u;
		dv = (float)pspan->v;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		izi = (int)(zi * 0x8000 * 0x10000);

		if ((long)pdest & 0x02) {
			*pdest++ = (short)(izi >> 16);
			izi += izistep;
			count--;
		}

		if ((doublecount = count >> 1) > 0) {
			do {
				ltemp = izi >> 16;
				izi += izistep;
				ltemp |= izi & 0xFFFF0000;
				izi += izistep;
				*(int *)pdest = ltemp;
				pdest += 2;
			} while (--doublecount > 0);
		}

		if (count & 1)
			*pdest = (short)(izi >> 16);

	} while ((pspan = pspan->pnext) != NULL);
}

#endif
