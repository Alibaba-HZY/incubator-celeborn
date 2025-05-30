/*
 * Based on ExceptionTest.cpp from Facebook Velox
 *
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fmt/format.h>
#include <folly/Random.h>
#include <gtest/gtest.h>

#include "celeborn/utils/Exceptions.h"

using namespace celeborn::utils;

struct Counter {
  mutable int counter = 0;
};

std::ostream& operator<<(std::ostream& os, const Counter& c) {
  os << c.counter;
  ++c.counter;
  return os;
}

template <>
struct fmt::formatter<Counter> {
  constexpr auto parse(format_parse_context& ctx) {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(const Counter& c, FormatContext& ctx) const {
    auto x = c.counter++;
    return format_to(ctx.out(), "{}", x);
  }
};

template <typename T>
void verifyException(
    std::function<void()> f,
    std::function<void(const T&)> exceptionVerifier) {
  try {
    f();
    FAIL() << "Expected exception of type " << typeid(T).name()
           << ", but no exception was thrown.";
  } catch (const T& e) {
    exceptionVerifier(e);
  } catch (...) {
    FAIL() << "Expected exception of type " << typeid(T).name()
           << ", but instead got an exception of a different type.";
  }
}

void verifyCelebornException(
    std::function<void()> f,
    const std::string& messagePrefix) {
  verifyException<CelebornException>(f, [&messagePrefix](const auto& e) {
    EXPECT_TRUE(folly::StringPiece{e.what()}.startsWith(messagePrefix))
        << "\nException message prefix mismatch.\n\nExpected prefix: "
        << messagePrefix << "\n\nActual message: " << e.what();
  });
}

void testExceptionTraceCollectionControl(bool userException, bool enabled) {
  // Disable rate control in the test.
  FLAGS_celeborn_exception_user_stacktrace_rate_limit_ms = 0;
  FLAGS_celeborn_exception_system_stacktrace_rate_limit_ms = 0;

  if (userException) {
    FLAGS_celeborn_exception_user_stacktrace_enabled = enabled ? true : false;
    FLAGS_celeborn_exception_system_stacktrace_enabled =
        folly::Random::oneIn(2);
  } else {
    FLAGS_celeborn_exception_system_stacktrace_enabled = enabled ? true : false;
    FLAGS_celeborn_exception_user_stacktrace_enabled = folly::Random::oneIn(2);
  }
  try {
    if (userException) {
      throw CelebornUserError(
          "file_name",
          1,
          "function_name()",
          "operator()",
          "test message",
          "",
          error_code::kArithmeticError,
          false);
    } else {
      throw CelebornRuntimeError(
          "file_name",
          1,
          "function_name()",
          "operator()",
          "test message",
          "",
          error_code::kArithmeticError,
          false);
    }
  } catch (CelebornException& e) {
    SCOPED_TRACE(fmt::format(
        "enabled: {}, user flag: {}, sys flag: {}",
        enabled,
        FLAGS_celeborn_exception_user_stacktrace_enabled,
        FLAGS_celeborn_exception_system_stacktrace_enabled));
    ASSERT_EQ(
        userException, e.exceptionType() == CelebornException::Type::kUser);
    ASSERT_EQ(enabled, e.stackTrace() != nullptr);
  }
}

void testExceptionTraceCollectionRateControl(
    bool userException,
    bool hasRateLimit) {
  // Disable rate control in the test.
  // Enable trace rate control in the test.
  FLAGS_celeborn_exception_user_stacktrace_enabled = true;
  FLAGS_celeborn_exception_system_stacktrace_enabled = true;
  // Set rate control interval to a large value to avoid time related test
  // flakiness.
  const int kRateLimitIntervalMs = 4000;
  if (hasRateLimit) {
    // Wait a bit to ensure that the last stack trace collection time has
    // passed sufficient long.
    /* sleep override */
    std::this_thread::sleep_for(
        std::chrono::milliseconds(kRateLimitIntervalMs)); // NOLINT
  }
  if (userException) {
    FLAGS_celeborn_exception_user_stacktrace_rate_limit_ms =
        hasRateLimit ? kRateLimitIntervalMs : 0;
    FLAGS_celeborn_exception_system_stacktrace_rate_limit_ms =
        folly::Random::rand32();
  } else {
    // Set rate control to a large interval to avoid time related test
    // flakiness.
    FLAGS_celeborn_exception_system_stacktrace_rate_limit_ms =
        hasRateLimit ? kRateLimitIntervalMs : 0;
    FLAGS_celeborn_exception_user_stacktrace_rate_limit_ms =
        folly::Random::rand32();
  }
  for (int iter = 0; iter < 3; ++iter) {
    try {
      if (userException) {
        throw CelebornUserError(
            "file_name",
            1,
            "function_name()",
            "operator()",
            "test message",
            "",
            error_code::kArithmeticError,
            false);
      } else {
        throw CelebornRuntimeError(
            "file_name",
            1,
            "function_name()",
            "operator()",
            "test message",
            "",
            error_code::kArithmeticError,
            false);
      }
    } catch (CelebornException& e) {
      SCOPED_TRACE(fmt::format(
          "userException: {}, hasRateLimit: {}, user limit: {}ms, sys limit: {}ms",
          userException,
          hasRateLimit,
          FLAGS_celeborn_exception_user_stacktrace_rate_limit_ms,
          FLAGS_celeborn_exception_system_stacktrace_rate_limit_ms));
      ASSERT_EQ(
          userException, e.exceptionType() == CelebornException::Type::kUser);
      ASSERT_EQ(!hasRateLimit || ((iter % 2) == 0), e.stackTrace() != nullptr);
      // NOTE: with rate limit control, we want to verify if we can collect
      // stack trace after waiting for a while.
      if (hasRateLimit && (iter % 2 != 0)) {
        /* sleep override */
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kRateLimitIntervalMs)); // NOLINT
      }
    }
  }
}

