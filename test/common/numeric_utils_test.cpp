#include "common/numeric_utils.h"

#include <limits>

#include "gtest/gtest.h"

using namespace lbug::common;

TEST(NumericUtilsTest, CheckedUnsignedArithmeticRejectsOverflow) {
    uint64_t result = 0;
    EXPECT_TRUE(numeric_utils::tryAdd(uint64_t{40}, uint64_t{2}, result));
    EXPECT_EQ(result, 42);
    EXPECT_FALSE(
        numeric_utils::tryAdd(std::numeric_limits<uint64_t>::max(), uint64_t{1}, result));

    EXPECT_TRUE(numeric_utils::tryMultiply(uint64_t{6}, uint64_t{7}, result));
    EXPECT_EQ(result, 42);
    EXPECT_FALSE(
        numeric_utils::tryMultiply(std::numeric_limits<uint64_t>::max(), uint64_t{2}, result));
}
