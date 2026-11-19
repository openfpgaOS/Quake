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
// r_edge.c

#include "quakedef.h"
#include "r_local.h"
#include "sysreg_stub.h"

#if 0
// FIXME
the complex cases add new polys on most lines, so dont optimize for keeping them the same
have multiple free span lists to try to get better coherence?
low depth complexity -- 1 to 3 or so

this breaks spans at every edge, even hidden ones (bad)

have a sentinal at both ends?
#endif


edge_t	*auxedges;
edge_t	*r_edges, *edge_p, *edge_max;

surf_t	*surfaces, *surface_p, *surf_max;

// surfaces are generated in back to front order by the bsp, so if a surf
// pointer is greater than another one, it should be drawn in front
// surfaces[1] is the background, and is used as the active surface stack

edge_t	*newedges[MAXHEIGHT];

espan_t	*span_p, *max_span_p;

int		r_currentkey;

extern	int	screenwidth;

int	current_iv;

int	edge_head_u_shift20, edge_tail_u_shift20;

static void (*pdrawfunc_array)(void);

edge_t	edge_head;
edge_t	edge_tail;
edge_t	edge_aftertail;
edge_t	edge_sentinel;

float	fv;

/*
==============
Array-based Active Edge Table (AET)

One 16-byte record per active edge, kept sorted by u.  Both per-scanline
walks then run sequentially off a single pointer with no dependent load:
R_StepRemoveEdges_Array reads u/u_step/v_end, R_GenerateSpans_Array reads
u and the two surface indices.  The surface indices ride in the sorted
record (packed into one word), so the scanline walk no longer gathers
through an order[] -> pool[] indirection.
==============
*/
typedef struct
{
	fixed16_t		u;			// 12.20 fixed-point x position
	fixed16_t		u_step;		// per-scanline x step
	unsigned int	surfs;		// surfs[0] | surfs[1]<<16 ([0]=trailing [1]=leading)
	unsigned int	v_end;		// last scanline this edge is active
} aetedge_t;

static aetedge_t	aet[NUMSTACKEDGES];
static int			aet_count;	// number of active edges

// Store v_end in edge_t->prev (unused by array-based AET, same cache line as u/surfs)
#define EDGE_V_END(e)  ((unsigned short)(unsigned long)((e)->prev))

// edge_t.surfs is two shorts at a word-aligned offset; fetch the pair as
// one word (the assume_aligned keeps it a single load)
static inline unsigned int EdgeSurfs32 (const edge_t *e)
{
	unsigned int	w;

	memcpy (&w, __builtin_assume_aligned (e->surfs, 4), sizeof(w));
	return w;
}

// Array-based AET functions
void R_InsertNewEdges_Array (edge_t *edgestoadd);
void R_StepRemoveEdges_Array (int iv);
void R_GenerateSpans_Array (void);
void R_GenerateSpansBackward_Array (void);
void R_TrailingEdge_A (surf_t *surf, int u);
void R_LeadingEdge_A (int surf_idx, int u);
void R_LeadingEdgeBackwards_A (int surf_idx, int u);


//=============================================================================


/*
==============
R_DrawCulledPolys
==============
*/
void R_DrawCulledPolys (void)
{
	surf_t			*s;
	msurface_t		*pface;

	currententity = &cl_entities[0];

	if (r_worldpolysbacktofront)
	{
		for (s=surface_p-1 ; s>&surfaces[1] ; s--)
		{
			if (!s->spans)
				continue;

			if (!(s->flags & SURF_DRAWBACKGROUND))
			{
				pface = (msurface_t *)s->data;
				R_RenderPoly (pface, 15);
			}
		}
	}
	else
	{
		for (s = &surfaces[1] ; s<surface_p ; s++)
		{
			if (!s->spans)
				continue;

			if (!(s->flags & SURF_DRAWBACKGROUND))
			{
				pface = (msurface_t *)s->data;
				R_RenderPoly (pface, 15);
			}
		}
	}
}


/*
==============
R_BeginEdgeFrame
==============
*/
PQ_FASTTEXT void R_BeginEdgeFrame (void)
{
	int		v;

	edge_p = r_edges;
	edge_max = &r_edges[r_numallocatededges];

	surface_p = &surfaces[2];	// background is surface 1,
								//  surface 0 is a dummy
	surfaces[1].spans = NULL;	// no background spans yet
	surfaces[1].flags = SURF_DRAWBACKGROUND;

// put the background behind everything in the world
	if (r_draworder.value)
	{
		pdrawfunc_array = R_GenerateSpansBackward_Array;
		surfaces[1].key = 0;
		r_currentkey = 1;
	}
	else
	{
		pdrawfunc_array = R_GenerateSpans_Array;
		surfaces[1].key = 0x7FFFFFFF;
		r_currentkey = 0;
	}

	{
		int height = r_refdef.vrectbottom - r_refdef.vrect.y;
		memset (&newedges[r_refdef.vrect.y], 0, height * sizeof(edge_t *));
	}
}


