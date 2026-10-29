#include "matter_thermostat.h"

#include <string.h>

#include <esp_matter.h>
#include <esp_matter_providers.h>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include "app_state.h"
#include "device_identity.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "matter_commissioner.h"
#include "sdkconfig.h"

using namespace esp_matter;
using namespace chip::app::Clusters;

namespace {

constexpr char TAG[] = "thermostat";

/* Thermostat feature bits (Matter Thermostat cluster FeatureMap). */
constexpr uint32_t kFeatureHeat = 0x1;
constexpr uint32_t kFeatureCool = 0x2;
/* ControlSequenceOfOperationEnum */
constexpr uint8_t kSeqCoolingOnly = 0;
constexpr uint8_t kSeqCoolingAndHeating = 4;

chip::CommonCaseDeviceServerInitParams sServerParams;
uint16_t sEndpointId = 0;
thermostat_caps_t sCaps;
thermostat_user_change_cb_t sUserCb;
SemaphoreHandle_t sLock;
StaticSemaphore_t sLockBuf;
thermostat_user_state_t sUser;
bool sStarted;
SemaphoreHandle_t sInitDone;
StaticSemaphore_t sInitDoneBuf;

struct Guard
{
    Guard() { xSemaphoreTake(sLock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(sLock); }
};

bool ModeAllowed(uint8_t mode)
{
    switch (mode) {
    case TSTAT_MODE_OFF:
    case TSTAT_MODE_COOL:
        return true;
    case TSTAT_MODE_HEAT:
        return sCaps.heat;
    case TSTAT_MODE_DRY:
        return sCaps.dry;
    case TSTAT_MODE_FAN_ONLY:
        return sCaps.fan_only;
    default:
        return false;
    }
}

uint8_t FabricBIndex()
{
    commissioner_info_t info;
    matter_commissioner_get_info(&info);
    return info.ready ? info.fabric_index : chip::kUndefinedFabricIndex;
}

/* Must run on the CHIP thread or with the stack lock held. */
uint8_t CountHomeFabrics()
{
    uint8_t fabricB = FabricBIndex();
    uint8_t n = 0;
    for (const auto & fb : chip::Server::GetInstance().GetFabricTable()) {
        if (fb.GetFabricIndex() != fabricB) {
            n++;
        }
    }
    return n;
}

void OpenWindowIfUncommissioned()
{
    auto & mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (CountHomeFabrics() == 0 && !mgr.IsCommissioningWindowOpen()) {
        CHIP_ERROR err = mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(CONFIG_APP_COMMISSIONING_WINDOW_S));
        if (err != CHIP_NO_ERROR) {
            app_log_error("thermostat", "OpenBasicCommissioningWindow: %s", err.AsString());
        } else {
            ESP_LOGI(TAG, "not part of any home yet: commissioning window open for %d s",
                     CONFIG_APP_COMMISSIONING_WINDOW_S);
        }
    }
}

void NotifyUser(uint32_t mask)
{
    thermostat_user_state_t copy;
    {
        Guard g;
        copy = sUser;
    }
    if (sUserCb) {
        sUserCb(&copy, mask);
    }
}

esp_err_t AttributeCallback(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                            uint32_t attribute_id, esp_matter_attr_val_t * val, void * priv_data)
{
    (void)priv_data;
    if (endpoint_id != sEndpointId || cluster_id != Thermostat::Id || val == nullptr) {
        return ESP_OK;
    }
    if (type == attribute::PRE_UPDATE) {
        if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
            if (!ModeAllowed(val->val.u8)) {
                ESP_LOGW(TAG, "rejecting SystemMode %u (not supported by the AC profile)", val->val.u8);
                return ESP_ERR_NOT_SUPPORTED;
            }
        } else if (attribute_id == Thermostat::Attributes::OccupiedCoolingSetpoint::Id) {
            if (val->val.i16 < sCaps.cool_min || val->val.i16 > sCaps.cool_max) {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id && sCaps.heat) {
            if (val->val.i16 < sCaps.heat_min || val->val.i16 > sCaps.heat_max) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        return ESP_OK;
    }
    if (type != attribute::POST_UPDATE) {
        return ESP_OK;
    }
    uint32_t mask = 0;
    {
        Guard g;
        if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
            sUser.system_mode = val->val.u8;
            mask = TSTAT_CHANGED_MODE;
        } else if (attribute_id == Thermostat::Attributes::OccupiedCoolingSetpoint::Id) {
            sUser.cool_setpoint = val->val.i16;
            mask = TSTAT_CHANGED_COOL_SP;
        } else if (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
            sUser.heat_setpoint = val->val.i16;
            mask = TSTAT_CHANGED_HEAT_SP;
        }
    }
    if (mask) {
        NotifyUser(mask);
    }
    return ESP_OK;
}

esp_err_t IdentifyCallback(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                           uint8_t effect_variant, void * priv_data)
{
    ESP_LOGI(TAG, "identify %s on endpoint %u", type == identification::START ? "start" : "stop", endpoint_id);
    return ESP_OK;
}

void AppEventCallback(const ChipDeviceEvent * event, intptr_t arg)
{
    using namespace chip::DeviceLayer;
    switch (event->Type) {
    case DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "added to a home ecosystem");
        break;
    case DeviceEventType::kFailSafeTimerExpired:
        app_log_error("thermostat", "commissioning by the hub failed (fail-safe expired)");
        break;
    case DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "a fabric was removed, %u home fabric(s) left", CountHomeFabrics());
        OpenWindowIfUncommissioned();
        break;
    case DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "commissioning window opened");
        break;
    case DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "commissioning window closed");
        break;
    default:
        break;
    }
}

