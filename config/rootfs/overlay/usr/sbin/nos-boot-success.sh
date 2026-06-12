#!/bin/sh
# nos-boot-success.sh - mark the current slot as good for auto-rollback.
#
# The U-Boot nos_bootcmd increments `boot_count` (and saveenv) before each
# boot, and switches cl.active to the other slot once boot_count exceeds
# boot_limit. This service resets boot_count to 0 ONCE the datapath is
# confirmed up, so a slot that boots Linux but whose edged never comes up is
# NOT marked good and will auto-roll-back on the next boots.
#
# Requires fw_setenv (u-boot-tools) + a working /etc/fw_env.config (fw_printenv
# already works on this box, so the config is present).

LOG="logger -t nos-boot-success"

# fw_setenv must be usable; the assembled rootfs ships fw_setenv -> fw_printenv.
command -v fw_setenv >/dev/null 2>&1 || { $LOG "fw_setenv missing; cannot clear boot_count"; exit 0; }

# Confirm the datapath daemon actually started before declaring success.
i=0
while [ $i -lt 30 ]; do
    pgrep -x edged >/dev/null 2>&1 && break
    i=$((i + 1)); sleep 2
done
if ! pgrep -x edged >/dev/null 2>&1; then
    $LOG "edged not running after 60s; NOT clearing boot_count (slot will auto-roll-back)"
    exit 1
fi

# Healthy: clear the counter so this slot is marked good.
if fw_setenv boot_count 0 2>/dev/null; then
    $LOG "boot_count reset to 0 (slot $(fw_printenv cl.active 2>/dev/null | cut -d= -f2) confirmed good)"
else
    $LOG "fw_setenv boot_count 0 failed"
fi
exit 0
