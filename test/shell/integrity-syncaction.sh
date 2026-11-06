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

aux have_fs ext4 || skip
aux have_integrity 1 5 0 || skip
aux devloc_bin > /dev/null || skip "devloc not available"
# devloc observes block_rq_issue events; brd ramdisk devices process
# bios without a request queue and emit none, so use loop devices
export LVM_TEST_PREFER_BRD=0

mnt="mnt"
mkdir -p "$mnt"

aux prepare_devs 3 40

# Generate standard test data, but use custom fileB pattern for corruption testing
awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
awk 'BEGIN { while (z++ < 4096) printf "B" ; while (z++ < 16384) printf "b" }' > fileB
awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC

_prepare_vg() {
	# zero devs for a clean start (file data is located on the
	# underlying devs with devloc, not by searching for patterns)
	aux clear_devs "$dev1" "$dev2" "$dev3"
	vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"
	pvs
}

_test1() {
	mkfs.ext4 -b 4096 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"

	cp fileA "$mnt"
	cp fileB "$mnt"
	cp fileC "$mnt"

	# map fileB's first block to a device sector while the fs is
	# mounted
	blocks=$(aux devloc_file_blocks "$mnt/fileB" 1)

	umount "$mnt"
	lvchange -an $vg/$lv1

	# reactivate so caching layers (dm-integrity journal) are empty and
	# devloc's probes are served from the data area on disk
	lvchange -ay $vg/$lv1
	devlocs=$(aux devloc_sector_locations "$DM_DEV_DIR/$vg/$lv1" $blocks)
	lvchange -an $vg/$lv1

	# Corrupting raid1 is simple - 1 leg needs to be modified
	# For raid5 corrupted block can be placed on any of its leg.
	for i in "$@" ; do
		aux corrupt_file_locations "$devlocs" "$i" BBBBBBBBCBBBBBBBB |& tee out
		grep -q "corrupted" out && break  # leg found and corrupted
	done

	lvchange -ay $vg/$lv1

	# so without synchecking the array - integrity doesn't know yet about failure
	check lv_field $vg/${lv1}_rimage_0 integritymismatches "0"
	check lv_field $vg/${lv1}_rimage_1 integritymismatches "0"
	[ $# -gt 2 ] && check lv_field $vg/${lv1}_rimage_2 integritymismatches "0"

	lvchange --syncaction check $vg/$lv1

	aux wait_recalc $vg/$lv1

	# after synaction check - integrity should recognize faulty devices
	baddev=0
	for i in 0 1 2 ; do
		[ "$i" -gt 1 ] && [ $# -lt 3 ] && continue  # only  raid5 has rimage_2
		[ "$(get lv_field $vg/${lv1}_rimage_${i} integritymismatches)" = "0" ] || baddev=$(( baddev + 1 ))
	done
	# ideally baddev should be 1, but raid5 syncaction can report
	# integrity mismatches on non-corrupted legs in the parity group
	[ "$baddev" -ge 1 ] || die "Unexpected number of integritymismatched devices ($baddev)!"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
	cmp -b "$mnt/fileA" fileA
	cmp -b "$mnt/fileB" fileB
	cmp -b "$mnt/fileC" fileC
	umount "$mnt"
}

_test2() {
	mkfs.ext4 -b 4096 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"

	cp fileA "$mnt"
	cp fileB "$mnt"
	cp fileC "$mnt"

	# map the files' blocks to device sectors while the fs is mounted
	blocksA=$(aux devloc_file_blocks "$mnt/fileA" 1)
	blocksB=$(aux devloc_file_blocks "$mnt/fileB" 1)
	blocksC=$(aux devloc_file_blocks "$mnt/fileC" 1)

	umount "$mnt"
	lvchange -an $vg/$lv1

	# reactivate so caching layers (dm-integrity journal) are empty and
	# devloc's probes are served from the data area on disk
	lvchange -ay $vg/$lv1

	# raid1 mirrors serve reads from a single leg, so force reads to
	# each leg in turn by marking the other leg writemostly while
	# probing: locate fileB and fileC on dev1's leg, fileA on dev2's
	lvchange $vg/$lv1 --writemostly "$dev2"
	devlocB=$(aux devloc_sector_locations "$DM_DEV_DIR/$vg/$lv1" $blocksB)
	devlocC=$(aux devloc_sector_locations "$DM_DEV_DIR/$vg/$lv1" $blocksC)
	lvchange $vg/$lv1 --writemostly "$dev2:n"
	lvchange $vg/$lv1 --writemostly "$dev1"
	devlocA=$(aux devloc_sector_locations "$DM_DEV_DIR/$vg/$lv1" $blocksA)
	lvchange $vg/$lv1 --writemostly "$dev1:n"
	lvchange -an $vg/$lv1

	# corrupt fileB and fileC on dev1
	aux corrupt_file_locations "$devlocB" "$dev1" BBBBBBBBCBBBBBBBB
	aux corrupt_file_locations "$devlocC" "$dev1" DDDDDDDDDDDDDDDDD

	# corrupt fileA on dev2
	aux corrupt_file_locations "$devlocA" "$dev2" AAAAAAAAAAAAAAEAA

	lvchange -ay $vg/$lv1

	check lv_field $vg/${lv1}_rimage_0 integritymismatches "0"
	check lv_field $vg/${lv1}_rimage_1 integritymismatches "0"

	lvchange --syncaction check $vg/$lv1

	aux wait_recalc $vg/$lv1

	not check lv_field $vg/${lv1}_rimage_0 integritymismatches "0"
	not check lv_field $vg/${lv1}_rimage_1 integritymismatches "0"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
	cmp -b "$mnt/fileA" fileA
	cmp -b "$mnt/fileB" fileB
	cmp -b "$mnt/fileC" fileC
	umount "$mnt"
}

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 6 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
# no writemostly here, so reads (and devloc's locations) may be served
# by either raid1 leg: try both
_test1 "$dev1" "$dev2"
not check lv_field $vg/$lv1 integritymismatches "0"
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 6 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
_test2
not check lv_field $vg/$lv1 integritymismatches "0"
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

# Test syncaction repair with raid+integrity
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 6 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
mkfs.ext4 -b 4096 "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
cp fileA "$mnt"
cp fileB "$mnt"
cp fileC "$mnt"
blocks=$(aux devloc_file_blocks "$mnt/fileB" 1)
umount "$mnt"
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
devlocs=$(aux devloc_sector_locations "$DM_DEV_DIR/$vg/$lv1" $blocks)
lvchange -an $vg/$lv1
# no writemostly here, so corrupt whichever leg holds the data
for dev in "$dev1" "$dev2"; do
	aux corrupt_file_locations "$devlocs" "$dev" BBBBBBBBCBBBBBBBB |& tee out
	grep -q "corrupted" out && break
done
lvchange -ay $vg/$lv1
lvchange --syncaction check $vg/$lv1
aux wait_recalc $vg/$lv1
not check lv_field $vg/$lv1 raid_mismatch_count "0"
lvchange --syncaction repair $vg/$lv1
aux wait_recalc $vg/$lv1
lvchange --syncaction check $vg/$lv1
aux wait_recalc $vg/$lv1
check lv_field $vg/$lv1 raid_mismatch_count "0"
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
cmp -b "$mnt/fileA" fileA
cmp -b "$mnt/fileB" fileB
cmp -b "$mnt/fileC" fileC
umount "$mnt"
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

# Test syncaction repair for raid mismatch without integrity errors.
# Write to one rimage through its integrity layer while the raid is
# suspended, so both images have valid integrity checksums but the
# data differs between them.
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 6 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
mkfs.ext4 -b 4096 "$DM_DEV_DIR/$vg/$lv1"
dmsetup suspend ${vg}-${lv1}
dd if=/dev/urandom of="$DM_DEV_DIR/mapper/${vg}-${lv1}_rimage_0" bs=4096 count=1 seek=10 oflag=direct
dmsetup resume ${vg}-${lv1}
check lv_field $vg/${lv1}_rimage_0 integritymismatches "0"
check lv_field $vg/${lv1}_rimage_1 integritymismatches "0"
lvchange --syncaction check $vg/$lv1
aux wait_recalc $vg/$lv1
not check lv_field $vg/$lv1 raid_mismatch_count "0"
check lv_field $vg/${lv1}_rimage_0 integritymismatches "0"
check lv_field $vg/${lv1}_rimage_1 integritymismatches "0"
lvchange --syncaction repair $vg/$lv1
aux wait_recalc $vg/$lv1
lvchange --syncaction check $vg/$lv1
aux wait_recalc $vg/$lv1
check lv_field $vg/$lv1 raid_mismatch_count "0"
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -I 4K -l 6 $vg "$dev1" "$dev2" "$dev3"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
_test1 "$dev1" "$dev2" "$dev3"
not check lv_field $vg/$lv1 integritymismatches "0"
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg
