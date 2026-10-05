# Stream protocol

`kolos_stream_wire.h` preserves the reference Android header. Messages are private
little-endian 16-byte records: magic `0x3154534b`, operation, slot index, serial.
They do not transport GPU fences. Slot count is fixed at three.

1. Open three sequential connections and send GET for slots 0, 1, 2.
2. Receive Android handle bytes and SCM_RIGHTS FDs until EOF on each connection.
3. Import/bind the slot, send byte `K`, then close that connection.
4. Open the control connection, send START, receive three initial FREE messages.
5. Acquire the front of the local FIFO and remove its available flag.
6. Acquire FOREIGN ownership, clear the image, release ownership to FOREIGN
   in GENERAL layout, and wait for the Vulkan GPU fence.
7. Send PRESENT(slot, frame). Process returned FREE messages until matching ACK.
8. Android sets the new AHB on SurfaceControl. Its transaction completion callback
   yields the previous buffer's release fence. Android waits before FREE(previous).
9. Send STOP after the last ACK. Android detaches and waits for the last buffer's
   release before returning DONE with the completed frame count.

FREE is the permission to reuse a slot. ACK confirms the transaction handshake
but leaves the current slot held by Android. A future pool's release of a presented
slot must be driven by FREE, not by return from present or render.

The current diagnostic is synchronous and fails if the FIFO is empty. A future
blocking acquire should process control events while waiting. Cancellation,
multiple frames in flight and explicit fence FD exchange are not introduced here.
