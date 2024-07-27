//===-- MemberwiseConstructorTests.cpp ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TweakTesting.h"
#include "gmock/gmock-matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace clang {
namespace clangd {
namespace {

TWEAK_TEST(OverridePureVirtuals);

TEST_F(OverridePureVirtualsTest, MinimalAvailability) {
  EXPECT_UNAVAILABLE("class ^C {};");
}

TEST_F(OverridePureVirtualsTest, Availability) {
  EXPECT_AVAILABLE(R"cpp(

class Base {
public:
virtual void F1() = 0;
virtual void F2() = 0;
};

class ^Derived : public Base {
public:
};

)cpp");
}

TEST_F(OverridePureVirtualsTest, Edit) {
  EXPECT_EQ(apply(
                R"cpp(
class Base {
public:
virtual void F1() = 0;
virtual void F2() = 0;
};

class ^Derived : public Base {
public:
};
)cpp"),
            R"cpp(
class Base {
public:
virtual void F1() = 0;
virtual void F2() = 0;
};

class Derived : public Base {
public:
void F1() override;
void F2() override;

};
)cpp");
}

} // namespace
} // namespace clangd
} // namespace clang
