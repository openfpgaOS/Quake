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
// host.c -- coordinates spawning and killing of local servers

#include "quakedef.h"
#include "r_local.h"

/* printf comes from stdio.h via quakedef.h; no custom declaration. */

/*

A server can allways be started, even if the system started out as a client
to a remote system.

A client can NOT be started if the system started as a dedicated server.

Memory is cleared / released when a server or client begins, not when they end.

*/

quakeparms_t host_parms;

qboolean	host_initialized;		// true if into command execution

float		host_frametime;
float		host_time;
float		realtime;				// without any filtering or bounding
float		oldrealtime;			// last frame run
int			host_framecount;

int			host_hunklevel;

int			minimum_memory;

client_t	*host_client;			// current client

jmp_buf 	host_abortserver;

byte		*host_basepal;
byte		*host_colormap;

cvar_t	host_framerate = {"host_framerate","0"};	// set for slow motion
cvar_t	host_speeds = {"host_speeds","0"};			// set for running times

cvar_t	sys_ticrate = {"sys_ticrate","0.05"};

cvar_t	fraglimit = {"fraglimit","0",false,true};
cvar_t	timelimit = {"timelimit","0",false,true};
cvar_t	teamplay = {"teamplay","0",false,true};

cvar_t	samelevel = {"samelevel","0"};
cvar_t	noexit = {"noexit","0",false,true};

/* The 2021 re-release QuakeC sets "campaign" on every level change to
 * tell the engine which episode set is running.  Vanilla has no such
 * cvar, so each map load printed "Cvar_Set: variable campaign not
 * found".  Registering it both silences that and lets the progs read
 * back what it wrote, which is what the mod actually expects. */
cvar_t	campaign = {"campaign","0"};

#ifdef QUAKE2
cvar_t	developer = {"developer","1"};	// should be 0 for release!
#else
cvar_t	developer = {"developer","0"};
#endif

cvar_t	skill = {"skill","1"};						// 0 - 3
cvar_t	deathmatch = {"deathmatch","0"};			// 0, 1, or 2
cvar_t	coop = {"coop","0"};			// 0 or 1

cvar_t	pausable = {"pausable","1"};

cvar_t	temp1 = {"temp1","0"};


/*
================
Host_EndGame
================
*/
void Host_EndGame (char *message, ...)
{
	va_list		argptr;
	char		string[1024];
	
	va_start (argptr,message);
	vsprintf (string,message,argptr);
	va_end (argptr);
	Con_DPrintf ("Host_EndGame: %s\n",string);

#ifdef QUAKE_OPENFPGA
	/* Switch to terminal so the message is visible */
	(*(volatile unsigned int *)0x4000000C) = 0;
	printf("Host_EndGame: %s\n", string);
#endif

	if (sv.active)
		Host_ShutdownServer (false);

	if (cls.state == ca_dedicated)
		Sys_Error ("Host_EndGame: %s\n",string);	// dedicated servers exit

	/* host_abortserver is initialized by setjmp() inside _Host_Frame().
	 * If EndGame triggers before the frame loop starts, longjmp target is
	 * undefined and can return to random code paths. */
	if (!host_initialized)
		Sys_Error ("Host_EndGame during init: %s\n", string);
	
	if (cls.demonum != -1)
		CL_NextDemo ();
	else
		CL_Disconnect ();

	longjmp (host_abortserver, 1);
}

