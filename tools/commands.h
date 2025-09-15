/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2014 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

xx(config,
   "Display and manipulate configuration information",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING,
   CATEGORY_CONFIGURATION)

xx(devtypes,
   "Display recognised built-in block device types",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING,
   CATEGORY_BUILTIN)

xx(dumpconfig,
   "Display and manipulate configuration information",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING,
   CATEGORY_CONFIGURATION)

xx(formats,
   "List available metadata formats",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING,
   CATEGORY_BUILTIN)

xx(fullreport,
   "Display full report",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | ALLOW_HINTS | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND,
   CATEGORY_REPORTING)

xx(help,
   "Display help for commands",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING,
   CATEGORY_BUILTIN)

xx(lastlog,
   "Display last command's log report",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING,
   CATEGORY_BUILTIN)

xx(lvchange,
   "Change the attributes of logical volume(s)",
   PERMITTED_READ_ONLY | ALLOW_HINTS,
   CATEGORY_LV)

xx(lvconvert,
   "Change logical volume layout",
   GET_VGNAME_FROM_OPTIONS,
   CATEGORY_LV)

xx(lvcreate,
   "Create a logical volume",
   ALLOW_HINTS | ALTERNATIVE_EXTENTS,
   CATEGORY_LV)

xx(lvdisplay,
   "Display information about a logical volume",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | CAN_USE_ONE_SCAN | ALLOW_HINTS | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND,
   CATEGORY_REPORTING)

xx(lvextend,
   "Add space to a logical volume",
   ALLOW_HINTS | ALTERNATIVE_EXTENTS,
   CATEGORY_LV)

xx(lvmchange,
   "With the device mapper, this is obsolete and does nothing.",
   0,
   CATEGORY_OTHER)

xx(lvmconfig,
   "Display and manipulate configuration information",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING,
   CATEGORY_CONFIGURATION)

xx(lvmdevices,
   "Manage the devices file",
   DEVICE_ID_NOT_FOUND,
   CATEGORY_DEVICES_FILE)

xx(lvmdiskscan,
   "List devices that may be used as physical volumes",
   PERMITTED_READ_ONLY | ENABLE_ALL_DEVS | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND,
   CATEGORY_SCANNING)

xx(lvmsadc,
   "Collect activity data",
   0,
   CATEGORY_OTHER)

xx(lvmsar,
   "Create activity report",
   0,
   CATEGORY_OTHER)

xx(lvpoll,
   "Continue already initiated poll operation on a logical volume",
   0,
   CATEGORY_UTILITY)

xx(lvreduce,
   "Reduce the size of a logical volume",
   ALLOW_HINTS | ALTERNATIVE_EXTENTS,
   CATEGORY_LV)

xx(lvremove,
   "Remove logical volume(s) from the system",
   ALL_VGS_IS_DEFAULT | ALLOW_HINTS,
   CATEGORY_LV) /* all VGs only with --select */

xx(lvrename,
   "Rename a logical volume",
   ALLOW_HINTS,
   CATEGORY_LV)

xx(lvresize,
   "Resize a logical volume",
   ALLOW_HINTS | ALTERNATIVE_EXTENTS,
   CATEGORY_LV)

xx(lvs,
   "Display information about logical volumes",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | CAN_USE_ONE_SCAN | ALLOW_HINTS | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND,
   CATEGORY_REPORTING)

xx(lvscan,
   "List all logical volumes in all volume groups",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND,
   CATEGORY_SCANNING)

xx(pvchange,
   "Change attributes of physical volume(s)",
   0,
   CATEGORY_PV)

xx(pvck,
   "Check metadata on physical volumes",
   LOCKD_VG_SH | ALLOW_EXPORTED,
   CATEGORY_CHECK)

xx(pvcreate,
   "Initialize physical volume(s) for use by LVM",
   ENABLE_ALL_DEVS,
   CATEGORY_PV)

xx(pvdata,
   "Display the on-disk metadata for physical volume(s)",
   0,
   CATEGORY_OTHER)

xx(pvdisplay,
   "Display various attributes of physical volume(s)",
   PERMITTED_READ_ONLY | ENABLE_ALL_DEVS | ENABLE_DUPLICATE_DEVS | LOCKD_VG_SH | CAN_USE_ONE_SCAN | ALLOW_HINTS | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND,
   CATEGORY_REPORTING)

