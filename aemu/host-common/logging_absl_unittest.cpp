// Copyright 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "absl/log/globals.h"
#include "absl/log/log.h"
#include "absl/log/log_sink_registry.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/civil_time.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "aemu/base/testing/TestUtils.h"
#include "host-common/logging.h"

namespace {

using ::testing::EndsWith;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::StartsWith;

class CaptureLogSink : public absl::LogSink {
   public:
    void Send(const absl::LogEntry& entry) override {
        char level = 'I';
        switch (entry.log_severity()) {
            case absl::LogSeverity::kInfo:
                level = 'I';
                break;
            case absl::LogSeverity::kError:
                level = 'E';
                break;
            case absl::LogSeverity::kWarning:
                level = 'W';
                break;

            case absl::LogSeverity::kFatal:
                level = 'F';
                break;
        }
        captured_log_ = absl::StrFormat("%c %s:%d |%s|- %s", level, entry.source_filename(),
                                        entry.source_line(), absl::FormatTime(entry.timestamp()),
                                        entry.text_message());
    }

    std::string captured_log_;
};

// Returns the microseconds since the Unix epoch for Sep 13, 2020 12:26:40.123456 in the machine's
// local timezone.
absl::Time defaultTimestamp() {
    absl::CivilSecond cs(2020, 9, 13, 12, 26, 40);
    return absl::FromCivil(cs, absl::UTCTimeZone());
}

class OutputLogTest : public ::testing::Test {
   protected:
    void SetUp() override {
        // Add the CaptureLogSink
        log_sink_ = std::make_unique<CaptureLogSink>();
        absl::AddLogSink(log_sink_.get());

        absl::SetVLogLevel("*", 2);
        set_gfxstream_enable_verbose_logs();

        // Set log level to capture everything (adjust as needed)
        absl::SetStderrThreshold(absl::LogSeverity::kInfo);
    }

    void TearDown() override {
        // Remove the CaptureLogSink
        absl::RemoveLogSink(log_sink_.get());
    }

    // Common test parameters
    const char* file = "test_file.cc";
    int line = 42;
    const char* format = "This is a %s message";

    // Helper to create a formatted log message string
    std::string FormattedLog(absl::Time timestamp, absl::LogSeverity severity,
                             const std::string& msg) {
        std::string formatted =
            absl::FormatTime("%Y-%m-%d %H:%M:%S", timestamp, absl::UTCTimeZone());
        return absl::StrFormat("%s: %s:%d] %s", formatted, file, line, msg);
    }

    std::unique_ptr<CaptureLogSink> log_sink_;
};

// Test INFO log level with timestamp
TEST_F(OutputLogTest, InfoLogWithTimestamp) {
    auto timestamp_us = absl::ToUnixMicros(defaultTimestamp());
    OutputLog(nullptr, 'I', file, line, timestamp_us, format, "INFO");
    EXPECT_EQ(log_sink_->captured_log_,
              "I test_file.cc:42 |2020-09-13T12:26:40+00:00|- This is a INFO message");
}

// Test WARNING log level with timestamp
TEST_F(OutputLogTest, WarningLogWithTimestamp) {
    auto timestamp_us = absl::ToUnixMicros(defaultTimestamp());
    OutputLog(nullptr, 'W', file, line, timestamp_us, format, "WARNING");
    EXPECT_EQ(log_sink_->captured_log_,
              "W test_file.cc:42 |2020-09-13T12:26:40+00:00|- This is a WARNING message");
}

// Test ERROR log level with timestamp
TEST_F(OutputLogTest, ErrorLogWithTimestamp) {
    auto timestamp_us = absl::ToUnixMicros(defaultTimestamp());
    OutputLog(nullptr, 'E', file, line, timestamp_us, format, "ERROR");
    EXPECT_EQ(log_sink_->captured_log_,
              "E test_file.cc:42 |2020-09-13T12:26:40+00:00|- This is a ERROR message");
}

// Test VERBOSE log level with timestamp
TEST_F(OutputLogTest, VerboseLogWithTimestamp) {
    auto timestamp_us = absl::ToUnixMicros(defaultTimestamp());
    OutputLog(nullptr, 'V', file, line, timestamp_us, format, "VERBOSE");
    EXPECT_EQ(log_sink_->captured_log_,
              "I test_file.cc:42 |2020-09-13T12:26:40+00:00|- This is a VERBOSE message");
}

// Test VERBOSE log level with timestamp
TEST_F(OutputLogTest, DebugLogWithTimestamp) {
    auto timestamp_us = absl::ToUnixMicros(defaultTimestamp());
    OutputLog(nullptr, 'D', file, line, timestamp_us, format, "DEBUG");
    EXPECT_EQ(log_sink_->captured_log_,
              "I test_file.cc:42 |2020-09-13T12:26:40+00:00|- This is a DEBUG message");
}

// Test truncation when message exceeds buffer size
TEST_F(OutputLogTest, Truncation) {
    std::string long_msg(4100, 'x');  // Exceeds buffer size
    auto now = absl::ToUnixMicros(defaultTimestamp());
    OutputLog(nullptr, 'I', file, line, now, "%s", long_msg.c_str());

    std::string expected_msg = long_msg.substr(0, 4093) + "...";
    EXPECT_THAT(log_sink_->captured_log_, testing::HasSubstr(expected_msg));
}

}  // namespace
