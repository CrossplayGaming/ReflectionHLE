/* KeenLauncher quicksave for Keen Dreams: F5 saves, F9 loads, using the
 * game's OWN save machinery (SaveGame/LoadGame hooks + the US save-file
 * header) into slot 7 ("SAVEGM6.KDR"), so the quicksave also appears in the
 * control panel's load list.  Guarded by a Y/N prompt, matching the
 * CONFIRM QUICKSAVE convention of the rest of the collection -- one mis-hit
 * pad button away from wrecking a run otherwise. */

#include "kd_def.h"
#include <stdio.h>

#define KL_QS_SLOT 6	/* last of the 7 US save slots */

static const char *kl_qs_filename(void)
{
	static char fn[16];

	fn[0] = 0;
	strcat(fn, "SAVEGM");
	fn[6] = '0' + KL_QS_SLOT;
	fn[7] = 0;
	strcat(fn, "." EXTENSION);
	return fn;
}

/* the US save-file header: signature[4], present (16-bit bool), name[33],
 * one byte of struct tail padding -- byte-for-byte what USL_Ctl writes */
static int kl_qs_write_header(BE_FILE_T file)
{
	id0_char_t signature[4];
	id0_char_t name[33];
	id0_boolean_t present = true;
	id0_byte_t padding = 0;

	memset(signature, 0, sizeof(signature));
	strcpy(signature, EXTENSION);
	memset(name, 0, sizeof(name));
	strcpy(name, "QuickSave");
	return BE_Cross_writeInt8LEBuffer(file, signature, sizeof(signature)) == sizeof(signature) &&
	       BE_Cross_write_boolean_To16LE(file, &present) == 2 &&
	       BE_Cross_writeInt8LEBuffer(file, name, sizeof(name)) == sizeof(name) &&
	       BE_Cross_writeInt8LE(file, &padding) == 1;
}

static int kl_qs_skip_header(BE_FILE_T file)
{
	id0_char_t signature[4];
	id0_char_t name[33];
	id0_boolean_t present;
	id0_byte_t padding;

	return BE_Cross_readInt8LEBuffer(file, signature, sizeof(signature)) == sizeof(signature) &&
	       BE_Cross_read_boolean_From16LE(file, &present) == 2 &&
	       BE_Cross_readInt8LEBuffer(file, name, sizeof(name)) == sizeof(name) &&
	       BE_Cross_readInt8LE(file, &padding) == 1 &&
	       !strcmp(signature, EXTENSION);
}

/* a small centered prompt; returns 1 on Y/Enter, 0 on anything else.
 * The menu controller mapping is pushed while it waits, so the pad can
 * answer: A arrives as Enter (yes), B as Escape (no).  Without it these
 * prompts only heard the keyboard -- reached through the pad's function-
 * key overlay, they could not be confirmed at all. */
static int kl_qs_confirm(const id0_char_t *msg)
{
	extern BE_ST_ControllerMapping g_ingame_altcontrol_mapping_menu;
	ScanCode sc;

	{
		void KL_OverlayNext(void);
		KL_OverlayNext();
	}
	US_CenterWindow(22, 3);
	US_PrintCentered(msg);
	VW_UpdateScreen();
	BE_ST_AltControlScheme_Push();
	BE_ST_AltControlScheme_PrepareControllerMapping(&g_ingame_altcontrol_mapping_menu);
	IN_ClearKeysDown();
	sc = IN_WaitForKey();
	IN_ClearKeysDown();
	BE_ST_AltControlScheme_Pop();
	return sc == 0x15 /* Y */ || sc == sc_Return;
}

static void kl_qs_notice(const id0_char_t *msg)
{
	extern BE_ST_ControllerMapping g_ingame_altcontrol_mapping_menu;

	{
		void KL_OverlayNext(void);
		KL_OverlayNext();
	}
	US_CenterWindow(22, 3);
	US_PrintCentered(msg);
	VW_UpdateScreen();
	BE_ST_AltControlScheme_Push();
	BE_ST_AltControlScheme_PrepareControllerMapping(&g_ingame_altcontrol_mapping_menu);
	IN_ClearKeysDown();
	IN_WaitForKey();
	IN_ClearKeysDown();
	BE_ST_AltControlScheme_Pop();
}

void KL_QuickSaveLoad(int load)
{
	BE_FILE_T file;

	if (load)
	{
		file = BE_Cross_open_rewritable_for_reading(kl_qs_filename());
		if (!BE_Cross_IsFileValid(file))
		{
			kl_qs_notice("No quicksave yet.");
			return;
		}
		if (!kl_qs_confirm("Quickload? (Y/N)"))
		{
			BE_Cross_close(file);
			return;
		}
		/* The control panel brackets its load in CA_UpLevel/CA_DownLevel:
		   LoadGame marks the loaded level's graphics one cache level DOWN,
		   and CA_DownLevel's pop is what actually caches them.  Without
		   this sandwich the restored level renders as garbage tiles. */
		CA_UpLevel();
		if (kl_qs_skip_header(file) && LoadGame(file))
		{
			loadedgame = true;
			fprintf(stderr, "KL qload ok: mapon=%d\n", gamestate.mapon);
		}
		else
		{
			fprintf(stderr, "KL qload FAILED\n");
			kl_qs_notice("Quickload failed!");
		}
		CA_DownLevel();
		fflush(stderr);
		BE_Cross_close(file);
	}
	else
	{
		if (!kl_qs_confirm("Quicksave? (Y/N)"))
			return;
		file = BE_Cross_open_rewritable_for_overwriting(kl_qs_filename());
		if (!BE_Cross_IsFileValid(file))
		{
			kl_qs_notice("Quicksave failed!");
			return;
		}
		if (kl_qs_write_header(file) && SaveGame(file))
		{
			fprintf(stderr, "KL qsave ok: mapon=%d\n", gamestate.mapon);
			fflush(stderr);
			kl_qs_notice("Quicksaved.");
		}
		else
			kl_qs_notice("Quicksave failed!");
		BE_Cross_close(file);
	}
}
