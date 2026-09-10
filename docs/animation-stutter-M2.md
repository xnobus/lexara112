# M2 animation stutter — cause and fix

**Status: CLOSED.** The symptom is gone and the fonts work.

## Symptom

While walking, the animations of every M2 model stuttered — player characters and
NPCs alike. Standing still, with nothing but camera rotation, everything was fine.
The world, the terrain and the interface behaved normally; only animated models
stuttered.

The key point: **this was never a drop in framerate.** The measured frame time
stayed at 16.67 ms throughout, peaking at 18–19 ms, with zero frames above 20 ms.

## Cause

To install a hook on `IDirect3DDevice9::EndScene`, the port needed the address of
the device's virtual table. It obtained it by creating **its own throwaway 8x8 D3D9
device together with a temporary window**, reading the table pointer out of it and
releasing it.

That alone was enough to upset animation across the whole client.

A second D3D9 device was being created on DXVK before the client created its own.
The client then got a device in a different driver state than usual. Frame time
stayed the same, so this was never a cost — it was a change of conditions.

## How it was established

Bisection through `lexara112.cfg` — the same technique that works elsewhere —
**could only produce false negatives here.** Every config that touched the font
path called `D3D::GetDevice()`, and `GetDevice()` created the stand-in device. So
the symptom tracked the mere FACT of touching that path, not what the path did.
Config N was not clean because it disabled six text geometry hooks, but because
with it nothing called `GetDevice()`. Series N, S, T and W therefore spent thirty
trips into the game measuring one and the same event.

What settled it was a ladder descending to zero, each rung its own trip into the
game:

| state | result |
|---|---|
| no `lexara112.dll` in `dlls.txt` | clean |
| DLL loaded, zero code per frame | clean |
| MSDF renderer enabled | stutters |
| renderer disabled, only the draw and shader captures | stutters |
| renderer disabled, no captures, only `hkEndSceneShared` once per frame | stutters |
| **as above, but without the stand-in device** | **clean** |

The last row isolates the cause: in that state NOTHING of ours runs during the
frame, and the only difference from the row above is the one-off creation and
release of the stand-in device at start-up.

## Fix

The stand-in device is **not created**. It stayed in the code behind
`shared_vtable=1`, for diagnostics only (`AcquireSharedDeviceVtable` in `D3D.cpp`).

The client's device is caught through a swapped `CreateDevice` entry in the
`IDirect3D9` virtual table. That required two things:

1. **A guard over the `vtbl[16]` entry** (`VtableGuardThread` in `D3D.cpp`).
   Something restores DXVK's original entry right after our swap, and the client
   creates its device in exactly that window — which is why `hkCreateDevice` never
   fired for it once. The guard reclaims the entry in a tight loop for the first
   few seconds and **exits as soon as the device is caught**. When a foreign entry
   is found in place, it becomes our original, so that mod's hook chain keeps
   working.
2. **`Direct3DCreate9Ex` covered** — the second entry point into d3d9, previously
   left unhooked.

## Pitfalls that cost trips into the game along the way

- **A vtable entry swap only catches the client's device if it is done AFTER the
  device has been created.** Installed early it never caught it once. It used to
  work by accident, because the capture was lazy and came from the font path.
- **A detour on the BODY of a DXVK device function installs cleanly and never
  fires**, if installed early. `commit=0`, correct addresses, zero calls. The same
  detours installed late, from the rendering thread, do work.
- **`DetourTransactionCommit` without a status check** — a silent failure to
  install hooks looks exactly like an absence of calls. The status is now logged.
- **After `Hooks::Detour` the "original" variable holds the TRAMPOLINE**, not the
  export address. Comparing it against `GetProcAddress` always fails and produces
  false alarms about a "second module".
- **The metric "the model pose has not changed" does not describe this symptom.**
  The client uploads bone matrices (`vs c10..c135`) before every model draw, so
  their content is a direct read of the pose. With the renderer disabled the
  average was 49% changed writes; with it enabled, 33–42% — the ranges overlap.
  A pose that holds still every other frame is normal behaviour for this client,
  not a symptom.
- **The measuring instrument itself triggered the symptom.** The probe that
  produced those numbers needed the stand-in device to work - i.e. exactly the
  thing that was the cause. Several "reference runs" were contaminated by this and
  were useless for comparison. Once the cause was established the probe was
  removed.

## Fixes made along the way

- **Constant registers moved above the client's range.** Measurement showed the
  client writes across `vs c2..c198`. Our hook wrote to `c0..c3` (WorldViewProj,
  assigned by default by the HLSL compiler) and `c23` (control), colliding on `c2`,
  `c3` and `c23` — about 190 overwrites per frame. `control` now sits in `c220` and
  `WorldViewProj` in `c240`; the values must match the `register(cNN)` declarations
  in `MSDFShaders.h`. The headroom above `c198` is small and `ps_3_0` ends at
  `c223` — if the client ever reaches higher, `control` has to come out of the
  constant registers rather than be moved further up.
- **`vtbl[20]` is touched only after `IDirect3D9Ex` has been confirmed.** DXVK
  returns an Ex object from `Direct3DCreate9` and it was safe there, but the
  Windows `d3d9.dll` returns a plain `IDirect3D9` whose table ends at slot 16 —
  writing to `vtbl[20]` there was a write past the end of the table, over someone
  else's data in `.rdata`.
- **`IsD3D9Name` also recognises a forward slash**, because the `dxvk` entry in
  `dlls.txt` can spell the path as `dxvk/d3d9.dll`.

## Known consequences

- **The CTRL+ALT+F shortcut does not work.** Its handler lives in
  `hkEndSceneShared`, which is installed by swapping `vtbl[42]` — and that requires
  the stand-in device. Toggling the renderer remains available through
  `msdf_enabled` in `lexara112.cfg`.
- **`shared_vtable=1` brings the stand-in device back**, and the stutter with it.
  It was kept only in case the `CreateDevice` capture fails somewhere and a
  comparison is needed.
