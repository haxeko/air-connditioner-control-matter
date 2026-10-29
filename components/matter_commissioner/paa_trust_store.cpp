#include "paa_trust_store.h"

#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/CodeUtils.h>

#include "esp_log.h"

using namespace chip;

namespace app_fabric_b {

namespace {
constexpr char TAG[] = "paa_store";

/* Length of the DER element starting at p (tag + length + value), 0 if malformed. */
size_t DerElementLength(const uint8_t * p, size_t avail)
{
    if (avail < 2 || p[0] != 0x30) {
        return 0;
    }
    size_t len;
    size_t hdr;
    if (p[1] < 0x80) {
        len = p[1];
        hdr = 2;
    } else {
        size_t n = p[1] & 0x7F;
        if (n == 0 || n > 3 || avail < 2 + n) {
            return 0;
        }
        len = 0;
        for (size_t i = 0; i < n; i++) {
            len = (len << 8) | p[2 + i];
        }
        hdr = 2 + n;
    }
    return (hdr + len <= avail) ? hdr + len : 0;
}
} // namespace

size_t PaaTrustStore::Init(const uint8_t * bundle, size_t bundleLen)
{
    mCount = 0;
    size_t off = 0;
    while (off < bundleLen && mCount < kMaxCerts) {
        size_t len = DerElementLength(bundle + off, bundleLen - off);
        if (len == 0) {
            ESP_LOGE(TAG, "malformed PAA bundle at offset %u", (unsigned)off);
            break;
        }
        mCerts[mCount++] = ByteSpan(bundle + off, len);
        off += len;
    }
    ESP_LOGI(TAG, "%u production PAA certificates loaded", (unsigned)mCount);
    return mCount;
}

CHIP_ERROR PaaTrustStore::GetProductAttestationAuthorityCert(const ByteSpan & skid, MutableByteSpan & outPaaDerBuffer) const
{
    VerifyOrReturnError(!skid.empty() && skid.data() != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(skid.size() == Crypto::kSubjectKeyIdentifierLength, CHIP_ERROR_INVALID_ARGUMENT);

    for (size_t i = 0; i < mCount; i++) {
        uint8_t skidBuf[Crypto::kSubjectKeyIdentifierLength] = { 0 };
        MutableByteSpan candidateSkid{ skidBuf };
        if (Crypto::ExtractSKIDFromX509Cert(mCerts[i], candidateSkid) != CHIP_NO_ERROR) {
            continue;
        }
        if (skid.data_equal(candidateSkid)) {
            return CopySpanToMutableSpan(mCerts[i], outPaaDerBuffer);
        }
    }

    if (mAllowTest.load()) {
        const Credentials::AttestationTrustStore * testStore = Credentials::GetTestAttestationTrustStore();
        if (testStore != nullptr) {
            return testStore->GetProductAttestationAuthorityCert(skid, outPaaDerBuffer);
        }
    }
    return CHIP_ERROR_CA_CERT_NOT_FOUND;
}

} // namespace app_fabric_b
