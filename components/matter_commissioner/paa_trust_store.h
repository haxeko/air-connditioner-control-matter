#pragma once

/*
 * PAA trust store backed by the production PAA bundle embedded in the firmware
 * (components/matter_commissioner/paa, a DCL MainNet mirror from connectedhomeip).
 *
 * When "allow test devices" is switched on for one pairing attempt, the CHIP test PAAs
 * (Chip-Test-PAA-FFF1 / NoVID) are also accepted so that DIY sensors with test DACs can be paired.
 */

#include <atomic>

#include <credentials/attestation_verifier/DeviceAttestationVerifier.h>
#include <lib/support/Span.h>

namespace app_fabric_b {

class PaaTrustStore : public chip::Credentials::AttestationTrustStore
{
public:
    static constexpr size_t kMaxCerts = 160;

    /** Parse the embedded bundle. Returns the number of certificates found. */
    size_t Init(const uint8_t * bundle, size_t bundleLen);
    size_t Count() const { return mCount; }
    void SetAllowTestPaa(bool allow) { mAllowTest.store(allow); }
    bool AllowTestPaa() const { return mAllowTest.load(); }

    CHIP_ERROR GetProductAttestationAuthorityCert(const chip::ByteSpan & skid,
                                                  chip::MutableByteSpan & outPaaDerBuffer) const override;

private:
    chip::ByteSpan mCerts[kMaxCerts];
    size_t mCount = 0;
    std::atomic<bool> mAllowTest{ false };
};

} // namespace app_fabric_b
