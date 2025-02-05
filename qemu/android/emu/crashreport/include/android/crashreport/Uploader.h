/* Copyright (C) 2023 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
#pragma once
#include "client/crash_report_database.h"

namespace android {
namespace crashreport {
enum class UploadResult {
    kSuccess,
    kPermanentFailure,
    kRetry,
};

// Pushes the given report to the crashpad server
// You should re-read the report upon success, as you should
// have a remote id.
UploadResult ProcessPendingReport(
        crashpad::CrashReportDatabase* database_,
        const crashpad::CrashReportDatabase::Report& report);
}  // namespace crashreport
}  // namespace android