/*
==============
R_CleanupSpan
==============
*/
PQ_FASTTEXT void R_CleanupSpan ()
{
	surf_t	*surf;
	int		iu;
	espan_t	*span;

// now that we've reached the right edge of the screen, we're done with any
// unfinished surfaces, so emit a span for whatever's on top
	surf = surfaces[1].next;
	iu = edge_tail_u_shift20;
	if (iu > surf->last_u)
	{
		span = span_p++;
		span->u = surf->last_u;
		span->count = iu - span->u;
		span->v = current_iv;
		span->pnext = surf->spans;
		surf->spans = span;
	}

// reset spanstate for all surfaces in the surface stack
	do
	{
		surf->spanstate = 0;
		surf = surf->next;
	} while (surf != &surfaces[1]);
}


/*
==============================================================================
Array-based AET functions
==============================================================================
*/

/*
==============
R_InsertNewEdges_Array

Merge sorted newedges linked list into sorted aet[] array.
==============
*/
PQ_FASTTEXT void R_InsertNewEdges_Array (edge_t *edgestoadd)
{
	edge_t		*rev, *next;
	aetedge_t	*src, *dst;
	int			new_count;

	// Reverse linked list to get descending u order for right-to-left merge.
	rev = NULL;
	new_count = 0;
	while (edgestoadd)
	{
		next = edgestoadd->next;
		edgestoadd->next = rev;
		rev = edgestoadd;
		new_count++;
		edgestoadd = next;
	}

	// Right-to-left merge straight into the sorted records: existing
	// entries are sorted ascending by u, the reversed list is descending.
	src = &aet[aet_count];
	aet_count += new_count;
	dst = &aet[aet_count];

	while (rev)
	{
		int		nu = rev->u;

		while (src > aet && src[-1].u > nu)
			*--dst = *--src;

		--dst;
		dst->u = nu;
		dst->u_step = rev->u_step;
		dst->surfs = EdgeSurfs32 (rev);
		dst->v_end = EDGE_V_END(rev);
		rev = rev->next;
	}
}


/*
==============
R_StepRemoveEdges_Array

Single pass per scanline over the sorted AET: drop entries whose
v_end == iv (edge ended on this scanline), step u for the survivors,
compact the records and insert each one into the sorted prefix as it
goes — remove, step and sort in one traversal.

Edges rarely end, so until the first one does the records keep their
slots: the leading run only reloads u/u_step/v_end, stores u back, and
sorts a crossing in place.  Past the first dropped edge the rest are
compacted down one slot as they are sorted in.  Either way this is the
same stable insertion sort the array had when the sort was a separate
pass over the compacted table.
==============
*/
PQ_FASTTEXT void R_StepRemoveEdges_Array (int iv)
{
	aetedge_t	*src, *dst, *stop, *p;
	int			u, prevu;

	stop = &aet[aet_count];
	prevu = -0x7FFFFFFF - 1;

	// nothing dropped yet: step u in place
	for (src = aet ; src < stop ; src++)
	{
		if (src->v_end == (unsigned int)iv)
			break;			// edge ends on this scanline — drop it
		u = src->u + src->u_step;
		if (u < prevu)
		{	// stepped past the edge to its left: sort it back in
			aetedge_t	e = *src;

			e.u = u;
			p = src;
			do {
				p[0] = p[-1];
				p--;
			} while (p > aet && p[-1].u > u);
			*p = e;
			continue;		// prevu is unchanged: src[0] holds it now
		}
		src->u = u;
		prevu = u;
	}

	// past the first dropped edge, survivors move down as they sort in
	dst = src;
	if (src < stop)
		src++;				// the edge that ended is gone

	for ( ; src < stop ; src++)
	{
		aetedge_t	e = *src;

		if (e.v_end == (unsigned int)iv)
			continue;
		e.u += e.u_step;

		if (e.u < prevu)
		{
			p = dst;
			do {
				p[0] = p[-1];
				p--;
			} while (p > aet && p[-1].u > e.u);
			*p = e;
		}
		else
		{
			prevu = e.u;
			*dst = e;
		}
		dst++;
	}

	aet_count = dst - aet;
}


