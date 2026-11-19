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
// sv_move.c -- monster movement

#include "quakedef.h"

#define	STEPSIZE	18

/*
=============
SV_CheckBottom

Returns false if any part of the bottom of the entity is off an edge that
is not a staircase.

=============
*/
int c_yes, c_no;

// distance of a corner from a non-axial plane.  -ffast-math re-associates a
// plain DotProduct differently at every site, and the corner descent must
// round exactly as the four inlined SV_PointContents calls this replaced:
// fma(y,n1, fma(x,n0, fma(z,n2,-dist))).  Written as fmaf so the association
// is the code's and not the optimizer's.
#define CornerDist(pl,x,y,z)	fmaf ((y), (pl)->normal[1], \
								fmaf ((x), (pl)->normal[0], \
								fmaf ((z), (pl)->normal[2], -(pl)->dist)))

/*
=============
SV_CornerContents

Finishes one corner's hull 0 descent, with SV_CheckBottom's rounding.  Not
SV_HullPointContents: that one's other callers round their own way.
=============
*/
static int SV_CornerContents (mclipnode_t *clipnodes, mplane_t *planes, int num, vec3_t p)
{
	mclipnode_t	*node;
	mplane_t	*plane;
	float		d;

	while (num >= 0)
	{
		node = clipnodes + num;
		plane = planes + node->planenum;
		d = (plane->type < 3) ? p[plane->type] - plane->dist
							  : CornerDist (plane, p[0], p[1], p[2]);
		num = node->children[d < 0];
	}

	return num;
}

/*
=============
SV_CornersAllSolid

The four points under the corners of the box, walked down hull 0 together.
Near the root the box is small against the node planes, so all four corners
take the same child and one descent serves them all; once a plane separates
them the four run interleaved, which fills each other's FPU latency.  Same
answer as four SV_PointContents calls, without their early out.
=============
*/
static qboolean SV_CornersAllSolid (vec3_t mins, vec3_t maxs, float z)
{
	hull_t		*hull;
	mclipnode_t	*clipnodes, *node;
	mplane_t	*planes, *plane, *pl0, *pl1, *pl2, *pl3;
	mclipnode_t	*n0, *n1, *n2, *n3;
	vec3_t		lo, hi, c0, c1, c2, c3;
	float		dz, dxlo, dxhi;
	float		dlo, dhi, d0, d1, d2, d3;
	int			t, slo, shi;
	int			num, num0, num1, num2, num3;

	lo[0] = mins[0];	lo[1] = mins[1];	lo[2] = z;
	hi[0] = maxs[0];	hi[1] = maxs[1];	hi[2] = z;

	c0[0] = lo[0];	c0[1] = lo[1];	c0[2] = z;
	c1[0] = lo[0];	c1[1] = hi[1];	c1[2] = z;
	c2[0] = hi[0];	c2[1] = lo[1];	c2[2] = z;
	c3[0] = hi[0];	c3[1] = hi[1];	c3[2] = z;

	hull = &sv.worldmodel->hulls[0];
	clipnodes = hull->clipnodes;
	planes = hull->planes;

// shared prefix: an axis plane sees only two distinct corner distances, and
// the corners only part when those two straddle it
	num = 0;
	while (num >= 0)
	{
		node = clipnodes + num;
		plane = planes + node->planenum;
		t = plane->type;
		if (t < 3)
		{
			dlo = lo[t] - plane->dist;
			dhi = hi[t] - plane->dist;
			slo = (dlo < 0);
			shi = (dhi < 0);
			if (slo != shi)
			{
				num0 = node->children[slo];
				num3 = node->children[shi];
				if (t == 0)
				{	// lo[0] is the x of c0 and c1
					num1 = num0;
					num2 = num3;
				}
				else
				{	// t == 1; lo[1] is the y of c0 and c2.  t == 2 cannot
					// reach here, lo[2] == hi[2] == z
					num2 = num0;
					num1 = num3;
				}
				goto parted;
			}
			num = node->children[slo];
		}
		else
		{	// the corners share z, and pairwise share x, so the common
			// leading fmas are computed once -- each d is still exactly the
			// three-fma chain for its own corner
			dz = fmaf (z, plane->normal[2], -plane->dist);
			dxlo = fmaf (lo[0], plane->normal[0], dz);
			dxhi = fmaf (hi[0], plane->normal[0], dz);
			d0 = fmaf (lo[1], plane->normal[1], dxlo);
			d1 = fmaf (hi[1], plane->normal[1], dxlo);
			d2 = fmaf (lo[1], plane->normal[1], dxhi);
			d3 = fmaf (hi[1], plane->normal[1], dxhi);
			num0 = node->children[d0 < 0];
			num1 = node->children[d1 < 0];
			num2 = node->children[d2 < 0];
			num3 = node->children[d3 < 0];
			if (num0 != num1 || num0 != num2 || num0 != num3)
				goto parted;
			num = num0;
		}
	}

	return num == CONTENTS_SOLID;

parted:
	while ((num0 | num1 | num2 | num3) >= 0)
	{
		n0 = clipnodes + num0;	pl0 = planes + n0->planenum;
		d0 = (pl0->type < 3) ? c0[pl0->type] - pl0->dist
							 : CornerDist (pl0, c0[0], c0[1], c0[2]);
		n1 = clipnodes + num1;	pl1 = planes + n1->planenum;
		d1 = (pl1->type < 3) ? c1[pl1->type] - pl1->dist
							 : CornerDist (pl1, c1[0], c1[1], c1[2]);
		n2 = clipnodes + num2;	pl2 = planes + n2->planenum;
		d2 = (pl2->type < 3) ? c2[pl2->type] - pl2->dist
							 : CornerDist (pl2, c2[0], c2[1], c2[2]);
		n3 = clipnodes + num3;	pl3 = planes + n3->planenum;
		d3 = (pl3->type < 3) ? c3[pl3->type] - pl3->dist
							 : CornerDist (pl3, c3[0], c3[1], c3[2]);

		num0 = n0->children[d0 < 0];
		num1 = n1->children[d1 < 0];
		num2 = n2->children[d2 < 0];
		num3 = n3->children[d3 < 0];
	}

// whichever descents have not reached a leaf yet finish on their own
	num0 = SV_CornerContents (clipnodes, planes, num0, c0);
	num1 = SV_CornerContents (clipnodes, planes, num1, c1);
	num2 = SV_CornerContents (clipnodes, planes, num2, c2);
	num3 = SV_CornerContents (clipnodes, planes, num3, c3);

	return num0 == CONTENTS_SOLID && num1 == CONTENTS_SOLID
		&& num2 == CONTENTS_SOLID && num3 == CONTENTS_SOLID;
}

