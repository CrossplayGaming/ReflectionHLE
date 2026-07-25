/* KeenLauncher widescreen compositor for Keen Dreams. See kl_compositor.h. */

#include "kd_def.h"
#include "kl_compositor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* backend side (be_video.c, under REFKEEN_VER_KDREAMS) */
void BE_ST_KL_WideFrame(const uint8_t *pix, int w, int h);
void BE_ST_KL_WideOff(void);

/* sprite world positions captured in RF_PlaceSprite_EGA (id_rf.c) */
typedef struct
{
	id0_unsigned_t worldx, worldy; /* live capture (placements in progress) */
	id0_unsigned_t showx, showy;   /* completed sim frame N */
	id0_unsigned_t prevx, prevy;   /* completed sim frame N-1 */
	id0_unsigned_t grseg;
	id0_int_t priority;
	id0_int_t live;
} KL_SprNote;

#define KL_MAXSPR 60 /* MAXSPRITES is 60 in id_rf.c */
static KL_SprNote kl_spr[KL_MAXSPR];

/* camera + timing snapshots for present-time interpolation */
static long kl_cam_cur_x, kl_cam_cur_y, kl_cam_prev_x, kl_cam_prev_y;
static uint32_t kl_refresh_ms;  /* when the current sim frame landed */
static uint32_t kl_frame_ms;    /* its expected duration */
static int kl_have_frame;

#include "be_st_sdl_private.h" /* BEL_ST_GetTicksMS */

void KL_NoteSprite(void *user, id0_unsigned_t gx, id0_unsigned_t gy,
                   id0_unsigned_t spritenumber, id0_int_t priority,
                   void *arraybase, size_t elemsize)
{
	size_t idx = ((char *)user - (char *)arraybase) / elemsize;

	if (idx >= KL_MAXSPR)
		return;
	if (!kl_spr[idx].live)
	{
		/* fresh sprite: no previous position to glide from */
		kl_spr[idx].showx = kl_spr[idx].prevx = gx;
		kl_spr[idx].showy = kl_spr[idx].prevy = gy;
	}
	kl_spr[idx].worldx = gx;
	kl_spr[idx].worldy = gy;
	kl_spr[idx].grseg = spritenumber;
	kl_spr[idx].priority = priority;
	kl_spr[idx].live = 1;
}

void KL_DropSprite(void *user, void *arraybase, size_t elemsize)
{
	size_t idx = ((char *)user - (char *)arraybase) / elemsize;

	if (idx < KL_MAXSPR)
		kl_spr[idx].live = 0;
}

/* Deactivation ghosts, keyed by the OBJECT (sprite slots get reused): the
 * engine erases sprites of objects leaving the vanilla activation window,
 * which in the wide view happens on screen and read as edge pop-in.  The
 * ghost keeps composing the last placement frozen at the object's true sim
 * position until the object reactivates or is removed. */
typedef struct
{
	void *owner;
	id0_unsigned_t wx, wy, grseg;
	id0_int_t priority;
	int used;
} KL_Ghost;

#define KL_MAXGHOST 128
static KL_Ghost kl_ghost[KL_MAXGHOST];