esp_err_t ReadInt16(uint32_t attributeId, int16_t & out)
{
    esp_matter_attr_val_t val;
    esp_err_t err = attribute::get_val(sEndpointId, Thermostat::Id, attributeId, &val);
    if (err == ESP_OK) {
        out = val.val.i16;
    }
    return err;
}

/* CHIP thread, right after server init. */
void InitAfterServer(intptr_t)
{
    if (matter_commissioner_init_on_chip_thread(&sServerParams) != ESP_OK) {
        ESP_LOGE(TAG, "Fabric B commissioner unavailable; thermostat keeps working");
    }

    /* Restore the persisted user state (esp-matter keeps NONVOLATILE attributes in NVS). */
    esp_matter_attr_val_t val;
    {
        Guard g;
        if (attribute::get_val(sEndpointId, Thermostat::Id, Thermostat::Attributes::SystemMode::Id, &val) == ESP_OK) {
            sUser.system_mode = val.val.u8;
        }
        int16_t v;
        if (ReadInt16(Thermostat::Attributes::OccupiedCoolingSetpoint::Id, v) == ESP_OK) {
            sUser.cool_setpoint = v;
        }
        if (sCaps.heat && ReadInt16(Thermostat::Attributes::OccupiedHeatingSetpoint::Id, v) == ESP_OK) {
            sUser.heat_setpoint = v;
        }
    }

    /* A profile change may have narrowed the limits or removed a mode: bring the stored state back
     * into range so the hub never shows a value the AC cannot execute. */
    if (!ModeAllowed(sUser.system_mode)) {
        esp_matter_attr_val_t off = esp_matter_enum8(TSTAT_MODE_OFF);
        attribute::update(sEndpointId, Thermostat::Id, Thermostat::Attributes::SystemMode::Id, &off);
    }
    int16_t clamped = sUser.cool_setpoint < sCaps.cool_min ? sCaps.cool_min
        : (sUser.cool_setpoint > sCaps.cool_max ? sCaps.cool_max : sUser.cool_setpoint);
    if (clamped != sUser.cool_setpoint) {
        esp_matter_attr_val_t v = esp_matter_int16(clamped);
        attribute::update(sEndpointId, Thermostat::Id, Thermostat::Attributes::OccupiedCoolingSetpoint::Id, &v);
    }

    OpenWindowIfUncommissioned();
    sStarted = true;
    NotifyUser(TSTAT_CHANGED_BOOT | TSTAT_CHANGED_MODE | TSTAT_CHANGED_COOL_SP | TSTAT_CHANGED_HEAT_SP);
    xSemaphoreGive(sInitDone);
}