// Ensures that expressions on the stream are not evaluated unless the condition
// is met.
TEST(ExceptionTest, lazyStreamEvaluation) {
  Counter c;

  EXPECT_EQ(0, c.counter);
  CELEBORN_CHECK(true, "{}", c);
  EXPECT_EQ(0, c.counter);

  EXPECT_THROW(
      ([&]() { CELEBORN_CHECK(false, "{}", c); })(), CelebornRuntimeError);
  EXPECT_EQ(1, c.counter);

  CELEBORN_CHECK(true, "{}", c);
  EXPECT_EQ(1, c.counter);

  EXPECT_THROW(
      ([&]() { CELEBORN_USER_CHECK(false, "{}", c); })(), CelebornUserError);
  EXPECT_EQ(2, c.counter);

  EXPECT_THROW(
      ([&]() { CELEBORN_CHECK(false, "{}", c); })(), CelebornRuntimeError);
  EXPECT_EQ(3, c.counter);

  // Simple types.
  size_t i = 0;
  CELEBORN_CHECK(true, "{}", i++);
  EXPECT_EQ(0, i);
  CELEBORN_CHECK(true, "{}", ++i);
  EXPECT_EQ(0, i);

  EXPECT_THROW(
      ([&]() { CELEBORN_CHECK(false, "{}", i++); })(), CelebornRuntimeError);
  EXPECT_EQ(1, i);
  EXPECT_THROW(
      ([&]() { CELEBORN_CHECK(false, "{}", ++i); })(), CelebornRuntimeError);
  EXPECT_EQ(2, i);
}

TEST(ExceptionTest, messageCheck) {
  verifyCelebornException(
      []() { CELEBORN_CHECK(4 > 5, "Test message 1"); },
      "Exception: CelebornRuntimeError\nError Source: RUNTIME\n"
      "Error Code: INVALID_STATE\nReason: Test message 1\n"
      "Retriable: False\nExpression: 4 > 5\nFunction: operator()\nFile: ");
}

TEST(ExceptionTest, messageUnreachable) {
  verifyCelebornException(
      []() { CELEBORN_UNREACHABLE("Test message 3"); },
      "Exception: CelebornRuntimeError\nError Source: RUNTIME\n"
      "Error Code: UNREACHABLE_CODE\nReason: Test message 3\n"
      "Retriable: False\nFunction: operator()\nFile: ");
}

#define RUN_TEST(test)            \
  TEST_##test(                    \
      CELEBORN_CHECK_##test,      \
      "RUNTIME",                  \
      "INVALID_STATE",            \
      "CelebornRuntimeError");    \
  TEST_##test(                    \
      CELEBORN_USER_CHECK_##test, \
      "USER",                     \
      "INVALID_ARGUMENT",         \
      "CelebornUserError");

