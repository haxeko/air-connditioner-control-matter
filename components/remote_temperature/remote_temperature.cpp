#include "remote_temperature.h"

#include <string.h>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>
#include <app/data-model/Decode.h>
#include <app/data-model/Nullable.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <lib/core/ScopedNodeId.h>
#include <platform/CHIPDeviceLayer.h>

#include "app_state.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "local_sensor.h"
#include "matter_commissioner.h"
#include "rt_logic.h"
#include "sdkconfig.h"

using namespace chip;
using namespace chip::app;

namespace {

constexpr char TAG[] = "remote_temp";
constexpr uint16_t kBindingVersion = 1;
constexpr uint32_t kStaleCheckMs = 10000;
constexpr uint32_t kWaitCommissionerMs = 5000;

SemaphoreHandle_t gLock;
StaticSemaphore_t gLockBuf;
/* Serialises Publish() (CHIP thread, local sensor task, API callers) so that the sink sees the
 * transitions in order; the sink only queues work and never blocks. */
SemaphoreHandle_t gPubLock;
StaticSemaphore_t gPubLockBuf;
rt_status_t gStatus;
app_config_t gCfg;
rt_sink_cb_t gSink;
bool gStarted;

/* last published effective value (to detect transitions) */
bool gPubValid;
int16_t gPubCenti;
temp_source_t gPubSource = TEMP_SRC_NONE;
bool gPubOnce;

#if CONFIG_APP_TEST_SIMULATED_SENSOR
bool gSimEnabled;
int16_t gSimCenti;
#endif

struct Guard
{
    Guard() { xSemaphoreTake(gLock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(gLock); }
};

void SetState(rt_state_t st, const char * err)
{
    Guard g;
    gStatus.state = st;
    if (err != nullptr) {
        strlcpy(gStatus.error, err, sizeof(gStatus.error));
    }
}

/* Choose the effective source and notify the sink on transitions or new samples. */
void Publish(bool newSample)
{
    xSemaphoreTake(gPubLock, portMAX_DELAY);
    bool valid = false;
    int16_t centi = 0;
    temp_source_t src = TEMP_SRC_NONE;
    int64_t now = app_time_us();
    {
        Guard g;
        bool matterOk = gStatus.bound && gStatus.has_value && !gStatus.value_null && gStatus.value_plausible &&
            !gStatus.stale;
        int64_t localMaxAge = 0;
#if !CONFIG_APP_LOCAL_SENSOR_NONE
        localMaxAge = (int64_t)(3 * CONFIG_APP_LOCAL_SENSOR_PERIOD_S + 60) * 1000000LL;
#endif
        bool localOk = gStatus.local_configured && gStatus.local_valid && gCfg.allow_local_fallback &&
            (now - gStatus.local_time_us) < localMaxAge;
#if CONFIG_APP_TEST_SIMULATED_SENSOR
        if (gSimEnabled) {
            valid = true;
            centi = gSimCenti;
            src = TEMP_SRC_SIMULATED;
        } else
#endif
            if (matterOk) {
            valid = true;
            centi = gStatus.value_centi;
            src = TEMP_SRC_MATTER;
        } else if (localOk) {
            valid = true;
            centi = gStatus.local_centi;
            src = TEMP_SRC_LOCAL;
        }
        gStatus.source = src;
        gStatus.effective_valid = valid;
        gStatus.effective_centi = valid ? centi : 0;
    }
    bool changed = !gPubOnce || valid != gPubValid || src != gPubSource || (valid && centi != gPubCenti);
    if (changed || (newSample && valid)) {
        gPubOnce = true;
        gPubValid = valid;
        gPubCenti = centi;
        gPubSource = src;
        if (gSink) {
            gSink(valid, centi, src);
        }
    }
    xSemaphoreGive(gPubLock);
}

esp_err_t SaveBinding(const sensor_binding_t & b)
{
    return cfg_set_blob(CFG_PART_MAIN, CFG_NS_SENSOR, "binding", &b, sizeof(b));
}

/* ------------------------------------------------------------------------------------------ */

class SensorSubscription : public ReadClient::Callback
{
public:
    /* CHIP thread */
    void Start()
    {
        CancelTimers();
        if (mClient != nullptr) {
            return;
        }
        sensor_binding_t b;
        bool bound;
        {
            Guard g;
            bound = gStatus.bound;
            b = gStatus.binding;
        }
        if (!bound) {
            SetState(RT_UNBOUND, "");
            return;
        }
        commissioner_info_t info;
        matter_commissioner_get_info(&info);
        if (!info.ready) {
            SetState(RT_WAITING_COMMISSIONER, "Đang chờ commissioner (fabric B)");
            DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(kWaitCommissionerMs), RestartTimer, this);
            return;
        }
        if (b.fabric_id != info.fabric_id || b.compressed_fabric_id != info.compressed_fabric_id) {
            SetState(RT_ERROR, "Fabric B của ESP đã thay đổi: cần ghép lại cảm biến");
            return;
        }
        auto * systemState = Controller::DeviceControllerFactory::GetInstance().GetSystemState();
        if (systemState == nullptr) {
            SetState(RT_ERROR, "Controller chưa khởi tạo");
            return;
        }

