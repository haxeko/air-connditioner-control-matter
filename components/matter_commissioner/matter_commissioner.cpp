/*
 * Fabric B commissioner running next to the Matter server on the same CHIP stack.
 *
 * Architecture (verified against connectedhomeip b1902b39ac, the version in esp_matter 1.6.1):
 *   - chip::Server is started by esp_matter::start() and owns the FabricTable, persistent storage,
 *     session keystore, session-resumption storage and group data provider.
 *   - chip::Controller::DeviceControllerFactory is then initialised with *those* objects
 *     (FactoryInitParams::fabricTable etc.) and its own UDP port, exactly like
 *     connectedhomeip examples/platform/linux/CommissionerMain.cpp. No second FabricTable, no second
 *     storage namespace, no port clash with 5540.
 *   - The factory re-initialises the InteractionModelEngine singleton with the controller's
 *     ExchangeManager; CHIP_CONFIG_UNSAFE_SUBSCRIPTION_EXCHANGE_MANAGER_USE=1 keeps server-side
 *     subscriptions on the server's ExchangeManager (issue #23625), and subscription persistence is
 *     disabled because resumed subscriptions would not have an ExchangeManager.
 */
#include "matter_commissioner.h"

#include <string.h>

#include <string>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>
#include <app/ReadHandler.h>
#include <app/data-model/Decode.h>
#include <app/data-model/Nullable.h>
#include <app/server/Server.h>
#include <controller/AutoCommissioner.h>
#include <controller/CHIPDeviceController.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <controller/CommissioningDelegate.h>
#include <controller/CurrentFabricRemover.h>
#include <credentials/FabricTable.h>
#include <credentials/GroupDataProvider.h>
#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>
#include <credentials/attestation_verifier/DeviceAttestationDelegate.h>
#include <lib/support/CodeUtils.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <setup_payload/ManualSetupPayloadParser.h>
#include <setup_payload/QRCodeSetupPayloadParser.h>

#include "app_state.h"
#include "esp_log.h"
#include "esp_system.h"
#include "fabric_b_issuer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "paa_trust_store.h"
#include "sdkconfig.h"

static_assert(CHIP_CONFIG_UNSAFE_SUBSCRIPTION_EXCHANGE_MANAGER_USE == 1,
              "server + commissioner on one stack requires CHIP_CONFIG_UNSAFE_SUBSCRIPTION_EXCHANGE_MANAGER_USE");
#if CHIP_CONFIG_PERSIST_SUBSCRIPTIONS
#error "CONFIG_ENABLE_PERSIST_SUBSCRIPTIONS must be disabled (resumed ReadHandlers have no ExchangeManager)"
#endif

extern const uint8_t paa_bundle_bin_start[] asm("_binary_paa_bundle_bin_start");
extern const uint8_t paa_bundle_bin_end[] asm("_binary_paa_bundle_bin_end");

using namespace chip;
using namespace chip::app;
using namespace chip::Controller;

namespace {

constexpr char TAG[] = "commissioner";
constexpr char kFabricLabel[] = "ESP AC sensor link";
constexpr uint32_t kPairingWatchdogMs = 240 * 1000;

app_fabric_b::FabricBIssuer gIssuer;
app_fabric_b::PaaTrustStore gTrustStore;
DeviceCommissioner gCommissioner;
AutoCommissioner gAutoCommissioner;
Credentials::DeviceAttestationVerifier * gDacVerifier = nullptr;

SemaphoreHandle_t gLock;
StaticSemaphore_t gLockBuf;
commissioner_progress_t gProgress;
commissioner_info_t gInfo;
bool gReady = false;
NodeId gPairingNodeId = kUndefinedNodeId;
NodeId gMatchingNodeId = kUndefinedNodeId; /* sensor already carries Fabric B (paired before) */
CHIP_ERROR gPrepareError = CHIP_ERROR_INCORRECT_STATE; /* until matter_commissioner_prepare() ran */
bool gAllowUncertified = false;                        /* user choice for the current attempt */
size_t gPaaCount = 0;

/* ------------------------------------------------------------------------------------------ */

struct Locked
{
    Locked() { xSemaphoreTake(gLock, portMAX_DELAY); }
    ~Locked() { xSemaphoreGive(gLock); }
};

void SetState(commissioner_state_t state)
{
    Locked l;
    gProgress.state = state;
    gProgress.updated_us = app_time_us();
}

void SetError(commissioner_state_t state, const char * text, CHIP_ERROR err)
{
    {
        Locked l;
        gProgress.state = state;
        if (err == CHIP_NO_ERROR) {
            strlcpy(gProgress.error, text, sizeof(gProgress.error));
        } else {
            snprintf(gProgress.error, sizeof(gProgress.error), "%s (%s)", text, err.AsString());
        }
        gProgress.updated_us = app_time_us();
    }
    app_log_error("commissioner", "%s: %s", text, err == CHIP_NO_ERROR ? "-" : err.AsString());
}

void SetStage(const char * stage)
{
    Locked l;
    strlcpy(gProgress.stage, stage, sizeof(gProgress.stage));
    gProgress.updated_us = app_time_us();
}

void PairingWatchdog(System::Layer *, void *);

void FinishBusy()
{
    DeviceLayer::SystemLayer().CancelTimer(PairingWatchdog, nullptr);
    app_state_set_busy(APP_BUSY_PAIRING | APP_BUSY_SENSOR_READ, false);
}

/* ------------------------------------------------------------------------------------------ */
/* Reading BasicInformation + Temperature Measurement endpoints of a Fabric B node              */
/* ------------------------------------------------------------------------------------------ */

class InfoReader : public ReadClient::Callback
{
public:
    InfoReader() : mOnConnected(OnConnected, this), mOnFailure(OnConnectionFailure, this) {}

