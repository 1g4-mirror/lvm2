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

# Adding stripes to an active RaidLV grows it, so the region size in use
# before the reshape may be below the minimum dm-raid accepts, which is
# len / 2^21 sectors to keep the MD bitmap within 2^21 regions.
#
# With 4KiB regions that minimum is exceeded once the RaidLV grows beyond
# 8GiB, so grow a 3GiB raid5 from 2 to 6 data stripes (3GiB -> 9GiB,
# minimum region size of 9 sectors).  The reshape used to be rejected with
# "Supplied region_size (8 sectors) below minimum (9)" after the LV had
# already been grown and the extra images committed, which left the LV
# grown in the metadata with a kernel mapping stuck at the old size.

. lib/inittest

aux have_raid 1 13 2 || skip

aux prepare_pvs 8 2048
get_devs

# Map the data areas of the PVs to zero targets: the reshape of the grown
# RaidLV then works on devices that discard writes, so it needs no real I/O
# or storage.  Only the table dm-raid validates matters here.
for d in "${DEVICES[@]}"
do
	aux zero_dev "$d" "$(( $(get first_extent_sector "$d") + 8192 )):"
done

vgcreate $SHARED -s 4M "$vg" "${DEVICES[@]}"

lvcreate --yes --nosync --type raid5_ls --stripes 2 --stripesize 4k --regionsize 4k \
	-L3G -n $lv1 $vg "$dev1" "$dev2" "$dev3"
check lv_first_seg_field $vg/$lv1 regionsize "4.00k"
check lv_first_seg_field $vg/$lv1 data_stripes 2

# Growing to 6 data stripes needs a region size of at least 9 sectors, so
# keeping the 4KiB one in use makes the final reshape table get rejected.
lvconvert --yes --stripes 6 $vg/$lv1
check lv_first_seg_field $vg/$lv1 data_stripes 6
check lv_first_seg_field $vg/$lv1 stripes 7

# The reshape re-lays-out all of the data, so the initial sync the --nosync
# creation skipped is no longer pending.
check lv_attr_bit type $vg/$lv1 "r"

# The reshape itself is not waited for: the table dm-raid accepted is what
# this checks.  The block device keeps the capacity it had before the
# reshape until that finishes, so the grown size is asserted on the table
# and in the metadata.
table=$(dmsetup table "$vg-$lv1")
len=$(echo "$table" | awk '{print $2}')
test "$len" -eq "$(get lv_field $vg/$lv1 size --units s --nosuffix)"

# The region size in the metadata and in the loaded table has to agree and
# satisfy the minimum for the grown size (len / 2^21 sectors).
region=$(echo "$table" |
	awk '{for (i = 1; i < NF; i++) if ($i == "region_size") print $(i + 1)}')
test "$region" -ge $(( len / (1 << 21) ))
test "$(get lv_first_seg_field $vg/$lv1 regionsize --units s --nosuffix)" \
	-eq "$region"

vgremove -ff $vg
