#!/bin/bash
#
# devloc raid test harness
#
# Verifies devloc's empirical mapping against every lvm raid type by
# building raid LVs with known geometry and checking devloc's observed
# device+sector against the md/dm-raid layout math:
#
#   raid0  top-level "striped" target, stripe math
#   raid1  every leg at rimage_start + fs_sector (1:1)
#   raid4  parity on the last leg (ALGORITHM_PARITY_N), data legs in order
#   raid5  left-symmetric (raid5_ls): parity rotates, data after parity
#   raid6  relaxed: any leg at row*chunk+within (layout not hard-coded),
#          discriminated by content and nonce
#   raid10 near layout: stripes = N/2, copies on legs i and i+S
#
# Also verifies, per type: the write path with --nonce (data copy found,
# original restored), and the row/within stripe arithmetic of every
# observed location.
#
# Requirements: root, four spare disks, lvm, mkfs.xfs, filefrag, devloc.
#
# Usage: test-devloc-raid.sh [disk1 disk2 disk3 disk4]

set -u

VG=devlocvg
DEVLOC=${DEVLOC:-/root/devloc}
MNT=/mnt/devloc-raid
FAIL=0
TESTS=0

if [ $# -ge 1 ]; then
	DISKS=("$@")
else
	DISKS=(/dev/sda /dev/sdb /dev/sdc /dev/sdd)
fi

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAIL=1; }
check_eq() { # name expected actual
	TESTS=$((TESTS + 1))
	if [ "$2" = "$3" ]; then pass "$1"; else fail "$1: expected [$2] got [$3]"; fi
}
check_ge() { # name actual min
	TESTS=$((TESTS + 1))
	if [ "$2" -ge "$3" ]; then pass "$1"; else fail "$1: [$2] < $3"; fi
}
check_in() { # name "dev sector" "candidate lines"
	TESTS=$((TESTS + 1))
	if echo "$3" | grep -qx "$2"; then pass "$1"; else
		fail "$1: [$2] not in candidates:
$3"; fi
}

frag_phys() { # file byte_offset -> fs physical sector (512B)
	local sect=$(( $2 / 512 ))
	filefrag -v -b512 "$1" | awk -v s="$sect" '
		/^[[:space:]]*[0-9]+:/ {
			l = $2; gsub(/\.\./, "", l); gsub(/:/, "", l)
			h = $3; gsub(/:/, "", h)
			pl = $4; gsub(/\.\./, "", pl)
			if (l+0 <= s && s <= h+0) { print pl + s - l; exit }
		}'
}

retry() {
	local rc
	for i in 1 2 3 4 5; do
		"$@" && return 0
		rc=$?
		sleep 2
	done
	return $rc
}

# this test host's lvm has experimental global-protect/PR locking that
# makes pv/vg commands fail intermittently ("Cannot protect devices");
# these tests use local VGs, so bypass it
for _c in pvcreate vgcreate vgremove lvcreate lvremove lvchange pvremove vgextend; do
	eval "${_c}() { command ${_c} --config 'global { use_lvmlockd = 0 }' \"\$@\"; }"
done
unset _c

# observations from devloc --json output:
#   "dev sector verified serves_reads" (in-leaf-set observations only;
#   one probe per invocation)
get_obs() { # json_output
	echo "$1" | grep -oE '"device": "[^"]*", "major": [0-9]+, "minor": [0-9]+, "offset": [0-9]+, "disk": "[^"]*", "disk_offset": [0-9]+, "verified": (true|false), "served": [0-9]+, "in_leaf_set": (true|false), "serves_reads": (true|false)' |
		sed -E 's/"device": "([^"]*)", "major": [0-9]+, "minor": [0-9]+, "offset": ([0-9]+), "disk": "[^"]*", "disk_offset": [0-9]+, "verified": (true|false), "served": [0-9]+, "in_leaf_set": (true|false), "serves_reads": (true|false)/\1 \2 \3 \4 \5/' |
		awk '$4 == "true" { print $1, $2 / 512, $3, $5 }'
}