#define TEST_GT(macro, system, code, prefix)                               \
  verifyCelebornException(                                                 \
      []() { macro(4, 5); },                                               \
      "Exception: " prefix "\nError Source: " system "\nError Code: " code \
      "\nReason: (4 vs. 5)"                                                \
      "\nRetriable: False"                                                 \
      "\nExpression: 4 > 5"                                                \
      "\nFunction: operator()"                                             \
      "\nFile: ");                                                         \
                                                                           \
  verifyCelebornException(                                                 \
      []() { macro(3, 3); },                                               \
      "Exception: " prefix "\nError Source: " system "\nError Code: " code \
      "\nReason: (3 vs. 3)"                                                \
      "\nRetriable: False"                                                 \
      "\nExpression: 3 > 3"                                                \
      "\nFunction: operator()"                                             \
      "\nFile: ");                                                         \
                                                                           \
  verifyCelebornException(                                                 \
      []() { macro(-1, 1, "Message 1"); },                                 \
      "Exception: " prefix "\nError Source: " system "\nError Code: " code \
      "\nReason: (-1 vs. 1) Message 1"                                     \
      "\nRetriable: False"                                                 \
      "\nExpression: -1 > 1"                                               \
      "\nFunction: operator()"                                             \
      "\nFile: ");                                                         \
                                                                           \
  macro(3, 2);                                                             \
  macro(1, -1, "Message 2");

TEST(ExceptionTest, greaterThan) {
  RUN_TEST(GT);
}

#define TEST_GE(macro, system, code, prefix)                               \
  verifyCelebornException(                                                 \
      []() { macro(4, 5); },                                               \
      "Exception: " prefix "\nError Source: " system "\nError Code: " code \
      "\nReason: (4 vs. 5)"                                                \
      "\nRetriable: False"                                                 \
      "\nExpression: 4 >= 5"                                               \
      "\nFunction: operator()"                                             \
      "\nFile: ");                                                         \
                                                                           \
  verifyCelebornException(                                                 \
      []() { macro(-1, 1, "Message 1"); },                                 \
      "Exception: " prefix "\nError Source: " system "\nError Code: " code \
      "\nReason: (-1 vs. 1) Message 1"                                     \
      "\nRetriable: False"                                                 \
      "\nExpression: -1 >= 1"                                              \
      "\nFunction: operator()"                                             \
      "\nFile: ");                                                         \
                                                                           \
  macro(3, 2);                                                             \
  macro(3, 3);                                                             \
  macro(1, -1, "Message 2");

TEST(ExceptionTest, greaterEqual) {
  RUN_TEST(GE);
}

#define TEST_LT(macro, system, code, prefix)                               \
  verifyCelebornException(                                                 \
      []() { macro(5, 4); },                                               \
      "Exception: " prefix "\nError Source: " system "\nError Code: " code \
      "\nReason: (5 vs. 4)"                                                \
      "\nRetriable: False"                                                 \
      "\nExpression: 5 < 4"                                                \
      "\nFunction: operator()"                                             \
      "\nFile: ");                                                         \
                                                                           \
  verifyCelebornException(                                                 \
      []() { macro(2, 2); },                                               \
      "Exception: " prefix "\nError Source: " system "\nError Code: " code \
      "\nReason: (2 vs. 2)"                                                \
      "\nRetriable: False"                                                 \
      "\nExpression: 2 < 2"                                                \
      "\nFunction: operator()"                                             \
      "\nFile: ");                                                         \
                                                                           \
  verifyCelebornException(                                                 \
      []() { macro(1, -1, "Message 1"); },                                 \
      "Exception: " prefix "\nError Source: " system "\nError Code: " code \
      "\nReason: (1 vs. -1) Message 1"                                     \
      "\nRetriable: False"                                                 \
      "\nExpression: 1 < -1"                                               \
      "\nFunction: operator()"                                             \
      "\nFile: ");                                                         \
                                                                           \
  macro(2, 3);                                                             \
  macro(-1, 1, "Message 2");

TEST(ExceptionTest, lessThan) {
  RUN_TEST(LT);
}

