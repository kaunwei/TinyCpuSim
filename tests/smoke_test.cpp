#include <gtest/gtest.h>
#include "tinyarmsim/cpu/common.hpp"

TEST(SmokeTest, VersionStringIsNotEmpty) {
    auto version = tinyarmsim::get_version_string();
    EXPECT_FALSE(version.empty());
    EXPECT_EQ(version, "0.1.0");
}
