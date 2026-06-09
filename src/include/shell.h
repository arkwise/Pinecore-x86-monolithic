#ifndef SHELL_H
#define SHELL_H

/* Pinecore Shell — native kernel-mode command interpreter
 *
 * Runs as a scheduler task, uses VT system for I/O.
 * Direct FAT driver access, no V86 or DOS emulation needed.
 *
 * (ch-17)
 */

/* Entry point for sched_create_kernel_task */
void shell_entry(void);

#endif
