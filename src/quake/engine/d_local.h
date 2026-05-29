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
// d_local.h:  private rasterization driver defs

#include "r_shared.h"

//
// TODO: fine-tune this; it's based on providing some overage even if there
// is a 2k-wide scan, with subdivision every 8, for 256 spans of 12 bytes each
//
#define SCANBUFFERPAD		0x1000

#define R_SKY_SMASK	0x007F0000
#define R_SKY_TMASK	0x007F0000

#define DS_SPAN_LIST_END	-128

#define SURFCACHE_SIZE_AT_320X200	600*1024

typedef struct surfcache_s
{
	struct surfcache_s	*next;
	struct surfcache_s 	**owner;		// NULL is an empty chunk of memory
	int					lightadj[MAXLIGHTMAPS]; // checked for strobe flush
	int					dlight;
	int					size;		// including header
	unsigned			width;
	unsigned			height;		// DEBUG only needed for debug
	float				mipscale;
	struct texture_s	*texture;	// checked for animating textures
	// Pad so data[] starts at 64-byte D-cache line boundary (offset 64).
	// CPU metadata writes (above) and HW pixel writes (data[]) are in
	// separate cache lines, eliminating the need for fence.i in D_CacheSurface.
	int					gpu_dirty;	// CPU rebuilt data[]; GPU needs cache clean before read
	int					_pad[3];
	byte				data[4];	// width*height elements
} surfcache_t;

static inline surfcache_t *D_SurfCacheForData (void *data)
{
	return (surfcache_t *)((byte *)data - __builtin_offsetof(surfcache_t, data));
}

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct sspan_s
{
	int				u, v, count;
} sspan_t;

extern cvar_t	d_subdiv16;

extern float	scale_for_mip;

extern qboolean		d_roverwrapped;
extern surfcache_t	*sc_rover;
extern surfcache_t	*d_initial_rover;

extern float	d_sdivzstepu, d_tdivzstepu, d_zistepu;
extern float	d_sdivzstepv, d_tdivzstepv, d_zistepv;
extern float	d_sdivzorigin, d_tdivzorigin, d_ziorigin;

fixed16_t	sadjust, tadjust;
fixed16_t	bbextents, bbextentt;


PQ_FASTTEXT void D_DrawSpans8 (espan_t *pspans);
void D_DrawSpans16 (espan_t *pspans);
PQ_FASTTEXT void D_DrawZSpans (espan_t *pspans);

/* Experimental world-triangle path.  The shipped LITE GPU has no
 * triangle rasterizer, so keep this out of the default build and run
 * the supported span path explicitly. */
#ifndef D_GPU_WORLD_TRIS
#define D_GPU_WORLD_TRIS 0
#endif

/* Direct BSP-to-triangle path. Retired on current openfpgaOS SDKs; keep the
 * code compiled out and use parametric spans for the supported GPU path. */
#ifndef D_GPU_WORLD_DIRECT
#define D_GPU_WORLD_DIRECT 0
#endif

struct msurface_s;
void R_DrawSurfaceTris (struct msurface_s *fa, int miplevel);
/* Flush any accumulated world triangles to the GPU.  Caller must
 * invoke this before any GPU state change (FB rebind, kick, finish)
 * or before reading the framebuffer back to CPU.  Idempotent. */
void R_FlushWorldTriBatch (void);
void Turbulent8 (espan_t *pspan);
void D_SpriteDrawSpans (sspan_t *pspan);

void D_DrawSkyScans8 (espan_t *pspan);
void D_DrawSkyScans16 (espan_t *pspan);

void R_ShowSubDiv (void);
void (*prealspandrawer)(void);
surfcache_t	*D_CacheSurface (msurface_t *surface, int miplevel);
byte		D_GpuLightSurface (msurface_t *surface, int miplevel);
extern unsigned blocklights[18*18];
extern byte pq_world_light;
extern unsigned short pq_world_tex_w_mask;
extern unsigned short pq_world_tex_h_mask;
extern int pq_world_tex_s_offset;
extern int pq_world_tex_t_offset;
extern int pq_world_light_mode;

extern int D_MipLevelForScale (float scale);

#if id386
extern void D_PolysetAff8Start (void);
extern void D_PolysetAff8End (void);
#endif

extern short *d_pzbuffer;
extern unsigned int d_zrowbytes, d_zwidth;

extern int	*d_pscantable;
extern int	d_scantable[MAXHEIGHT];

extern int	d_vrectx, d_vrecty, d_vrectright_particle, d_vrectbottom_particle;

extern int	d_y_aspect_shift, d_pix_min, d_pix_max, d_pix_shift;

extern pixel_t	*d_viewbuffer;

extern short	*zspantable[MAXHEIGHT];

extern int		d_minmip;
extern float	d_scalemip[3];

extern void (*d_drawspans) (espan_t *pspan);
