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
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstring>
#include <sstream>
#include <thread>

#include "absl/log/absl_log.h"
#include "absl/log/log.h"
#include "logging.h"
namespace {
bool sEnableVerbose = false;
}  // namespace

void set_gfxstream_logger(gfxstream_logger_t f) {}
void set_gfxstream_fine_logger(gfxstream_logger_t f) {}
void set_gfxstream_enable_log_colors() {}
void set_gfxstream_enable_verbose_logs() { sEnableVerbose = true; }

void OutputLog(FILE* stream, char severity, const char* file, unsigned int line,
               int64_t timestamp_us, const char* format, ...) {
    if (severity == 'V' && !sEnableVerbose) {
        return;
    }

    constexpr int bufferSize = 4096;
    char buffer[bufferSize];
    int strlen = bufferSize;
    va_list args;
    va_start(args, format);
    int size = vsnprintf(buffer, bufferSize, format, args);
    va_end(args);
    if (size >= bufferSize) {
        // Indicate trunctation.
        strncpy(buffer + bufferSize - 3, "...", 3);
    } else {
        strlen = size;
    }

    std::string_view msg(buffer, strlen);

    if (timestamp_us == 0) {
        switch (severity) {
            case 'I':  // INFO
                ABSL_LOG(INFO).AtLocation(file, line) << msg;
                break;
            case 'W':  // WARNING
                ABSL_LOG(WARNING).AtLocation(file, line) << msg;
                break;
            case 'E':  // ERROR
                ABSL_LOG(ERROR).AtLocation(file, line) << msg;
                break;
            case 'F':  // FATAL
                ABSL_LOG(FATAL).AtLocation(file, line) << msg;
                break;
            case 'V':
                VLOG(1).AtLocation(file, line) << msg;
                break;
            case 'D':
                VLOG(2).AtLocation(file, line) << msg;
                break;
        };
    } else {
        auto ts = absl::UnixEpoch() + absl::Microseconds(timestamp_us);
        switch (severity) {
            case 'I':  // INFO
                ABSL_LOG(INFO).AtLocation(file, line).WithTimestamp(ts) << msg;
                break;
            case 'W':  // WARNING
                ABSL_LOG(WARNING).AtLocation(file, line).WithTimestamp(ts) << msg;
                break;
            case 'E':  // ERROR
                ABSL_LOG(ERROR).AtLocation(file, line).WithTimestamp(ts) << msg;
                break;
            case 'F':  // FATAL
                ABSL_LOG(FATAL).AtLocation(file, line).WithTimestamp(ts) << msg;
                break;
            case 'V':
                VLOG(1).AtLocation(file, line).WithTimestamp(ts) << msg;
                break;
            case 'D':
                VLOG(2).AtLocation(file, line).WithTimestamp(ts) << msg;
                break;
        };
    }
}
