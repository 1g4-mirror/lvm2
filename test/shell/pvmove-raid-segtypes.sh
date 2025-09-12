#!/usr/bin/env bash

# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description="ensure pvmove disallows raid segment types"

. lib/inittest --skip-with-lvmlockd

which md5sum || skip

aux have_raid 1 3 5 || skip

aux prepare_pvs 6 20
get_devs

vgcreate -s 128k "$vg" "${DEVICES[@]}"

for mode in "--atomic" ""
do

# Testing pvmove of RAID0 LV - pvmove should pass
lvcreate -aey -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate -aey --regionsize 16K -l 2 --type raid0 -n $lv1 $vg "$dev1" "$dev2"
check lv_tree_on $vg ${lv1}_foo "$dev1"
check lv_tree_on $vg $lv1 "$dev1" "$dev2"
pvmove $mode "$dev1" "$dev6"
pvmove $mode -n $lv1 "$dev6" "$dev1"
pvmove $mode -n ${lv1}_foo "$dev6" "$dev1"
lvremove -ff $vg

# Each of the following tests does:
# 1) Create two LVs - one linear and one other segment type
#    The two LVs will share a PV.
# 2) Move both LVs together (should fail - has raid LV)
# 3) Move only the first LV by name (should succeed)
# 4) Move only the second LV by name (should fail - is raid LV)

# Testing pvmove of RAID1 LV
lvcreate -aey -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate -aey --regionsize 16K -l 2 --type raid1 -m 1 -n $lv1 $vg "$dev1" "$dev2"
check lv_tree_on $vg ${lv1}_foo "$dev1"
check lv_tree_on $vg $lv1 "$dev1" "$dev2"
not pvmove $mode "$dev1" "$dev6" 2>err
grep "pvmove not allowed on PV $dev1 containing raid LV $vg/$lv1" err
not pvmove $mode -n $lv1 "$dev1" "$dev6" 2>err
grep "pvmove not allowed on raid LV" err
pvmove $mode -n ${lv1}_foo "$dev1" "$dev6"
lvremove -ff $vg

# Testing pvmove of RAID4 LV
lvcreate -aey -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate -aey --regionsize 16K -l 2 --type raid4 -n $lv1 $vg "$dev1" "$dev2" "$dev3"
check lv_tree_on $vg ${lv1}_foo "$dev1"
check lv_tree_on $vg $lv1 "$dev1" "$dev2" "$dev3"
not pvmove $mode "$dev1" "$dev6" 2>err
grep "pvmove not allowed on PV $dev1 containing raid LV $vg/$lv1" err
not pvmove $mode -n $lv1 "$dev1" "$dev6" 2>err
grep "pvmove not allowed on raid LV" err
pvmove $mode -n ${lv1}_foo "$dev1" "$dev6"
lvremove -ff $vg

# Testing pvmove of RAID5 LV
lvcreate -aey -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate -aey --regionsize 16K -l 2 --type raid5 -n $lv1 $vg "$dev1" "$dev2" "$dev3"
check lv_tree_on $vg ${lv1}_foo "$dev1"
check lv_tree_on $vg $lv1 "$dev1" "$dev2" "$dev3"
not pvmove $mode "$dev1" "$dev6" 2>err
grep "pvmove not allowed on PV $dev1 containing raid LV $vg/$lv1" err
not pvmove $mode -n $lv1 "$dev1" "$dev6" 2>err
grep "pvmove not allowed on raid LV" err
pvmove $mode -n ${lv1}_foo "$dev1" "$dev6"
lvremove -ff $vg

# Testing pvmove of RAID6 LV
lvcreate -aey -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate -aey --regionsize 16K -l 2 --type raid6 -n $lv1 $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
check lv_tree_on $vg ${lv1}_foo "$dev1"
check lv_tree_on $vg $lv1 "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
not pvmove $mode "$dev1" "$dev6" 2>err
grep "pvmove not allowed on PV $dev1 containing raid LV $vg/$lv1" err
not pvmove $mode -n $lv1 "$dev1" "$dev6" 2>err
grep "pvmove not allowed on raid LV" err
pvmove $mode -n ${lv1}_foo "$dev1" "$dev6"
lvremove -ff $vg

# Testing pvmove of RAID10 LV
lvcreate -aey -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate -aey --regionsize 16K -l 2 --type raid10 -m 1 -n $lv1 $vg "$dev1" "$dev2" "$dev3" "$dev4"
check lv_tree_on $vg ${lv1}_foo "$dev1"
check lv_tree_on $vg $lv1 "$dev1" "$dev2" "$dev3" "$dev4"
not pvmove $mode "$dev1" "$dev6" 2>err
grep "pvmove not allowed on PV $dev1 containing raid LV $vg/$lv1" err
not pvmove $mode -n $lv1 "$dev1" "$dev6" 2>err
grep "pvmove not allowed on raid LV" err
pvmove $mode -n ${lv1}_foo "$dev1" "$dev6"
lvremove -ff $vg

done

vgremove -ff $vg
