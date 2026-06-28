# core/control-plane — routing control plane (shared)

Quagga (zebra + ospfd) — the FIB manager + OSPF, arch-portable, ASIC-agnostic.
`edged`/`bcmd` mirror the kernel FIB that zebra programs into the ASIC.

- `build-quagga.sh` — cross-compile recipe (static zebra/ospfd)
- `config/` — zebra.conf, ospfd.conf
- `services/` — zebra.service, ospfd.service

Packaged as `quagga` (arch-specific, asic=any). Build artifacts (zebra-<arch>,
ospfd-<arch>) come from the cross-build; the pkgspec sources them.
