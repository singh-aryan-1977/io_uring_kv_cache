#pragma once

#include <unistd.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "uringtier/tier_file.hpp"

namespace uringtier::test {
    // Test files go in $UT_TEST_DIR (use /tmp: the VM's own disk). The pid suffix
    // keeps parallel test runs from colliding.
    inline std::string test_path(const std::string& name) {
        const char* dir = std::getenv("UT_TEST_DIR");
        return std::string(dir ? dir : ".") + "/" + name + "." + std::to_string(::getpid());
    }

    // owns a TierFile and deletes it at scope exit
    struct TempTierFile {
        std::unique_ptr<TierFile> m_file;

        ~TempTierFile() {
            if (m_file) {
                ::unlink(m_file->path().c_str());
            }
        }
    };
}
