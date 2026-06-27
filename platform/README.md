# platform/<vendor>-<model>/ — per-board support

Device tree, CPLD driver, ONLP platform layer, port map, and firmware list for one
physical switch, keyed by the platform entry's `platform_dir`. This is where a new
board's board-specific bits live; everything reusable belongs in core/, arch/, asic/.
