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

# When table load fails, devices created so far are removed just once
# and their device numbers are not used for any other device in the tree.

. lib/inittest --skip-with-lvmpolld

aux prepare_vg 3

lvcreate -an -l1 -n $lv1 $vg "$dev1"
lvcreate -an -l1 -n $lv2 $vg "$dev1"
lvcreate -an -l1 -n $lv3 $vg "$dev1"
lvcreate -an -i3 -l3 -n $lv4 $vg

# Stack $lv4 on top of $lv1, $lv2, $lv3, and $lv3 on top of $lv1,
# with $lv2 mapped behind the end of its PV.
vgcfgbackup -f backup $vg
sed -e "s/\tpe_count = .*/\tpe_count = 1000000/" \
    -e "/\t$lv2 {/,/\t$lv3 {/s/\"pv0\", [0-9]*/\"pv0\", 900000/" \
    -e "/\t$lv3 {/,/\t$lv4 {/s/\"pv0\", [0-9]*/\"$lv1\", 0/" \
    -e "/\t$lv4 {/,\$s/\"pv0\", [0-9]*/\"$lv1\", 0/" \
    -e "/\t$lv4 {/,\$s/\"pv1\", [0-9]*/\"$lv2\", 0/" \
    -e "/\t$lv4 {/,\$s/\"pv2\", [0-9]*/\"$lv3\", 0/" backup >backup_stacked
vgcfgrestore -f backup_stacked $vg
check lv_field $vg/$lv4 devices "$lv1(0),$lv2(0),$lv3(0)"

not lvchange -ay -v $vg/$lv4 2>&1 | tee err
test "$(grep -c "Removing $vg-$lv1 " err)" -eq 1
test "$(grep -c "Removing $vg-$lv2 " err)" -eq 1
not grep "Creating $vg-$lv3" err
check inactive $vg $lv1
check inactive $vg $lv2
check inactive $vg $lv3
check inactive $vg $lv4

vgcfgrestore -f backup $vg
vgchange -ay $vg

vgremove -f $vg
