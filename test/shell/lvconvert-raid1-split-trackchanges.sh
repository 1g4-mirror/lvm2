#!/usr/bin/env bash

# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA



. lib/inittest --skip-with-lvmpolld

# rhbz1579072/rhbz1579438

aux have_raid 1 3 0 || skip

# 8 PVs needed for RAID10 testing (4-stripes/2-mirror)
aux prepare_vg 4 2

lvcreate -y --ty raid1 -m 2 -n $lv1 -l 1 $vg
lvconvert -y --splitmirrors 1 --trackchanges $vg/$lv1

not lvconvert -y --ty linear $vg/$lv1
not lvconvert -y --ty striped -i 3 $vg/$lv1
not lvconvert -y --ty mirror $vg/$lv1
not lvconvert -y --ty raid4 $vg/$lv1
not lvconvert -y --ty raid5 $vg/$lv1
not lvconvert -y --ty raid6 $vg/$lv1
not lvconvert -y --ty raid10 $vg/$lv1
not lvconvert -y --ty striped -m 1 $vg/${lv1}_rimage_2
not lvconvert -y --ty raid1 -m 1 $vg/${lv1}_rimage_2
not lvconvert -y --ty mirror -m 1 $vg/${lv1}_rimage_2
not lvconvert -y --ty cache-pool $vg/${lv1}_rimage_2
not lvconvert -y --ty thin-pool $vg/${lv1}_rimage_2

vgremove -ff $vg
