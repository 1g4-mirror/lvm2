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

aux have_raid 1 5 0 || skip
aux prepare_vg 3

# degrade_leg <dev> <byte>: fail writes to one RAID leg while its PV label
# stays readable, dirty the array through the top LV and check that the leg
# is reported dead.
degrade_leg() {
	aux error_dev "$1" "$(get first_extent_sector "$1"):"
	echo "$2" > "$DM_DEV_DIR/$vg/$lv1"
	sync
	check raid_leg_status "$vg" "$lv1" "AD"
}

lvcreate --type raid1 -m 1 -L 16M -n "$lv1" "$vg" "$dev1" "$dev2"
aux wait_for_sync "$vg" "$lv1"

# A degraded array must not be extended.
degrade_leg "$dev2" b
dmsetup table "$vg-$lv1" > table_before
not lvextend -L +4M "$vg/$lv1" 2>&1 | tee out
grep -F "Cannot resize RAID LV" out
check lv_field "$vg/$lv1" lv_size "16.00m"
dmsetup table "$vg-$lv1" > table_after
cmp table_before table_after
aux enable_dev "$dev2"
aux wait_for_sync "$vg" "$lv1"

# A replacement must finish recovery before extending the array.
aux delay_dev "$dev3" 0 100 "$(get first_extent_sector "$dev3"):"
lvconvert -y --repair "$vg/$lv1" "$dev3"
aux wait_raid_state "$vg" "$lv1" recover rebuild
dmsetup table "$vg-$lv1" > table_before
not lvextend -L +4M "$vg/$lv1" 2>&1 | tee out
grep -F "Cannot resize RAID LV" out
check lv_field "$vg/$lv1" lv_size "16.00m"
dmsetup table "$vg-$lv1" > table_after
cmp table_before table_after
aux enable_dev "$dev3"
aux wait_for_sync "$vg" "$lv1"
check raid_leg_status "$vg" "$lv1" "AA"

# A healthy running array is extended.  A target before 1.15.0 cannot
# synchronize the new space itself, so the command warns after the resize
# and points at the operation that resynchronizes the whole LV; a target
# with extend resync does it itself and stays silent.
lvextend -L +4M "$vg/$lv1" 2>&1 | tee out
if aux have_raid 1 15 0; then
	not check raid_resync_warning_present out
else
	check raid_resync_warning_present out
fi
check lv_field "$vg/$lv1" lv_size "20.00m"
aux wait_for_sync "$vg" "$lv1"

# A stopped array has no table to check, so the resize is allowed; the
# warning about the new space still follows the target, not the table.
lvchange -an "$vg/$lv1"
lvextend -L +4M "$vg/$lv1" 2>&1 | tee out
if aux have_raid 1 15 0; then
	not check raid_resync_warning_present out
else
	check raid_resync_warning_present out
fi
check lv_field "$vg/$lv1" lv_size "24.00m"
lvchange -ay "$vg/$lv1"
aux wait_for_sync "$vg" "$lv1"

# Without activation the target cannot be queried, so the extension warns
# on every kernel and covers the warning text that the target-based checks
# above only exercise on an older kernel.
lvextend --config 'global/activation=0' -L +4M "$vg/$lv1" 2>&1 | tee out
grep -F "Activation disabled" out
check raid_resync_warning_present out
check lv_field "$vg/$lv1" lv_size "28.00m"

# --nosync keeps the LV marked not synchronized and asks no resync.
lvchange -an "$vg/$lv1"
lvextend --yes --nosync -L +4M "$vg/$lv1" 2>&1 | tee out
not check raid_resync_warning_present out
check lv_field "$vg/$lv1" lv_size "32.00m"

# A shrink removes space, so it must not claim that a new area needs a
# resync.  Resync first, so the reduce runs on a synchronized LV: the early
# return for a not-synchronized one would hide a warning that leaked through
# the tail that extensions and reductions share.
if aux have_raid 1 9 0; then
	lvchange -ay "$vg/$lv1"
	lvchange --yes --resync "$vg/$lv1"
	aux wait_for_sync "$vg" "$lv1"
	lvresize --fs ignore --yes -L -4M "$vg/$lv1" 2>&1 | tee out
	not check raid_resync_warning_present out
	check lv_field "$vg/$lv1" lv_size "28.00m"
fi

vgremove -f "$vg"
