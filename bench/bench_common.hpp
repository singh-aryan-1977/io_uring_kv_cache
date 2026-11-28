#pragma once

/**
 * bench_common: This is a shared header file for common function sused by engine_bench. 
 */

#include "uringtier/aligned_buffer.hpp"
#include "uringtier/tier_file.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace uringtier {
    namespace bench {
        using Clock = std::chrono::steady_clock;

        constexpr std::uint32_t k_fill_chunk = 16u << 20;

        /**
         * One row of csv so all result data points
         */
        struct Result {
            double bw_mbps = 0; // bandwidth in MB per s
            double iops = 0; // I/O operations per second
            double lat_mean_us = 0; // average latency
            double p50_us = 0; // p50 latency
            double p99_us = 0; // p99 latency
            double p999_us = 0; // p99.9 latency
            double usr_cpu_pct = 0; // CPU time spent in my code
            double sys_cpu_pct = 0; // CPU time spent in kernel because of my code
        };

        /**
         * Settings for current run read from env vars. Same names and defaults as scripts/run_fio_ceiling.sh
         */
        struct Config {
            std::uint64_t size = 0;
            Clock::duration runtime{};
            Clock::duration ramp{};
            std::vector<std::string> rw_list;
            std::vector<std::string> bs_list;
            std::vector<std::string> qd_list;
            bool skip_fill = false;
        };

        /**
         * Runs one data point and returns its results. Need function pointer since we will be running
         * experiments differently for pool and engine bench 
         */
        using PointFn = Result (*)(const TierFile& file, bool is_read, std::uint32_t bs, unsigned int qd,
                                   Clock::duration ramp, Clock::duration runtime, std::mt19937_64& rng);

        /**
         * Parse a size similar to fio e.g. 256k
         *
         * @param s size string
         * @return size in bytes
         * @throws std::invalid_argument if the string isnt a size
         */
        inline std::uint64_t parse_size(const std::string& s) {
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
        inline std::string env_or(const char* name, const char* fallback) {
            const char* v = std::getenv(name);
            return v ? v : fallback;
        }

        /**
         * Split a space-separated list like "1 8 32 64"
         *
         * @param s the list
         * @return the items
         */
        inline std::vector<std::string> split(const std::string& s) {
            std::istringstream in(s);
            std::vector<std::string> out;
            for (std::string item; in >> item;) {
                out.push_back(item);
            }
            return out;
        }

        inline double seconds(Clock::duration d) {
            return std::chrono::duration<double>(d).count();
        }

        inline double tv_seconds(const timeval& tv) {
            return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
        }

        /**
         * Fill a buffer with random bytes, so writes never send all-zero data
         * since all 0 bytes might be compressed by drives
         *
         * @param buf buffer to fill
         * @param rng random source
         */
        inline void randomize(AlignedBuffer& buf, std::mt19937_64& rng) {
            auto* words = reinterpret_cast<std::uint64_t*>(buf.data());
            for (std::size_t i = 0; i < buf.size() / sizeof(std::uint64_t); ++i) {
                words[i] = rng();
            }
        }

        /**
         * Write the whole file once with real data before benchmarking.
         * fallocate only reserves the blocks without zeroing them out. Until a block is written, the filesystem
         * answers reads of it with zeros itself, without touching the drive. Benchmarking
         * reads on that would measure the kernel handing out zeros, not the NVMe.
         * Shared by both benchmarks: the fill is never measured, so how it writes doesn't matter.
         * Plain pwrite is the simplest. Usually skipped anyway (SKIP_FILL=1 on fio's filled file).
         *
         * @param file the test file
         * @param rng random source for the data
         * @throws std::runtime_error if a write fails or comes back short
         */
        inline void fill_file(const TierFile& file, std::mt19937_64& rng) {
            AlignedBuffer buf(k_fill_chunk);
            randomize(buf, rng);

            for (std::uint64_t offset = 0; offset < file.size(); offset += k_fill_chunk) {
                auto len = static_cast<std::size_t>(std::min<std::uint64_t>(k_fill_chunk, file.size() - offset));
                ssize_t ret = ::pwrite(file.fd(), buf.data(), len, static_cast<off_t>(offset));
                if (ret != static_cast<ssize_t>(len)) {
                    int err = errno;  // save before anything else can change it
                    throw std::runtime_error("fill write at " + std::to_string(offset) + ": " +
                                             (ret < 0 ? std::strerror(err) : "short write"));
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
        inline double percentile_us(const std::vector<std::uint64_t>& sorted, double q) {
            if (sorted.empty()) {
                return 0;
            }
            auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(sorted.size())));
            return static_cast<double>(sorted[std::max<std::size_t>(rank, 1) - 1]) / 1000.0;
        }

        /**
         * Turn one data point's raw measurements into a Result. Both benchmarks go through
         * this so bandwidth, latency stats and CPU % are computed identically.
         * CPU % is CPU time / measured seconds, as % of one core, like fio's usr_cpu / sys_cpu.
         *
         * @param lat_ns every measured latency in ns (sorted in place)
         * @param bytes bytes moved while measuring
         * @param elapsed measured seconds
         * @param ru_start getrusage(RUSAGE_SELF) when measuring started
         * @param ru_end getrusage(RUSAGE_SELF) when measuring stopped
         * @return the results for the CSV row
         */
        inline Result make_result(std::vector<std::uint64_t>& lat_ns, std::uint64_t bytes, double elapsed,
                                  const rusage& ru_start, const rusage& ru_end) {
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
            r.usr_cpu_pct = (tv_seconds(ru_end.ru_utime) - tv_seconds(ru_start.ru_utime)) / elapsed * 100.0;
            r.sys_cpu_pct = (tv_seconds(ru_end.ru_stime) - tv_seconds(ru_start.ru_stime)) / elapsed * 100.0;
            return r;
        }

        /**
         * Read the sweep settings from env vars, with the same defaults as scripts/run_fio_ceiling.sh
         *
         * @return the settings
         * @throws std::invalid_argument if SIZE isn't a multiple of 4 KiB
         */
        inline Config load_config() {
            Config c;
            c.size = parse_size(env_or("SIZE", "64g"));
            c.runtime = std::chrono::seconds(std::stoi(env_or("RUNTIME", "30")));
            c.ramp = std::chrono::seconds(std::stoi(env_or("RAMP", "5")));
            c.rw_list = split(env_or("RW_LIST", "randread randwrite"));
            c.bs_list = split(env_or("BS_LIST", "256k 1m 2m 4m 8m 16m"));
            c.qd_list = split(env_or("QD_LIST", "1 8 32 64"));
            c.skip_fill = env_or("SKIP_FILL", "0") == "1";
            if (c.size % AlignedBuffer::k_align != 0) {
                throw std::invalid_argument("SIZE must be a multiple of 4 KiB");
            }
            return c;
        }

        /**
         * One CSV row, same formatting as the fio CSV (jq's @csv + floor)
         *
         * @param rw pattern, e.g. randread
         * @param bs block size as written in BS_LIST, e.g. 2m
         * @param qd queue depth
         * @param r the results
         * @return the row, ending in a newline
         */
        inline std::string format_row(const std::string& rw, const std::string& bs, unsigned int qd, const Result& r) {
            char line[256];
            std::snprintf(line, sizeof(line), "\"%s\",\"%s\",%u,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.6f,%.6f\n",
                          rw.c_str(), bs.c_str(), qd,
                          std::floor(r.bw_mbps), std::floor(r.iops), std::floor(r.lat_mean_us),
                          std::floor(r.p50_us), std::floor(r.p99_us), std::floor(r.p999_us),
                          r.usr_cpu_pct, r.sys_cpu_pct);
            return line;
        }

        /**
         * The whole benchmark: parse args + env vars, open (and fill) the test file, run every
         * data point of the sweep through `run_point` (defined inidivdually for engine and pool) and write the CSV.
         *
         * @param argc from main
         * @param argv from main: <test file on NVMe> <out.csv>
         * @param run_point how this benchmark runs one data point
         * @return exit code
         * @throws std::exception on any setup or I/O failure
         */
        inline int run_sweep(int argc, char** argv, PointFn run_point) {
            if (argc != 3) {
                std::fprintf(stderr, "usage: %s <test file on NVMe> <out.csv>\n", argv[0]);
                return 2;
            }
            const std::string path = argv[1];
            const std::string out_path = argv[2];
            const Config cfg = load_config();

            TierFile file(path, cfg.size);
            std::mt19937_64 rng(42);

            if (!cfg.skip_fill) {
                std::fprintf(stderr, "filling %s (%llu MiB)...\n", path.c_str(), static_cast<unsigned long long>(cfg.size >> 20));
                fill_file(file, rng);
            }

            std::ofstream out(out_path);
            if (!out) {
                throw std::runtime_error("can't write " + out_path);
            }
            out << "rw,bs,qd,bw_MBps,iops,lat_mean_us,p50_us,p99_us,p999_us,usr_cpu_pct,sys_cpu_pct\n";

            for (const auto& rw : cfg.rw_list) {
                if (rw != "randread" && rw != "randwrite") {
                    throw std::invalid_argument("RW_LIST items must be randread or randwrite, got " + rw);
                }
                for (const auto& bs_str : cfg.bs_list) {
                    const std::uint64_t bs = parse_size(bs_str);
                    if (bs % AlignedBuffer::k_align != 0 || bs > cfg.size) {
                        throw std::invalid_argument("bad block size " + bs_str);
                    }
                    for (const auto& qd_str : cfg.qd_list) {
                        const auto qd = static_cast<unsigned int>(std::stoul(qd_str));
                        if (qd == 0) {
                            throw std::invalid_argument("queue depth must be at least 1");
                        }
                        Result r = run_point(file, rw == "randread", static_cast<std::uint32_t>(bs), qd,
                                             cfg.ramp, cfg.runtime, rng);
                        out << format_row(rw, bs_str, qd, r) << std::flush;  // flush so a crash keeps finished rows
                        std::fprintf(stderr, "done %s bs=%s qd=%u: %.0f MB/s, p99 %.0f us\n",
                                     rw.c_str(), bs_str.c_str(), qd, r.bw_mbps, r.p99_us);
                    }
                }
            }
            std::fprintf(stderr, "wrote %s\n", out_path.c_str());
            return 0;
        }

        /**
         * What main calls: run_sweep, turning any exception into an error message + exit code 1.
         *
         * @param name benchmark name for error messages
         * @param argc from main
         * @param argv from main
         * @param run_point how this benchmark runs one data point
         * @return exit code
         */
        inline int bench_main(const char* name, int argc, char** argv, PointFn run_point) {
            try {
                return run_sweep(argc, argv, run_point);
            } catch (const std::system_error& e) {
                std::fprintf(stderr, "%s: %s\n", name, e.what());
                if (e.code() == std::errc::not_enough_memory) {
                    std::fprintf(stderr, "hint: registered buffers exceed RLIMIT_MEMLOCK (ulimit -l); "
                                         "raise it or use smaller BS_LIST/QD_LIST\n");
                }
                return 1;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "%s: %s\n", name, e.what());
                return 1;
            }
        }
    }
}
