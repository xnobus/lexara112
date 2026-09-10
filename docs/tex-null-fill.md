# The `site_texnullfill` patch - crash 0x00448955 on a missing texture

Applies to the **Turtle WoW 1.12 / `WoW_Modernized.exe` (build 5875)** client.
It has nothing to do with the MSDF renderer - it is installed independently of
`msdf_enabled`.

## Symptom

`ERROR #132`, `0xC0000005 (ACCESS_VIOLATION)` at `0x00448955`, a write to
`0x00000000`. The registers are **always the same**, because the path is
deterministic:

```
EAX=FFFFFFFF EBX=00000008 ECX=00000002 EDX=00000001 ESI=00000002 EDI=00000000
```

Stack: `EvtSched.cpp` -> `CSimpleTop.cpp` -> `CGxDevice.cpp` -> `CGxD3dTexture.cpp`
-> `Texture.cpp`. **No Lua frame at all**, so it looks like a renderer crash - and
it is really a path to a file that does not exist, handed over earlier from Lua
through `SetTexture`. Loading is deferred until drawing, so the crash lands a frame
after whatever caused it, and the culprit is not visible on the stack.

## Cause

The texture loading callback at `0x0044A260`:

```
0044A2AF  mov eax, [ebp+14h]      ; the texture object
0044A2B2  mov edx, [0B05D14h]     ; the global placeholder bits table
0044A2C0  lea ecx, [eax+120h]
0044A2C8  mov [ecx], edx          ; obj->mipBits = the placeholder table
0044A2D2  lea esi, [eax+0Ch]      ; obj->filename
0044A2D9  mov ecx, esi
0044A2DB  call 004491F0           ; load the file
0044A2E0  test eax, eax
0044A2E2  jne 0044A306            ; success
0044A2E4  mov edx, [ebp+8]
0044A2E7  push 1
0044A2E9  mov ecx, edi
0044A2EB  call 00448920           ; FAILURE -> "fill with white"
```

`0x004491F0` returns 0 in two ways, both through `0x005A3660`:

- an empty path - `cmp byte ptr [edi],0`, `SetLastError(57h)`, `xor eax,eax`;
- a failed file open - `0x005A369D`: `test eax,eax` / `jne`, and the error branch
  exits with `eax` already zeroed.

**An open question.** The function that creates the texture (`0x0044A140`) opens
the file right at its entry (`0x0044A164 call 005A3660`) and on failure returns 0
without creating an object - so by that code a missing file should NEVER reach the
callback at all. And yet it does: the reproduction below rests precisely on paths
to files that do not exist. So either the object is created somewhere else as well,
or the name is resolved differently at creation than at load time. Not established.

The registers at the moment of the crash say the mipmap level that broke was
**2x1** (`ESI=2`, `EDX=1`, `EBX=8` = 2*1*4). A 256x32 chain (bars) and a 256x128
one (`logo.tga`) come down to exactly 2x1, and both are `.tga`, i.e. files without
mipmaps. A lead, not a finding.

`0x00448920` walks the mipmap chain of the placeholder table and fills it with
`0FFFFFFFFh`. The table exists, but **its entries are NULL** - the placeholder
buffer is never allocated in this client:

```
00448936  cmp edx, 1              ; loop over levels
00448939  ja  00448940
0044893B  cmp esi, 1
0044893E  jbe 00448985            ; exit
00448940  mov edi, [ebp-4]        ; next table entry
00448943  mov edi, [edi]          ; <-- NULL
00448945  mov ecx, esi
00448947  imul ecx, edx
0044894A  shl  ecx, 2
0044894D  mov ebx, ecx            ; size in bytes
0044894F  shr  ecx, 2             ; size in dwords
00448952  or  eax, 0FFFFFFFFh     ; <-- PATCH SITE (5 bytes)
00448955  rep stosd               ; <-- CRASH
00448957  mov ecx, ebx            ; <-- return here when edi != 0
00448959  and ecx, 3
0044895C  rep stosb               ; a second write, also through NULL
0044895E  mov edi, [ebp-4]        ; <-- return here when edi == 0
```

## The patch

`src/font_exact/TexNullFill.cpp`. A detour at `0x00448952` occupying exactly
5 bytes (`83 C8 FF` + `F3 AB`); both instructions are complete and the site is on
an instruction boundary at both ends.

```asm
or   eax, 0FFFFFFFFh
cmp  edi, 10000h
jb   no_buffer         ; NULL and junk from the first page of the process
cmp  edi, 7FFFFFFFh
ja   no_buffer         ; -1 and kernel addresses
rep  stosd
jmp  00448957          ; the normal route - the rest of the original finishes stosb
no_buffer:
jmp  0044895E          ; skips BOTH writes
```

**The guard checks a range, not just zero - and that is the fix from 2026-09-09
21:50.** The first version had `test edi,edi / jz` and survived exactly one
session: `Errors\2026-09-09 21.39.32 Crash.txt` is a crash INSIDE this very stub.
The registers were as always on this path (`EAX=FFFFFFFF EBX=8 ECX=2 EDX=1
ESI=2`), but `EDI=FFFFFFFF` instead of `0`, the faulting address equal to `EDI`,
EIP inside `lexara112.dll`, and the frame below it `0044A2F0`, i.e. the return from
`call 00448920`. So an entry in the placeholder table can be not only NULL but also
the `-1` sentinel; the assumption "the entries are NULL" in the Cause section is
incomplete.

Both bounds are `cmp`, i.e. they touch only EFLAGS. The code at `00448957` reads
`ecx` and `ebx`, not the flags left by `or`, so the live registers stay untouched.

**Two return addresses, not one.** The second write (`rep stosb` at `0044895C`)
would fault just the same, so on NULL the jump has to go straight to `0044895E`.

Skipping a level is safe: the loop recomputes `ecx` (`00448945`) and `ebx` from
scratch, and takes `edi` again from `[ebp-4]` (`0044895E`). No state passes between
iterations in the registers the patch leaves behind.

Before installing, the patch compares the bytes at `0x00448952` with the expected
`83 C8 FF F3 AB` and on a mismatch **refuses** - otherwise, after a swap of
`WoW_Modernized.exe`, it would write a jump into the middle of something else.

## Result

A failed texture load ends in missing artwork instead of a dead process.
**This fixes the error handling, not the cause** - a path to a file that does not
exist is still a bug on the addon's side.

## Verification - CONFIRMED IN GAME 2026-09-09

In `lexara112.log` at start-up:

```
[MSDF] site_texnullfill: the 0x00448955 crash patch is installed
```

`commit = 0` from Detours means only "no error", NOT "the hook works", and the log
line itself says only that the patch was installed. What settles it is a test with
a control:

| `site_texnullfill` | result |
|---|---|
| `0` | crash at `0x00448955`, registers identical to the 19:33 and 19:41 sessions (`Errors\2026-09-09 20.31.18 Crash.txt`) |
| `1` | no crash, same action |

**A `/run` probe with `SetTexture` on a dead path does NOT reproduce this crash** -
verified, the client survives even with the patch disabled. Reproducing it requires
a path stored in an addon's database plus a repaint of the row that draws it: in
WeakestAuras this is a group's `groupIcon` and **collapsing that group** in the aura
list (`Regions.lua`, `groupModifyThumbnail`).

The methodological lesson: a probe without a control of known outcome is not a
probe. The first version of this probe did not crash the client, and that looked
like a working patch - it was simply code that never touched the patched site.

## Disabling it

In `lexara112.cfg` next to the client:

```
site_texnullfill=0
```
