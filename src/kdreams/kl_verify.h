/* KeenLauncher sim-verification harness for Keen Dreams (port-only).
 *
 * Same architecture as the Keen 1-3 / Omnispeak harnesses in this
 * collection: record a play session's timing + inputs + a per-frame hash of
 * the COMPLETE simulation state, then replay it deterministically and compare
 * every frame.  A green replay proves a change touched only presentation.
 *
 * Env:
 *   KL_RECORD=<file>  record this session
 *   KL_REPLAY=<file>  replay + verify; prints "KL REPLAY OK (n frames)" or
 *                     the first mismatching frame, then exits
 *   KL_WARP=<level>   skip the control panel: NewGame straight into a level
 *                     (must match between record and replay)
 */

#ifndef KL_VERIFY_H
#define KL_VERIFY_H

/* No includes here: id-era headers lack include guards, so this header
 * relies on kd_def.h having been included first (true for every consumer). */

/* kd_play.c, right after IN_ReadControl(0,&c): record or overwrite the
 * frame's input + LastScan, and log/check the state hash */
void KL_FrameInput(CursorInfo *c);

/* id_rf.c, after RF_Refresh computes `tics`: record or force it */
void KL_FrameTics(void);

/* kd_demo.c DemoLoop entry: KL_WARP handling; returns the level to warp
 * into, or -1 for the normal title flow */
int KL_WarpLevel(void);

/* true when any harness env (KL_RECORD/KL_REPLAY/KL_WARP) is set; startup
 * key-waits are skipped in that case since nobody is at the keyboard */
int KL_HarnessActive(void);

#endif
