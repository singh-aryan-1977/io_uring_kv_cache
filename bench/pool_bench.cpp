/**
 * pool_bench: This file measures how fast a thread pool doing blocking pread/pwrite moves
 * data on the drive. That's the model vLLM's fs tier uses (blocking I/O on 16 read + 16 write
 * threads). Same sweep and CSV columns as engine_bench and fio/ceiling.fio, so the three CSVs
 * compare row by row. engine vs pool on CPU per GB/s is the first evidence for our headline.
 *
 * What it does:
 *   1. open the test file (TierFile: with O_DIRECT + fallocate)
 *   2. fill test file with real data once (shared bench::fill_file), so reads actually hit
 *      the drive instead of getting zeros back from the filesystem for unwritten blocks
 *   3. run various sweeps of data points of format (pattern x block size x queue depth).
 *      here queue depth = number of threads: each thread does one blocking random read or
 *      write of bs bytes at a time, so qd threads = qd I/Os in flight, same as engine_bench.
 *      skip the ramp up time, then measure for the runtime for each I/O operation
 *   4. write one row per data point to the CSV with the same columns as fio_ceiling.csv
 *      (bandwidth, iops, latency mean/p50/p99/p99.9, user + kernel CPU)
 *
 * What it doesn't include: the fs tier's per-chunk open/close/rename and directory work.
 * It's one big file here too, so this only isolates thread pool vs io_uring and is the
 * kindest possible baseline for the fs tier.
 *
 * How to run:
 *   build without sanitizers (Release, optimized) otherwise the numbers are meaningless:
 *     cmake -S . -B /tmp/build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/build-rel
 *   run on fio's already-filled file so all three hit the same blocks on the drive. Run fio first
 *     SKIP_FILL=1 /tmp/build-rel/pool_bench /mnt/nvme/fio.test results/i4i/pool.csv
 *
 * Arguments:
 *   <test file>  file on the NVMe to read/write (never inside the repo)
 *   <out.csv>    where to write the results
 *
 * Env vars (same names and defaults as engine_bench and scripts/run_fio_ceiling.sh, so keep them equal):
 *   SIZE=64g                           size of test file
 *   RUNTIME=30                         seconds measured per data point
 *   RAMP=5                             warm-up seconds per data point, not measured
 *   RW_LIST="randread randwrite"       patterns: randread = loads, randwrite = stores
 *   BS_LIST="256k 1m 2m 4m 8m 16m"     block sizes = chunk sizes
 *   QD_LIST="1 8 32 64"                queue depths = number of threads
 *   SKIP_FILL=1                        skip the fill and reuse an already-filled file
 *
 * Biggest data point (64 threads x 16m) allocates 1 GiB of buffers. Nothing is registered,
 * so RLIMIT_MEMLOCK doesn't matter here.
 */

#include "bench_common.hpp"