cleanup() {
	umount $MNT 2>/dev/null || true
	vgremove -y -f $VG 2>/dev/null || true
	for d in "${DISKS[@]}"; do
		pvremove -y $d 2>/dev/null || true
		wipefs -a ${d}1 ${d}2 2>/dev/null || true
		wipefs -a $d 2>/dev/null || true
		partprobe $d 2>/dev/null || true
	done
}
trap cleanup EXIT

[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 1; }
DEVLOC=$(command -v "$DEVLOC")

# ------------------------------------------------------------------ setup

# defensive: clear leftover partitions and labels from previous runs
for d in "${DISKS[@]}"; do
	wipefs -a ${d}1 ${d}2 2>/dev/null || true
	wipefs -a $d 2>/dev/null || true
done
partprobe "${DISKS[@]}" 2>/dev/null || true
sleep 2

for i in 1 2 3 4 5; do pvcreate -y -q "${DISKS[@]}" >/dev/null 2>&1 && break; sleep 2; done
for i in 1 2 3; do vgcreate -q $VG "${DISKS[@]}" >/dev/null 2>&1 && break; sleep 2; done
mkdir -p $MNT

wait_sync() { # lv-name
	for i in $(seq 1 90); do
		local s
		s=$(dmsetup status $VG-$1 2>/dev/null)
		if ! echo "$s" | grep -qE "resync|recover|reshape|frozen"; then
			return 0
		fi
		sleep 4
	done
	echo "   (warning: $1 still syncing)"
}

# candidate locations "dev sector" for a type and fs sector
compute_candidates() { # type fs_sector
	local type=$1 fs=$2
	local i r d P S
	local k=0 w=0
	if [ "$CHUNK" -gt 0 ]; then
		k=$((fs / CHUNK)); w=$((fs % CHUNK))
	fi
	case $type in
	raid0)
		i=$((k % N)); r=$((k / N))
		echo "${LEG_DEV[i]} $((LEG_START[i] + r * CHUNK + w))" ;;
	raid1)
		for ((i = 0; i < N; i++)); do
			echo "${LEG_DEV[i]} $((LEG_START[i] + fs))"
		done ;;
	raid4)
		# dedicated parity on the FIRST leg (ALGORITHM_PARITY_0);
		# data chunk i on leg 1+i
		i=$((k % (N - 1))); r=$((k / (N - 1)))
		echo "${LEG_DEV[i + 1]} $((LEG_START[i + 1] + r * CHUNK + w))" ;;
	raid5)
		r=$((k / (N - 1))); i=$((k % (N - 1)))
		P=$(( (N - 1 - (r % N)) % N ))
		d=$(( (P + 1 + i) % N ))
		echo "${LEG_DEV[d]} $((LEG_START[d] + r * CHUNK + w))" ;;
	raid6)
		r=$((k / (N - 2)))
		for ((i = 0; i < N; i++)); do
			echo "${LEG_DEV[i]} $((LEG_START[i] + r * CHUNK + w))"
		done ;;
	raid10)
		# near layout: copies on adjacent legs (k*2, k*2+1)
		S=$((N / 2)); i=$((k % S)); r=$((k / S))
		echo "${LEG_DEV[$(( (k * 2) % N ))]} $((LEG_START[$(( (k * 2) % N ))] + r * CHUNK + w))"
		echo "${LEG_DEV[$(( (k * 2 + 1) % N ))]} $((LEG_START[$(( (k * 2 + 1) % N ))] + r * CHUNK + w))" ;;
	esac
}

# row/within invariant: sector - leg_start == row*chunk + within
check_row_invariant() { # type fs_sector "dev sector" observations
	local type=$1 fs=$2 obs=$3
	local k=0 w=0
	if [ "$CHUNK" -gt 0 ]; then
		k=$((fs / CHUNK)); w=$((fs % CHUNK))
	fi
	local r
	case $type in
	raid0)   r=$((k / N)) ;;
	raid1)   r=0 ;;
	raid4)   r=$((k / (N - 1))) ;;
	raid5)   r=$((k / (N - 1))) ;;
	raid6)   r=$((k / (N - 2))) ;;
	raid10)  r=$((k / (N / 2))) ;;
	esac
	local want=$((r * CHUNK + w))
	[ "$type" = raid1 ] && want=$fs
	local dev sec ver sr bad=0 i
	while read -r dev sec ver sr; do
		local found=0
		for ((i = 0; i < N; i++)); do
			if [ "${LEG_DEV[i]}" = "$dev" ]; then
				if [ $((sec - LEG_START[i])) -eq "$want" ]; then
					found=1
				fi
			fi
		done
		[ $found = 0 ] && bad=1
	done <<< "$obs"
	TESTS=$((TESTS + 1))
	if [ $bad = 0 ]; then pass "$type row/within invariant"; else
		fail "$type row/within invariant: expected offset $want, got:
