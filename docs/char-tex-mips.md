# The `site_chartexmips` patch - crash 0x0047801D on an oversized character texture

Applies to the **Turtle WoW 1.12 / `WoW_Modernized.exe` (build 5875)** client.
Like `site_texnullfill`, it has nothing to do with the MSDF renderer and is
installed independently of `msdf_enabled`.

## Symptom

`ERROR #132`, `0xC0000005 (ACCESS_VIOLATION)` at `0x0047801D`, a write to an
address that looks like two 16-bit pixels (`0xAD54AD54` in the first report). A few
seconds after entering the world - when the client builds the skin textures of the
player and the characters around them. No lexara112.dll frame on the stack:

```
0047801D  mov word ptr [eax], cx       ; EAX = AD54AD54
004774E6  <- 00477460 (dispatcher)
00477160  <- 004770F0 (one body region)
00477350  <- 004772F0 (region 3, upper torso)
00524D6F  <- 00524CD0 (queue of characters to re-texture)
0048322D  <- world render
```

First report: Nobus, `Errors\2026-09-11 21.32.58 Crash.txt` + `.dmp`. He crashed
with lexara112.dll loaded and **not** without it - see "Why it depends on our DLL".

## Cause

The client composites a character's skin on the CPU into one **256x256 RGB565
canvas**, allocated at `0x00476183` into `[0x00B422D8]`: a table of **9** level
pointers (256 .. 1) followed immediately by the pixels. Every body region (base
skin, arms, torso, legs, the guild tabard...) is pasted into it by `0x00477460`,
which picks one of three loops by blend mode:

```
004774B2  call 00477C80     ; mode 0 - copy
004774D6  call 00477DB0     ; mode 1
004774E1  call 00477F20     ; mode 2 - 2-bit alpha (the crash)
```

All three walk the mip chain bounded by the **source's** level count and never
the canvas's:

```
00477F40  mov  dword ptr [ebp-8], 200h   ; canvas pitch at level 0 = 256 px
...
00478139  mov  esi, [ecx+0Ch]            ; info->mipCount (source)
0047813E  inc  eax
00478142  cmp  eax, esi
0047814D  jb   00477F6F                  ; next level
```

In vanilla no component is larger than the canvas, so no source has more than 9
levels and the bug never fires. In the dump the source was
`Textures\GuildEmblems\Background_45_TU_U` - the guild tabard background for the
upper torso - at **512x256, 10 levels** (its mip table at `A2120008`, level 0
exactly `0x48000` bytes = 512x256 x 2.25 B of RGB565 + 2-bit alpha, stack locals
`[ebp-0x28]` = level 9, `[ebp-8]` = canvas pitch 1). On level 9 the loop reads
`canvas_table[9]`, which is the first 4 bytes of the canvas's level 0 - two skin
pixels, `AD54 AD54` - and writes through them.

Level 0 is NOT an overflow: the loops copy the region's size (128x64 for the upper
torso) with the source's pitch, so the oversized texture lands cropped, in bounds.
The only bad write is the one on the extra level.

## Why it depends on our DLL

The target of the stray write is the character's skin colour, not memory the
client owns. The process is large-address-aware (the dump has live heap at
`0x98xxxxxx`-`0xA2xxxxxx`), so `0xAD54AD54` is an ordinary user-mode address: if
something happens to be mapped there, 2 bytes of it are silently overwritten; if
not, ACCESS_VIOLATION. Loading lexara112.dll (atlas, shader compiler, D3DX)
moves the rest of the address space around, and in Nobus's case that left the
address unmapped. The bug is the client's and fires either way.

## The patch

`src/font_exact/CharTexMips.cpp`. A Detours hook on the dispatcher `0x00477460`
(`__thiscall`, 6 stack arguments, `ret 18h`) that caps `info->mipCount` (6th
argument, offset `+0xC`) to the canvas's 9 before calling the original. One hook
covers all three loops - the dispatcher is their only caller, and it is called only
from `0x004770D7` and `0x0047715B`.

`info` is a 6-dword stack copy that both callers fill through `0x0047B150`
(`rep movsd` from the texture's cached description) and drop right after the call,
so the texture's own description is not touched.

Levels 0-8 are pasted as before; only the level the canvas does not have is
skipped.

Before installing, the patch checks the bytes everything above rests on and
**refuses** on any mismatch (another build, or another DLL that already patched
this path - e.g. a skin-resolution patch, for which a fixed cap of 9 would be
wrong):

| address | bytes | what |
|---|---|---|
| `00477460` | `55 8B EC 83 EC 18` | dispatcher prologue, the Detours site |
| `004774B2` / `004774D6` / `004774E1` | `E8 ...` | the calls to the three loops |
| `00477C9E` / `00477DCE` / `00477F40` | `C7 45 F8 00 02 00 00` | canvas pitch `200h` in each loop |
| `00477D90` / `00477EFC` / `00478139` | `8B xx 0C` | each loop's bound is `info->mipCount` |

No branch in the binary targets the interior of the 6 bytes Detours overwrites
(scanned: only the two `call 00477460`).

## Verification

In `lexara112.log` at start-up:

```
[MSDF] site_chartexmips: the 0x0047801D crash patch is installed
```

As with every patch here, that line only says the hook was installed. What
proves it works is the line from inside the hook, printed when a character with an
oversized component is re-textured:

```
[MSDF] site_chartexmips: component 512x256 has 10 mip levels, the skin canvas has 9 - capped (mode 2) ...
```

together with no crash on the setup that crashed before (Nobus: his client +
the guild tabard, lexara112.dll loaded), and the crash coming back with
`site_chartexmips=0`.

## Disabling it

In `lexara112.cfg` next to the client:

```
site_chartexmips=0
```
