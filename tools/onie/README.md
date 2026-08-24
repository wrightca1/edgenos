# tools/onie — exercise the real ONIE install flow in qemu

`onie-install-test.py ISO INSTALLER.bin [--uefi]` boots ONIE's `kvm_x86_64` recovery ISO
on a blank disk, embeds ONIE from the rescue shell, reboots into ONIE install mode, runs
`onie-nos-install http://…/EdgeNOS-<ver>-x86_64-kvm_x86_64-r0.bin` (served by the script),
waits for the EdgeNOS login after ONIE's reboot and checks identity + partition layout.
Legacy BIOS by default, `--uefi` for OVMF (needs `/usr/share/OVMF/OVMF_CODE_4M.fd`).

No prebuilt ONIE KVM ISO is public any more; `build-onie-kvm-iso.sh` builds it from source
in ONIE's Docker build-env (with the build-env rot patched, see the script) — ~25 min.

Verified 2026-08: BIOS and UEFI installs both reach the EdgeNOS menu (with the ONIE entry)
and log in; layout `EFI System | ONIE-BOOT | EDGENOS-BOOT | EDGENOS-DATA`.
