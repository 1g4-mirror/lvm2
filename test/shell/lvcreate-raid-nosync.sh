#!/usr/bin/env bash

# Copyright (C) 2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA



. lib/inittest --skip-with-lvmpolld

aux have_raid 1 7 0 || skip

segtypes="raid5"
aux have_raid4 && segtypes="raid4 $segtypes"

aux prepare_vg 6

_sync() {
	aux enable_dev "$dev1"

	aux wait_for_sync $vg $lv1
	test -z "$1" || check raid_leg_status $vg $lv1 $1
	lvremove --yes $vg/$lv1

	# restore to delay_dev tables for all devices
	aux restore_from_devtable "$dev1"
}

# Workaround for raid targets returning 'a' shortly after initialization
# TODO: maybe there is some workaround to be made on lvm side
_check_raid_in_loop() {
	local vg=$1
	local lv=$2
	local health=$3
	for i in {1..10} ; do
		check raid_leg_status $vg $lv ${health} && return 0
		sleep .05
	done
	die "Cannot get $A status for $vg/$lv";
}

# Delay 1st leg so that rebuilding status characters
#  can be read before resync finished too quick.
aux delay_dev "$dev1" 0 100 "$(get first_extent_sector "$dev1")"

# raid0/raid0_meta don't support resynchronization
for r in raid0 raid0_meta
do
	lvcreate --type $r -Zn -i 3 -l 1 -n $lv1 $vg
	_check_raid_in_loop $vg $lv1 "AAA"
	lvremove --yes $vg/$lv1
done

# raid1 supports resynchronization
lvcreate --type raid1 -m 2 -Zn -l 4 -n $lv1 $vg
should check raid_leg_status $vg $lv1 "aaa"
_sync "AAA"

# raid1 supports --nosync
lvcreate --type raid1 --nosync -Zn -m 2 -l 1 -n $lv1 $vg
_check_raid_in_loop $vg $lv1 "AAA"
lvremove --yes $vg/$lv1

for r in $segtypes
do
	# raid4/5 still support both synchronization and --nosync.
	lvcreate --type $r -Zn -i 3 -L10 -n $lv1 $vg
	should check raid_leg_status $vg $lv1 "aaaa"
	_sync "AAAA"

	# No prompt, even without --yes or input; warn about parity instead.
	lvcreate --type $r -Zn --nosync -i 3 -l 3 -n $lv1 $vg < /dev/null > out 2>&1
	grep 'Data and parity must already be consistent' out
	grep 'fully zeroed' out
	grep 'Inconsistent parity may cause data loss during recovery' out
	not grep 'Continue.*\[y/n\]' out
	check lv_attr_bit type $vg/$lv1 "R"
	dmsetup table "$vg-$lv1" > table
	grep -w nosync table
	_check_raid_in_loop $vg $lv1 "AAAA"
	lvremove --yes $vg/$lv1

	# --yes does not suppress the warning for inactive creation.  Existing
	# activation behavior initializes an LV created without activation.
	lvcreate --yes -an --type $r -Zn --nosync -i 3 -l 3 -n $lv1 $vg > out 2>&1
	grep 'Inconsistent parity may cause data loss during recovery' out
	check lv_attr_bit type $vg/$lv1 "R"
	lvchange -ay $vg/$lv1
	dmsetup table "$vg-$lv1" > table
	not grep -w nosync table
	_sync "AAAA"
done

# raid6 supports resynchronization
lvcreate --type raid6 -Zn -i 3 -l 4 -n $lv1 $vg
should check raid_leg_status $vg $lv1 "aaaaa"
_sync "AAAAA"

# raid6 rejects --nosync; it has to initialize P- and Q-Syndromes
not lvcreate --type raid6 --nosync -Zn -i 3 -l 1 -n $lv1 $vg
not lvcreate --yes --type raid6 --nosync -Zn -i 3 -l 1 -n $lv1 $vg
not lvcreate --yes -an --type raid6 --nosync -Zn -i 3 -l 1 -n $lv1 $vg
not lvs $vg/$lv1

# raid10 supports resynchronization
lvcreate --type raid10 -m 1 -Zn -i 3 -L10 -n $lv1 $vg
should check raid_leg_status $vg $lv1 "aaaaaa"
_sync "AAAAAA"

# raid10 supports --nosync
lvcreate --type raid10 --nosync -m 1 -Zn -i 3 -l 1 -n $lv1 $vg
_check_raid_in_loop $vg $lv1 "AAAAAA"

vgremove -ff $vg
