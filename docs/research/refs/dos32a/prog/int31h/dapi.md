# 2.52 - DOS/32 Advanced vendor-specific DPMI functions
The entry point to the vendor-specific DPMI functions can be obtained through the DPMI call 0A00h with DS register containing the selector of the ID-string and ESI register containing the offset. The entry point code selector will be returned in ES and the offset of the entry point routine will be placed in EDI. To access vendor-specific DPMI API functions you must issue a far call to the address pointed to by ES:EDI.

The ASCIIZ ID-string for DOS/4G extensions is: "RATIONAL DOS/4G",\0 (Please note that DOS/4G API extensions have not been implemented in this version of DOS/32 Advanced DOS Extender. Any call to DOS/4G API, ie to the address returned in ES:EDI will terminate the program immediately and display an error message on the screen.)

The ASCIIZ ID-string for DOS/32 Advanced extensions is: "SUNSYS DOS/32A",\0 To call DOS/32 Advanced DPMI API functions you must place the function number in AL register. On return the carry flag will be cleared, if the function was successful. Otherwise the carry flag will be set and no registers will be modified. Please note that none of API functions will perform any kind of action on the system, instead they will simply return pointers and variables from the DPMI kernel code that can be altered by the user himself.

**WARNING**: The following functions will give the user an access to the internal variables inside DOS/32 Advanced DPMI kernel. Setting these variables to improper values may cause the DOS Extender to malfunction and/or result in loss of data!
