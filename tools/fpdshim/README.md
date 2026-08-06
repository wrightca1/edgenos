# fpdshim — drive Intel's FocalPoint SDK against the FM6000 from our own userspace

The FM6000 analogue of `td2plus-re-gitlab/tools/sdkshim`. EdgeNOS's one *working* switch (the AS5610
Trident+) forwards packets by delegating to the vendor SDK; so does the TD2+ shim. This does the same
for the FM6000 instead of hand-replaying the forwarding plane.

## Why it's simple
`libFocalpointSDK.so` (EOS 4.16.8M, **ELF 32-bit i386**, deps only libc/libpthread/librt) imports **no
mmap/open/ioctl** — it never touches the device. All hardware access arrives via
`fmPlatformHwAccessInitialize()`, a pure setter that stores 12 args into a global table.
The real caller (`libFocalPointV2Agent.so` @0x7e8b71) passes **8 function pointers + 4 NULLs**.

## Verified on the cold M1 (2026-08-05)
- 32-bit userspace runs (kernel has IA32 emulation); `linux-gate.so.1` present.
- 32-bit runtime staged from the extracted EOS rootfs into `/tmp/lib32`:
  `ld-linux.so.2 libc.so.6 libpthread.so.0 librt.so.1 libdl.so.2 libm.so.6` + `libFocalpointSDK.so`.
- **`dlopen` works**; symbols resolve at the addresses the disassembly predicted:
  `fmPlatformHwAccessInitialize=0x232687  fm6000BootSwitch=0x3c8dd8  fmPlatformReadCSR=0x23496c`.
- **`fmPlatformHwAccessInitialize(cb0..cb7, NULL,NULL,NULL,NULL)` accepts our callbacks and returns.**
- `fmPlatformReadCSR` then **segfaults** → it is not a thin wrapper; it needs SDK global state, so
  `fmPlatformInitialize()` (and probably `fmInitialize()`) must run first. **That is the next step.**

## Build (no 32-bit headers needed on the host)
Declare libc entry points by hand, compile `-nostdinc`, supply our own crt1 `_start` that calls
`__libc_start_main` (a naive `_start` segfaults), and **dlopen the SDK — do NOT link it directly**
(direct linking segfaults during load-time init).
```
gcc -m32 -nostdinc -fno-builtin -fno-stack-protector -O1 -c fpdshim.c -o fpdshim.o
ld -m elf_i386 -dynamic-linker /tmp/lib32/ld-linux.so.2 -L lib32 -rpath-link lib32 -rpath /tmp/lib32 \
   -o fpdshim fpdshim.o -l:libc.so.6 -l:libdl.so.2
```
Run: `LD_LIBRARY_PATH=/tmp/lib32 ./fpdshim` (watchdog armed + petting first).
Debug tip: stdout is buffered and lost on a crash — use the `mark()` write(2) markers.

## Progress 2026-08-05 (later)
Full init chain succeeds on the cold chip:
```
fmOSInitialize()            rc=0
fmPlatformInitialize(&nsw)  rc=0   numSwitches=0    <-- blocker
fmInitialize()              rc=0
fmPlatformReadCSR(0,...)    -> ERROR:...fmCaptureLock:689:Attempted to lock an uninitialized lock
```
The SDK emits its own formatted log messages through our process — it is genuinely running.
`numSwitches=0` means no per-switch state (incl. switch 0's lock) was created, which is exactly why the
accessor faults.

**Signature gotcha:** `fmPlatformInitialize` is `(fm_int *numSwitches)`, an out-param
(disasm 0x2329c7: `mov 0x8(%ebp),%eax ; mov %edx,(%eax)`). Calling it as `(void)` segfaults.
Check every SDK prototype against the disassembly.

**Next:** call `fmPlatformConfigure` (exported, not yet used) before `fmPlatformInitialize` so the platform
reports >=1 switch; then re-test the accessor, then `fmSetSwitchState`/`fm6000BootSwitch(0)`.

## Blocker located precisely: `fmPlatformConfig`
`fmPlatformInitialize` takes the switch count from `*(int*)fmPlatformConfig`. Proven by relocation:
`00604108 R_386_GLOB_DAT 0060f480 fmPlatformConfig` (the GOT slot at ebx-0x104).
`fmPlatformConfig` is a **6708-byte GLOBAL OBJECT in .bss**, zero-filled at load → numSwitches = 0 →
no per-switch lock → `fmPlatformReadCSR` faults in `fmCaptureLock`.

`fmPlatformConfigure(sw)` does NOT initialise it — it is a 5-instruction accessor returning
`&fmPlatformConfig[sw]` (1024 bytes/switch).

Forcing `((u32*)fmPlatformConfig)[0] = 1` gets *further* into `fmPlatformInitialize` and then segfaults —
the rest of the per-switch config (port maps, board data) is still zeros.

**Next:** find EOS's writer of `fmPlatformConfig` — likely `libFocalPointV2Agent.so`, which is also the
caller of `fmPlatformHwAccessInitialize` (@0x7e8b71). Fastest shortcut: gdb-attach a live EOS
`FocalPointV2` and dump the populated `fmPlatformConfig` blob, then replay it into the shim.

**PIC-base tip:** `get_pc_thunk` returns the address of the instruction *after* the call. Use that when
computing `%ebx`; verify by checking a known string resolves (e.g. `SIMULATION_VMID` in
`fmPlatformInitialize`).
