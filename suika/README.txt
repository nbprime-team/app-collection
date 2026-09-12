Suika Prime v4 - DOOM-derived firmware input hook
===================================================

This version does NOT poll SVC #0x1003f from the game loop.
It reproduces the input-hook mechanism observed in puredoom.elf:

  firmware -> 0x307FBFA0 trampoline -> suika_event_hook()
                        -> SVC #0x1003f
                        -> parse ui_event_prime_s
                        -> volatile input state

The hook target 0x307FBFA0 and the privileged cache-flushing copy routine
are extracted from the supplied DOOM/puredoom.elf. Therefore this build is
intended for the same HP Prime firmware/environment as that DOOM binary.

Input behavior
--------------
* Touch begin/move: move the current fruit horizontally.
* Touch end: drop the current fruit.
* Any key down OR key up event: quit immediately and restore the original
  16 bytes at 0x307FBFA0.

The framebuffer is rendered off-screen (320x240x32-bit) and then copied to
LCD once per frame to reduce visible tearing/flicker.

Build in Debian / Termux
-------------------------
  make clean
  make

If Unifont is not already cached, make downloads GNU Unifont 17.0.04 and
builds a compact ASCII subset into unifont_font.c.

Output:
  suika_prime.elf

Replace the ELF loaded by the Python launcher (for example my_app.elf).
Do not test this hook build on an unrelated firmware version: the fixed
0x307FBFA0 hook address comes from the supplied DOOM binary.

Safety
------
The program saves and restores the original 16 bytes at the hook address.
If the program terminates because of a key event, main() calls
remove_input_hook() before returning.

V5 changes:
- Moved the score/NEXT HUD down slightly and moved the red playfield boundary to y=40 so the HUD no longer crosses the line.
- Added a 20-frame (about 400 ms) lock after dropping a fruit. A new fruit cannot be spawned during this interval, preventing a second immediate tap from producing an edge-stuck fruit.
- Moved ANY KEY EXIT to the left side at x=4.
- Draws ANY KEY EXIT after fruit sprites so fruits do not cover the exit hint.
