/*
 * core/timing.h — RDTSC-based timing helpers
 *
 * Used in place of Allegro's PIT-driven timer because PIT delivery
 * to PM clients was the s37 stall investigation; RDTSC is unprivileged
 * at CPL=3 and always works.
 */
#ifndef PINECONE_CORE_TIMING_H
#define PINECONE_CORE_TIMING_H

unsigned long long rdtsc(void);
void               poll_delay_ms(unsigned ms);
unsigned long      ms_since_boot(void);

#endif
