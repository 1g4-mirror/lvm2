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

# A mirrored RaidLV retains its --nosync policy across extensions until an
# explicit resync.  The policy can be set at creation or during an extend.

. lib/inittest

aux have_raid 1 3 0 || skip

aux prepare_vg 5

# Check the device table independently of the persistent LV attribute.
check_nosync_table() {
	dmsetup table "$1" > table
	if [ "$2" = yes ]; then
		grep -q nosync table
	else
		not grep -q nosync table
	fi
}

lvcreate --yes --type raid1 -m 1 -L 8M -n $lv1 $vg
check lv_attr_bit type $vg/$lv1 "r"

# Without --nosync the extended region is synchronized.
lvextend --yes -L +4M $vg/$lv1
check lv_field $vg/$lv1 lv_size "12.00m"
check lv_attr_bit type $vg/$lv1 "r"
check_nosync_table "$vg-$lv1" no
aux wait_for_sync $vg $lv1

# A request that does not extend the LV must not prompt.
not lvextend --nosync -L 12M $vg/$lv1 <<< n > out 2>&1
not grep 'Continue.*\[y/n\]' out
check lv_field $vg/$lv1 lv_size "12.00m"
check lv_attr_bit type $vg/$lv1 "r"

# Declining --nosync must not change size, metadata or the live table.
not lvextend --nosync -L +4M $vg/$lv1 <<< n > out 2>&1
grep 'lvchange --resync synchronizes the whole LV' out
grep 'marked not synchronized, even if later reduced' out
grep 'Continue.*\[y/n\]' out
check lv_field $vg/$lv1 lv_size "12.00m"
check lv_attr_bit type $vg/$lv1 "r"
check_nosync_table "$vg-$lv1" no

# Missing input is treated as a refusal too.
not lvextend --nosync -L +4M $vg/$lv1 < /dev/null > out 2>&1
grep 'Continue.*\[y/n\]' out
check lv_field $vg/$lv1 lv_size "12.00m"
check lv_attr_bit type $vg/$lv1 "r"

# Accepting --nosync skips resync of the extension and flags the LV.
lvextend --nosync -L +4M $vg/$lv1 <<< y > out 2>&1
grep 'Continue.*\[y/n\]' out
check lv_field $vg/$lv1 lv_size "16.00m"
check lv_attr_bit type $vg/$lv1 "R"
check_nosync_table "$vg-$lv1" yes

# An already flagged LV does not prompt again, even with --nosync.
lvextend --nosync -L +4M $vg/$lv1 <<< n > out 2>&1
not grep 'Continue.*\[y/n\]' out
not grep 'WARNING: Extending' out
check lv_attr_bit type $vg/$lv1 "R"
check_nosync_table "$vg-$lv1" yes

# A later plain extend inherits the policy set by lvextend --nosync.
lvextend --yes -L +4M $vg/$lv1
check lv_field $vg/$lv1 lv_size "24.00m"
check lv_attr_bit type $vg/$lv1 "R"
check_nosync_table "$vg-$lv1" yes

# Explicit resync clears the policy; the next plain extend must not skip.
lvchange --yes --resync $vg/$lv1
aux wait_for_sync $vg $lv1
check lv_attr_bit type $vg/$lv1 "r"
lvextend --yes -L +4M $vg/$lv1
check lv_field $vg/$lv1 lv_size "28.00m"
check lv_attr_bit type $vg/$lv1 "r"
check_nosync_table "$vg-$lv1" no
aux wait_for_sync $vg $lv1

# --yes accepts a new --nosync policy without prompting.  Shrinking away
# the added extents does not restore the LV's synchronized status.
lvextend --yes --nosync -L +4M $vg/$lv1 <<< n > out 2>&1
grep 'lvchange --resync synchronizes the whole LV' out
not grep 'Continue.*\[y/n\]' out
check lv_field $vg/$lv1 lv_size "32.00m"
check lv_attr_bit type $vg/$lv1 "R"
check_nosync_table "$vg-$lv1" yes
# Shrinking a RAID LV needs dm-raid 1.9.0.  Do not pass --fs ignore:
# without libblkid only the fsadm-based fs handling exists, and the LV
# has no filesystem to resize anyway.
if aux have_raid 1 9 0; then
	lvresize --yes -L 28M $vg/$lv1
	check lv_field $vg/$lv1 lv_size "28.00m"
	check lv_attr_bit type $vg/$lv1 "R"
