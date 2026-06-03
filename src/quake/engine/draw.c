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

// draw.c -- this is the only file outside the refresh that touches the
// vid buffer

#include "quakedef.h"
#include "of_emit.h"

typedef struct {
	vrect_t	rect;
	int		width;
	int		height;
	byte	*ptexbytes;
	int		rowbytes;
} rectdesc_t;

static rectdesc_t	r_rectdesc;

byte		*draw_chars;				// 8*8 graphic characters
qpic_t		*draw_disc;
qpic_t		*draw_backtile;

static byte draw_chars_keyed[128 * 128] __attribute__((aligned(64)));
static byte draw_fade_pattern[8] __attribute__((aligned(64))) = {
	TRANSPARENT_COLOR, 0, 0, 0,
	0, 0, TRANSPARENT_COLOR, 0
};

#define DRAW_TEXTLINE_MAX_CHARS 40
#define DRAW_TEXTLINE_BUFFERS 128
#define DRAW_TRANSLATE_MAX_WIDTH 1280
#define DRAW_TRANSLATE_ROW_BUFFERS 128

static byte draw_textline_buffers[DRAW_TEXTLINE_BUFFERS]
	[DRAW_TEXTLINE_MAX_CHARS * 8 * 8] __attribute__((aligned(64)));
static int draw_textline_buffer_next;
static byte draw_translate_row_buffers[DRAW_TRANSLATE_ROW_BUFFERS]
	[DRAW_TRANSLATE_MAX_WIDTH] __attribute__((aligned(64)));
static int draw_translate_row_buffer_next;

//=============================================================================
/* Support Routines */

typedef struct cachepic_s
{
	char		name[MAX_QPATH];
	cache_user_t	cache;
	unsigned	generation;
} cachepic_t;

#define	MAX_CACHED_PICS		128
cachepic_t	menu_cachepics[MAX_CACHED_PICS];
int			menu_numcachepics;

static uint32_t Draw_PicDataSize (const qpic_t *pic)
{
	if (!pic || pic->width <= 0 || pic->height <= 0)
		return 0;

	return (uint32_t)(pic->width * pic->height);
}

static void Draw_CleanPicForGpu (qpic_t *pic, qboolean force)
{
	uint32_t size;

	size = Draw_PicDataSize (pic);
	if (!size)
		return;

	if (force)
		of_emit_cache_clean_forget (pic->data, size);
	of_emit_cache_clean_once (pic->data, size);
}

static unsigned Draw_CachePicGeneration (char *path)
{
	cachepic_t	*pic;
	int			i;

	for (pic=menu_cachepics, i=0 ; i<menu_numcachepics ; pic++, i++)
		if (!strcmp (path, pic->name))
			return pic->generation;

	return 0;
}

qpic_t	*Draw_PicFromWad (char *name)
{
	qpic_t	*pic;

	pic = W_GetLumpName (name);
	Draw_CleanPicForGpu (pic, false);
	return pic;
}

/*
================
Draw_CachePic
================
*/
qpic_t	*Draw_CachePic (char *path)
{
	cachepic_t	*pic;
	int			i;
	qpic_t		*dat;
	
	for (pic=menu_cachepics, i=0 ; i<menu_numcachepics ; pic++, i++)
		if (!strcmp (path, pic->name))
			break;

	if (i == menu_numcachepics)
	{
		if (menu_numcachepics == MAX_CACHED_PICS)
			Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");
		menu_numcachepics++;
		strcpy (pic->name, path);
	}

	dat = Cache_Check (&pic->cache);

	if (dat)
		return dat;

//
// load the pic from disk
//
	COM_LoadCacheFile (path, &pic->cache);
	
	dat = (qpic_t *)pic->cache.data;
	if (!dat)
	{
		Sys_Error ("Draw_CachePic: failed to load %s", path);
	}

	SwapPic (dat);
	pic->generation++;
	if (!pic->generation)
		pic->generation = 1;
	Draw_CleanPicForGpu (dat, true);

	return dat;
}



