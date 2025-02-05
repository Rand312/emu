// Copyright 2019 The Android Open Source Project
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

#include "aemu/base/LayoutResolver.h"

#include <gtest/gtest.h>

#include "aemu/base/ArraySize.h"

namespace android {
namespace base {

typedef struct testDisplayInput {
    uint32_t id;
    uint32_t width;
    uint32_t height;
} testDisplayInput;

typedef struct testDisplayOutput {
    uint32_t id;
    uint32_t x;
    uint32_t y;
} testDisplayOutput;

TEST(LayoutResolver, twoIdenticalDisplays) {
    const std::pair<testDisplayInput, testDisplayOutput> kData[] = {
            std::make_pair(testDisplayInput{0, 1080, 1960},
                           testDisplayOutput{0, 0, 0}),
            std::make_pair(testDisplayInput{1, 1080, 1960},
                           testDisplayOutput{1, 1080, 0})};
    const size_t kDataSize = ARRAY_SIZE(kData);
    const double kMonitorAspectRatio = 16.0 / 9.0;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> displays;
    for (size_t i = 0; i < kDataSize; i++) {
        const auto& input = kData[i].first;
        displays[input.id] = std::make_pair(input.width, input.height);
    }

    const auto layout = resolveLayout(displays, kMonitorAspectRatio);
    for (size_t i = 0; i < kDataSize; i++) {
        const auto& expectedOutput = kData[i].second;
        const auto& actualOutput = layout.find(expectedOutput.id);
        EXPECT_NE(actualOutput, layout.end());
        EXPECT_EQ(actualOutput->second.first, expectedOutput.x);
        EXPECT_EQ(actualOutput->second.second, expectedOutput.y);
    }
}

TEST(LayoutResolver, FourDifferentDisplays) {
    const std::pair<testDisplayInput, testDisplayOutput> kData[] = {
            std::make_pair(testDisplayInput{0, 1080, 1960},
                           testDisplayOutput{0, 0, 1860}),
            std::make_pair(testDisplayInput{1, 1080, 1860},
                           testDisplayOutput{1, 0, 0}),
            std::make_pair(testDisplayInput{2, 1200, 1300},
                           testDisplayOutput{2, 1080, 1860}),
            std::make_pair(testDisplayInput{3, 1200, 1400},
                           testDisplayOutput{3, 1080, 0})};
    const size_t kDataSize = ARRAY_SIZE(kData);
    const double kMonitorAspectRatio = 16.0 / 9.0;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> displays;
    for (size_t i = 0; i < kDataSize; i++) {
        const auto& input = kData[i].first;
        displays[input.id] = std::make_pair(input.width, input.height);
    }

    const auto layout = resolveLayout(displays, kMonitorAspectRatio);
    for (size_t i = 0; i < kDataSize; i++) {
        const auto& expectedOutput = kData[i].second;
        const auto& actualOutput = layout.find(expectedOutput.id);
        EXPECT_NE(actualOutput, layout.end());
        EXPECT_EQ(actualOutput->second.first, expectedOutput.x);
        EXPECT_EQ(actualOutput->second.second, expectedOutput.y);
    }
}

TEST(LayoutResolver, threeDisplaysWithDistantDisplayStackedLayout) {
    const std::array<std::pair<testDisplayInput, testDisplayOutput>, 3> kTestSet[] = {
            // 3 Displys including one wide width display
            // Wide display should be located on the first row
            // Remaining displays on the second row should be on the centor area
            {
                std::make_pair(testDisplayInput{0, 1080, 600},
                            testDisplayOutput{0, 1760, 0}),
                std::make_pair(testDisplayInput{6, 400, 600},
                            testDisplayOutput{6, 2840, 0}),
                std::make_pair(testDisplayInput{7, 5000, 600},
                            testDisplayOutput{7, 0, 600})
            }};

    for (const auto kData : kTestSet) {
        size_t kDataSize = ARRAY_SIZE(kData);
        std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> displays;
        for (size_t i = 0; i < kDataSize; i++) {
            const auto& input = kData[i].first;
            displays[input.id] = std::make_pair(input.width, input.height);
        }

        const auto layout = resolveStackedLayout(displays, true);
        for (size_t i = 0; i < kDataSize; i++) {
            const auto& expectedOutput = kData[i].second;
            const auto& actualOutput = layout.find(expectedOutput.id);
            EXPECT_EQ(actualOutput->second.first, expectedOutput.x);
            EXPECT_EQ(actualOutput->second.second, expectedOutput.y);
        }
    }
}

TEST(LayoutResolver, fourDisplaysWithMumdStackedLayout) {
    const std::array<std::pair<testDisplayInput, testDisplayOutput>, 4> kTestSet[] = {
            // 4 Displys - main, cluster and 2 mumd displays
            // main and cluster displays should be placed on the first row while
            // keeping the remaining 2 displays on the second row
            {
                std::make_pair(testDisplayInput{0, 1080, 600},
                            testDisplayOutput{0, 340, 600}),
                std::make_pair(testDisplayInput{6, 400, 600},
                            testDisplayOutput{6, 1420, 600}),
                std::make_pair(testDisplayInput{7, 1080, 600},
                            testDisplayOutput{7, 0, 0}),
                std::make_pair(testDisplayInput{8, 1080, 600},
                            testDisplayOutput{8, 1080, 0})
            }};

    for (const auto kData : kTestSet) {
        size_t kDataSize = ARRAY_SIZE(kData);
        std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> displays;
        for (size_t i = 0; i < kDataSize; i++) {
            const auto& input = kData[i].first;
            displays[input.id] = std::make_pair(input.width, input.height);
        }

        const auto layout = resolveStackedLayout(displays, false);

        for (size_t i = 0; i < kDataSize; i++) {
            const auto& expectedOutput = kData[i].second;
            const auto& actualOutput = layout.find(expectedOutput.id);
            EXPECT_EQ(actualOutput->second.first, expectedOutput.x);
            EXPECT_EQ(actualOutput->second.second, expectedOutput.y);
        }
    }
}


}  // namespace base
}  // namespace android