#define TEST_LE(macro, system, code, prefix)                               \
  verifyCelebornException(                                                 \
      []() { macro(6, 2); },                                               \
      "Exception: " prefix "\nError Source: " system "\nError Code: " code \
      "\nReason: (6 vs. 2)"                                                \
      "\nRetriable: False"                                                 \
      "\nExpression: 6 <= 2"                                               \
      "\nFunction: operator()"                                             \
      "\nFile: ");                                                         \
                                                                           \
  verifyCelebornException(                                                 \
      []() { macro(3, -3, "Message 1"); },                                 \
      "Exception: " prefix "\nError Source: " system "\nError Code: " code \
      "\nReason: (3 vs. -3) Message 1"                                     \
      "\nRetriable: False"                                                 \
      "\nExpression: 3 <= -3"                                              \
      "\nFunction: operator()"                                             \
      "\nFile: ");                                                         \
                                                                           \
  macro(5, 54);                                                            \
  macro(1, 1);                                                             \
  macro(-3, 3, "Message 2");

TEST(ExceptionTest, lessEqual) {
  RUN_TEST(LE);
}

#define TEST_EQ(macro, system, code, prefix)                                 \
  {                                                                          \
    verifyCelebornException(                                                 \
        []() { macro(1, 2); },                                               \
        "Exception: " prefix "\nError Source: " system "\nError Code: " code \
        "\nReason: (1 vs. 2)"                                                \
        "\nRetriable: False"                                                 \
        "\nExpression: 1 == 2"                                               \
        "\nFunction: operator()"                                             \
        "\nFile: ");                                                         \
                                                                             \
    verifyCelebornException(                                                 \
        []() { macro(2, 1, "Message 1"); },                                  \
        "Exception: " prefix "\nError Source: " system "\nError Code: " code \
        "\nReason: (2 vs. 1) Message 1"                                      \
        "\nRetriable: False"                                                 \
        "\nExpression: 2 == 1"                                               \
        "\nFunction: operator()"                                             \
        "\nFile: ");                                                         \
                                                                             \
    auto t = true;                                                           \
    auto f = false;                                                          \
    macro(521, 521);                                                         \
    macro(1.1, 1.1);                                                         \
    macro(true, t, "Message 2");                                             \
    macro(f, false, "Message 3");                                            \
  }

TEST(ExceptionTest, equal) {
  RUN_TEST(EQ);
}

#define TEST_NE(macro, system, code, prefix)                                 \
  {                                                                          \
    verifyCelebornException(                                                 \
        []() { macro(1, 1); },                                               \
        "Exception: " prefix "\nError Source: " system "\nError Code: " code \
        "\nReason: (1 vs. 1)"                                                \
        "\nRetriable: False"                                                 \
        "\nExpression: 1 != 1"                                               \
        "\nFunction: operator()"                                             \
        "\nFile: ");                                                         \
                                                                             \
    verifyCelebornException(                                                 \
        []() { macro(2.2, 2.2, "Message 1"); },                              \
        "Exception: " prefix "\nError Source: " system "\nError Code: " code \
        "\nReason: (2.2 vs. 2.2) Message 1"                                  \
        "\nRetriable: False"                                                 \
        "\nExpression: 2.2 != 2.2"                                           \
        "\nFunction: operator()"                                             \
        "\nFile: ");                                                         \
                                                                             \
    auto t = true;                                                           \
    auto f = false;                                                          \
    macro(521, 522);                                                         \
    macro(1.2, 1.1);                                                         \
    macro(true, f, "Message 2");                                             \
    macro(t, false, "Message 3");                                            \
  }

TEST(ExceptionTest, notEqual) {
  RUN_TEST(NE);
}

#define TEST_NOT_NULL(macro, system, code, prefix)                           \
  {                                                                          \
    verifyCelebornException(                                                 \
        []() { macro(nullptr); },                                            \
        "Exception: " prefix "\nError Source: " system "\nError Code: " code \
        "\nRetriable: False"                                                 \
        "\nExpression: nullptr != nullptr"                                   \
        "\nFunction: operator()"                                             \
        "\nFile: ");                                                         \
    verifyCelebornException(                                                 \
        []() {                                                               \
          std::shared_ptr<int> a;                                            \
          macro(a, "Message 1");                                             \
        },                                                                   \
        "Exception: " prefix "\nError Source: " system "\nError Code: " code \
        "\nReason: Message 1"                                                \
        "\nRetriable: False"                                                 \
        "\nExpression: a != nullptr"                                         \
        "\nFunction: operator()"                                             \
        "\nFile: ");                                                         \
    auto b = std::make_shared<int>(5);                                       \
    macro(b);                                                                \
  }

