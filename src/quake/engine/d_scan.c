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
// of_emit_span() from of_emit.h. Surface cache is pre-lit by
// R_DrawSurfaceBlock8 (CPU), so the GPU runs NO colormap at draw
// time — a COLORMAP lookup on already-lit bytes would double-index
// and produce wrong colors. Z-buffer is filled CPU-side by
// D_DrawZSpans (below), so we don't ask the GPU to do DEPTH_WRITE
// either (which would also require GEQUAL-direction compare that
// LITE doesn't expose today).

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

/* Per-surface colormap row index 0..63. Set in d_edge.c by
 * D_GpuLightSurface() right before each surface's D_DrawSpans8 call,
 * embedded in the span's `light` field below. */
byte pq_world_light;

unsigned char	*r_turb_pbase, *r_turb_pdest;
fixed16_t		r_turb_s, r_turb_t, r_turb_sstep, r_turb_tstep;
int				*r_turb_turb;
int				r_turb_spancount;
unsigned int	pq_prof_spans8_cycles_frame;
unsigned int	pq_prof_spans8_calls_frame;
unsigned int	pq_prof_zspans_cycles_frame;
unsigned int	pq_prof_zspans_calls_frame;
extern cvar_t	pq_cycleprof;

int		pq_combined_z_active;  /* tells d_edge.c to skip D_DrawZSpans when 1 */

void D_DrawTurbulent8Span (void);


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
D_DrawSpans8

World textured spans. Two modes (compile-time via D_USE_GPU_PERSP):
  1. GPU perspective: one of_emit_span per scanline, the GPU does
     1/z reciprocal per 16-pixel sub-segment internally.
  2. CPU-side per-16-pixel affine: we pre-compute 16.16 s/sstep and
     emit one span per sub-segment.
=============
*/
PQ_FASTTEXT void D_DrawSpans8 (espan_t *pspan)
{
	unsigned char *pbase = (unsigned char *)cacheblock;

#if D_USE_GPU_PERSP
#if !D_GPU_WORLD_LIGHT
	/* Pre-lit surface cache path: the CPU just wrote new bytes into
	 * `cacheblock` via R_DrawSurfaceBlock8_mipN, so flush them so
	 * the GPU's AXI reads see fresh data. With D_GPU_WORLD_LIGHT,
	 * cacheblock points at raw mip texture data that's loaded once
	 * at level start and never modified — no per-span flush needed.
	 * The kernel's PAK loader is expected to leave loaded data
	 * coherent (or write-through) so the GPU sees it. */
	of_emit_cache_clean(pbase, cachewidth * ((bbextentt >> 16) + 1));
#endif

	/* tex_w_mask / tex_h_mask intentionally LEFT 0 → GPU treats as
	 * 0xFFFF (no-wrap legacy default). Setting POT masks here works
	 * for world (cachewidth is POT for mipped surface caches) but
	 * leaves sp_tex_w_mask sticky for the next DRAW_TRIANGLES — alias
	 * skins are non-POT (224×64 etc), so the inherited mask would
	 * read garbage for skin widths > 2× the world surface's width.
	 * The triangle path in gpu_core.v doesn't reset masks at span
	 * emit; flagging that as a gateware design issue. */

	/* Bake Quake's `+ sadjust` offset into both the starting sdivz AND
	 * the per-pixel step so GPU output stays exact across the scanline:
	 *   target: s_16.16(u) = sdivz_q(u) * 0x10000 / zi_q(u) + sadjust
	 *   GPU:    s_out(u)   = sdivz_sent(u) * 0x10000 / zi_persp_sent(u)
	 * with zi_persp_sent(u) = zi_q(u) * 0x10000, the math gives
	 *   sdivz_sent(u) = sdivz_q(u) * 0x10000 + sadjust * zi_q(u)
	 * Linear-in-u, so init = sdivz_q*0x10000 + sadjust*zi_q,
	 * step = sdivzstep*0x10000 + sadjust*zistep.
	 *
	 * Computing the step in float keeps int overflow at bay even when
	 * sadjust × zistep gets large on oblique surfaces. */
	const float   sadjustf   = (float)sadjust;
	const float   tadjustf   = (float)tadjust;
	const int32_t sdivz_step = (int32_t)(d_sdivzstepu * 65536.0f +
	                                     sadjustf * d_zistepu);
	const int32_t tdivz_step = (int32_t)(d_tdivzstepu * 65536.0f +
	                                     tadjustf * d_zistepu);
	const int32_t zi_step    = (int32_t)(d_zistepu    * 65536.0f);

	/* zi step per pixel in Quake's 1/z × 0x8000 × 0x10000 space — same
	 * math as PocketQuake's combined-z path. Per-pixel zistep is
	 * constant across scanlines. */
	const int32_t izi_step = (int32_t)(d_zistepu * 0x8000 * 0x10000);

	do {
		const int u = pspan->u, v = pspan->v;
		const float sdivz_q = d_sdivzorigin + v*d_sdivzstepv + u*d_sdivzstepu;
		const float tdivz_q = d_tdivzorigin + v*d_tdivzstepv + u*d_tdivzstepu;
		const float zi_q    = d_ziorigin    + v*d_zistepv    + u*d_zistepu;

		of_emit_span_t sp = {
			.fb_addr    = (uint32_t)(uintptr_t)d_viewbuffer + screenwidth*v + u,
			.tex_addr   = (uint32_t)(uintptr_t)pbase,
			.count      = (uint16_t)pspan->count,
			/* World is drawn back-to-front via BSP, so DEPTH_TEST
			 * isn't strictly required — but DEPTH_WRITE populates
			 * the z-buffer for subsequent alias/sprite passes,
			 * replacing the CPU-side D_DrawZSpans loop. */
#if D_GPU_WORLD_LIGHT
			.flags      = OF_EMIT_PERSP | OF_EMIT_COLORMAP,
			.light      = pq_world_light,
#else
			.flags      = OF_EMIT_PERSP,
#endif
			.fb_stride  = 1,
			.tex_width  = (uint16_t)cachewidth,
			.sdivz      = (int32_t)(sdivz_q * 65536.0f + sadjustf * zi_q),
			.tdivz      = (int32_t)(tdivz_q * 65536.0f + tadjustf * zi_q),
			.zi_persp   = (int32_t)(zi_q    * 65536.0f),
			.sdivz_step = sdivz_step,
			.tdivz_step = tdivz_step,
			.zi_step    = zi_step,
		};
		of_emit_span(&sp);
	} while ((pspan = pspan->pnext) != NULL);

	/* Kick the GPU so it starts consuming this surface's spans while
	 * the CPU moves on to building the next surface's cache + spans.
	 * Without this, the GPU sits idle until the ring fills or
	 * of_emit_finish() is called at frame end. One MMIO write — cheap. */
	of_emit_kick();

	/* GPU co-wrote z in the same pass — d_edge.c's D_DrawZSpans call
	 * is now redundant. Setting this flag tells it to skip. */
	pq_combined_z_active = 1;

#else  /* CPU-side perspective, affine per-16-pixel GPU spans */
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
				.s = s, .t = t, .sstep = sstep, .tstep = tstep,
				.count     = (uint16_t)spancount,
				.flags     = 0,
				.fb_stride = 1,
				.tex_width = (uint16_t)cachewidth,
			};
			of_emit_span(&sp);
			pdest += spancount;
			s = snext; t = tnext;
		} while (count > 0);
	} while ((pspan = pspan->pnext) != NULL);

	pq_combined_z_active = 0;
#endif /* D_USE_GPU_PERSP */
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
