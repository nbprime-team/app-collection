# PureDOOM input hook ABI used by Suika v4

Derived from the supplied `puredoom.elf` disassembly.

## Fixed firmware hook target

`install_input_hack()` saves 16 bytes at:

```text
0x307FBFA0
```

Then it writes 8 bytes:

```text
e51ff004
<hook address>
```

which is equivalent to:

```asm
ldr pc, [pc, #-4]
.word hook_address
```

The write uses the same privileged byte-copy/cache-flush routine as the
original DOOM binary.

## Event syscall

The hook receives an event pointer in `R0`, then calls:

```asm
svc #0x1003f
```

The relevant `ui_event_prime_s` layout is:

```text
+0x04  event_type          uint32
+0x18  available_events    uint16
+0x1C  event[0]            12-byte record
```

Each record:

```text
+0x00  action/type         uint16
+0x04  validity field      uint16  (DOOM accepts zero)
+0x06  x                   uint16
+0x08  y                   uint16
```

Touch values used by DOOM:

```text
1 = begin
2 = move
8 = end
```

For keyboard events DOOM recognizes outer event type:

```text
0x00100010
```

and the first record at +0x1C contains the key action.

Suika v4 deliberately exits on any key down or key up.