TEST(ExceptionTest, notNull) {
  RUN_TEST(NOT_NULL);
}

TEST(ExceptionTest, expressionString) {
  size_t i = 1;
  size_t j = 100;
  constexpr auto msgTemplate =
      "Exception: CelebornRuntimeError"
      "\nError Source: RUNTIME"
      "\nError Code: INVALID_STATE"
      "\nReason: ({1})"
      "\nRetriable: False"
      "\nExpression: {0}"
      "\nFunction: operator()"
      "\nFile: ";

  verifyCelebornException(
      [&]() { CELEBORN_CHECK_EQ(i, j); },
      fmt::format(msgTemplate, "i == j", "1 vs. 100"));

  verifyCelebornException(
      [&]() { CELEBORN_CHECK_NE(i, 1); },
      fmt::format(msgTemplate, "i != 1", "1 vs. 1"));

  verifyCelebornException(
      [&]() { CELEBORN_CHECK_LT(i + j, j); },
      fmt::format(msgTemplate, "i + j < j", "101 vs. 100"));

  verifyCelebornException(
      [&]() { CELEBORN_CHECK_GE(i + j * 2, 1000); },
      fmt::format(msgTemplate, "i + j * 2 >= 1000", "201 vs. 1000"));
}

TEST(ExceptionTest, notImplemented) {
  verifyCelebornException(
      []() { CELEBORN_NYI(); },
      "Exception: CelebornRuntimeError\nError Source: RUNTIME\n"
      "Error Code: NOT_IMPLEMENTED\n"
      "Retriable: False\nFunction: operator()\nFile: ");

  verifyCelebornException(
      []() { CELEBORN_NYI("Message 1"); },
      "Exception: CelebornRuntimeError\nError Source: RUNTIME\n"
      "Error Code: NOT_IMPLEMENTED\nReason: Message 1\nRetriable: False\n"
      "Function: operator()\nFile: ");
}

TEST(ExceptionTest, errorCode) {
  std::string msgTemplate =
      "Exception: {}"
      "\nError Source: {}"
      "\nError Code: {}"
      "\nRetriable: {}"
      "\nExpression: {}"
      "\nFunction: {}"
      "\nFile: ";

  verifyCelebornException(
      [&]() { CELEBORN_FAIL(); },
      fmt::format(
          "Exception: {}"
          "\nError Source: {}"
          "\nError Code: {}"
          "\nRetriable: {}"
          "\nFunction: {}"
          "\nFile: ",
          "CelebornRuntimeError",
          "RUNTIME",
          "INVALID_STATE",
          "False",
          "operator()"));

  verifyCelebornException(
      [&]() { CELEBORN_USER_FAIL(); },
      fmt::format(
          "Exception: {}"
          "\nError Source: {}"
          "\nError Code: {}"
          "\nRetriable: {}"
          "\nFunction: {}"
          "\nFile: ",
          "CelebornUserError",
          "USER",
          "INVALID_ARGUMENT",
          "False",
          "operator()"));
}

