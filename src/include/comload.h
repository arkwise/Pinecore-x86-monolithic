#ifndef COMLOAD_H
#define COMLOAD_H

#include "types.h"

/* COM file loader — loads a .COM executable into V86 memory
 *
 * .COM format:
 *   - No header, raw code/data
 *   - Loaded at offset 0x100 in a segment
 *   - PSP (Program Segment Prefix) at offset 0x00-0xFF
 *   - CS=DS=ES=SS=segment, IP=0x100, SP=0xFFFE
 *   - Maximum size: 64KB - 256 bytes (0xFF00)
 */

/* Set up a Memory Control Block at segment seg-1 */
void mcb_setup(uint16_t seg, uint16_t owner, uint16_t size_paras, char type);

/* Load a .COM file from FAT into V86 memory.
 * Sets up PSP, environment block, and returns the load segment.
 * Returns segment on success, 0 on failure. */
uint16_t com_load(const char *filename, const char *cmdline);

/* Set up a PSP at the given segment */
void psp_setup(uint16_t seg, uint16_t env_seg, uint16_t top_seg,
               const char *cmdline);

/* Create an environment block at the given segment.
 * Returns the segment. */
uint16_t env_setup(uint16_t seg, const char *program_path);

#endif
