#!/bin/bash
#
# devloc test harness
#
# Verifies devloc's empirical mapping against known dm mappings by
# building LVM stacks with known tables, then comparing devloc's
# observed device+offset against arithmetic computed from the dm table
# and FIEMAP.
#
# Requirements: root, two spare disks (or loop devices), lvm, mkfs.xfs,
# filefrag, and the devloc binary.
#
# Usage: test-devloc.sh [disk1 disk2]
#        DEVLOC=/path/to/devloc test-devloc.sh

set -u

DISK1=${1:-/dev/sda}
DISK2=${2:-/dev/sdb}
DISK3=${3:-/dev/sdc}
VG=devlocvg
DEVLOC=${DEVLOC:-./devloc}
MNT=/mnt/devloc-test
FAIL=0
TESTS=0

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

# fs physical sector (512B) for a file offset, from filefrag -v -b512
frag_phys() { # file byte_offset
	local sect=$(( $2 / 512 ))
	filefrag -v -b512 "$1" | awk -v s="$sect" '
		/^[[:space:]]*[0-9]+:/ {
			l = $2; gsub(/\.\./, "", l); gsub(/:/, "", l)
			h = $3; gsub(/:/, "", h)
			pl = $4; gsub(/\.\./, "", pl)
			if (l+0 <= s && s <= h+0) { print pl + s - l; exit }
		}'
}

cleanup() {
	umount $MNT/part $MNT/str $MNT/lin 2>/dev/null || true
	lvchange -an $VG 2>/dev/null || true
	vgremove -y -f $VG 2>/dev/null || true
	pvremove -y $DISK1 $DISK2 ${DISK3}1 2>/dev/null || true
	# pvremove can fail on this test setup (PR protection); wipe as backstop
	wipefs -a $DISK1 $DISK2 2>/dev/null || true
	wipefs -a ${DISK3}1 $DISK3 2>/dev/null || true
}

# the VM's experimental lvm build can transiently fail pv/vg commands with
# "Cannot protect devices"; retry a few times
retry() {
	local rc
	for i in 1 2 3; do
		"$@" && return 0
		rc=$?
		sleep 2
	done
	return $rc
}

# bypass the experimental global-protect/PR locking for local test VGs
for _c in pvcreate vgcreate vgremove lvcreate lvremove lvchange pvremove vgextend; do
	eval "${_c}() { command ${_c} --config 'global { use_lvmlockd = 0 }' \"\$@\"; }"
done
unset _c
trap cleanup EXIT

[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 1; }
for cmd in "$DEVLOC" lvm mkfs.xfs filefrag dmsetup lsblk; do
	command -v "$cmd" >/dev/null || { echo "missing: $cmd"; exit 1; }
done
DEVLOC=$(command -v "$DEVLOC")
D1=$(basename $DISK1)
D2=$(basename $DISK2)

# ---------------------------------------------------------------- setup

echo "== setup: $VG on $DISK1 $DISK2 (+ partition on $DISK3)"

# partition disk3 first; all pvcreates must happen before any VG exists
# (the VM's experimental global-protect lvm fails pvcreate otherwise)
if command -v parted >/dev/null 2>&1; then
	parted -s $DISK3 mklabel msdos mkpart primary 1MiB 100% >/dev/null 2>&1
else
	printf "o\nn\np\n1\n\n\nt\n8e\nw\n" | fdisk $DISK3 >/dev/null 2>&1
fi
partprobe $DISK3 2>/dev/null || true
sleep 2
PART=${DISK3}1
D3=$(basename $DISK3)
PSTART=$(cat /sys/class/block/${D3}1/start 2>/dev/null)
if [ -z "$PSTART" ]; then
	fail "could not create partition on $DISK3"
	PSTART=0
fi

retry pvcreate -y -q $DISK1 $DISK2 $PART >/dev/null 2>&1
retry vgcreate -q $VG $DISK1 $DISK2 $PART >/dev/null 2>&1
mkdir -p $MNT/lin $MNT/str

# linear LV on disk1
lvcreate -y -q -n lin -L 512M $VG $DISK1 >/dev/null
mkfs.xfs -q -f /dev/$VG/lin
mount /dev/$VG/lin $MNT/lin
dd if=/dev/urandom of=$MNT/lin/file bs=1M count=64 2>/dev/null
sync

# linear LV on the partition of disk3
lvcreate -y -q -n part -L 400M $VG $PART >/dev/null
mkfs.xfs -q -f /dev/$VG/part
mkdir -p $MNT/part
mount /dev/$VG/part $MNT/part
dd if=/dev/urandom of=$MNT/part/file bs=1M count=64 2>/dev/null
sync

# parse the linear table: "0 N linear <maj:min> <phys_start>"
LINE=$(dmsetup table $VG-lin)
LIN_PHYS_START=$(echo "$LINE" | awk '{print $5}')
check_eq "linear table parsed" "linear" "$(echo "$LINE" | awk '{print $3}')"

# ---------------------------------------------------- device mode, linear

echo "== test: device mode, linear LV"
out=$($DEVLOC --device /dev/$VG/lin --sector 100000)
rc=$?
check_eq "device-mode exit" 0 $rc
got=$(echo "$out" | awk '$1 == 51200000 {print $3, $4}')
check_eq "device-mode result" "$D1 $(( (LIN_PHYS_START + 100000) * 512 ))" "$got"

# ------------------------------------------------------ file mode, linear

echo "== test: file mode, linear LV"
TOFF=$((16 * 1024 * 1024 + 4096))
PHYS=$(frag_phys $MNT/lin/file $TOFF)
[ -n "$PHYS" ] || { fail "could not get FIEMAP physical for file offset"; PHYS=0; }
echo "   file offset $TOFF -> fs sector $PHYS"