    CHIP_ERROR Start(NodeId nodeId)
    {
        VerifyOrReturnError(mClient == nullptr && !mConnecting, CHIP_ERROR_BUSY);
        mNodeId = nodeId;
        {
            Locked l;
            gProgress.endpoint_count = 0;
            memset(gProgress.endpoints, 0, sizeof(gProgress.endpoints));
            gProgress.vendor_name[0] = gProgress.product_name[0] = gProgress.node_label[0] = 0;
        }
        mConnecting = true;
        CHIP_ERROR err = gCommissioner.GetConnectedDevice(nodeId, &mOnConnected, &mOnFailure);
        if (err != CHIP_NO_ERROR) {
            mConnecting = false;
        }
        return err;
    }

    bool Active() const { return mConnecting || mClient != nullptr; }

    /* CHIP thread: abandon a pending CASE connection or read (no callback fires afterwards). */
    void Cancel()
    {
        if (mConnecting) {
            mOnConnected.Cancel();
            mOnFailure.Cancel();
            mConnecting = false;
        }
        if (mClient != nullptr) {
            Platform::Delete(mClient); /* the destructor aborts the exchange; OnDone is not called */
            mClient = nullptr;
        }
        mError = CHIP_NO_ERROR;
    }

private:
    static void OnConnected(void * ctx, Messaging::ExchangeManager & exchangeMgr, const SessionHandle & session)
    {
        auto * self = static_cast<InfoReader *>(ctx);
        self->mConnecting = false;
        self->SendRead(exchangeMgr, session);
    }

    static void OnConnectionFailure(void * ctx, const ScopedNodeId & peer, CHIP_ERROR err)
    {
        auto * self = static_cast<InfoReader *>(ctx);
        self->mConnecting = false;
        SetError(COMMISSIONER_FAILED, "Không kết nối được (CASE) tới cảm biến để đọc endpoint", err);
        FinishBusy();
    }

    void SendRead(Messaging::ExchangeManager & exchangeMgr, const SessionHandle & session)
    {
        namespace BI = Clusters::BasicInformation::Attributes;
        namespace TM = Clusters::TemperatureMeasurement::Attributes;
        mPaths[0] = AttributePathParams(0, Clusters::BasicInformation::Id, BI::VendorName::Id);
        mPaths[1] = AttributePathParams(0, Clusters::BasicInformation::Id, BI::VendorID::Id);
        mPaths[2] = AttributePathParams(0, Clusters::BasicInformation::Id, BI::ProductName::Id);
        mPaths[3] = AttributePathParams(0, Clusters::BasicInformation::Id, BI::ProductID::Id);
        mPaths[4] = AttributePathParams(0, Clusters::BasicInformation::Id, BI::NodeLabel::Id);
        /* Wildcard endpoint, concrete cluster: the publisher expands to every endpoint that
         * actually implements Temperature Measurement (0x0402). No endpoint is hard-coded. */
        mPaths[5] = AttributePathParams(Clusters::TemperatureMeasurement::Id, TM::MeasuredValue::Id);
        mPaths[6] = AttributePathParams(Clusters::TemperatureMeasurement::Id, TM::MinMeasuredValue::Id);
        mPaths[7] = AttributePathParams(Clusters::TemperatureMeasurement::Id, TM::MaxMeasuredValue::Id);

        ReadPrepareParams params(session);
        params.mpAttributePathParamsList = mPaths;
        params.mAttributePathParamsListSize = sizeof(mPaths) / sizeof(mPaths[0]);
        params.mIsFabricFiltered = true;

        mClient = Platform::New<ReadClient>(InteractionModelEngine::GetInstance(), &exchangeMgr, *this,
                                            ReadClient::InteractionType::Read);
        if (mClient == nullptr) {
            SetError(COMMISSIONER_FAILED, "Hết bộ nhớ khi đọc cảm biến", CHIP_ERROR_NO_MEMORY);
            FinishBusy();
            return;
        }
        CHIP_ERROR err = mClient->SendRequest(params);
        if (err != CHIP_NO_ERROR) {
            Platform::Delete(mClient);
            mClient = nullptr;
            SetError(COMMISSIONER_FAILED, "Gửi yêu cầu đọc thất bại", err);
            FinishBusy();
        }
    }

    static commissioner_endpoint_t * EndpointSlot(EndpointId ep)
    {
        for (uint8_t i = 0; i < gProgress.endpoint_count; i++) {
            if (gProgress.endpoints[i].endpoint == ep) {
                return &gProgress.endpoints[i];
            }
        }
        if (gProgress.endpoint_count >= COMMISSIONER_MAX_ENDPOINTS) {
            return nullptr;
        }
        commissioner_endpoint_t * slot = &gProgress.endpoints[gProgress.endpoint_count++];
        memset(slot, 0, sizeof(*slot));
        slot->endpoint = ep;
        return slot;
    }

    static void CopyString(TLV::TLVReader * data, char * out, size_t outLen)
    {
        CharSpan s;
        if (DataModel::Decode(*data, s) == CHIP_NO_ERROR) {
            size_t n = s.size() < outLen - 1 ? s.size() : outLen - 1;
            memcpy(out, s.data(), n);
            out[n] = 0;
        }
    }

