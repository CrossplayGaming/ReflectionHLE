/* KeenLauncher in-game pad rebinding menu for Keen Dreams (F6), bringing it
 * to the collection's standard: every pad action rebindable with a single
 * press, SWAP on conflict (one button never drives two actions, nothing left
 * unbound).  Keyboard movement/buttons keep the game's own control panel;
 * bindings persist through refkeen's cfg on exit, and take effect
 * immediately via the game's own controller-mapping refresh. */

#include "kd_def.h"
#include "be_st_cfg.h"

#include <stdio.h>

int BE_ST_KL_PollPadFeature(void);              /* backend */
extern const char *g_be_st_padFeatureIdToNameMap[];
void PrepareGamePlayControllerMapping(void);    /* kd_play.c */

typedef struct
{
	const char *label;
	int bind;
} KL_BindRow;

static const KL_BindRow kl_rows[] = {
	{"Jump",       BE_ST_CTRL_BIND_KDREAMS_JUMP},
	{"Throw",      BE_ST_CTRL_BIND_KDREAMS_THROW},
	{"Status",     BE_ST_CTRL_BIND_KDREAMS_STATS},
	{"Menu keys",  BE_ST_CTRL_BIND_KDREAMS_FUNCKEYS},
	{"Up",         BE_ST_CTRL_BIND_KDREAMS_UP},
	{"Down",       BE_ST_CTRL_BIND_KDREAMS_DOWN},
	{"Left",       BE_ST_CTRL_BIND_KDREAMS_LEFT},
	{"Right",      BE_ST_CTRL_BIND_KDREAMS_RIGHT},
};
#define KL_NROWS ((int)(sizeof(kl_rows) / sizeof(kl_rows[0])))

static const char *kl_pad_name(int pad)
{
	if (pad < 0 || pad > 21 || !g_be_st_padFeatureIdToNameMap[pad] ||
	    !g_be_st_padFeatureIdToNameMap[pad][0])
		return "-";
	return g_be_st_padFeatureIdToNameMap[pad];
}

static void kl_draw_menu(int pos, const char *prompt)
{
	int i;
	id0_char_t line[40];

	VW_FixRefreshBuffer(); /* both pages coherent before drawing the window */
	US_CenterWindow(26, KL_NROWS + 6);
	US_CPrint("GAMEPAD CONTROLS");
	US_Print("\n");
	for (i = 0; i < KL_NROWS; i++)
	{
		snprintf(line, sizeof(line), "%c %-10s %s\n",
		         (i == pos) ? '>' : ' ', kl_rows[i].label,
		         kl_pad_name(g_refKeenCfg.kdreams.binds[kl_rows[i].bind].pad));
		US_Print(line);
	}
	US_Print("\n");
	US_CPrint(prompt);
	VW_UpdateScreen();
}

void KL_BindMenu(void)
{
	int pos = 0, done = 0;

	while (!done)
	{
		ScanCode sc;

		kl_draw_menu(pos, "Enter: rebind   Esc: done");
		IN_ClearKeysDown();
		sc = IN_WaitForKey();
		if (sc == sc_Escape)
			done = 1;
		else if (sc == sc_UpArrow)
			pos = (pos == 0) ? KL_NROWS - 1 : pos - 1;
		else if (sc == sc_DownArrow)
			pos = (pos == KL_NROWS - 1) ? 0 : pos + 1;
		else if (sc == sc_Return || sc == sc_Space)
		{
			int feat = -1;

			kl_draw_menu(pos, "Press a pad button...");
			IN_ClearKeysDown();
			/* wait for release of whatever is held, then a fresh press */
			while (BE_ST_KL_PollPadFeature() >= 0)
				BE_ST_ShortSleep();
			while (feat < 0)
			{
				BE_ST_ShortSleep(); /* sleeps, presents AND polls events */
				if (LastScan == sc_Escape)
					break;
				feat = BE_ST_KL_PollPadFeature();
			}
			if (feat >= 0)
			{
				/* SWAP with any action already holding this button */
				int other;

				for (other = 0; other < BE_ST_CTRL_BIND_KDREAMS_TOTAL; other++)
					if (other != kl_rows[pos].bind &&
					    g_refKeenCfg.kdreams.binds[other].pad == feat)
					{
						g_refKeenCfg.kdreams.binds[other].pad =
							g_refKeenCfg.kdreams.binds[kl_rows[pos].bind].pad;
						break;
					}
				g_refKeenCfg.kdreams.binds[kl_rows[pos].bind].pad = feat;
			}
			/* let go before the menu reads keys again */
			while (BE_ST_KL_PollPadFeature() >= 0)
				BE_ST_ShortSleep();
			IN_ClearKeysDown();
		}
	}
	IN_ClearKeysDown();
	/* apply immediately through the game's own mapping refresh */
	PrepareGamePlayControllerMapping();
}