void SetLocalTemperatureWork(intptr_t arg)
{
    bool valid = (arg >> 16) & 1;
    int16_t centi = static_cast<int16_t>(arg & 0xFFFF);
    nullable<int16_t> value;
    if (valid) {
        value = centi;
    }
    esp_matter_attr_val_t val = esp_matter_nullable_int16(value);
    /* report(): stores + marks dirty for subscribers, without invoking our own update callback. */
    esp_err_t err = attribute::report(sEndpointId, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id, &val);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "LocalTemperature report failed: %s", esp_err_to_name(err));
    }
}

struct UserStateRequest
{
    thermostat_user_state_t state;
    uint32_t mask;
};

void SetUserStateWork(intptr_t arg)
{
    auto * req = reinterpret_cast<UserStateRequest *>(arg);
    if (req->mask & TSTAT_CHANGED_MODE) {
        esp_matter_attr_val_t v = esp_matter_enum8(req->state.system_mode);
        attribute::update(sEndpointId, Thermostat::Id, Thermostat::Attributes::SystemMode::Id, &v);
    }
    if (req->mask & TSTAT_CHANGED_COOL_SP) {
        esp_matter_attr_val_t v = esp_matter_int16(req->state.cool_setpoint);
        attribute::update(sEndpointId, Thermostat::Id, Thermostat::Attributes::OccupiedCoolingSetpoint::Id, &v);
    }
    if ((req->mask & TSTAT_CHANGED_HEAT_SP) && sCaps.heat) {
        esp_matter_attr_val_t v = esp_matter_int16(req->state.heat_setpoint);
        attribute::update(sEndpointId, Thermostat::Id, Thermostat::Attributes::OccupiedHeatingSetpoint::Id, &v);
    }
    chip::Platform::Delete(req);
}

void OpenWindowWork(intptr_t seconds)
{
    auto & mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (mgr.IsCommissioningWindowOpen()) {
        mgr.CloseCommissioningWindow();
    }
    CHIP_ERROR err = mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(static_cast<uint32_t>(seconds)));
    if (err != CHIP_NO_ERROR) {
        app_log_error("thermostat", "open commissioning window: %s", err.AsString());
    }
}

void CloseWindowWork(intptr_t)
{
    auto & mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (mgr.IsCommissioningWindowOpen()) {
        mgr.CloseCommissioningWindow();
    }
}

} // namespace