/*
===============
Draw_Init
===============
*/
void Draw_Init (void)
{
	int		i;

	draw_chars = W_GetLumpName ("conchars");
	draw_disc = W_GetLumpName ("disc");
	draw_backtile = W_GetLumpName ("backtile");

	for (i=0 ; i<128*128 ; i++)
		draw_chars_keyed[i] = draw_chars[i] ? draw_chars[i] : TRANSPARENT_COLOR;

	r_rectdesc.width = draw_backtile->width;
	r_rectdesc.height = draw_backtile->height;
	r_rectdesc.ptexbytes = draw_backtile->data;
	r_rectdesc.rowbytes = draw_backtile->width;

	/* These lumps are loaded once and never modified — flush them out
	 * to SDRAM now so the GPU's AXI master sees fresh bytes when
	 * Draw_TileClear / Draw_Character / Draw_Pic later route their
	 * blits through of_emit_blit (which reads via cached AXI alias). */
	of_emit_cache_clean(draw_chars,
	                    (uint32_t)(128 * 128));   /* conchars: 16x16 grid of 8x8 */
	of_emit_cache_clean(draw_chars_keyed, (uint32_t)sizeof(draw_chars_keyed));
	of_emit_cache_clean(draw_fade_pattern, (uint32_t)sizeof(draw_fade_pattern));
	of_emit_cache_clean(draw_backtile->data,
	                    (uint32_t)(draw_backtile->width * draw_backtile->height));
}



/*
================
Draw_Character

Draws one 8*8 graphics character with 0 being transparent.
It can be clipped to the top of the screen to allow the console to be
smoothly scrolled off.
================
*/
void Draw_Character (int x, int y, int num)
{
	byte			*dest;
	byte			*source;
	unsigned short	*pusdest;
	int				drawline;
	int				row, col;

	num &= 255;

	if (y <= -8)
		return;			// totally off screen

#ifdef PARANOID
	if (y > vid.height - 8 || x < 0 || x > vid.width - 8)
		Sys_Error ("Con_DrawCharacter: (%i, %i)", x, y);
	if (num < 0 || num > 255)
		Sys_Error ("Con_DrawCharacter: char %i", num);
#endif

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	if (y < 0)
	{	// clipped
		drawline = 8 + y;
		source -= 128*y;
		y = 0;
	}
	else
		drawline = 8;


	if (r_pixbytes == 1)
	{
		byte *gpu_source = draw_chars_keyed + (source - draw_chars);

		for (int line=0 ; line<drawline ; line++)
		{
			of_emit_span_t sp = {
				.fb_addr   = (uint32_t)(uintptr_t)
					(vid.conbuffer + (y + line) * vid.conrowbytes + x),
				.tex_addr  = (uint32_t)(uintptr_t)(gpu_source + line * 128),
				.s         = 0,
				.t         = 0,
				.sstep     = 0x10000,
				.tstep     = 0,
				.count     = 8,
				.flags     = OF_EMIT_SKIP_ZERO,
				.fb_stride = 1,
				.tex_width = 128,
			};
			of_emit_span(&sp);
		}
	}
	else
	{
		pusdest = (unsigned short *)
				((byte *)vid.conbuffer + y*vid.conrowbytes + (x<<1));

		while (drawline--)
		{
			if (source[0])
				pusdest[0] = d_8to16table[source[0]];
			if (source[1])
				pusdest[1] = d_8to16table[source[1]];
			if (source[2])
				pusdest[2] = d_8to16table[source[2]];
			if (source[3])
				pusdest[3] = d_8to16table[source[3]];
			if (source[4])
				pusdest[4] = d_8to16table[source[4]];
			if (source[5])
				pusdest[5] = d_8to16table[source[5]];
			if (source[6])
				pusdest[6] = d_8to16table[source[6]];
			if (source[7])
				pusdest[7] = d_8to16table[source[7]];

			source += 128;
			pusdest += (vid.conrowbytes >> 1);
		}
	}
}

static byte *Draw_AllocTextLineBuffer (void)
{
	if (draw_textline_buffer_next >= DRAW_TEXTLINE_BUFFERS)
	{
		of_emit_finish();
		of_emit_texture_cache_flush();
		draw_textline_buffer_next = 0;
	}

	return draw_textline_buffers[draw_textline_buffer_next++];
}

static byte *Draw_AllocTranslateRowBuffer (void)
{
	if (draw_translate_row_buffer_next >= DRAW_TRANSLATE_ROW_BUFFERS)
	{
		of_emit_finish();
		of_emit_texture_cache_flush();
		draw_translate_row_buffer_next = 0;
	}

	return draw_translate_row_buffers[draw_translate_row_buffer_next++];
}