fi

lvremove -ff $vg/$lv1

# An inactive nosync mirror can be extended without a sync-percent query.
lvcreate --yes --nosync --type raid1 -m 1 -L 8M -n $lv1 $vg
lvchange -an $vg/$lv1
lvextend --yes -L +8M $vg/$lv1
check lv_field $vg/$lv1 lv_size "16.00m"
check lv_attr_bit type $vg/$lv1 "R"
lvchange -ay $vg/$lv1
aux wait_for_sync $vg $lv1
lvremove -ff $vg/$lv1

# A plain extend also inherits the policy set by lvcreate --nosync.
lvcreate --yes --nosync --type raid1 -m 1 -L 8M -n $lv2 $vg "$dev1" "$dev2"
check lv_attr_bit type $vg/$lv2 "R"
check_nosync_table "$vg-$lv2" yes

lvextend --yes -L +8M $vg/$lv2
check lv_field $vg/$lv2 lv_size "16.00m"
check lv_attr_bit type $vg/$lv2 "R"
check_nosync_table "$vg-$lv2" yes
aux wait_for_sync $vg $lv2

# An active LV that still carries the not-synced flag is refused while the
# array is recovering: the resync of the extended portion can only be
# skipped when the array reports 100%.  Break the leg data and write a byte
# the repair has to reproduce.  Keep the PV label readable by erroring only
# from the first extent, so the VG is not reported as missing a PV.  Delay
# the replacement leg so the array is demonstrably recovering when the
# extension is attempted.
aux error_dev "$dev2" "$(get first_extent_sector "$dev2"):"
echo a > "$DM_DEV_DIR/$vg/$lv2"
sync
aux delay_dev "$dev3" 0 100 "$(get first_extent_sector "$dev3"):"
lvconvert -y --repair $vg/$lv2 "$dev3"
aux wait_raid_state $vg $lv2 recover rebuild

not lvextend --yes -L +8M $vg/$lv2 > out 2>&1
grep "cannot be extended while it is recovering" out
check lv_field $vg/$lv2 lv_size "16.00m"
check lv_attr_bit type $vg/$lv2 "R"
check lv_field $vg/$lv2 lv_active active

# Let the rest of the rebuild finish at full speed and verify that the
# repaired leg is back and the array is in sync with the written data.
aux enable_dev "$dev3"
aux wait_for_sync $vg $lv2
check raid_leg_status $vg $lv2 "AA"
check lv_on $vg ${lv2}_rimage_1 "$dev3"
dd if="$DM_DEV_DIR/$vg/$lv2" bs=1 count=1 2>/dev/null | grep -q a
dd if="$DM_DEV_DIR/mapper/$vg-${lv2}_rimage_1" bs=1 count=1 2>/dev/null | grep -q a
aux enable_dev "$dev2"
lvremove -ff $vg/$lv2