TEST(ExceptionTest, context) {
  // No context.
  verifyCelebornException(
      [&]() { CELEBORN_CHECK_EQ(1, 3); },
      "Exception: CelebornRuntimeError"
      "\nError Source: RUNTIME"
      "\nError Code: INVALID_STATE"
      "\nReason: (1 vs. 3)"
      "\nRetriable: False"
      "\nExpression: 1 == 3"
      "\nFunction: operator()"
      "\nFile: ");

  // With context.
  int callCount = 0;

  struct MessageFunctionArg {
    std::string message;
    int* callCount;
  };

  auto messageFunction =
      [](celeborn::utils::CelebornException::Type exceptionType,
         void* untypedArg) {
        auto arg = static_cast<MessageFunctionArg*>(untypedArg);
        ++(*arg->callCount);
        switch (exceptionType) {
          case celeborn::utils::CelebornException::Type::kUser:
            return fmt::format("User error: {}", arg->message);
          case celeborn::utils::CelebornException::Type::kSystem:
            return fmt::format("System error: {}", arg->message);
          default:
            return fmt::format("Unexpected error type: {}", arg->message);
        }
      };

  {
    // Create multi-layer contexts with top level marked as essential.
    MessageFunctionArg topLevelTroubleshootingAid{
        "Top-level troubleshooting aid.", &callCount};
    celeborn::utils::ExceptionContextSetter additionalContext(
        {.messageFunc = messageFunction, .arg = &topLevelTroubleshootingAid});

    MessageFunctionArg midLevelTroubleshootingAid{
        "Mid-level troubleshooting aid.", &callCount};
    celeborn::utils::ExceptionContextSetter midLevelContext(
        {messageFunction, &midLevelTroubleshootingAid});

    MessageFunctionArg innerLevelTroubleshootingAid{
        "Inner-level troubleshooting aid.", &callCount};
    celeborn::utils::ExceptionContextSetter innerLevelContext(
        {messageFunction, &innerLevelTroubleshootingAid});

    verifyCelebornException(
        [&]() { CELEBORN_CHECK_EQ(1, 3); },
        "Exception: CelebornRuntimeError"
        "\nError Source: RUNTIME"
        "\nError Code: INVALID_STATE"
        "\nReason: (1 vs. 3)"
        "\nRetriable: False"
        "\nExpression: 1 == 3"
        "\nContext: System error: Inner-level troubleshooting aid."
        "\nTop-Level Context: System error: Top-level troubleshooting aid."
        "\nFunction: operator()"
        "\nFile: ");

    EXPECT_EQ(2, callCount);

    verifyCelebornException(
        [&]() { CELEBORN_USER_CHECK_EQ(1, 3); },
        "Exception: CelebornUserError"
        "\nError Source: USER"
        "\nError Code: INVALID_ARGUMENT"
        "\nReason: (1 vs. 3)"
        "\nRetriable: False"
        "\nExpression: 1 == 3"
        "\nContext: User error: Inner-level troubleshooting aid."
        "\nTop-Level Context: User error: Top-level troubleshooting aid."
        "\nFunction: operator()"
        "\nFile: ");

    EXPECT_EQ(4, callCount);
  }

  {
    callCount = 0;
    // Create multi-layer contexts with none marked as essential.
    MessageFunctionArg topLevelTroubleshootingAid{
        "Top-level troubleshooting aid.", &callCount};
    celeborn::utils::ExceptionContextSetter additionalContext(
        {.messageFunc = messageFunction, .arg = &topLevelTroubleshootingAid});

    MessageFunctionArg midLevelTroubleshootingAid{
        "Mid-level troubleshooting aid.", &callCount};
    celeborn::utils::ExceptionContextSetter midLevelContext(
        {.messageFunc = messageFunction, .arg = &midLevelTroubleshootingAid});

    MessageFunctionArg innerLevelTroubleshootingAid{
        "Inner-level troubleshooting aid.", &callCount};
    celeborn::utils::ExceptionContextSetter innerLevelContext(
        {messageFunction, &innerLevelTroubleshootingAid});

    verifyCelebornException(
        [&]() { CELEBORN_CHECK_EQ(1, 3); },
        "Exception: CelebornRuntimeError"
        "\nError Source: RUNTIME"
        "\nError Code: INVALID_STATE"
        "\nReason: (1 vs. 3)"
        "\nRetriable: False"
        "\nExpression: 1 == 3"
        "\nContext: System error: Inner-level troubleshooting aid."
        "\nTop-Level Context: System error: Top-level troubleshooting aid."
        "\nFunction: operator()"
        "\nFile: ");

    EXPECT_EQ(2, callCount);

    verifyCelebornException(
        [&]() { CELEBORN_USER_CHECK_EQ(1, 3); },
        "Exception: CelebornUserError"
        "\nError Source: USER"
        "\nError Code: INVALID_ARGUMENT"
        "\nReason: (1 vs. 3)"
        "\nRetriable: False"
        "\nExpression: 1 == 3"
        "\nContext: User error: Inner-level troubleshooting aid."
        "\nTop-Level Context: User error: Top-level troubleshooting aid."
        "\nFunction: operator()"
        "\nFile: ");

    EXPECT_EQ(4, callCount);
  }

  // Different context.
  {
    callCount = 0;

    // Create a single layer of context. Context and top-level context are
    // expected to be the same.
    MessageFunctionArg debuggingInfo{"Debugging info.", &callCount};
    celeborn::utils::ExceptionContextSetter context(
        {messageFunction, &debuggingInfo});

    verifyCelebornException(
        [&]() { CELEBORN_CHECK_EQ(1, 3); },
        "Exception: CelebornRuntimeError"
        "\nError Source: RUNTIME"
        "\nError Code: INVALID_STATE"
        "\nReason: (1 vs. 3)"
        "\nRetriable: False"
        "\nExpression: 1 == 3"
        "\nContext: System error: Debugging info."
        "\nTop-Level Context: Same as context."
        "\nFunction: operator()"
        "\nFile: ");

    EXPECT_EQ(1, callCount);

    verifyCelebornException(
        [&]() { CELEBORN_USER_CHECK_EQ(1, 3); },
        "Exception: CelebornUserError"
        "\nError Source: USER"
        "\nError Code: INVALID_ARGUMENT"
        "\nReason: (1 vs. 3)"
        "\nRetriable: False"
        "\nExpression: 1 == 3"
        "\nContext: User error: Debugging info."
        "\nTop-Level Context: Same as context."
        "\nFunction: operator()"
        "\nFile: ");

    EXPECT_EQ(2, callCount);
  }

  callCount = 0;

  // No context.
  verifyCelebornException(
      [&]() { CELEBORN_CHECK_EQ(1, 3); },
      "Exception: CelebornRuntimeError"
      "\nError Source: RUNTIME"
      "\nError Code: INVALID_STATE"
      "\nReason: (1 vs. 3)"
      "\nRetriable: False"
      "\nExpression: 1 == 3"
      "\nFunction: operator()"
      "\nFile: ");

  EXPECT_EQ(0, callCount);

  // With message function throwing an exception.
  auto throwingMessageFunction =
      [](celeborn::utils::CelebornException::Type /*exceptionType*/,
         void* untypedArg) -> std::string {
    auto arg = static_cast<MessageFunctionArg*>(untypedArg);
    ++(*arg->callCount);
    CELEBORN_FAIL("Test failure.");
  };
  {
    MessageFunctionArg debuggingInfo{"Debugging info.", &callCount};
    celeborn::utils::ExceptionContextSetter context(
        {throwingMessageFunction, &debuggingInfo});

    verifyCelebornException(
        [&]() { CELEBORN_CHECK_EQ(1, 3); },
        "Exception: CelebornRuntimeError"
        "\nError Source: RUNTIME"
        "\nError Code: INVALID_STATE"
        "\nReason: (1 vs. 3)"
        "\nRetriable: False"
        "\nExpression: 1 == 3"
        "\nContext: Failed to produce additional context."
        "\nTop-Level Context: Same as context."
        "\nFunction: operator()"
        "\nFile: ");

    EXPECT_EQ(1, callCount);
  }
}

