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

#include "quakedef.h"


/*

*/

typedef struct
{
	int				s;
	dfunction_t		*f;
} prstack_t;

#define	MAX_STACK_DEPTH		32
prstack_t	pr_stack[MAX_STACK_DEPTH];
int			pr_depth;

#define	LOCALSTACK_SIZE		2048
int			localstack[LOCALSTACK_SIZE];
int			localstack_used;


qboolean	pr_trace;
dfunction_t	*pr_xfunction;
int			pr_xstatement;


int		pr_argc;

char *pr_opnames[] =
{
"DONE",

"MUL_F",
"MUL_V", 
"MUL_FV",
"MUL_VF",
 
"DIV",

"ADD_F",
"ADD_V", 
  
"SUB_F",
"SUB_V",

"EQ_F",
"EQ_V",
"EQ_S", 
"EQ_E",
"EQ_FNC",
 
"NE_F",
"NE_V", 
"NE_S",
"NE_E", 
"NE_FNC",
 
"LE",
"GE",
"LT",
"GT", 

"INDIRECT",
"INDIRECT",
"INDIRECT", 
"INDIRECT", 
"INDIRECT",
"INDIRECT", 

"ADDRESS", 

"STORE_F",
"STORE_V",
"STORE_S",
"STORE_ENT",
"STORE_FLD",
"STORE_FNC",

"STOREP_F",
"STOREP_V",
"STOREP_S",
"STOREP_ENT",
"STOREP_FLD",
"STOREP_FNC",

"RETURN",
  
"NOT_F",
"NOT_V",
"NOT_S", 
"NOT_ENT", 
"NOT_FNC", 
  
"IF",
"IFNOT",
  
"CALL0",
"CALL1",
"CALL2",
"CALL3",
"CALL4",
"CALL5",
"CALL6",
"CALL7",
"CALL8",
  
"STATE",
  
"GOTO", 
  
"AND",
"OR", 

"BITAND",
"BITOR"
};

char *PR_GlobalString (int ofs);
char *PR_GlobalStringNoContents (int ofs);


//=============================================================================

/*
=================
PR_PrintStatement
=================
*/
void PR_PrintStatement (dstatement_t *s)
{
	int		i;
	
	if ( (unsigned)s->op < sizeof(pr_opnames)/sizeof(pr_opnames[0]))
	{
		Con_Printf ("%s ",  pr_opnames[s->op]);
		i = strlen(pr_opnames[s->op]);
		for ( ; i<10 ; i++)
			Con_Printf (" ");
	}
		
	if (s->op == OP_IF || s->op == OP_IFNOT)
		Con_Printf ("%sbranch %i",PR_GlobalString(s->a),s->b);
	else if (s->op == OP_GOTO)
	{
		Con_Printf ("branch %i",s->a);
	}
	else if ( (unsigned)(s->op - OP_STORE_F) < 6)
	{
		Con_Printf ("%s",PR_GlobalString(s->a));
		Con_Printf ("%s", PR_GlobalStringNoContents(s->b));
	}
	else
	{
		if (s->a)
			Con_Printf ("%s",PR_GlobalString(s->a));
		if (s->b)
			Con_Printf ("%s",PR_GlobalString(s->b));
		if (s->c)
			Con_Printf ("%s", PR_GlobalStringNoContents(s->c));
	}
	Con_Printf ("\n");
}

/*
============
PR_StackTrace
============
*/
void PR_StackTrace (void)
{
	dfunction_t	*f;
	int			i;
	
	if (pr_depth == 0)
	{
		Con_Printf ("<NO STACK>\n");
		return;
	}
	
	pr_stack[pr_depth].f = pr_xfunction;
	for (i=pr_depth ; i>=0 ; i--)
	{
		f = pr_stack[i].f;
		
		if (!f)
		{
			Con_Printf ("<NO FUNCTION>\n");
		}
		else
			Con_Printf ("%12s : %s\n", pr_strings + f->s_file, pr_strings + f->s_name);		
	}
}