# RAID4/5 keep the same persistent policy as mirrored LVs.  Delay one
# image to distinguish actual synchronization from a table containing nosync.
segtypes="raid5"
aux have_raid4 && segtypes="raid4 $segtypes"
for r in $segtypes; do
	lvcreate --yes --type $r -i 3 -L 12M -n $lv1 $vg "$dev1" "$dev2" "$dev3" "$dev4"
	aux wait_for_sync $vg $lv1
	aux delay_dev "$dev1" 0 100 "$(get first_extent_sector "$dev1"):"
	lvextend --yes -L +12M $vg/$lv1
	check_nosync_table "$vg-$lv1" no
	aux wait_raid_state $vg $lv1 resync repair
	aux enable_dev "$dev1"
	aux wait_for_sync $vg $lv1
	aux restore_from_devtable "$dev1"

	not lvextend --nosync -L 24M $vg/$lv1 <<< n > out 2>&1
	not grep 'Continue.*\[y/n\]' out
	lvextend --nosync -L +12M $vg/$lv1 <<< n > out 2>&1
	grep 'New data and parity must already be consistent' out
	grep 'Inconsistent parity may cause data loss' out
	not grep 'Continue.*\[y/n\]' out
	check lv_field $vg/$lv1 lv_size "36.00m"
	check lv_attr_bit type $vg/$lv1 "R"
	check_nosync_table "$vg-$lv1" yes
	check raid_action $vg $lv1 idle
	check raid_leg_status $vg $lv1 "AAAA"

	# Explicit and plain extensions warn without a prompt, even with no input.
	lvresize --nosync -L +12M $vg/$lv1 < /dev/null > out 2>&1
	grep 'Inconsistent parity may cause data loss' out
	not grep 'Continue.*\[y/n\]' out
	check lv_attr_bit type $vg/$lv1 "R"
	check_nosync_table "$vg-$lv1" yes
	check raid_action $vg $lv1 idle
	aux wait_for_sync $vg $lv1
	lvextend -L +12M $vg/$lv1 > out 2>&1
	grep 'Inconsistent parity may cause data loss' out
	not grep 'Continue.*\[y/n\]' out
	check lv_attr_bit type $vg/$lv1 "R"
	check_nosync_table "$vg-$lv1" yes
	check raid_action $vg $lv1 idle
	aux wait_for_sync $vg $lv1
	aux enable_dev "$dev1"
	lvchange --yes --resync $vg/$lv1
	aux wait_for_sync $vg $lv1
	check lv_attr_bit type $vg/$lv1 "r"
	lvextend --yes --nosync -L +12M $vg/$lv1 <<< n > out 2>&1
	grep 'Inconsistent parity may cause data loss' out
	not grep 'Continue.*\[y/n\]' out
	check lv_attr_bit type $vg/$lv1 "R"
	check_nosync_table "$vg-$lv1" yes
	lvremove -ff $vg/$lv1

	# An LV created with --nosync retains its policy on a plain extension.
	lvcreate --yes --nosync --type $r -i 3 -L 12M -n $lv1 $vg "$dev1" "$dev2" "$dev3" "$dev4"
	check lv_attr_bit type $vg/$lv1 "R"
	lvextend --yes -L +12M $vg/$lv1 > out 2>&1
	grep 'Inconsistent parity may cause data loss' out
	not grep 'Continue.*\[y/n\]' out
	check lv_attr_bit type $vg/$lv1 "R"
	check_nosync_table "$vg-$lv1" yes
	check raid_action $vg $lv1 idle
	aux wait_for_sync $vg $lv1
	if [ "$r" = raid5 ]; then
		# An inherited nosync must not interrupt a real image rebuild.
		aux delay_dev "$dev5" 0 100 "$(get first_extent_sector "$dev5"):"
		lvconvert -y --replace "$dev2" $vg/$lv1 "$dev5"
		aux wait_raid_state $vg $lv1 recover rebuild
		not lvextend --yes -L +12M $vg/$lv1 > out 2>&1
		grep 'cannot be extended while it is recovering' out
		not grep 'New data and parity must already be consistent' out
		not grep 'Inconsistent parity may cause data loss' out
		check lv_field $vg/$lv1 lv_size "24.00m"
		check lv_attr_bit type $vg/$lv1 "R"
		aux enable_dev "$dev5"
		aux wait_for_sync $vg $lv1
	fi
	# An inactive parity LV retains its nosync policy on extension too.
	lvchange -an $vg/$lv1
	lvextend --yes -L +12M $vg/$lv1 > out 2>&1
	grep 'Inconsistent parity may cause data loss' out
	check lv_field $vg/$lv1 lv_size "36.00m"
	check lv_attr_bit type $vg/$lv1 "R"
	lvchange -ay $vg/$lv1
	aux wait_for_sync $vg $lv1
	lvremove -ff $vg/$lv1
done

# RAID6 extensions ignore --nosync and state that the new space is
# synchronized; there is no prompt, also with --yes.
lvcreate --yes --type raid6 -i 3 -L 12M -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvextend --nosync -L +12M $vg/$lv1 < /dev/null > out 2>&1
grep 'Ignoring --nosync' out
grep 'raid6 always synchronizes the new space' out
not grep 'Continue.*\[y/n\]' out
check lv_field $vg/$lv1 lv_size "24.00m"
check lv_attr_bit type $vg/$lv1 "r"
check_nosync_table "$vg-$lv1" no
aux wait_for_sync $vg $lv1
lvresize --yes --nosync -L +12M $vg/$lv1 > out 2>&1
grep 'Ignoring --nosync' out
not grep 'Continue.*\[y/n\]' out
check lv_field $vg/$lv1 lv_size "36.00m"
check lv_attr_bit type $vg/$lv1 "r"
check_nosync_table "$vg-$lv1" no
aux wait_for_sync $vg $lv1
lvremove -ff $vg/$lv1

vgremove -ff $vg