/*
================
Host_Error

This shuts down both the client and server
================
*/
void Host_Error (char *error, ...)
{
	va_list		argptr;
	char		string[1024];
	static	qboolean inerror = false;
	
	if (inerror)
		Sys_Error ("Host_Error: recursively entered");
	inerror = true;
	
	SCR_EndLoadingPlaque ();		// reenable screen updates

	va_start (argptr,error);
	vsprintf (string,error,argptr);
	va_end (argptr);
	Sys_Printf ("Host_Error: %s\n",string);
	Con_Printf ("Host_Error: %s\n",string);

#ifdef QUAKE_OPENFPGA
	/* Switch to terminal so the message is visible */
	(*(volatile unsigned int *)0x4000000C) = 0;
	printf("Host_Error: %s\n", string);
#endif
	
	if (sv.active)
		Host_ShutdownServer (false);

	if (cls.state == ca_dedicated)
		Sys_Error ("Host_Error: %s\n",string);	// dedicated servers exit

	/* host_abortserver is initialized by setjmp() inside _Host_Frame().
	 * During init, route fatal errors to Sys_Error instead of longjmp. */
	if (!host_initialized)
		Sys_Error ("Host_Error during init: %s\n", string);

	CL_Disconnect ();
	cls.demonum = -1;

	inerror = false;

	longjmp (host_abortserver, 1);
}

/*
================
Host_FindMaxClients
================
*/
void	Host_FindMaxClients (void)
{
	int		i;

	svs.maxclients = 1;
		
	i = COM_CheckParm ("-dedicated");
	if (i)
	{
		cls.state = ca_dedicated;
		if (i != (com_argc - 1))
		{
			svs.maxclients = Q_atoi (com_argv[i+1]);
		}
		else
			svs.maxclients = 8;
	}
	else
		cls.state = ca_disconnected;

	i = COM_CheckParm ("-listen");
	if (i)
	{
		if (cls.state == ca_dedicated)
			Sys_Error ("Only one of -dedicated or -listen can be specified");
		if (i != (com_argc - 1))
			svs.maxclients = Q_atoi (com_argv[i+1]);
		else
			svs.maxclients = 8;
	}
	if (svs.maxclients < 1)
		svs.maxclients = 8;
	else if (svs.maxclients > MAX_SCOREBOARD)
		svs.maxclients = MAX_SCOREBOARD;

	svs.maxclientslimit = svs.maxclients;
	if (svs.maxclientslimit < 4)
		svs.maxclientslimit = 4;
	svs.clients = Hunk_AllocName (svs.maxclientslimit*sizeof(client_t), "clients");

	if (svs.maxclients > 1)
		Cvar_SetValue ("deathmatch", 1.0);
	else
		Cvar_SetValue ("deathmatch", 0.0);
}


/*
=======================
Host_InitLocal
======================
*/
void Host_InitLocal (void)
{
	Host_InitCommands ();
	
	Cvar_RegisterVariable (&host_framerate);
	Cvar_RegisterVariable (&host_speeds);

	/* Render trace defaults OFF. Enable from quake.ini ([quake]
	 * HOST_SPEEDS=1) or the console (host_speeds 1) — the handheld has no
	 * easy console input. When on, the once/second averaged
	 * [perf]/[gfx]/[wrld]/[edge]/[surf] block goes to the system console
	 * (UART) only, so it never disturbs the screen. */
	{
		extern int Sys_IniGetInt(const char *key, int def);
		Cvar_SetValue ("host_speeds", Sys_IniGetInt("HOST_SPEEDS", 0));
		if (host_speeds.value)
			Con_Printf ("[quake] host_speeds=%d (render trace -> UART; "
			            "set [quake] HOST_SPEEDS=0 in quake.ini to disable)\n",
			            (int)host_speeds.value);
	}

	Cvar_RegisterVariable (&sys_ticrate);

	Cvar_RegisterVariable (&fraglimit);
	Cvar_RegisterVariable (&timelimit);
	Cvar_RegisterVariable (&teamplay);
	Cvar_RegisterVariable (&samelevel);
	Cvar_RegisterVariable (&noexit);
	Cvar_RegisterVariable (&campaign);
	Cvar_RegisterVariable (&skill);
	Cvar_RegisterVariable (&developer);
	Cvar_RegisterVariable (&deathmatch);
	Cvar_RegisterVariable (&coop);

	Cvar_RegisterVariable (&pausable);

	Cvar_RegisterVariable (&temp1);

	Host_FindMaxClients ();
	
	host_time = 1.0f;		// so a think at time 0 won't get called
}


