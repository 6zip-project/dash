// Copyright (c) 2022 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/assetlocktx.h>
#include <evo/specialtx.h>
#include <evo/creditpool.h>

#include <consensus/params.h>

#include <chainparams.h>
#include <validation.h>

#include <llmq/commitment.h>
#include <llmq/signing.h>
#include <llmq/utils.h>
#include <llmq/quorums.h>

#include <algorithm>

/*
   Common code for Asset Lock and Asset Unlock
    */
maybe_error CheckAssetLockUnlockTx(const CTransaction& tx, const CBlockIndex* pindexPrev, const CCreditPool& creditPool)
{
    switch (tx.nType) {
    case TRANSACTION_ASSET_LOCK:
        return CheckAssetLockTx(tx);
    case TRANSACTION_ASSET_UNLOCK:
        return CheckAssetUnlockTx(tx, pindexPrev, creditPool);
    default:
        return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-not-asset-locks-at-all"};
    }
}

/*
   Asset Lock Transaction
   */
maybe_error CheckAssetLockTx(const CTransaction& tx)
{
    if (tx.nType != TRANSACTION_ASSET_LOCK) {
        return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetlocktx-type"};
    }

    CAmount returnAmount{0};
    for (const CTxOut& txout : tx.vout) {
        const CScript& script = txout.scriptPubKey;
        if (script.empty() || script[0] != OP_RETURN) continue;

        if (script.size() != 2 || script[1] != 0) return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetlocktx-non-empty-return"};

        if (txout.nValue <= 0) return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetlocktx-zeroout-return"};

        // Should be only one OP_RETURN
        if (returnAmount) return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetlocktx-multiple-return"};
        returnAmount = txout.nValue;
    }

    if (returnAmount == 0) return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetlocktx-no-return"};

    CAssetLockPayload assetLockTx;
    if (!GetTxPayload(tx, assetLockTx)) {
        return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetlocktx-payload"};
    }

    if (assetLockTx.getVersion() == 0 || assetLockTx.getVersion() > CAssetLockPayload::CURRENT_VERSION) {
        return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetlocktx-version"};
    }

    if (assetLockTx.getType() != 0) {
        return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetlocktx-locktype"};
    }

    if (assetLockTx.getCreditOutputs().empty()) {
        return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetlocktx-emptycreditoutputs"};
    }

    CAmount creditOutputsAmount = 0;
    for (const CTxOut& out : assetLockTx.getCreditOutputs()) {
        creditOutputsAmount += out.nValue;
        if (!out.scriptPubKey.IsPayToPublicKeyHash()) {
            return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetlocktx-pubKeyHash"};
        }
    }
    if (creditOutputsAmount != returnAmount) {
        return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetlocktx-creditamount"};
    }

    return {};
}
std::string CAssetLockPayload::ToString() const
{
    std::string outputs{"["};
    for (const CTxOut& tx: creditOutputs) {
        outputs.append(tx.ToString());
        outputs.append(",");
    }
    outputs.back() = ']';
    return strprintf("CAssetLockPayload(nVersion=%d,nType=%d,creditOutputs=%s)", nVersion, nType, outputs.c_str());
}

uint16_t CAssetLockPayload::getVersion() const {
    return nVersion;
}

uint16_t CAssetLockPayload::getType() const {
    return nType;
}

const std::vector<CTxOut>& CAssetLockPayload::getCreditOutputs() const {
    return creditOutputs;
}

/*
   Asset Unlock Transaction (withdrawals)
   */
