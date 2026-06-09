# 2.63 - DPMI error codes
If a DPMI function was unsuccessful, carry flag (CF) will be set and an error code will be placed in register AX. The following is a list of error codes returned by a DPMI host.

**8001h** - Unsupported function
Returned in response to any function call which is not implemented by this host, because the requested function is either undefined or optional.

**8002h** - Invalid state
Some object is in the wrong state for the requested operation.

**8003h** - System integrity
The requested operation would endanger system integrity, eg, a request to map linear addresses onto system code or data.

**8004h** - Deadlock
Host detected a deadlock situation.

**8005h** - Request canceled
A pending serialization request was canceled.

**8010h** - Resource Unavailable
The DPMI host cannot allocate internal resources to complete an operation.

**8011h** - Descriptor unavailable
Host is unable to allocate a descriptor.

**8012h** - Linear memory unavailable
Host is unable to allocate the required linear memory.

**8013h** - Physical memory unavailable
Host is unable to allocate the required physical memory.

**8014h** - Backing store unavailable
Host is unable to allocate the required backing store.

**8015h** - Callback unavailable
Host is unable to allocate the required callback address.

**8016h** - Handle unavailable
Host is unable to allocate the required handle.

**8017h** - Lock count exceeded
A locking operation exceeds the maximum count maintained by the host.

**8018h** - Resource owned exclusively
A request for serialization of a shared memory block could not be satisfied because it is already serialized exclusively by another client.

**8019h** - Resource owned shared
A request for exclusive serialization of a shared memory block could not be satisfied because it is already serialized shared by another client.

**8021h** - Invalid value
A numeric or flag parameter has an invalid value.

**8022h** - Invalid selector
A selector does not correspond to a valid descriptor.

**8023h** - Invalid handle
A handle parameter is invalid.

**8024h** - Invalid callback
A callback parameter is invalid.

**8025h** - Invalid linear address
A linear address range (either supplied as a parameter or implied by the call) is invalid.

**8026h** - Invalid request
The request is not supported by the underlying hardware.
