# images/ — per-platform image recipes (Phase 3)

Resolve a platform from the switch DB -> pull its component .epk packages -> compose
the rootfs squashfs -> wrap in the platform's installer envelope (onie-sfx | onl-swi).
