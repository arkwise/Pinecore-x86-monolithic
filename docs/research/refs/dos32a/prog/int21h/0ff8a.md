# 3.47 - DOS function 0FF8Ah - DOS/32 Advanced Get ADPMI Configuration Info
| **In:** | **AX** = 0FF8Ah |
|---|---|
| **Out:** | <br><br>**EAX** = 49443332h ('ID32') (if the function is supported)<br> **EBX** = DOS/32 Advanced version number<br> **CL** = CPU type <br><br>03h = 80386<br> 04h = 80486<br> 05h = 80586 (Pentium)<br> 06h = 80686 (Pentium Pro or Pentium II)<br> 07h-0FFh = reserved <br><br>**CH** = System software type: <br><br>00h = Clean<br> 01h = XMS<br> 02h = VCPI<br> 03h = DPMI <br><br>**DL** = ADPMI Kernel configuration bits<br> **ESI** = pointer to DOS/32 Advanced ADPMI Configuration Header<br> **FS** = selector that can be used to access ADPMI Configuration Header |

**Notes: **

a) This call is specific to DOS/32 Advanced DOS Extender only, and is not supported by standard DOS.

Additional information about value returned in DL register and about DOS/32 Advanced DPMI Kernel configuration can be found in the document "DOS/32 Advanced Technical Reference".
