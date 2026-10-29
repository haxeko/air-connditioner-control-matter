#pragma once

/*
 * Operational credentials issuer of Fabric B (the fabric this device owns to reach sensors).
 *
 * Persisted once, never regenerated while the data exists (NVS namespace CFG_NS_FABRIC_B):
 *   - root CA key pair (P-256) and its self-signed RCAC (DER, no expiry),
 *   - random 64-bit fabric ID, random controller node ID,
 *   - random IPK (identity protection key) delivered to every commissioned sensor.
 * NOCs are signed directly by the root (no ICAC) and carry no expiry, so a device without a
 * real-time clock never has to renew them.
 */

#include <controller/OperationalCredentialsDelegate.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/DataModelTypes.h>
#include <lib/core/NodeId.h>
#include <lib/support/Span.h>

namespace app_fabric_b {

class FabricBIssuer : public chip::Controller::OperationalCredentialsDelegate
{
public:
    static constexpr size_t kIpkLength = chip::Crypto::CHIP_CRYPTO_SYMMETRIC_KEY_LENGTH_BYTES;
    static constexpr size_t kMaxDerCertLength = 600;

    /** Load the stored identity or create (and persist) a new one. */
    CHIP_ERROR LoadOrCreate();
    bool IsReady() const { return mReady; }
    bool WasCreatedNow() const { return mCreatedNow; }

    chip::FabricId GetFabricId() const { return mFabricId; }
    chip::NodeId GetControllerNodeId() const { return mControllerNodeId; }
    const chip::Crypto::P256PublicKey & GetRootPublicKey() const { return mRootKey.Pubkey(); }
    chip::ByteSpan GetRcac() const { return chip::ByteSpan(mRcac, mRcacLen); }
    chip::ByteSpan GetIpk() const { return chip::ByteSpan(mIpk, sizeof(mIpk)); }

    /** NOC for this controller itself (only needed when fabric B is (re)created in the FabricTable). */
    CHIP_ERROR IssueControllerNoc(const chip::Crypto::P256PublicKey & pubkey, chip::MutableByteSpan & outNocDer);

    /** Pick a random operational node ID that is not the controller's own. */
    CHIP_ERROR AllocateDeviceNodeId(chip::NodeId & outNodeId);

    /** Remove the whole identity (used when the user resets Fabric B). */
    static CHIP_ERROR EraseStored();

    // chip::Controller::OperationalCredentialsDelegate
    CHIP_ERROR GenerateNOCChain(const chip::ByteSpan & csrElements, const chip::ByteSpan & csrNonce,
                                const chip::ByteSpan & attestationSignature, const chip::ByteSpan & attestationChallenge,
                                const chip::ByteSpan & DAC, const chip::ByteSpan & PAI,
                                chip::Callback::Callback<chip::Controller::OnNOCChainGeneration> * onCompletion) override;
    void SetNodeIdForNextNOCRequest(chip::NodeId nodeId) override
    {
        mNextNodeId = nodeId;
        mNodeIdRequested = true;
    }
    void SetFabricIdForNextNOCRequest(chip::FabricId fabricId) override { mNextFabricId = fabricId; }

private:
    CHIP_ERROR IssueNoc(chip::NodeId nodeId, chip::FabricId fabricId, const chip::Crypto::P256PublicKey & pubkey,
                        chip::MutableByteSpan & outNocDer);
    CHIP_ERROR CreateNew();

    chip::Crypto::P256Keypair mRootKey;
    uint8_t mRcac[kMaxDerCertLength];
    size_t mRcacLen = 0;
    uint8_t mIpk[kIpkLength];
    uint64_t mRcacId = 0;
    chip::FabricId mFabricId = chip::kUndefinedFabricId;
    chip::NodeId mControllerNodeId = chip::kUndefinedNodeId;
    chip::NodeId mNextNodeId = chip::kUndefinedNodeId;
    chip::FabricId mNextFabricId = chip::kUndefinedFabricId;
    bool mNodeIdRequested = false;
    bool mReady = false;
    bool mCreatedNow = false;
};

} // namespace app_fabric_b
