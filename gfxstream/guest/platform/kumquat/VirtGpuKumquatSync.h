/*
 * Copyright 2023 Google
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "Sync.h"

namespace gfxstream {

class VirtGpuKumquatSyncHelper : public SyncHelper {
   public:
    VirtGpuKumquatSyncHelper();

    int wait(int syncFd, int timeoutMilliseconds) override;

    int dup(int syncFd) override;

    int close(int syncFd) override;
};

}  // namespace gfxstream
