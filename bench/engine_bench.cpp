/**
 * engine_bench: This file measures how fast my Ring implementation moves data on the drive, using the same
 * sweep as fio/ceiling.fio so the two CSVs compare row by row.
 *
 * What it does:
 *   1. open the test file (TierFile: with O_DIRECT + fallocate)
 *   2. fill test file with real data once, so reads actually hit the drive
 *      instead of getting zeros back from the filesystem since fallocate can do that if blocks are unwritten to iniitally
 *   3. Run various sweeps of data points of format (pattern (how to read/write but for bench, random is the way to go) x block size x queue depth). for each one,
 *      keep qd random reads or writes of bs bytes in flight, skip the ramp up time,
 *      then measure for the runtime for each I/O operation
 *   4. write one row per data point to the CSV with the same columns as fio_ceiling.csv
 *      (bandwidth, iops, latency mean/p50/p99/p99.9, user + kernel CPU)
 *
 * How to run:
 *   build without sanitizers otherwise the numbers are not results worthhy sincer ASAN cehcks memory accesses, and UBSAN checks on pointer castings.
 *     cmake -S . -B /tmp/build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/build-rel
 *   run on fio's already-filled file so both hit the same blocks on the drive meaning its a fairer comparision. Make sure to run fio before this file on the same machine
 *     SKIP_FILL=1 /tmp/build-rel/engine_bench /mnt/nvme/fio.test results/i4i/engine.csv
 *
 * Arguments:
 *   <test file>  file on the NVMe to read/write (never inside the repo)
 *   <out.csv>    where to write the results
 *
 * Env vars (same names and defaults as scripts/run_fio_ceiling.sh, so keep them equal):
 *   SIZE=64g                           size of test file
 *   RUNTIME=30                         seconds measured per data point
 *   RAMP=5                             warm-up seconds per data point, not measured
 *   RW_LIST="randread randwrite"       patterns: randread = loads, randwrite = stores
 *   BS_LIST="256k 1m 2m 4m 8m 16m"     block sizes = chunk sizes
 *   QD_LIST="1 8 32 64"                queue depths = I/Os in flight
 *   SKIP_FILL=1                        skip the fill and reuse an already-filled file
 *
 * Biggest data point (qd 64 x 16m) registers 1 GiB, so RLIMIT_MEMLOCK must allow that
 * (setup_box.sh raises it). If we get ENOMEM, raise ulimit -l or shrink BS_LIST/QD_LIST.
 */

#include "bench_common.hpp"

#include "uringtier/aligned_buffer.hpp"
#include "uringtier/ring.hpp"
#include "uringtier/tier_file.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace uringtier {
    namespace {
        using bench::Clock;
        using bench::Result;

        /**
         * Run one sweep point: keep `qd` random reads or writes of `bs` bytes in flight
         * for ramp + runtime seconds, and measure only the runtime part. We 
         * need a rampup time becasue first moments arent representative due to CPU rampu time,
         * drive ramp up time etc. 
         * Each in-flight I/O owns its own `bs` slice of one registered buffer
         * (user_data = slice index), and is re-issued at a new random offset as soon
         * as it completes, so the queue depth stays at `qd` the whole time. Right now,
         * this is a closed loop generator model but another benchmark could play with an 
         * open loop generator with a zipf distribution
         *
         * @param file the test file (shoukld already be filled)
         * @param is_read true for randread, false for randwrite
         * @param bs block size in bytes
         * @param qd queue depth
         * @param ramp warm-up, not measured
         * @param runtime measured duration
         * @param rng random source for offsets
         * @return the measured results
         * @throws std::runtime_error if an I/O fails or comes back short
         */
        Result run_point(const TierFile& file, bool is_read, std::uint32_t bs, unsigned int qd,
                         Clock::duration ramp, Clock::duration runtime, std::mt19937_64& rng) {
            AlignedBuffer buf(std::size_t{bs} * qd);
            bench::randomize(buf, rng);

            Ring ring(qd);
            ring.register_file(file.fd());
            iovec iov{buf.data(), buf.size()};
            ring.register_buffers({&iov, 1});

            const std::uint64_t num_blocks = file.size() / bs;
            std::vector<Clock::time_point> started(qd);

            // lambda to issue random block-aligned offsets, similar to fio with norandommap
            auto issue = [&](unsigned int slice) {
                std::uint64_t offset = (rng() % num_blocks) * bs;
                std::byte* addr = buf.data() + std::size_t{slice} * bs;
                started[slice] = Clock::now();
                if (is_read) {
                    ring.queue_read(0, addr, bs, offset, slice);
                } else {
                    ring.queue_write(0, addr, bs, offset, slice);
                }
            };

            // right now, we reserve 8MiB of vector spce since we want exact quantile.
            // will replace with historgram based approach similar to fio later
            std::vector<std::uint64_t> lat_ns;
            lat_ns.reserve(1u << 20);
            std::uint64_t bytes = 0;
            bool measuring = false;
            Clock::time_point measure_start;
            rusage ru_start{};

            // issue qd items
            for (unsigned int i = 0; i < qd; ++i) {
                issue(i);
            }
            const Clock::time_point t_measure = Clock::now() + ramp;
            const Clock::time_point t_end = t_measure + runtime;

            std::vector<Completion> done;
            while (true) {
                ring.submit_and_wait(1);  // submits the re-issued I/Os, waits for at least one
                done.clear();
                ring.reap(done);
                const Clock::time_point now = Clock::now();

                // if we werent measuring before and we have reached ramp up time, we can start
                // measring now
                if (!measuring && now >= t_measure) {
                    measuring = true;
                    measure_start = now;
                    ::getrusage(RUSAGE_SELF, &ru_start);
                }

                for (const auto& c : done) {
                    if (c.res != static_cast<std::int32_t>(bs)) {
                        std::string why = c.res < 0 ? std::strerror(-c.res) : "short I/O " + std::to_string(c.res);
                        throw std::runtime_error(std::string(is_read ? "read" : "write") + " failed: " + why);
                    }
                    if (measuring) {
                        // submit -> cqe seen: the "device latency" from DESIGN.md
                        lat_ns.push_back(static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(now - started[c.user_data]).count()));
                        bytes += bs;
                    }
                }

                if (now >= t_end) {
                    break;
                }

                // issue again so we always have atleast qd items in flight
                for (const auto& c : done) {
                    issue(static_cast<unsigned int>(c.user_data));
                }
            }

            const double elapsed = bench::seconds(Clock::now() - measure_start);
            rusage ru_end{};
            ::getrusage(RUSAGE_SELF, &ru_end);

            // wait for remaining entries so we can tear them down with nothing in flight
            while (ring.in_flight() > 0) {
                ring.submit_and_wait(1);
                done.clear();
                ring.reap(done);
            }

            return bench::make_result(lat_ns, bytes, elapsed, ru_start, ru_end);
        }
    }
}

int main(int argc, char** argv) {
    return uringtier::bench::bench_main("engine_bench", argc, argv, uringtier::run_point);
}
