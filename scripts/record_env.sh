#!/usr/bin/env bash
# Records everything about this machine that affects benchmark numbers into
# <results dir>/env.txt. Run before every benchmark; commit it with the CSVs.
#
# usage: scripts/record_env.sh <results dir> [data dir on the NVMe]
#   e.g. scripts/record_env.sh results/i4i /mnt/nvme
set -uo pipefail  # no -e: a missing tool should leave a gap, not stop the script

OUT_DIR=${1:?usage: record_env.sh <results dir> [data dir]}
DATA_DIR=${2:-/tmp}
REPO=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT_DIR"
ENV_FILE="$OUT_DIR/env.txt"

# AWS instance type via the local metadata endpoint (IMDSv2); empty off AWS
instance_type() {
    local token
    token=$(curl -s -m 1 -X PUT "http://169.254.169.254/latest/api/token" \
        -H "X-aws-ec2-metadata-token-ttl-seconds: 60" 2>/dev/null) || return
    curl -s -m 1 -H "X-aws-ec2-metadata-token: $token" \
        "http://169.254.169.254/latest/meta-data/instance-type" 2>/dev/null
}

# block device that holds DATA_DIR, e.g. nvme1n1 (partition stripped)
data_src=$(findmnt -n -o SOURCE -T "$DATA_DIR" 2>/dev/null)
data_dev=$(lsblk -n -o PKNAME "$data_src" 2>/dev/null | head -1)
[[ -z "$data_dev" ]] && data_dev=$(basename "$data_src" 2>/dev/null)
sysblk="/sys/block/$data_dev"

{
    echo "date_utc:          $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "instance_type:     $(instance_type)"
    echo "os:                $(. /etc/os-release && echo "$PRETTY_NAME")"
    echo "kernel:            $(uname -r)"
    echo "liburing:          $(pkg-config --modversion liburing 2>/dev/null)"
    echo "gcc:               $(gcc -dumpfullversion 2>/dev/null)"
    echo "fio:               $(fio --version 2>/dev/null)"
    echo "uringtier_commit:  $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null)$(git -C "$REPO" diff --quiet 2>/dev/null || echo ' (dirty)')"
    echo
    echo "cpu_model:         $(lscpu | sed -n 's/^Model name: *//p')"
    echo "cpus:              $(nproc)"
    echo "numa_nodes:        $(lscpu | sed -n 's/^NUMA node(s): *//p')"
    echo "cpu_governor:      $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
    echo "mem_total:         $(free -g | awk '/^Mem:/ {print $2 " GiB"}')"
    echo
    echo "data_dir:          $DATA_DIR"
    echo "data_device:       $data_src ($data_dev)"
    echo "filesystem:        $(findmnt -n -o FSTYPE,OPTIONS -T "$DATA_DIR" 2>/dev/null)"
    echo "device_model:      $(cat "$sysblk/device/model" 2>/dev/null | xargs)"
    echo "device_firmware:   $(cat "$sysblk/device/firmware_rev" 2>/dev/null | xargs)"
    echo "device_size:       $(lsblk -d -n -o SIZE "/dev/$data_dev" 2>/dev/null | xargs)"
    echo "io_scheduler:      $(cat "$sysblk/queue/scheduler" 2>/dev/null)"
    echo "max_sectors_kb:    $(cat "$sysblk/queue/max_sectors_kb" 2>/dev/null)"
    echo "nr_requests:       $(cat "$sysblk/queue/nr_requests" 2>/dev/null)"
    echo
    echo "io_uring_disabled: $(cat /proc/sys/kernel/io_uring_disabled 2>/dev/null || echo n/a)"
    echo "memlock_kib:       $(ulimit -l)"
    echo "iou_wrk_threads:   $(ps -eLo comm= | grep -c '^iou-wrk')"
} > "$ENV_FILE"

echo "wrote $ENV_FILE"
