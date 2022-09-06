#!/usr/bin/env bash

# Copyright (C) 2007-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA


SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_vg 3 256

# Test combinations of the following:
# lvreduce / lvextend
# no fs / ext4 / xfs
# each --fs opt / no --fs opt / --resizefs
# active / inactive
# mounted / unmounted
# fs size less than, equal to or greater than reduced lv size

mount_dir="mnt"
mkdir -p "$mount_dir"


#
# lvextend, no fs
#

# lvextend, no fs, active, no --fs
lvcreate -n $lv -L 256M $vg
aux wipefs_a "$DM_DEV_DIR/$vg/$lv"
lvextend -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, no fs, inactive, no --fs
lvcreate -n $lv -L 256M $vg
aux wipefs_a "$DM_DEV_DIR/$vg/$lv"
lvchange -an $vg/$lv
lvextend -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
lvremove $vg/$lv

# lvextend, no fs, active, --fs resize fails with no fs found
lvcreate -n $lv -L 256M $vg
aux wipefs_a "$DM_DEV_DIR/$vg/$lv"
not lvextend -L+200M --fs resize $vg/$lv
check lv_field $vg/$lv lv_size "256.00m"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, no fs, inactive, --fs resize error requires active lv
lvcreate -n $lv -L 256M $vg
aux wipefs_a "$DM_DEV_DIR/$vg/$lv"
lvchange -an $vg/$lv
not lvextend -L+200M --fs resize $vg/$lv
check lv_field $vg/$lv lv_size "256.00m"
lvremove $vg/$lv

#
# lvextend, ext4
#

# lvextend, ext4, active, mounted, no --fs setting is same as --fs ignore
lvcreate -n $lv -L 256M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
# with no --fs used, the fs size should be the same
df --output=size "$mount_dir" |tee df2
diff df1 df2
resize2fs "$DM_DEV_DIR/$vg/$lv"
df --output=size "$mount_dir" |tee df3
not diff df2 df3
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, ext4, inactive, --fs ignore
lvcreate -n $lv -L 256M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
lvchange -an $vg/$lv
lvextend --fs ignore -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
lvremove $vg/$lv

# lvextend, ext4, active, mounted, --fs ignore
lvcreate -n $lv -L 256M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
lvextend --fs ignore -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, ext4, active, mounted, --fs resize
lvcreate -n $lv -L 256M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend --fs resize -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, ext4, active, mounted, --resizefs (same as --fs resize)
lvcreate -n $lv -L 256M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend --resizefs -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, ext4, active, mounted, --fs resize_remount (same as --fs resize)
lvcreate -n $lv -L 256M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend --fs resize_remount -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, ext4, active, mounted, --fs resize_unmount
lvcreate -n $lv -L 256M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend --fs resize_unmount -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
# resize_unmount leaves fs unmounted 
df -a | tee dfa
not grep "$mount_dir" dfa
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, ext4, active, mounted, --fs resize_keepmount
lvcreate -n $lv -L 256M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend --fs resize_keepmount -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, ext4, active, mounted, --fs resize_fsadm
lvcreate -n $lv -L 256M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend --fs resize_fsadm -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, ext4, active, unmounted, --fs resize_keepmount
lvcreate -n $lv -L 256M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvextend --fs resize_keepmount -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, ext4, active, unmounted, --fs resize
lvcreate -n $lv -L 256M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvextend --fs resize -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, ext4, active, unmounted, --fs resize_fsadm
lvcreate -n $lv -L 256M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvextend --fs resize_fsadm -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv


#
# lvextend, xfs
#

# lvextend, xfs, active, mounted, no --fs setting is same as --fs ignore
lvcreate -n $lv -L 256M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
# with no --fs used, the fs size should be the same
df --output=size "$mount_dir" |tee df2
diff df1 df2
xfs_growfs "$DM_DEV_DIR/$vg/$lv"
df --output=size "$mount_dir" |tee df3
not diff df2 df3
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, xfs, inactive, --fs ignore
lvcreate -n $lv -L 256M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
lvchange -an $vg/$lv
lvextend --fs ignore -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
lvremove $vg/$lv