out=$($DEVLOC $MNT/lin/file $TOFF)
rc=$?
check_eq "file-mode exit" 0 $rc
got=$(echo "$out" | awk -v off=$TOFF '$1 == off {print $3, $4}')
check_eq "file-mode result" "$D1 $(( (LIN_PHYS_START + PHYS) * 512 ))" "$got"
echo "$out" | grep -q verified && pass "file-mode content verified" || fail "file-mode content not verified"

# ------------------------------------------------- file mode, partition PV

echo "== test: file mode, LV on a partition of $DISK3"
if [ -z "$(dmsetup table $VG-part 2>/dev/null)" ]; then
	fail "partition LV was not created in setup"
else
PLINE=$(dmsetup table $VG-part)
PPHYS_START=$(echo "$PLINE" | awk '{print $5}')
PTOFF=$((8 * 1024 * 1024))
PPHYS=$(frag_phys $MNT/part/file $PTOFF)
echo "   partition $PART start $PSTART, table: $PLINE"

out=$($DEVLOC $MNT/part/file $PTOFF)
rc=$?
check_eq "partition file-mode exit" 0 $rc
# event sector is disk-relative: part_start + table phys + fs phys;
# devloc prints the partition-relative offset (event - part_start)
got=$(echo "$out" | awk -v off=$PTOFF '$1 == off {print $3, $4}')
check_eq "partition file-mode result" \
	"${D3}1 $(( (PPHYS_START + PPHYS) * 512 ))" "$got"
echo "$out" | grep -q verified && pass "partition content verified" || fail "partition content not verified"
fi

# ----------------------------------------------------------- summary mode

echo "== test: summary mode, striped LV"
lvcreate -y -q -n str -i 2 -I 64 -L 512M $VG $DISK1 $DISK2 >/dev/null
mkfs.xfs -q -f /dev/$VG/str
mount /dev/$VG/str $MNT/str
dd if=/dev/urandom of=$MNT/str/file bs=1M count=64 2>/dev/null
sync

out=$($DEVLOC --summary --sample 64 $MNT/str/file 0 64M)
rc=$?
check_eq "summary exit" 0 $rc
n1=$(echo "$out" | grep -c "^$D1")
n2=$(echo "$out" | grep -c "^$D2")
check_ge "summary shows stripe leaf $D1" "$n1" 1
check_ge "summary shows stripe leaf $D2" "$n2" 1

# --------------------------------------------- stripe math verification

echo "== test: stripe arithmetic, single block"
# striped table: "0 N striped <count> <chunk> <dev1> <start1> <dev2> <start2>"
SLINE=$(dmsetup table $VG-str)
CHUNK=$(echo "$SLINE" | awk '{print $5}')
NSTR=$(echo "$SLINE" | awk '{print $4}')
S0=$(echo "$SLINE" | awk '{print $7}')
S1=$(echo "$SLINE" | awk '{print $9}')
echo "   striped table: $SLINE (chunk $CHUNK sectors)"

TOFF2=$((48 * 1024 * 1024 + 8192))
PHYS2=$(frag_phys $MNT/str/file $TOFF2)
CI=$((PHYS2 / CHUNK))
STRIPE=$((CI % 2))
WITHIN=$((PHYS2 % CHUNK))
CHUNKN=$((CI / NSTR))
if [ $STRIPE = 0 ]; then
	EXPECT_DISK=$D1; EXPECT_SECT=$((S0 + CHUNKN * CHUNK + WITHIN))
else
	EXPECT_DISK=$D2; EXPECT_SECT=$((S1 + CHUNKN * CHUNK + WITHIN))
fi
echo "   fs sector $PHYS2 -> stripe $STRIPE, expect $EXPECT_DISK sector $EXPECT_SECT"

out=$($DEVLOC $MNT/str/file $TOFF2)
rc=$?
check_eq "stripe exit" 0 $rc
got=$(echo "$out" | awk -v off=$TOFF2 '$1 == off {print $3, $4}')
check_eq "stripe result" "$EXPECT_DISK $(( EXPECT_SECT * 512 ))" "$got"

# -------------------------------------------------------------- nonce mode

echo "== test: nonce mode"
md5a=$(md5sum $MNT/lin/file | cut -d" " -f1)
out=$($DEVLOC --nonce --force $MNT/lin/file 8M)
rc=$?
md5b=$(md5sum $MNT/lin/file | cut -d" " -f1)
check_eq "nonce exit" 0 $rc
check_eq "nonce restored (md5 unchanged)" "$md5a" "$md5b"
echo "$out" | grep -q "nonce verified" && pass "nonce found copy" || fail "nonce copy not verified"

# ----------------------------------------------------------- hole / sparse

echo "== test: unmappable (hole)"
truncate -s 8M $MNT/lin/hole
out=$($DEVLOC $MNT/lin/hole 4M)
rc=$?
check_eq "hole exit" 3 $rc
echo "$out" | grep -q UNMAPPABLE && pass "hole reported" || fail "hole not reported"

# ------------------------------------------------------------------ json

echo "== test: json output"
out=$($DEVLOC --json $MNT/lin/file $TOFF)
rc=$?
check_eq "json exit" 0 $rc
echo "$out" | grep -q '"device"' && pass "json has observations" || fail "json missing observations"

# ------------------------------------------------------------------ done

echo
if [ $FAIL = 0 ]; then
	echo "all $TESTS tests passed"
else
	echo "FAILURES present"
fi
exit $FAIL
