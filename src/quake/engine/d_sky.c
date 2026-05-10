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
// d_sky.c

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"
#include "of_emit.h"

#define SKY_SPAN_SHIFT	8
#define SKY_SPAN_MAX	(1 << SKY_SPAN_SHIFT)


/*
=================
D_Sky_uv_To_st
=================
*/
void D_Sky_uv_To_st (int u, int v, fixed16_t *s, fixed16_t *t)
{
	float	wu, wv, temp;
	vec3_t	end;

	if (r_refdef.vrect.width >= r_refdef.vrect.height)
		temp = (float)r_refdef.vrect.width;
	else
		temp = (float)r_refdef.vrect.height;

	wu = 8192.0 * (float)(u-((int)vid.width>>1)) / temp;
	wv = 8192.0 * (float)(((int)vid.height>>1)-v) / temp;

	end[0] = 4096*vpn[0] + wu*vright[0] + wv*vup[0];
	end[1] = 4096*vpn[1] + wu*vright[1] + wv*vup[1];
	end[2] = 4096*vpn[2] + wu*vright[2] + wv*vup[2];
	end[2] *= 3;
	VectorNormalize (end);

	temp = skytime*skyspeed;	// TODO: add D_SetupFrame & set this there
	*s = (int)((temp + 6*(SKYSIZE/2-1)*end[0]) * 0x10000);
	*t = (int)((temp + 6*(SKYSIZE/2-1)*end[1]) * 0x10000);
}


/*
=================
D_DrawSkyScans8
=================
*/
void D_DrawSkyScans8 (espan_t *pspan)
{
	/* GPU path. Sky is a 128×128 paletted texture (composited from two
	 * scrolling layers by R_MakeSky into r_skysource). For each input
	 * scanline we sub-divide into SKY_SPAN_MAX-pixel chunks because
	 * D_Sky_uv_To_st is non-linear across u — straight affine across
	 * a long span would skew the texture. Emit one affine GPU SPAN
	 * per chunk with POT wrap masks so the sky tiles correctly.
	 *
	 * No depth test (sky is the far backdrop), no skip-zero (front
	 * layer transparency was merged into r_skysource by R_MakeSky),
	 * no colormap (raw passthrough). */
	extern viddef_t vid;
	const uint32_t  fb_base = (uint32_t)(uintptr_t)vid.buffer;
	const int       fb_row  = vid.rowbytes;

	do {
		int        v       = pspan->v;
		int        u_start = pspan->u;
		int        u_end   = pspan->u + pspan->count;
		int        u       = u_start;
		fixed16_t  s, t, snext, tnext, sstep = 0, tstep = 0;

		D_Sky_uv_To_st (u, v, &s, &t);

		while (u < u_end) {
			int remain = u_end - u;
			int spancount = (remain >= SKY_SPAN_MAX) ? SKY_SPAN_MAX : remain;
			int u_next = u + spancount;

			if (u_next < u_end) {
				D_Sky_uv_To_st (u_next, v, &snext, &tnext);
				sstep = (snext - s) >> SKY_SPAN_SHIFT;
				tstep = (tnext - t) >> SKY_SPAN_SHIFT;
			} else if (spancount > 1) {
				D_Sky_uv_To_st (u + spancount - 1, v, &snext, &tnext);
				sstep = (snext - s) / (spancount - 1);
				tstep = (tnext - t) / (spancount - 1);
			} else {
				snext = s; tnext = t;
				sstep = tstep = 0;
			}

			of_emit_span_t sp = {
				.fb_addr   = fb_base + (uint32_t)(v * fb_row + u),
				.tex_addr  = (uint32_t)(uintptr_t)r_skysource,
				.s         = s,
				.t         = t,
				.sstep     = sstep,
				.tstep     = tstep,
				.count     = (uint16_t)spancount,
				.flags     = 0,
				.fb_stride = 1,
				.tex_width = SKYSIZE,
				.tex_w_mask = SKYSIZE - 1,
				.tex_h_mask = SKYSIZE - 1,
			};
			of_emit_span(&sp);

			u = u_next;
			s = snext;
			t = tnext;
		}
	} while ((pspan = pspan->pnext) != NULL);

	of_emit_kick();
}

