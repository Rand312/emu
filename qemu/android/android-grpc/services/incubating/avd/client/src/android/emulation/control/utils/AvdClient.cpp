// Copyright (C) 2023 The Android Open Source Project
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
#include "android/emulation/control/utils/AvdClient.h"

#include <grpcpp/grpcpp.h>
#include <tuple>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "aemu/base/logging/Log.h"
#include "android/emulation/control/utils/GenericCallbackFunctions.h"
#include "android/grpc/utils/SimpleAsyncGrpc.h"

namespace android {
namespace emulation {
namespace control {

// #define DEBUG 1
#if DEBUG >= 1
#define DD(fmt, ...)                                               \
    dinfo("SimpleAvdClient: %s:%d| " fmt "\n", __func__, __LINE__, \
          ##__VA_ARGS__)
#else
#define DD(...) (void)0
#endif

using ::google::protobuf::Empty;

void AvdClient::getAvdInfoAsync(OnCompleted<AvdInfo> onDone) {
    if (mLoaded) {
        onDone(&mCachedInfo);
        return;
    }

    OnCompleted<AvdInfo> cacheForward =
            [this, onDone](absl::StatusOr<AvdInfo*> status) {
                if (status.ok()) {
                    mCachedInfo = *status.value();
                    mLoaded = true;
                }
                onDone(status);
            };

    auto [request, response, context] =
            createGrpcRequestContext<Empty, AvdInfo>(mClient);

    // If for some reason we don't have a sync implementation (mocks)
    // we will transform the sync call to an async one.
    mService->async()->getAvdInfo(
            context.get(), request, response,
            grpcCallCompletionHandler(context, request, response,
                                      cacheForward));
}

absl::StatusOr<AvdInfo> AvdClient::getAvdInfo() {
    if (mLoaded) {
        return mCachedInfo;
    }

    auto context = mClient->newContext();
    Empty empty;
    auto status = ConvertGrpcStatusToAbseilStatus(
            mService->getAvdInfo(context.get(), empty, &mCachedInfo));
    if (status.ok()) {
        return mCachedInfo;
    }

    return status;
}

}  // namespace control
}  // namespace emulation
}  // namespace android