# lvextend, xfs, active, mounted, --fs ignore
lvcreate -n $lv -L 256M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
lvextend --fs ignore -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, xfs, active, mounted, --fs resize
lvcreate -n $lv -L 256M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend --fs resize -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, xfs, active, mounted, --resizefs (same as --fs resize)
lvcreate -n $lv -L 256M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend --resizefs -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, xfs, active, mounted, --fs resize_remount (same as --fs resize)
lvcreate -n $lv -L 256M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend --fs resize_remount -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, xfs, active, mounted, --fs resize_unmount
lvcreate -n $lv -L 256M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
# xfs_growfs requires the fs to be mounted, so extending the lv is
# succeeds, then the xfs extend fails because it cannot be done unmounted
not lvextend --fs resize_unmount -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df -a | tee dfa
grep "$mount_dir" dfa
df --output=size "$mount_dir" |tee df2
# fs not extended so fs size not changed
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, xfs, active, mounted, --fs resize_keepmount
lvcreate -n $lv -L 256M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend --fs resize_keepmount -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, xfs, active, mounted, --fs resize_fsadm
lvcreate -n $lv -L 256M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
lvextend --fs resize_fsadm -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, xfs, active, unmounted, --fs resize_keepmount
lvcreate -n $lv -L 256M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
# xfs_growfs requires the fs to be mounted to grow, so resize_keepmount
# with an unmounted fs fails
not lvextend --fs resize_keepmount -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs not extended so fs size not changed
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, xfs, active, unmounted, --fs resize
lvcreate -n $lv -L 256M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
# --yes needed because mount changes are required and plain "resize"
# fsopt did not specify if the user wants to change mount state
lvextend --yes --fs resize -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvextend, xfs, active, unmounted, --fs resize_fsadm
lvcreate -n $lv -L 256M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df1
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvextend --fs resize_fsadm -L+200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
not diff df1 df2
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=200 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv


#
# lvreduce, no fs
#

# lvreduce, no fs, active, no --fs setting is same as --fs checksize
lvcreate -n $lv -L 456M $vg
aux wipefs_a "$DM_DEV_DIR/$vg/$lv"
lvreduce -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "256.00m"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, no fs, inactive, no --fs setting is same as --fs checksize
lvcreate -n $lv -L 456M $vg
aux wipefs_a "$DM_DEV_DIR/$vg/$lv"
lvchange -an $vg/$lv
lvreduce -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "256.00m"
lvremove $vg/$lv

# lvreduce, no fs, active, --fs resize requires fs to be found
lvcreate -n $lv -L 456M $vg
aux wipefs_a "$DM_DEV_DIR/$vg/$lv"
not lvreduce -L-200M --fs resize $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, no fs, inactive, --fs ignore
lvcreate -n $lv -L 456M $vg
aux wipefs_a "$DM_DEV_DIR/$vg/$lv"
lvchange -an $vg/$lv
lvreduce -L-200M --fs ignore $vg/$lv
check lv_field $vg/$lv lv_size "256.00m"
lvchange -an $vg/$lv
lvremove $vg/$lv


#
# lvreduce, ext4, no --fs setting and the equivalent --fs checksize
# i.e. fs is not resized
#

# lvreduce, ext4, active, mounted, no --fs setting is same as --fs checksize
# fs smaller than the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# fs is 200M, reduced size is 216M, so no fs reduce is needed
# todo: check that resize2fs was not run?
lvreduce -L216M $vg/$lv
check lv_field $vg/$lv lv_size "216.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, mounted, no --fs setting is same as --fs checksize
# fs equal to the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# fs is 200M, reduced size is 200M, so no fs reduce is needed
# todo: check that resize2fs was not run?
lvreduce -L200M $vg/$lv
check lv_field $vg/$lv lv_size "200.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, mounted, no --fs setting is same as --fs checksize
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# lvreduce fails because fs needs to be reduced and checksize does not resize
not lvreduce -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, mounted, no --fs setting is same as --fs checksize
# fs larger than the reduced size, fs is using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=300 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# lvreduce fails because fs needs to be reduced and checksize does not resize
not lvreduce -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# repeat lvreduce tests with unmounted instead of mounted fs