        namespace TM = Clusters::TemperatureMeasurement::Attributes;
        mPaths[0] = AttributePathParams(b.endpoint, Clusters::TemperatureMeasurement::Id, TM::MeasuredValue::Id);
        mPaths[1] = AttributePathParams(b.endpoint, Clusters::TemperatureMeasurement::Id, TM::MinMeasuredValue::Id);
        mPaths[2] = AttributePathParams(b.endpoint, Clusters::TemperatureMeasurement::Id, TM::MaxMeasuredValue::Id);

        mClient = Platform::New<ReadClient>(InteractionModelEngine::GetInstance(), systemState->ExchangeMgr(), *this,
                                            ReadClient::InteractionType::Subscribe);
        if (mClient == nullptr) {
            ScheduleRestart(CHIP_ERROR_NO_MEMORY);
            return;
        }
        ReadPrepareParams params;
        params.mpAttributePathParamsList = mPaths;
        params.mAttributePathParamsListSize = 3;
        params.mMinIntervalFloorSeconds = CONFIG_APP_SENSOR_MIN_INTERVAL_S;
        params.mMaxIntervalCeilingSeconds = CONFIG_APP_SENSOR_MAX_INTERVAL_S;
        params.mKeepSubscriptions = false; /* drop our stale subscription on the sensor after a reboot */
        params.mIsFabricFiltered = true;
        SetState(RT_CONNECTING, "");
        CHIP_ERROR err = mClient->SendAutoResubscribeRequest(ScopedNodeId(b.node_id, info.fabric_index), std::move(params));
        if (err != CHIP_NO_ERROR) {
            Platform::Delete(mClient);
            mClient = nullptr;
            ScheduleRestart(err);
            return;
        }
        ESP_LOGI(TAG, "subscribing to node 0x%016llX endpoint %u", (unsigned long long)b.node_id, b.endpoint);
    }

    /* CHIP thread */
    void Stop()
    {
        CancelTimers();
        if (mClient != nullptr) {
            mStopping = true;
            Platform::Delete(mClient); /* destructor shuts the subscription down, OnDone is not called */
            mClient = nullptr;
            mStopping = false;
        }
        mAttempt = 0;
    }

    void RetryNow()
    {
        if (mClient != nullptr) {
            if (!mClient->TriggerResubscribeIfScheduled("user request")) {
                ESP_LOGI(TAG, "no resubscription pending");
            }
            return;
        }
        mAttempt = 0;
        Start();
    }

    void OnNetworkUp()
    {
        if (mClient != nullptr) {
            mClient->TriggerResubscribeIfScheduled("network up");
        } else if (mRestartPending) {
            CancelTimers();
            Start();
        }
    }

    void CheckStale()
    {
        bool changed = false;
        {
            Guard g;
            if (gStatus.bound && gStatus.alive_time_us != 0) {
                int64_t age = app_time_us() - gStatus.alive_time_us;
                bool stale = age > (int64_t)gStatus.stale_timeout_s * 1000000LL;
                if (stale != gStatus.stale) {
                    gStatus.stale = stale;
                    changed = true;
                }
            } else if (gStatus.bound && !gStatus.stale && gStatus.alive_time_us == 0) {
                /* never heard from the sensor since boot */
                gStatus.stale = true;
                changed = true;
            }
        }
        if (changed) {
            ESP_LOGW(TAG, "sensor data %s", gStatus.stale ? "is stale (no report within the timeout)" : "is fresh again");
            Publish(false);
        }
    }

private:
    static void RestartTimer(System::Layer *, void * ctx)
    {
        auto * self = static_cast<SensorSubscription *>(ctx);
        self->mRestartPending = false;
        self->Start();
    }

    void CancelTimers()
    {
        DeviceLayer::SystemLayer().CancelTimer(RestartTimer, this);
        mRestartPending = false;
    }