qboolean SV_CheckBottom (edict_t *ent)
{
	vec3_t	mins, maxs, start, stop;
	trace_t	trace;
	int		x, y;
	float	mid, bottom;

	VectorAdd (ent->v.origin, ent->v.mins, mins);
	VectorAdd (ent->v.origin, ent->v.maxs, maxs);

// if all of the points under the corners are solid world, don't bother
// with the tougher checks
// the corners must be within 16 of the midpoint
	if (!SV_CornersAllSolid (mins, maxs, mins[2] - 1))
		goto realcheck;

	c_yes++;
	return true;		// we got out easy

realcheck:
	c_no++;
//
// check it for real...
//
	start[2] = mins[2];
	
// the midpoint must be within 16 of the bottom
	start[0] = stop[0] = (mins[0] + maxs[0])*0.5;
	start[1] = stop[1] = (mins[1] + maxs[1])*0.5;
	stop[2] = start[2] - 2*STEPSIZE;
	trace = SV_Move (start, vec3_origin, vec3_origin, stop, true, ent);

	if (trace.fraction == 1.0f)
		return false;
	mid = bottom = trace.endpos[2];
	
// the corners must be within 16 of the midpoint	
	for	(x=0 ; x<=1 ; x++)
		for	(y=0 ; y<=1 ; y++)
		{
			start[0] = stop[0] = x ? maxs[0] : mins[0];
			start[1] = stop[1] = y ? maxs[1] : mins[1];
			
			trace = SV_Move (start, vec3_origin, vec3_origin, stop, true, ent);
			
			if (trace.fraction != 1.0f && trace.endpos[2] > bottom)
				bottom = trace.endpos[2];
			if (trace.fraction == 1.0f || mid - trace.endpos[2] > STEPSIZE)
				return false;
		}

	c_yes++;
	return true;
}


