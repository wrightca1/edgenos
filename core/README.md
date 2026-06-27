# core/ — shared, arch/ASIC-agnostic code

Datapath framework, control-plane integration (Quagga/FRR + netlink→chip sync),
and platform services (config-file scheme, swp tap devices, fan/SFP/LED daemons)
that are common across all switches. Populated in Phase 4 as the 5610/4610 trees
are migrated in and their shared logic is factored out of the per-board forks.
