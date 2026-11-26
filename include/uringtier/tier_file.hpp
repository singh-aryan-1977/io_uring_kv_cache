#pragma once

#include <cstdint>
#include <string>

namespace uringtier {
    /**
     * One file on NVMe that holds every cached KV chunk as flat array of 
     * fixed-size slots. Slot i is att i*chunk_size offset. one big file so that 
     * no open/close updates per store or load
     */
    class TierFile {
        private:
            int m_fd = -1;
            std::uint64_t m_size = 0;
            std::string m_path;

        public:

            /**
             * Constructor to open file with O_DIRECT and fallocate full size up front so 
             * writes never allocate disk blocks on hot path. O_DIRECT to skip page cache, O_CLOEXEC stops fd leaking
             * into child porcesses. Throws std::system_error on failure. EINVAL from open means O_DIRECT isnt supported
             * by file system
             * 
             * @param path where the file is
             * @param size_bytes total size of file to preallocate
             * @throws std::system_error on failure. EINVAL from open means O_DIRECT isnt supported by file system
             */
            TierFile(const std::string& path, std::uint64_t size_bytes);

            /**
             * Destructor for TierFile, which closes the fd. The file itself stays on disk.
             * If the fd is registered with a Ring, destroy the Ring first.
             */
            ~TierFile();

            /**
             * delete copy constructors so that we can't have a copy of a TierFile
             * move constructors are also deleted
             */
            TierFile(const TierFile&) = delete;
            TierFile& operator=(const TierFile&) = delete;

            /**
             * Raw file descriptor, to pass to Ring::register_file.
             *
             * @return the open fd
             */
            int fd() const noexcept {
                return m_fd;
            }

            /**
             * Size the file was preallocated to.
             *
             * @return size in bytes
             */
            std::uint64_t size() const noexcept {
                return m_size;
            }

            /**
             * Path the file was opened at.
             *
             * @return the file path
             */
            const std::string& path() const noexcept {
                return m_path;
            }
    };
}