void KL_GhostFromSprite(void *owner, void *user, void *arraybase,
                        size_t elemsize)
{
	size_t idx = ((char *)user - (char *)arraybase) / elemsize;
	int i, slot = -1;

	if (idx >= KL_MAXSPR || !kl_spr[idx].live || !owner)
		return;
	for (i = 0; i < KL_MAXGHOST; i++)
	{
		if (kl_ghost[i].used && kl_ghost[i].owner == owner)
		{
			slot = i;
			break;
		}
		if (!kl_ghost[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return;
	kl_ghost[slot].owner = owner;
	kl_ghost[slot].wx = kl_spr[idx].showx;
	kl_ghost[slot].wy = kl_spr[idx].showy;
	kl_ghost[slot].grseg = kl_spr[idx].grseg;
	kl_ghost[slot].priority = kl_spr[idx].priority;
	kl_ghost[slot].used = 1;
}

void KL_DropGhostObj(void *owner)
{
	int i;

	for (i = 0; i < KL_MAXGHOST; i++)
		if (kl_ghost[i].used && kl_ghost[i].owner == owner)
			kl_ghost[i].used = 0;
}

/* ---------------------------------------------------------------- decode */

/* Planar EGA -> chunky 8bpp caches.  Tiles are 4 planes of 32 bytes;
 * masked tiles are mask + 4 planes; sprites are mask + 4 planes of
 * planesize bytes at sourceoffset[0] (shift 0: whole-pixel placement,
 * matching the collection's crisp-pixel recipe). */

static uint8_t *kl_tile_cache[NUMTILE16];
static uint8_t *kl_mtile_cache[NUMTILE16M]; /* 2 bytes/px: color, opaque */

static uint8_t *kl_decode_tile(id0_unsigned_t num)
{
	const uint8_t *src;
	uint8_t *out;
	int p, row, b, bit;

	if (num >= NUMTILE16)
		return NULL;
	if (kl_tile_cache[num])
		return kl_tile_cache[num];
	src = (const uint8_t *)grsegs[STARTTILE16 + num];
	if (!src)
		return NULL;
	out = (uint8_t *)calloc(1, 16 * 16);
	if (!out)
		return NULL;
	for (p = 0; p < 4; p++)
		for (row = 0; row < 16; row++)
			for (b = 0; b < 2; b++)
			{
				uint8_t byte = src[p * 32 + row * 2 + b];
				for (bit = 0; bit < 8; bit++)
					if (byte & (0x80 >> bit))
						out[row * 16 + b * 8 + bit] |= 1 << p;
			}
	kl_tile_cache[num] = out;
	return out;
}

static uint8_t *kl_decode_mtile(id0_unsigned_t num)
{
	const uint8_t *src;
	uint8_t *out;
	int p, row, b, bit;

	if (num >= NUMTILE16M)
		return NULL;
	if (kl_mtile_cache[num])
		return kl_mtile_cache[num];
	src = (const uint8_t *)grsegs[STARTTILE16M + num];
	if (!src)
		return NULL;
	out = (uint8_t *)calloc(1, 16 * 16 * 2);
	if (!out)
		return NULL;
	/* plane 0 is the mask: 0 bits = opaque foreground */
	for (row = 0; row < 16; row++)
		for (b = 0; b < 2; b++)
		{
			uint8_t byte = src[row * 2 + b];
			for (bit = 0; bit < 8; bit++)
				if (!(byte & (0x80 >> bit)))
					out[(row * 16 + b * 8 + bit) * 2 + 1] = 1;
		}
	for (p = 0; p < 4; p++)
		for (row = 0; row < 16; row++)
			for (b = 0; b < 2; b++)
			{
				uint8_t byte = src[32 + p * 32 + row * 2 + b];
				for (bit = 0; bit < 8; bit++)
					if (byte & (0x80 >> bit))
						out[(row * 16 + b * 8 + bit) * 2] |= 1 << p;
			}
	kl_mtile_cache[num] = out;
	return out;
}

/* sprite decode cache, keyed by chunk; 2 bytes/px: color, opaque */
typedef struct
{
	id0_unsigned_t grseg;
	int w, h; /* pixels */
	uint8_t *pix;
} KL_SprImg;

#define KL_SPRIMG_MAX 128
static KL_SprImg kl_sprimg[KL_SPRIMG_MAX];
static int kl_nsprimg;

/* The score box is special twice over: the game updates its digits by
 * writing INTO the sprite chunk (so it must never be cached), and it
 * follows the vanilla camera (so it must be pinned, not world-placed). */
static KL_SprImg kl_scorebox_img;
static uint8_t kl_scorebox_pix[64 * 64 * 2];

static KL_SprImg *kl_decode_sprite(id0_unsigned_t grseg)
{
	const spritetype_ega *block;
	const uint8_t *data;
	spritetabletype id0_far *spr;
	KL_SprImg *img;
	int wbytes, h, planesize, p, row, b, bit, i;

	int nocache = (grseg == SCOREBOXSPR);

	if (!nocache)
		for (i = 0; i < kl_nsprimg; i++)
			if (kl_sprimg[i].grseg == grseg)
				return &kl_sprimg[i];
	if (kl_nsprimg >= KL_SPRIMG_MAX)
	{
		/* cache full (level change churn): drop everything, re-fill */
		for (i = 0; i < kl_nsprimg; i++)
			free(kl_sprimg[i].pix);
		kl_nsprimg = 0;
	}
	block = (const spritetype_ega *)grsegs[grseg];
	if (!block || grseg < STARTSPRITES || grseg >= STARTSPRITES + NUMSPRITES)
		return NULL;
	spr = &spritetable[grseg - STARTSPRITES];
	wbytes = block->width[0];
	h = spr->height;
	planesize = block->planesize[0];
	if (wbytes <= 0 || h <= 0 || planesize < wbytes * h)
		return NULL;
	if (nocache)
	{
		if ((size_t)(wbytes * 8) * h * 2 > sizeof(kl_scorebox_pix))
			return NULL;
		img = &kl_scorebox_img;
		img->grseg = grseg;
		img->w = wbytes * 8;
		img->h = h;
		img->pix = kl_scorebox_pix;
		memset(kl_scorebox_pix, 0, (size_t)img->w * h * 2);
	}
	else
	{
		img = &kl_sprimg[kl_nsprimg];
		img->grseg = grseg;
		img->w = wbytes * 8;
		img->h = h;
		img->pix = (uint8_t *)calloc(1, (size_t)img->w * h * 2);
		if (!img->pix)
			return NULL;
	}
	data = (const uint8_t *)block + block->sourceoffset[0];
	/* mask plane first: 0 bits are opaque */
	for (row = 0; row < h; row++)
		for (b = 0; b < wbytes; b++)
		{
			uint8_t byte = data[row * wbytes + b];
			for (bit = 0; bit < 8; bit++)
				if (!(byte & (0x80 >> bit)))
					img->pix[(row * img->w + b * 8 + bit) * 2 + 1] = 1;
		}
	for (p = 0; p < 4; p++)
		for (row = 0; row < h; row++)
			for (b = 0; b < wbytes; b++)
			{
				uint8_t byte = data[(p + 1) * planesize + row * wbytes + b];
				for (bit = 0; bit < 8; bit++)
					if (byte & (0x80 >> bit))
						img->pix[(row * img->w + b * 8 + bit) * 2] |= 1 << p;
			}
	if (!nocache)
		kl_nsprimg++;
	return img;
}

void KL_CompReset(void)
{
	int i;

	memset(kl_spr, 0, sizeof(kl_spr));
	memset(kl_ghost, 0, sizeof(kl_ghost));
	kl_have_frame = 0;
	/* graphics chunks may have been purged/reloaded on level change */
	for (i = 0; i < (int)NUMTILE16; i++)
		if (kl_tile_cache[i])
		{
			free(kl_tile_cache[i]);
			kl_tile_cache[i] = NULL;
		}
	for (i = 0; i < (int)NUMTILE16M; i++)
		if (kl_mtile_cache[i])
		{
			free(kl_mtile_cache[i]);
			kl_mtile_cache[i] = NULL;
		}
	for (i = 0; i < kl_nsprimg; i++)
		free(kl_sprimg[i].pix);
	kl_nsprimg = 0;
}

/* ---------------------------------------------------------------- compose */

#define KL_OVL_MAX (320 * 200)
static uint8_t kl_ovl[KL_OVL_MAX];
static int kl_ovl_w, kl_ovl_h, kl_ovl_x, kl_ovl_y, kl_ovl_active;
static long kl_last_camx, kl_last_camy;

static uint8_t kl_frame[KL_COMP_W * KL_COMP_H];

static int kl_wide_enabled(void)
{
	static int cached = -1;

	if (cached < 0)
	{
		const char *e = getenv("KL_WIDE");
		cached = e ? (atoi(e) != 0) : 1;
	}
	return cached;
}

static void kl_draw_ghosts(int priority, long camx, long camy)
{
	int i, x, y;

	for (i = 0; i < KL_MAXGHOST; i++)
	{
		const KL_SprImg *img;
		long sx, sy;

		if (!kl_ghost[i].used || kl_ghost[i].priority != priority)
			continue;
		img = kl_decode_sprite(kl_ghost[i].grseg);
		if (!img)
			continue;
		sx = (long)(kl_ghost[i].wx >> 4) - camx;
		sy = (long)(kl_ghost[i].wy >> 4) - camy;
		for (y = 0; y < img->h; y++)
		{
			long dy = sy + y;
			if (dy < 0 || dy >= KL_COMP_H)
				continue;
			for (x = 0; x < img->w; x++)
			{
				long dx = sx + x;
				if (dx < 0 || dx >= KL_COMP_W)
					continue;
				if (img->pix[(y * img->w + x) * 2 + 1])
					kl_frame[dy * KL_COMP_W + dx] =
						img->pix[(y * img->w + x) * 2];
			}
		}
	}
}

static void kl_draw_sprites(int priority, long camx, long camy, long alpha256)
{
	int i, x, y;

	for (i = 0; i < KL_MAXSPR; i++)
	{
		const KL_SprImg *img;
		long sx, sy, px, py, cx, cy;

		if (!kl_spr[i].live || kl_spr[i].priority != priority)
			continue;
		img = kl_decode_sprite(kl_spr[i].grseg);
		if (!img)
			continue;
		if (kl_spr[i].grseg == SCOREBOXSPR)
		{
			/* pinned HUD: place relative to the vanilla camera of the
			   current sim frame -- immune to the wide camera's widening,
			   clamping and interpolation (it followed the clamp before,
			   which is what made it drift at level edges) */
			sx = (long)(kl_spr[i].showx >> 4) - kl_cam_cur_x;
			sy = (long)(kl_spr[i].showy >> 4) - kl_cam_cur_y;
		}
		else
		{
		/* lerp in whole world pixels; big jumps (teleports) snap */
		px = (long)(kl_spr[i].prevx >> 4);
		py = (long)(kl_spr[i].prevy >> 4);
		cx = (long)(kl_spr[i].showx >> 4);
		cy = (long)(kl_spr[i].showy >> 4);
		if (cx - px > 32 || px - cx > 32 || cy - py > 32 || py - cy > 32)
		{
			px = cx;
			py = cy;
		}
		sx = px + ((cx - px) * alpha256 >> 8) - camx;
		sy = py + ((cy - py) * alpha256 >> 8) - camy;
		}
		for (y = 0; y < img->h; y++)
		{
			long dy = sy + y;
			if (dy < 0 || dy >= KL_COMP_H)
				continue;
			for (x = 0; x < img->w; x++)
			{
				long dx = sx + x;
				if (dx < 0 || dx >= KL_COMP_W)
					continue;
				if (img->pix[(y * img->w + x) * 2 + 1])
					kl_frame[dy * KL_COMP_W + dx] =
						img->pix[(y * img->w + x) * 2];
			}
		}
	}
}

static void kl_compose(long alpha256)
{
	long camx, camy, maxcamx, cx0, cy0, cx1, cy1;
	int tx0, ty0, px, py, tx, ty, pri;

	/* camera: lerp the vanilla origin between the last two sim frames,
	   widen equally both sides, slide at map edges */
	cx0 = kl_cam_prev_x;
	cy0 = kl_cam_prev_y;
	cx1 = kl_cam_cur_x;
	cy1 = kl_cam_cur_y;
	if (cx1 - cx0 > 64 || cx0 - cx1 > 64 || cy1 - cy0 > 64 || cy0 - cy1 > 64)
	{
		cx0 = cx1; /* teleport/level start: snap */
		cy0 = cy1;
	}
	camx = cx0 + ((cx1 - cx0) * alpha256 >> 8) - (KL_COMP_W - 320) / 2;
	camy = cy0 + ((cy1 - cy0) * alpha256 >> 8);
	/* Slide at the PLAYABLE bounds: Dreams maps carry a MAPBORDER ring of
	   out-of-bounds tiles the vanilla camera never shows (originxmin/max);
	   clamping to the raw map edge exposed them in the wide view -- the
	   same bug the Keen 4-6 build fixed with its edge-sliding crop. */
	{
		long mincamx = (long)MAPBORDER * 16;

		maxcamx = ((long)mapwidth - MAPBORDER) * 16 - KL_COMP_W;
		if (maxcamx < mincamx)
		{
			/* level narrower than the wide view: centre what exists */
			camx = (mincamx + maxcamx) / 2;
		}
		else
		{
			if (camx > maxcamx)
				camx = maxcamx;
			if (camx < mincamx)
				camx = mincamx;
		}
	}

	kl_last_camx = camx; /* for the dialog overlay's page->wide mapping */
	kl_last_camy = camy;

	tx0 = (int)(camx >> 4);
	ty0 = (int)(camy >> 4);
	px = -(int)(camx & 15);
	py = -(int)(camy & 15);

	for (ty = 0; ty <= KL_COMP_H / 16 + 1; ty++)
		for (tx = 0; tx <= KL_COMP_W / 16 + 1; tx++)
		{
			int mx = tx0 + tx, my = ty0 + ty;
			const uint8_t *til;
			int ox = px + tx * 16, oy = py + ty * 16, x, y;

			if (mx < 0 || my < 0 || mx >= (int)mapwidth ||
			    my >= (int)mapheight)
				continue;
			til = kl_decode_tile(mapsegs[0][my * mapwidth + mx]);
			if (!til)
				continue;
			for (y = 0; y < 16; y++)
			{
				int dy = oy + y;
				if (dy < 0 || dy >= KL_COMP_H)
					continue;
				for (x = 0; x < 16; x++)
				{
					int dx = ox + x;
					if (dx < 0 || dx >= KL_COMP_W)
						continue;
					kl_frame[dy * KL_COMP_W + dx] = til[y * 16 + x];
				}
			}
			/* Foreground tiles WITHOUT the INTILE high bit belong to the
			   scenery UNDER the sprites (bridges Keen walks on); only
			   tiles with the bit go over sprites, exactly as the engine's
			   RFL_MaskForegroundTiles decides. */
			{
				id0_unsigned_t fnum = mapsegs[1][my * mapwidth + mx];
				const uint8_t *ftl;

				if (fnum && !(tinf[fnum + INTILE] & 0x80) &&
				    (ftl = kl_decode_mtile(fnum)) != NULL)
					for (y = 0; y < 16; y++)
					{
						int dy = oy + y;
						if (dy < 0 || dy >= KL_COMP_H)
							continue;
						for (x = 0; x < 16; x++)
						{
							int dx = ox + x;
							if (dx < 0 || dx >= KL_COMP_W)
								continue;
							if (ftl[(y * 16 + x) * 2 + 1])
								kl_frame[dy * KL_COMP_W + dx] =
									ftl[(y * 16 + x) * 2];
						}
					}
			}
		}

	for (pri = 0; pri < PRIORITIES; pri++)
	{
		if (pri == MASKEDTILEPRIORITY)
		{
			/* masked foreground tiles above sprite priorities 0-2 */
			for (ty = 0; ty <= KL_COMP_H / 16 + 1; ty++)
				for (tx = 0; tx <= KL_COMP_W / 16 + 1; tx++)
				{
					int mx = tx0 + tx, my = ty0 + ty;
					const uint8_t *til;
					int ox = px + tx * 16, oy = py + ty * 16, x, y;
					id0_unsigned_t fnum;

					if (mx < 0 || my < 0 || mx >= (int)mapwidth ||
					    my >= (int)mapheight)
						continue;
					fnum = mapsegs[1][my * mapwidth + mx];
					if (!fnum || !(tinf[fnum + INTILE] & 0x80))
						continue;
					til = kl_decode_mtile(fnum);
					if (!til)
						continue;
					for (y = 0; y < 16; y++)
					{
						int dy = oy + y;
						if (dy < 0 || dy >= KL_COMP_H)
							continue;
						for (x = 0; x < 16; x++)
						{
							int dx = ox + x;
							if (dx < 0 || dx >= KL_COMP_W)
								continue;
							if (til[(y * 16 + x) * 2 + 1])
								kl_frame[dy * KL_COMP_W + dx] =
									til[(y * 16 + x) * 2];
						}
					}
				}
		}
		kl_draw_ghosts(pri, camx, camy);
		kl_draw_sprites(pri, camx, camy, alpha256);
	}

	if (kl_ovl_active)
	{
		/* Centre the dialog horizontally in the wide frame (matching the
		   Keen 1-3 build's explicit centering); vertical keeps the page
		   position mapped through the camera difference. */
		int ox = (KL_COMP_W - kl_ovl_w) / 2, x, y;
		int oy = (int)(kl_cam_cur_y - kl_last_camy);

		for (y = 0; y < kl_ovl_h; y++)
		{
			int dy = kl_ovl_y + oy + y;
			if (dy < 0 || dy >= KL_COMP_H)
				continue;
			for (x = 0; x < kl_ovl_w; x++)
			{
				int dx = ox + x;
				if (dx < 0 || dx >= KL_COMP_W)
					continue;
				kl_frame[dy * KL_COMP_W + dx] =
					(getenv("KL_TRACE") &&
					 (y == 0 || y == kl_ovl_h - 1 || x == 0 ||
					  x == kl_ovl_w - 1))
					? 5 /* magenta debug border */
					: kl_ovl[y * kl_ovl_w + x];
			}
		}
	}

	if (kl_ovl_active && getenv("KL_FRAMEDUMP"))
	{
		static int dumped, settle;
		FILE *df = (!dumped && ++settle >= 40)
		           ? fopen(getenv("KL_FRAMEDUMP"), "wb") : NULL;
		if (df)
			dumped = 1;
		if (df)
		{
			static const uint8_t rgb16[16][3] = {
				{0,0,0},{0,0,170},{0,170,0},{0,170,170},{170,0,0},
				{170,0,170},{170,85,0},{170,170,170},{85,85,85},
				{85,85,255},{85,255,85},{85,255,255},{255,85,85},
				{255,85,255},{255,255,85},{255,255,255}};
			int i2;
			fprintf(df, "P6\n%d %d\n255\n", KL_COMP_W, KL_COMP_H);
			for (i2 = 0; i2 < KL_COMP_W * KL_COMP_H; i2++)
				fwrite(rgb16[kl_frame[i2] & 15], 1, 3, df);
			fclose(df);
		}
	}

	BE_ST_KL_WideFrame(kl_frame, KL_COMP_W, KL_COMP_H);
}

/* present-time recompose; called by the backend whenever it refreshes the
 * host display while the wide view is live.  Interpolates on the monotonic
 * ms clock -- NEVER composes a fresh frame at alpha=1 and then rewinds (the
 * "everything jitters" sawtooth the Keen 1-3 port hit). */
int KL_CompPresentTick(void)
{
	uint32_t now;
	long alpha256;

	if (!kl_have_frame || !kl_wide_enabled() || !mapsegs[0])
		return 0;
	now = BEL_ST_GetTicksMS();
	if (kl_frame_ms == 0)
		alpha256 = 256;
	else
	{
		alpha256 = (long)((now - kl_refresh_ms) * 256 / kl_frame_ms);
		if (alpha256 < 0)
			alpha256 = 0;
		if (alpha256 > 256)
			alpha256 = 256;
	}
	kl_compose(alpha256);
	return 1;
}

/* Backdrop: pick a dark, lightly-textured tile from the episode's OWN
 * tileset by measurement (mean luminance + spread -- the same scoring the
 * Keen 1-3 port used; eyeballing picks garish tiles), then scatter it into
 * a 64x64 pattern (tile in 5 of 16 cells, mask 0x8412) so it reads as a
 * starfield rather than wallpaper. */
static void kl_send_backdrop(void)
{
	static const uint8_t luma[16] = {
		0, 20, 35, 40, 30, 35, 45, 65,
		40, 60, 75, 80, 70, 75, 90, 100
	};
	void BE_ST_KL_SetBackdrop(const uint8_t *pix64);
	static int sent;
	long best = -1, bestscore = 0;
	id0_unsigned_t t;
	uint8_t pattern[64 * 64];
	int cx, cy, x, y;

	if (sent)
		return;
	/* KL_BACKDROP=<tile> overrides the measured pick (-1 = plain black) */
	{
		const char *e = getenv("KL_BACKDROP");
		if (e)
		{
			long v = atol(e);
			if (v < 0)
			{
				sent = 1;
				return;
			}
			if (v > 0 && v < (long)NUMTILE16 && kl_decode_tile((id0_unsigned_t)v))
				best = v;
		}
	}
	if (best < 0)
	for (t = 1; t < NUMTILE16; t++)
	{
		const uint8_t *til = kl_decode_tile(t);
		long mean = 0, var = 0, i2, d;

		if (!til)
			continue;
		for (i2 = 0; i2 < 256; i2++)
			mean += luma[til[i2] & 15];
		mean /= 256;
		for (i2 = 0; i2 < 256; i2++)
		{
			d = luma[til[i2] & 15] - mean;
			var += d * d;
		}
		var /= 256;
		{
			long sd = 0;
			while (sd * sd < var)
				sd++;
			if (getenv("KL_TRACE") && mean < 45 && sd >= 2)
				fprintf(stderr, "KL cand: tile %u mean %ld sd %ld\n",
				        t, mean, sd);
			if (mean < 40 && sd >= 2 && sd <= 35)
			{
				long score = mean + (sd > 11 ? sd - 11 : 11 - sd);
				if (best < 0 || score < bestscore)
				{
					best = (long)t;
					bestscore = score;
				}
			}
		}
	}
	if (getenv("KL_TRACE"))
		fprintf(stderr, "KL backdrop: tile %ld score %ld\n", best, bestscore);
	if (best < 0)
		return;
	/* Solid textures tile CONTINUOUSLY (the scatter mask the other games
	   use was a starfield trick -- sparse points on black; floating dirt
	   blocks just read as noise).  A dimming remap keeps the wall clearly
	   behind the game: bright EGA colors drop to their dark counterparts. */
	{
		static const uint8_t dim[16] = {
			0, 1, 2, 3, 4, 5, 6, 8,
			0, 1, 2, 3, 4, 5, 6, 8
		};
		const uint8_t *til = kl_decode_tile((id0_unsigned_t)best);

		if (!til)
			return;
		for (cy = 0; cy < 4; cy++)
			for (cx = 0; cx < 4; cx++)
				for (y = 0; y < 16; y++)
					for (x = 0; x < 16; x++)
						pattern[(cy * 16 + y) * 64 + cx * 16 + x] =
							dim[til[y * 16 + x] & 15];
	}
	BE_ST_KL_SetBackdrop(pattern);
	sent = 1;
}

void KL_CompRefresh(void)
{
	int i;

	if (!kl_wide_enabled() || !mapsegs[0])
	{
		BE_ST_KL_WideOff();
		return;
	}

	kl_send_backdrop();

	kl_ovl_active = 0; /* a real sim frame: any dialog is gone */

	/* rotate snapshots: the placements of this frame are complete */
	kl_cam_prev_x = kl_cam_cur_x;
	kl_cam_prev_y = kl_cam_cur_y;
	kl_cam_cur_x = (long)(originxglobal >> 4);
	kl_cam_cur_y = (long)(originyglobal >> 4);
	for (i = 0; i < KL_MAXSPR; i++)
		if (kl_spr[i].live)
		{
			kl_spr[i].prevx = kl_spr[i].showx;
			kl_spr[i].prevy = kl_spr[i].showy;
			kl_spr[i].showx = kl_spr[i].worldx;
			kl_spr[i].showy = kl_spr[i].worldy;
		}
	kl_refresh_ms = BEL_ST_GetTicksMS();
	kl_frame_ms = (uint32_t)(tics > 0 ? tics : 1) * 1000 / 70;
	if (!kl_have_frame)
	{
		/* first frame: no previous state to glide from */
		kl_cam_prev_x = kl_cam_cur_x;
		kl_cam_prev_y = kl_cam_cur_y;
		kl_have_frame = 1;
	}

	KL_CompPresentTick();
}

/* KL_ARTDUMP companion: write the measured backdrop tile as a 16x16 PPM
 * (whatever qualifying tiles are loaded at the time -- at the title map
 * that's the title tileset, which is fine for a launcher background). */
void KL_DumpBackdropTile(const char *path)
{
	static const uint8_t egaRGB[16][3] = {
		{0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00},
		{0x00, 0xAA, 0xAA}, {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA},
		{0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA}, {0x55, 0x55, 0x55},
		{0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
		{0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55},
		{0xFF, 0xFF, 0xFF}};
	static const uint8_t luma[16] = {
		0, 20, 35, 40, 30, 35, 45, 65,
		40, 60, 75, 80, 70, 75, 90, 100
	};
	long best = -1, bestscore = 0;
	id0_unsigned_t t;
	const uint8_t *til;
	FILE *f;
	int x, y;

	for (t = 1; t < NUMTILE16; t++)
	{
		const uint8_t *tt = kl_decode_tile(t);
		long mean = 0, var = 0, i2, d, sd = 0;

		if (!tt)
			continue;
		for (i2 = 0; i2 < 256; i2++)
			mean += luma[tt[i2] & 15];
		mean /= 256;
		for (i2 = 0; i2 < 256; i2++)
		{
			d = luma[tt[i2] & 15] - mean;
			var += d * d;
		}
		var /= 256;
		while (sd * sd < var)
			sd++;
		if (mean < 40 && sd >= 2 && sd <= 35)
		{
			long score = mean + (sd > 11 ? sd - 11 : 11 - sd);
			if (best < 0 || score < bestscore)
			{
				best = (long)t;
				bestscore = score;
			}
		}
	}
	if (best < 0)
		return;
	til = kl_decode_tile((id0_unsigned_t)best);
	if (!til)
		return;
	f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6" "%c" "16 16" "%c" "255" "%c", 10, 10, 10);
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
			fwrite(egaRGB[til[y * 16 + x] & 15], 1, 3, f);
	fclose(f);
}

/* US dialog overlay: while gameplay is paused behind a window (quicksave
 * prompt, PAUSED box...), keep showing the frozen WIDE frame and composite
 * the window rectangle over its centre -- the same behaviour as the other
 * games, instead of dropping to framed 4:3.  Cleared by the next real
 * RF_Refresh (gameplay resumed). */

void KL_CompStandDown(void)
{
	void BE_ST_KL_ReadWindow(int, int, int, int, uint8_t *);
	int wx, wy, ww, wh;

	if (!kl_have_frame || !kl_wide_enabled())
	{
		/* no live wide frame (menus outside gameplay): classic view */
		kl_have_frame = 0;
		BE_ST_KL_WideOff();
		return;
	}
	/* capture the window (plus its 1-tile frame ring) from the page */
	wx = (int)WindowX - 8;
	wy = (int)WindowY - 8;
	ww = (int)WindowW + 16;
	wh = (int)WindowH + 16;
	if (wx < 0) wx = 0;
	if (wy < 0) wy = 0;
	if (ww > 320 - wx) ww = 320 - wx;
	if (wh > 200 - wy) wh = 200 - wy;
	if (ww <= 0 || wh <= 0 || ww * wh > KL_OVL_MAX)
	{
		kl_have_frame = 0;
		BE_ST_KL_WideOff();
		return;
	}
	if (getenv("KL_TRACE"))
	{
		fprintf(stderr,
		        "KL OVL: Win=%d,%d %dx%d bufferofs=%u displayofs=%u panadjust=%u panx=%u\n",
		        (int)WindowX, (int)WindowY, (int)WindowW, (int)WindowH,
		        (unsigned)bufferofs, (unsigned)displayofs,
		        (unsigned)panadjust, (unsigned)panx);
		fflush(stderr);
	}
	BE_ST_KL_ReadWindow(wx, wy, ww, wh, kl_ovl);
	kl_ovl_w = ww;
	kl_ovl_h = wh;
	kl_ovl_x = wx;
	kl_ovl_y = wy;
	kl_ovl_active = 1;
	/* recompose immediately so the dialog shows this frame */
	KL_CompPresentTick();
}