    void OnAttributeData(const ConcreteDataAttributePath & path, TLV::TLVReader * data, const StatusIB & status) override
    {
        namespace BI = Clusters::BasicInformation::Attributes;
        namespace TM = Clusters::TemperatureMeasurement::Attributes;
        Locked l;
        if (path.mClusterId == Clusters::TemperatureMeasurement::Id) {
            commissioner_endpoint_t * ep = EndpointSlot(path.mEndpointId);
            if (ep == nullptr) {
                return;
            }
            if (!status.IsSuccess() || data == nullptr) {
                return;
            }
            DataModel::Nullable<int16_t> v;
            if (DataModel::Decode(*data, v) != CHIP_NO_ERROR) {
                return;
            }
            switch (path.mAttributeId) {
            case TM::MeasuredValue::Id:
                ep->read_ok = true;
                ep->value_null = v.IsNull();
                ep->measured = v.IsNull() ? 0 : v.Value();
                break;
            case TM::MinMeasuredValue::Id:
                ep->has_min = !v.IsNull();
                ep->min_measured = v.IsNull() ? 0 : v.Value();
                break;
            case TM::MaxMeasuredValue::Id:
                ep->has_max = !v.IsNull();
                ep->max_measured = v.IsNull() ? 0 : v.Value();
                break;
            default:
                break;
            }
            return;
        }
        if (path.mClusterId != Clusters::BasicInformation::Id || !status.IsSuccess() || data == nullptr) {
            return;
        }
        switch (path.mAttributeId) {
        case BI::VendorName::Id:
            CopyString(data, gProgress.vendor_name, sizeof(gProgress.vendor_name));
            break;
        case BI::ProductName::Id:
            CopyString(data, gProgress.product_name, sizeof(gProgress.product_name));
            break;
        case BI::NodeLabel::Id:
            CopyString(data, gProgress.node_label, sizeof(gProgress.node_label));
            break;
        case BI::VendorID::Id: {
            uint16_t vid;
            if (DataModel::Decode(*data, vid) == CHIP_NO_ERROR) {
                gProgress.vendor_id = vid;
            }
            break;
        }
        case BI::ProductID::Id: {
            uint16_t pid;
            if (DataModel::Decode(*data, pid) == CHIP_NO_ERROR) {
                gProgress.product_id = pid;
            }
            break;
        }
        default:
            break;
        }
    }

    void OnError(CHIP_ERROR err) override { mError = err; }

    void OnDone(ReadClient * client) override
    {
        Platform::Delete(client);
        mClient = nullptr;
        uint8_t count;
        {
            Locked l;
            count = gProgress.endpoint_count;
        }
        if (mError != CHIP_NO_ERROR) {
            SetError(COMMISSIONER_FAILED, "Đọc thông tin cảm biến thất bại", mError);
        } else if (count == 0) {
            SetError(COMMISSIONER_FAILED, "Thiết bị không có endpoint Temperature Measurement (0x0402)", CHIP_NO_ERROR);
        } else {
            SetState(COMMISSIONER_DONE);
            ESP_LOGI(TAG, "node 0x%016llX: %u temperature endpoint(s)", (unsigned long long)mNodeId, count);
        }
        mError = CHIP_NO_ERROR;
        FinishBusy();
    }

    chip::Callback::Callback<chip::OnDeviceConnected> mOnConnected;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> mOnFailure;
    ReadClient * mClient = nullptr;
    bool mConnecting = false;
    NodeId mNodeId = kUndefinedNodeId;
    CHIP_ERROR mError = CHIP_NO_ERROR;
    AttributePathParams mPaths[8];
};

InfoReader gInfoReader;

/* ------------------------------------------------------------------------------------------ */
/* Attestation: strict by default, user may explicitly accept an uncertified/test device        */
/* ------------------------------------------------------------------------------------------ */

/*
 * Failures the user may accept for a DIY/test or not-yet-listed device: the trust anchor (PAA) or the
 * certification declaration signer is unknown to the offline store. The device has already proven
 * possession of its DAC key at this point (DefaultDACVerifier checks the attestation signature
 * before looking up the PAA). Forged/broken signatures, revocation, nonce or VID/PID mismatches
 * are never accepted.
 */
bool OverridableAttestationFailure(Credentials::AttestationVerificationResult r)
{
    using R = Credentials::AttestationVerificationResult;
    switch (r) {
    case R::kPaaNotFound:
    case R::kPaaExpired:
    case R::kPaiExpired:
    case R::kDacExpired:
    case R::kCertificationDeclarationNoKeyId:
    case R::kCertificationDeclarationNoCertificateFound:
    case R::kCertificationDeclarationInvalidSignature:
    case R::kCertificationDeclarationInvalidVendorId:
    case R::kCertificationDeclarationInvalidProductId:
    case R::kCertificationDeclarationInvalidPAA:
        return true;
    default:
        return false;
    }
}

class AttestationDelegate : public Credentials::DeviceAttestationDelegate
{
public:
    Optional<uint16_t> FailSafeExpiryTimeoutSecs() const override { return MakeOptional(static_cast<uint16_t>(120)); }

