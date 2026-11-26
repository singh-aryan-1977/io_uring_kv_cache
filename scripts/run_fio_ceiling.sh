#!/usr/bin/env bash
# Measures the drive's ceiling with fio: fills the test file, then sweeps
# pattern x block size x queue depth and writes one CSV row per point.
#
# usage: scripts/run_fio_ceiling.sh <test file on NVMe> <results dir>
#   e.g. scripts/run_fio_ceiling.sh /mnt/nvme/fio.test results/i4i
#
# env overrides (defaults are the W1 sweep, ~30 min):
#   SIZE=64g RUNTIME=30 RAMP=5
#   RW_LIST="randread randwrite" BS_LIST="256k 1m 2m 4m 8m 16m" QD_LIST="1 8 32 64"
#   SKIP_FILL=1  reuse an already-filled test file
set -euo pipefail

FILE=${1:?usage: run_fio_ceiling.sh <test file on NVMe> <results dir>}
OUT_DIR=${2:?usage: run_fio_ceiling.sh <test file on NVMe> <results dir>}

SIZE=${SIZE:-64g}
RUNTIME=${RUNTIME:-30}
RAMP=${RAMP:-5}
RW_LIST=${RW_LIST:-"randread randwrite"}
BS_LIST=${BS_LIST:-"256k 1m 2m 4m 8m 16m"}
QD_LIST=${QD_LIST:-"1 8 32 64"}

REPO=$(cd "$(dirname "$0")/.." && pwd)
JOB="$REPO/fio/ceiling.fio"

for tool in fio jq; do
    command -v "$tool" >/dev/null || { echo "missing $tool (run scripts/setup_box.sh)" >&2; exit 1; }
done

# the test file is many GiB: never let it land inside the repo
FILE_DIR=$(cd "$(dirname "$FILE")" && pwd)
case "$FILE_DIR/" in
    "$REPO"/*) echo "refusing: test file $FILE is inside the repo" >&2; exit 1 ;;
esac

mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/fio_ceiling.csv"
"$REPO/scripts/record_env.sh" "$OUT_DIR" "$FILE_DIR"

# fill the file with real data first. a fallocated or sparse file reads back
# as zeros without touching the drive, which gives impossibly fast reads.
if [[ "${SKIP_FILL:-0}" != 1 ]]; then
    echo "filling $FILE ($SIZE)..."
    fio --name=fill --filename="$FILE" --size="$SIZE" --rw=write --bs=16m \
        --direct=1 --ioengine=io_uring --iodepth=16 --output-format=terse >/dev/null
fi

# root only for dropping the page cache (O_DIRECT skips it anyway; methodology says drop)
SUDO=""
[[ $(id -u) -ne 0 ]] && SUDO="sudo"

echo "rw,bs,qd,bw_MBps,iops,lat_mean_us,p50_us,p99_us,p999_us,usr_cpu_pct,sys_cpu_pct" > "$CSV"

export FILE SIZE RUNTIME RAMP
for RW in $RW_LIST; do
    for BS in $BS_LIST; do
        for QD in $QD_LIST; do
            export RW BS QD
            sync
            echo 3 | $SUDO tee /proc/sys/vm/drop_caches >/dev/null

            json=$(fio "$JOB" --output-format=json)

            # fio's JSON keeps results under "read" or "write"
            dir=${RW#rand}
            jq -r --arg d "$dir" --arg rw "$RW" --arg bs "$BS" --arg qd "$QD" '
                .jobs[0] as $j | $j[$d] as $r |
                [$rw, $bs, ($qd | tonumber),
                 ($r.bw_bytes / 1e6 | floor),
                 ($r.iops | floor),
                 ($r.clat_ns.mean / 1000 | floor),
                 ($r.clat_ns.percentile["50.000000"] / 1000 | floor),
                 ($r.clat_ns.percentile["99.000000"] / 1000 | floor),
                 ($r.clat_ns.percentile["99.900000"] / 1000 | floor),
                 $j.usr_cpu, $j.sys_cpu] | @csv' <<< "$json" >> "$CSV"

            echo "done $RW bs=$BS qd=$QD"
        done
    done
done

echo "wrote $CSV"
