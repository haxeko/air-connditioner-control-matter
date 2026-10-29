#include "fabric_b_issuer.h"

#include <string.h>

#include <credentials/CHIPCert.h>
#include <lib/asn1/ASN1.h>
#include <lib/core/CHIPEncoding.h>
#include <lib/core/TLV.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/ScopedMemoryBuffer.h>
#include <lib/support/logging/CHIPLogging.h>

#include "config_store.h"
#include "esp_log.h"

using namespace chip;
using namespace chip::Credentials;
using namespace chip::Crypto;

namespace app_fabric_b {

namespace {

constexpr char TAG[] = "fabric_b";
constexpr uint32_t kFormatVersion = 1;

constexpr char kKeyVersion[] = "ver";
constexpr char kKeyRootKey[] = "root_key";
constexpr char kKeyRcac[] = "rcac";
constexpr char kKeyRcacId[] = "rcac_id";
constexpr char kKeyFabricId[] = "fabric_id";
constexpr char kKeyCtrlNode[] = "ctrl_node";
constexpr char kKeyIpk[] = "ipk";

CHIP_ERROR FromEsp(esp_err_t err)
{
    if (err == ESP_OK) {
        return CHIP_NO_ERROR;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
    }
    return CHIP_ERROR_PERSISTED_STORAGE_FAILED;
}

CHIP_ERROR RandomU64(uint64_t & out)
{
    return DRBG_get_bytes(reinterpret_cast<uint8_t *>(&out), sizeof(out));
}

/* Certificates start being valid on 2024-01-01; "not before" is not enforced by nodes that only
 * know a Last Known Good Time, and nodes with real time are past that date. */
CHIP_ERROR ValidityStart(uint32_t & out)
{
    ASN1::ASN1UniversalTime t;
    memset(&t, 0, sizeof(t));
    t.Year = 2024;
    t.Month = 1;
    t.Day = 1;
    return ASN1ToChipEpochTime(t, out);
}

int64_t RandomSerial()
{
    uint64_t v = 1;
    if (DRBG_get_bytes(reinterpret_cast<uint8_t *>(&v), sizeof(v)) != CHIP_NO_ERROR) {
        return 1;
    }
    v &= 0x7FFFFFFFFFFFFFFFULL; /* positive INTEGER */
    return v == 0 ? 1 : static_cast<int64_t>(v);
}

} // namespace

CHIP_ERROR FabricBIssuer::EraseStored()
{
    return FromEsp(cfg_erase_namespace(CFG_PART_MAIN, CFG_NS_FABRIC_B));
}

CHIP_ERROR FabricBIssuer::LoadOrCreate()
{
    VerifyOrReturnError(!mReady, CHIP_NO_ERROR);

    uint32_t version = 0;
    esp_err_t err = cfg_get_u32(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyVersion, &version);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return CreateNew();
    }
    VerifyOrReturnError(err == ESP_OK, FromEsp(err));
    VerifyOrReturnError(version == kFormatVersion, CHIP_ERROR_VERSION_MISMATCH);

    P256SerializedKeypair serialized;
    size_t len = serialized.Capacity();
    ReturnErrorOnFailure(FromEsp(cfg_get_blob(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyRootKey, serialized.Bytes(), &len)));
    ReturnErrorOnFailure(serialized.SetLength(len));
    ReturnErrorOnFailure(mRootKey.Deserialize(serialized));

    len = sizeof(mRcac);
    ReturnErrorOnFailure(FromEsp(cfg_get_blob(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyRcac, mRcac, &len)));
    mRcacLen = len;
    len = sizeof(mIpk);
    ReturnErrorOnFailure(FromEsp(cfg_get_blob(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyIpk, mIpk, &len)));
    VerifyOrReturnError(len == sizeof(mIpk), CHIP_ERROR_INTEGRITY_CHECK_FAILED);
    ReturnErrorOnFailure(FromEsp(cfg_get_u64(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyRcacId, &mRcacId)));
    ReturnErrorOnFailure(FromEsp(cfg_get_u64(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyFabricId, &mFabricId)));
    ReturnErrorOnFailure(FromEsp(cfg_get_u64(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyCtrlNode, &mControllerNodeId)));