#include "uringtier/aligned_buffer.hpp"
#include "uringtier/tier_file.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <latch>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace uringtier {
    namespace {
        using bench::Clock;
        using bench::Result;
        
        /**
         * What each worker thread measures. Each thread owns their own
         */
        struct ThreadStats {
            std::vector<std::uint64_t> lat_ns;
            std::uint64_t bytes = 0;
            std::string error;  // set if an I/O failed; main rethrows it
        };

        /**
         * One worker thread: loop doing one blocking random read or write of `bs` bytes at a
         * time until t_end. Latency = time spent inside pread/pwrite; only I/Os that finish
         * between t_measure and t_end are counted (same window as engine_bench).
         *
         * @param fd the test file's fd, shared by all threads (pread/pwrite take their own offset)
         * @param is_read true for randread, false for randwrite
         * @param bs block size in bytes
         * @param num_blocks number of bs-sized blocks in the file
         * @param seed per-thread seed, since one rng can't be shared between threads
         * @param start latch every thread waits on, so they all start together
         * @param t_measure when measuring starts (end of ramp)
         * @param t_end when measuring stops
         * @param failed set by any thread that hits an error, so the others stop early
         * @param stats where this thread writes its results
         */
        void worker(int fd, bool is_read, std::uint32_t bs, std::uint64_t num_blocks, std::uint64_t seed,
                    std::latch& start, const Clock::time_point& t_measure, const Clock::time_point& t_end,
                    std::atomic<bool>& failed, ThreadStats& stats) {
            std::mt19937_64 rng(seed);
            AlignedBuffer buf(bs);  // each thread its own buffer, like each in-flight I/O in engine_bench
            bench::randomize(buf, rng);

            start.wait();
            while (!failed.load(std::memory_order_relaxed)) {
                // random block-aligned offset, similar to fio with norandommap
                auto offset = static_cast<off_t>((rng() % num_blocks) * bs);

                const Clock::time_point t0 = Clock::now();

                // pread and pwrite are blocking calls
                ssize_t ret = is_read ? ::pread(fd, buf.data(), bs, offset) : ::pwrite(fd, buf.data(), bs, offset);
                const Clock::time_point now = Clock::now();

                if (ret != static_cast<ssize_t>(bs)) {
                    int err = errno;
                    stats.error = std::string(is_read ? "read" : "write") + " failed: " +
                                  (ret < 0 ? std::strerror(err) : "short I/O " + std::to_string(ret));
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                if (now >= t_measure && now < t_end) {
                    stats.lat_ns.push_back(static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now - t0).count()));
                    stats.bytes += bs;
                }
                if (now >= t_end) {
                    return;
                }
            }
        }

        /**
         * Run one sweep across one data point. Start `qd` threads, each keeping one blocking random read or
         * write of `bs` bytes going, for ramp + runtime seconds, and measure only the runtime part.
         * Main thread only sleeps and takes the CPU snapshots: getrusage(RUSAGE_SELF) covers
         * every thread in the process, so usr/sys are the whole pool's CPU
         *
         * @param file the test file (should already be filled)
         * @param is_read true for randread, false for randwrite
         * @param bs block size in bytes
         * @param qd number of threads
         * @param ramp warm-up, not measured
         * @param runtime measured duration
         * @param rng random source; one draw from it seeds the threads (thread i uses seed + i)
         * @return the measured results
         * @throws std::runtime_error if an I/O fails or comes back short
         */
        Result run_point(const TierFile& file, bool is_read, std::uint32_t bs, unsigned int qd,
                         Clock::duration ramp, Clock::duration runtime, std::mt19937_64& rng) {
            const std::uint64_t num_blocks = file.size() / bs;
            const std::uint64_t seed = rng();

            std::vector<ThreadStats> stats(qd);
            for (auto& s : stats) {
                // same total sample budget as engine_bench's single 1M reserve, split across threads and 1024 added as extra buffer
                s.lat_ns.reserve((1u << 20) / qd + 1024);
            }


            // initialize latch so threads dont start until we count down and we have made the timestamps
            std::latch start(1);
            std::atomic<bool> failed{false};
            Clock::time_point t_measure;
            Clock::time_point t_end;

            std::vector<std::thread> threads;
            threads.reserve(qd);
            for (unsigned int i = 0; i < qd; ++i) {
                threads.emplace_back(worker, file.fd(), is_read, bs, num_blocks, seed + i, std::ref(start),
                                     std::cref(t_measure), std::cref(t_end), std::ref(failed), std::ref(stats[i]));
            }

            // set the window, then release every thread at once via latch countdown
            t_measure = Clock::now() + ramp;
            t_end = t_measure + runtime;
            start.count_down();

            // sleep_until doesnt really guarantee that it sleeps until since 
            // OS scheduler can take it off so it can be awoken > t_measure. However, it is 
            // acceprable for this benchamrk. Alternative is to use busy polling but that wastes
            // cpu resouces whichbis what we're trying to measure
            std::this_thread::sleep_until(t_measure);
            const Clock::time_point measure_start = Clock::now();
            rusage ru_start{};
            ::getrusage(RUSAGE_SELF, &ru_start);

            std::this_thread::sleep_until(t_end);
            const double elapsed = bench::seconds(Clock::now() - measure_start);
            rusage ru_end{};
            ::getrusage(RUSAGE_SELF, &ru_end);

            // each thread finishes its current I/O and exits
            for (auto& t : threads) {
                t.join();
            }
            for (const auto& s : stats) {
                if (!s.error.empty()) {
                    throw std::runtime_error(s.error);
                }
            }

            // merge every thread's samples, then the same math as engine_bench (bench::make_result)
            std::vector<std::uint64_t> lat_ns;
            std::uint64_t bytes = 0;
            for (auto& s : stats) {
                lat_ns.insert(lat_ns.end(), s.lat_ns.begin(), s.lat_ns.end());
                bytes += s.bytes;
            }
            return bench::make_result(lat_ns, bytes, elapsed, ru_start, ru_end);
        }
    }
}

int main(int argc, char** argv) {
    return uringtier::bench::bench_main("pool_bench", argc, argv, uringtier::run_point);
}
