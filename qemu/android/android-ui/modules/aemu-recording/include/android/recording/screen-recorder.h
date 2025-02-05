// Copyright (C) 2017 The Android Open Source Project
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

#pragma once

#include <stdint.h>                                   // for uint32_t
#include <stdbool.h>

#include "android/emulation/control/display_agent.h"  // for QAndroidDisplay...
#include "android/emulation/control/multi_display_agent.h"
#include "android/utils/compiler.h"                   // for ANDROID_BEGIN_H...
#include "android/emulation/control/record_screen_agent.h"

ANDROID_BEGIN_HEADER


// Initializes internal global structure. Call this before doing any recording
// operations. |w| and |h| are the FrameBuffer width and height. |dpy_agent| is
// the display agent for recording in guest mode. If |dpy_agent| is NULL, then
// the recorder will assume it is in host mode.
extern void screen_recorder_init(uint32_t w,
                                 uint32_t h,
                                 const QAndroidDisplayAgent* dpy_agent,
                                 const QAndroidMultiDisplayAgent* mdpy_agent);
// Starts recording the screen. When stopped, the file will be saved as
// |info->filename|. Set |async| true do not block as recording initialization
// takes time. Returns true if recorder started recording, false if it failed.
extern bool screen_recorder_start(const RecordingInfo* info, bool async);
// Stop recording. After calling this function, the encoder will stop processing
// frames. The encoder still needs to process any remaining frames it has, so
// calling this does not mean that the encoder has finished and |filename| is
// ready. Attach a RecordingStoppedCallback to get an update when the encoder
// has finished. Set |async| to false if you want to block until recording is
// finished.
extern bool screen_recorder_stop(bool async);
// Get the recorder's current state.
extern RecorderStates screen_recorder_state_get(void);
// Starts the shared memory region. Note that the desired framerate
// can be ignored.
extern const char* start_shared_memory_module(int desiredFps);
// Stops the webrtc module
extern bool stop_shared_memory_module();
ANDROID_END_HEADER