    void ScheduleRestart(CHIP_ERROR cause)
    {
        uint32_t delay = rt_backoff_ms(mAttempt++, CONFIG_APP_SENSOR_RETRY_MIN_S * 1000u,
                                       CONFIG_APP_SENSOR_RETRY_MAX_S * 1000u, esp_random());
        {
            Guard g;
            gStatus.state = RT_RETRYING;
            gStatus.retries++;
            gStatus.next_retry_ms = delay;
            snprintf(gStatus.error, sizeof(gStatus.error), "%s", cause.AsString());
        }
        mRestartPending = true;
        DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(delay), RestartTimer, this);
    }

    void OnAttributeData(const ConcreteDataAttributePath & path, TLV::TLVReader * data, const StatusIB & status) override
    {
        namespace TM = Clusters::TemperatureMeasurement::Attributes;
        if (path.mClusterId != Clusters::TemperatureMeasurement::Id) {
            return;
        }
        if (!status.IsSuccess() || data == nullptr) {
            Guard g;
            snprintf(gStatus.error, sizeof(gStatus.error), "Cảm biến trả lỗi cho attribute 0x%04" PRIX32,
                     path.mAttributeId);
            return;
        }
        DataModel::Nullable<int16_t> v;
        if (DataModel::Decode(*data, v) != CHIP_NO_ERROR) {
            return;
        }
        bool sample = false;
        {
            Guard g;
            int64_t now = app_time_us();
            gStatus.alive_time_us = now;
            switch (path.mAttributeId) {
            case TM::MeasuredValue::Id:
                gStatus.has_value = true;
                gStatus.value_null = v.IsNull();
                gStatus.value_centi = v.IsNull() ? 0 : v.Value();
                gStatus.value_time_us = now;
                gStatus.value_plausible = !v.IsNull() &&
                    rt_value_plausible(gStatus.value_centi, gStatus.has_bounds_min, gStatus.min_centi,
                                       gStatus.has_bounds_max, gStatus.max_centi);
                sample = true;
                break;
            case TM::MinMeasuredValue::Id:
                gStatus.has_bounds_min = !v.IsNull();
                gStatus.min_centi = v.IsNull() ? 0 : v.Value();
                break;
            case TM::MaxMeasuredValue::Id:
                gStatus.has_bounds_max = !v.IsNull();
                gStatus.max_centi = v.IsNull() ? 0 : v.Value();
                break;
            default:
                break;
            }
            gStatus.stale = false;
        }
        if (sample) {
            Publish(true);
        }
    }

    void NotifySubscriptionStillActive(const ReadClient & client) override
    {
        bool wasStale;
        {
            Guard g;
            wasStale = gStatus.stale;
            gStatus.alive_time_us = app_time_us();
            gStatus.stale = false;
        }
        if (wasStale) {
            Publish(false);
        }
    }

    void OnSubscriptionEstablished(SubscriptionId id) override
    {
        mAttempt = 0;
        uint16_t minFloor = 0, maxCeiling = 0;
        if (mClient != nullptr) {
            LogErrorOnFailure(mClient->GetReportingIntervals(minFloor, maxCeiling));
        }
        {
            Guard g;
            gStatus.state = RT_SUBSCRIBED;
            gStatus.error[0] = 0;
            gStatus.min_interval_s = minFloor;
            gStatus.max_interval_s = maxCeiling;
            gStatus.stale_timeout_s = rt_stale_timeout_s(maxCeiling ? maxCeiling : CONFIG_APP_SENSOR_MAX_INTERVAL_S,
                                                         gCfg.stale_min_s);
            gStatus.next_retry_ms = 0;
            gStatus.alive_time_us = app_time_us();
        }
        ESP_LOGI(TAG, "subscription 0x%08" PRIX32 " established: interval %u..%u s, stale after %lu s", id, minFloor,
                 maxCeiling, (unsigned long)gStatus.stale_timeout_s);
        Publish(false);
    }

    CHIP_ERROR OnResubscriptionNeeded(ReadClient * client, CHIP_ERROR cause) override
    {
        uint32_t delay = rt_backoff_ms(mAttempt++, CONFIG_APP_SENSOR_RETRY_MIN_S * 1000u,
                                       CONFIG_APP_SENSOR_RETRY_MAX_S * 1000u, esp_random());
        {
            Guard g;
            gStatus.state = RT_RETRYING;
            gStatus.retries++;
            gStatus.next_retry_ms = delay;
            snprintf(gStatus.error, sizeof(gStatus.error), "%s", cause.AsString());
        }
        ESP_LOGW(TAG, "subscription lost (%" CHIP_ERROR_FORMAT "), retry in %lu ms", cause.Format(), (unsigned long)delay);
        /* Always set up a fresh CASE session: the old one is most likely dead. */
        return client->ScheduleResubscription(delay, NullOptional, true);
    }

