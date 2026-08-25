/* fm6000_mrl_table.h - MRL scan-config table, loaded at RUNTIME from a data file.
 *
 * PROVENANCE NOTE
 * ---------------
 * The MRL scan-configuration values are a third-party data table (they were
 * originally observed in Intel's FocalPoint SDK). That data is NOT part of this
 * source tree and is NOT distributed with it. This header provides only our own
 * loader; the values must be supplied at runtime via a plain text data file:
 *
 *     FM6000_MRL_TABLE=/path/to/mrl-table.txt   (default: /etc/edgenos/mrl-table.txt)
 *
 * File format: one entry per line, two hex words separated by whitespace:
 *     0x00000090 0x00000000
 * Blank lines and '#' comments are ignored.
 *
 * The MRL scan is NOT used by the working cold bring-up sequence
 * (build/arista-7150/m1/payload/fm6000-fullseq.sh); it is retained for
 * diagnostic use only. See docs/EDGENOS-7150.md (was PROVENANCE).
 *
 * Copyright (C) 2024 EdgeNOS Contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FM6000_MRL_TABLE_H
#define FM6000_MRL_TABLE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FM6000_MRL_MAX_ENTRIES 16384
#define FM6000_MRL_DEFAULT_PATH "/etc/edgenos/mrl-table.txt"

static unsigned int fm6000_mrl_table[FM6000_MRL_MAX_ENTRIES][2];
static int FM6000_MRL_ENTRIES;

/* Load the scan table. Returns the entry count, or -1 on error (message on stderr). */
static int fm6000_mrl_table_load(void)
{
	const char *path = getenv("FM6000_MRL_TABLE");
	char line[256];
	FILE *f;
	int n = 0;

	if (!path || !*path)
		path = FM6000_MRL_DEFAULT_PATH;

	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr,
			"[mrl] cannot open scan table '%s'.\n"
			"[mrl] This data is not distributed with EdgeNOS; supply it via\n"
			"[mrl] FM6000_MRL_TABLE=<file> (two hex words per line).\n", path);
		return -1;
	}

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		unsigned long w0, w1;
		char *end;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;

		w0 = strtoul(p, &end, 16);
		if (end == p)
			continue;
		p = end;
		w1 = strtoul(p, &end, 16);
		if (end == p) {
			fprintf(stderr, "[mrl] %s:%d: expected two hex words\n", path, n + 1);
			fclose(f);
			return -1;
		}
		if (n >= FM6000_MRL_MAX_ENTRIES) {
			fprintf(stderr, "[mrl] %s: more than %d entries\n", path,
				FM6000_MRL_MAX_ENTRIES);
			fclose(f);
			return -1;
		}
		fm6000_mrl_table[n][0] = (unsigned int)w0;
		fm6000_mrl_table[n][1] = (unsigned int)w1;
		n++;
	}
	fclose(f);

	FM6000_MRL_ENTRIES = n;
	fprintf(stderr, "[mrl] loaded %d scan entries from %s\n", n, path);
	return n;
}

#endif /* FM6000_MRL_TABLE_H */