$obs"; fi
}

# ------------------------------------------------------------- per type

LEG_DEV=()
LEG_START=()
N=0
CHUNK=0

test_type() { # type [lvcreate args...]
	local type=$1; shift
	local lv=$type
	echo
	echo "=== $type"

	retry lvcreate -y -q --type $type "$@" -n $lv $VG >/dev/null 2>&1 \
		|| { fail "$type lvcreate"; return; }
	wait_sync $lv
	mkfs.xfs -q -f /dev/$VG/$lv
	mount /dev/$VG/$lv $MNT
	dd if=/dev/urandom of=$MNT/f bs=1M count=48 2>/dev/null
	sync

	# geometry: "start len raid <type> <param_cnt> <chunk> [ctr args] <raid_disks> <meta> <data>..."
	# raid_disks is field 5 of dmsetup status; data devs are the 2nd token
	# of each (meta,data) pair at the end of the table (metas may be "-")
	local tbl st
	tbl=$(dmsetup table $VG-$lv)
	st=$(dmsetup status $VG-$lv)
	LEG_DEV=(); LEG_START=()
	N=$(echo "$st" | awk '{print $5}')
	CHUNK=$(echo "$tbl" | awk '{print $6}')
	local i=0 tok
	while read -r tok; do
		local name
		name=$(cat /sys/dev/block/$tok/dm/name 2>/dev/null)
		local t
		t=$(dmsetup table "$name")
		LEG_DEV[$i]=$(basename "$(readlink -f /sys/dev/block/$(echo "$t" | awk '{print $4}'))")
		LEG_START[$i]=$(echo "$t" | awk '{print $5}')
		i=$((i + 1))
	done < <(echo "$tbl" | awk -v n=$N '{ for (i = NF - 2*n + 2; i <= NF; i += 2) print $i }')
	echo "   $type: N=$N chunk=$CHUNK legs: ${LEG_DEV[*]} starts: ${LEG_START[*]}"

	# read probes at two offsets
	for OFF in 8388608 $((24 * 1024 * 1024 + 8192)); do
		local fs
		fs=$(frag_phys $MNT/f $OFF)
		local out rc
		out=$($DEVLOC --json $MNT/f $OFF 4096); rc=$?
		check_eq "$type read exit ($OFF)" 0 $rc
		local obs
		obs=$(get_obs "$out")
		local cands
		cands=$(compute_candidates $type $fs)
		local d s v sr found=""
		while read -r d s v sr; do
			if echo "$cands" | grep -qx "$d $s"; then
				found="$d $s"
				break
			fi
		done <<< "$obs"
		check_in "$type read location ($OFF)" "$found" "$cands"
		echo "$obs" | awk '$3 == "true"' | grep -q true \
			&& pass "$type read content ($OFF)" \
			|| fail "$type read content ($OFF): $obs"
	done

	# raid1: report which legs served repeated reads (md may pin to one)
	if [ "$type" = raid1 ]; then
		local out
		out=$($DEVLOC --json --repeat 8 $MNT/f 12582912 4096)
		local legs
		legs=$(get_obs "$out" | awk '$3 == "true" {print $1}' | sort -u | tr '\n' ' ')
		echo "   raid1: reads served by: $legs"
	fi

	# nonce: write path, copies, restore
	local md5a md5b out rc
	md5a=$(md5sum $MNT/f | cut -d" " -f1)
	out=$($DEVLOC --json --nonce --force $MNT/f 12582912 4096); rc=$?
	md5b=$(md5sum $MNT/f | cut -d" " -f1)
	check_eq "$type nonce exit" 0 $rc
	check_eq "$type nonce restored" "$md5a" "$md5b"
	local fs
	fs=$(frag_phys $MNT/f 12582912)
	local obs
	obs=$(get_obs "$out")
	echo "$obs" | sed 's/^/    obs: /'
	local ncopy
	ncopy=$(echo "$obs" | awk '$3 == "true"' | wc -l)
	check_ge "$type nonce copies found" "$ncopy" 1
	check_row_invariant $type $fs "$obs"

	# zero-region nonce: in an all-zero stripe, parity equals the data
	# by coincidence, so content alone cannot tell them apart; the read
	# probe must identify the (single) location serving reads as the
	# data copy, matching the computed layout candidate
	if [ "$type" = raid4 ] || [ "$type" = raid5 ] || [ "$type" = raid6 ]; then
		dd if=/dev/zero of=$MNT/zeros bs=1M count=48 2>/dev/null
		sync
		local zoff=$((16 * 1024 * 1024 + 4096))
		local zfs
		zfs=$(frag_phys $MNT/zeros $zoff)
		local zout zrc
		zout=$($DEVLOC --json --nonce --force $MNT/zeros $zoff 4096); zrc=$?
		check_eq "$type zero-region nonce exit" 0 $zrc
		local zobs
		zobs=$(get_obs "$zout")
		echo "$zobs" | sed 's/^/    obs: /'
		local data_loc=""
		while read -r d s v sr; do
			[ "$sr" = "true" ] && data_loc="$d $s"
		done <<< "$zobs"
		check_in "$type zero-region data location (serves reads)" "$data_loc" \
			"$(compute_candidates $type $zfs)"
		local nsrv
		nsrv=$(echo "$zobs" | awk '$4 == "true"' | wc -l)
		check_eq "$type zero-region single read server" 1 "$nsrv"
	fi

	umount $MNT
	lvremove -y -q $VG/$lv >/dev/null 2>&1
}