    void OnError(CHIP_ERROR err) override
    {
        Guard g;
        snprintf(gStatus.error, sizeof(gStatus.error), "%s", err.AsString());
    }

    void OnDone(ReadClient * client) override
    {
        if (client == mClient) {
            mClient = nullptr;
        }
        Platform::Delete(client);
        if (!mStopping) {
            ScheduleRestart(CHIP_ERROR_INTERNAL);
        }
    }

    void OnDeallocatePaths(ReadPrepareParams && params) override
    {
        /* paths are members of this object */
        params.mpAttributePathParamsList = nullptr;
        params.mAttributePathParamsListSize = 0;
    }

    ReadClient * mClient = nullptr;
    AttributePathParams mPaths[3];
    uint32_t mAttempt = 0;
    bool mStopping = false;
    bool mRestartPending = false;
};

SensorSubscription gSub;

void StaleTimer(System::Layer *, void *)
{
    gSub.CheckStale();
    DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(kStaleCheckMs), StaleTimer, nullptr);
}

void DeviceEventHandler(const DeviceLayer::ChipDeviceEvent * event, intptr_t)
{
    using namespace DeviceLayer;
    if (event->Type == DeviceEventType::kInterfaceIpAddressChanged) {
        if (event->InterfaceIpAddressChanged.Type == InterfaceIpChangeType::kIpV6_Assigned ||
            event->InterfaceIpAddressChanged.Type == InterfaceIpChangeType::kIpV4_Assigned) {
            gSub.OnNetworkUp();
        }
    }
}

void StartWork(intptr_t)
{
    if (!gStarted) {
        gStarted = true;
        LogErrorOnFailure(DeviceLayer::PlatformMgr().AddEventHandler(DeviceEventHandler, 0));
        DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(kStaleCheckMs), StaleTimer, nullptr);
    }
    gSub.Start();
}

void RestartWork(intptr_t)
{
    gSub.Stop();
    gSub.Start();
}

void StopWork(intptr_t)
{
    gSub.Stop();
}

void RetryWork(intptr_t)
{
    gSub.RetryNow();
}

void OnLocalSample(bool valid, int16_t centi)
{
    {
        Guard g;
        gStatus.local_valid = valid;
        if (valid) {
            gStatus.local_centi = centi;
            gStatus.local_time_us = app_time_us();
        }
    }
    Publish(valid);
}

} // namespace

extern "C" esp_err_t remote_temperature_init(const app_config_t * cfg, rt_sink_cb_t sink)
{
    gLock = xSemaphoreCreateMutexStatic(&gLockBuf);
    gPubLock = xSemaphoreCreateMutexStatic(&gPubLockBuf);
    gCfg = *cfg;
    gSink = sink;
    memset(&gStatus, 0, sizeof(gStatus));
    gStatus.stale_timeout_s = rt_stale_timeout_s(CONFIG_APP_SENSOR_MAX_INTERVAL_S, gCfg.stale_min_s);

    sensor_binding_t b;
    size_t len = sizeof(b);
    esp_err_t err = cfg_get_blob(CFG_PART_MAIN, CFG_NS_SENSOR, "binding", &b, &len);
    if (err == ESP_OK && len == sizeof(b) && b.version == kBindingVersion && IsOperationalNodeId(b.node_id)) {
        gStatus.bound = true;
        gStatus.binding = b;
        gStatus.stale = true; /* nothing received yet */
    }
    gStatus.local_configured = local_sensor_configured();
    if (gStatus.local_configured) {
        err = local_sensor_start(OnLocalSample);
        if (err != ESP_OK) {
            app_log_error("remote_temp", "local sensor start failed: %s", esp_err_to_name(err));
        }
    }
    Publish(false); /* LocalTemperature = null until a valid value exists */
    return ESP_OK;
}

