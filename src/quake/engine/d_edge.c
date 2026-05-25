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
// d_edge.c

#include "quakedef.h"
#include "d_local.h"
#include "of_emit.h"
#include "sysreg_stub.h"

extern int	pq_combined_z_active;
extern int	r_gpu_world_direct_active;
extern cvar_t	pq_gpu_world_light;

/* Sub-profiling for D_DrawSurfaces breakdown */
unsigned int pq_prof_ds_calcgrad_cycles;
unsigned int pq_prof_ds_cachesurf_cycles;
unsigned int pq_prof_ds_sky_cycles;
extern unsigned int pq_prof_spans8_cycles_frame;
extern unsigned int pq_prof_zspans_cycles_frame;
extern cvar_t pq_cycleprof;

static int	miplevel;

float		scale_for_mip;
int			screenwidth;
int			ubasestep, errorterm, erroradjustup, erroradjustdown;
int			vstartscan;

// FIXME: should go away
extern void			R_RotateBmodel (void);
extern void			R_TransformFrustum (void);

vec3_t		transformed_modelorg;

/*
==============
D_DrawPoly

==============
*/
void D_DrawPoly (void)
{
// this driver takes spans, not polygons
}


/*
=============
D_MipLevelForScale
=============
*/
PQ_HOT int D_MipLevelForScale (float scale)
{
	int		lmiplevel;

	if (scale >= d_scalemip[0] )
		lmiplevel = 0;
	else if (scale >= d_scalemip[1] )
		lmiplevel = 1;
	else if (scale >= d_scalemip[2] )
		lmiplevel = 2;
	else
		lmiplevel = 3;

	if (lmiplevel < d_minmip)
		lmiplevel = d_minmip;

	return lmiplevel;
}


/*
==============
D_DrawSolidSurface
==============
*/

// FIXME: clean this up

void D_DrawSolidSurface (surf_t *surf, int color)
{
	espan_t	*span;
	byte	*pdest;
	int		u, u2, pix;
	
	of_emit_prepare_framebuffer_for_cpu();

	pix = (color<<24) | (color<<16) | (color<<8) | color;
	for (span=surf->spans ; span ; span=span->pnext)
	{
		pdest = (byte *)d_viewbuffer + screenwidth*span->v;
		u = span->u;
		u2 = span->u + span->count - 1;
		((byte *)pdest)[u] = pix;

		if (u2 - u < 8)
		{
			for (u++ ; u <= u2 ; u++)
				((byte *)pdest)[u] = pix;
		}
		else
		{
			for (u++ ; u & 3 ; u++)
				((byte *)pdest)[u] = pix;

			u2 -= 4;
			for ( ; u <= u2 ; u+=4)
				*(int *)((byte *)pdest + u) = pix;
			u2 += 4;
			for ( ; u <= u2 ; u++)
				((byte *)pdest)[u] = pix;
		}
	}
}


/*
==============
D_CalcGradients
==============
*/
PQ_FASTTEXT void D_CalcGradients (msurface_t *pface)
{
	float		mipscale;
	vec3_t		p_temp1;
	vec3_t		p_saxis, p_taxis;
	float		t;

	mipscale = 1.0f / (float)(1 << miplevel);

	TransformVector (pface->texinfo->vecs[0], p_saxis);
	TransformVector (pface->texinfo->vecs[1], p_taxis);

	t = xscaleinv * mipscale;
	d_sdivzstepu = p_saxis[0] * t;
	d_tdivzstepu = p_taxis[0] * t;

	t = yscaleinv * mipscale;
	d_sdivzstepv = -p_saxis[1] * t;
	d_tdivzstepv = -p_taxis[1] * t;

	d_sdivzorigin = p_saxis[2] * mipscale - xcenter * d_sdivzstepu -
			ycenter * d_sdivzstepv;
	d_tdivzorigin = p_taxis[2] * mipscale - xcenter * d_tdivzstepu -
			ycenter * d_tdivzstepv;

	VectorScale (transformed_modelorg, mipscale, p_temp1);

	t = 0x10000*mipscale;
	sadjust = ((fixed16_t)(DotProduct (p_temp1, p_saxis) * 0x10000 + 0.5f)) -
			((pface->texturemins[0] << 16) >> miplevel)
			+ pface->texinfo->vecs[0][3]*t;
	tadjust = ((fixed16_t)(DotProduct (p_temp1, p_taxis) * 0x10000 + 0.5f)) -
			((pface->texturemins[1] << 16) >> miplevel)
			+ pface->texinfo->vecs[1][3]*t;

//
// -1 (-epsilon) so we never wander off the edge of the texture
//
	bbextents = ((pface->extents[0] << 16) >> miplevel) - 1;
	bbextentt = ((pface->extents[1] << 16) >> miplevel) - 1;
}


