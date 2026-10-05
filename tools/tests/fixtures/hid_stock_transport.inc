// Copyright 2015-2020 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Transport-region fixture from Arduino-ESP32 USBHID.cpp; never linked.
bool USBHID::ready(void){
    return tud_hid_n_ready(0);
}

// TinyUSB is in the process of changing the type of the last argument to
// tud_hid_report_complete_cb(), so extract the type from the version of TinyUSB that we're
// compiled with.
template <class F> struct ArgType;

template <class R, class T1, class T2, class T3>
struct ArgType<R(*)(T1, T2, T3)> {
	typedef T1 type1;
	typedef T2 type2;
	typedef T3 type3;
};

typedef ArgType<decltype(&tud_hid_report_complete_cb)>::type3 tud_hid_report_complete_cb_len_t;

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, tud_hid_report_complete_cb_len_t len){
    if (tinyusb_hid_device_input_sem) {
        xSemaphoreGive(tinyusb_hid_device_input_sem);
    }
}

bool USBHID::SendReport(uint8_t id, const void* data, size_t len, uint32_t timeout_ms){
    if(!tinyusb_hid_device_input_sem || !tinyusb_hid_device_input_mutex){
        log_e("TX Semaphore is NULL. You must call USBHID::begin() before you can send reports");
        return false;
    }

    if(xSemaphoreTake(tinyusb_hid_device_input_mutex, timeout_ms / portTICK_PERIOD_MS) != pdTRUE){
        log_e("report %u mutex failed", id);
        return false;
    }

    bool res = ready();
    if(!res){
        log_e("not ready");
    } else {
        // The semaphore may be given if the last SendReport() timed out waiting for the report to
        // be sent. Or, tud_hid_report_complete_cb() may be called an extra time, causing the
        // semaphore to be given. In these cases, take the semaphore to clear its state so that
        // we can wait for it to be given after calling tud_hid_n_report().
        xSemaphoreTake(tinyusb_hid_device_input_sem, 0);

        res = tud_hid_n_report(0, id, data, len);
        if(!res){
            log_e("report %u failed", id);
        } else {
            if(xSemaphoreTake(tinyusb_hid_device_input_sem, timeout_ms / portTICK_PERIOD_MS) != pdTRUE){
                log_e("report %u wait failed", id);
                res = false;
            }
        }
    }

    xSemaphoreGive(tinyusb_hid_device_input_mutex);
    return res;
}
