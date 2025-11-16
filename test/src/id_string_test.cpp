#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "pdnsol/common.hpp"

using namespace pdnsol;

TEST(ModuleTest, TryBasic) {
    IdString a("abc");
    IdString b("abc");
    EXPECT_EQ(a, b);
}