/*
===============
Host_WriteConfiguration

Writes key bindings and archived cvars to the current instance config.
===============
*/
void Sys_QuakeConfigPath(char *out, int out_size);

void Host_WriteConfiguration (void)
{
	FILE	*f;
	char	config_path[MAX_OSPATH];

// dedicated servers initialize the host but don't parse and set the
// instance config cvars
	if (host_initialized & !isDedicated)
	{
		Sys_QuakeConfigPath(config_path, sizeof(config_path));
		CDAudio_DrainAsync ();	/* free the slot bridge before a blocking open */
		f = fopen (config_path, "w");
		if (!f)
		{
			Con_Printf ("Couldn't write %s.\n", config_path);
			return;
		}
		
		fprintf (f, "// Quake 3.0 config\n");
		Key_WriteBindings (f);
		Cvar_WriteVariables (f);

		fclose (f);
		CDAudio_NotifySlotWrite ();	/* see post-save DMA wedge note */
	}
}


/*
=================
SV_ClientPrintf

Sends text across to be displayed 
FIXME: make this just a stuffed echo?
=================
*/
void SV_ClientPrintf (char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];
	
	va_start (argptr,fmt);
	vsprintf (string, fmt,argptr);
	va_end (argptr);
	
	MSG_WriteByte (&host_client->message, svc_print);
	MSG_WriteString (&host_client->message, string);
}

/*
=================
SV_BroadcastPrintf

Sends text to all active clients
=================
*/
void SV_BroadcastPrintf (char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];
	int			i;
	
	va_start (argptr,fmt);
	vsprintf (string, fmt,argptr);
	va_end (argptr);
	
	for (i=0 ; i<svs.maxclients ; i++)
		if (svs.clients[i].active && svs.clients[i].spawned)
		{
			MSG_WriteByte (&svs.clients[i].message, svc_print);
			MSG_WriteString (&svs.clients[i].message, string);
		}
}

/*
=================
Host_ClientCommands

Send text over to the client to be executed
=================
*/
void Host_ClientCommands (char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];
	
	va_start (argptr,fmt);
	vsprintf (string, fmt,argptr);
	va_end (argptr);
	
	MSG_WriteByte (&host_client->message, svc_stufftext);
	MSG_WriteString (&host_client->message, string);
}

/*
=====================
SV_DropClient

Called when the player is getting totally kicked off the host
if (crash = true), don't bother sending signofs
=====================
*/
void SV_DropClient (qboolean crash)
{
	int		saveSelf;
	int		i;
	client_t *client;

	if (!crash)
	{
		// send any final messages (don't check for errors)
		if (NET_CanSendMessage (host_client->netconnection))
		{
			MSG_WriteByte (&host_client->message, svc_disconnect);
			NET_SendMessage (host_client->netconnection, &host_client->message);
		}
	
		if (host_client->edict && host_client->spawned)
		{
		// call the prog function for removing a client
		// this will set the body to a dead frame, among other things
			saveSelf = pr_global_struct->self;
			pr_global_struct->self = EDICT_TO_PROG(host_client->edict);
			PR_ExecuteProgram (pr_global_struct->ClientDisconnect);
			pr_global_struct->self = saveSelf;
		}

		Sys_Printf ("Client %s removed\n",host_client->name);
	}

// break the net connection
	NET_Close (host_client->netconnection);
	host_client->netconnection = NULL;

// free the client (the body stays around)
	host_client->active = false;
	host_client->name[0] = 0;
	host_client->old_frags = -999999;
	net_activeconnections--;

// send notification to all clients
	for (i=0, client = svs.clients ; i<svs.maxclients ; i++, client++)
	{
		if (!client->active)
			continue;
		MSG_WriteByte (&client->message, svc_updatename);
		MSG_WriteByte (&client->message, host_client - svs.clients);
		MSG_WriteString (&client->message, "");
		MSG_WriteByte (&client->message, svc_updatefrags);
		MSG_WriteByte (&client->message, host_client - svs.clients);
		MSG_WriteShort (&client->message, 0);
		MSG_WriteByte (&client->message, svc_updatecolors);
		MSG_WriteByte (&client->message, host_client - svs.clients);
		MSG_WriteByte (&client->message, 0);
	}
}

