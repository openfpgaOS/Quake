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
// d_part.c: software driver module for drawing particles

#include "quakedef.h"
#include "d_local.h"
#include "of_emit.h"

extern int pq_gpu_zwrite_pending;

static byte d_particle_colors[256] __attribute__((aligned(64)));
static int d_particle_colors_ready;

static void D_InitParticleColors (void)
{
	int i;

	if (d_particle_colors_ready)
		return;

	for (i=0 ; i<256 ; i++)
		d_particle_colors[i] = (byte)i;

	of_emit_cache_clean(d_particle_colors, sizeof(d_particle_colors));
	d_particle_colors_ready = 1;
}

static void D_EmitParticleRun (uint32_t fb_addr, int count, byte color)
{
	of_emit_span_t sp = {
		.fb_addr   = fb_addr,
		.tex_addr  = (uint32_t)(uintptr_t)(d_particle_colors + color),
		.s         = 0,
		.t         = 0,
		.sstep     = 0,
		.tstep     = 0,
		.count     = (uint16_t)count,
		.flags     = 0,
		.fb_stride = 1,
		.tex_width = 1,
	};
	of_emit_span(&sp);
}


/*
==============
D_EndParticles
==============
*/
void D_EndParticles (void)
{
// not used by software driver
}


/*
==============
D_StartParticles
==============
*/
void D_StartParticles (void)
{
	D_InitParticleColors ();

	if (pq_gpu_zwrite_pending)
	{
		of_emit_prepare_framebuffer_for_cpu();
		pq_gpu_zwrite_pending = 0;
	}
}


#if	!id386

/*
==============
D_DrawParticle
==============
*/
PQ_HOT void D_DrawParticle (particle_t *pparticle)
{
	vec3_t	local, transformed;
	float	zi;
	short	*pz;
	uint32_t fb_addr;
	int		i, izi, pix, rows, row, u, v;

// transform point
	VectorSubtract (pparticle->org, r_origin, local);

	transformed[0] = DotProduct(local, r_pright);
	transformed[1] = DotProduct(local, r_pup);
	transformed[2] = DotProduct(local, r_ppn);		

	if (transformed[2] < PARTICLE_Z_CLIP)
		return;

// project the point
// FIXME: preadjust xcenter and ycenter
	zi = 1.0f / transformed[2];
	u = (int)(xcenter + zi * transformed[0] + 0.5f);
	v = (int)(ycenter - zi * transformed[1] + 0.5f);

	if ((v > d_vrectbottom_particle) || 
		(u > d_vrectright_particle) ||
		(v < d_vrecty) ||
		(u < d_vrectx))
	{
		return;
	}

	pz = d_pzbuffer + (d_zwidth * v) + u;
	fb_addr = (uint32_t)(uintptr_t)(d_viewbuffer + d_scantable[v] + u);
	izi = (int)(zi * 0x8000);

	byte color = (byte)pparticle->color;

	pix = izi >> d_pix_shift;

	if (pix < d_pix_min)
		pix = d_pix_min;
	else if (pix > d_pix_max)
		pix = d_pix_max;

	rows = pix << d_y_aspect_shift;
	for (row=0 ; row<rows ; row++, pz += d_zwidth, fb_addr += screenwidth)
	{
		int run_start = -1;

		for (i=0 ; i<pix ; i++)
		{
			if (pz[i] <= izi)
			{
				pz[i] = izi;
				if (run_start < 0)
					run_start = i;
			}
			else if (run_start >= 0)
			{
				D_EmitParticleRun(fb_addr + run_start,
					i - run_start, color);
				run_start = -1;
			}
		}

		if (run_start >= 0)
			D_EmitParticleRun(fb_addr + run_start,
				pix - run_start, color);
	}
}

#endif	// !id386
