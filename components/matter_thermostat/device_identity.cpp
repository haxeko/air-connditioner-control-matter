#include "device_identity.h"

#include <string.h>

#include <lib/support/CodeUtils.h>
#include <setup_payload/SetupPayload.h>

#include "config_store.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "psa/crypto.h"

using namespace chip;

namespace app_identity {

namespace {
constexpr char TAG[] = "identity";
constexpr uint32_t kFormatVersion = 1;
DeviceIdentityProvider gProvider;
} // namespace

DeviceIdentityProvider & GetDeviceIdentity()
{
    return gProvider;
}

esp_err_t DeviceIdentityProvider::Create()
{
    /* Uniform draw (rejection sampling) of a passcode the spec allows (not 00000000, 11111111, 12345678, ...). */
    const uint32_t range = kSetupPINCodeMaximumValue + 1;
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % range);
    uint32_t passcode = 0;
    do {
        uint32_t r;
        do {
            r = esp_random();
        } while (r >= limit);
        passcode = r % range;
    } while (!PayloadContents::IsValidSetupPIN(passcode));

    uint16_t discriminator;
    do {
        discriminator = static_cast<uint16_t>(esp_random() & 0x0FFF);
    } while (discriminator == 0x0F00); /* the well known test discriminator (3840) */

    esp_fill_random(mSalt, sizeof(mSalt));

    Crypto::Spake2pVerifier verifier;
    CHIP_ERROR err = verifier.Generate(kIterations, ByteSpan(mSalt), passcode);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "SPAKE2+ verifier generation failed: %" CHIP_ERROR_FORMAT, err.Format());
        return ESP_FAIL;
    }
    MutableByteSpan verifierSpan(mVerifier);
    err = verifier.Serialize(verifierSpan);
    if (err != CHIP_NO_ERROR || verifierSpan.size() != sizeof(mVerifier)) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(cfg_erase_namespace(CFG_PART_MAIN, CFG_NS_IDENTITY), TAG, "erase");
    ESP_RETURN_ON_ERROR(cfg_set_u32(CFG_PART_MAIN, CFG_NS_IDENTITY, "pin", passcode), TAG, "pin");
    ESP_RETURN_ON_ERROR(cfg_set_u32(CFG_PART_MAIN, CFG_NS_IDENTITY, "disc", discriminator), TAG, "disc");
    ESP_RETURN_ON_ERROR(cfg_set_blob(CFG_PART_MAIN, CFG_NS_IDENTITY, "salt", mSalt, sizeof(mSalt)), TAG, "salt");
    ESP_RETURN_ON_ERROR(cfg_set_blob(CFG_PART_MAIN, CFG_NS_IDENTITY, "verifier", mVerifier, sizeof(mVerifier)), TAG,
                        "verifier");
    ESP_RETURN_ON_ERROR(cfg_set_u32(CFG_PART_MAIN, CFG_NS_IDENTITY, "ver", kFormatVersion), TAG, "ver");

    mPasscode = passcode;
    mDiscriminator = discriminator;
    ESP_LOGI(TAG, "unique commissioning data generated (discriminator %u)", discriminator);
    return ESP_OK;
}

esp_err_t DeviceIdentityProvider::LoadOrCreate()
{
    if (mReady) {
        return ESP_OK;
    }
    /* CHIP's crypto PAL uses PSA on IDF >= 6; make sure it is initialised before CHIP starts. */
    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed");
        return ESP_FAIL;
    }

    uint32_t ver = 0;
    esp_err_t err = cfg_get_u32(CFG_PART_MAIN, CFG_NS_IDENTITY, "ver", &ver);
    if (err == ESP_OK && ver == kFormatVersion) {
        uint32_t pin = 0, disc = 0;
        size_t saltLen = sizeof(mSalt), verLen = sizeof(mVerifier);
        if (cfg_get_u32(CFG_PART_MAIN, CFG_NS_IDENTITY, "pin", &pin) == ESP_OK &&
            cfg_get_u32(CFG_PART_MAIN, CFG_NS_IDENTITY, "disc", &disc) == ESP_OK &&
            cfg_get_blob(CFG_PART_MAIN, CFG_NS_IDENTITY, "salt", mSalt, &saltLen) == ESP_OK &&
            cfg_get_blob(CFG_PART_MAIN, CFG_NS_IDENTITY, "verifier", mVerifier, &verLen) == ESP_OK &&
            saltLen == sizeof(mSalt) && verLen == sizeof(mVerifier) && PayloadContents::IsValidSetupPIN(pin) &&
            disc <= 0x0FFF) {
            mPasscode = pin;
            mDiscriminator = static_cast<uint16_t>(disc);
            mReady = true;
            return ESP_OK;
        }
        ESP_LOGE(TAG, "stored commissioning data is corrupt, regenerating");
    } else if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_OK) {
        ESP_LOGE(TAG, "cannot read commissioning data: %s", esp_err_to_name(err));
        return err;
    }
    err = Create();
    mReady = (err == ESP_OK);
    return err;
}

CHIP_ERROR DeviceIdentityProvider::GetSetupDiscriminator(uint16_t & setupDiscriminator)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    setupDiscriminator = mDiscriminator;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DeviceIdentityProvider::SetSetupDiscriminator(uint16_t setupDiscriminator)
{
    (void)setupDiscriminator;
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR DeviceIdentityProvider::GetSpake2pIterationCount(uint32_t & iterationCount)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    iterationCount = kIterations;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DeviceIdentityProvider::GetSpake2pSalt(MutableByteSpan & saltBuf)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    return CopySpanToMutableSpan(ByteSpan(mSalt), saltBuf);
}

CHIP_ERROR DeviceIdentityProvider::GetSpake2pVerifier(MutableByteSpan & verifierBuf, size_t & outVerifierLen)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    outVerifierLen = sizeof(mVerifier);
    return CopySpanToMutableSpan(ByteSpan(mVerifier), verifierBuf);
}

CHIP_ERROR DeviceIdentityProvider::GetSetupPasscode(uint32_t & setupPasscode)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    setupPasscode = mPasscode;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DeviceIdentityProvider::SetSetupPasscode(uint32_t setupPasscode)
{
    (void)setupPasscode;
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

} // namespace app_identity
