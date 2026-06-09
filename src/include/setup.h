#ifndef SETUP_H
#define SETUP_H

/* Pinecore first-boot setup — Phase 4.6.5 M4.
 *
 * Runs as a takeover screen on a VT before the shell prompt appears.
 * Currently single-screen: keyboard layout picklist. M9 expands this to
 * the full 7-screen flow (language, country, codepage, mouse, gfx, etc.).
 *
 * The setup function blocks until the user picks a layout (Enter) or
 * presses Esc to skip. On any exit path it returns; the caller (shell
 * task) then proceeds with its normal banner + prompt. */

void setup_run(int vt_num);

#endif