/*
=============
SV_movestep

Called by monster program code.
The move will be adjusted for slopes and stairs, but if the move isn't
possible, no move is done, false is returned, and
pr_global_struct->trace_normal is set to the normal of the blocking wall
=============
*/
qboolean SV_movestep (edict_t *ent, vec3_t move, qboolean relink)
{
	float		dz;
	vec3_t		oldorg, neworg, end;
	trace_t		trace;
	int			i;
	edict_t		*enemy;

// try the move	
	VectorCopy (ent->v.origin, oldorg);
	VectorAdd (ent->v.origin, move, neworg);

// flying monsters don't step up
	if ( (int)ent->v.flags & (FL_SWIM | FL_FLY) )
	{
	// try one move with vertical motion, then one without
		for (i=0 ; i<2 ; i++)
		{
			VectorAdd (ent->v.origin, move, neworg);
			enemy = PROG_TO_EDICT(ent->v.enemy);
			if (i == 0 && enemy != sv.edicts)
			{
				dz = ent->v.origin[2] - PROG_TO_EDICT(ent->v.enemy)->v.origin[2];
				if (dz > 40)
					neworg[2] -= 8;
				if (dz < 30)
					neworg[2] += 8;
			}
			trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, neworg, false, ent);
	
			if (trace.fraction == 1.0f)
			{
				if ( ((int)ent->v.flags & FL_SWIM) && SV_PointContents(trace.endpos) == CONTENTS_EMPTY )
					return false;	// swim monster left water
	
				VectorCopy (trace.endpos, ent->v.origin);
				if (relink)
					SV_LinkEdict (ent, true);
				return true;
			}
			
			if (enemy == sv.edicts)
				break;
		}
		
		return false;
	}

// push down from a step height above the wished position
	neworg[2] += STEPSIZE;
	VectorCopy (neworg, end);
	end[2] -= STEPSIZE*2;

	trace = SV_Move (neworg, ent->v.mins, ent->v.maxs, end, false, ent);

	if (trace.allsolid)
		return false;

	if (trace.startsolid)
	{
		neworg[2] -= STEPSIZE;
		trace = SV_Move (neworg, ent->v.mins, ent->v.maxs, end, false, ent);
		if (trace.allsolid || trace.startsolid)
			return false;
	}
	if (trace.fraction == 1.0f)
	{
	// if monster had the ground pulled out, go ahead and fall
		if ( (int)ent->v.flags & FL_PARTIALGROUND )
		{
			VectorAdd (ent->v.origin, move, ent->v.origin);
			if (relink)
				SV_LinkEdict (ent, true);
			ent->v.flags = (int)ent->v.flags & ~FL_ONGROUND;
//	Con_Printf ("fall down\n"); 
			return true;
		}
	
		return false;		// walked off an edge
	}

// check point traces down for dangling corners
	VectorCopy (trace.endpos, ent->v.origin);
	
	if (!SV_CheckBottom (ent))
	{
		if ( (int)ent->v.flags & FL_PARTIALGROUND )
		{	// entity had floor mostly pulled out from underneath it
			// and is trying to correct
			if (relink)
				SV_LinkEdict (ent, true);
			return true;
		}
		VectorCopy (oldorg, ent->v.origin);
		return false;
	}

	if ( (int)ent->v.flags & FL_PARTIALGROUND )
	{
//		Con_Printf ("back on ground\n"); 
		ent->v.flags = (int)ent->v.flags & ~FL_PARTIALGROUND;
	}
	ent->v.groundentity = EDICT_TO_PROG(trace.ent);

// the move is ok
	if (relink)
		SV_LinkEdict (ent, true);
	return true;
}


//============================================================================

/*
======================
SV_SinCos

sinf and cosf of angle, remembered by the exact bits of angle
======================
*/
#define	SINCOS_HASH_BITS	6

typedef struct
{
	unsigned	bits;
	qboolean	valid;
	float		sinval, cosval;
} sincos_t;

static sincos_t	sincos_cache[1<<SINCOS_HASH_BITS];

void SV_SinCos (float angle, float *sinval, float *cosval)
{
	union
	{
		float		f;
		unsigned	u;
	} key;
	sincos_t	*c;

	key.f = angle;
	c = &sincos_cache[(key.u * 0x9e3779b1u) >> (32 - SINCOS_HASH_BITS)];
	if (!c->valid || c->bits != key.u)
	{
		c->sinval = sinf(angle);
		c->cosval = cosf(angle);
		c->bits = key.u;
		c->valid = true;
	}
	*sinval = c->sinval;
	*cosval = c->cosval;
}

/*
======================
SV_StepDirection

Turns to the movement direction, and walks the current distance if
facing it.

======================
*/
void PF_changeyaw (void);
qboolean SV_StepDirection (edict_t *ent, float yaw, float dist)
{
	vec3_t		move, oldorigin;
	float		delta;
	float		sinval, cosval;
	
	ent->v.ideal_yaw = yaw;
	PF_changeyaw();
	
	yaw = yaw * (float)(M_PI / 180.0);
	SV_SinCos (yaw, &sinval, &cosval);
	move[0] = cosval*dist;
	move[1] = sinval*dist;
	move[2] = 0;

	VectorCopy (ent->v.origin, oldorigin);
	if (SV_movestep (ent, move, false))
	{
		delta = ent->v.angles[YAW] - ent->v.ideal_yaw;
		if (delta > 45 && delta < 315)
		{		// not turned far enough, so don't take the step
			VectorCopy (oldorigin, ent->v.origin);
		}
		SV_LinkEdict (ent, true);
		return true;
	}
	SV_LinkEdict (ent, true);
		
	return false;
}

