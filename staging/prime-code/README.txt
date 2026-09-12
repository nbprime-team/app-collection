Prime Phigros - DOOM-derived firmware input hook
================================================

This build is a small Phigros-style rhythm game for HP Prime. It renders a
four-lane chart, falling tap/flick/hold-shaped notes, a judgement line,
combo and score feedback, and a retry screen. Touch the lane when a note is
near the judgement line; any keyboard event exits and restores the hook.

The original Suika fruit physics is retained only as an unused compatibility
implementation. The active game loop uses the chart clock and note renderer.

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
* Tap notes: touch the matching lane near the judgement line.
* Flick notes: touch and release with an upward swipe near the line.
* Hold notes: touch near the line and keep holding until the long note ends.
* Touch end: restart after the chart result screen, or release a Hold note.
* Any key down OR key up event: quit immediately and restore the original
  16 bytes at 0x307FBFA0.

The framebuffer is rendered off-screen (320x240x32-bit) and then copied to
LCD once per frame to reduce visible tearing/flicker.

Build in Debian / Termux
-------------------------
  make clean
  make

When using the bundled toolchain in `armtc/`, its binaries need execute
permission. The GCC driver also needs its internal compiler directory and the
bundled assembler before the system tools:

  chmod u+x armtc/root/usr/bin/arm-none-eabi-*
  chmod u+x armtc/root/usr/lib/gcc/arm-none-eabi/14.2.1/cc1
  mkdir -p armtc-tools
  ln -sf ../armtc/root/usr/bin/arm-none-eabi-as armtc-tools/as
  ln -sf ../armtc/root/usr/bin/arm-none-eabi-ld armtc-tools/ld
  PATH="$PWD/armtc-tools:$PWD/armtc/root/usr/bin:$PWD/armtc/root/usr/lib/gcc/arm-none-eabi/14.2.1:$PATH" make

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

Phigros changes:
- Replaced the active Suika loop with a 32-note four-lane chart.
- Added tap, flick and hold-shaped note rendering, timing windows, score,
  combo, misses and retry state.