/*
==================
Host_ShutdownServer

This only happens at the end of a game, not between levels
==================
*/
void Host_ShutdownServer(qboolean crash)
{
	int		i;
	int		count;
	sizebuf_t	buf;
	char		message[4];
	float	start;

	if (!sv.active)
		return;

	sv.active = false;

// stop all client sounds immediately
	if (cls.state == ca_connected)
		CL_Disconnect ();

// flush any pending messages - like the score!!!
	start = Sys_FloatTime();
	do
	{
		count = 0;
		for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
		{
			if (host_client->active && host_client->message.cursize)
			{
				if (NET_CanSendMessage (host_client->netconnection))
				{
					NET_SendMessage(host_client->netconnection, &host_client->message);
					SZ_Clear (&host_client->message);
				}
				else
				{
					NET_GetMessage(host_client->netconnection);
					count++;
				}
			}
		}
		if ((Sys_FloatTime() - start) > 3.0)
			break;
	}
	while (count);

// make sure all the clients know we're disconnecting
	buf.data = message;
	buf.maxsize = 4;
	buf.cursize = 0;
	MSG_WriteByte(&buf, svc_disconnect);
	count = NET_SendToAll(&buf, 5);
	if (count)
		Con_Printf("Host_ShutdownServer: NET_SendToAll failed for %u clients\n", count);

	for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
		if (host_client->active)
			SV_DropClient(crash);

//
// clear structures
//
	memset (&sv, 0, sizeof(sv));
	memset (svs.clients, 0, svs.maxclientslimit*sizeof(client_t));
}


/*
================
Host_ClearMemory

This clears all the memory used by both the client and server, but does
not reinitialize anything.
================
*/
void Host_ClearMemory (void)
{
	Con_DPrintf ("Clearing memory\n");
	D_FlushCaches ();
	Mod_ClearAll ();
	if (host_hunklevel)
		Hunk_FreeToLowMark (host_hunklevel);

	cls.signon = 0;
	memset (&sv, 0, sizeof(sv));
	memset (&cl, 0, sizeof(cl));
}


//============================================================================


/*
===================
Host_FilterTime

Returns false if the time is too short to run a frame
===================
*/
qboolean Host_FilterTime (float time)
{
	realtime += time;

	if (!cls.timedemo && realtime - oldrealtime < 1.0/72.0)
		return false;		// framerate is too high

	host_frametime = realtime - oldrealtime;
	oldrealtime = realtime;

	if (host_framerate.value > 0)
		host_frametime = host_framerate.value;
	else
	{	// don't allow really long or short frames
		if (host_frametime > 0.1f)
			host_frametime = 0.1f;
		if (host_frametime < 0.001f)
			host_frametime = 0.001f;
	}
	
	return true;
}


/*
===================
Host_GetConsoleCommands

Add them exactly as if they had been typed at the console
===================
*/
void Host_GetConsoleCommands (void)
{
	char	*cmd;

	while (1)
	{
		cmd = Sys_ConsoleInput ();
		if (!cmd)
			break;
		Cbuf_AddText (cmd);
	}
}


/*
==================
Host_ServerFrame

==================
*/
#ifdef FPS_20

void _Host_ServerFrame (void)
{
// run the world state	
	pr_global_struct->frametime = host_frametime;

// read client messages
	SV_RunClients ();
	
// move things around and think
// always pause in single player if in console or menus
	if (!sv.paused && (svs.maxclients > 1 || key_dest == key_game) )
		SV_Physics ();
}