    void OnDeviceAttestationCompleted(DeviceCommissioner * commissioner, DeviceProxy * device,
                                      const Credentials::DeviceAttestationVerifier::AttestationDeviceInfo & info,
                                      Credentials::AttestationVerificationResult result) override
    {
        if (result == Credentials::AttestationVerificationResult::kSuccess) {
            LogErrorOnFailure(commissioner->ContinueCommissioningAfterDeviceAttestation(device, result));
            return;
        }
        /* Only installed when the user ticked "accept uncertified device" for this attempt. */
        bool accept = OverridableAttestationFailure(result);
        ESP_LOGW(TAG, "attestation failed (%u): %s", static_cast<unsigned>(result),
                 accept ? "accepted by explicit user choice" : "not overridable, rejected");
        {
            Locked l;
            gProgress.attestation_overridden = accept;
            snprintf(gProgress.attestation_note, sizeof(gProgress.attestation_note), "%s: %s",
                     accept ? "Chấp nhận" : "Từ chối", Credentials::GetAttestationResultDescription(result));
        }
        LogErrorOnFailure(commissioner->ContinueCommissioningAfterDeviceAttestation(
            device, accept ? Credentials::AttestationVerificationResult::kSuccess : result));
    }
};

AttestationDelegate gAttestationDelegate;

/* ------------------------------------------------------------------------------------------ */
/* Pairing delegate                                                                             */
/* ------------------------------------------------------------------------------------------ */

class PairingDelegate : public DevicePairingDelegate
{
public:
    void OnStatusUpdate(DevicePairingDelegate::Status status) override
    {
        if (status == DevicePairingDelegate::Status::SecurePairingSuccess) {
            SetState(COMMISSIONER_COMMISSIONING);
        }
    }

    void OnPairingComplete(CHIP_ERROR error) override
    {
        if (error == CHIP_NO_ERROR) {
            SetState(COMMISSIONER_COMMISSIONING);
            SetStage("PASE established");
            return;
        }
        if (error == CHIP_ERROR_TIMEOUT) {
            SetError(COMMISSIONER_FAILED,
                     "Không tìm thấy hoặc không bắt tay được với cảm biến trên mạng. Kiểm tra cửa sổ chia sẻ còn mở và mã đúng",
                     error);
        } else {
            SetError(COMMISSIONER_FAILED, "PASE thất bại (mã chia sẻ sai hoặc cửa sổ ghép đã đóng)", error);
        }
        gPairingNodeId = kUndefinedNodeId;
        FinishBusy();
    }

    void OnCommissioningStatusUpdate(PeerId peerId, CommissioningStage stageCompleted, CHIP_ERROR error) override
    {
        SetStage(StageToString(stageCompleted));
        if (error != CHIP_NO_ERROR) {
            ESP_LOGW(TAG, "stage %s failed: %" CHIP_ERROR_FORMAT, StageToString(stageCompleted), error.Format());
        }
    }

    void OnCommissioningFailure(PeerId peerId, CHIP_ERROR error, CommissioningStage stageFailed,
                                Optional<Credentials::AttestationVerificationResult> attestation) override
    {
        (void)peerId;
        (void)error;
        (void)stageFailed;
        if (!attestation.HasValue() || attestation.Value() == Credentials::AttestationVerificationResult::kSuccess) {
            return;
        }
        Credentials::AttestationVerificationResult r = attestation.Value();
        Locked l;
        snprintf(gProgress.error, sizeof(gProgress.error), "Xác thực thiết bị thất bại: %s",
                 Credentials::GetAttestationResultDescription(r));
        if (OverridableAttestationFailure(r) && !gAllowUncertified) {
            strlcpy(gProgress.attestation_note,
                    "Cảm biến thử nghiệm/DIY: bật ô 'chưa được chứng nhận' rồi ghép lại",
                    sizeof(gProgress.attestation_note));
        }
    }

    void OnFabricCheck(NodeId matchingNodeId) override
    {
        if (matchingNodeId == kUndefinedNodeId) {
            return;
        }
        /* The sensor already has a NOC of Fabric B (e.g. the binding was only forgotten on this
         * device). A second AddNOC would fail with FabricConflict: stop right after reading the
         * commissioning info (before the fail-safe is armed) and reuse the existing node. */
        ESP_LOGI(TAG, "sensor already on fabric B as node 0x%016llX, reusing it", (unsigned long long)matchingNodeId);
        gMatchingNodeId = matchingNodeId;
        {
            Locked l;
            gProgress.already_on_fabric = true;
            gProgress.node_id = matchingNodeId;
        }
        gAutoCommissioner.StopCommissioning();
    }