/*
============
PR_Profile_f

============
*/
void PR_Profile_f (void)
{
	dfunction_t	*f, *best;
	int			max;
	int			num;
	int			i;
	
	num = 0;	
	do
	{
		max = 0;
		best = NULL;
		for (i=0 ; i<progs->numfunctions ; i++)
		{
			f = &pr_functions[i];
			if (f->profile > max)
			{
				max = f->profile;
				best = f;
			}
		}
		if (best)
		{
			if (num < 10)
				Con_Printf ("%7i %s\n", best->profile, pr_strings+best->s_name);
			num++;
			best->profile = 0;
		}
	} while (best);
}


/*
============
PR_RunError

Aborts the currently executing function
============
*/
void PR_RunError (char *error, ...)
{
	va_list		argptr;
	char		string[1024];

	va_start (argptr,error);
	vsprintf (string,error,argptr);
	va_end (argptr);

	PR_PrintStatement (pr_statements + pr_xstatement);
	PR_StackTrace ();
	Con_Printf ("%s\n", string);
	
	pr_depth = 0;		// dump the stack so host_error can shutdown functions

	Host_Error ("Program error");
}

/*
============================================================================
PR_ExecuteProgram

The interpretation main loop
============================================================================
*/

/*
====================
PR_EnterFunction

Returns the new program statement counter
====================
*/
int PR_EnterFunction (dfunction_t *f)
{
	int		i, j, c, o;

	pr_stack[pr_depth].s = pr_xstatement;
	pr_stack[pr_depth].f = pr_xfunction;	
	pr_depth++;
	if (pr_depth >= MAX_STACK_DEPTH)
		PR_RunError ("stack overflow");

// save off any locals that the new function steps on
	c = f->locals;
	if (localstack_used + c > LOCALSTACK_SIZE)
		PR_RunError ("PR_ExecuteProgram: locals stack overflow\n");

	for (i=0 ; i < c ; i++)
		localstack[localstack_used+i] = ((int *)pr_globals)[f->parm_start + i];
	localstack_used += c;

// copy parameters
	o = f->parm_start;
	for (i=0 ; i<f->numparms ; i++)
	{
		for (j=0 ; j<f->parm_size[i] ; j++)
		{
			((int *)pr_globals)[o] = ((int *)pr_globals)[OFS_PARM0+i*3+j];
			o++;
		}
	}

	pr_xfunction = f;
	return f->first_statement - 1;	// offset the s++
}

/*
====================
PR_LeaveFunction
====================
*/
int PR_LeaveFunction (void)
{
	int		i, c;

	if (pr_depth <= 0)
		Sys_Error ("prog stack underflow");

// restore locals from the stack
	c = pr_xfunction->locals;
	localstack_used -= c;
	if (localstack_used < 0)
		PR_RunError ("PR_ExecuteProgram: locals stack underflow\n");

	for (i=0 ; i < c ; i++)
		((int *)pr_globals)[pr_xfunction->parm_start + i] = localstack[localstack_used+i];

// up stack
	pr_depth--;
	pr_xfunction = pr_stack[pr_depth].f;
	return pr_stack[pr_depth].s;
}


/*
====================
PR_ExecuteProgram

pr_xstatement and pr_xfunction->profile are only brought up to date
before anything that can read them: calls, returns, and error paths.
The statements executed since the last update are seg - runaway.
====================
*/
#define	PR_SYNC()	(pr_xstatement = st - statements, pr_xfunction->profile += seg - runaway, seg = runaway)

// threaded dispatch: every handler ends in its own indirect jump, so the
// predictor keeps one target per opcode instead of one for the whole switch,
// and only the operands a handler uses are ever formed
#define	OPA		((eval_t *)&globals[st->a])
#define	OPB		((eval_t *)&globals[st->b])
#define	OPC		((eval_t *)&globals[st->c])

#define	NEXT()								\
	{										\
		if (!--runaway)						\
			goto runaway_error;				\
		st++;								\
		if (trace)							\
			goto traced;					\
		if (st->op > OP_BITOR)				\
			goto bad_opcode;				\
		goto *jt[st->op];					\
	}