/*
==============
R_TrailingEdge_A

Array variant: takes surface pointer and u value directly.
==============
*/
PQ_FASTTEXT void R_TrailingEdge_A (surf_t *surf, int u)
{
	espan_t		*span;
	surf_t		*snext, *sprev;
	int			iu;

	if (--surf->spanstate == 0)
	{
		if (surf->insubmodel)
			r_bmodelactive--;

		// both links read before the unlink stores, which cannot
		// change either of them
		snext = surf->next;
		sprev = surf->prev;

		if (surf == surfaces[1].next)
		{
			iu = u >> 20;
			if (iu < edge_head_u_shift20)
				iu = edge_head_u_shift20;
			else if (iu > edge_tail_u_shift20)
				iu = edge_tail_u_shift20;
			if (iu > surf->last_u)
			{
				span = span_p++;
				span->u = surf->last_u;
				span->count = iu - span->u;
				span->v = current_iv;
				span->pnext = surf->spans;
				surf->spans = span;
			}

			snext->last_u = iu;
		}

		sprev->next = snext;
		snext->prev = sprev;
	}
}


/*
==============
R_LeadingEdge_A

Array variant: takes surface index and u value directly.
==============
*/
// Precomputed constant for fixed-point to float conversion in depth tests
static const float inv_0x100000 = 1.0f / (float)0x100000;

PQ_FASTTEXT void R_LeadingEdge_A (int surf_idx, int u)
{
	espan_t		*span;
	surf_t		*surf, *surf2;
	int			iu;
	float		fu, newzi, testzi, newzitop, newzibottom;
	float		surf_zi_base;  // surf->d_ziorigin + fv*d_zistepv (once per call)

	if (surf_idx)
	{
		surf = &surfaces[surf_idx];

		if (++surf->spanstate == 1)
		{
			if (surf->insubmodel)
				r_bmodelactive++;

			surf2 = surfaces[1].next;

			if (surf->key < surf2->key)
				goto newtop;

			// Precompute fu and surf zi base ONCE for all depth comparisons.
			// Only needed for submodel coplanar cases (key==key), but computing
			// here avoids duplicate work when both depth-test branches execute.
			// fv is per-scanline (hoisted from R_ScanEdges), fu is per-edge.
			if (surf->insubmodel)
			{
				fu = (float)(u - 0xFFFFF) * inv_0x100000;
				surf_zi_base = surf->d_ziorigin + fv*surf->d_zistepv;
			}

			if (surf->insubmodel && (surf->key == surf2->key))
			{
				newzi = surf_zi_base + fu*surf->d_zistepu;
				newzibottom = newzi * 0.99f;

				testzi = surf2->d_ziorigin + fv*surf2->d_zistepv +
						fu*surf2->d_zistepu;

				if (newzibottom >= testzi)
					goto newtop;

				newzitop = newzi * 1.01f;
				if (newzitop >= testzi)
				{
					if (surf->d_zistepu >= surf2->d_zistepu)
						goto newtop;
				}
			}

continue_search:
			do
			{
				surf2 = surf2->next;
			} while (surf->key > surf2->key);

			if (surf->key == surf2->key)
			{
				if (!surf->insubmodel)
					goto continue_search;

				// fu and surf_zi_base already computed above
				newzi = surf_zi_base + fu*surf->d_zistepu;
				newzibottom = newzi * 0.99f;

				testzi = surf2->d_ziorigin + fv*surf2->d_zistepv +
						fu*surf2->d_zistepu;

				if (newzibottom >= testzi)
					goto gotposition;

				newzitop = newzi * 1.01f;
				if (newzitop >= testzi)
				{
					if (surf->d_zistepu >= surf2->d_zistepu)
						goto gotposition;
				}

				goto continue_search;
			}

			goto gotposition;

newtop:
			iu = u >> 20;
			if (iu < edge_head_u_shift20)
				iu = edge_head_u_shift20;
			else if (iu > edge_tail_u_shift20)
				iu = edge_tail_u_shift20;

			if (iu > surf2->last_u)
			{
				span = span_p++;
				span->u = surf2->last_u;
				span->count = iu - span->u;
				span->v = current_iv;
				span->pnext = surf2->spans;
				surf2->spans = span;
			}

			surf->last_u = iu;

gotposition:
			surf->next = surf2;
			surf->prev = surf2->prev;
			surf2->prev->next = surf;
			surf2->prev = surf;
		}
	}
}


