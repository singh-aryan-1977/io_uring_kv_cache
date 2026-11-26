#include <gtest/gtest.h>

#include <sys/stat.h>

#include "test_util.hpp"

using namespace uringtier;
using namespace uringtier::test;

TEST(TierFile, PreallocatesRequestedSize) {
    constexpr std::uint64_t k_size = 16ull << 20;

    TempTierFile t;
    t.m_file = std::make_unique<TierFile>(test_path("tierfile_size"), k_size);

    struct stat st {};
    ASSERT_EQ(::fstat(t.m_file->fd(), &st), 0);
    EXPECT_EQ(static_cast<std::uint64_t>(st.st_size), k_size);
}