static void Draw_StringChunk8 (int x, int y, char *str, int len,
	int src_row, int drawline)
{
	int row, col, pitch;
	byte *scratch;

	if (len <= 0 || drawline <= 0)
		return;

	pitch = len * 8;
	scratch = Draw_AllocTextLineBuffer ();

	for (row=0 ; row<drawline ; row++)
	{
		byte *dst = scratch + row * pitch;
		for (col=0 ; col<len ; col++)
		{
			int ch = str[col] & 255;
			byte *src = draw_chars_keyed +
				((ch >> 4) << 10) + ((ch & 15) << 3) +
				(src_row + row) * 128;
			memcpy (dst + col * 8, src, 8);
		}
	}

	of_emit_cache_clean(scratch, (uint32_t)(pitch * drawline));

	for (row=0 ; row<drawline ; row++)
	{
		of_emit_span_t sp = {
			.fb_addr   = (uint32_t)(uintptr_t)
				(vid.conbuffer + (y + row) * vid.conrowbytes + x),
			.tex_addr  = (uint32_t)(uintptr_t)(scratch + row * pitch),
			.s         = 0,
			.t         = 0,
			.sstep     = 0x10000,
			.tstep     = 0,
			.count     = (uint16_t)pitch,
			.flags     = OF_EMIT_SKIP_ZERO,
			.fb_stride = 1,
			.tex_width = (uint16_t)pitch,
		};
		of_emit_span(&sp);
	}
}

/*
================
Draw_String
================
*/
void Draw_StringLen (int x, int y, char *str, int len);

void Draw_String (int x, int y, char *str)
{
	Draw_StringLen (x, y, str, strlen(str));
}

void Draw_StringLen (int x, int y, char *str, int len)
{
	int i, src_row, drawline;

	if (len <= 0 || y <= -8)
		return;

	while (len > 0 && (str[len-1] == ' ' || str[len-1] == 0))
		len--;
	if (len <= 0)
		return;

	if (y < 0)
	{
		src_row = -y;
		drawline = 8 + y;
		y = 0;
	}
	else
	{
		src_row = 0;
		drawline = 8;
	}

	if (r_pixbytes != 1 || len < 4)
	{
		for (i=0 ; i<len ; i++)
			Draw_Character (x + i*8, y - src_row, str[i]);
		return;
	}

	while (len > 0)
	{
		int chunk = len;
		if (chunk > DRAW_TEXTLINE_MAX_CHARS)
			chunk = DRAW_TEXTLINE_MAX_CHARS;

		Draw_StringChunk8 (x, y, str, chunk, src_row, drawline);
		x += chunk * 8;
		str += chunk;
		len -= chunk;
	}
}

/*
================
Draw_DebugChar

Draws a single character directly to the upper right corner of the screen.
This is for debugging lockups by drawing different chars in different parts
of the code.
================
*/
void Draw_DebugChar (char num)
{
	byte			*dest;
	byte			*source;
	int				drawline;	
	extern byte		*draw_chars;
	int				row, col;

	if (!vid.direct)
		return;		// don't have direct FB access, so no debugchars...

	drawline = 8;

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	dest = vid.direct + 312;

	while (drawline--)
	{
		dest[0] = source[0];
		dest[1] = source[1];
		dest[2] = source[2];
		dest[3] = source[3];
		dest[4] = source[4];
		dest[5] = source[5];
		dest[6] = source[6];
		dest[7] = source[7];
		source += 128;
		dest += 320;
	}
}

/*
=============
Draw_Pic
=============
*/
void Draw_Pic (int x, int y, qpic_t *pic)
{
	unsigned short	*pusdest;
	int				v, u;

	if ((x < 0) ||
		(x + pic->width > vid.width) ||
		(y < 0) ||
		(y + pic->height > vid.height))
	{
		Sys_Error ("Draw_Pic: bad coordinates");
	}

	if (r_pixbytes == 1)
	{
		/* qpic pixels are immutable after load; publish each source to
		 * SDRAM once, then let repeated HUD/menu draws reuse it. */
		Draw_CleanPicForGpu (pic, false);
		of_emit_blit(x, y, pic->width, pic->height,
		             pic->data, pic->width, 0, 0, /*skip_key_ff=*/0);
	}
	else
	{
		pusdest = (unsigned short *)vid.buffer + y * (vid.rowbytes >> 1) + x;

		for (v=0 ; v<pic->height ; v++)
		{
			byte *source = pic->data + v * pic->width;
			for (u=0 ; u<pic->width ; u++)
			{
				pusdest[u] = d_8to16table[source[u]];
			}

			pusdest += vid.rowbytes >> 1;
		}
	}
}


