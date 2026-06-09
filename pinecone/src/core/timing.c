#include "core/timing.h"

#define CYCLES_PER_MS  2500000ULL

unsigned long long rdtsc(void)
{
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

void poll_delay_ms(unsigned ms)
{
    unsigned long long t = rdtsc() + (unsigned long long)ms * CYCLES_PER_MS;
    while (rdtsc() < t) { }
}

unsigned long ms_since_boot(void)
{
    static unsigned long long boot_tsc = 0;
    if (!boot_tsc) boot_tsc = rdtsc();
    return (unsigned long)((rdtsc() - boot_tsc) / CYCLES_PER_MS);
}
