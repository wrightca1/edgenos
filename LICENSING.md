# Licensing

EdgeNOS is under the **MIT Licence** (see `LICENSE`). This file records the
scope of that grant, the per-file exceptions, and the obligations that come
with the third-party components EdgeNOS builds on.

SCOPE OF THIS LICENCE

The MIT terms above cover the code in this repository that EdgeNOS Contributors
wrote. They do not, and cannot, relicense third-party components that EdgeNOS
builds on or ships alongside.

Per-file exceptions, marked with SPDX identifiers in the files themselves:

  platform/arista-7050sx2-72q/scdreset.c        GPL-2.0-only

    Its SCD SMBus master was transcribed from Arista's GPL-2.0 driver
    (scd-smbus.c) -- the bitfield layout and transaction protocol, not merely
    the register addresses. Transcription produces a derivative work, so the
    licence follows it. Rewriting that section from the register map alone
    would bring the file under the MIT terms above.

Third-party components, under their own licences:

  Linux kernel, BDE-KNET, Buildroot, Quagga, FRR, glibc      GPL-2.0 / LGPL
  Broadcom OpenBCM SDK, OpenMDK, PHY firmware                Broadcom licence
  Arista scd-reset.c, scd-led.c, raven-fan-driver.c          GPL-2.0

None of those are redistributed in this repository; they are dependencies you
supply. Where an EdgeNOS *image* ships them as binaries -- FRR and glibc do get
shipped -- distributing that image carries the corresponding source obligation
of their licences, which the MIT grant above does not affect.

Register maps, port numbers and register addresses read from third-party
drivers or datasheets are facts and are not licensed by anyone. Code
transcribed from those drivers is not, which is why the exception above exists.
