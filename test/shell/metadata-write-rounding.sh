#!/usr/bin/env bash

# Copyright (C) 2026 Itai Handler. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# lvm writes a metadata update back from data it read earlier, up to the
# end of the metadata area, rounded up to whole blocks for direct I/O.
# With 1024- or 2048-byte logical and 4096-byte physical blocks, the
# first PE can start at an offset that is not a multiple of 4096.  Check
# that a metadata update does not undo a write to an LV on that PE, made
# while the command waits for confirmation.


. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

mkfifo fifo

for bs in 1024 2048 ; do
	aux prepare_scsi_debug_dev 64 sector_size=$bs \
		physblk_exp=$(( bs == 1024 ? 2 : 1 )) || skip
	check sysfs "$(< SCSI_DEBUG_DEV)" queue/logical_block_size $bs
	check sysfs "$(< SCSI_DEBUG_DEV)" queue/physical_block_size 4096
	aux prepare_pvs 1 60
	get_devs

	# The metadata area ends, and the first PE starts, one logical
	# block after 64KiB.
	pvcreate --metadatasize 8k --dataalignment 64k \
		--dataalignmentoffset $(( bs / 1024 ))k "$dev1"
	check pv_field "$dev1" pe_start $(( 64 + bs / 1024 )).00k
	vgcreate $SHARED $vg "$dev1"
	lvcreate -l1 -n $lv1 $vg "$dev1:0"
	lvcreate -l1 -n $lv2 $vg

	head -c $bs /dev/zero | tr '\0' A > A
	head -c $bs /dev/zero | tr '\0' B > B
	dd if=A of="$DM_DEV_DIR/$vg/$lv1" bs=$bs count=1 oflag=direct

	# lvremove reads the start of the PV before asking, and writes the
	# metadata after.
	lvremove $vg/$lv2 <fifo 2>err &
	PID=$!
	exec 3>fifo
	for i in {1..300} ; do
		grep -q "really want" err && break
		sleep .1
	done
	dd if=B of="$DM_DEV_DIR/$vg/$lv1" bs=$bs count=1 oflag=direct
	echo y >&3
	exec 3>&-
	wait $PID
	grep "really want" err

	dd if="$DM_DEV_DIR/$vg/$lv1" of=data bs=$bs count=1 iflag=direct
	cmp data B

	vgremove -ff $vg
	aux cleanup_scsi_debug_dev
done
