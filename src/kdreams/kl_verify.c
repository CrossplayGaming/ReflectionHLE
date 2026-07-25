/* KeenLauncher sim-verification harness for Keen Dreams. See kl_verify.h. */

#include "kd_def.h"
#include "kl_verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern id0_int_t rndindex;              /* id_us_a.c */

static FILE *kl_recf, *kl_repf;
static long kl_frames;
static int kl_inited;

/* one frame on disk; field-by-field so struct padding can never differ */
typedef struct
{
	id0_unsigned_t tics;
	id0_int_t b0, b1, cx, cy, xaxis, yaxis, dir;
	id0_int_t lastscan;
	id0_longword_t hash;
} KL_Frame;

/* ---------------------------------------------------------------- hashing */

static id0_longword_t kl_h;

static void kl_hash_begin(void) { kl_h = 2166136261u; }

static void kl_hash(const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	while (n--)
		kl_h = (kl_h ^ *b++) * 16777619u;
}

#define KL_H16(v) { id0_int_t kl_tmp = (id0_int_t)(v); kl_hash(&kl_tmp, 2); }
#define KL_HU16(v) { id0_unsigned_t kl_tmp = (id0_unsigned_t)(v); kl_hash(&kl_tmp, 2); }
#define KL_H32(v) { id0_long_t kl_tmp32 = (id0_long_t)(v); kl_hash(&kl_tmp32, 4); }

/* Complete sim state: every live object (state via its DOS-compatible
 * pointer so the hash is build-independent), gamestate, the RF origin, and
 * the RNG index.  This doubles as the spec of what any future quicksave
 * must capture. */
static id0_longword_t kl_state_hash(void)
{
	objtype *ob;

	kl_hash_begin();
	for (ob = player; ob; ob = (objtype *)ob->next)
	{
		KL_H16(ob->obclass);
		KL_H16(ob->active);
		KL_H16(ob->needtoclip);
		KL_HU16(ob->nothink);
		KL_HU16(ob->x);
		KL_HU16(ob->y);
		KL_H16(ob->xdir);
		KL_H16(ob->ydir);
		KL_H16(ob->xmove);
		KL_H16(ob->ymove);
		KL_H16(ob->xspeed);
		KL_H16(ob->yspeed);
		KL_H16(ob->ticcount);
		KL_H16(ob->ticadjust);
		KL_HU16(ob->state ? ob->state->compatdospointer : 0);
		KL_HU16(ob->shapenum);
		KL_HU16(ob->left);
		KL_HU16(ob->top);
		KL_HU16(ob->right);
		KL_HU16(ob->bottom);
		KL_H16(ob->hitnorth);
		KL_H16(ob->hiteast);
		KL_H16(ob->hitsouth);
		KL_H16(ob->hitwest);
		KL_H16(ob->temp1);
		KL_H16(ob->temp2);
		KL_H16(ob->temp3);
		KL_H16(ob->temp4);
	}
	KL_HU16(gamestate.worldx);
	KL_HU16(gamestate.worldy);
	kl_hash(gamestate.leveldone, sizeof(gamestate.leveldone));
	KL_H32(gamestate.score);
	KL_H32(gamestate.nextextra);
	KL_H16(gamestate.flowerpowers);
	KL_H16(gamestate.boobusbombs);
	KL_H16(gamestate.bombsthislevel);
	KL_H16(gamestate.keys);
	KL_H16(gamestate.mapon);
	KL_H16(gamestate.lives);
	KL_H16(gamestate.difficulty);
	KL_HU16(originxglobal);
	KL_HU16(originyglobal);
	KL_H16(rndindex);
	return kl_h;
}

/* ------------------------------------------------------------ init + warp */

static void kl_init(void)
{
	const char *e;

	if (kl_inited)
		return;
	kl_inited = 1;
	if ((e = getenv("KL_RECORD")) != NULL)
		kl_recf = fopen(e, "wb");
	else if ((e = getenv("KL_REPLAY")) != NULL)
		kl_repf = fopen(e, "rb");
	/* determinism: both sides start from the same RNG index */
	if (kl_recf || kl_repf)
		rndindex = 0;
}

int KL_HarnessActive(void)
{
	return getenv("KL_RECORD") != NULL || getenv("KL_REPLAY") != NULL ||
	       getenv("KL_WARP") != NULL || getenv("KL_ARTDUMP") != NULL;
}

int KL_WarpLevel(void)
{
	const char *e = getenv("KL_WARP");

	kl_init();
	if (e)
	{
		int n = atoi(e);
		if (n >= 0 && n < GAMELEVELS)
			return n;
	}
	return -1;
}

/* -------------------------------------------------------------- per frame */

static void kl_fail(const char *what, long a, long b)
{
	fprintf(stderr, "KL VERIFY FAIL frame %ld: %s %ld != %ld\n",
	        kl_frames, what, a, b);
	fflush(stderr);
	exit(1);
}

void KL_FrameTics(void)
{
	if (!kl_repf)
		return;
	/* peek the frame's recorded tics and force them; the full frame record
	   is consumed in KL_FrameInput right after */
	{
		long pos = ftell(kl_repf);
		KL_Frame fr;
		if (fread(&fr, sizeof(fr), 1, kl_repf) == 1)
			tics = fr.tics;
		fseek(kl_repf, pos, SEEK_SET);
	}
}

void KL_FrameInput(CursorInfo *c)
{
	KL_Frame fr;

	kl_init();
	if (kl_recf)
	{
		fr.tics = tics;
		fr.b0 = c->button0; fr.b1 = c->button1;
		fr.cx = c->x; fr.cy = c->y;
		fr.xaxis = (id0_int_t)c->xaxis; fr.yaxis = (id0_int_t)c->yaxis;
		fr.dir = (id0_int_t)c->dir;
		fr.lastscan = (id0_int_t)LastScan;
		fr.hash = kl_state_hash();
		fwrite(&fr, sizeof(fr), 1, kl_recf);
		fflush(kl_recf);
		kl_frames++;
	}
	else if (kl_repf)
	{
		if (fread(&fr, sizeof(fr), 1, kl_repf) != 1)
		{
			fprintf(stderr, "KL REPLAY OK (%ld frames)\n", kl_frames);
			fflush(stderr);
			exit(0);
		}
		tics = fr.tics;
		c->button0 = fr.b0; c->button1 = fr.b1;
		c->x = fr.cx; c->y = fr.cy;
		c->xaxis = (Motion)fr.xaxis; c->yaxis = (Motion)fr.yaxis;
		c->dir = (Direction)fr.dir;
		LastScan = (ScanCode)fr.lastscan;
		{
			id0_longword_t h = kl_state_hash();
			if (h != fr.hash)
				kl_fail("state hash", (long)h, (long)fr.hash);
		}
		kl_frames++;
	}
}
