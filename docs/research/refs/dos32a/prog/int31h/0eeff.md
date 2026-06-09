# 2.51 - DPMI function 0EEFFh - Get DOS Extender Info
Returns information about the DOS Extender.

| **In:** | **AX** = 0EEFFh |
|---|---|
| **Out:** | <br><br>if successful:<br> **CF** clear<br> **EAX** = "D32A" (44333241h)<br> **CL** = CPU type: <br><br>03h = 80386<br> 04h = 80486<br> 05h = 80586 (Pentium)<br> 06h = 80686 (Pentium Pro)<br> 07h-0FFh = reserved <br><br>**CH** = System software type: <br><br>00h = Clean<br> 01h = XMS<br> 02h = VCPI<br> 03h = DPMI <br><br>**DL** = DOS Extender minor version (binary)<br> **DH** = DOS Extender major version (binary)<br> **ES:EBX** = selector:offset of ASCIIZ