TEST(ExceptionTest, traceCollectionEnabling) {
  // Switch on/off tests.
  for (const bool enabled : {false, true}) {
    for (const bool userException : {false, true}) {
      testExceptionTraceCollectionControl(userException, enabled);
    }
  }
}

TEST(ExceptionTest, traceCollectionRateControl) {
  // Rate limit tests.
  for (const bool withLimit : {false, true}) {
    for (const bool userException : {false, true}) {
      testExceptionTraceCollectionRateControl(userException, withLimit);
    }
  }
}

TEST(ExceptionTest, wrappedException) {
  try {
    throw std::invalid_argument("This is a test.");
  } catch (const std::exception& e) {
    CelebornUserError ve(std::current_exception(), e.what(), false);
    ASSERT_EQ(ve.message(), "This is a test.");
    ASSERT_TRUE(ve.isUserError());
    ASSERT_EQ(ve.context(), "");
    ASSERT_EQ(ve.topLevelContext(), "");
    ASSERT_THROW(
        std::rethrow_exception(ve.wrappedException()), std::invalid_argument);
  }

  try {
    throw std::invalid_argument("This is a test.");
  } catch (const std::exception& e) {
    CelebornRuntimeError ve(std::current_exception(), e.what(), false);
    ASSERT_EQ(ve.message(), "This is a test.");
    ASSERT_FALSE(ve.isUserError());
    ASSERT_EQ(ve.context(), "");
    ASSERT_EQ(ve.topLevelContext(), "");
    ASSERT_THROW(
        std::rethrow_exception(ve.wrappedException()), std::invalid_argument);
  }

  try {
    CELEBORN_FAIL("This is a test.");
  } catch (const CelebornException& e) {
    ASSERT_EQ(e.message(), "This is a test.");
    ASSERT_TRUE(e.wrappedException() == nullptr);
  }
}

