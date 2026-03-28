# iProc Sub-Window Register Access

## Problem

The BCM56846 BAR0 is divided into 8 x 4KB sub-windows.  Only sub-window 0
(BAR0 0x0000-0x0FFF) is permanently mapped to the legacy CMIC register block
at AXI 0x18000000.  Registers in other AXI regions (CMICm MIIM at 0x18032000,
CMICm SCHAN at 0x18033000, XLPORT at 0x18012000) require sub-window remapping.

Without remapping, our BDE module could read/write legacy CMIC registers
(SCHAN_CTRL at 0x050, MIIM at 0x150) but could not access CMICm registers.

## Solution

Added iProc AXI sub-window translation to `linux-kernel-bde.c`, matching the
Broadcom SDK `shbde_iproc_pci_read/write` mechanism:

### Kernel API

```c
// Direct BAR0 access (legacy, for offsets < base_size)
u32  iproc_read(bdev, bar0_offset);
void iproc_write(bdev, bar0_offset, value);

// AXI sub-window access (new, for any AXI address)
u32  iproc_axi_read(bdev, axi_addr);
void iproc_axi_write(bdev, axi_addr, value);
```

### Userspace ioctls

| IOCTL              | Code | Direction | Description                    |
|--------------------|------|-----------|--------------------------------|
| BDE_IOC_IPROC_READ | 7    | IOWR      | Read via AXI sub-window lookup |
| BDE_IOC_IPROC_WRITE| 8    | IOW       | Write via AXI sub-window lookup|

Both use `struct bde_reg_io { dev, addr, val }` where `addr` is the full
AXI address (e.g. 0x18032000 for CMICm MIIM base).

### How it works

1. At probe time, `subwin_cache_init()` reads IMAP0_0 through IMAP0_7
   from BAR0 + 0x2C00..0x2C1C and caches the AXI base for each sub-window.

2. On each `iproc_axi_read/write(axi_addr)`:
   - Compute `page = axi_addr & ~0xFFF`, `offset = axi_addr & 0xFFF`
   - Scan cached sub-windows 0-7 for matching page
   - If found: access `BAR0 + (subwin * 0x1000) + offset`
   - If not found: remap sub-window 7 by writing `(page | 1)` to IMAP0_7,
     then access `BAR0 + 0x7000 + offset`

3. A spinlock (`iproc_lock`) protects concurrent sub-window 7 remapping.

### Relevant AXI addresses

```
0x18000000  CMIC base         (sub-window 0, always available)
0x18012000  XLPORT/MAC        (sub-window 2 default)
0x18030000  CMICm SCHAN/DMA   (sub-window 1 default)
0x18032000  CMICm MIIM        (requires sub-window 7 remap)
```

## Usage from CDK/BMD userspace

For the OpenMDK CDK/BMD layer that mmaps BAR0 directly, the sub-window remap
can be done from userspace by writing to the mmap'd IMAP0_7 register:

```c
volatile uint32_t *bar0 = mmap(...);

// Remap sub-window 7 to CMICm MIIM (AXI 0x18032000)
bar0[0x2C1C / 4] = 0x18032001;  // page | valid
// Read CMICm MIIM register at offset 0x000 within the 4K page
uint32_t val = bar0[0x7000 / 4];
```

For the BDE ioctl path, use the new IPROC ioctls instead.

## Files changed

- `asic/bde/linux-kernel-bde.c` — Added sub-window cache, remap logic, ioctls