/*
==============
R_LeadingEdgeBackwards_A

Array variant: takes surface index and u value directly.
==============
*/
PQ_FASTTEXT void R_LeadingEdgeBackwards_A (int surf_idx, int u)
{
	espan_t		*span;
	surf_t		*surf, *surf2;
	int			iu;

	surf = &surfaces[surf_idx];

	if (++surf->spanstate == 1)
	{
		surf2 = surfaces[1].next;

		if (surf->key > surf2->key)
			goto newtop;

		if (surf->insubmodel && (surf->key == surf2->key))
			goto newtop;

continue_search:
		do
		{
			surf2 = surf2->next;
		} while (surf->key < surf2->key);

		if (surf->key == surf2->key)
		{
			if (!surf->insubmodel)
				goto continue_search;
		}

		goto gotposition;

newtop:
		iu = u >> 20;
		if (iu < edge_head_u_shift20)
			iu = edge_head_u_shift20;
		else if (iu > edge_tail_u_shift20)
			iu = edge_tail_u_shift20;

		if (iu > surf2->last_u)
		{
			span = span_p++;
			span->u = surf2->last_u;
			span->count = iu - span->u;
			span->v = current_iv;
			span->pnext = surf2->spans;
			surf2->spans = span;
		}

		surf->last_u = iu;

gotposition:
		surf->next = surf2;
		surf->prev = surf2->prev;
		surf2->prev->next = surf;
		surf2->prev = surf;
	}
}


#if HW_SCANLINE_ACCEL
/*
==============
R_GenerateSpans_HW

Feed sorted AET edges to hardware scanline engine, read back spans.
==============
*/
PQ_HOT void R_GenerateSpans_HW (void)
{
	int i;
	int hw_count;

	r_bmodelactive = 0;

	// Cap to edge buffer size (256 entries)
	hw_count = aet_count;
	if (hw_count > 256)
		hw_count = 256;

	// Write edge count (also resets HW edge buffer write pointer)
	SCAN_EDGE_COUNT = hw_count;

	// Feed all AET edges to hardware (in sorted order)
	for (i = 0; i < hw_count; i++) {
		unsigned int sw = aet[i].surfs;
		int iu = aet[i].u >> 20;
		// Clamp to valid range — negative u from edge stepping would
		// produce garbage in the unsigned 9-bit iu field
		if (iu < 0) iu = 0;
		else if (iu > 511) iu = 511;
		SCAN_EDGE_DATA = ((unsigned int)iu << 23) |
		                 ((sw >> 16) << 10) |
		                 (sw & 0xFFFF);
	}

	// Start processing (bit 0 = start, bit 1 = backward mode)
	SCAN_CONTROL = r_draworder.value ? 0x3 : 0x1;
	scanline_wait();

	// Read span results from hardware
	{
		int nspans = SCAN_SPAN_COUNT;
		int max_si = surface_p - surfaces;

		if (nspans > 512)
			nspans = 0;  // garbage — HW not responding correctly
		for (i = 0; i < nspans; i++) {
			unsigned int hw = SCAN_SPAN_DATA;
			int si  = hw & 0x3FF;
			int u   = (hw >> 10) & 0x3FF;
			int cnt = (hw >> 20) & 0x3FF;

			if (si < 1 || si >= max_si || cnt == 0)
				continue;  // skip invalid spans
			{
				espan_t *span = span_p++;
				span->u = u;
				span->count = cnt;
				span->v = current_iv;
				span->pnext = surfaces[si].spans;
				surfaces[si].spans = span;
			}
		}
	}
}
#endif


/*
==============
R_GenerateSpans_Array

Walk sorted aet[] array instead of linked list.
==============
*/
PQ_FASTTEXT void R_GenerateSpans_Array (void)
{
	const aetedge_t	*e, *estop;

	r_bmodelactive = 0;

	surfaces[1].next = surfaces[1].prev = &surfaces[1];
	surfaces[1].last_u = edge_head_u_shift20;

	estop = &aet[aet_count];
	for (e = aet ; e < estop ; e++)
	{
		unsigned int	sw = e->surfs;
		int				u = e->u;
		unsigned int	s0 = sw & 0xFFFF;
		unsigned int	s1 = sw >> 16;

		if (s0)
		{
			R_TrailingEdge_A (&surfaces[s0], u);
			if (!s1)
				continue;
		}

		R_LeadingEdge_A (s1, u);
	}

	R_CleanupSpan ();
}


