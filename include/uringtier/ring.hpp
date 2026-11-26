#pragma once

#include <liburing.h>
#include <sys/uio.h>

#include <cstdint>
#include <span>
#include <vector>

namespace uringtier {

    /**
     * A finished I/O, copied out of the completion queue by Ring::reap.
     */
    struct Completion {
        std::uint64_t user_data;  // user tag to identify sqes
        std::int32_t res;  // res from cqe or -errno on failure
    };

    /**
     * IO_uring wrapper. Simple for now as more perf enhancements will be added later.
     * Moves bytes between registered memory (vLLM's host region, but for now our own
     * AlignedBuffer) and a registered NVMe file (TierFile). Doesn't own either of them.
     * To use it, register file + buffers first, then repeatedly queue_* -> submit_and_wait -> reap.
     */
    class Ring {
        private:
            io_uring m_ring{};

            unsigned int m_in_flight = 0;

        public:

            /**
             * Ring constructor to create a ring with `queue_depth` submission slots.
             * Note the kernel makes the completion queue twice the size of `queue_depth` by default.
             * Throws std::system_error on failure. If io_uring is blocked, we should get EPERM/ENOSYS.
             *
             * @param queue_depth number of submission queue slots
             * @throws std::system_error on failure. If io_uring is blocked, we should get EPERM/ENOSYS.
             */
            explicit Ring(unsigned int queue_depth);

            /**
             * Destructor for ring, which also unregisters the file and buffers.
             * Before destroying the ring, drain any in-flight I/O first.
             */
            ~Ring();

            // delete copy constructors so that we can't have a copy of a Ring
            // deleting copy constructors also deletes move constructors
            Ring(const Ring&) = delete;
            Ring& operator=(const Ring&) = delete;

            /**
             * Registers file as fixed file 0 so kernel can keep its own reference
             * and I/O can skip fd lookup. Call once per ring. We throw std::system_error on failure
             * 
             * @param fd file descriptor to register
             * @throws std::system_error on failure
             */
            void register_file(int fd);

            /**
             * Register memory regions as fixed bufferes so kernel can pin pages once 
             * instead of on every I/O. This pinned memory counts against RLIMIT_MEMLOCK
             * which is Linux cap on how much memory a user can lock. If we exceed, we get ENOMEM. Each region
             * must be at most 1GiB. Call once per ring and throw std::system_error on failure
             * 
             * @param iovs memory regions to reigster; buffer index = position in iovs
             * @throws std::system_error on failure. If we exceed RLIMIT_MEMLOCK, we get ENOMEM
             */
            void register_buffers(std::span<const iovec> iovs);

            /**
             * Queue a read from registered file into registered buffer. Only queues and doesnt reach the kernel
             * until submit_and_wait is called. With O_DIRECT, addr, len and file_offset must be 4KiB aligned
             * 
             * @param buf_index index of registered buffer that addr lies in
             * @param addr where to read into; [addr, addr+len] must be inisde buffer[buf_index]
             * @param len num of bytes to read
             * @param file_offset byte offset in file to read from
             * @param user_data user tag returned in Completion
             * @return true if queued, false if submission queue is full
             * 
             */
            bool queue_read(unsigned int buf_index, void* addr, std::uint32_t len, std::uint64_t file_offset, std::uint64_t user_data);

            /**
             * Queues a write from reigstered buffer into registered file. Same rules as queue read
             * 
             * @param buf_index index of registered buffer that addr lies in
             * @param addr where to read into; [addr, addr+len] must be inisde buffer[buf_index]
             * @param len num of bytes to read
             * @param file_offset byte offset in file to read from
             * @param user_data user tag returned in Completion
             * @return true if queued, false if submission queue is full
             */
            bool queue_write(unsigned int buf_index, void* addr, std::uint32_t len, std::uint64_t file_offset, std::uint64_t user_data);

            /**
             * Hand every queued request to kernel and then blocks until at least min_completions
             * cqes are ready. Retries if a signal interrupts the wait (by checking -EINTR). Throw std::systemm_error on failure
             * Blocking operation so fine for testing and benchmarking for now unless we pass in 0 as min_completions
             * 
             * @param min_completions number of completions to wait for (0 = submit without waiting)
             * @throws std::system_error on failure
             */
            void submit_and_wait(unsigned int min_completions);

            /**
             * Copy every completion that is ready into out and mark them as consumed and 
             * decrease in_flgiht. Check each completion res since I/O can fail
             * 
             * @param out vector that completions get appended to
             * @return number of completions ready
             */
            unsigned int reap(std::vector<Completion>& out);

            /**
             * Number of requests queued but not reaped yet.
             *
             * @return requests in flight
             */
            unsigned int in_flight() const noexcept {
                return m_in_flight;
            }
    };
}
