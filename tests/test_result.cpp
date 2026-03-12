#include "core/common/Result.h"

#include <gtest/gtest.h>
#include <string>

using namespace spw;

TEST(ResultTest, SuccessValue)
{
    Result<int> r(42);
    EXPECT_TRUE(r.isOk());
    EXPECT_FALSE(r.isError());
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultTest, ErrorValue)
{
    Result<int> r(ErrorInfo::fromCode(ErrorCode::DiskNotFound, "disk missing"));
    EXPECT_FALSE(r.isOk());
    EXPECT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, ErrorCode::DiskNotFound);
    EXPECT_EQ(r.error().message, "disk missing");
}

TEST(ResultTest, ValueOr)
{
    Result<int> ok(10);
    EXPECT_EQ(ok.valueOr(99), 10);

    Result<int> err(ErrorInfo::fromCode(ErrorCode::Unknown));
    EXPECT_EQ(err.valueOr(99), 99);
}

TEST(ResultTest, BoolConversion)
{
    Result<std::string> ok(std::string("hello"));
    EXPECT_TRUE(static_cast<bool>(ok));

    Result<std::string> err(ErrorInfo::fromCode(ErrorCode::Unknown));
    EXPECT_FALSE(static_cast<bool>(err));
}

TEST(ResultTest, Map)
{
    Result<int> r(5);
    auto mapped = r.map([](int v) { return v * 2; });
    EXPECT_TRUE(mapped.isOk());
    EXPECT_EQ(mapped.value(), 10);
}

TEST(ResultTest, MapOnError)
{
    Result<int> r(ErrorInfo::fromCode(ErrorCode::DiskReadError));
    auto mapped = r.map([](int v) { return v * 2; });
    EXPECT_TRUE(mapped.isError());
    EXPECT_EQ(mapped.error().code, ErrorCode::DiskReadError);
}

TEST(ResultVoidTest, Success)
{
    Result<void> r = Result<void>::ok();
    EXPECT_TRUE(r.isOk());
    EXPECT_FALSE(r.isError());
}

TEST(ResultVoidTest, Error)
{
    Result<void> r(ErrorInfo::fromCode(ErrorCode::DiskWriteError, "write failed"));
    EXPECT_FALSE(r.isOk());
    EXPECT_TRUE(r.isError());
    EXPECT_EQ(r.error().code, ErrorCode::DiskWriteError);
}
