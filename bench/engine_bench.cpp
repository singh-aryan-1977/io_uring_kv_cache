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
 *   build without sanitizers otherwise the numbers are not results worthhy sincer ASAN cehcks memory accesses, and UBSAN checks on pointer castings. Also
 *   do not build with optimizer flags such as -O2
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

#include "uringtier/aligned_buffer.hpp"
#include "uringtier/ring.hpp"
#include "uringtier/tier_file.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace uringtier {
    namespace {
        using Clock = std::chrono::steady_clock;

        constexpr std::uint32_t k_fill_chunk = 16u << 20;
        constexpr unsigned int k_fill_qd = 8;

        /**
         * One row of csv so all result data points
         */
        struct Result {
            double bw_mbps = 0; // bandwith in mb per s
            double iops = 0; // I/O operations per second
            double lat_mean_us = 0; // average latency
            double p50_us = 0; // p50 latencty
            double p99_us = 0; // p99 latency
            double p999_us = 0; // p99 latency
            double usr_cpu_pct = 0; // CPU time spent in my code
            double sys_cpu_pct = 0; // CPU time spent in kernel because of my code
        };

        /**
         * Parse a size similar to fio e.g. 256k
         * 
         * @param s size string
         * @return size in bytes
         * @throws std::invalid_argument if the string isnt a size
         */
        std::uint64_t parse_size(const std::string& s) {
            std::size_t pos = 0;
            std::uint64_t n = std::stoull(s, &pos);
            if (pos == s.size()) {
                return n;
            }
            switch (s[pos]) {
                case 'k': case 'K': return n << 10;
                case 'm': case 'M': return n << 20;
                case 'g': case 'G': return n << 30;
                default: throw std::invalid_argument("bad size: " + s);
            }
        }

        /**
         * Environment variable, or a default if it isn't set
         *
         * @param name variable name
         * @param fallback value to use when env variable isnt set
         * @return the value
         */
        std::string env_or(const char* name, const char* fallback) {
            const char* v = std::getenv(name);
            return v ? v : fallback;
        }

        /**
         * Split a space-separated list like "1 8 32 64"
         *
         * @param s the list
         * @return the items
         */
        std::vector<std::string> split(const std::string& s) {
            std::istringstream in(s);
            std::vector<std::string> out;
            for (std::string item; in >> item;) {
                out.push_back(item);
            }
            return out;
        }

        double seconds(Clock::duration d) {
            return std::chrono::duration<double>(d).count();
        }

        double tv_seconds(const timeval& tv) {
            return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
        }

        /**
         * Fill a buffer with random bytes, so writes never send all-zero data
         * since all 0 bytes might be compressed by drives
         * @param buf buffer to fill
         * @param rng random source
         */
        void randomize(AlignedBuffer& buf, std::mt19937_64& rng) {
            auto* words = reinterpret_cast<std::uint64_t*>(buf.data());
            for (std::size_t i = 0; i < buf.size() / sizeof(std::uint64_t); ++i) {
                words[i] = rng();
            }
        }

        /**
         * Write the whole file once with real data before benchmarking.
         * fallocate only reserves the blocks with zeroing it out. Until a block is written, the filesystem
         * answers reads of it with zeros itself, without touching the drive making it useless. Benchmarking
         * reads on that would measure the kernel handing out zeros, not the NVMe.
         *
         * @param file the test file
         * @param rng random source for the data
         * @throws std::runtime_error if a write fails or comes back short
         */
        void fill_file(const TierFile& file, std::mt19937_64& rng) {
            // one source buffer shared by every fill write: the kernel only reads
            // from it, so concurrent writes from the same memory are fine
            AlignedBuffer buf(k_fill_chunk);
            randomize(buf, rng);

            Ring ring(k_fill_qd);
            ring.register_file(file.fd());
            iovec iov{buf.data(), buf.size()};
            ring.register_buffers({&iov, 1});

            std::uint64_t offset = 0;
            std::vector<Completion> done;
            while (offset < file.size() || ring.in_flight() > 0) {
                while (offset < file.size() && ring.in_flight() < k_fill_qd) {
                    auto len = static_cast<std::uint32_t>(std::min<std::uint64_t>(k_fill_chunk, file.size() - offset));
                    ring.queue_write(0, buf.data(), len, offset, offset);  // user_data = offset
                    offset += len;
                }
                ring.submit_and_wait(1);
                done.clear();
                ring.reap(done);
                for (const auto& c : done) {
                    auto expected = std::min<std::uint64_t>(k_fill_chunk, file.size() - c.user_data);
                    if (c.res != static_cast<std::int32_t>(expected)) {
                        throw std::runtime_error("fill write at " + std::to_string(c.user_data) + ": res " + std::to_string(c.res));
                    }
                }
            }
        }

        /**
         * Return value at specified quantile
         *
         * @param sorted latencies in ns, sorted ascending
         * @param q quantile in (0, 1], e.g. 0.99
         * @return latency in us
         */
        double percentile_us(const std::vector<std::uint64_t>& sorted, double q) {
            if (sorted.empty()) {
                return 0;
            }
            auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(sorted.size())));
            return static_cast<double>(sorted[std::max<std::size_t>(rank, 1) - 1]) / 1000.0;
        }

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
            randomize(buf, rng);

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

            const double elapsed = seconds(Clock::now() - measure_start);
            rusage ru_end{};
            ::getrusage(RUSAGE_SELF, &ru_end);

            // wait for remaining entries so we can tear them down with nothing in flight
            while (ring.in_flight() > 0) {
                ring.submit_and_wait(1);
                done.clear();
                ring.reap(done);
            }

            std::sort(lat_ns.begin(), lat_ns.end());
            Result r;
            r.bw_mbps = static_cast<double>(bytes) / elapsed / 1e6;
            r.iops = static_cast<double>(lat_ns.size()) / elapsed;
            if (!lat_ns.empty()) {
                double sum = 0;
                for (auto ns : lat_ns) {
                    sum += static_cast<double>(ns);
                }
                r.lat_mean_us = sum / static_cast<double>(lat_ns.size()) / 1000.0;
            }
            r.p50_us = percentile_us(lat_ns, 0.50);
            r.p99_us = percentile_us(lat_ns, 0.99);
            r.p999_us = percentile_us(lat_ns, 0.999);
            // Use ru stats for cpu time
            r.usr_cpu_pct = (tv_seconds(ru_end.ru_utime) - tv_seconds(ru_start.ru_utime)) / elapsed * 100.0;
            r.sys_cpu_pct = (tv_seconds(ru_end.ru_stime) - tv_seconds(ru_start.ru_stime)) / elapsed * 100.0;
            return r;
        }

        int run(int argc, char** argv) {
            if (argc != 3) {
                std::fprintf(stderr, "usage: %s <test file on NVMe> <out.csv>\n", argv[0]);
                return 2;
            }
            const std::string path = argv[1];
            const std::string out_path = argv[2];

            const std::uint64_t size = parse_size(env_or("SIZE", "64g"));
            const auto runtime = std::chrono::seconds(std::stoi(env_or("RUNTIME", "30")));
            const auto ramp = std::chrono::seconds(std::stoi(env_or("RAMP", "5")));
            const auto rw_list = split(env_or("RW_LIST", "randread randwrite"));
            const auto bs_list = split(env_or("BS_LIST", "256k 1m 2m 4m 8m 16m"));
            const auto qd_list = split(env_or("QD_LIST", "1 8 32 64"));

            if (size % AlignedBuffer::k_align != 0) {
                throw std::invalid_argument("SIZE must be a multiple of 4 KiB");
            }

            TierFile file(path, size);
            std::mt19937_64 rng(42);

            if (env_or("SKIP_FILL", "0") != "1") {
                std::fprintf(stderr, "filling %s (%llu MiB)...\n", path.c_str(), static_cast<unsigned long long>(size >> 20));
                fill_file(file, rng);
            }

            std::ofstream out(out_path);
            if (!out) {
                throw std::runtime_error("can't write " + out_path);
            }
            out << "rw,bs,qd,bw_MBps,iops,lat_mean_us,p50_us,p99_us,p999_us,usr_cpu_pct,sys_cpu_pct\n";

            for (const auto& rw : rw_list) {
                if (rw != "randread" && rw != "randwrite") {
                    throw std::invalid_argument("RW_LIST items must be randread or randwrite, got " + rw);
                }
                for (const auto& bs_str : bs_list) {
                    const std::uint64_t bs = parse_size(bs_str);
                    if (bs % AlignedBuffer::k_align != 0 || bs > size) {
                        throw std::invalid_argument("bad block size " + bs_str);
                    }
                    for (const auto& qd_str : qd_list) {
                        const auto qd = static_cast<unsigned int>(std::stoul(qd_str));
                        Result r = run_point(file, rw == "randread", static_cast<std::uint32_t>(bs), qd, ramp, runtime, rng);

                        // same formatting as the fio CSV (jq's @csv + floor)
                        char line[256];
                        std::snprintf(line, sizeof(line), "\"%s\",\"%s\",%u,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.6f,%.6f\n",
                                      rw.c_str(), bs_str.c_str(), qd,
                                      std::floor(r.bw_mbps), std::floor(r.iops), std::floor(r.lat_mean_us),
                                      std::floor(r.p50_us), std::floor(r.p99_us), std::floor(r.p999_us),
                                      r.usr_cpu_pct, r.sys_cpu_pct);
                        out << line << std::flush;
                        std::fprintf(stderr, "done %s bs=%s qd=%u: %.0f MB/s, p99 %.0f us\n",
                                     rw.c_str(), bs_str.c_str(), qd, r.bw_mbps, r.p99_us);
                    }
                }
            }
            std::fprintf(stderr, "wrote %s\n", out_path.c_str());
            return 0;
        }
    }
}

int main(int argc, char** argv) {
    try {
        return uringtier::run(argc, argv);
    } catch (const std::system_error& e) {
        std::fprintf(stderr, "engine_bench: %s\n", e.what());
        if (e.code() == std::errc::not_enough_memory) {
            std::fprintf(stderr, "hint: registered buffers exceed RLIMIT_MEMLOCK (ulimit -l); "
                                 "raise it or use smaller BS_LIST/QD_LIST\n");
        }
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "engine_bench: %s\n", e.what());
        return 1;
    }
}
