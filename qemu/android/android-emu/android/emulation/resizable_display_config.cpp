/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "android/emulation/resizable_display_config.h"
#include "aemu/base/Log.h"
#include "aemu/base/logging/Log.h"
#include "aemu/base/memory/LazyInstance.h"
#include "aemu/base/misc/StringUtils.h"
#include "android/console.h"
#include "android/emulation/control/adb/AdbInterface.h"  // for AdbInterface
#include "android/metrics/MetricsReporter.h"
#include "android/skin/user-config.h"
#include "android/user-config.h"
#include "host-common/MultiDisplayPipe.h"
#include "host-common/feature_control.h"
#include "host-common/hw-config.h"
#include "host-common/opengles.h"
#include "studio_stats.pb.h"

#include <atomic>
#include <map>

using android::metrics::MetricsReporter;

namespace android {
namespace emulation {

class ResizableConfig {
public:
    ResizableConfig() {
        std::string configStr(getConsoleAgents()->settings->hw()->hw_resizable_configs);
        if (configStr == "") {
            configStr = "phone-0-1080-2340-420, unfolded-1-1768-2208-420,"
                        "tablet-2-1920-1200-240, desktop-3-1920-1080-160";
        }
        std::vector<std::string> entrys;
        android::base::splitTokens(configStr, &entrys, ",");
        if (entrys.size() != PRESET_SIZE_MAX) {
            LOG(ERROR) << "Failed to parse resizable config " << configStr;
            return;
        }
        for (auto entry : entrys) {
            std::vector<std::string> tokens;
            android::base::splitTokens(entry, &tokens, "-");
            if (tokens.size() != 5) {
                LOG(ERROR) << "Failed to parse resizable config entry "
                           << entry;
                mConfigs.clear();
                return;
            }
            int id = std::stoi(tokens[1]);
            if (id < 0 || id >= PRESET_SIZE_MAX) {
                LOG(ERROR) << "Failed to parse resizable config entry, "
                              "incorrect index "
                           << tokens[1];
                mConfigs.clear();
                return;
            }
            int width = std::stoi(tokens[2]);
            int height = std::stoi(tokens[3]);
            int dpi = std::stoi(tokens[4]);
            mConfigs[static_cast<PresetEmulatorSizeType>(id)] =
                    PresetEmulatorSizeInfo{
                            static_cast<PresetEmulatorSizeType>(id), width,
                            height, dpi};
            if (width == getConsoleAgents()->settings->hw()->hw_lcd_width &&
                height == getConsoleAgents()->settings->hw()->hw_lcd_height &&
                dpi == getConsoleAgents()->settings->hw()->hw_lcd_density) {
                mActiveConfigId = static_cast<PresetEmulatorSizeType>(id);
            }
        }
    }

    void init() {
       for (auto config : mConfigs) {
            android_setOpenglesDisplayConfigs(
                    (int)config.first, config.second.width,
                    config.second.height, config.second.dpi, config.second.dpi);
        }
        android_setOpenglesDisplayActiveConfig(mActiveConfigId);
        mTypeCount[mActiveConfigId]++;
    }

    void updateAndroidDisplayConfigPath(enum PresetEmulatorSizeType configId) {
        auto adbInterface = emulation::AdbInterface::getGlobal();
        if (!adbInterface) {
            derror("Unable to update display configuration, adb interface "
                   "unavailable");
            return;
        }

        if (shouldApplyLargeDisplaySetting(configId)) {
            adbInterface->enqueueCommand(
                    {"shell",
                     "cmd window set-ignore-orientation-request true"});
        } else {
            adbInterface->enqueueCommand(
                    {"shell",
                     "cmd window set-ignore-orientation-request false"});
        }
    }

    bool getResizableConfig(PresetEmulatorSizeType id,
                            struct PresetEmulatorSizeInfo* info) {
        if (mConfigs.find(id) == mConfigs.end()) {
            LOG(ERROR) << "Failed to find resizable config for " << id;
            return false;
        }
        *info = mConfigs[id];  // structure copy
        return true;
    }

    PresetEmulatorSizeType getResizableActiveConfigId() {
        return mActiveConfigId;
    }

    void setResizableActiveConfigId(PresetEmulatorSizeType configId) {
        if (mActiveConfigId == configId) {
            return;
        }
        mActiveConfigId = configId;
        user_config_set_resizable_config(configId);
        mTypeCount[mActiveConfigId]++;

        android_setOpenglesDisplayActiveConfig(configId);

        auto adbInterface = emulation::AdbInterface::getGlobal();
        if (!adbInterface) {
            derror("Unable to set active display configuration, adb interface "
                   "unavailable");
            return;
        }

        // SurfaceFlinger index the configId in reverse order.
        int sfConfigId = PRESET_SIZE_MAX - 1 - configId;
        std::string cmd = "su 0 service call SurfaceFlinger 1035 i32 " +
                          std::to_string(sfConfigId);
        // Tell SurfaceFlinger to change display mode to sfConfigId.
        if (feature_is_enabled(kFeature_PlayStoreImage)) {
            // on user build, su 0 won't work, try multidisplay
            setDisplay(sfConfigId);
        } else {
            adbInterface->enqueueCommand({"shell", cmd});
        }
        // window manager dpi
        cmd = "wm density " + std::to_string(mConfigs[configId].dpi);
        adbInterface->enqueueCommand({"shell", cmd});

        // tablet setting
        updateAndroidDisplayConfigPath(configId);

        registerMetrics();
    }

