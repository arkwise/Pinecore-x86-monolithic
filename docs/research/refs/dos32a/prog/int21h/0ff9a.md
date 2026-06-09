# 3.58 - DOS function 0FF9Ah - DOS/32 Advanced Allocate Selector
| **In:** | **AX** = 0FF9Ah<br> **EBX** = selector base<br> **ECX** = selector limit<br> **DX** = selector access rights |
|---|---|
| **Out:** | <br><br>if successful:<br> **CF** clear<br> **AX** = selector <br><br>if failed:<br> **CF** set |

**Notes:**

a) This call is specific to DOS/32 Advanced DOS Extender only, and is not supported by standard DOS.

b) This function allocates a descriptor and sets its base, limit and access rights to the requested values using DPMI calls.

c) Selector allocated by this function can be freed using DPMI function 0001h.
