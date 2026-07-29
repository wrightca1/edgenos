# TODO (next M1 SWI build): build in the flash/HDD block drivers

The M1 kernel currently has **NO block-device support** — on M1, `/proc/partitions` is empty and there
is no `/dev/sda` (the Aboot/EOS flash `/dev/sda1`, a 1.5G vfat, is invisible). Confirmed 2026-07-28 (boot 28
recovery): M1 cannot see or mount the flash, so it **cannot self-update the SWIs on flash** — every reflash
has to go through EOS (`copy http://... flash:...`), which is slow and needs an EOS boot.

## What to add to the M1 kernel config (`/home/smiley/own_kernel/linux-6.12`) — DONE 2026-07-29
**CORRECTION: the flash is a USB DOM, NOT SATA.** EOS shows `/dev/sda` at
`pci0000:00/0000:00:12.2/usb1/.../usb-storage` (EOS cmdline: `block_flash=...00:12.[02]/usb.*`). The SB700
SATA controller (00:11.0, IDE mode) is unused. So the M1 kernel needs the **USB stack**, not SATA:
- `CONFIG_USB_SUPPORT=y CONFIG_USB=y CONFIG_USB_PCI=y` + SB700 host: `CONFIG_USB_EHCI_HCD=y CONFIG_USB_OHCI_HCD=y`
- `CONFIG_USB_STORAGE=y` + `CONFIG_SCSI=y CONFIG_BLK_DEV_SD=y`
- `CONFIG_VFAT_FS=y CONFIG_FAT_FS=y CONFIG_MSDOS_PARTITION=y CONFIG_NLS_CODEPAGE_437/ISO8859_1=y` (already =y)
Applied via `scripts/config` on 2026-07-29 (also dropped `quiet loglevel=0` from CONFIG_CMDLINE for boot
visibility). After boot: `mount /dev/sda1 /mnt/flash` should work; then M1 can write SWIs directly.

Then `build-m1-rootfs.sh` / init-m1 can `mount /dev/sda1 /mnt/flash` and write SWIs directly from M1 — so the
BIST/bring-up reflash loop no longer needs an EOS round-trip. Also add a small `m1-flash-update` helper that
mounts flash, writes a streamed SWI, `sync`s, and unmounts.

## Also: the copy-to-flash sync gotcha (EOS path)
When copying an SWI to flash from EOS, the write sits in page cache; an immediate `reload now` can lose it
(boot 28: md5 matched from cache but Aboot said "not found"). Always `bash sudo sync` (and ideally verify the
on-disk md5 after a drop_caches) BEFORE reloading.