    bool shouldApplyLargeDisplaySetting(enum PresetEmulatorSizeType id) {
        if (id == PRESET_SIZE_UNFOLDED || id == PRESET_SIZE_TABLET ||
            id == PRESET_SIZE_DESKTOP) {
            return true;
        }
        return false;
    }

    void setDisplay(int sfConfigId) {
        // Service in guest has already started through QemuMiscPipe when
        // bootCompleted. But this service may be killed, e.g., Android low
        // memory. Send broadcast again to guarantee servce running.
        // P.S. guest Service has check to avoid duplication.
        auto adbInterface = emulation::AdbInterface::getGlobal();
        if (!adbInterface) {
            derror("Adb unavailable, not starting multidisplay "
                   "service in android. Please install adb and restart the "
                   "emulator. Multi display might not work as expected.");
            return;
        }
        adbInterface->enqueueCommand({"shell", "am", "broadcast", "-a",
                                      "com.android.emulator.multidisplay.START",
                                      "-n",
                                      "com.android.emulator.multidisplay/"
                                      ".MultiDisplayServiceReceiver",
                                      "--user 0"});

        MultiDisplayPipe* pipe = MultiDisplayPipe::getInstance();
        if (pipe) {
            std::vector<uint8_t> data;
            pipe->fillData(data, sfConfigId, -1, -1, -1, 0, 0x10);
            LOG(DEBUG) << "MultiDisplayPipe send " << sfConfigId;
            pipe->send(std::move(data));
        }
    }

    void registerMetrics() {
        if (mMetricsRegistered || !MetricsReporter::get().isReportingEnabled()) {
            return;
        }
        MetricsReporter::get().reportOnExit(
            [&](android_studio::AndroidStudioEvent* event) {
              LOG(DEBUG) << "Send resizable metrics";
              android_studio::EmulatorResizableDisplay metrics;
              metrics.set_display_phone_count(mTypeCount[PRESET_SIZE_PHONE]);
              metrics.set_display_foldable_count(mTypeCount[PRESET_SIZE_UNFOLDED]);
              metrics.set_display_tablet_count(mTypeCount[PRESET_SIZE_TABLET]);
              metrics.set_display_desktop_count(mTypeCount[PRESET_SIZE_DESKTOP]);
              event->mutable_emulator_details()
                   ->mutable_resizable_display()
                   ->CopyFrom(metrics);
            }
        );
        mMetricsRegistered = true;
    }

    bool isTransitionInProgress() const {
        bool result = mTransitionInProgress.load(std::memory_order_relaxed);
        return result;
    }

    void setTransitionInProgress(int inProgress) {
        mTransitionInProgress.store(inProgress, std::memory_order_relaxed);
    }

private:
    std::map<PresetEmulatorSizeType, PresetEmulatorSizeInfo> mConfigs;
    PresetEmulatorSizeType mActiveConfigId = PRESET_SIZE_MAX;
    std::map<PresetEmulatorSizeType, uint32_t> mTypeCount;
    bool mMetricsRegistered = false;
    std::atomic<bool> mTransitionInProgress{false};
};

static android::base::LazyInstance<ResizableConfig> sResizableConfig =
        LAZY_INSTANCE_INIT;

}  // namespace emulation
}  // namespace android

void resizableInit() {
    android::emulation::sResizableConfig->init();
}

bool getResizableConfig(enum PresetEmulatorSizeType id,
                        struct PresetEmulatorSizeInfo* info) {
    return android::emulation::sResizableConfig->getResizableConfig(id, info);
}

enum PresetEmulatorSizeType getResizableActiveConfigId() {
    return android::emulation::sResizableConfig->getResizableActiveConfigId();
}

void setResizableActiveConfigId(enum PresetEmulatorSizeType id) {
    android::emulation::sResizableConfig->setResizableActiveConfigId(id);
}

void updateAndroidDisplayConfigPath(enum PresetEmulatorSizeType id) {
    android::emulation::sResizableConfig->updateAndroidDisplayConfigPath(id);
}

bool isResizableTransitionInProgress() {
    if (!resizableEnabled())
        return false;

    return android::emulation::sResizableConfig->isTransitionInProgress();
}

void setResizableTransitionInProgress(bool inProgress) {
    if (!resizableEnabled())
        return;
    android::emulation::sResizableConfig->setTransitionInProgress(inProgress);
}

bool resizableEnabled34() {
    const char* pconfigs =
            getConsoleAgents()->settings->hw()->hw_resizable_configs;
    if (!pconfigs) {
        return false;
    }

    std::string configStr(pconfigs);
    if (configStr.empty()) {
        return false;
    }

    if(!feature_is_enabled(kFeature_HWCMultiConfigs)) {
        return false;
    }

    if(!feature_is_enabled(kFeature_SupportPixelFold)) {
        return false;
    }
    return true;
}

const char* getResizableOverlayName() {
    if (resizableEnabled34()) {
        return "pixel_fold";
    } else {
        return NULL;
    }
}

bool resizableEnabled() {
    if (resizableEnabled34()) {
        return true;
    }
    return getConsoleAgents()->settings->hw()->hw_device_name &&
           !strcmp(getConsoleAgents()->settings->hw()->hw_device_name, "resizable") &&
           feature_is_enabled(kFeature_HWCMultiConfigs);
}
