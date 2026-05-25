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
// d_sprite.c: software top-level rasterization driver module for drawing
// sprites

#include "quakedef.h"
#include "d_local.h"
#include "of_emit.h"

#ifndef D_USE_GPU_SPRITES
#define D_USE_GPU_SPRITES 1
#endif
#ifndef D_USE_GPU_SPRITES_LEGACY_NOZ
#define D_USE_GPU_SPRITES_LEGACY_NOZ 0
#endif

static int		sprite_height;
static int		minindex, maxindex;
static sspan_t	*sprite_spans;
static of_emit_param_span_record_t d_sprite_param_records[OF_EMIT_PARAM_SPAN_MAX_RECORDS];
extern cvar_t pq_gpu_zwrite;
extern int pq_gpu_zwrite_pending;

static inline int32_t D_SpriteMad2I32(int32_t origin,
	int32_t stepv, int v, int32_t stepu, int u)
{
	return (int32_t)((uint32_t)origin
		+ (uint32_t)stepv * (uint32_t)v
		+ (uint32_t)stepu * (uint32_t)u);
}

#if D_USE_GPU_SPRITES
static int D_SpriteDrawSpans_ParamZ(sspan_t *pspan, byte *pbase)
{
	sspan_t *span;
	int base_u = 0, base_v = 0;
	uint32_t total_records = 0;

	if (!(int)pq_gpu_zwrite.value ||
	    !of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_ZTEST) ||
	    d_pzbuffer == NULL || d_zwidth == 0)
		return 0;

	for (span = pspan; span->count != DS_SPAN_LIST_END; span++) {
		if (span->count <= 0)
			continue;
		if (total_records == 0 || span->u < base_u)
			base_u = span->u;
		if (total_records == 0 || span->v < base_v)
			base_v = span->v;
		total_records++;
	}

	if (total_records == 0)
		return 1;

	{
		static byte *last_sprite_flushed = 0;
		if (pbase != last_sprite_flushed) {
			last_sprite_flushed = pbase;
			of_emit_cache_clean(pbase, cachewidth * sprite_height);
		}
	}

	const double sadjustd = (double)sadjust;
	const double tadjustd = (double)tadjust;
	const double fixscale = 65536.0;
	const int32_t sdivz_org = (int32_t)((double)d_sdivzorigin * fixscale +
	                                    sadjustd * (double)d_ziorigin);
	const int32_t tdivz_org = (int32_t)((double)d_tdivzorigin * fixscale +
	                                    tadjustd * (double)d_ziorigin);
	const int32_t zi_org = (int32_t)((double)d_ziorigin * fixscale);
	const int32_t sdivz_stepv = (int32_t)((double)d_sdivzstepv * fixscale +
	                                      sadjustd * (double)d_zistepv);
	const int32_t tdivz_stepv = (int32_t)((double)d_tdivzstepv * fixscale +
	                                      tadjustd * (double)d_zistepv);
	const int32_t zi_stepv = (int32_t)((double)d_zistepv * fixscale);
	const int32_t sdivz_stepu = (int32_t)((double)d_sdivzstepu * fixscale +
	                                      sadjustd * (double)d_zistepu);
	const int32_t tdivz_stepu = (int32_t)((double)d_tdivzstepu * fixscale +
	                                      tadjustd * (double)d_zistepu);
	const int32_t zi_stepu = (int32_t)((double)d_zistepu * fixscale);

	of_emit_param_span_list_t params;
	memset(&params, 0, sizeof(params));
	params.fb_base = (uint32_t)(uintptr_t)
		(d_viewbuffer + screenwidth * base_v + base_u);
	params.fb_major_step = screenwidth;
	params.fb_minor_step = 1;
	params.tex_addr = (uint32_t)(uintptr_t)pbase;
	params.tex_width = (uint16_t)cachewidth;
	params.flags = OF_EMIT_SKIP_ZERO;
	params.attr_mode = OF_EMIT_PARAM_ATTR_PERSP;
	params.span_axis = OF_EMIT_PARAM_AXIS_X;
	params.z_mode = OF_EMIT_PARAM_Z_TEST_WRITE;
	params.attr_origin[0] =
		D_SpriteMad2I32(sdivz_org, sdivz_stepv, base_v, sdivz_stepu, base_u);
	params.attr_origin[1] =
		D_SpriteMad2I32(tdivz_org, tdivz_stepv, base_v, tdivz_stepu, base_u);
	params.attr_origin[2] =
		D_SpriteMad2I32(zi_org, zi_stepv, base_v, zi_stepu, base_u);
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
	params.z_base = (uint32_t)(uintptr_t)
		(d_pzbuffer + d_zwidth * base_v + base_u);
	params.z_major_step = (int32_t)d_zwidth * (int32_t)sizeof(short);
	params.z_minor_step = (int32_t)sizeof(short);

	uint32_t batch_count = 0;
	uint32_t submitted = 0;
	for (span = pspan; span->count != DS_SPAN_LIST_END; span++) {
		if (span->count > 0) {
			of_emit_param_span_record_t *rec =
				&d_sprite_param_records[batch_count++];
			rec->u = (uint16_t)(span->u - base_u);
			rec->v = (uint16_t)(span->v - base_v);
			rec->count = (uint16_t)span->count;
			if (batch_count == OF_EMIT_PARAM_SPAN_MAX_RECORDS) {
				submitted += of_emit_param_span_list(
					&params, d_sprite_param_records, batch_count);
				batch_count = 0;
			}
		}
	}
	if (batch_count != 0) {
		submitted += of_emit_param_span_list(
			&params, d_sprite_param_records, batch_count);
	}

	if (submitted != total_records)
		return 0;

	pq_gpu_zwrite_pending = 1;
	return 1;
}
#endif