    void OnCommissioningComplete(NodeId deviceId, CHIP_ERROR error) override
    {
        gPairingNodeId = kUndefinedNodeId;
        if (gMatchingNodeId != kUndefinedNodeId) {
            deviceId = gMatchingNodeId; /* stopped on purpose, see OnFabricCheck() */
            gMatchingNodeId = kUndefinedNodeId;
            error = CHIP_NO_ERROR;
        }
        if (error != CHIP_NO_ERROR) {
            if (error == CHIP_ERROR_FAILED_DEVICE_ATTESTATION) {
                /* refined with the attestation result in OnCommissioningFailure(), called next */
                SetError(COMMISSIONER_FAILED, "Xác thực thiết bị (Device Attestation) thất bại", error);
            } else {
                SetError(COMMISSIONER_FAILED, "Commissioning thất bại", error);
            }
            FinishBusy();
            return;
        }
        ESP_LOGI(TAG, "sensor commissioned into fabric B as node 0x%016llX", (unsigned long long)deviceId);
        {
            Locked l;
            gProgress.node_id = deviceId;
            gProgress.state = COMMISSIONER_READING;
            gProgress.updated_us = app_time_us();
        }
        app_state_set_busy(APP_BUSY_SENSOR_READ, true);
        CHIP_ERROR err = gInfoReader.Start(deviceId);
        if (err != CHIP_NO_ERROR) {
            SetError(COMMISSIONER_FAILED, "Không bắt đầu đọc endpoint được", err);
            FinishBusy();
        } else {
            app_state_set_busy(APP_BUSY_PAIRING, false);
        }
    }
};

PairingDelegate gPairingDelegate;

void PairingWatchdog(System::Layer *, void *)
{
    if (gPairingNodeId != kUndefinedNodeId) {
        ESP_LOGW(TAG, "pairing watchdog expired, stopping");
        (void)gCommissioner.StopPairing(gPairingNodeId);
        gPairingNodeId = kUndefinedNodeId;
        SetError(COMMISSIONER_FAILED, "Hết thời gian ghép cảm biến", CHIP_ERROR_TIMEOUT);
    }
    FinishBusy();
}

/* ------------------------------------------------------------------------------------------ */
/* Removing Fabric B from a sensor                                                              */
/* ------------------------------------------------------------------------------------------ */

CurrentFabricRemover * gRemover = nullptr;
commissioner_removed_cb_t gRemovedCb = nullptr;

void OnRemoveDone(void * context, NodeId remoteNodeId, CHIP_ERROR status)
{
    (void)context;
    if (gRemover != nullptr) {
        Platform::Delete(gRemover);
        gRemover = nullptr;
    }
    if (status == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "fabric B removed from node 0x%016llX", (unsigned long long)remoteNodeId);
        Locked l;
        gProgress.state = COMMISSIONER_IDLE;
        gProgress.error[0] = 0;
        snprintf(gProgress.stage, sizeof(gProgress.stage), "RemoveFabric OK");
        gProgress.updated_us = app_time_us();
    } else {
        SetError(COMMISSIONER_FAILED, "Gửi RemoveFabric tới cảm biến thất bại (cảm biến không phản hồi?)", status);
    }
    if (gRemovedCb != nullptr) {
        gRemovedCb(remoteNodeId, status == CHIP_NO_ERROR);
    }
}

chip::Callback::Callback<OnCurrentFabricRemove> gRemoveCallback(OnRemoveDone, nullptr);

/* ------------------------------------------------------------------------------------------ */
/* Fabric B removed from *our* FabricTable by an administrator of another fabric                */
/* ------------------------------------------------------------------------------------------ */

class FabricWatcher : public FabricTable::Delegate
{
public:
    void OnFabricRemoved(const FabricTable & table, FabricIndex index) override
    {
        if (!gReady || index != gInfo.fabric_index) {
            return;
        }
        gReady = false;
        {
            Locked l;
            gInfo.ready = false;
            strlcpy(gInfo.init_error, "Fabric B đã bị xóa khỏi thiết bị; khởi động lại để khôi phục", sizeof(gInfo.init_error));
        }
        app_log_error("commissioner", "fabric B was removed from the fabric table, restarting in 10 s");
        DeviceLayer::SystemLayer().StartTimer(System::Clock::Seconds32(10), [](System::Layer *, void *) { esp_restart(); },
                                              nullptr);
    }
};

FabricWatcher gFabricWatcher;

/* ------------------------------------------------------------------------------------------ */

bool ParseCode(const char * code, SetupPayload & payload)
{
    if (code == nullptr) {
        return false;
    }
    std::string s;
    for (const char * p = code; *p; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            s.push_back(*p);
        }
    }
    if (s.size() > 3 && (s.compare(0, 3, "MT:") == 0 || s.compare(0, 3, "mt:") == 0)) {
        s[0] = 'M';
        s[1] = 'T';
        return QRCodeSetupPayloadParser(s).populatePayload(payload) == CHIP_NO_ERROR && payload.isValidQRCodePayload();
    }
    for (char c : s) {
        if ((c < '0' || c > '9') && c != '-') {
            return false;
        }
    }
    return ManualSetupPayloadParser(s).populatePayload(payload) == CHIP_NO_ERROR && payload.isValidManualCode();
}