extern "C" esp_err_t matter_thermostat_create(const thermostat_caps_t * caps, thermostat_user_change_cb_t cb)
{
    if (sLock == nullptr) {
        sLock = xSemaphoreCreateMutexStatic(&sLockBuf);
        sInitDone = xSemaphoreCreateBinaryStatic(&sInitDoneBuf);
    }
    sCaps = *caps;
    sUserCb = cb;

    node::config_t nodeConfig;
    strlcpy(nodeConfig.root_node.basic_information.node_label, CONFIG_APP_DEVICE_NAME,
            sizeof(nodeConfig.root_node.basic_information.node_label));
    node_t * node = node::create(&nodeConfig, AttributeCallback, IdentifyCallback);
    if (node == nullptr) {
        return ESP_FAIL;
    }

    endpoint::thermostat::config_t cfg;
    cfg.thermostat.local_temperature = nullable<int16_t>(); /* null until a valid sensor value arrives */
    cfg.thermostat.system_mode = TSTAT_MODE_OFF;
    cfg.thermostat.control_sequence_of_operation = caps->heat ? kSeqCoolingAndHeating : kSeqCoolingOnly;
    cfg.thermostat.feature_flags = kFeatureCool | (caps->heat ? kFeatureHeat : 0);
    int16_t coolDefault = 2600;
    if (coolDefault < caps->cool_min || coolDefault > caps->cool_max) {
        coolDefault = caps->cool_min;
    }
    cfg.thermostat.features.cooling.occupied_cooling_setpoint = coolDefault;
    if (caps->heat) {
        int16_t heatDefault = 2400;
        if (heatDefault < caps->heat_min || heatDefault > caps->heat_max) {
            heatDefault = caps->heat_max;
        }
        cfg.thermostat.features.heating.occupied_heating_setpoint = heatDefault;
    }

    endpoint_t * ep = endpoint::thermostat::create(node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return ESP_FAIL;
    }
    sEndpointId = endpoint::get_id(ep);

    cluster_t * tstat = cluster::get(ep, Thermostat::Id);
    if (tstat == nullptr) {
        return ESP_FAIL;
    }
    /* Setpoint limits = what the AC profile can execute. */
    cluster::thermostat::attribute::create_abs_min_cool_setpoint_limit(tstat, caps->cool_min);
    cluster::thermostat::attribute::create_abs_max_cool_setpoint_limit(tstat, caps->cool_max);
    cluster::thermostat::attribute::create_min_cool_setpoint_limit(tstat, caps->cool_min);
    cluster::thermostat::attribute::create_max_cool_setpoint_limit(tstat, caps->cool_max);
    if (caps->heat) {
        cluster::thermostat::attribute::create_abs_min_heat_setpoint_limit(tstat, caps->heat_min);
        cluster::thermostat::attribute::create_abs_max_heat_setpoint_limit(tstat, caps->heat_max);
        cluster::thermostat::attribute::create_min_heat_setpoint_limit(tstat, caps->heat_min);
        cluster::thermostat::attribute::create_max_heat_setpoint_limit(tstat, caps->heat_max);
    }

    {
        Guard g;
        sUser.system_mode = TSTAT_MODE_OFF;
        sUser.cool_setpoint = coolDefault;
        sUser.heat_setpoint = caps->heat ? cfg.thermostat.features.heating.occupied_heating_setpoint : 0;
    }
    ESP_LOGI(TAG, "thermostat endpoint %u (cool %d..%d%s)", sEndpointId, caps->cool_min, caps->cool_max,
             caps->heat ? ", heat" : "");
    return ESP_OK;
}