/*
==============
D_DrawSurfaces
==============
*/
PQ_HOT void D_DrawSurfaces (void)
{
	surf_t			*s;
	msurface_t		*pface;
	surfcache_t		*pcurrentcache;
	vec3_t			world_transformed_modelorg;
	vec3_t			local_modelorg;
	int profiling = (int)pq_cycleprof.value;
	unsigned int prof_t;

	currententity = &cl_entities[0];
	TransformVector (modelorg, transformed_modelorg);
	VectorCopy (transformed_modelorg, world_transformed_modelorg);


// TODO: could preset a lot of this at mode set time
	if (r_drawflat.value)
	{
		for (s = &surfaces[1] ; s<surface_p ; s++)
		{
			if (!s->spans)
				continue;

			d_zistepu = s->d_zistepu;
			d_zistepv = s->d_zistepv;
			d_ziorigin = s->d_ziorigin;

			D_DrawSolidSurface (s, (int)s->data & 0xFF);
			if (profiling) prof_t = SYS_CYCLE_LO;
			D_DrawZSpans (s->spans);
			if (profiling) pq_prof_zspans_cycles_frame += SYS_CYCLE_LO - prof_t;
		}
	}
	else
	{
		entity_t *last_bmodel_entity = NULL;

		for (s = &surfaces[1] ; s<surface_p ; s++)
		{
			if (!s->spans)
				continue;

			r_drawnpolycount++;

			d_zistepu = s->d_zistepu;
			d_zistepv = s->d_zistepv;
			d_ziorigin = s->d_ziorigin;

			if (s->flags & SURF_DRAWSKY)
			{
				if (!r_skymade)
				{
					R_MakeSky ();
				}

				if (profiling) prof_t = SYS_CYCLE_LO;
				D_DrawSkyScans8 (s->spans);
				if (profiling) pq_prof_ds_sky_cycles += SYS_CYCLE_LO - prof_t;
			}
			else if (s->flags & SURF_DRAWBACKGROUND)
			{
				d_zistepu = 0;
				d_zistepv = 0;
				d_ziorigin = -0.9;

				if (!r_gpu_world_direct_active)
				{
					D_DrawSolidSurface (s, (int)r_clearcolor.value & 0xFF);
					if (profiling) prof_t = SYS_CYCLE_LO;
					D_DrawZSpans (s->spans);
					if (profiling) pq_prof_zspans_cycles_frame += SYS_CYCLE_LO - prof_t;
				}
			}
			else if (s->flags & SURF_DRAWTURB)
			{
				pface = s->data;
				miplevel = 0;
				cacheblock = (pixel_t *)
						((byte *)pface->texinfo->texture +
						pface->texinfo->texture->offsets[0]);
				cachewidth = 64;

				if (s->insubmodel)
				{
					if (s->entity != last_bmodel_entity) {
						if (last_bmodel_entity) {
							currententity = &cl_entities[0];
							VectorCopy(world_transformed_modelorg, transformed_modelorg);
							VectorCopy(base_vpn, vpn);
							VectorCopy(base_vup, vup);
							VectorCopy(base_vright, vright);
							VectorCopy(base_modelorg, modelorg);
							R_TransformFrustum();
						}
						currententity = s->entity;
						VectorSubtract(r_origin, currententity->origin, local_modelorg);
						TransformVector(local_modelorg, transformed_modelorg);
						/* Update GLOBAL modelorg before R_RotateBmodel.  R_RotateBmodel
						 * rotates `modelorg` in place; without this copy, it rotates
						 * the leftover base value (r_origin) instead of (r_origin -
						 * entity_origin), giving a translation offset to any code
						 * that uses the GLOBAL modelorg later — including
						 * R_DrawSurfaceTris's VectorSubtract(world, modelorg, local).
						 * Mirrors the R_DrawBrushModel setup in r_main.c:1010. */
						VectorCopy(local_modelorg, modelorg);
						R_RotateBmodel();
						last_bmodel_entity = s->entity;
					}
				}
				else if (last_bmodel_entity)
				{
					currententity = &cl_entities[0];
					VectorCopy(world_transformed_modelorg, transformed_modelorg);
					VectorCopy(base_vpn, vpn);
					VectorCopy(base_vup, vup);
					VectorCopy(base_vright, vright);
					VectorCopy(base_modelorg, modelorg);
					R_TransformFrustum();
					last_bmodel_entity = NULL;
				}

				if (profiling) prof_t = SYS_CYCLE_LO;
				D_CalcGradients (pface);
				if (profiling) pq_prof_ds_calcgrad_cycles += SYS_CYCLE_LO - prof_t;
				Turbulent8 (s->spans);
				if (!pq_combined_z_active)
				{
					if (profiling) prof_t = SYS_CYCLE_LO;
					D_DrawZSpans (s->spans);
					if (profiling) pq_prof_zspans_cycles_frame += SYS_CYCLE_LO - prof_t;
				}
				pq_combined_z_active = 0;
			}
			else
			{
				if (s->insubmodel)
				{
					if (s->entity != last_bmodel_entity) {
						if (last_bmodel_entity) {
							currententity = &cl_entities[0];
							VectorCopy(world_transformed_modelorg, transformed_modelorg);
							VectorCopy(base_vpn, vpn);
							VectorCopy(base_vup, vup);
							VectorCopy(base_vright, vright);
							VectorCopy(base_modelorg, modelorg);
							R_TransformFrustum();
						}
						currententity = s->entity;
						VectorSubtract(r_origin, currententity->origin, local_modelorg);
						TransformVector(local_modelorg, transformed_modelorg);
						/* Update GLOBAL modelorg before R_RotateBmodel.  R_RotateBmodel
						 * rotates `modelorg` in place; without this copy, it rotates
						 * the leftover base value (r_origin) instead of (r_origin -
						 * entity_origin), giving a translation offset to any code
						 * that uses the GLOBAL modelorg later — including
						 * R_DrawSurfaceTris's VectorSubtract(world, modelorg, local).
						 * Mirrors the R_DrawBrushModel setup in r_main.c:1010. */
						VectorCopy(local_modelorg, modelorg);
						R_RotateBmodel();
						last_bmodel_entity = s->entity;
					}
				}
				else if (last_bmodel_entity)
				{
					currententity = &cl_entities[0];
					VectorCopy(world_transformed_modelorg, transformed_modelorg);
					VectorCopy(base_vpn, vpn);
					VectorCopy(base_vup, vup);
					VectorCopy(base_vright, vright);
					VectorCopy(base_modelorg, modelorg);
					R_TransformFrustum();
					last_bmodel_entity = NULL;
				}

				pface = s->data;
				miplevel = D_MipLevelForScale (s->nearzi * scale_for_mip
				* pface->texinfo->mipadjust);

				if (profiling) prof_t = SYS_CYCLE_LO;
#if D_GPU_WORLD_LIGHT
				if ((int)pq_gpu_world_light.value)
				{
					/* Optional raw-texture GPU lighting path.  Keep it
					 * opt-in until the GPU perspective path is exact with
					 * large absolute texture coordinates. */
					pq_world_light = D_GpuLightSurface (pface, miplevel);
					(void)pcurrentcache;
				}
				else
#endif
				{
					pcurrentcache = D_CacheSurface (pface, miplevel);
					cacheblock = (pixel_t *)pcurrentcache->data;
					cachewidth = pcurrentcache->width;
					pq_world_light = 0;
					pq_world_tex_w_mask = 0;
					pq_world_tex_h_mask = 0;
					pq_world_tex_s_offset = 0;
					pq_world_tex_t_offset = 0;
				}
				if (profiling) pq_prof_ds_cachesurf_cycles += SYS_CYCLE_LO - prof_t;

	#if D_GPU_WORLD_TRIS && !D_GPU_WORLD_DIRECT
				/* T6 better: tessellate the surface into GPU triangles
				 * with per-vertex Gouraud light from blocklights[]
				 * (populated by D_GpuLightSurface above).  Replaces
				 * D_CalcGradients + (*d_drawspans) — gradients are
				 * computed per-vertex by R_PackTriVert and the GPU's
				 * triangle rasteriser does perspective + per-pixel
				 * colormap lookup. */
				R_DrawSurfaceTris (pface, miplevel);
#else
				if (profiling) prof_t = SYS_CYCLE_LO;
				D_CalcGradients (pface);
				if (profiling) pq_prof_ds_calcgrad_cycles += SYS_CYCLE_LO - prof_t;

				if (profiling) prof_t = SYS_CYCLE_LO;
				(*d_drawspans) (s->spans);
				if (profiling) pq_prof_spans8_cycles_frame += SYS_CYCLE_LO - prof_t;
#endif

				/* CPU-side z-fill for sprite/alias depth occlusion when the
				 * selected world draw path did not co-write compatible z. */
				if (!pq_combined_z_active)
				{
					if (profiling) prof_t = SYS_CYCLE_LO;
					D_DrawZSpans (s->spans);
					if (profiling) pq_prof_zspans_cycles_frame += SYS_CYCLE_LO - prof_t;
				}
				pq_combined_z_active = 0;
			}
		}

		// Restore world state if loop ended while in bmodel state
		if (last_bmodel_entity)
		{
			currententity = &cl_entities[0];
			VectorCopy(world_transformed_modelorg, transformed_modelorg);
			VectorCopy(base_vpn, vpn);
			VectorCopy(base_vup, vup);
			VectorCopy(base_vright, vright);
			VectorCopy(base_modelorg, modelorg);
			R_TransformFrustum();
		}
	}

#if D_GPU_WORLD_TRIS
	/* Drain any triangles still pending from the texture-bucketed
	 * batch.  R_DrawSurfaceTris accumulates across consecutive
	 * same-texture surfaces; this flush guarantees the batch is
	 * empty before the caller (R_ScanEdges) potentially proceeds
	 * to other GPU work. */
	R_FlushWorldTriBatch ();
#endif
}
