#include <string.h>

#include <lib/support/CHIPMem.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/NetworkCommissioningDriver.h>

#include "app_state.h"
#include "wifi_config.h"

namespace {

struct ConnectRequest
{
    char ssid[33];
    char password[65];
};

void ConnectWork(intptr_t arg)
{
    auto * req = reinterpret_cast<ConnectRequest *>(arg);
    CHIP_ERROR err = chip::DeviceLayer::NetworkCommissioning::ESPWiFiDriver::GetInstance().ConnectWiFiNetwork(
        req->ssid, static_cast<uint8_t>(strlen(req->ssid)), req->password, static_cast<uint8_t>(strlen(req->password)));
    if (err != CHIP_NO_ERROR) {
        app_log_error("wifi", "ConnectWiFiNetwork: %s", err.AsString());
    }
    memset(req->password, 0, sizeof(req->password));
    chip::Platform::Delete(req);
}

} // namespace

extern "C" esp_err_t wifi_chip_connect_sta(const char * ssid, const char * password)
{
    size_t ssidLen = strlen(ssid);
    size_t passLen = password ? strlen(password) : 0;
    if (ssidLen == 0 || ssidLen > 32 || passLen > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    auto * req = chip::Platform::New<ConnectRequest>();
    if (req == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(req->ssid, ssid, ssidLen + 1);
    if (password) {
        memcpy(req->password, password, passLen + 1);
    } else {
        req->password[0] = 0;
    }
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(ConnectWork, reinterpret_cast<intptr_t>(req)) != CHIP_NO_ERROR) {
        memset(req->password, 0, sizeof(req->password));
        chip::Platform::Delete(req);
        return ESP_FAIL;
    }
    return ESP_OK;
}
