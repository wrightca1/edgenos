# TODO (next M1 SWI build): build in the flash/HDD block drivers

The M1 kernel currently has **NO block-device support** — on M1, `/proc/partitions` is empty and there
is no `/dev/sda` (the Aboot/EOS flash `/dev/sda1`, a 1.5G vfat, is invisible). Confirmed 2026-07-28 (boot 28
recovery): M1 cannot see or mount the flash, so it **cannot self-update the SWIs on flash** — every reflash
has to go through EOS (`copy http://... flash:...`), which is slow and needs an EOS boot.

## What to add to the M1 kernel config (`/home/smiley/own_kernel/linux-6.12`)
Build in (=y, not =m, so they're present in the initramfs-only boot with no module loading):
- **SATA/AHCI**: `CONFIG_ATA=y`, `CONFIG_SATA_AHCI=y` (+ `CONFIG_ATA_PIIX=y` if the flash is on the ICH/PIIX
  controller — check `lspci` on EOS for the storage controller). The 7150 flash is a SATA DOM / disk on
  `/dev/sda`.
- **Block core + partitions**: `CONFIG_BLOCK=y`, `CONFIG_MSDOS_PARTITION=y` (GPT: `CONFIG_EFI_PARTITION=y`).
- **vfat**: `CONFIG_VFAT_FS=y`, `CONFIG_FAT_FS=y`, `CONFIG_NLS_CODEPAGE_437=y`, `CONFIG_NLS_ISO8859_1=y`.
- (If the flash turns out to be USB/eMMC instead: `CONFIG_USB_STORAGE=y` / `CONFIG_MMC=y`+`CONFIG_MMC_SDHCI=y`.)

Then `build-m1-rootfs.sh` / init-m1 can `mount /dev/sda1 /mnt/flash` and write SWIs directly from M1 — so the
BIST/bring-up reflash loop no longer needs an EOS round-trip. Also add a small `m1-flash-update` helper that
mounts flash, writes a streamed SWI, `sync`s, and unmounts.

## Also: the copy-to-flash sync gotcha (EOS path)
When copying an SWI to flash from EOS, the write sits in page cache; an immediate `reload now` can lose it
(boot 28: md5 matched from cache but Aboot said "not found"). Always `bash sudo sync` (and ideally verify the
on-disk md5 after a drop_caches) BEFORE reloading.