/* ALL_VGS_IS_DEFAULT is for polldaemon to find pvmoves in-progress using process_each_vg. */

xx(pvmove,
   "Move extents from one physical volume to another",
   ALL_VGS_IS_DEFAULT | DISALLOW_TAG_ARGS,
   CATEGORY_PV)

xx(pvremove,
   "Remove LVM label(s) from physical volume(s)",
   ENABLE_ALL_DEVS,
   CATEGORY_PV)

xx(pvresize,
   "Resize physical volume(s)",
   0,
   CATEGORY_PV)

xx(pvs,
   "Display information about physical volumes",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | ENABLE_ALL_DEVS | ENABLE_DUPLICATE_DEVS | LOCKD_VG_SH | CAN_USE_ONE_SCAN | ALLOW_HINTS | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND,
   CATEGORY_REPORTING)

xx(pvscan,
   "List all physical volumes",
   PERMITTED_READ_ONLY | LOCKD_VG_SH | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND,
   CATEGORY_SCANNING)

xx(segtypes,
   "List available segment types",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING,
   CATEGORY_BUILTIN)

xx(systemid,
   "Display the system ID, if any, currently set on this host",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING,
   CATEGORY_BUILTIN)

xx(tags,
   "List tags defined on this host",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING,
   CATEGORY_BUILTIN)

xx(version,
   "Display software and driver version information",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING,
   CATEGORY_BUILTIN)

xx(vgcfgbackup,
   "Backup volume group configuration(s)",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | ALLOW_EXPORTED,
   CATEGORY_VG)

xx(vgcfgrestore,
   "Restore volume group configuration",
   ALLOW_EXPORTED,
   CATEGORY_VG)

xx(vgchange,
   "Change volume group attributes",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | ALLOW_HINTS,
   CATEGORY_VG)

xx(vgck,
   "Check the consistency of volume group(s)",
   ALL_VGS_IS_DEFAULT | LOCKD_VG_SH,
   CATEGORY_CHECK)

xx(vgconvert,
   "Change volume group metadata format",
   0,
   CATEGORY_VG)

xx(vgcreate,
   "Create a volume group",
   MUST_USE_ALL_ARGS | ENABLE_ALL_DEVS,
   CATEGORY_VG)

xx(vgdisplay,
   "Display volume group information",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | CAN_USE_ONE_SCAN | ALLOW_HINTS | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND,
   CATEGORY_REPORTING)

xx(vgexport,
   "Unregister volume group(s) from the system",
   ALL_VGS_IS_DEFAULT,
   CATEGORY_VG)

xx(vgextend,
   "Add physical volumes to a volume group",
   MUST_USE_ALL_ARGS | ENABLE_ALL_DEVS,
   CATEGORY_VG)

xx(vgimport,
   "Register exported volume group with system",
   ALL_VGS_IS_DEFAULT | ALLOW_EXPORTED,
   CATEGORY_VG)

xx(vgimportclone,
   "Import a VG from cloned PVs",
   ALLOW_EXPORTED,
   CATEGORY_VG)

xx(vgimportdevices,
   "Add devices for a VG to the devices file.",
   ALL_VGS_IS_DEFAULT | ALLOW_EXPORTED,
   CATEGORY_DEVICES_FILE)

xx(vgmerge,
   "Merge volume groups",
   0,
   CATEGORY_VG)

xx(vgmknodes,
   "Create the special files for volume group devices in /dev",
   ALL_VGS_IS_DEFAULT,
   CATEGORY_VG)

xx(vgreduce,
   "Remove physical volume(s) from a volume group",
   0,
   CATEGORY_VG)

xx(vgremove,
   "Remove volume group(s)",
   ALL_VGS_IS_DEFAULT,
   CATEGORY_VG) /* all VGs only with select */

xx(vgrename,
   "Rename a volume group",
   ALLOW_UUID_AS_NAME | ALLOW_EXPORTED,
   CATEGORY_VG)

xx(vgs,
   "Display information about volume groups",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | CAN_USE_ONE_SCAN | ALLOW_HINTS | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND,
   CATEGORY_REPORTING)

xx(vgscan,
   "Search for all volume groups",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND,
   CATEGORY_SCANNING)

xx(vgsplit,
   "Move physical volumes into a new or existing volume group",
   0,
   CATEGORY_VG)