void Host_ServerFrame (void)
{
	float	save_host_frametime;
	float	temp_host_frametime;

// run the world state	
	pr_global_struct->frametime = host_frametime;

// set the time and clear the general datagram
	SV_ClearDatagram ();
	
// check for new clients
	SV_CheckForNewClients ();

	temp_host_frametime = save_host_frametime = host_frametime;
	while(temp_host_frametime > (1.0f/72.0f))
	{
		if (temp_host_frametime > 0.05f)
			host_frametime = 0.05f;
		else
			host_frametime = temp_host_frametime;
		temp_host_frametime -= host_frametime;
		_Host_ServerFrame ();
	}
	host_frametime = save_host_frametime;

// send all messages to the clients
	SV_SendClientMessages ();
}

#else

void Host_ServerFrame (void)
{
// run the world state	
	pr_global_struct->frametime = host_frametime;

// set the time and clear the general datagram
	SV_ClearDatagram ();
	
// check for new clients
	SV_CheckForNewClients ();

// read client messages
	SV_RunClients ();
	
// move things around and think
// always pause in single player if in console or menus
	if (!sv.paused && (svs.maxclients > 1 || key_dest == key_game) )
		SV_Physics ();

// send all messages to the clients
	SV_SendClientMessages ();
}

#endif


