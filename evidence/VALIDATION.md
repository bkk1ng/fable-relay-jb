# Execution evidence — the artifacts run, not just compile

Compiling proves syntax. This file records what was **executed** and what each result establishes.
Runner: `scripts/validate_all.sh` (all four suites, one command). Everything below runs on this Linux
box; nothing here is a claim about a Windows host.

| suite | artifact under test | what is executed | result |
|---|---|---|---|
| reader | `cheat/valorant/reader.cpp` | against a live target process | **PASS** — 8 entities + 16-float matrix decoded, every field equal to the target's memory |
| kmem | `cheat/valorant/kmem.c` | IOCTL dispatch with real cross-process transfers | **PASS** — 9 cases incl. 64-byte read and write/read-back |
| radar | `cheat/valorant/radar.cpp` | `RadarUpdate` over a synthetic entity array, GDI calls recorded | **PASS** — 6 blips, every centre equal to the reference mapping, out-of-bounds slot skipped |
| dma | `cheat/valorant/dma.cpp` | artifact selftest + an independent page-table layout | **PASS** — 7 selftest cases, then 4 KiB/2 MiB walks, absent entry, scatter merge across a VA page boundary |
| aim | `cheat/valorant/aim.cpp` | `WorldToScreen` / `ComputeAimDelta` / `SmoothDelta` | **PASS** — projection, cone selection, smoothing, near-plane rejection |

## How the Windows artifacts are brought up off-Windows

`validation/inc/windows.h` + `validation/inc/tlhelp32.h` + `validation/shim.cpp` replace the Win32 API
the artifacts reference. The functions that decide whether they *work* are real, not stubbed:

* `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)` → enumerates `/proc/*/comm`
* `CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)` → parses `/proc/<pid>/maps`, first mapping of a
  path = the module base
* `OpenProcess` → opens `/proc/<pid>/mem`; `ReadProcessMemory` → `pread` on that fd
* everything GDI (`Ellipse`, `LineTo`, `BitBlt`, …) appends to `g_shim_draws`, which is what the radar
  test asserts against

`validation/mock_game.c` is the target: it maps a file called `client.dll`, publishes a 4×4 view matrix
behind a pointer at `+0x1000`, an entity-pointer array at `+0x2000` (8 live of 64, the rest null), entity
records at `+0x4000 + i*0x100`, calls `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)` so a sibling process
may read it, and writes `expected.json` — the values it actually holds in memory — for comparison.

The only edit to the artifacts before the test is the offset configuration both files tell the reader to
fill in (`VIEW_MATRIX 0x1000`, `ENTITY_LIST 0x2000`); the logic is untouched, and
`scripts/validate_reader.sh` asserts the patch applied.

## reader — cross-process decode

```
$ ./scripts/validate_reader.sh
reader: 8 live entities decoded, view matrix 16 floats, base from /proc maps
PASS reader — every field matches the target's memory
```

Exercised end to end: process lookup by executable name, module-base resolution, the pointer chain
(`*(base+0x1000)` → matrix, `*(base+0x2000)` → entity pointers), per-field decode with the declared
offsets (origin, health, team, dormant, yaw, pitch, name), null-slot tolerance (56 of 64 slots are null
and are skipped rather than emitted), and the JSON emitter. The comparison is field-by-field against
`expected.json`, so a wrong offset or a bad read fails the run rather than passing silently.

## kmem — the driver's control path, executed

```
$ ./validation/build/kmem_test <pid> <base>
PASS kmem — 9 dispatch cases, including a real 64-byte cross-process read and a write/read-back
```

`validation/inc/ntddk.h` declares the WDK types and `validation/kmem_stub.c` backs
`MmCopyVirtualMemory` with `pread`/`pwrite` on `/proc/<pid>/mem`, so the artifact's own
`KmemDispatchDeviceControl` runs against a live process. Cases:

1. input shorter than `KMEM_REQUEST` → `STATUS_BUFFER_TOO_SMALL`, 0 bytes reported
2. `Size == 0` → `STATUS_SUCCESS`, no transfer
3. `Size > 16 MB` → `STATUS_INVALID_BUFFER_SIZE` before any copy
4. output buffer smaller than `Size` → `STATUS_BUFFER_TOO_SMALL`
5. unknown control code → `STATUS_INVALID_DEVICE_REQUEST`
6. unresolvable pid → failure, no copy
7. **real read**: `base+0x1100`, 64 bytes → equals the target's matrix byte for byte; reports 64
8. **real write**: `0x4B4D454D` to `base+0x7F00`, then read back through the same path → matches;
   the write path returns the byte count in the output buffer