CHIP_ERROR InitImpl(CommonCaseDeviceServerInitParams * sp)
{
    VerifyOrReturnError(sp != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(sp->persistentStorageDelegate && sp->sessionKeystore && sp->groupDataProvider &&
                            sp->dataModelProvider,
                        CHIP_ERROR_INCORRECT_STATE);

    /* SetDataModelProvider() inside the factory VerifyOrDie()s on active read handlers. */
    VerifyOrReturnError(InteractionModelEngine::GetInstance()->GetNumActiveReadHandlers() == 0, CHIP_ERROR_INCORRECT_STATE);

    /* Fabric B identity and PAA store were prepared before the stack started. */
    ReturnErrorOnFailure(gPrepareError);
    size_t paaCount = gPaaCount;

    FabricTable & fabricTable = Server::GetInstance().GetFabricTable();

    FactoryInitParams factoryParams;
    factoryParams.listenPort = CONFIG_APP_COMMISSIONER_UDP_PORT;
    factoryParams.fabricIndependentStorage = sp->persistentStorageDelegate;
    factoryParams.fabricTable = &fabricTable;
    factoryParams.operationalKeystore = sp->operationalKeystore;
    factoryParams.opCertStore = sp->opCertStore;
    factoryParams.sessionKeystore = sp->sessionKeystore;
    factoryParams.sessionResumptionStorage = sp->sessionResumptionStorage;
    factoryParams.groupDataProvider = sp->groupDataProvider;
    factoryParams.certificateValidityPolicy = sp->certificateValidityPolicy;
    factoryParams.dataModelProvider = sp->dataModelProvider;
    factoryParams.enableServerInteractions = false;
    factoryParams.preventDnssdPortOverwrite = true;
    ReturnErrorOnFailure(DeviceControllerFactory::GetInstance().Init(factoryParams));

    gDacVerifier = Credentials::GetDefaultDACVerifier(&gTrustStore, nullptr);
    VerifyOrReturnError(gDacVerifier != nullptr, CHIP_ERROR_NO_MEMORY);
    gDacVerifier->EnableCdTestKeySupport(false);

    uint16_t vendorId = 0;
    ReturnErrorOnFailure(DeviceLayer::GetDeviceInstanceInfoProvider()->GetVendorId(vendorId));

    SetupParams params;
    params.operationalCredentialsDelegate = &gIssuer;
    params.controllerVendorId = static_cast<VendorId>(vendorId);
    params.defaultCommissioner = &gAutoCommissioner;
    params.deviceAttestationVerifier = gDacVerifier;
    params.permitMultiControllerFabrics = false;
    params.enableServerInteractions = false;
    params.removeFromFabricTableOnShutdown = false;
    params.deleteFromFabricTableOnShutdown = false;

    const FabricInfo * existing = fabricTable.FindFabric(gIssuer.GetRootPublicKey(), gIssuer.GetFabricId());
    Crypto::P256Keypair ephemeralKey;
    uint8_t nocBuf[app_fabric_b::FabricBIssuer::kMaxDerCertLength];
    MutableByteSpan nocSpan(nocBuf);
    bool createdFabric = false;
    if (existing != nullptr && existing->GetNodeId() == gIssuer.GetControllerNodeId() &&
        fabricTable.HasOperationalKeyForFabric(existing->GetFabricIndex())) {
        /* Normal boot: reuse the persisted fabric, nothing is regenerated or rewritten. */
        params.fabricIndex.SetValue(existing->GetFabricIndex());
    } else {
        if (existing != nullptr) {
            ESP_LOGW(TAG, "fabric B entry inconsistent, re-provisioning the controller NOC");
        }
        ReturnErrorOnFailure(ephemeralKey.Initialize(Crypto::ECPKeyTarget::ECDSA));
        ReturnErrorOnFailure(gIssuer.IssueControllerNoc(ephemeralKey.Pubkey(), nocSpan));
        params.operationalKeypair = &ephemeralKey;
        params.hasExternallyOwnedOperationalKeypair = false;
        params.controllerRCAC = gIssuer.GetRcac();
        params.controllerICAC = ByteSpan();
        params.controllerNOC = nocSpan;
        createdFabric = true;
    }

    ReturnErrorOnFailure(DeviceControllerFactory::GetInstance().SetupCommissioner(params, gCommissioner));

    FabricIndex fabricIndex = gCommissioner.GetFabricIndex();
    VerifyOrReturnError(fabricIndex != kUndefinedFabricIndex, CHIP_ERROR_INTERNAL);

    uint8_t cfid[sizeof(uint64_t)] = { 0 };
    MutableByteSpan cfidSpan(cfid);
    ReturnErrorOnFailure(gCommissioner.GetCompressedFabricIdBytes(cfidSpan));
    Credentials::GroupDataProvider::KeySet ipkSet;
    if (createdFabric || sp->groupDataProvider->GetIpkKeySet(fabricIndex, ipkSet) != CHIP_NO_ERROR) {
        ReturnErrorOnFailure(Credentials::SetSingleIpkEpochKey(sp->groupDataProvider, fabricIndex, gIssuer.GetIpk(), cfidSpan));
    }
    if (createdFabric) {
        LogErrorOnFailure(fabricTable.SetFabricLabel(fabricIndex, CharSpan::fromCharString(kFabricLabel)));
    }
    ReturnErrorOnFailure(fabricTable.AddFabricDelegate(&gFabricWatcher));
    gCommissioner.RegisterPairingDelegate(&gPairingDelegate);

    {
        Locked l;
        gInfo.ready = true;
        gInfo.init_error[0] = 0;
        gInfo.fabric_index = fabricIndex;
        gInfo.fabric_id = gCommissioner.GetFabricId();
        gInfo.compressed_fabric_id = gCommissioner.GetCompressedFabricId();
        gInfo.controller_node_id = gCommissioner.GetNodeId();
        gInfo.paa_count = static_cast<uint16_t>(paaCount);
        gInfo.listen_port = CONFIG_APP_COMMISSIONER_UDP_PORT;
    }
    gReady = true;
    ESP_LOGI(TAG, "commissioner ready: fabric index %u, fabric 0x%016llX, node 0x%016llX (%s)", fabricIndex,
             (unsigned long long)gInfo.fabric_id, (unsigned long long)gInfo.controller_node_id,
             createdFabric ? "created" : "restored");
    return CHIP_NO_ERROR;
}

/* ------------------------------------------------------------------------------------------ */

struct PairRequest
{
    char code[128];
    bool allowUncertified;
};

void DoStartPairing(intptr_t arg)
{
    auto * req = reinterpret_cast<PairRequest *>(arg);
    CHIP_ERROR err = CHIP_NO_ERROR;
    NodeId nodeId = kUndefinedNodeId;

    if (!gReady || gPairingNodeId != kUndefinedNodeId || gInfoReader.Active() || gRemover != nullptr) {
        SetError(COMMISSIONER_FAILED, "Commissioner chưa sẵn sàng hoặc đang bận", CHIP_ERROR_INCORRECT_STATE);
        FinishBusy();
        goto exit;
    }
    gMatchingNodeId = kUndefinedNodeId;
    err = gIssuer.AllocateDeviceNodeId(nodeId);
    if (err != CHIP_NO_ERROR) {
        SetError(COMMISSIONER_FAILED, "Không cấp được Node ID", err);
        FinishBusy();
        goto exit;
    }
    {
        gAllowUncertified = req->allowUncertified;
        gTrustStore.SetAllowTestPaa(req->allowUncertified);
        gDacVerifier->EnableCdTestKeySupport(req->allowUncertified);
        CommissioningParameters cp;
        cp.SetCheckForMatchingFabric(true);
        cp.SetDeviceAttestationDelegate(req->allowUncertified ? &gAttestationDelegate : nullptr);
        err = gCommissioner.PairDevice(nodeId, req->code, cp, DiscoveryType::kDiscoveryNetworkOnly);
    }
    if (err != CHIP_NO_ERROR) {
        SetError(COMMISSIONER_FAILED, "Không bắt đầu được commissioning", err);
        FinishBusy();
        goto exit;
    }
    gPairingNodeId = nodeId;
    {
        Locked l;
        gProgress.node_id = nodeId;
    }
    DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(kPairingWatchdogMs), PairingWatchdog, nullptr);
    ESP_LOGI(TAG, "on-network commissioning started (node 0x%016llX)", (unsigned long long)nodeId);

exit:
    memset(req->code, 0, sizeof(req->code)); /* setup code must not linger in RAM */
    Platform::Delete(req);
}