# lvreduce, ext4, active, unmounted, no --fs setting is same as --fs checksize
# fs smaller than the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# fs is 200M, reduced size is 216M, so no fs reduce is needed
lvreduce -L216M $vg/$lv
check lv_field $vg/$lv lv_size "216.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, unmounted, no --fs setting is same as --fs checksize
# fs equal to the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# fs is 200M, reduced size is 200M, so no fs reduce is needed
lvreduce -L200M $vg/$lv
check lv_field $vg/$lv lv_size "200.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, unmounted, no --fs setting is same as --fs checksize
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# lvreduce fails because fs needs to be reduced and checksize does not resize
not lvreduce -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, unmounted, no --fs setting is same as --fs checksize
# fs larger than the reduced size, fs is using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=300 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# lvreduce fails because fs needs to be reduced and checksize does not resize
not lvreduce -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# repeat a couple prev lvreduce that had no --fs setting,
# now using --fs checksize to verify it's the same as using no --fs set

# lvreduce, ext4, active, mounted, --fs checksize (same as no --fs set)
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# lvreduce fails because fs needs to be reduced and checksize does not resize
not lvreduce --fs checksize -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, unmounted, --fs checksize (same as no --fs set)
# fs smaller than the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# fs is 200M, reduced size is 216M, so no fs reduce is needed
lvreduce --fs checksize -L216M $vg/$lv
check lv_field $vg/$lv lv_size "216.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce with inactive and no --fs setting fails because
# default behavior is fs checksize which activates the LV
# and sees the fs

# lvreduce, ext4, inactive, no --fs setting same as --fs checksize
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
lvchange -an $vg/$lv
# lvreduce fails because default is --fs checksize which sees the fs
not lvreduce -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
lvremove $vg/$lv

#
# lvreduce, ext4, --fs resize*
#

# lvreduce, ext4, active, mounted, --fs resize
# fs smaller than the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# fs is 200M, reduced size is 216M, so no fs reduce is needed
lvreduce -L216M $vg/$lv
check lv_field $vg/$lv lv_size "216.00m"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, mounted, --fs resize
# fs equal to the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# fs is 200M, reduced size is 200M, so no fs reduce is needed
lvreduce -L200M $vg/$lv
check lv_field $vg/$lv lv_size "200.00m"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, mounted, --fs resize
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# lvreduce runs resize2fs to shrink the fs
lvreduce --yes --fs resize -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "256.00m"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df2
# fs size is changed
not diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, mounted, --fs resize
# fs larger than the reduced size, fs is using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=300 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# lvreduce runs resize2fs to shrink the fs but resize2fs fails
# the fs is not remounted after resize2fs fails because the
# resize failure might leave the fs in an unknown state
not lvreduce --yes --fs resize -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
ls -l $mount_dir/zeros1
ls -l $mount_dir/zeros2
df --output=size "$mount_dir" |tee df2
# fs size is unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# repeat with unmounted instead of mounted

# lvreduce, ext4, active, unmounted, --fs resize
# fs smaller than the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# fs is 200M, reduced size is 216M, so no fs reduce is needed
lvreduce -L216M $vg/$lv
check lv_field $vg/$lv lv_size "216.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, unmounted, --fs resize
# fs equal to the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# fs is 200M, reduced size is 200M, so no fs reduce is needed
lvreduce -L200M $vg/$lv
check lv_field $vg/$lv lv_size "200.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, unmounted, --fs resize
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# lvreduce runs resize2fs to shrink the fs
lvreduce --fs resize -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "256.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df2
# fs size is changed
not diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, unmounted, --fs resize
# fs larger than the reduced size, fs is using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
dd if=/dev/zero of="$mount_dir/zeros2" bs=1M count=300 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# lvreduce runs resize2fs to shrink the fs but resize2fs fails
not lvreduce --yes --fs resize -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df2
# fs size is unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# repeat resizes that shrink the fs, replacing --fs resize with
# --fs resize_keepmount | resize_unmount | resize_fsadm
# while mounted and unmounted

