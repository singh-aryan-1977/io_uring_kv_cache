#!/usr/bin/env bash
# Prepares a fresh benchmark box (i4i, later the GPU box): packages, io_uring
# check, memlock limit, local NVMe formatted + mounted, project built and tested.
# Safe to re-run: skips steps that are already done.
#
# usage: sudo scripts/setup_box.sh [device]
#   device   NVMe to format, e.g. /dev/nvme1n1. If omitted, the one AWS
#            instance-store drive is found by its model name.
# env:
#   MOUNT=/mnt/nvme   where to mount it
#   FS=ext4           filesystem (ext4 or xfs)
#   SKIP_NVME=1       skip the disk step (e.g. drive already mounted, or a VM)
#   FORCE=1           format even if the device already has a filesystem
set -euo pipefail

[[ $(id -u) -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }

DEVICE=${1:-}
MOUNT=${MOUNT:-/mnt/nvme}
FS=${FS:-ext4}
OWNER=${SUDO_USER:-root}
REPO=$(cd "$(dirname "$0")/.." && pwd)

echo "== packages"
apt-get update -q
apt-get install -y -q --no-install-recommends \
    build-essential cmake ninja-build pkg-config liburing-dev libgtest-dev fio jq xfsprogs

echo "== io_uring"
if [[ "$(cat /proc/sys/kernel/io_uring_disabled 2>/dev/null || echo 0)" != 0 ]]; then
    echo "io_uring is disabled (kernel.io_uring_disabled != 0); fix that first" >&2
    exit 1
fi
echo "ok"

echo "== memlock"
# registered buffers count against RLIMIT_MEMLOCK (ENOMEM past it). applies to new logins.
LIMITS=/etc/security/limits.d/90-uringtier.conf
if [[ ! -f "$LIMITS" ]]; then
    printf '* - memlock unlimited\nroot - memlock unlimited\n' > "$LIMITS"
    echo "wrote $LIMITS: log out and back in for it to apply"
else
    echo "$LIMITS already present"
fi

if [[ "${SKIP_NVME:-0}" != 1 ]]; then
    echo "== nvme"
    if mountpoint -q "$MOUNT"; then
        echo "$MOUNT already mounted, skipping"
    else
        if [[ -z "$DEVICE" ]]; then
            # instance store reports "Amazon EC2 NVMe Instance Storage"; the boot
            # disk (EBS) also shows up as nvme, so match on the model, not the name
            mapfile -t found < <(lsblk -d -n -o NAME,MODEL | awk '/Instance Storage/ {print "/dev/" $1}')
            if [[ ${#found[@]} -ne 1 ]]; then
                echo "found ${#found[@]} instance-store drives; pass the device explicitly:" >&2
                lsblk -d -o NAME,MODEL,SIZE,MOUNTPOINTS >&2
                exit 1
            fi
            DEVICE=${found[0]}
        fi
        [[ -b "$DEVICE" ]] || { echo "$DEVICE is not a block device" >&2; exit 1; }

        # never touch the disk the system runs from, or anything mounted
        root_dev=$(lsblk -n -o PKNAME "$(findmnt -n -o SOURCE /)" | head -1)
        if [[ "$(basename "$DEVICE")" == "$root_dev" ]]; then
            echo "refusing: $DEVICE holds the root filesystem" >&2; exit 1
        fi
        if lsblk -n -o MOUNTPOINTS "$DEVICE" | grep -q .; then
            echo "refusing: $DEVICE (or a partition) is mounted" >&2; exit 1
        fi
        if blkid "$DEVICE" >/dev/null 2>&1 && [[ "${FORCE:-0}" != 1 ]]; then
            echo "refusing: $DEVICE already has a filesystem ($(blkid -o value -s TYPE "$DEVICE")); FORCE=1 to wipe" >&2
            exit 1
        fi

        echo "formatting $DEVICE as $FS"
        case "$FS" in
            # no lazy init: background zeroing would run during the benchmarks
            ext4) mkfs.ext4 -F -q -E lazy_itable_init=0,lazy_journal_init=0 "$DEVICE" ;;
            xfs)  mkfs.xfs -f -q "$DEVICE" ;;
            *) echo "FS must be ext4 or xfs" >&2; exit 1 ;;
        esac
        mkdir -p "$MOUNT"
        mount -o noatime "$DEVICE" "$MOUNT"
        chown "$OWNER": "$MOUNT"
        echo "mounted $DEVICE at $MOUNT (owner $OWNER)"
    fi
fi

echo "== build + test"
# build as the normal user, in /tmp, so no root-owned files land in the repo
TEST_DIR=/tmp
[[ "${SKIP_NVME:-0}" != 1 ]] && TEST_DIR=$MOUNT
sudo -u "$OWNER" bash -c "
    set -e
    cmake -S '$REPO' -B /tmp/uringtier-build -G Ninja -DUT_SANITIZE=ON >/dev/null
    cmake --build /tmp/uringtier-build
    UT_TEST_DIR='$TEST_DIR' ctest --test-dir /tmp/uringtier-build --output-on-failure
"

echo "== done"
echo "next: scripts/run_fio_ceiling.sh $MOUNT/fio.test results/<box>"
