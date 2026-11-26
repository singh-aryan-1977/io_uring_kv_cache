#include "uringtier/ring.hpp"

#include <cerrno>
#include <system_error>

/**
 * 
 * General idea is 
 * AlignedBuffer (in ram KV Cache). This will be VLLM host memory but for our standin, it is AlignedBUffer
 * This goes to this Ring which is responsible for transferring from RAM to NVME 
 * NVME = TierFile
 */

namespace uringtier {
    namespace {
        [[noreturn]] void throw_neg_errno(int ret, const char* what) {
            throw std::system_error(-ret, std::generic_category(), what);  // liburing returns -errno so we need to negate it since system _error expects +ve 
        }
    }

    Ring::Ring(unsigned int queue_depth) {
        if (int ret = io_uring_queue_init(queue_depth, &m_ring, 0); ret < 0) {
            throw_neg_errno(ret, "io_uring_queue_init");
        }
    }

    Ring::~Ring() {
        io_uring_queue_exit(&m_ring);
    }

    void Ring::register_file(int fd) {
        if (int ret = io_uring_register_files(&m_ring, &fd, 1); ret < 0) {
            throw_neg_errno(ret, "io_uring_register_files");
        }
    }

    void Ring::register_buffers(std::span<const iovec> iovs) {
        int ret = io_uring_register_buffers(&m_ring, iovs.data(), static_cast<unsigned int>(iovs.size()));
        if (ret < 0) {
            throw_neg_errno(ret, "io_uring_register_buffers");
        }
    }

    bool Ring::queue_read(unsigned int buf_index, void* addr, std::uint32_t len, std::uint64_t file_offset, std::uint64_t user_data) {
        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
        if (!sqe) {
            return false;
        }

        io_uring_prep_read_fixed(sqe, 0, addr, len, file_offset, static_cast<int>(buf_index));

        sqe->flags |= IOSQE_FIXED_FILE; // needed to tell that 0 above is not normal fd but regsitered file index
        io_uring_sqe_set_data64(sqe, user_data);
        ++m_in_flight;
        return true;
    }

    bool Ring::queue_write(unsigned int buf_index, void* addr, std::uint32_t len, std::uint64_t file_offset, std::uint64_t user_data) {
        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
        if (!sqe) {
            return false;
        }

        io_uring_prep_write_fixed(sqe, 0, addr, len, file_offset, static_cast<int>(buf_index));

        sqe->flags |= IOSQE_FIXED_FILE; // needed to tell that 0 above is not normal fd but regsitered file index
        io_uring_sqe_set_data64(sqe, user_data);
        ++m_in_flight;
        return true;
    }

    void Ring::submit_and_wait(unsigned int min_completions) {
        int ret;
        // Retry only when signal interrupted wait signalled by -EINTR. ON first go, if no signal,
        // we should get number of SQES submitted (>= 0)
        do {
            ret = io_uring_submit_and_wait(&m_ring, min_completions);
        } while (ret == -EINTR);
        if (ret < 0) {
            throw_neg_errno(ret, "io_uring_submit_and_wait");
        }
    }

    unsigned int Ring::reap(std::vector<Completion>& out) {
        unsigned int n = 0; 
        unsigned int head = 0;
        io_uring_cqe* cqe;
        io_uring_for_each_cqe(&m_ring, head, cqe) {
            out.push_back({io_uring_cqe_get_data64(cqe), cqe->res});
            ++n;
        }

        io_uring_cq_advance(&m_ring, n);
        m_in_flight -= n;
        return n;
    }

}