void PR_ExecuteProgram (func_t fnum)
{
	static void	*const dispatch[OP_BITOR+1] =
	{
		&&L_DONE,		&&L_MUL_F,		&&L_MUL_V,		&&L_MUL_FV,
		&&L_MUL_VF,		&&L_DIV_F,		&&L_ADD_F,		&&L_ADD_V,
		&&L_SUB_F,		&&L_SUB_V,
		&&L_EQ_F,		&&L_EQ_V,		&&L_EQ_S,		&&L_EQ_E,
		&&L_EQ_FNC,
		&&L_NE_F,		&&L_NE_V,		&&L_NE_S,		&&L_NE_E,
		&&L_NE_FNC,
		&&L_LE,			&&L_GE,			&&L_LT,			&&L_GT,
		&&L_LOAD_F,		&&L_LOAD_V,		&&L_LOAD_S,		&&L_LOAD_ENT,
		&&L_LOAD_FLD,	&&L_LOAD_FNC,
		&&L_ADDRESS,
		&&L_STORE_F,	&&L_STORE_V,	&&L_STORE_S,	&&L_STORE_ENT,
		&&L_STORE_FLD,	&&L_STORE_FNC,
		&&L_STOREP_F,	&&L_STOREP_V,	&&L_STOREP_S,	&&L_STOREP_ENT,
		&&L_STOREP_FLD,	&&L_STOREP_FNC,
		&&L_RETURN,
		&&L_NOT_F,		&&L_NOT_V,		&&L_NOT_S,		&&L_NOT_ENT,
		&&L_NOT_FNC,
		&&L_IF,			&&L_IFNOT,
		&&L_CALL0,		&&L_CALL1,		&&L_CALL2,		&&L_CALL3,
		&&L_CALL4,		&&L_CALL5,		&&L_CALL6,		&&L_CALL7,
		&&L_CALL8,
		&&L_STATE,		&&L_GOTO,
		&&L_AND,		&&L_OR,
		&&L_BITAND,		&&L_BITOR
	};
	void	*const *jt;
	eval_t	*a, *b, *c;
	dstatement_t	*st;
	dfunction_t	*f, *newf;
	int		runaway;
	int		i;
	edict_t	*ed;
	int		exitdepth;
	eval_t	*ptr;
	int		seg;
	qboolean	trace;
	dstatement_t	*statements;
	float	*globals;

	if (!fnum || fnum >= progs->numfunctions)
	{
		if (pr_global_struct->self)
			ED_Print (PROG_TO_EDICT(pr_global_struct->self));
		Host_Error ("PR_ExecuteProgram: NULL function");
	}

	f = &pr_functions[fnum];

	runaway = 0x1000000; /* QuakeSpasm: was 100000 */
	pr_trace = false;
	trace = false;

// make a stack frame
	exitdepth = pr_depth;

// progs can't be reloaded while they are running
	statements = pr_statements;
	globals = pr_globals;
	jt = dispatch;

	st = statements + PR_EnterFunction (f);	// PR_EnterFunction offsets the st++
	seg = runaway;

	NEXT ();

runaway_error:
	// st is the statement about to run; at a call its own PR_SYNC site is
	// the better report, so leave pr_xstatement alone there
	if ((unsigned)(st->op - OP_CALL0) > OP_CALL8 - OP_CALL0)
		pr_xstatement = st - statements;
	pr_xfunction->profile += seg - 1;
	PR_RunError ("runaway loop error");

traced:
	PR_SYNC ();
	PR_PrintStatement (st);
	if (st->op > OP_BITOR)
		goto bad_opcode;
	goto *jt[st->op];

bad_opcode:
	PR_SYNC ();
	PR_RunError ("Bad opcode %i", st->op);

L_ADD_F:
	OPC->_float = OPA->_float + OPB->_float;
	NEXT ();
L_ADD_V:
	a = OPA;	b = OPB;	c = OPC;
	c->vector[0] = a->vector[0] + b->vector[0];
	c->vector[1] = a->vector[1] + b->vector[1];
	c->vector[2] = a->vector[2] + b->vector[2];
	NEXT ();

L_SUB_F:
	OPC->_float = OPA->_float - OPB->_float;
	NEXT ();
L_SUB_V:
	a = OPA;	b = OPB;	c = OPC;
	c->vector[0] = a->vector[0] - b->vector[0];
	c->vector[1] = a->vector[1] - b->vector[1];
	c->vector[2] = a->vector[2] - b->vector[2];
	NEXT ();

L_MUL_F:
	OPC->_float = OPA->_float * OPB->_float;
	NEXT ();
L_MUL_V:
	a = OPA;	b = OPB;
	OPC->_float = a->vector[0]*b->vector[0]
			+ a->vector[1]*b->vector[1]
			+ a->vector[2]*b->vector[2];
	NEXT ();
L_MUL_FV:
	a = OPA;	b = OPB;	c = OPC;
	c->vector[0] = a->_float * b->vector[0];
	c->vector[1] = a->_float * b->vector[1];
	c->vector[2] = a->_float * b->vector[2];
	NEXT ();
L_MUL_VF:
	a = OPA;	b = OPB;	c = OPC;
	c->vector[0] = b->_float * a->vector[0];
	c->vector[1] = b->_float * a->vector[1];
	c->vector[2] = b->_float * a->vector[2];
	NEXT ();

L_DIV_F:
	OPC->_float = OPA->_float / OPB->_float;
	NEXT ();

L_BITAND:
	OPC->_float = (int)OPA->_float & (int)OPB->_float;
	NEXT ();

L_BITOR:
	OPC->_float = (int)OPA->_float | (int)OPB->_float;
	NEXT ();


L_GE:
	OPC->_float = OPA->_float >= OPB->_float;
	NEXT ();
L_LE:
	OPC->_float = OPA->_float <= OPB->_float;
	NEXT ();
L_GT:
	OPC->_float = OPA->_float > OPB->_float;
	NEXT ();
L_LT:
	OPC->_float = OPA->_float < OPB->_float;
	NEXT ();
L_AND:
	OPC->_float = OPA->_float && OPB->_float;
	NEXT ();
L_OR:
	OPC->_float = OPA->_float || OPB->_float;
	NEXT ();

L_NOT_F:
	OPC->_float = !OPA->_float;
	NEXT ();
L_NOT_V:
	a = OPA;
	OPC->_float = !a->vector[0] && !a->vector[1] && !a->vector[2];
	NEXT ();
L_NOT_S:
	a = OPA;
	PR_SYNC ();		// PR_GetString can Host_Error
	OPC->_float = !a->string || !*PR_GetString(a->string);
	NEXT ();
L_NOT_FNC:
	OPC->_float = !OPA->function;
	NEXT ();
L_NOT_ENT:
	OPC->_float = (PROG_TO_EDICT(OPA->edict) == sv.edicts);
	NEXT ();

L_EQ_F:
	OPC->_float = OPA->_float == OPB->_float;
	NEXT ();
L_EQ_V:
	a = OPA;	b = OPB;
	OPC->_float = (a->vector[0] == b->vector[0]) &&
				(a->vector[1] == b->vector[1]) &&
				(a->vector[2] == b->vector[2]);
	NEXT ();
L_EQ_S:
	a = OPA;	b = OPB;
	PR_SYNC ();
	OPC->_float = !strcmp(PR_GetString(a->string),PR_GetString(b->string));
	NEXT ();
L_EQ_E:
	OPC->_float = OPA->_int == OPB->_int;
	NEXT ();
L_EQ_FNC:
	OPC->_float = OPA->function == OPB->function;
	NEXT ();


L_NE_F:
	OPC->_float = OPA->_float != OPB->_float;
	NEXT ();
L_NE_V:
	a = OPA;	b = OPB;
	OPC->_float = (a->vector[0] != b->vector[0]) ||
				(a->vector[1] != b->vector[1]) ||
				(a->vector[2] != b->vector[2]);
	NEXT ();
L_NE_S:
	a = OPA;	b = OPB;
	PR_SYNC ();
	OPC->_float = strcmp(PR_GetString(a->string),PR_GetString(b->string));
	NEXT ();
L_NE_E:
	OPC->_float = OPA->_int != OPB->_int;
	NEXT ();
L_NE_FNC:
	OPC->_float = OPA->function != OPB->function;
	NEXT ();

//==================
L_STORE_F:
L_STORE_ENT:
L_STORE_FLD:		// integers
L_STORE_S:
L_STORE_FNC:		// pointers
	OPB->_int = OPA->_int;
	NEXT ();
L_STORE_V:
	a = OPA;	b = OPB;
	b->vector[0] = a->vector[0];
	b->vector[1] = a->vector[1];
	b->vector[2] = a->vector[2];
	NEXT ();

L_STOREP_F:
L_STOREP_ENT:
L_STOREP_FLD:		// integers
L_STOREP_S:
L_STOREP_FNC:		// pointers
	ptr = (eval_t *)((byte *)sv.edicts + OPB->_int);
	ptr->_int = OPA->_int;
	NEXT ();
L_STOREP_V:
	a = OPA;
	ptr = (eval_t *)((byte *)sv.edicts + OPB->_int);
	ptr->vector[0] = a->vector[0];
	ptr->vector[1] = a->vector[1];
	ptr->vector[2] = a->vector[2];
	NEXT ();

L_ADDRESS:
	ed = PROG_TO_EDICT(OPA->edict);
#ifdef PARANOID
	NUM_FOR_EDICT(ed);		// make sure it's in range
#endif
	if (ed == (edict_t *)sv.edicts && sv.state == ss_active)
	{
		PR_SYNC ();
		PR_RunError ("assignment to world entity");
	}
	OPC->_int = (byte *)((int *)&ed->v + OPB->_int) - (byte *)sv.edicts;
	NEXT ();

L_LOAD_F:
L_LOAD_FLD:
L_LOAD_ENT:
L_LOAD_S:
L_LOAD_FNC:
	ed = PROG_TO_EDICT(OPA->edict);
#ifdef PARANOID
	NUM_FOR_EDICT(ed);		// make sure it's in range
#endif
	a = (eval_t *)((int *)&ed->v + OPB->_int);
	OPC->_int = a->_int;
	NEXT ();

L_LOAD_V:
	ed = PROG_TO_EDICT(OPA->edict);
#ifdef PARANOID
	NUM_FOR_EDICT(ed);		// make sure it's in range
#endif
	a = (eval_t *)((int *)&ed->v + OPB->_int);
	c = OPC;
	c->vector[0] = a->vector[0];
	c->vector[1] = a->vector[1];
	c->vector[2] = a->vector[2];
	NEXT ();

//==================

L_IFNOT:
	if (!OPA->_int)
		st += st->b - 1;	// offset the st++
	NEXT ();

L_IF:
	if (OPA->_int)
		st += st->b - 1;	// offset the st++
	NEXT ();

L_GOTO:
	st += st->a - 1;	// offset the st++
	NEXT ();

L_CALL0:
L_CALL1:
L_CALL2:
L_CALL3:
L_CALL4:
L_CALL5:
L_CALL6:
L_CALL7:
L_CALL8:
	a = OPA;
	PR_SYNC ();
	pr_argc = st->op - OP_CALL0;
	if (!a->function)
		PR_RunError ("NULL function");

	newf = &pr_functions[a->function];

	if (newf->first_statement < 0)
	{	// negative statements are built in functions
		i = -newf->first_statement;
		if (i >= pr_numbuiltins)
			PR_RunError ("Bad builtin call number");
		pr_builtins[i] ();
		trace = pr_trace;
		NEXT ();
	}

	st = statements + PR_EnterFunction (newf);
	NEXT ();

L_DONE:
L_RETURN:
	globals[OFS_RETURN] = globals[(unsigned short)st->a];
	globals[OFS_RETURN+1] = globals[(unsigned short)st->a+1];
	globals[OFS_RETURN+2] = globals[(unsigned short)st->a+2];

	PR_SYNC ();
	st = statements + PR_LeaveFunction ();
	if (pr_depth == exitdepth)
		return;		// all done
	NEXT ();

L_STATE:
	ed = PROG_TO_EDICT(pr_global_struct->self);
#ifdef FPS_20
	ed->v.nextthink = pr_global_struct->time + 0.05;
#else
	if (fabsf(pr_global_struct->time) >= 0.5f)
		ed->v.nextthink = pr_global_struct->time + 0.1f;	// same float as the double sum
	else
		ed->v.nextthink = pr_global_struct->time + 0.1;
#endif
	a = OPA;
	if (a->_float != ed->v.frame)
	{
		ed->v.frame = a->_float;
	}
	ed->v.think = OPB->function;
	NEXT ();
}
