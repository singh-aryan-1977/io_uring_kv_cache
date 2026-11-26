#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <vector>

#include "test_util.hpp"
#include "uringtier/aligned_buffer.hpp"
#include "uringtier/ring.hpp"

using namespace uringtier;
using namespace uringtier::test;

namespace {
    constexpr std::uint32_t k_chunk = 64 * 1024;
    constexpr unsigned int k_slots = 32;

    // submit + reap until nothing is in flight
    void drain(Ring& ring, std::vector<Completion>& done) {
        while (ring.in_flight() > 0) {
            ring.submit_and_wait(1);
            ring.reap(done);
        }
    }

    // write every slot from RAM to file and then read each back into
    // second buffer and compare byte for bite
    TEST(Ring, WriteThenReadBackEverySlot) {
        TempTierFile t;
        t.m_file = std::make_unique<TierFile>(test_path("ring_roundtrip"), std::uint64_t{k_chunk} * k_slots);

        AlignedBuffer src(std::size_t{k_chunk} * k_slots);
        AlignedBuffer dst(std::size_t{k_chunk} * k_slots);
        std::mt19937 rng(42);
        for (std::size_t i = 0; i < src.size(); ++i) {
            src.data()[i] = static_cast<std::byte>(rng());
        }

        Ring ring(8); 
        ring.register_file(t.m_file->fd());
        iovec iovs[2] = {{src.data(), src.size()}, {dst.data(), dst.size()}};
        ring.register_buffers(iovs);

        std::vector<Completion> done;
        for (unsigned int s = 0; s < k_slots; ++s) {
            while (!ring.queue_write(0, src.data() + s * k_chunk, k_chunk, std::uint64_t{s} * k_chunk, s)) {
                ring.submit_and_wait(1);  // SQ full so wait for a completion
                ring.reap(done);
            }
        }
        drain(ring, done);
        ASSERT_EQ(done.size(), k_slots);
        for (const auto& c : done) {
            ASSERT_EQ(c.res, static_cast<int>(k_chunk)) << "write " << c.user_data;
        }

        done.clear();

        // now after writing to file, read from file into each buffer dst
        for (unsigned int s = 0; s < k_slots; ++s) {
            while (!ring.queue_read(1, dst.data() + s * k_chunk, k_chunk, std::uint64_t{s} * k_chunk, s)) {
                ring.submit_and_wait(1);
                ring.reap(done);
            }
        }
        drain(ring, done);
        ASSERT_EQ(done.size(), k_slots);
        for (const auto& c : done) {
            ASSERT_EQ(c.res, static_cast<int>(k_chunk)) << "read " << c.user_data;
        }

        EXPECT_EQ(std::memcmp(src.data(), dst.data(), src.size()), 0);
    }

    TEST(Ring, QueueReturnsFalseWhenSqFull) {
        TempTierFile t;
        t.m_file = std::make_unique<TierFile>(test_path("ring_full"), k_chunk);
        AlignedBuffer buf(k_chunk);

        Ring ring(4);
        ring.register_file(t.m_file->fd());
        iovec iov{buf.data(), buf.size()};
        ring.register_buffers({&iov, 1});

        unsigned int queued = 0;
        while (ring.queue_read(0, buf.data(), k_chunk, 0, queued)) {
            ++queued;
        }
        EXPECT_EQ(queued, 4u);

        std::vector<Completion> done;
        drain(ring, done);
        EXPECT_EQ(done.size(), 4u);
    }
}