maybe_error CAssetUnlockPayload::VerifySig(const uint256& msgHash, const CBlockIndex* pindexTip) const
{
    // That quourm hash must be active at `requestHeight`,
    // and at the quorumHash must be active in either the current or previous quorum cycle
    // and the sig must validate against that specific quorumHash.

    Consensus::LLMQType llmqType = Params().GetConsensus().llmqTypeAssetLocks;

    if (!Params().HasLLMQ(llmqType)) {
        return {ValidationInvalidReason::CONSENSUS, "bad-assetunlock-llmq-type"};
    }

    const auto& llmq_params = llmq::GetLLMQParams(llmqType);

    // We check at most 2 quorums, so, count is equal to 2
    const int count = 2;
    auto quorums = llmq::quorumManager->ScanQuorums(llmqType, pindexTip, count > -1 ? count : llmq_params.signingActiveQuorumCount);
    bool isActive = std::any_of(quorums.begin(), quorums.end(), [&](const auto &q) { return q->qc->quorumHash == quorumHash; });

    if (!isActive) {
        return {ValidationInvalidReason::CONSENSUS, "bad-assetunlock-not-active-quorum"};
    }

    if (pindexTip->nHeight < requestedHeight || pindexTip->nHeight >= getHeightToExpiry()) {
        LogPrintf("Asset unlock tx %d with requested height %d could not be accepted on height: %d\n",
                index, requestedHeight, pindexTip->nHeight);
        return {ValidationInvalidReason::CONSENSUS, "bad-assetunlock-too-late"};
    }

    const auto quorum = llmq::quorumManager->GetQuorum(llmqType, quorumHash);
    assert(quorum);

    const std::string id(strprintf("plwdtx%lld", index));

    std::vector<uint8_t> vchHash(32);
    CSHA256().Write(reinterpret_cast<const uint8_t*>(id.data()), id.size()).Finalize(vchHash.data());
    uint256 requestId(vchHash);

    uint256 signHash = llmq::utils::BuildSignHash(llmqType, quorum->qc->quorumHash, requestId, msgHash);
    if (quorumSig.VerifyInsecure(quorum->qc->quorumPublicKey, signHash)) {
        return {};
    }

    return {ValidationInvalidReason::CONSENSUS, "bad-assetunlock-not-verified"};
}

maybe_error CheckAssetUnlockTx(const CTransaction& tx, const CBlockIndex* pindexPrev, const CCreditPool& creditPool)
{
    if (tx.nType != TRANSACTION_ASSET_UNLOCK) {
        return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetunlocktx-type"};
    }

    if (!tx.vin.empty()) {
        return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetunlocktx-have-input"};
    }

    if (tx.vout.size() > CAssetUnlockPayload::MAXIMUM_WITHDRAWALS) {
        return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetunlocktx-too-many-outs"};
    }

    CAssetUnlockPayload assetUnlockTx;
    if (!GetTxPayload(tx, assetUnlockTx)) {
        return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetunlocktx-payload"};
    }

    if (assetUnlockTx.getVersion() == 0 || assetUnlockTx.getVersion() > CAssetUnlockPayload::CURRENT_VERSION) {
        return {ValidationInvalidReason::TX_BAD_SPECIAL, "bad-assetunlocktx-version"};
    }

    if (creditPool.indexes.contains(assetUnlockTx.getIndex())) {
        return {ValidationInvalidReason::CONSENSUS, "bad-assetunlock-duplicated-index"};
    }

    const CBlockIndex* pindexQuorum = WITH_LOCK(cs_main, return LookupBlockIndex(assetUnlockTx.getQuorumHash()));
    if (!pindexQuorum) {
        return {ValidationInvalidReason::CONSENSUS, "bad-assetunlock-quorum-hash"};
    }

    // Copy transaction except `quorumSig` field to calculate hash
    CMutableTransaction tx_copy(tx);
    auto payload_copy = CAssetUnlockPayload(assetUnlockTx.getVersion(), assetUnlockTx.getIndex(), assetUnlockTx.getFee(), assetUnlockTx.getRequestedHeight(), assetUnlockTx.getQuorumHash(), CBLSSignature());
    SetTxPayload(tx_copy, payload_copy);

    uint256 msgHash = tx_copy.GetHash();
    return assetUnlockTx.VerifySig(msgHash, pindexPrev);
}

std::string CAssetUnlockPayload::ToString() const
{
    return strprintf("CAssetUnlockPayload(nVersion=%d,index=%d,fee=%d.%08d,requestedHeight=%d,quorumHash=%d,quorumSig=%s",
            nVersion, index, fee / COIN, fee % COIN, requestedHeight, quorumHash.GetHex(), quorumSig.ToString().c_str());
}

uint16_t CAssetUnlockPayload::getVersion() const {
    return nVersion;
}

uint64_t CAssetUnlockPayload::getIndex() const {
    return index;
}

uint32_t CAssetUnlockPayload::getFee() const {
    return fee;
}

uint32_t CAssetUnlockPayload::getRequestedHeight() const {
    return requestedHeight;
}

const uint256& CAssetUnlockPayload::getQuorumHash() const {
    return quorumHash;
}

const CBLSSignature& CAssetUnlockPayload::getQuorumSig() const {
    return quorumSig;
}

int CAssetUnlockPayload::getHeightToExpiry() const {
    int expiryHeight = 48;
    return requestedHeight + expiryHeight;
}