/*
=============
Draw_TransPic
=============
*/
void Draw_TransPic (int x, int y, qpic_t *pic)
{
	byte	*source, tbyte;
	unsigned short	*pusdest;
	int				v, u;

	if (x < 0 || (unsigned)(x + pic->width) > vid.width || y < 0 ||
		 (unsigned)(y + pic->height) > vid.height)
	{
		Sys_Error ("Draw_TransPic: bad coordinates");
	}

	source = pic->data;

	if (r_pixbytes == 1)
	{
		/* GPU blit with key-color transparency.  TRANSPARENT_COLOR
		 * (0xFF) maps to of_emit_blit's skip_key_ff path which
		 * drops 0xFF texels via the SPAN_SKIP_ZERO flag in the
		 * fragment processor. */
		Draw_CleanPicForGpu (pic, false);
		of_emit_blit(x, y, pic->width, pic->height,
		             pic->data, pic->width, 0, 0, /*skip_key_ff=*/1);
	}
	else
	{
		pusdest = (unsigned short *)vid.buffer + y * (vid.rowbytes >> 1) + x;

		for (v=0 ; v<pic->height ; v++)
		{
			for (u=0 ; u<pic->width ; u++)
			{
				tbyte = source[u];

				if (tbyte != TRANSPARENT_COLOR)
				{
					pusdest[u] = d_8to16table[tbyte];
				}
			}

			pusdest += vid.rowbytes >> 1;
			source += pic->width;
		}
	}
}


/*
=============
Draw_TransPicTranslate
=============
*/
void Draw_TransPicTranslate (int x, int y, qpic_t *pic, byte *translation)
{
	byte	*source, tbyte;
	unsigned short	*pusdest;
	int				v, u;

	if (x < 0 || (unsigned)(x + pic->width) > vid.width || y < 0 ||
		 (unsigned)(y + pic->height) > vid.height)
	{
		Sys_Error ("Draw_TransPic: bad coordinates");
	}
		
	source = pic->data;

	if (r_pixbytes == 1)
	{
		for (v=0 ; v<pic->height ; v++)
		{
			byte *scratch = Draw_AllocTranslateRowBuffer ();

			for (u=0 ; u<pic->width ; u++)
			{
				tbyte = source[u];
				scratch[u] = (tbyte != TRANSPARENT_COLOR) ?
					translation[tbyte] : TRANSPARENT_COLOR;
			}

			of_emit_cache_clean(scratch, (uint32_t)pic->width);

			of_emit_span_t sp = {
				.fb_addr   = (uint32_t)(uintptr_t)
					(vid.buffer + (y + v) * vid.rowbytes + x),
				.tex_addr  = (uint32_t)(uintptr_t)scratch,
				.s         = 0,
				.t         = 0,
				.sstep     = 0x10000,
				.tstep     = 0,
				.count     = (uint16_t)pic->width,
				.flags     = OF_EMIT_SKIP_ZERO,
				.fb_stride = 1,
				.tex_width = (uint16_t)pic->width,
			};
			of_emit_span(&sp);

			source += pic->width;
		}
	}
	else
	{
	// FIXME: pretranslate at load time?
		pusdest = (unsigned short *)vid.buffer + y * (vid.rowbytes >> 1) + x;

		for (v=0 ; v<pic->height ; v++)
		{
			for (u=0 ; u<pic->width ; u++)
			{
				tbyte = source[u];

				if (tbyte != TRANSPARENT_COLOR)
				{
					pusdest[u] = d_8to16table[tbyte];
				}
			}

			pusdest += vid.rowbytes >> 1;
			source += pic->width;
		}
	}
}


void Draw_CharToConback (int num, byte *dest)
{
	int		row, col;
	byte	*source;
	int		drawline;
	int		x;

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	drawline = 8;

	while (drawline--)
	{
		for (x=0 ; x<8 ; x++)
			if (source[x])
				dest[x] = 0x60 + source[x];
		source += 128;
		dest += 320;
	}

}

