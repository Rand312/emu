/* Copyright (C) 2020 The Android Open Source Project
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

#include "android_modem_v2.h"

#include "android/emulation/control/adb/AdbInterface.h"
#include "android/telephony/phone_number.h"
#include "android/utils/debug.h"

#include <functional>
#include <mutex>
#include <thread>
#include "ModemLegacy.h"
#include "ModemSimulator.h"

static std::unique_ptr<android::modem::ModemBase> s_modem{
        new android::modem::ModemLegacy()};

extern "C" {
void init_modem_simulator() {
    static std::once_flag just_once;
    std::call_once(just_once, [&]() {
            s_modem.reset(new android::modem::ModemSimulator());
    });
}
}

android::modem::ModemBase* getModemBase() {
    return s_modem.get();
}


extern int amodem_number_of_calls_vx(AModem modem) {
    return s_modem->number_of_calls(modem);
}

extern ACall amodem_call_by_idx_vx(AModem modem, int idx) {
    return s_modem->call_by_index(modem, idx);
}


void amodem_receive_sms_vx(AModem modem, SmsPDU sms) {
    s_modem->receive_sms(modem, sms);
}

ACall amodem_find_call_by_number_vx(AModem modem, const char* args) {
    return s_modem->find_call_by_number(modem, args);
}

int amodem_add_inbound_call_vx(AModem modem, const char* args) {
    return s_modem->add_inbound_call(modem, args);
}

int amodem_disconnect_call_vx(AModem modem, const char* args) {
    return s_modem->disconnect_call(modem, args);
}

int amodem_update_call_vx(AModem modem, const char* args, int state) {
    return s_modem->update_call(modem, args, state);
}

void amodem_set_data_network_type_vx(AModem modem, ADataNetworkType type) {
    s_modem->set_data_network_type(modem, type);
}

void amodem_set_signal_strength_profile_vx(AModem modem, int quality) {
    s_modem->set_signal_strength_profile(modem, quality);
}

void amodem_update_time(AModem modem) {
    s_modem->update_time(modem);
}

int amodem_update_phone_number(AModem modem, const char* number) {
    char phone_number[16];
    int ret = validate_and_parse_phone_number(number, phone_number);
    if (ret) {
        dwarning("%s: bad phone number format: %s , use digits, # and + only\n",
                 __func__, number);
        return -1;
    }

    auto adbInterface = android::emulation::AdbInterface::getGlobal();
    if (!adbInterface) {
        dwarning("%s: No adb binary found, cannot set the phone number.\n",
                 __func__);
        return -1;
    }
    int res = s_modem->set_phone_number(modem, phone_number);
    adbInterface->enqueueCommand(
            {"shell", "cmd", "connectivity", "airplane-mode", "enable"});
    adbInterface->enqueueCommand(
            {"shell", "cmd", "connectivity", "airplane-mode", "disable"});
    return res;
}

void amodem_set_data_registration_vx(AModem modem, ARegistrationState state) {
    s_modem->set_data_registration(modem, state);
}

ARegistrationState amodem_get_data_registration_vx(AModem modem) {
    return s_modem->get_data_registration(modem);
}

ARegistrationState amodem_get_voice_registration_vx(AModem modem) {
    return s_modem->get_voice_registration(modem);
}

void amodem_set_voice_registration_vx(AModem modem, ARegistrationState state) {
    s_modem->set_voice_registration(modem, state);
}

void amodem_state_save_vx(AModem modem, SysFile* file) {
    s_modem->save_sate(modem, file);
}

int amodem_state_load_vx(AModem modem, SysFile* file, int version_id) {
    return s_modem->load_sate(modem, file, version_id);
}

void amodem_set_notification_callback_vx(AModem modem,
                                         ModemCallback callbackFunc,
                                         void* userData) {
    s_modem->set_notification_callback_vx(modem, callbackFunc, userData);
}
