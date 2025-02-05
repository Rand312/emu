// Copyright 2020 The Android Open Source Project
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

// A small benchmark used to compare the performance of android::base::Lock
// with other mutex implementions.

#include <ostream>
#include <string>

#include <regex>
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "aemu/base/StringFormat.h"
#include "aemu/base/logging/Log.h"
#include "aemu/base/logging/LogFormatter.h"
#include "android/utils/debug.h"
#include "benchmark/benchmark.h"

#include "aemu/base/logging/LogSeverity.h"

#define BASIC_BENCHMARK_TEST(x) BENCHMARK(x)->Arg(8)->Arg(512)->Arg(8192)

using android::base::LogParams;
using android::base::NoDuplicateLinesFormatter;
using android::base::LogFormatter;
using android::base::SimpleLogFormatter;
using android::base::VerboseLogFormatter;
using android::base::GoogleLogFormatter;

inline std::string call_format(LogFormatter& formatter,
                        const LogParams& lg,
                        const char* fmt) {
    std::string formatted_string = formatter.format(lg, fmt);
    return formatted_string;
}

void BM_LG_Log_simple_string(benchmark::State& state) {
    setMinLogLevel(EMULATOR_LOG_INFO);
    while (state.KeepRunning()) {
        LOG(INFO) << "Hello world!";
    }
}

void BM_LG_Log_simple_string_logging_off(benchmark::State& state) {
    setMinLogLevel(EMULATOR_LOG_FATAL);
    while (state.KeepRunning()) {
        LOG(INFO) << "Hello world!";
    }
}

void BM_LG_QLog_simple_string(benchmark::State& state) {
    setMinLogLevel(EMULATOR_LOG_INFO);
    while (state.KeepRunning()) {
        QLOG(INFO) << "Hello world!";
    }
}

void BM_LG_VLog_simple_string_off(benchmark::State& state) {
    setMinLogLevel(EMULATOR_LOG_INFO);
    set_verbosity_mask(0);
    while (state.KeepRunning()) {
        VLOG(virtualscene) << "Hello virtual world!";
    }
}

void BM_LG_VLog_simple_string_on(benchmark::State& state) {
    setMinLogLevel(EMULATOR_LOG_INFO);
    VERBOSE_ENABLE(virtualscene);
    while (state.KeepRunning()) {
        VLOG(virtualscene) << "Hello virtual world!";
    }
}

void BM_LG_Log_simple_int(benchmark::State& state) {
    setMinLogLevel(EMULATOR_LOG_INFO);

    int i = 0;
    while (state.KeepRunning()) {
        i++;
        LOG(INFO) << i;
    }
}

void BM_LG_Log_complex_msg(benchmark::State& state) {
    setMinLogLevel(EMULATOR_LOG_INFO);

    int i = 0;
    int j = 0;
    while (state.KeepRunning()) {
        LOG(INFO) << "Hello, i: " << i++ << ", with: " << j
                  << ", and that's all!";
        j = 2 * i;
    }
}

void BM_FMT_SimpleLogFormatter(benchmark::State& state) {
    SimpleLogFormatter sf;
    while (state.KeepRunning()) {
        LogParams lg = {"filename.c", 123, EMULATOR_LOG_INFO};
          call_format(sf, lg, "Hello World");
    }
}

void BM_FMT_VerboseLogFormatter(benchmark::State& state) {
    VerboseLogFormatter sf;
    while (state.KeepRunning()) {
        LogParams lg = {"filename.c", 123, EMULATOR_LOG_INFO};
          call_format(sf, lg, "Hello World");
    }
}

void BM_FMT_GoogleLogFormatter(benchmark::State& state) {
    GoogleLogFormatter sf;
    while (state.KeepRunning()) {
        LogParams lg = {"filename.c", 123, EMULATOR_LOG_INFO};
          call_format(sf, lg, "Hello World");
    }
}


void BM_FMT_DuplicateLogFormatter(benchmark::State& state) {
    NoDuplicateLinesFormatter ndlf(std::make_shared<SimpleLogFormatter>());
    while (state.KeepRunning()) {
        LogParams lg = {"filename.c", 123, EMULATOR_LOG_INFO};
        call_format(ndlf, lg, "Hello World");
    }
}

void BM_FMT_DuplicateLogFormatterNoDupes(benchmark::State& state) {
    NoDuplicateLinesFormatter ndlf(std::make_shared<SimpleLogFormatter>());
    int i = 0;
    while (state.KeepRunning()) {
        LogParams lg = {"filename.c", i++, EMULATOR_LOG_INFO};
        call_format(ndlf, lg, "Hello World");
    }
}

void BM_FMT_DuplicateLogFormatterNoDupesStr(benchmark::State& state) {
    NoDuplicateLinesFormatter ndlf(std::make_shared<SimpleLogFormatter>());
    int i = 0;
    while (state.KeepRunning()) {
        LogParams lg = {"filename.c", 123, EMULATOR_LOG_INFO};
        i++;
        call_format(ndlf, lg, i % 2 == 0 ? "Hello World" : "Hello Friend");
    }
}

void BM_FMT_AbslStringFormat(benchmark::State& state) {
    SimpleLogFormatter sf;
    int i = 0;
    auto format_string = absl::ParsedFormat<'s', 'd', 's'>("%s (%d)\n%s");

    while (state.KeepRunning()) {
        LogParams lg = {"filename.c", 123, EMULATOR_LOG_INFO};
        auto res = absl::StrFormat(format_string,
                                     call_format(sf, lg, "Hello World"), i++,
                                     call_format(sf, lg, "Hello World"));
    }
}

void BM_FMT_AndroidStringFormat(benchmark::State& state) {
    SimpleLogFormatter sf;
    int i = 0;
    while (state.KeepRunning()) {
        LogParams lg = {"filename.c", 123, EMULATOR_LOG_INFO};
        auto res = absl::StrFormat("%s (%dx)\n%s",
                                     call_format(sf, lg, "Hello World"), i++,
                                     call_format(sf, lg, "Hello World"));
    }
}

void BM_FMT_StringConcatFormat(benchmark::State& state) {
    SimpleLogFormatter sf;
    int i = 0;
    while (state.KeepRunning()) {
        LogParams lg = {"filename.c", 123, EMULATOR_LOG_INFO};
        auto res =   call_format(sf, lg, "Hello World") + "(" +
                   std::to_string(i++) + "x)\n" +
                     call_format(sf, lg, "Hello World");
    }
}

BENCHMARK(BM_FMT_SimpleLogFormatter);
BENCHMARK(BM_FMT_VerboseLogFormatter);
BENCHMARK(BM_FMT_GoogleLogFormatter);
BENCHMARK(BM_FMT_DuplicateLogFormatter);
BENCHMARK(BM_FMT_DuplicateLogFormatterNoDupes);
BENCHMARK(BM_FMT_DuplicateLogFormatterNoDupesStr);

// Lets see who's the fastest.
BENCHMARK(BM_FMT_AbslStringFormat);
BENCHMARK(BM_FMT_AndroidStringFormat);
BENCHMARK(BM_FMT_StringConcatFormat);

BENCHMARK(BM_LG_Log_simple_string);
BENCHMARK(BM_LG_Log_simple_string_logging_off);
BENCHMARK(BM_LG_QLog_simple_string);
BENCHMARK(BM_LG_VLog_simple_string_off);
BENCHMARK(BM_LG_VLog_simple_string_on);
BENCHMARK(BM_LG_Log_simple_int);
BENCHMARK(BM_LG_Log_complex_msg);

BENCHMARK_MAIN();