# lvreduce, ext4, active, mounted, --fs resize_keepmount
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# lvreduce needs to unmount to run resize2fs but keepmount doesn't let it
not lvreduce --fs resize_keepmount -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
# fs size is unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, unmounted, --fs resize_keepmount
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# lvreduce runs resize2fs to shrink the fs
lvreduce --fs resize_keepmount -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "256.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size is changed
not diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, mounted, --fs resize_unmount
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# lvreduce runs resize2fs to shrink the fs
# resize_unmount leaves the fs unmounted
lvreduce --fs resize_unmount -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "256.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size is changed
not diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, unmounted, --fs resize_unmount
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# lvreduce runs resize2fs to shrink the fs
lvreduce --fs resize_unmount -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "256.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size is changed
not diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, mounted, --fs resize_fsadm
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# lvreduce runs resize2fs to shrink the fs
lvreduce --yes --fs resize_fsadm -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "256.00m"
ls -l $mount_dir/zeros1
df --output=size "$mount_dir" |tee df2
# fs size is changed
not diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, ext4, active, unmounted, --fs resize_fsadm
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# lvreduce runs resize2fs to shrink the fs
lvreduce --fs resize_fsadm -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "256.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size is changed
not diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

#
# lvreduce, xfs (xfs does not support shrinking)
#

# lvreduce, xfs, active, mounted, no --fs setting is same as --fs checksize
# fs smaller than the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# fs is 200M, reduced size is 216M, so no fs reduce is needed
lvreduce -L216M $vg/$lv
check lv_field $vg/$lv lv_size "216.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, xfs, active, mounted, no --fs setting is same as --fs checksize
# fs equal to the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# fs is 200M, reduced size is 200M, so no fs reduce is needed
lvreduce -L200M $vg/$lv
check lv_field $vg/$lv lv_size "200.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, xfs, active, mounted, no --fs setting is same as --fs checksize
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# lvreduce fails because fs needs to be reduced
not lvreduce -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, xfs, active, unmounted, no --fs setting is same as --fs checksize
# fs smaller than the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# fs is 200M, reduced size is 216M, so no fs reduce is needed
lvreduce -L216M $vg/$lv
check lv_field $vg/$lv lv_size "216.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, xfs, active, mounted, --fs resize
# fs smaller than the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# fs is 200M, reduced size is 216M, so no fs reduce is needed
lvreduce -L216M $vg/$lv
check lv_field $vg/$lv lv_size "216.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, xfs, active, mounted, --fs resize
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
# lvreduce fails because xfs cannot shrink
not lvreduce --yes --fs resize -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, xfs, active, unmounted, --fs resize*
# fs larger than the reduced size, fs not using reduced space
lvcreate -n $lv -L 456M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
# lvreduce fails because xfs cannot shrink
not lvreduce --yes --fs resize -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
not lvreduce --yes --fs resize_remount -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
not lvreduce --yes --fs resize_keepmount -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
not lvreduce --yes --fs resize_unmount -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
not lvreduce --yes --fs resize_fsadm -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
not lvreduce --yes --resizefs -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, xfs, inactive, no --fs setting is same as --fs checksize
# fs equal to the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
lvchange -an $vg/$lv
# no fs reduce is needed
lvreduce -L200M $vg/$lv
check lv_field $vg/$lv lv_size "200.00m"
lvchange -ay $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, xfs, inactive, no fs setting is same as --fs checksize
# fs smaller than the reduced size
lvcreate -n $lv -L 200M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
lvextend -L+256M $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=128 conv=fdatasync
df --output=size "$mount_dir" |tee df1
umount "$mount_dir"
lvchange -an $vg/$lv
# fs is 200M, reduced size is 216M, so no fs reduce is needed
lvreduce -L216M $vg/$lv
check lv_field $vg/$lv lv_size "216.00m"
lvchange -ay $vg/$lv
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
df --output=size "$mount_dir" |tee df2
# fs size unchanged
diff df1 df2
umount "$mount_dir"
lvchange -an $vg/$lv
lvremove $vg/$lv

# lvreduce, xfs, inactive, no --fs setting is same as --fs checksize
# fs larger than the reduced size
lvcreate -n $lv -L 456M $vg
mkfs.xfs "$DM_DEV_DIR/$vg/$lv"
mount "$DM_DEV_DIR/$vg/$lv" "$mount_dir"
dd if=/dev/zero of="$mount_dir/zeros1" bs=1M count=100 conv=fdatasync
umount "$mount_dir"
lvchange -an $vg/$lv
# lvreduce fails because fs needs to be reduced
not lvreduce -L-200M $vg/$lv
check lv_field $vg/$lv lv_size "456.00m"
lvremove $vg/$lv

vgremove -ff $vg