/*
==================
Host_Frame

Runs all active servers
==================
*/
void _Host_Frame (float time)
{
	static float		time1 = 0;
	static float		time2 = 0;
	static float		time3 = 0;
	/* host_speeds: per-second averaged render trace to UART (us/frame) */
	static double		perf_srv = 0, perf_gfx = 0, perf_tot = 0;
	static double		perf_gwait = 0, perf_gworld = 0, perf_g2d = 0, perf_gpresent = 0;
	static double		perf_setup = 0, perf_redge = 0, perf_ralias = 0, perf_rview = 0, perf_rpart = 0, perf_rwarp = 0;
	static double		perf_rworld = 0, perf_bent = 0, perf_scan = 0, perf_surf = 0;
	static double		perf_scache = 0, perf_sz = 0, perf_ssky = 0, perf_sturb = 0;
	static double		perf_smiss = 0;
	static int		perf_frames = 0;
	static float		perf_anchor = 0;
	extern float		scr_t_wait, scr_t_world, scr_t_2d, scr_t_present;
	extern float		r_t_setup, r_t_edges, r_t_alias, r_t_view, r_t_part, r_t_warp;
	extern float		r_t_rworld, r_t_bent, r_t_scan, r_t_surf;
	extern float		r_t_surfcache, r_t_surfz, r_t_surfsky, r_t_surfturb;
	extern int		r_c_surfmiss;

	if (setjmp (host_abortserver) )
		return;			// something bad happened, or the server disconnected

// keep the random time dependent
	rand ();

// decide the simulation time
	if (!Host_FilterTime (time))
		return;			// don't run too fast, or packets will flood out

// get new key events
	Sys_SendKeyEvents ();

// allow mice or other external controllers to add commands
	IN_Commands ();

// process console commands
	Cbuf_Execute ();

	NET_Poll();

// if running the server locally, make intentions now
	if (sv.active)
		CL_SendCmd ();
	
//-------------------
//
// server operations
//
//-------------------

// check for commands typed to the host
	Host_GetConsoleCommands ();
	
	if (sv.active)
		Host_ServerFrame ();

//-------------------
//
// client operations
//
//-------------------

// if running the server remotely, send intentions now after
// the incoming messages have been read
	if (!sv.active)
		CL_SendCmd ();

	host_time += host_frametime;

// fetch results from server
	if (cls.state == ca_connected)
	{
		CL_ReadFromServer ();
	}

// update video
	if (host_speeds.value)
		time1 = Sys_FloatTime ();
		
	SCR_UpdateScreen ();

	if (host_speeds.value)
		time2 = Sys_FloatTime ();
		
// update audio
	if (cls.signon == SIGNONS)
	{
		S_Update (r_origin, vpn, vright, vup);
		CL_DecayLights ();
	}
	else
		S_Update (vec3_origin, vec3_origin, vec3_origin, vec3_origin);
	
	CDAudio_Update();

	if (host_speeds.value)
	{
		float	fsrv, fgfx, fnow, ftot;

		fsrv = time1 - time3;		/* time3 = previous frame end */
		fnow = Sys_FloatTime ();
		ftot = fnow - time3;		/* whole frame */
		time3 = fnow;
		fgfx = time2 - time1;		/* SCR_UpdateScreen (render) */

		{
			/* Accumulate; emit once per ~second so the UART stays readable. */
			perf_srv += fsrv * 1.0e6; perf_gfx += fgfx * 1.0e6;
			perf_tot += ftot * 1.0e6;
			perf_gwait += scr_t_wait * 1.0e6; perf_gworld += scr_t_world * 1.0e6;
			perf_g2d += scr_t_2d * 1.0e6; perf_gpresent += scr_t_present * 1.0e6;
			perf_setup += r_t_setup * 1.0e6;
			perf_redge += r_t_edges * 1.0e6; perf_ralias += r_t_alias * 1.0e6;
			perf_rview += r_t_view * 1.0e6;  perf_rpart += r_t_part * 1.0e6;
			perf_rwarp += r_t_warp * 1.0e6;
			perf_rworld += r_t_rworld * 1.0e6; perf_bent += r_t_bent * 1.0e6;
			perf_scan += r_t_scan * 1.0e6;     perf_surf += r_t_surf * 1.0e6;
			perf_scache += r_t_surfcache * 1.0e6; perf_sz += r_t_surfz * 1.0e6;
			perf_ssky += r_t_surfsky * 1.0e6;  perf_sturb += r_t_surfturb * 1.0e6;
			perf_smiss += (double)r_c_surfmiss;
			perf_frames++;
			if (fnow - perf_anchor >= 1.0f && perf_frames > 0)
			{
				float	n = (float)perf_frames;
				float	win = fnow - perf_anchor;
				/* Sys_Printf, not Con_Printf: the trace is on by
				 * default and Con_Printf would park 5 notify lines on
				 * screen every second.  UART/system console only. */
				Sys_Printf ("[perf] %4.1f fps | srv %5.0f gfx %5.0f tot %5.0f us/fr (avg %d fr)\n",
					perf_frames / win, perf_srv / n, perf_gfx / n,
					perf_tot / n, perf_frames);
				Sys_Printf ("[gfx]  wait %5.0f world %5.0f 2d %5.0f present %5.0f us/fr\n",
					perf_gwait / n, perf_gworld / n, perf_g2d / n, perf_gpresent / n);
				Sys_Printf ("[wrld] setup %5.0f edges %5.0f alias %5.0f weap %5.0f part %5.0f warp %5.0f\n",
					perf_setup / n, perf_redge / n, perf_ralias / n,
					perf_rview / n, perf_rpart / n, perf_rwarp / n);
				Sys_Printf ("[edge] rworld %5.0f bent %5.0f scan %5.0f (surf %5.0f) us/fr\n",
					perf_rworld / n, perf_bent / n, perf_scan / n, perf_surf / n);
				/* emit = remainder of [surf]: D_CalcGradients + Q29 setup +
				 * param-span submit + per-surface loop overhead.  Only the
				 * rare paths are bracketed directly, so the common path adds
				 * no measurement cost. */
				{
					double surf_other = (perf_surf - perf_scache - perf_sz
						- perf_ssky - perf_sturb) / n;
					if (surf_other < 0) surf_other = 0;
					Sys_Printf ("[surf] cache %5.0f emit %5.0f z %5.0f sky %5.0f turb %5.0f (miss %3.0f/fr)\n",
						perf_scache / n, surf_other, perf_sz / n,
						perf_ssky / n, perf_sturb / n, perf_smiss / n);
				}
				perf_srv = perf_gfx = perf_tot = 0;
				perf_gwait = perf_gworld = perf_g2d = perf_gpresent = 0;
				perf_setup = perf_redge = perf_ralias = perf_rview = perf_rpart = perf_rwarp = 0;
				perf_rworld = perf_bent = perf_scan = perf_surf = 0;
				perf_scache = perf_sz = perf_ssky = perf_sturb = perf_smiss = 0;
				perf_frames = 0;
				perf_anchor = fnow;
			}
		}
	}
	
	host_framecount++;

}