#if !id386

/*
=====================
D_SpriteDrawSpans — Software fallback
=====================
*/
void D_SpriteDrawSpans (sspan_t *pspan)
{
	byte		*pbase = cacheblock;

#if D_USE_GPU_SPRITES
	if (D_SpriteDrawSpans_ParamZ(pspan, pbase))
		return;
#endif

#if D_USE_GPU_SPRITES_LEGACY_NOZ
	if (of_emit_supports(OF_EMIT_CAP_PERSP)) {
		/* GPU perspective path. Each visible scanline of the projected
		 * sprite polygon was already computed by D_SpriteScanLeftEdge /
		 * D_SpriteScanRightEdge into sprite_spans[]. We emit one
		 * perspective-correct GPU span per scanline — same math as world
		 * surfaces in d_scan.c, but with SKIP_ZERO (Quake sprites use
		 * 0xFF as transparent — matches the GPU's SKIP_ZERO). No COLORMAP
		 * — sprite colors pass through raw to the framebuffer. If
		 * perspective spans are not advertised, the legacy CPU path below
		 * preserves the original z test.
		 *
		 * Flush the sprite frame's pixel data once per unique frame so
		 * the GPU reads fresh bytes via AXI. Sprite frames are loaded once
		 * per level and never modified, so the static-tracking is just to
		 * amortise the syscall across many sprites of the same frame in
		 * a single hot scene. */
		{
			static byte *last_sprite_flushed = 0;
			if (pbase != last_sprite_flushed) {
				last_sprite_flushed = pbase;
				of_emit_cache_clean(pbase, cachewidth * sprite_height);
			}
		}

		const float   sadjustf   = (float)sadjust;
		const float   tadjustf   = (float)tadjust;
		const int32_t sdivz_step = (int32_t)(d_sdivzstepu * 65536.0f);
		const int32_t tdivz_step = (int32_t)(d_tdivzstepu * 65536.0f);
		const int32_t zi_step    = (int32_t)(d_zistepu    * 65536.0f);
		extern viddef_t vid;
		const uint32_t fb_base   = (uint32_t)(uintptr_t)vid.buffer;
		const int      fb_row    = vid.rowbytes;

		do {
			const int u = pspan->u;
			const int v = pspan->v;
			const int count = pspan->count;
			if (count <= 0) { pspan++; continue; }

			const float sdivz_q = d_sdivzorigin + v*d_sdivzstepv + u*d_sdivzstepu;
			const float tdivz_q = d_tdivzorigin + v*d_tdivzstepv + u*d_tdivzstepu;
			const float zi_q    = d_ziorigin    + v*d_zistepv    + u*d_zistepu;

			of_emit_span_t sp = {
				.fb_addr    = fb_base + (uint32_t)(v * fb_row + u),
				.tex_addr   = (uint32_t)(uintptr_t)pbase,
				.count      = (uint16_t)count,
				.flags      = OF_EMIT_PERSP | OF_EMIT_SKIP_ZERO,
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

			pspan++;
		} while (pspan->count != DS_SPAN_LIST_END);

		of_emit_kick();
		return;
	}
#endif

	of_emit_prepare_framebuffer_for_cpu();
	pq_gpu_zwrite_pending = 0;

	int			count, spancount, izistep;
	int			izi;
	byte		*pdest;
	fixed16_t	s, t, snext, tnext, sstep, tstep;
	float		sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float		sdivz8stepu, tdivz8stepu, zi8stepu;
	byte		btemp;
	short		*pz;

	sstep = 0;
	tstep = 0;

	sdivz8stepu = d_sdivzstepu * 8;
	tdivz8stepu = d_tdivzstepu * 8;
	zi8stepu = d_zistepu * 8;

// we count on FP exceptions being turned off to avoid range problems
	izistep = (int)(d_zistepu * 0x8000 * 0x10000);

	do
	{
		pdest = (byte *)d_viewbuffer + (screenwidth * pspan->v) + pspan->u;
		pz = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;

		count = pspan->count;

		if (count <= 0)
			goto NextSpan;

	// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
	// we count on FP exceptions being turned off to avoid range problems
		izi = (int)(zi * 0x8000 * 0x10000);

		s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;

		t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;

		do
		{
		// calculate s and t at the far end of the span
			if (count >= 8)
				spancount = 8;
			else
				spancount = count;

			count -= spancount;

			if (count)
			{
			// calculate s/z, t/z, zi->fixed s and t at far end of span,
			// calculate s and t steps across span by shifting
				sdivz += sdivz8stepu;
				tdivz += tdivz8stepu;
				zi += zi8stepu;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;	// prevent round-off error on <0 steps from
								//  from causing overstepping & running off the
								//  edge of the texture

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;	// guard against round-off error on <0 steps

				sstep = (snext - s) >> 3;
				tstep = (tnext - t) >> 3;
			}
			else
			{
			// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
			// can't step off polygon), clamp, calculate s and t steps across
			// span by division, biasing steps low so we don't run off the
			// texture
				spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;	// prevent round-off error on <0 steps from
								//  from causing overstepping & running off the
								//  edge of the texture

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;	// guard against round-off error on <0 steps

				if (spancount > 1)
				{
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			do
			{
				btemp = *(pbase + (s >> 16) + (t >> 16) * cachewidth);
				if (btemp != 255)
				{
					if (*pz <= (izi >> 16))
					{
						*pz = izi >> 16;
						*pdest = btemp;
					}
				}

				izi += izistep;
				pdest++;
				pz++;
				s += sstep;
				t += tstep;
			} while (--spancount > 0);

			s = snext;
			t = tnext;

		} while (count > 0);

NextSpan:
		pspan++;

	} while (pspan->count != DS_SPAN_LIST_END);
}

#endif


/*
=====================
D_SpriteScanLeftEdge
=====================
*/
void D_SpriteScanLeftEdge (void)
{
	int			i, v, itop, ibottom, lmaxindex;
	emitpoint_t	*pvert, *pnext;
	sspan_t		*pspan;
	float		du, dv, vtop, vbottom, slope;
	fixed16_t	u, u_step;

	pspan = sprite_spans;
	i = minindex;
	if (i == 0)
		i = r_spritedesc.nump;

	lmaxindex = maxindex;
	if (lmaxindex == 0)
		lmaxindex = r_spritedesc.nump;

	vtop = ceilf (r_spritedesc.pverts[i].v);

	do
	{
		pvert = &r_spritedesc.pverts[i];
		pnext = pvert - 1;

		vbottom = ceilf (pnext->v);

		if (vtop < vbottom)
		{
			du = pnext->u - pvert->u;
			dv = pnext->v - pvert->v;
			slope = du / dv;
			u_step = (int)(slope * 0x10000);
		// adjust u to ceil the integer portion
			u = (int)((pvert->u + (slope * (vtop - pvert->v))) * 0x10000) +
					(0x10000 - 1);
			itop = (int)vtop;
			ibottom = (int)vbottom;

			for (v=itop ; v<ibottom ; v++)
			{
				pspan->u = u >> 16;
				pspan->v = v;
				u += u_step;
				pspan++;
			}
		}

		vtop = vbottom;

		i--;
		if (i == 0)
			i = r_spritedesc.nump;

	} while (i != lmaxindex);
}


/*
=====================
D_SpriteScanRightEdge
=====================
*/
void D_SpriteScanRightEdge (void)
{
	int			i, v, itop, ibottom;
	emitpoint_t	*pvert, *pnext;
	sspan_t		*pspan;
	float		du, dv, vtop, vbottom, slope, uvert, unext, vvert, vnext;
	fixed16_t	u, u_step;

	pspan = sprite_spans;
	i = minindex;

	vvert = r_spritedesc.pverts[i].v;
	if (vvert < r_refdef.fvrecty_adj)
		vvert = r_refdef.fvrecty_adj;
	if (vvert > r_refdef.fvrectbottom_adj)
		vvert = r_refdef.fvrectbottom_adj;

	vtop = ceilf (vvert);

	do
	{
		pvert = &r_spritedesc.pverts[i];
		pnext = pvert + 1;

		vnext = pnext->v;
		if (vnext < r_refdef.fvrecty_adj)
			vnext = r_refdef.fvrecty_adj;
		if (vnext > r_refdef.fvrectbottom_adj)
			vnext = r_refdef.fvrectbottom_adj;

		vbottom = ceilf (vnext);

		if (vtop < vbottom)
		{
			uvert = pvert->u;
			if (uvert < r_refdef.fvrectx_adj)
				uvert = r_refdef.fvrectx_adj;
			if (uvert > r_refdef.fvrectright_adj)
				uvert = r_refdef.fvrectright_adj;

			unext = pnext->u;
			if (unext < r_refdef.fvrectx_adj)
				unext = r_refdef.fvrectx_adj;
			if (unext > r_refdef.fvrectright_adj)
				unext = r_refdef.fvrectright_adj;

			du = unext - uvert;
			dv = vnext - vvert;
			slope = du / dv;
			u_step = (int)(slope * 0x10000);
		// adjust u to ceil the integer portion
			u = (int)((uvert + (slope * (vtop - vvert))) * 0x10000) +
					(0x10000 - 1);
			itop = (int)vtop;
			ibottom = (int)vbottom;

			for (v=itop ; v<ibottom ; v++)
			{
				pspan->count = (u >> 16) - pspan->u;
				u += u_step;
				pspan++;
			}
		}

		vtop = vbottom;
		vvert = vnext;

		i++;
		if (i == r_spritedesc.nump)
			i = 0;

	} while (i != maxindex);

	pspan->count = DS_SPAN_LIST_END;	// mark the end of the span list 
}


/*
=====================
D_SpriteCalculateGradients
=====================
*/
void D_SpriteCalculateGradients (void)
{
	vec3_t		p_normal, p_saxis, p_taxis, p_temp1;
	float		distinv;

	TransformVector (r_spritedesc.vpn, p_normal);
	TransformVector (r_spritedesc.vright, p_saxis);
	TransformVector (r_spritedesc.vup, p_taxis);
	VectorInverse (p_taxis);

	distinv = 1.0f / (-DotProduct (modelorg, r_spritedesc.vpn));

	d_sdivzstepu = p_saxis[0] * xscaleinv;
	d_tdivzstepu = p_taxis[0] * xscaleinv;

	d_sdivzstepv = -p_saxis[1] * yscaleinv;
	d_tdivzstepv = -p_taxis[1] * yscaleinv;

	d_zistepu = p_normal[0] * xscaleinv * distinv;
	d_zistepv = -p_normal[1] * yscaleinv * distinv;

	d_sdivzorigin = p_saxis[2] - xcenter * d_sdivzstepu -
			ycenter * d_sdivzstepv;
	d_tdivzorigin = p_taxis[2] - xcenter * d_tdivzstepu -
			ycenter * d_tdivzstepv;
	d_ziorigin = p_normal[2] * distinv - xcenter * d_zistepu -
			ycenter * d_zistepv;

	TransformVector (modelorg, p_temp1);

	sadjust = ((fixed16_t)(DotProduct (p_temp1, p_saxis) * 0x10000 + 0.5f)) -
			(-(cachewidth >> 1) << 16);
	tadjust = ((fixed16_t)(DotProduct (p_temp1, p_taxis) * 0x10000 + 0.5f)) -
			(-(sprite_height >> 1) << 16);

// -1 (-epsilon) so we never wander off the edge of the texture
	bbextents = (cachewidth << 16) - 1;
	bbextentt = (sprite_height << 16) - 1;
}


/*
=====================
D_DrawSprite
=====================
*/
void D_DrawSprite (void)
{
	int			i, nump;
	float		ymin, ymax;
	emitpoint_t	*pverts;
	static sspan_t	spans[MAXHEIGHT+1];

	sprite_spans = spans;

// find the top and bottom vertices, and make sure there's at least one scan to
// draw
	ymin = 999999.9;
	ymax = -999999.9;
	pverts = r_spritedesc.pverts;

	for (i=0 ; i<r_spritedesc.nump ; i++)
	{
		if (pverts->v < ymin)
		{
			ymin = pverts->v;
			minindex = i;
		}

		if (pverts->v > ymax)
		{
			ymax = pverts->v;
			maxindex = i;
		}

		pverts++;
	}

	ymin = ceilf (ymin);
	ymax = ceilf (ymax);

	if (ymin >= ymax)
		return;		// doesn't cross any scans at all

	cachewidth = r_spritedesc.pspriteframe->width;
	sprite_height = r_spritedesc.pspriteframe->height;
	cacheblock = (byte *)&r_spritedesc.pspriteframe->pixels[0];

// copy the first vertex to the last vertex, so we don't have to deal with
// wrapping
	nump = r_spritedesc.nump;
	pverts = r_spritedesc.pverts;
	pverts[nump] = pverts[0];

	D_SpriteCalculateGradients ();
	D_SpriteScanLeftEdge ();
	D_SpriteScanRightEdge ();
	D_SpriteDrawSpans (sprite_spans);
}