void DoCancel(intptr_t)
{
    if (gRemover != nullptr) {
        return; /* RemoveFabric cannot be interrupted half-way; its result is reported normally */
    }
    if (gPairingNodeId != kUndefinedNodeId) {
        (void)gCommissioner.StopPairing(gPairingNodeId);
        gPairingNodeId = kUndefinedNodeId;
    }
    gInfoReader.Cancel();
    SetError(COMMISSIONER_IDLE, "Đã hủy", CHIP_NO_ERROR);
    FinishBusy();
}

void DoRefresh(intptr_t arg)
{
    NodeId nodeId = static_cast<NodeId>(*reinterpret_cast<uint64_t *>(arg));
    Platform::Delete(reinterpret_cast<uint64_t *>(arg));
    if (!gReady) {
        SetError(COMMISSIONER_FAILED, "Commissioner chưa sẵn sàng", CHIP_ERROR_INCORRECT_STATE);
        FinishBusy();
        return;
    }
    {
        Locked l;
        gProgress.node_id = nodeId;
        gProgress.state = COMMISSIONER_READING;
        gProgress.error[0] = 0;
    }
    CHIP_ERROR err = gInfoReader.Start(nodeId);
    if (err != CHIP_NO_ERROR) {
        SetError(COMMISSIONER_FAILED, "Không đọc được cảm biến", err);
        FinishBusy();
    }
}

void DoRemove(intptr_t arg)
{
    NodeId nodeId = static_cast<NodeId>(*reinterpret_cast<uint64_t *>(arg));
    Platform::Delete(reinterpret_cast<uint64_t *>(arg));
    if (!gReady || gRemover != nullptr) {
        SetError(COMMISSIONER_FAILED, "Commissioner chưa sẵn sàng hoặc đang bận", CHIP_ERROR_INCORRECT_STATE);
        return;
    }
    gRemover = Platform::New<CurrentFabricRemover>(&gCommissioner);
    if (gRemover == nullptr) {
        SetError(COMMISSIONER_FAILED, "Hết bộ nhớ", CHIP_ERROR_NO_MEMORY);
        return;
    }
    SetState(COMMISSIONER_REMOVING);
    CHIP_ERROR err = gRemover->RemoveCurrentFabric(nodeId, &gRemoveCallback);
    if (err != CHIP_NO_ERROR) {
        Platform::Delete(gRemover);
        gRemover = nullptr;
        SetError(COMMISSIONER_FAILED, "Không gửi được RemoveFabric", err);
    }
}

void ResetProgress(commissioner_state_t state)
{
    Locked l;
    memset(&gProgress, 0, sizeof(gProgress));
    gProgress.state = state;
    gProgress.started_us = gProgress.updated_us = app_time_us();
}

} // namespace

/* ------------------------------------------------------------------------------------------ */
/* C API                                                                                         */
/* ------------------------------------------------------------------------------------------ */