void Host_Frame (float time)
{
	_Host_Frame (time);
}

//============================================================================


extern int vcrFile;
#define	VCR_SIGNATURE	0x56435231
// "VCR1"

void Host_InitVCR (quakeparms_t *parms)
{
	int		i, len, n;
	char	*p;
	
	if (COM_CheckParm("-playback"))
	{
		if (com_argc != 2)
			Sys_Error("No other parameters allowed with -playback\n");

		Sys_FileOpenRead("quake.vcr", &vcrFile);
		if (vcrFile == -1)
			Sys_Error("playback file not found\n");

		Sys_FileRead (vcrFile, &i, sizeof(int));
		if (i != VCR_SIGNATURE)
			Sys_Error("Invalid signature in vcr file\n");

		Sys_FileRead (vcrFile, &com_argc, sizeof(int));
		com_argv = malloc(com_argc * sizeof(char *));
		com_argv[0] = parms->argv[0];
		for (i = 0; i < com_argc; i++)
		{
			Sys_FileRead (vcrFile, &len, sizeof(int));
			p = malloc(len);
			Sys_FileRead (vcrFile, p, len);
			com_argv[i+1] = p;
		}
		com_argc++; /* add one for arg[0] */
		parms->argc = com_argc;
		parms->argv = com_argv;
	}

	if ( (n = COM_CheckParm("-record")) != 0)
	{
		vcrFile = Sys_FileOpenWrite("quake.vcr");

		i = VCR_SIGNATURE;
		Sys_FileWrite(vcrFile, &i, sizeof(int));
		i = com_argc - 1;
		Sys_FileWrite(vcrFile, &i, sizeof(int));
		for (i = 1; i < com_argc; i++)
		{
			if (i == n)
			{
				len = 10;
				Sys_FileWrite(vcrFile, &len, sizeof(int));
				Sys_FileWrite(vcrFile, "-playback", len);
				continue;
			}
			len = Q_strlen(com_argv[i]) + 1;
			Sys_FileWrite(vcrFile, &len, sizeof(int));
			Sys_FileWrite(vcrFile, com_argv[i], len);
		}
	}
	
}

