#pragma once

/*
 * Commissionable data unique to this device (setup passcode, discriminator, SPAKE2+ salt/verifier).
 * Generated on first boot from the hardware RNG and stored in NVS (namespace CFG_NS_IDENTITY);
 * never the shared test values 20202021/3840. Exposed to CHIP through
 * CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER.
 */

#include <crypto/CHIPCryptoPAL.h>
#include <platform/CommissionableDataProvider.h>

#include "esp_err.h"

namespace app_identity {

class DeviceIdentityProvider : public chip::DeviceLayer::CommissionableDataProvider
{
public:
    static constexpr uint32_t kIterations = 10000;
    static constexpr size_t kSaltLength = 32;

    /** Load the identity or create it. Must run before esp_matter::start(). */
    esp_err_t LoadOrCreate();

    CHIP_ERROR GetSetupDiscriminator(uint16_t & setupDiscriminator) override;
    CHIP_ERROR SetSetupDiscriminator(uint16_t setupDiscriminator) override;
    CHIP_ERROR GetSpake2pIterationCount(uint32_t & iterationCount) override;
    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan & saltBuf) override;
    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan & verifierBuf, size_t & outVerifierLen) override;
    CHIP_ERROR GetSetupPasscode(uint32_t & setupPasscode) override;
    CHIP_ERROR SetSetupPasscode(uint32_t setupPasscode) override;

private:
    esp_err_t Create();

    bool mReady = false;
    uint32_t mPasscode = 0;
    uint16_t mDiscriminator = 0;
    uint8_t mSalt[kSaltLength];
    uint8_t mVerifier[chip::Crypto::kSpake2p_VerifierSerialized_Length];
};

DeviceIdentityProvider & GetDeviceIdentity();

} // namespace app_identity
