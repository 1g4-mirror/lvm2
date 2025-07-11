#!/usr/bin/env bash

# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA



. lib/inittest --skip-with-lvmpolld

aux prepare_vg 3

pvchange --metadataignore y "$dev1"

lvcreate -aey --type mirror -m 1 -l 1 -n mirror $vg
lvchange -a n $vg/mirror
lvcreate -l 1 -n lv1 $vg "$dev1"

# try to just change metadata; we expect the new version (with MISSING_PV set
# on the reappeared volume) to be written out to the previously missing PV
aux disable_dev "$dev1"
lvremove $vg/mirror
not vgck $vg 2>&1 | tee log
grep "missing 1 physical volume" log
not lvcreate -aey --type mirror -m 1 -l 1 -n mirror $vg # write operations fail
aux enable_dev "$dev1"
# Old versions would automatically clear MISSING_PV on a PV that had no mda,
# but this made no sense; the existence of an mda means nothing for the
# validity of the data on the device.  I suspect that at some point in the
# past, the MISSING_PV flag was used to decide if metadata could be used
# from the device, so the flag could be cleared on a PV with no mda.
# These days lvm knows when to ignore outdated metadata.
# MISSING_PV probably has little to no value for determining valid data either,
# so it's likely that we'll begin to automatically clear MISSING_PV in the
# future (but it will have nothing to do with having mdas.)
not lvcreate -aey --type mirror -m 1 -l 1 -n mirror $vg 
vgextend --restoremissing $vg "$dev1"
lvcreate -aey --type mirror -m 1 -l 1 -n mirror $vg 
vgck $vg

vgremove -ff $vg