/*
====================
Host_Init
====================
*/
void Host_Init (quakeparms_t *parms)
{

	if (standard_quake)
		minimum_memory = MINIMUM_MEMORY;
	else
		minimum_memory = MINIMUM_MEMORY_LEVELPAK;

	if (COM_CheckParm ("-minmemory"))
		parms->memsize = minimum_memory;

	host_parms = *parms;

	if (parms->memsize < minimum_memory)
		Sys_Error ("Only %4.1f megs of memory available, can't execute game", parms->memsize / (float)0x100000);

	com_argc = parms->argc;
	com_argv = parms->argv;

	Sys_Printf("Memory_Init\n");
	Memory_Init (parms->membase, parms->memsize);
	Sys_Printf("Cbuf_Init\n");
	Cbuf_Init ();
	Sys_Printf("Cbuf_Init OK\n");
	Sys_Printf("Cmd_Init\n");
	Cmd_Init ();
	Sys_Printf("V_Init\n");
	V_Init ();
	Sys_Printf("Chase_Init...");
	Chase_Init ();
	Sys_Printf("OK\n");
	Sys_Printf("Host_InitVCR...");
	Host_InitVCR (parms);
	Sys_Printf("OK\n");
	Sys_Printf("COM_Init...");
	COM_Init (parms->basedir);
	Sys_Printf("OK\n");
	{ extern void Sys_PrintDmaStats(void); Sys_PrintDmaStats(); }
	Sys_Printf("Host_InitLocal\n");
	Host_InitLocal ();
	Sys_Printf("W_LoadWadFile\n");
	W_LoadWadFile ("gfx.wad");
	Sys_Printf("Key_Init\n");
	Key_Init ();
	Sys_Printf("Con_Init\n");
	Con_Init ();
	Sys_Printf("M_Init\n");
	M_Init ();
	Sys_Printf("PR_Init\n");
	PR_Init ();
	Sys_Printf("Mod_Init\n");
	Mod_Init ();
	Sys_Printf("NET_Init\n");
	NET_Init ();
	Sys_Printf("SV_Init\n");
	SV_Init ();

	Con_Printf ("Exe: "__TIME__" "__DATE__"\n");
	Con_Printf ("%4.1f megabyte heap\n",parms->memsize/ (1024*1024.0));

	R_InitTextures ();		// needed even for dedicated servers

	if (cls.state != ca_dedicated)
	{
		Sys_Printf("palette.lmp\n");
		host_basepal = (byte *)COM_LoadHunkFile ("gfx/palette.lmp");
		if (!host_basepal)
			Sys_Error ("Couldn't load gfx/palette.lmp");
		Sys_Printf("colormap.lmp\n");
		host_colormap = (byte *)COM_LoadHunkFile ("gfx/colormap.lmp");
		if (!host_colormap)
			Sys_Error ("Couldn't load gfx/colormap.lmp");

#ifndef _WIN32 // on non win32, mouse comes before video for security reasons
		IN_Init ();
#endif
		Sys_Printf("VID_Init\n");
		VID_Init (host_basepal);

		Sys_Printf("Draw_Init\n");
		Draw_Init ();
		Sys_Printf("SCR_Init\n");
		SCR_Init ();
		Sys_Printf("R_Init\n");
		R_Init ();
#ifndef	_WIN32
	// on Win32, sound initialization has to come before video initialization, so we
	// can put up a popup if the sound hardware is in use
		S_Init ();
		Audio_TimerStart ();
#else

#ifdef	GLQUAKE
	// FIXME: doesn't use the new one-window approach yet
		S_Init ();
		Audio_TimerStart ();
#endif

#endif	// _WIN32
		CDAudio_Init ();
		Sbar_Init ();
		CL_Init ();
#ifdef _WIN32 // on non win32, mouse comes before video for security reasons
		IN_Init ();
#endif
	}

	Cbuf_InsertText ("exec quake.rc\n");

	// Queue Quake defaults AFTER quake.rc/default.cfg.
	// Cbuf_InsertText puts quake.rc at the front; Cbuf_AddText appends
	// these after it, so they override default.cfg's values.
	Cbuf_AddText ("gamma 0.7\n");

	Hunk_AllocName (0, "-HOST_HUNKLEVEL-");
	host_hunklevel = Hunk_LowMark ();

	host_initialized = true;

	{ extern void Sys_PrintDmaStats(void); Sys_PrintDmaStats(); }
	Sys_Printf ("========Quake Initialized=========\n");
}


/*
===============
Host_Shutdown

FIXME: this is a callback from Sys_Quit and Sys_Error.  It would be better
to run quit through here before the final handoff to the sys code.
===============
*/
void Host_Shutdown(void)
{
	static qboolean isdown = false;
	
	if (isdown)
	{
		printf ("recursive shutdown\n");
		return;
	}
	isdown = true;

// keep Con_Printf from trying to update the screen
	scr_disabled_for_loading = true;

	/* Stop the music stream (and drain its DMA) before the config write
	 * touches the slot bridge — same single-user-bridge rule as saves. */
	CDAudio_Shutdown ();

	Host_WriteConfiguration ();

	NET_Shutdown ();
	S_Shutdown();
	IN_Shutdown ();

	if (cls.state != ca_dedicated)
	{
		VID_Shutdown();
	}
}