test_type raid0 -i 4 -I 64 -L 512M
test_type raid1 -m 3 -I 64 -L 512M
test_type raid4 -i 3 -I 64 -L 512M
test_type raid5 -i 3 -I 64 -L 512M
test_type raid10 -i 2 -I 64 -L 512M

# raid6 needs 5 devices (3 data stripes + 2 parity, lvm minimum);
# split the 4th disk into two partitions to provide a 5th PV (a loop
# PV would work too, but this host's lvm requires persistent-reservation
# support on all PVs, which loop devices lack)
echo
echo "=== raid6 setup: 5th PV is a partition of ${DISKS[3]}"
vgremove -y -f $VG >/dev/null 2>&1
for d in "${DISKS[@]}"; do
	pvremove -y $d >/dev/null 2>&1 || true
	wipefs -a $d >/dev/null 2>&1 || true
done
D4=$(basename "${DISKS[3]}")
if command -v parted >/dev/null 2>&1; then
	parted -s "${DISKS[3]}" mklabel msdos mkpart primary 1MiB 50% mkpart primary 50% 100% >/dev/null 2>&1
else
	printf "o\nn\np\n1\n\n+500M\nn\np\n2\n\n\nt\n1\n8e\nt\n2\n8e\nw\n" | fdisk "${DISKS[3]}" >/dev/null 2>&1
fi
partprobe "${DISKS[3]}" 2>/dev/null || true
sleep 2
PVS5=("${DISKS[0]}" "${DISKS[1]}" "${DISKS[2]}" "${DISKS[3]}1" "${DISKS[3]}2")
for i in 1 2 3 4 5; do pvcreate -y -q "${PVS5[@]}" >/dev/null 2>&1 && break; sleep 2; done
for i in 1 2 3; do vgcreate -q $VG "${PVS5[@]}" >/dev/null 2>&1 && break; sleep 2; done
test_type raid6 -i 3 -I 64 -L 512M
# remove the VG before wiping: the partitions are in use until then
vgremove -y -f $VG >/dev/null 2>&1
wipefs -a "${DISKS[3]}1" "${DISKS[3]}2" "${DISKS[3]}" 2>/dev/null || true
partprobe "${DISKS[3]}" 2>/dev/null || true

echo
if [ $FAIL = 0 ]; then
	echo "all $TESTS tests passed"
else
	echo "FAILURES present"
fi
exit $FAIL