/*
================
Draw_ConsoleBackground

================
*/
void Draw_ConsoleBackground (int lines)
{
	int				x, y, v;
	byte			*src, *dest;
	unsigned short	*pusdest;
	int				f, fstep;
	qpic_t			*conback;
	char			ver[100];
	unsigned		generation;
	static qpic_t	*last_conback;
	static unsigned	last_generation;

	conback = Draw_CachePic ("gfx/conback.lmp");
	generation = Draw_CachePicGeneration ("gfx/conback.lmp");

// hack the version number directly into the pic
	if (conback != last_conback || generation != last_generation)
	{
		sprintf (ver, "%s", QUAKE_CORE_DISPLAY_VERSION);
		dest = conback->data + 320*186 + 320 - 11 - 8*strlen(ver);

		for (x=0 ; x<strlen(ver) ; x++)
			Draw_CharToConback (ver[x], dest+(x<<3));

		Draw_CleanPicForGpu (conback, true);
		last_conback = conback;
		last_generation = generation;
	}
	
// draw the pic
	if (r_pixbytes == 1)
	{
		fstep = 320*0x10000/vid.conwidth;

		for (y=0 ; y<lines ; y++)
		{
			v = (vid.conheight - lines + y)*200/vid.conheight;
			of_emit_span_t sp = {
				.fb_addr   = (uint32_t)(uintptr_t)
					(vid.conbuffer + y * vid.conrowbytes),
				.tex_addr  = (uint32_t)(uintptr_t)conback->data,
				.s         = 0,
				.t         = v << 16,
				.sstep     = fstep,
				.tstep     = 0,
				.count     = (uint16_t)vid.conwidth,
				.flags     = 0,
				.fb_stride = 1,
				.tex_width = 320,
			};
			of_emit_span(&sp);
		}
	}
	else
	{
		pusdest = (unsigned short *)vid.conbuffer;

		for (y=0 ; y<lines ; y++, pusdest += (vid.conrowbytes >> 1))
		{
		// FIXME: pre-expand to native format?
		// FIXME: does the endian switching go away in production?
			v = (vid.conheight - lines + y)*200/vid.conheight;
			src = conback->data + v*320;
			f = 0;
			fstep = 320*0x10000/vid.conwidth;
			for (x=0 ; x<vid.conwidth ; x+=4)
			{
				pusdest[x] = d_8to16table[src[f>>16]];
				f += fstep;
				pusdest[x+1] = d_8to16table[src[f>>16]];
				f += fstep;
				pusdest[x+2] = d_8to16table[src[f>>16]];
				f += fstep;
				pusdest[x+3] = d_8to16table[src[f>>16]];
				f += fstep;
			}
		}
	}
}


/*
==============
R_DrawRect8
==============
*/
void R_DrawRect8 (vrect_t *prect, int rowbytes, byte *psrc,
	int transparent)
{
	/* Route the rect blit through the GPU.  psrc is pre-offset into
	 * draw_backtile by Draw_TileClear, so it points partway into the
	 * tile data — of_emit_blit treats it as the upper-left of a
	 * (rowbytes pitch) source surface and walks blit_w × blit_h
	 * pixels.  Cache-clean of the tile happens once in Draw_Init
	 * (data never mutates), so per-call here is cheap. */
	of_emit_blit(prect->x, prect->y,
	             prect->width, prect->height,
	             psrc, rowbytes,
	             0, 0,
	             /*skip_key_ff=*/transparent ? 1 : 0);
}


/*
==============
R_DrawRect16
==============
*/
void R_DrawRect16 (vrect_t *prect, int rowbytes, byte *psrc,
	int transparent)
{
	byte			t;
	int				i, j, srcdelta, destdelta;
	unsigned short	*pdest;

// FIXME: would it be better to pre-expand native-format versions?

	pdest = (unsigned short *)vid.buffer +
			(prect->y * (vid.rowbytes >> 1)) + prect->x;

	srcdelta = rowbytes - prect->width;
	destdelta = (vid.rowbytes >> 1) - prect->width;

	if (transparent)
	{
		for (i=0 ; i<prect->height ; i++)
		{
			for (j=0 ; j<prect->width ; j++)
			{
				t = *psrc;
				if (t != TRANSPARENT_COLOR)
				{
					*pdest = d_8to16table[t];
				}

				psrc++;
				pdest++;
			}

			psrc += srcdelta;
			pdest += destdelta;
		}
	}
	else
	{
		for (i=0 ; i<prect->height ; i++)
		{
			for (j=0 ; j<prect->width ; j++)
			{
				*pdest = d_8to16table[*psrc];
				psrc++;
				pdest++;
			}

			psrc += srcdelta;
			pdest += destdelta;
		}
	}
}