extern "C" esp_err_t matter_commissioner_prepare(void)
{
    if (gLock == nullptr) {
        gLock = xSemaphoreCreateMutexStatic(&gLockBuf);
    }
    gPaaCount = gTrustStore.Init(paa_bundle_bin_start, static_cast<size_t>(paa_bundle_bin_end - paa_bundle_bin_start));
    gPrepareError = gIssuer.LoadOrCreate();
    if (gPrepareError != CHIP_NO_ERROR) {
        {
            Locked l;
            snprintf(gInfo.init_error, sizeof(gInfo.init_error), "Không tạo/đọc được Fabric B: %s", gPrepareError.AsString());
        }
        app_log_error("commissioner", "fabric B identity: %s", gPrepareError.AsString());
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" esp_err_t matter_commissioner_init_on_chip_thread(void * server_init_params)
{
    if (gLock == nullptr) {
        gLock = xSemaphoreCreateMutexStatic(&gLockBuf);
    }
    CHIP_ERROR err = InitImpl(static_cast<CommonCaseDeviceServerInitParams *>(server_init_params));
    if (err != CHIP_NO_ERROR) {
        {
            Locked l;
            gInfo.ready = false;
            snprintf(gInfo.init_error, sizeof(gInfo.init_error), "Khởi tạo commissioner lỗi: %s", err.AsString());
        }
        app_log_error("commissioner", "init failed: %s", err.AsString());
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" void matter_commissioner_get_info(commissioner_info_t * out)
{
    if (gLock == nullptr) {
        memset(out, 0, sizeof(*out));
        strlcpy(out->init_error, "Chưa khởi tạo", sizeof(out->init_error));
        return;
    }
    Locked l;
    *out = gInfo;
}

extern "C" void matter_commissioner_get_progress(commissioner_progress_t * out)
{
    if (gLock == nullptr) {
        memset(out, 0, sizeof(*out));
        return;
    }
    Locked l;
    *out = gProgress;
}

extern "C" bool matter_commissioner_busy(void)
{
    commissioner_progress_t p;
    matter_commissioner_get_progress(&p);
    return p.state == COMMISSIONER_DISCOVERING || p.state == COMMISSIONER_PASE || p.state == COMMISSIONER_COMMISSIONING ||
        p.state == COMMISSIONER_READING || p.state == COMMISSIONER_REMOVING;
}

extern "C" bool matter_commissioner_validate_code(const char * setup_code)
{
    SetupPayload payload;
    return ParseCode(setup_code, payload);
}

extern "C" esp_err_t matter_commissioner_start_pairing(const char * setup_code, bool allow_uncertified)
{
    if (gLock == nullptr || !gReady) {
        return ESP_ERR_INVALID_STATE;
    }
    if (matter_commissioner_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!matter_commissioner_validate_code(setup_code)) {
        return ESP_ERR_INVALID_ARG;
    }
    auto * req = Platform::New<PairRequest>();
    if (req == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    /* Normalise: blanks are removed from both forms; '-' only from the 11/21 digit manual code,
     * because '-' is a symbol of the Base38 alphabet of "MT:" QR payloads. */
    const char * s = setup_code;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    const bool isQr = (s[0] == 'M' || s[0] == 'm') && (s[1] == 'T' || s[1] == 't') && s[2] == ':';
    size_t n = 0;
    for (const char * c = s; *c && n < sizeof(req->code) - 1; c++) {
        if (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n' || (!isQr && *c == '-')) {
            continue;
        }
        req->code[n++] = *c;
    }
    req->code[n] = 0;
    if (isQr) {
        req->code[0] = 'M';
        req->code[1] = 'T';
    }
    req->allowUncertified = allow_uncertified;

    ResetProgress(COMMISSIONER_DISCOVERING);
    app_state_set_busy(APP_BUSY_PAIRING, true);
    if (DeviceLayer::PlatformMgr().ScheduleWork(DoStartPairing, reinterpret_cast<intptr_t>(req)) != CHIP_NO_ERROR) {
        memset(req->code, 0, sizeof(req->code));
        Platform::Delete(req);
        app_state_set_busy(APP_BUSY_PAIRING, false);
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" esp_err_t matter_commissioner_cancel(void)
{
    if (gLock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    commissioner_progress_t p;
    matter_commissioner_get_progress(&p);
    if (p.state == COMMISSIONER_REMOVING) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return DeviceLayer::PlatformMgr().ScheduleWork(DoCancel, 0) == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
}

static esp_err_t ScheduleWithNode(void (*fn)(intptr_t), uint64_t node_id)
{
    auto * arg = Platform::New<uint64_t>(node_id);
    if (arg == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (DeviceLayer::PlatformMgr().ScheduleWork(fn, reinterpret_cast<intptr_t>(arg)) != CHIP_NO_ERROR) {
        Platform::Delete(arg);
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" esp_err_t matter_commissioner_refresh(uint64_t node_id)
{
    if (gLock == nullptr || !gReady || !IsOperationalNodeId(node_id)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (matter_commissioner_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    ResetProgress(COMMISSIONER_READING);
    app_state_set_busy(APP_BUSY_SENSOR_READ, true);
    esp_err_t err = ScheduleWithNode(DoRefresh, node_id);
    if (err != ESP_OK) {
        app_state_set_busy(APP_BUSY_SENSOR_READ, false);
    }
    return err;
}

extern "C" esp_err_t matter_commissioner_remove_node(uint64_t node_id)
{
    if (gLock == nullptr || !gReady || !IsOperationalNodeId(node_id)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (matter_commissioner_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    ResetProgress(COMMISSIONER_REMOVING);
    return ScheduleWithNode(DoRemove, node_id);
}

extern "C" void matter_commissioner_set_removed_cb(commissioner_removed_cb_t cb)
{
    gRemovedCb = cb;
}