/*
==============
R_GenerateSpansBackward_Array

Walk sorted aet[] array instead of linked list (backward variant).
==============
*/
PQ_HOT void R_GenerateSpansBackward_Array (void)
{
	const aetedge_t	*e, *estop;

	r_bmodelactive = 0;

	surfaces[1].next = surfaces[1].prev = &surfaces[1];
	surfaces[1].last_u = edge_head_u_shift20;

	estop = &aet[aet_count];
	for (e = aet ; e < estop ; e++)
	{
		unsigned int	sw = e->surfs;
		int				u = e->u;
		unsigned int	s0 = sw & 0xFFFF;
		unsigned int	s1 = sw >> 16;

		if (s0)
			R_TrailingEdge_A (&surfaces[s0], u);

		if (s1)
			R_LeadingEdgeBackwards_A (s1, u);
	}

	R_CleanupSpan ();
}


/*
==============
R_ScanEdges

Input:
newedges[] array
	this has links to edges, which have links to surfaces

Output:
Each surface has a linked list of its visible spans
==============
*/

/* Span-fill (D_DrawSurfaces) time accumulates here for the host.c [edge]
 * trace; defined in r_main.c, reset per frame in R_EdgeDrawing. */
extern float r_t_surf;
extern cvar_t host_speeds;

PQ_FASTTEXT void R_ScanEdges (void)
{
	int		iv, bottom;
	static byte	basespans[MAXSPANS*sizeof(espan_t)+CACHE_SIZE];
	espan_t	*basespan_p;
	surf_t	*s;

	basespan_p = (espan_t *)
			((long)(basespans + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));
	max_span_p = &basespan_p[MAXSPANS - r_refdef.vrect.width];

	span_p = basespan_p;

// set up background edge u values (used by R_CleanupSpan and GenerateSpans)
	edge_head.u = r_refdef.vrect.x << 20;
	edge_head_u_shift20 = edge_head.u >> 20;
	edge_tail.u = (r_refdef.vrectright << 20) + 0xFFFFF;
	edge_tail_u_shift20 = edge_tail.u >> 20;

// clear array-based AET
	aet_count = 0;

#if HW_SCANLINE_ACCEL
// Frame setup: clear HW spanstate BRAM and load surface keys
	SCAN_FRAME_INIT = 1;
	scanline_wait();
	for (s = &surfaces[1] ; s<surface_p ; s++)
		scanline_load_surface(s - surfaces, s->key, s->insubmodel);
	SCAN_EDGE_HEAD_U = edge_head_u_shift20;
	SCAN_EDGE_TAIL_U = edge_tail_u_shift20;
#endif

//
// process all scan lines
//
	bottom = r_refdef.vrectbottom - 1;

	for (iv=r_refdef.vrect.y ; iv<bottom ; iv++)
	{
		current_iv = iv;
		fv = (float)iv;

	// mark that the head (background start) span is pre-included
		surfaces[1].spanstate = 1;

		if (newedges[iv])
			R_InsertNewEdges_Array (newedges[iv]);

#if HW_SCANLINE_ACCEL
		R_GenerateSpans_HW ();
#else
		(*pdrawfunc_array) ();
#endif

	// flush the span list if we can't be sure we have enough spans left for
	// the next scan
		if (span_p >= max_span_p)
		{
			if (r_drawculledpolys)
				R_DrawCulledPolys ();
			else {
				float _ts = host_speeds.value ? Sys_FloatTime () : 0;
				D_DrawSurfaces ();
				if (host_speeds.value) r_t_surf += Sys_FloatTime () - _ts;
			}

		// clear the surface span pointers
			for (s = &surfaces[1] ; s<surface_p ; s++)
				s->spans = NULL;

			span_p = basespan_p;
		}

		R_StepRemoveEdges_Array (iv);
	}

// do the last scan (no need to step or sort or remove on the last scan)
	current_iv = iv;
	fv = (float)iv;

// mark that the head (background start) span is pre-included
	surfaces[1].spanstate = 1;

	if (newedges[iv])
		R_InsertNewEdges_Array (newedges[iv]);

#if HW_SCANLINE_ACCEL
	R_GenerateSpans_HW ();
#else
	(*pdrawfunc_array) ();
#endif

// draw whatever's left in the span list
	if (r_drawculledpolys)
		R_DrawCulledPolys ();
	else {
		float _ts = host_speeds.value ? Sys_FloatTime () : 0;
		D_DrawSurfaces ();
		if (host_speeds.value) r_t_surf += Sys_FloatTime () - _ts;
	}
}