TEST(ExceptionTest, wrappedExceptionWithContext) {
  auto messageFunction =
      [](celeborn::utils::CelebornException::Type exceptionType,
         void* untypedArg) {
        auto data = static_cast<char*>(untypedArg);
        switch (exceptionType) {
          case celeborn::utils::CelebornException::Type::kUser:
            return fmt::format("User error: {}", data);
          case celeborn::utils::CelebornException::Type::kSystem:
            return fmt::format("System error: {}", data);
          default:
            return fmt::format("Unexpected error type: {}", data);
        }
      };

  std::string data = "lakes";
  celeborn::utils::ExceptionContextSetter context(
      {messageFunction, data.data()});

  try {
    throw std::invalid_argument("This is a test.");
  } catch (const std::exception& e) {
    CelebornUserError ve(std::current_exception(), e.what(), false);
    ASSERT_EQ(ve.message(), "This is a test.");
    ASSERT_TRUE(ve.isUserError());
    ASSERT_EQ(ve.context(), "User error: lakes");
    ASSERT_EQ(ve.topLevelContext(), "Same as context.");
    ASSERT_THROW(
        std::rethrow_exception(ve.wrappedException()), std::invalid_argument);
  }

  try {
    throw std::invalid_argument("This is a test.");
  } catch (const std::exception& e) {
    CelebornRuntimeError ve(std::current_exception(), e.what(), false);
    ASSERT_EQ(ve.message(), "This is a test.");
    ASSERT_FALSE(ve.isUserError());
    ASSERT_EQ(ve.context(), "System error: lakes");
    ASSERT_EQ(ve.topLevelContext(), "Same as context.");
    ASSERT_THROW(
        std::rethrow_exception(ve.wrappedException()), std::invalid_argument);
  }

  std::string innerData = "mountains";
  celeborn::utils::ExceptionContextSetter innerContext(
      {messageFunction, innerData.data()});

  try {
    throw std::invalid_argument("This is a test.");
  } catch (const std::exception& e) {
    CelebornUserError ve(std::current_exception(), e.what(), false);
    ASSERT_EQ(ve.message(), "This is a test.");
    ASSERT_TRUE(ve.isUserError());
    ASSERT_EQ(ve.context(), "User error: mountains");
    ASSERT_EQ(ve.topLevelContext(), "User error: lakes");
    ASSERT_THROW(
        std::rethrow_exception(ve.wrappedException()), std::invalid_argument);
  }

  try {
    throw std::invalid_argument("This is a test.");
  } catch (const std::exception& e) {
    CelebornRuntimeError ve(std::current_exception(), e.what(), false);
    ASSERT_EQ(ve.message(), "This is a test.");
    ASSERT_FALSE(ve.isUserError());
    ASSERT_EQ(ve.context(), "System error: mountains");
    ASSERT_EQ(ve.topLevelContext(), "System error: lakes");
    ASSERT_THROW(
        std::rethrow_exception(ve.wrappedException()), std::invalid_argument);
  }
}

TEST(ExceptionTest, exceptionMacroInlining) {
  // Verify that the right formatting method is inlined when using
  // _CELEBORN_THROW macro. This test can be removed if fmt::vformat changes
  // behavior and starts ignoring extra brackets.

  // The following string should throw an error when passed to fmt::vformat.
  std::string errorStr = "This {} {is a test.";
  // Inlined with the method that directly returns the std::string input.
  try {
    CELEBORN_USER_FAIL(errorStr);
  } catch (const CelebornUserError& ve) {
    ASSERT_EQ(ve.message(), errorStr);
  }

  // Inlined with the method that directly returns the char* input.
  try {
    CELEBORN_USER_FAIL(errorStr.c_str());
  } catch (const CelebornUserError& ve) {
    ASSERT_EQ(ve.message(), errorStr);
  }

  // Inlined with the method that passes the errorStr and the next argument via
  // fmt::vformat. Should throw format_error.
  try {
    CELEBORN_USER_FAIL(errorStr, "definitely");
  } catch (const std::exception& e) {
    ASSERT_TRUE(folly::StringPiece{e.what()}.startsWith("argument not found"));
  }
}
