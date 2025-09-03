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

test_description="ensure pvmove works with thin segment types"

. lib/inittest --skip-with-lvmlockd

which md5sum || skip

aux have_thin 1 8 0 || skip
# for stacking
aux have_raid 1 3 5 || skip

aux prepare_pvs 5 20
get_devs

vgcreate -s 128k "$vg" "${DEVICES[@]}"

for mode in "--atomic" ""
do

# Each of the following tests does:
# 1) Create two LVs - one linear and one other segment type
#    The two LVs will share a PV.
# 2) Move both LVs together
# 3) Move only the second LV by name


# Testing pvmove of thin LV
lvcreate -aey -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate -aey -T $vg/${lv1}_pool -l8 -V 8 -n $lv1 "$dev1"
check lv_tree_on $vg ${lv1}_foo "$dev1"
check lv_tree_on $vg $lv1 "$dev1"
aux mkdev_md5sum $vg $lv1
pvmove "$dev1" "$dev5" $mode
check lv_tree_on $vg ${lv1}_foo "$dev5"
check lv_tree_on $vg $lv1 "$dev5"
check dev_md5sum $vg $lv1
pvmove -n $lv1 "$dev5" "$dev4" $mode
check lv_tree_on $vg $lv1 "$dev4"
check lv_tree_on $vg ${lv1}_foo "$dev5"
check dev_md5sum $vg $lv1
lvremove -ff $vg

done

vgremove -ff $vg