extern "C" esp_err_t matter_thermostat_start(void)
{
    app_identity::DeviceIdentityProvider & identity = app_identity::GetDeviceIdentity();
    esp_err_t err = identity.LoadOrCreate();
    if (err != ESP_OK) {
        return err;
    }
    esp_matter::set_custom_commissionable_data_provider(&identity);
    err = esp_matter::set_server_init_params(&sServerParams);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_matter::start(AppEventCallback);
    if (err != ESP_OK) {
        return err;
    }
    /* Immediately, before Wi-Fi is up and before any controller can subscribe to us. */
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(InitAfterServer, 0) != CHIP_NO_ERROR) {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(sInitDone, pdMS_TO_TICKS(30000)) != pdTRUE) {
        app_log_error("thermostat", "post-start initialisation timed out");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

extern "C" void matter_thermostat_get_status(matter_status_t * out)
{
    memset(out, 0, sizeof(*out));
    out->started = sStarted;
    out->endpoint_id = sEndpointId;
    out->heat_feature = sCaps.heat;
    commissioner_info_t info;
    matter_commissioner_get_info(&info);
    out->commissioner_ready = info.ready;
    if (!sStarted) {
        return;
    }
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    out->home_fabric_count = CountHomeFabrics();
    out->window_open = chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
    LogErrorOnFailure(chip::DeviceLayer::GetDeviceInstanceInfoProvider()->GetVendorId(out->vendor_id));
    LogErrorOnFailure(chip::DeviceLayer::GetDeviceInstanceInfoProvider()->GetProductId(out->product_id));
    LogErrorOnFailure(app_identity::GetDeviceIdentity().GetSetupDiscriminator(out->discriminator));
}

extern "C" void matter_thermostat_get_user_state(thermostat_user_state_t * out)
{
    Guard g;
    *out = sUser;
}

extern "C" void matter_thermostat_get_caps(thermostat_caps_t * out)
{
    *out = sCaps;
}

extern "C" esp_err_t matter_thermostat_set_local_temperature(bool valid, int16_t centi_c)
{
    if (!sStarted) {
        return ESP_ERR_INVALID_STATE;
    }
    intptr_t arg = (static_cast<intptr_t>(valid ? 1 : 0) << 16) | static_cast<uint16_t>(centi_c);
    return chip::DeviceLayer::PlatformMgr().ScheduleWork(SetLocalTemperatureWork, arg) == CHIP_NO_ERROR ? ESP_OK
                                                                                                         : ESP_FAIL;
}

extern "C" esp_err_t matter_thermostat_set_user_state(const thermostat_user_state_t * state, uint32_t mask)
{
    if (!sStarted) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((mask & TSTAT_CHANGED_MODE) && !ModeAllowed(state->system_mode)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((mask & TSTAT_CHANGED_COOL_SP) && (state->cool_setpoint < sCaps.cool_min || state->cool_setpoint > sCaps.cool_max)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((mask & TSTAT_CHANGED_HEAT_SP) &&
        (!sCaps.heat || state->heat_setpoint < sCaps.heat_min || state->heat_setpoint > sCaps.heat_max)) {
        return ESP_ERR_INVALID_ARG;
    }
    auto * req = chip::Platform::New<UserStateRequest>();
    if (req == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    req->state = *state;
    req->mask = mask;
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(SetUserStateWork, reinterpret_cast<intptr_t>(req)) != CHIP_NO_ERROR) {
        chip::Platform::Delete(req);
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" esp_err_t matter_thermostat_open_commissioning_window(uint16_t seconds)
{
    if (!sStarted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (seconds < 180 || seconds > 900) {
        return ESP_ERR_INVALID_ARG; /* basic commissioning window: 3..15 min */
    }
    return chip::DeviceLayer::PlatformMgr().ScheduleWork(OpenWindowWork, seconds) == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t matter_thermostat_close_commissioning_window(void)
{
    if (!sStarted) {
        return ESP_ERR_INVALID_STATE;
    }
    return chip::DeviceLayer::PlatformMgr().ScheduleWork(CloseWindowWork, 0) == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t matter_thermostat_get_onboarding(char * qr, size_t qr_len, char * manual, size_t manual_len)
{
    if (!sStarted || qr == nullptr || manual == nullptr || qr_len == 0 || manual_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* This device joins Wi-Fi through its own portal, so it is discoverable on the IP network only. */
    chip::RendezvousInformationFlags flags(chip::RendezvousInformationFlag::kOnNetwork);
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    chip::MutableCharSpan qrSpan(qr, qr_len);
    chip::MutableCharSpan manualSpan(manual, manual_len);
    if (GetQRCode(qrSpan, flags) != CHIP_NO_ERROR || GetManualPairingCode(manualSpan, flags) != CHIP_NO_ERROR) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" size_t matter_thermostat_list_home_fabrics(matter_fabric_info_t * out, size_t max)
{
    if (!sStarted) {
        return 0;
    }
    uint8_t fabricB = FabricBIndex();
    size_t n = 0;
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    for (const auto & fb : chip::Server::GetInstance().GetFabricTable()) {
        if (fb.GetFabricIndex() == fabricB || n >= max) {
            continue;
        }
        out[n].fabric_index = fb.GetFabricIndex();
        out[n].vendor_id = static_cast<uint16_t>(fb.GetVendorId());
        chip::CharSpan label = fb.GetFabricLabel();
        size_t len = label.size() < sizeof(out[n].label) - 1 ? label.size() : sizeof(out[n].label) - 1;
        memcpy(out[n].label, label.data(), len);
        out[n].label[len] = 0;
        n++;
    }
    return n;
}
