#!/usr/bin/env bash

# Copyright (C) 2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test that every bcache I/O backend is selectable and reports device
# read errors with the common "WARNING: bcache <backend>" prefix.
# Short-read handling is covered deterministically by
# test/unit/io_engine_t.c, so it is not asserted here.

. lib/inittest --skip-with-lvmpolld

aux prepare_devs 3

#
# Discover which backends are actually built in.  bcache_create_io_engine()
# silently falls back to sync when a backend is unavailable, so use the
# "Created bcache ... engine" debug message to detect real support.
#
BACKENDS=(sync)
for backend in threads uring async; do
	pvs -vvv --config "global/io_backend=\"$backend\"" "$dev1" 2>&1 |
		grep -q "Created bcache.*$backend" && BACKENDS+=("$backend")
done

vgcreate $SHARED -s 4k $vg "$dev1" "$dev2" "$dev3"
lvcreate -aey -n $lv -l 10 $vg

for backend in "${BACKENDS[@]}"; do
	# Test 1: normal operation must not warn.
	vgs --config "global/io_backend=\"$backend\"" $vg |& tee err.log
	not grep "WARNING:" err.log

	#
	# Test 2: device read errors.
	# Error the first sectors so the PV label read fails.  Every backend
	# must report it using the same "WARNING: bcache <backend>" prefix.
	#
	aux error_dev "$dev1" 0:8
	pvs --config "global/io_backend=\"$backend\"" "$dev1" |& tee err.log || true
	aux enable_dev "$dev1"

	grep "WARNING: bcache $backend" err.log
done

vgremove -ff $vg
