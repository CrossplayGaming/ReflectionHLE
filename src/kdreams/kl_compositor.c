/* KeenLauncher widescreen compositor for Keen Dreams. See kl_compositor.h. */

#include "kd_def.h"
#include "kl_compositor.h"

#include <stdlib.h>
#include <string.h>

/* backend side (be_video.c, under REFKEEN_VER_KDREAMS) */
void BE_ST_KL_WideFrame(const uint8_t *pix, int w, int h);
void BE_ST_KL_WideOff(void);

/* sprite world positions captured in RF_PlaceSprite_EGA (id_rf.c) */
typedef struct
{
	id0_unsigned_t worldx, worldy; /* global units, org-adjusted */
	id0_unsigned_t grseg;
	id0_int_t priority;
	id0_int_t live;
} KL_SprNote;

#define KL_MAXSPR 60 /* MAXSPRITES is 60 in id_rf.c */
static KL_SprNote kl_spr[KL_MAXSPR];

void KL_NoteSprite(void *user, id0_unsigned_t gx, id0_unsigned_t gy,
                   id0_unsigned_t spritenumber, id0_int_t priority,
                   void *arraybase, size_t elemsize)
{
	size_t idx = ((char *)user - (char *)arraybase) / elemsize;

	if (idx >= KL_MAXSPR)
		return;
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

static KL_SprImg *kl_decode_sprite(id0_unsigned_t grseg)
{
	const spritetype_ega *block;
	const uint8_t *data;
	spritetabletype id0_far *spr;
	KL_SprImg *img;
	int wbytes, h, planesize, p, row, b, bit, i;

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
	img = &kl_sprimg[kl_nsprimg];
	img->grseg = grseg;
	img->w = wbytes * 8;
	img->h = h;
	img->pix = (uint8_t *)calloc(1, (size_t)img->w * h * 2);
	if (!img->pix)
		return NULL;
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
	kl_nsprimg++;
	return img;
}

void KL_CompReset(void)
{
	int i;

	memset(kl_spr, 0, sizeof(kl_spr));
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

static void kl_draw_sprites(int priority, long camx, long camy)
{
	int i, x, y;

	for (i = 0; i < KL_MAXSPR; i++)
	{
		const KL_SprImg *img;
		long sx, sy;

		if (!kl_spr[i].live || kl_spr[i].priority != priority)
			continue;
		img = kl_decode_sprite(kl_spr[i].grseg);
		if (!img)
			continue;
		sx = (long)(kl_spr[i].worldx >> 4) - camx;
		sy = (long)(kl_spr[i].worldy >> 4) - camy;
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

void KL_CompRefresh(void)
{
	long camx, camy, maxcamx;
	int tx0, ty0, px, py, tx, ty, pri;

	if (!kl_wide_enabled() || !mapsegs[0])
	{
		BE_ST_KL_WideOff();
		return;
	}

	/* camera: vanilla origin, widened equally both sides, slid at edges */
	camx = (long)(originxglobal >> 4) - (KL_COMP_W - 320) / 2;
	camy = (long)(originyglobal >> 4);
	maxcamx = (long)mapwidth * 16 - KL_COMP_W;
	if (camx > maxcamx)
		camx = maxcamx;
	if (camx < 0)
		camx = 0;

	/* background + masked foreground context, sprites between priorities
	   as the engine documents: planes go 0,1,2,MTILES,3 */
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
					if (!fnum)
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
		kl_draw_sprites(pri, camx, camy);
	}

	BE_ST_KL_WideFrame(kl_frame, KL_COMP_W, KL_COMP_H);
}

void KL_CompStandDown(void)
{
	BE_ST_KL_WideOff();
}