/*
=============
Draw_TileClear

This repeats a 64*64 tile graphic to fill the screen around a sized down
refresh window.
=============
*/
void Draw_TileClear (int x, int y, int w, int h)
{
	int				width, height, tileoffsetx, tileoffsety;
	byte			*psrc;
	vrect_t			vr;

	r_rectdesc.rect.x = x;
	r_rectdesc.rect.y = y;
	r_rectdesc.rect.width = w;
	r_rectdesc.rect.height = h;

	vr.y = r_rectdesc.rect.y;
	height = r_rectdesc.rect.height;

	tileoffsety = vr.y % r_rectdesc.height;

	while (height > 0)
	{
		vr.x = r_rectdesc.rect.x;
		width = r_rectdesc.rect.width;

		if (tileoffsety != 0)
			vr.height = r_rectdesc.height - tileoffsety;
		else
			vr.height = r_rectdesc.height;

		if (vr.height > height)
			vr.height = height;

		tileoffsetx = vr.x % r_rectdesc.width;

		while (width > 0)
		{
			if (tileoffsetx != 0)
				vr.width = r_rectdesc.width - tileoffsetx;
			else
				vr.width = r_rectdesc.width;

			if (vr.width > width)
				vr.width = width;

			psrc = r_rectdesc.ptexbytes +
					(tileoffsety * r_rectdesc.rowbytes) + tileoffsetx;

			if (r_pixbytes == 1)
			{
				R_DrawRect8 (&vr, r_rectdesc.rowbytes, psrc, 0);
			}
			else
			{
				R_DrawRect16 (&vr, r_rectdesc.rowbytes, psrc, 0);
			}

			vr.x += vr.width;
			width -= vr.width;
			tileoffsetx = 0;	// only the left tile can be left-clipped
		}

		vr.y += vr.height;
		height -= vr.height;
		tileoffsety = 0;		// only the top tile can be top-clipped
	}
}


/*
=============
Draw_Fill

Fills a box of pixels with a single color
=============
*/
void Draw_Fill (int x, int y, int w, int h, int c)
{
	unsigned short	*pusdest;
	unsigned		uc;
	int				u, v;

	if (r_pixbytes == 1)
	{
		/* GPU CMD_CLEAR_RECT — single ring command, no per-pixel CPU
		 * work.  Beats the previous tight uncached-SDRAM byte-write
		 * loop on any non-trivial rect size. */
		of_emit_clear_rect(x, y, w, h, (unsigned char)c);
	}
	else
	{
		uc = d_8to16table[c];

		pusdest = (unsigned short *)vid.buffer + y * (vid.rowbytes >> 1) + x;
		for (v=0 ; v<h ; v++, pusdest += (vid.rowbytes >> 1))
			for (u=0 ; u<w ; u++)
				pusdest[u] = uc;
	}
}
//=============================================================================

/*
================
Draw_FadeScreen

================
*/
void Draw_FadeScreen (void)
{
	int			x,y;
	byte		*pbuf;

	if (r_pixbytes == 1)
	{
		for (y=0 ; y<vid.height ; y++)
		{
			of_emit_span_t sp = {
				.fb_addr   = (uint32_t)(uintptr_t)(vid.buffer + vid.rowbytes*y),
				.tex_addr  = (uint32_t)(uintptr_t)draw_fade_pattern,
				.s         = 0,
				.t         = (y & 1) << 16,
				.sstep     = 0x10000,
				.tstep     = 0,
				.count     = (uint16_t)vid.width,
				.flags     = OF_EMIT_SKIP_ZERO,
				.fb_stride = 1,
				.tex_width = 4,
				.tex_w_mask = 3,
				.tex_h_mask = 1,
			};
			of_emit_span(&sp);
		}
		of_emit_kick();
		return;
	}

	of_emit_prepare_framebuffer_for_cpu();

	VID_UnlockBuffer ();
	S_ExtraUpdate ();
	VID_LockBuffer ();

	for (y=0 ; y<vid.height ; y++)
	{
		int	t;

		pbuf = (byte *)(vid.buffer + vid.rowbytes*y);
		t = (y & 1) << 1;

		for (x=0 ; x<vid.width ; x++)
		{
			if ((x & 3) != t)
				pbuf[x] = 0;
		}
	}

	VID_UnlockBuffer ();
	S_ExtraUpdate ();
	VID_LockBuffer ();
}

//=============================================================================

/*
================
Draw_BeginDisc

Draws the little blue disc in the corner of the screen.
Call before beginning any disc IO.
================
*/
void Draw_BeginDisc (void)
{
	if (!draw_disc)
		return;
	D_BeginDirectRect (vid.width - 24, 0, draw_disc->data, 24, 24);
}


/*
================
Draw_EndDisc

Erases the disc icon.
Call after completing any disc IO
================
*/
void Draw_EndDisc (void)
{

	D_EndDirectRect (vid.width - 24, 0, 24, 24);
}
