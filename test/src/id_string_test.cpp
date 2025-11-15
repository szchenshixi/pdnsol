#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "pdnsol/utils/id_string.hpp"
#include "pdnsol/utils/logging.hpp"

using namespace pdnsol;

TEST(ModuleTest, TryBasic) {
    IdString a("abc");
    IdString b("abc");
    EXPECT_EQ(a, b);
}
