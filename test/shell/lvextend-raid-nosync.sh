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

# A RaidLV created with --nosync and its areas mirrored (raid1/raid10) has
# nothing to synchronize, so extending it must not require the sync percent
# only an active LV can provide: the extension is activated with the resync
# skipped, and the LV stays flagged as not synced.

. lib/inittest

aux have_raid 1 3 0 || skip

aux prepare_vg 4

lvcreate --yes --nosync --type raid1 -m 1 -L 8M -n $lv1 $vg
check lv_attr_bit type $vg/$lv1 "R"

# An inactive LV cannot be recovering, so extending it must not ask for a
# full resync of the extended portion.
lvchange -an $vg/$lv1
lvextend --yes -L +8M $vg/$lv1
check lv_field $vg/$lv1 lv_size "16.00m"
check lv_attr_bit type $vg/$lv1 "R"

# The same on an active LV, where the in-sync check still applies.
lvchange -ay $vg/$lv1
lvextend --yes -L +8M $vg/$lv1
check lv_field $vg/$lv1 lv_size "24.00m"
check lv_attr_bit type $vg/$lv1 "R"

# An active LV that still carries the not-synced flag must be refused while
# the array is recovering: the resync of the extended portion can only be
# skipped when the array reports 100%.  Rebuild a failed leg slowly enough
# that the extension is attempted while it is demonstrably running.
lvcreate --yes --nosync --type raid1 -m 1 -L 8M -n $lv2 $vg "$dev1" "$dev2"
check lv_attr_bit type $vg/$lv2 "R"

dmsetup remove -f "$vg-${lv2}_rimage_1" || true
echo a > "$DM_DEV_DIR/$vg/$lv2"
sync
aux delay_dev "$dev3" 0 100 "$(get first_extent_sector "$dev3"):"
lvconvert -y --repair $vg/$lv2 "$dev3"
aux wait_raid_state $vg $lv2 recover rebuild

not lvextend --yes -L +8M $vg/$lv2 > out 2>&1
grep "cannot be extended while it is recovering" out
check lv_field $vg/$lv2 lv_size "8.00m"
check lv_attr_bit type $vg/$lv2 "R"
check lv_field $vg/$lv2 lv_active active

aux wait_for_sync $vg $lv2
lvremove -ff $vg/$lv2

vgremove -ff $vg