    /* The stored RCAC must belong to the stored root key, otherwise every NOC would be unverifiable. */
    P256PublicKeySpan rcacKeySpan;
    {
        Platform::ScopedMemoryBuffer<uint8_t> chipCert;
        VerifyOrReturnError(chipCert.Alloc(kMaxCHIPCertLength), CHIP_ERROR_NO_MEMORY);
        MutableByteSpan chipCertSpan(chipCert.Get(), kMaxCHIPCertLength);
        ReturnErrorOnFailure(ConvertX509CertToChipCert(GetRcac(), chipCertSpan));
        ReturnErrorOnFailure(ExtractPublicKeyFromChipCert(chipCertSpan, rcacKeySpan));
        VerifyOrReturnError(memcmp(rcacKeySpan.data(), mRootKey.Pubkey().ConstBytes(), rcacKeySpan.size()) == 0,
                            CHIP_ERROR_INTEGRITY_CHECK_FAILED);
    }
    VerifyOrReturnError(IsValidFabricId(mFabricId) && IsOperationalNodeId(mControllerNodeId),
                        CHIP_ERROR_INTEGRITY_CHECK_FAILED);

    mReady = true;
    mCreatedNow = false;
    ESP_LOGI(TAG, "fabric B identity loaded (fabric 0x%016llX)", (unsigned long long)mFabricId);
    return CHIP_NO_ERROR;
}

CHIP_ERROR FabricBIssuer::CreateNew()
{
    ESP_LOGI(TAG, "creating a new fabric B identity");
    ReturnErrorOnFailure(mRootKey.Initialize(ECPKeyTarget::ECDSA));
    ReturnErrorOnFailure(DRBG_get_bytes(mIpk, sizeof(mIpk)));
    do {
        ReturnErrorOnFailure(RandomU64(mFabricId));
    } while (!IsValidFabricId(mFabricId));
    do {
        ReturnErrorOnFailure(RandomU64(mControllerNodeId));
    } while (!IsOperationalNodeId(mControllerNodeId));
    ReturnErrorOnFailure(RandomU64(mRcacId));

    uint32_t start;
    ReturnErrorOnFailure(ValidityStart(start));
    ChipDN rcacDn;
    ReturnErrorOnFailure(rcacDn.AddAttribute_MatterRCACId(mRcacId));
    X509CertRequestParams req = { RandomSerial(), start, kNullCertTime, rcacDn, rcacDn };
    MutableByteSpan rcacSpan(mRcac, sizeof(mRcac));
    ReturnErrorOnFailure(NewRootX509Cert(req, mRootKey, rcacSpan));
    mRcacLen = rcacSpan.size();

    P256SerializedKeypair serialized;
    ReturnErrorOnFailure(mRootKey.Serialize(serialized));

    /* Write the version marker last: an interrupted first boot simply starts over. */
    ReturnErrorOnFailure(FromEsp(cfg_erase_namespace(CFG_PART_MAIN, CFG_NS_FABRIC_B)));
    ReturnErrorOnFailure(FromEsp(
        cfg_set_blob(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyRootKey, serialized.ConstBytes(), serialized.Length())));
    ReturnErrorOnFailure(FromEsp(cfg_set_blob(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyRcac, mRcac, mRcacLen)));
    ReturnErrorOnFailure(FromEsp(cfg_set_blob(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyIpk, mIpk, sizeof(mIpk))));
    ReturnErrorOnFailure(FromEsp(cfg_set_u64(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyRcacId, mRcacId)));
    ReturnErrorOnFailure(FromEsp(cfg_set_u64(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyFabricId, mFabricId)));
    ReturnErrorOnFailure(FromEsp(cfg_set_u64(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyCtrlNode, mControllerNodeId)));
    ReturnErrorOnFailure(FromEsp(cfg_set_u32(CFG_PART_MAIN, CFG_NS_FABRIC_B, kKeyVersion, kFormatVersion)));

    mReady = true;
    mCreatedNow = true;
    return CHIP_NO_ERROR;
}

