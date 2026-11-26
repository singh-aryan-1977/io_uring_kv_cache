#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

namespace uringtier {
    /**
     * Block of RAM whose adress and zsize are both multiples of 4 KiB since O_DIRECT 
     * needs it. Stand in for vLLM host region in tests and benchmarks
     */
    class AlignedBuffer {
        private:
            std::byte* m_data = nullptr;
            std::size_t m_size = 0;

        public:
            static constexpr std::size_t k_align = 4096;

            /**
             * AlignedBuffer constructor to allocate 4 KiB aligned memory and zero-fill it. 
             * Zeroing also touches every page so now on first benchmark I/O, we dont pay for page faults. 
             * Throws std::bad_alloc if allocation fails
             * 
             * @param bytes minimum size; rounded up to next multiple of 4 KiB
             * @throws std::bad_alloc if allocation fails
             */
            explicit AlignedBuffer(std::size_t bytes) : m_size((bytes + k_align - 1) / k_align * k_align) {
                m_data = static_cast<std::byte*>(std::aligned_alloc(k_align, m_size));
                if (!m_data) {
                    throw std::bad_alloc();
                }

                std::memset(m_data, 0, m_size);
            }

            /**
             * Destructor for AlignedBuffer, which frees the memory.
             * If it's registered with a Ring, destroy the Ring first since the kernel still holds those pages.
             */
            ~AlignedBuffer() {
                std::free(m_data);
            }

            /**
             * Delete copy constructors so we cant have a copy. Also deletes move constructors
             */
            AlignedBuffer(const AlignedBuffer&) = delete;
            AlignedBuffer& operator=(const AlignedBuffer&) = delete;

            /**
             * Start of the buffer.
             *
             * @return pointer to the memory, always 4 KiB aligned
             */
            std::byte* data() noexcept {
                return m_data;
            }

            /**
             * Size of the buffer after rounding up.
             *
             * @return size in bytes, always a multiple of 4 KiB
             */
            std::size_t size() noexcept {
                return m_size;
            }
    };
}
