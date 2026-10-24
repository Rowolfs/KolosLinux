# Graphics IPC v2 smoke protocol

This is a private Android–Linux AF_UNIX SOCK_STREAM protocol. Every frame begins
with four **little-endian uint32** values: magic `0x3158474b` (KGX1), version `2`,
opcode and payload byte length. The payload is at most 4096 bytes. Fields below
are encoded explicitly in the listed order; C++ padding is never transmitted.
An identity is `uint64 sessionId, uint64 poolGeneration, uint32 slot`.

| Opcode | Payload |
| --- | --- |
| HELLO / HELLO_ACK | empty |
| SESSION_INFO | uint64 sessionId, uint64 generation, uint32 width, height, AHB format, slot count |
| GET_BUFFER | identity |
| BUFFER_INFO | identity, uint32 width, height, AHB format, flags, stride. Bit 0 of flags means stride is known. Other layout fields are absent. |
| IMPORT_ACK | identity |
| START / STOP | uint64 sessionId, uint64 generation |
| FREE | identity |
| PRESENT / ACK | identity, uint64 serial |
| DONE | uint64 sessionId, uint64 generation, uint64 completed frames |
| ERROR | uint32 error code followed by UTF-8 diagnostic bytes |
| POOL_REPLACED | Same fields as SESSION_INFO; Android announces a new current generation. |
| POOL_READY | uint64 sessionId, uint64 generation; Linux has imported every new slot. |

The first connection is the control stream: HELLO, HELLO_ACK, SESSION_INFO.
Each GET_BUFFER uses a separate connection to the same socket. Android sends
BUFFER_INFO, then `AHardwareBuffer_sendHandleToUnixSocket` bytes and SCM_RIGHTS
FDs, then shuts down its write side. Linux receives the handle to EOF, imports
it, sends IMPORT_ACK and closes that connection. This preserves the tested
native-handle transfer boundary without inventing a handle framing format.
After all imports, Linux sends START on control and receives one initial FREE
per slot. Only the control stream carries PRESENT/ACK/FREE/STOP/DONE.

ACK confirms a PRESENT transaction; **only FREE permits reacquisition**. On
replacement, Android waits for the previous SurfaceControl release fence before
FREE(previous). On STOP it detaches the final buffer, emits its FREE after
release, then DONE. All control writes are serialized by the Android IPC thread.

For live pool replacement, Android sends POOL_REPLACED on the existing control
connection. Linux repeats GET_BUFFER / BUFFER_INFO / handle / IMPORT_ACK for
each new slot on fresh import sockets, then sends POOL_READY. Android responds
with one initial FREE for each new slot. Linux makes the new generation current
only after all three imports and the POOL_READY write succeed. Older FREE
messages retain their generation and never enter the current acquire queue.
An old PRESENT already sent when POOL_REPLACED arrives is discarded by Android;
Linux retries that frame on the new generation. The last old displayed AHB is
released by the first new SurfaceControl transaction and its release fence.

The smoke importer still recognizes only the previously observed Qualcomm
linear RGBA8 handle family: 148 bytes, two FDs, known handle field signatures,
single plane, modifier hypothesis 0 and offset 0. It validates reported
width/height/stride against native handle fields and allocation size before
Vulkan import. New dimensions are attempted, not claimed validated; unsupported
layout fails closed. No Android DRM modifier, offset or allocation size is
invented or placed on the wire.