/*
======================
SV_FixCheckBottom

======================
*/
void SV_FixCheckBottom (edict_t *ent)
{
//	Con_Printf ("SV_FixCheckBottom\n");
	
	ent->v.flags = (int)ent->v.flags | FL_PARTIALGROUND;
}



/*
================
SV_NewChaseDir

================
*/
#define	DI_NODIR	-1
void SV_NewChaseDir (edict_t *actor, edict_t *enemy, float dist)
{
	float		deltax,deltay;
	float			d[3];
	float		tdir, olddir, turnaround;

	olddir = anglemod( (int)(actor->v.ideal_yaw/45)*45 );
	turnaround = anglemod(olddir - 180);

	deltax = enemy->v.origin[0] - actor->v.origin[0];
	deltay = enemy->v.origin[1] - actor->v.origin[1];
	if (deltax>10)
		d[1]= 0;
	else if (deltax<-10)
		d[1]= 180;
	else
		d[1]= DI_NODIR;
	if (deltay<-10)
		d[2]= 270;
	else if (deltay>10)
		d[2]= 90;
	else
		d[2]= DI_NODIR;

// try direct route
	if (d[1] != DI_NODIR && d[2] != DI_NODIR)
	{
		if (d[1] == 0)
			tdir = d[2] == 90 ? 45 : 315;
		else
			tdir = d[2] == 90 ? 135 : 215;
			
		if (tdir != turnaround && SV_StepDirection(actor, tdir, dist))
			return;
	}

// try other directions
	if ( ((rand()&3) & 1) ||  fabsf(deltay)>fabsf(deltax))
	{
		tdir=d[1];
		d[1]=d[2];
		d[2]=tdir;
	}

	if (d[1]!=DI_NODIR && d[1]!=turnaround 
	&& SV_StepDirection(actor, d[1], dist))
			return;

	if (d[2]!=DI_NODIR && d[2]!=turnaround
	&& SV_StepDirection(actor, d[2], dist))
			return;

/* there is no direct path to the player, so pick another direction */

	if (olddir!=DI_NODIR && SV_StepDirection(actor, olddir, dist))
			return;

	if (rand()&1) 	/*randomly determine direction of search*/
	{
		for (tdir=0 ; tdir<=315 ; tdir += 45)
			if (tdir!=turnaround && SV_StepDirection(actor, tdir, dist) )
					return;
	}
	else
	{
		for (tdir=315 ; tdir >=0 ; tdir -= 45)
			if (tdir!=turnaround && SV_StepDirection(actor, tdir, dist) )
					return;
	}

	if (turnaround != DI_NODIR && SV_StepDirection(actor, turnaround, dist) )
			return;

	actor->v.ideal_yaw = olddir;		// can't move

// if a bridge was pulled out from underneath a monster, it may not have
// a valid standing position at all

	if (!SV_CheckBottom (actor))
		SV_FixCheckBottom (actor);

}

/*
======================
SV_CloseEnough

======================
*/
qboolean SV_CloseEnough (edict_t *ent, edict_t *goal, float dist)
{
	int		i;
	
	for (i=0 ; i<3 ; i++)
	{
		if (goal->v.absmin[i] > ent->v.absmax[i] + dist)
			return false;
		if (goal->v.absmax[i] < ent->v.absmin[i] - dist)
			return false;
	}
	return true;
}

/*
======================
SV_MoveToGoal

======================
*/
void SV_MoveToGoal (void)
{
	edict_t		*ent, *goal;
	float		dist;
#ifdef QUAKE2
	edict_t		*enemy;
#endif

	ent = PROG_TO_EDICT(pr_global_struct->self);
	goal = PROG_TO_EDICT(ent->v.goalentity);
	dist = G_FLOAT(OFS_PARM0);

	if ( !( (int)ent->v.flags & (FL_ONGROUND|FL_FLY|FL_SWIM) ) )
	{
		G_FLOAT(OFS_RETURN) = 0;
		return;
	}

// if the next step hits the enemy, return immediately
#ifdef QUAKE2
	enemy = PROG_TO_EDICT(ent->v.enemy);
	if (enemy != sv.edicts &&  SV_CloseEnough (ent, enemy, dist) )
#else
	if ( PROG_TO_EDICT(ent->v.enemy) != sv.edicts &&  SV_CloseEnough (ent, goal, dist) )
#endif
		return;

// bump around...
	if ( (rand()&3)==1 ||
	!SV_StepDirection (ent, ent->v.ideal_yaw, dist))
	{
		SV_NewChaseDir (ent, goal, dist);
	}
}