CHIP_ERROR FabricBIssuer::IssueNoc(NodeId nodeId, FabricId fabricId, const P256PublicKey & pubkey,
                                   MutableByteSpan & outNocDer)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(IsOperationalNodeId(nodeId), CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(fabricId == mFabricId, CHIP_ERROR_INVALID_ARGUMENT);

    ChipDN rcacDn;
    ReturnErrorOnFailure(ExtractSubjectDNFromX509Cert(GetRcac(), rcacDn));
    ChipDN nocDn;
    ReturnErrorOnFailure(nocDn.AddAttribute_MatterFabricId(fabricId));
    ReturnErrorOnFailure(nocDn.AddAttribute_MatterNodeId(nodeId));

    uint32_t start;
    ReturnErrorOnFailure(ValidityStart(start));
    X509CertRequestParams req = { RandomSerial(), start, kNullCertTime, nocDn, rcacDn };
    return NewNodeOperationalX509Cert(req, pubkey, mRootKey, outNocDer);
}

CHIP_ERROR FabricBIssuer::IssueControllerNoc(const P256PublicKey & pubkey, MutableByteSpan & outNocDer)
{
    return IssueNoc(mControllerNodeId, mFabricId, pubkey, outNocDer);
}

CHIP_ERROR FabricBIssuer::AllocateDeviceNodeId(NodeId & outNodeId)
{
    for (int i = 0; i < 16; i++) {
        uint64_t candidate;
        ReturnErrorOnFailure(RandomU64(candidate));
        if (IsOperationalNodeId(candidate) && candidate != mControllerNodeId) {
            outNodeId = candidate;
            return CHIP_NO_ERROR;
        }
    }
    return CHIP_ERROR_INTERNAL;
}

CHIP_ERROR FabricBIssuer::GenerateNOCChain(const ByteSpan & csrElements, const ByteSpan & csrNonce,
                                           const ByteSpan & attestationSignature, const ByteSpan & attestationChallenge,
                                           const ByteSpan & DAC, const ByteSpan & PAI,
                                           Callback::Callback<Controller::OnNOCChainGeneration> * onCompletion)
{
    (void)csrNonce;
    (void)attestationSignature;
    (void)attestationChallenge;
    (void)DAC;
    (void)PAI;
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(onCompletion != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(mNodeIdRequested, CHIP_ERROR_INCORRECT_STATE);
    mNodeIdRequested = false;

    /* The commissioner already verified the NOCSR response signature (attestation key) before calling
     * us; here we only need the subject public key out of the PKCS#10 CSR. */
    TLV::TLVReader reader;
    reader.Init(csrElements);
    if (reader.GetType() == TLV::kTLVType_NotSpecified) {
        ReturnErrorOnFailure(reader.Next());
    }
    ReturnErrorOnFailure(reader.Expect(TLV::kTLVType_Structure, TLV::AnonymousTag()));
    TLV::TLVType containerType;
    ReturnErrorOnFailure(reader.EnterContainer(containerType));
    ReturnErrorOnFailure(reader.Next(TLV::kTLVType_ByteString, TLV::ContextTag(1)));
    ByteSpan csr(reader.GetReadPoint(), reader.GetLength());
    ReturnErrorOnFailure(reader.ExitContainer(containerType));

    P256PublicKey pubkey;
    ReturnErrorOnFailure(VerifyCertificateSigningRequest(csr.data(), csr.size(), pubkey));

    Platform::ScopedMemoryBuffer<uint8_t> noc;
    VerifyOrReturnError(noc.Alloc(kMaxDERCertLength), CHIP_ERROR_NO_MEMORY);
    MutableByteSpan nocSpan(noc.Get(), kMaxDERCertLength);
    FabricId fabricId = (mNextFabricId != kUndefinedFabricId) ? mNextFabricId : mFabricId;
    ReturnErrorOnFailure(IssueNoc(mNextNodeId, fabricId, pubkey, nocSpan));

    uint8_t ipkValue[kIpkLength];
    memcpy(ipkValue, mIpk, sizeof(ipkValue));
    IdentityProtectionKeySpan ipkSpan(ipkValue);

    ESP_LOGI(TAG, "issued NOC for node 0x%016llX", (unsigned long long)mNextNodeId);
    onCompletion->mCall(onCompletion->mContext, CHIP_NO_ERROR, nocSpan, ByteSpan(), GetRcac(), MakeOptional(ipkSpan),
                        MakeOptional(mControllerNodeId));
    return CHIP_NO_ERROR;
}

} // namespace app_fabric_b