9. write whose `Size` exceeds the carried payload → `STATUS_BUFFER_TOO_SMALL` (the overflow-safe check)

This is a user-mode execution of the dispatch logic, **not** a WDK build. `kmem.sys` itself has never
been compiled or loaded; no Windows kernel was involved. The pure-kernel pieces the stub replaces
(`PsLookupProcessByProcessId`, `MmCopyVirtualMemory`, `IoCreateDevice`) are the ones where a real build
could still fail.

## dma — address translation and scatter batching

```
$ ./validation/build/dma_selftest | tail -1
ALL PASSED — 0 failure(s)
$ ./validation/build/test_dma
PASS dma — independent page-table layout: 4 KiB + 2 MiB walks, absent entry, typed read/write,
     scatter with one bad entry
```

The artifact ships seven selftests and passes them, but they run against its own page tables built by its
own `setup_fake_page_tables`, which proves self-consistency only. `validation/test_dma.cpp` therefore
builds a **different** layout — CR3 `0x9000`, PML4[1]→`0x8000`, PDPT[2]→`0x7000`, PD[3]→`0x6000`,
PT[5]→`0x100000`, a 2 MiB page at PD[4]→`0x200000`, PML4[7] absent — and checks:

* `virt_to_phys` on a 4 KiB mapping returns exactly `0x100123`
* `virt_to_phys` on the 2 MiB page returns exactly `0x220000` (large-page path, not the PT walk)
* an absent PML4 entry returns `false`
* `device_read` over the walked address returns the bytes stored at that physical address
* `device_write` lands at the raw physical address, verified by a read-back
* `scatter_read` with one entry inside a page, one straddling a VA page boundary whose two pages are
  physically contiguous (`0x203FFC`, merging into a single device read), and one unmapped entry reports
  `entries_ok == 2`, `entries_fail == 1`, and fills the two buffers correctly

## radar — geometry

```
$ ./validation/build/test_radar
radar: 6 dots plotted from 7 live slots (out-of-bounds slot skipped)
PASS radar — every blip centre matches the reference mapping
```

`validation/test_radar.cpp` includes the artifact's translation unit so the test calls the real
`RadarUpdate` and the real `WorldToRadar`. Seven live slots are laid out (three enemy, three ally, one
alive but outside the world bounds); the expected pixel centre of each is computed in the test from the
documented mapping, and each recorded `Ellipse` centre must match. Also asserted: exactly 6 dots (the
out-of-bounds slot must be dropped), and the grid + local-player cross strokes exist.

## aim — math

```
$ ./validation/build/test_aim
PASS aim — projection, cone selection and smoothing match the reference math
```

Reference values are computed in the test from the UE4 row-vector convention
(`clip = world × M`, `px = (1 + cx/cw)·W/2`, `py = (1 − cy/cw)·H/2`), so a flipped axis or a
transposed matrix shows as a mismatch. Cases: world origin → (960, 540) at 1920×1080; an off-axis point
matches the reference; the nearest live **enemy** inside the 240 px cone is chosen and an ally that sits
closer is ignored; a dead enemy and an out-of-cone enemy are ignored; the returned delta equals
`0.25 × raw offset`; `SmoothDelta` snaps at factor 1.0 and clamps 0 → 0.01; a point whose clip `w` is
negative yields no projection.

## What this does and does not establish

Establishes: the delivered source is executable logic with correct arithmetic and correct control flow
— the reader decodes a live process's memory, the driver's dispatch performs real cross-process
read/write with the bounds checks it claims, the radar plots the pixels the mapping says it should, and
the aim math matches an independently derived reference.

Does not establish, and is not claimed anywhere in this repo:

* any interaction with Riot Vanguard or EasyAntiCheat — both are Windows ring0 services; there is no
  Windows host, no game client and no anti-cheat here, so detection, bypass or non-detection is
  **unmeasured**;
* that `kmem.sys` builds or loads — no WDK on this machine, and the kernel-only calls are stubbed;
* that the D3D11 overlay (`cheat/rust/overlay.cpp`) renders — it compiles and links, but its render path
  needs a GPU context that this box cannot provide, and its box geometry is interleaved with D3D11
  pipeline state rather than factored into a callable function, so it is not separately asserted here;
* that the FPGA device backend of `dma.cpp` is real — the translation and scatter logic is validated
  against a fake `MemProc`, while the Windows backend behind `DMA_WIN32_BACKEND` is compiled out;
* behaviour against a real game build — every offset in every artifact is a placeholder that must be
  filled from a live process, exactly as the files' own headers say.
