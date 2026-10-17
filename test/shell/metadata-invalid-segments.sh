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

. lib/inittest --skip-with-lvmpolld

aux prepare_pvs 1
get_devs

vgcreate $SHARED "$vg" "$dev1"
lvcreate -an -l1 -n base "$vg"

PVID=$(pvs --noheadings -o uuid "$dev1" | tr -d ' ')

CRAFTED="$PWD/metadata-invalid-segments.d"
mkdir -p "$CRAFTED"
trap 'rm -rf "$CRAFTED"' EXIT

# ANT-2026-768S52OM: an LV with no segments.  The pvmoveN exception in the
# shared validator exists for the in-memory pvmove LV, not stored metadata.
vgcfgbackup -f "$CRAFTED/base" "$vg"
awk '
	/^[[:space:]]*logical_volumes \{/ {
		print
		print "\t\tpvmove0 {"
		print "\t\t\tid = \"xNTfBx-Y88c-VjSt-QD7z-dWnV-4ugf-ICB1jX\""
		print "\t\t\tstatus = [\"READ\", \"WRITE\", \"VISIBLE\"]"
		print "\t\t\tflags = []"
		print "\t\t\tsegment_count = 0"
		print "\t\t}"
		next
	}
	{ print }
' "$CRAFTED/base" > "$CRAFTED/empty"
not vgcfgrestore --test -f "$CRAFTED/empty" "$vg" 2>"$CRAFTED/empty.err"
grep "has no segment" "$CRAFTED/empty.err"

# ANT-2026-03DO1VQT: a non-raid segment that names a RAID metadata LV as one
# of its areas.  Importing the raid1 LV flags r_rmeta_0 as RAID_META; the
# integrity origin then reaches set_lv_segment_area_lv() with no meta_areas.
cat > "$CRAFTED/raidmeta.tmpl" <<'EOF'
guest {
	id = "6uJUeL-PtbF-NIdn-Zw07-hIUG-yzdU-Ru9RcH"
	seqno = 1
	format = "lvm2"
	status = ["RESIZEABLE", "READ", "WRITE"]
	flags = []
	extent_size = 8192
	max_lv = 0
	max_pv = 0
	metadata_copies = 0

	physical_volumes {
		pv0 {
			id = "KQ5kcB-85hM-mKmu-UA6a-7rN4-wrWL-JP2s6e"
			device = "/dev/sdx"
			status = ["ALLOCATABLE"]
			flags = []
			dev_size = 131072
			pe_start = 2048
			pe_count = 15
		}
	}

	logical_volumes {
		r {
			id = "AAAAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAAA1"
			status = ["READ", "WRITE", "VISIBLE"]
			flags = []
			segment_count = 1
			segment1 {
				start_extent = 0
				extent_count = 1
				type = "raid1"
				device_count = 2
				region_size = 1024
				raids = ["r_rmeta_0", "r_rimage_0", "r_rmeta_1", "r_rimage_1"]
			}
		}
		data {
			id = "AAAAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAAA2"
			status = ["READ", "WRITE", "VISIBLE"]
			flags = []
			segment_count = 1
			segment1 {
				start_extent = 0
				extent_count = 1
				type = "integrity"
				origin = "r_rmeta_0"
				data_sectors = 8192
				mode = "J"
				tag_size = 4
				block_size = 512
				internal_hash = "crc32c"
				meta_dev = "data_imeta"
			}
		}
		data_iorig {
			id = "AAAAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAAA3"
			status = ["READ", "WRITE"]
			flags = []
			segment_count = 1
			segment1 {
				start_extent = 0
				extent_count = 1
				type = "striped"
				stripe_count = 1
				stripes = ["pv0", 0]
			}
		}
		data_imeta {
			id = "AAAAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAAA4"
			status = ["READ", "WRITE"]
			flags = []
			segment_count = 1
			segment1 {
				start_extent = 0
				extent_count = 1
				type = "striped"
				stripe_count = 1
				stripes = ["pv0", 1]
			}
		}
		r_rmeta_0 {
			id = "AAAAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAAA5"
			status = ["READ", "WRITE"]
			flags = []
			segment_count = 1
			segment1 {
				start_extent = 0
				extent_count = 1
				type = "striped"
				stripe_count = 1
				stripes = ["pv0", 2]
			}
		}
		r_rimage_0 {
			id = "AAAAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAAA6"
			status = ["READ", "WRITE"]
			flags = []
			segment_count = 1
			segment1 {
				start_extent = 0
				extent_count = 1
				type = "striped"
				stripe_count = 1
				stripes = ["pv0", 3]
			}
		}
		r_rmeta_1 {
			id = "AAAAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAAA7"
			status = ["READ", "WRITE"]
			flags = []
			segment_count = 1
			segment1 {
				start_extent = 0
				extent_count = 1
				type = "striped"
				stripe_count = 1
				stripes = ["pv0", 4]
			}
		}
		r_rimage_1 {
			id = "AAAAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAAA8"
			status = ["READ", "WRITE"]
			flags = []
			segment_count = 1
			segment1 {
				start_extent = 0
				extent_count = 1
				type = "striped"
				stripe_count = 1
				stripes = ["pv0", 5]
			}
		}
	}
}
contents = "Text Format Volume Group"
version = 1
description = ""
creation_host = "elsewhere"
creation_time = 1700000000
EOF
sed -e "s/^guest {/$vg {/" -e "s#device = \"/dev/sdx\"#device = \"$dev1\"#" \
	-e "s/KQ5kcB-85hM-mKmu-UA6a-7rN4-wrWL-JP2s6e/$PVID/" \
	"$CRAFTED/raidmeta.tmpl" > "$CRAFTED/raidmeta"
not vgcfgrestore --test -f "$CRAFTED/raidmeta" "$vg" 2>"$CRAFTED/raidmeta.err"
grep "Cannot use RAID metadata LV" "$CRAFTED/raidmeta.err"

# ANT-2026-HCCPSV9Q: a thin LV that merges into a pool which has no origin.
# The sub-LV walk runs during import, before validation could reject the
# metadata; without the NULL guard it dereferences a NULL origin here.
cat > "$CRAFTED/thinmerge.tmpl" <<'EOF'
usbvg {
	id = "VGVGVG-VGVG-VGVG-VGVG-VGVG-VGVG-VGVGVG"
	seqno = 1
	format = "lvm2"
	status = ["RESIZEABLE", "READ", "WRITE"]
	flags = []
	extent_size = 8192
	max_lv = 0
	max_pv = 0
	metadata_copies = 0

	physical_volumes {
		pv0 {
			id = "PVPVPV-PVPV-PVPV-PVPV-PVPV-PVPV-PVPVPV"
			device = "/dev/sdz"
			status = ["ALLOCATABLE"]
			flags = []
			dev_size = 131072
			pe_start = 2048
			pe_count = 15
		}
	}

	logical_volumes {
		pool {
			id = "LVPOOL-LVPO-OLLV-POOL-LVPO-OLLV-POOLLV"
			status = ["READ", "WRITE", "VISIBLE"]
			flags = []
			segment_count = 1
			segment1 {
				start_extent = 0
				extent_count = 2
				type = "thin-pool"
				metadata = "pool_tmeta"
				pool = "pool_tdata"
				transaction_id = 1
				chunk_size = 128
				discards = "passdown"
			}
		}
		thin1 {
			id = "LVTHIN-LVTH-INLV-THIN-LVTH-INLV-THINLV"
			status = ["READ", "WRITE", "VISIBLE"]
			flags = []
			segment_count = 1
			segment1 {
				start_extent = 0
				extent_count = 2
				type = "thin"
				thin_pool = "pool"
				transaction_id = 0
				device_id = 1
				merge = "pool"
			}
		}
		pool_tmeta {
			id = "LVMETA-LVME-TALV-META-LVME-TALV-METALV"
			status = ["READ", "WRITE"]
			flags = []
			segment_count = 1
			segment1 {
				start_extent = 0
				extent_count = 1
				type = "striped"
				stripe_count = 1
				stripes = ["pv0", 0]
			}
		}
		pool_tdata {
			id = "LVDATA-LVDA-TALV-DATA-LVDA-TALV-DATALV"
			status = ["READ", "WRITE"]
			flags = []
			segment_count = 1
			segment1 {
				start_extent = 0
				extent_count = 2
				type = "striped"
				stripe_count = 1
				stripes = ["pv0", 1]
			}
		}
	}
}
contents = "Text Format Volume Group"
version = 1
description = ""
creation_host = "elsewhere"
creation_time = 1700000000
EOF
sed -e "s/^usbvg {/$vg {/" -e "s#device = \"/dev/sdz\"#device = \"$dev1\"#" \
	-e "s/PVPVPV-PVPV-PVPV-PVPV-PVPV-PVPV-PVPVPV/$PVID/" \
	"$CRAFTED/thinmerge.tmpl" > "$CRAFTED/thinmerge"
# The NULL origin would crash the import walk with SIGSEGV.  Disable the
# internal-error abort (the harness exports DM_ABORT_ON_INTERNAL_ERRORS=1,
# which overrides the config value) so a surviving walk fails cleanly and
# the exit status still tells the two apart.
rc=0
DM_ABORT_ON_INTERNAL_ERRORS=0 \
	vgcfgrestore --force --test -f "$CRAFTED/thinmerge" "$vg" \
	2>"$CRAFTED/thinmerge.err" || rc=$?
test "$rc" -lt 128

vgremove -ff "$vg"