extern "C" esp_err_t remote_temperature_start(void)
{
    return DeviceLayer::PlatformMgr().ScheduleWork(StartWork, 0) == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t remote_temperature_bind(uint64_t node_id, uint16_t endpoint, uint16_t vendor_id, uint16_t product_id,
                                             const char * vendor_name, const char * product_name, const char * label)
{
    if (!IsOperationalNodeId(node_id) || endpoint == 0 || endpoint == kInvalidEndpointId) {
        return ESP_ERR_INVALID_ARG;
    }
    commissioner_info_t info;
    matter_commissioner_get_info(&info);
    if (!info.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_binding_t b;
    memset(&b, 0, sizeof(b));
    b.version = kBindingVersion;
    b.endpoint = endpoint;
    b.node_id = node_id;
    b.fabric_id = info.fabric_id;
    b.compressed_fabric_id = info.compressed_fabric_id;
    b.vendor_id = vendor_id;
    b.product_id = product_id;
    strlcpy(b.vendor_name, vendor_name ? vendor_name : "", sizeof(b.vendor_name));
    strlcpy(b.product_name, product_name ? product_name : "", sizeof(b.product_name));
    strlcpy(b.label, label ? label : "", sizeof(b.label));
    b.bound_at_s = app_time_s();
    esp_err_t err = SaveBinding(b);
    if (err != ESP_OK) {
        return err;
    }
    {
        Guard g;
        gStatus.bound = true;
        gStatus.binding = b;
        gStatus.has_value = false;
        gStatus.value_null = false;
        gStatus.alive_time_us = 0;
        gStatus.stale = true;
        gStatus.retries = 0;
        gStatus.has_bounds_min = gStatus.has_bounds_max = false;
    }
    Publish(false);
    return DeviceLayer::PlatformMgr().ScheduleWork(RestartWork, 0) == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t remote_temperature_unbind(void)
{
    if (DeviceLayer::PlatformMgr().ScheduleWork(StopWork, 0) != CHIP_NO_ERROR) {
        return ESP_FAIL;
    }
    esp_err_t err = cfg_erase_key(CFG_PART_MAIN, CFG_NS_SENSOR, "binding");
    {
        Guard g;
        gStatus.bound = false;
        memset(&gStatus.binding, 0, sizeof(gStatus.binding));
        gStatus.state = RT_UNBOUND;
        gStatus.has_value = false;
        gStatus.stale = false;
        gStatus.alive_time_us = 0;
        gStatus.error[0] = 0;
    }
    Publish(false);
    return (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
}

extern "C" esp_err_t remote_temperature_retry_now(void)
{
    return DeviceLayer::PlatformMgr().ScheduleWork(RetryWork, 0) == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
}

extern "C" void remote_temperature_config_changed(const app_config_t * cfg)
{
    {
        Guard g;
        gCfg = *cfg;
        uint32_t maxI = gStatus.max_interval_s ? gStatus.max_interval_s : CONFIG_APP_SENSOR_MAX_INTERVAL_S;
        gStatus.stale_timeout_s = rt_stale_timeout_s(maxI, gCfg.stale_min_s);
    }
    Publish(false);
}

extern "C" void remote_temperature_get_status(rt_status_t * out)
{
    Guard g;
    *out = gStatus;
#if CONFIG_APP_TEST_SIMULATED_SENSOR
    out->simulated = gSimEnabled;
#endif
}

extern "C" bool remote_temperature_get_binding(sensor_binding_t * out)
{
    Guard g;
    if (!gStatus.bound) {
        return false;
    }
    *out = gStatus.binding;
    return true;
}

extern "C" esp_err_t remote_temperature_set_simulated(bool enable, int16_t centi)
{
#if CONFIG_APP_TEST_SIMULATED_SENSOR
    if (enable && !rt_value_plausible(centi, false, 0, false, 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    {
        Guard g;
        gSimEnabled = enable;
        gSimCenti = centi;
    }
    ESP_LOGW(TAG, "SIMULATED temperature %s (%d)", enable ? "ON" : "off", centi);
    Publish(true);
    return ESP_OK;
#else
    (void)enable;
    (void)centi;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

extern "C" const char * rt_state_name(rt_state_t s)
{
    switch (s) {
    case RT_UNBOUND:
        return "Chưa ghép cảm biến";
    case RT_WAITING_COMMISSIONER:
        return "Chờ commissioner";
    case RT_CONNECTING:
        return "Đang kết nối/subscribe";
    case RT_SUBSCRIBED:
        return "Đã subscribe";
    case RT_RETRYING:
        return "Đang thử lại";
    case RT_ERROR:
        return "Lỗi liên kết";
    default:
        return "?";
    }
}

extern "C" const char * temp_source_name(temp_source_t s)
{
    switch (s) {
    case TEMP_SRC_MATTER:
        return "Cảm biến Matter";
    case TEMP_SRC_LOCAL:
        return "Cảm biến cục bộ";
    case TEMP_SRC_SIMULATED:
        return "GIẢ LẬP (thử nghiệm)";
    default:
        return "Không có";
    }
}
