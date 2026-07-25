/* KeenLauncher widescreen compositor for Keen Dreams (present-time).
 *
 * Same architecture as the Keen 1-3 port's k13_compositor: the simulation
 * and the RF refresh keep composing their vanilla EGA pages untouched (the
 * verify baseline stays green by construction).  Each RF_Refresh additionally
 * recomposes the visible world -- background tiles, sprites by priority,
 * masked foreground tiles between priorities 2 and 3 -- straight from
 * mapsegs + grsegs into a wider 8bpp buffer, which the backend presents
 * instead of the vanilla CRTC window while gameplay is live.  Menus, text
 * windows and every non-RF screen automatically fall back to the classic
 * 4:3 view.
 *
 * Config: cfg key "widescreen" (default true).  KL_WIDE=0 env disables.
 */

#ifndef KL_COMPOSITOR_H
#define KL_COMPOSITOR_H

/* No includes: id-era headers lack include guards; include kd_def.h first. */

#define KL_COMP_W 426
#define KL_COMP_H 200

/* end of RF_Refresh_EGA: recompose the wide frame from the fresh sim state */
void KL_CompRefresh(void);

/* any direct-to-page draw (VW_UpdateScreen etc): drop back to classic view */
void KL_CompStandDown(void);

/* RF_NewPosition / level starts: forget stale state */
void KL_CompReset(void);

#endif
