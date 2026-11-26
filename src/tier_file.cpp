#include "uringtier/tier_file.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace uringtier {
    TierFile::TierFile(const std::string& path, std::uint64_t size_bytes) : m_size(size_bytes), m_path(path) {
        m_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_DIRECT | O_CLOEXEC, 0644); // 0644 means writer can read and exec but everyone else can just read
        if (m_fd < 0) {
            throw std::system_error(errno, std::generic_category(), "open " + path);
        }

        if (::fallocate(m_fd, 0, 0, static_cast<off_t>(size_bytes)) != 0) {
            int err = errno;  // save before close() because close() might overwrite it
            ::close(m_fd);
            throw std::system_error(err, std::generic_category(), "fallocate " + path);
        }
    }

    TierFile::~TierFile() {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }
}