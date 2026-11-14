#include "splendor/play/json.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

using splendor::play::Json;

// Logs and reports are stored as hosts wrote them: key order and exact integers survive.
TEST(PlayJson, ValuesDumpAsParsed) {
    const std::string text =
        R"({"z":18446744073709551615,"a":-9223372036854775808,"m":[true,null,0.5,"\u00e9\n"]})";
    const Json value = Json::parse(text);
    EXPECT_EQ(value.at("z").as_uint("z"), UINT64_MAX);
    EXPECT_EQ(value.dump(), R"({"z":18446744073709551615,"a":-9223372036854775808,"m":[true,null,0.5,"é\n"]})");
    EXPECT_EQ(Json::parse(value.dump()), value);
}

// Untrusted logs and requests: anything but one well-formed value is rejected.
TEST(PlayJson, MalformedTextIsRejected) {
    for (const char* text : {"", "{} {}", R"({"a":1,"a":2})", R"("\ud800")", "[1,]", "01", "nul"}) {
        EXPECT_THROW(static_cast<void>(Json::parse(text)), std::invalid_argument) << text;
    }
    // Nesting is bounded, so a hostile log cannot exhaust the stack.
    EXPECT_THROW(static_cast<void>(Json::parse(std::string(100'000, '[') + std::string(100'000, ']'))),
                 std::invalid_argument);
    EXPECT_NO_THROW(static_cast<void>(Json::parse(std::string(64, '[') + std::string(64, ']'))));
}

TEST(PlayJson, CheckedReadsEnforceTheirContract) {
    const Json value = Json::parse(R"({"seat":4,"zero":0,"negative":-1,"half":1.5})");
    EXPECT_EQ(value.at("seat").as_uint("seat", 3), 4U);
    EXPECT_THROW(static_cast<void>(value.at("seat").as_uint("seat", 2)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(value.at("zero").as_uint("zero", 8, true)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(value.at("negative").as_uint("negative")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(value.at("half").as_uint("half")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(value.at("seat").as_string("seat")), std::invalid_argument);
    EXPECT_NO_THROW(static_cast<void>(value.closed({"half", "negative", "zero", "seat"}, "request")));
    EXPECT_THROW(static_cast<void>(value.closed({"seat", "zero", "negative"}, "request")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(value.closed({"seat", "zero", "negative", "half", "extra"}, "request")),
                 std::invalid_argument);
}

}  // namespace
