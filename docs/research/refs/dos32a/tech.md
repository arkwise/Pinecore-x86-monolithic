## Contents
[1.0 - DOS/32 Advanced - Overview](tech/1.md)
[2.0 - DOS/32 Advanced - Startup](tech/2.md)
[3.0 - DOS/32 Advanced - The Loader](tech/3.md)
[4.0 - DOS/32 Advanced - Exit to DOS](tech/4.md)
[5.0 - DOS/32 Advanced - Exceptions and Hardware Interrupts](tech/5.md)
[6.0 - DOS/32 Advanced - Built-in Debugger](tech/6.md)
[7.0 - DOS/32 Advanced - Memory Management](tech/7.md)
[8.0 - DOS/32 Advanced - Allocation of >64MB of memory](tech/8.md)
[9.0 - DOS/32 Advanced - Physical Memory Mapping](tech/9.md)
[10.0 - DOS/32 Advanced - Values returned by DPMI function 0A00h](tech/10.md)
[11.0 - DOS/32 Advanced - Spawning programs](tech/11.md)

**Terminology and definitions:**
ADPMI = DOS/32 Advanced built-in DPMI
DOS or Low memory = conventional memory, available under 1MB
Extended or High memory = extended memory, available above 1MB
DOS/32 Advanced or DOS/32A = the whole DOS Extender (DOS Extender + ADPMI)

Words "DOS Extender" refer to that part of DOS/32 Advanced which performs initialization and cleanup of protected mode and DPMI, and extends software interrupts INT 10h, INT 21h and INT 33h.

Words "built-in DPMI" or "ADPMI" refer to the DPMI server that is built into DOS/32 Advanced and provides INT 31h interface.

Words "external DPMI" refer to any DPMI host other than DOS/32 Advanced built-in DPMI (for example Windows DPMI).

Please remember that by definition, a DOS Extender is not necessarily includes an incorporated (built-in) DPMI host, and a DPMI host is not necessarily extends any software interrupts, but only provides an API entry point through INT 31h.

_ _ _

The intention of this document is to provide protected mode programmers and application software developers with certain technical information on how DOS/32 Advanced DOS Extender and DOS/32 Advanced built-in DPMI host operate, what system resources they use as well as how they interact with the present (underlying) system software and with 16/32-bit protected mode application. The reader should have strong skills and a good experience of protected mode programming and system programming in order to understand the topics covered by this documentation.
