#ifndef VCPI_H
#define VCPI_H

#include "types.h"

/* VCPI (Virtual Control Program Interface) 1.0
 *
 * Provides V86-to-PM switching for DOS extenders like DOS/16M.
 * Implemented as an extension to EMS (INT 67h).
 *
 * (ch-32)
 */

/* VCPI function codes (AH=DEh, AL=function) */
#define VCPI_DETECT       0xDE00
#define VCPI_GET_PM_IFACE 0xDE01
#define VCPI_GET_MAX_PHYS 0xDE02
#define VCPI_GET_FREE     0xDE03
#define VCPI_ALLOC_PAGE   0xDE04
#define VCPI_FREE_PAGE    0xDE05
#define VCPI_GET_PHYS     0xDE06
#define VCPI_READ_CR0     0xDE07
#define VCPI_GET_PIC      0xDE0A
#define VCPI_SET_PIC      0xDE0B
#define VCPI_SWITCH_PM    0xDE0C

/* EMS status codes */
#define EMS_OK            0x00
#define EMS_NOT_FOUND     0x84
#define EMS_NO_PAGES      0x88

/* Install VCPI/EMS handler — sets up INT 67h IVT entry and EMMXXXX0 signature */
void vcpi_init(void);

#endif
