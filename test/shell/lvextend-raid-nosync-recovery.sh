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

. lib/inittest

aux have_raid 1 3 0 || skip
aux prepare_devs 4 34
get_devs

# Sparse partial-stripe writes must be recoverable from consistent parity.
# The test directory may be on tmpfs, which lacked direct I/O support
# before Linux 6.6, so keep this regular file buffered.
dd if=/dev/zero of=data bs=1M count=12
for block in 0 1 4 64 191; do
	dd if=/dev/urandom of=data bs=64K count=1 seek="$block" conv=notrunc
done
segtypes="raid5"
aux have_raid4 && segtypes="raid4 $segtypes"
for r in $segtypes; do
	# Zero the entire test PVs before creating the VG, including free extents.
	aux clear_devs "${DEVICES[@]}"
	vgcreate $SHARED -s 512K "$vg" "${DEVICES[@]}"
	lvcreate --yes --type "$r" -i 3 -L 12M -n "$lv1" "$vg" "${DEVICES[@]}"
	check lv_on "$vg" "${lv1}_rimage_1" "$dev2"
	aux wait_for_sync "$vg" "$lv1"
	lvextend --yes --nosync -L +12M "$vg/$lv1"
	check lv_attr_bit type "$vg/$lv1" "R"
	dmsetup table "$vg-$lv1" > table
	grep -w nosync table
	check raid_action "$vg" "$lv1" idle

	for block in 0 1 4 64 191; do
		dd if=data of="$DM_DEV_DIR/$vg/$lv1" bs=64K skip="$block" \
			seek=$((192 + block)) count=1 oflag=direct conv=notrunc
	done
	sync
	dd if="$DM_DEV_DIR/$vg/$lv1" bs=1M skip=12 count=12 iflag=direct | cmp data -

	# Trigger failure even if this PV contains the dedicated RAID4 parity leg.
	aux error_dev "$dev2" "$(get first_extent_sector "$dev2"):"
	dd if=data of="$DM_DEV_DIR/$vg/$lv1" bs=64K seek=192 count=1 oflag=direct conv=notrunc
	sync
	check grep_dmsetup status "$vg-$lv1" ADAA
	dd if="$DM_DEV_DIR/$vg/$lv1" bs=1M skip=12 count=12 iflag=direct | cmp data -

	aux enable_dev "$dev2"
	lvremove -ff "$vg/$lv1"
	vgremove -ff "$vg"
done
