// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "validation.h"

#include "arith_uint256.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "config.h"
#include "chain.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "hash.h"
#include "init.h"
#include "netbase.h"
#include "policy/fees.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "random.h"
#include "script/script.h"
#include "script/scriptcache.h"
#include "script/sigcache.h"
#include "script/standard.h"
#include "timedata.h"
#include "tinyformat.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "undo.h"
#include "util.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "validationinterface.h"
#include "versionbits.h"
#include "warnings.h"
#include "dstencode.h"

#include <atomic>
#include <sstream>

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/math/distributions/poisson.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/thread.hpp>

#if defined(NDEBUG)
#error "Bitcoin cannot be compiled without assertions."
#endif

/**
 * Global state
 */

CCriticalSection cs_main;

BlockMap mapBlockIndex;
CChain chainActive;
CBlockIndex *pindexBestHeader = nullptr;
CWaitableCriticalSection csBestBlock;
CConditionVariable cvBlockChange;
int nScriptCheckThreads = 0;
std::atomic_bool fImporting(false);
bool fReindex = false;
bool fTxIndex = false;
bool fHavePruned = false;
bool fPruneMode = false;
bool fIsBareMultisigStd = DEFAULT_PERMIT_BAREMULTISIG;
bool fRequireStandard = true;
bool fCheckBlockIndex = false;
bool fCheckpointsEnabled = DEFAULT_CHECKPOINTS_ENABLED;
size_t nCoinCacheUsage = 5000 * 300;
uint64_t nPruneTarget = 0;
int64_t nMaxTipAge = DEFAULT_MAX_TIP_AGE;

uint256 hashAssumeValid;
arith_uint256 nMinimumChainWork;

CFeeRate minRelayTxFee = CFeeRate(DEFAULT_MIN_RELAY_TX_FEE);
Amount maxTxFee = DEFAULT_TRANSACTION_MAXFEE;

CTxMemPool mempool(::minRelayTxFee);

static void CheckBlockIndex(const Consensus::Params &consensusParams);

/** Constant stuff for coinbase transactions we create: */
CScript COINBASE_FLAGS;

const std::string strMessageMagic = "Bitcoin Candy Signed Message:\n";

// Internal stuff
namespace {

struct CBlockIndexWorkComparator {
    bool operator()(CBlockIndex *pa, CBlockIndex *pb) const {
        // First sort by most total work, ...
        if (pa->nChainWork > pb->nChainWork) return false;
        if (pa->nChainWork < pb->nChainWork) return true;

        // ... then by earliest time received, ...
        if (pa->nSequenceId < pb->nSequenceId) return false;
        if (pa->nSequenceId > pb->nSequenceId) return true;

        // Use pointer address as tie breaker (should only happen with blocks
        // loaded from disk, as those all have id 0).
        if (pa < pb) return false;
        if (pa > pb) return true;

        // Identical blocks.
        return false;
    }
};

CBlockIndex *pindexBestInvalid;
CBlockIndex *pindexBestParked;

/**
 * The best finalized block.
 * This block cannot be reorged in any way, shape or form.
 */
CBlockIndex const *pindexFinalized;

/**
 * The set of all CBlockIndex entries with BLOCK_VALID_TRANSACTIONS (for itself
 * and all ancestors) and as good as our current tip or better. Entries may be
 * failed, though, and pruning nodes may be missing the data for the block.
 */
std::set<CBlockIndex *, CBlockIndexWorkComparator> setBlockIndexCandidates;
/**
 * All pairs A->B, where A (or one of its ancestors) misses transactions, but B
 * has transactions. Pruned nodes may have entries where B is missing data.
 */
std::multimap<CBlockIndex *, CBlockIndex *> mapBlocksUnlinked;

CCriticalSection cs_LastBlockFile;
std::vector<CBlockFileInfo> vinfoBlockFile;
int nLastBlockFile = 0;
/**
 * Global flag to indicate we should check to see if there are block/undo files
 * that should be deleted. Set on startup or if we allocate more file space when
 * we're in prune mode.
 */
bool fCheckForPruning = false;

/**
 * Every received block is assigned a unique and increasing identifier, so we
 * know which one to give priority in case of a fork.
 */
CCriticalSection cs_nBlockSequenceId;
/** Blocks loaded from disk are assigned id 0, so start the counter at 1. */
int32_t nBlockSequenceId = 1;
/** Decreasing counter (used by subsequent preciousblock calls). */
int32_t nBlockReverseSequenceId = -1;
/** chainwork for the last block that preciousblock has been applied to. */
arith_uint256 nLastPreciousChainwork = 0;

/** Dirty block index entries. */
std::set<CBlockIndex *> setDirtyBlockIndex;

/** Dirty block file entries. */
std::set<int> setDirtyFileInfo;
} // namespace

/**
 * Use this class to start tracking transactions that are removed from the
 * mempool and pass all those transactions through SyncTransaction when the
 * object goes out of scope. This is currently only used to call SyncTransaction
 * on conflicts removed from the mempool during block connection. Applied in
 * ActivateBestChain around ActivateBestStep which in turn calls:
 * ConnectTip->removeForBlock->removeConflicts
 */
class MemPoolConflictRemovalTracker {
private:
    std::vector<CTransactionRef> conflictedTxs;
    CTxMemPool &pool;

public:
    MemPoolConflictRemovalTracker(CTxMemPool &_pool) : pool(_pool) {
        pool.NotifyEntryRemoved.connect(boost::bind(
            &MemPoolConflictRemovalTracker::NotifyEntryRemoved, this, _1, _2));
    }

    void NotifyEntryRemoved(CTransactionRef txRemoved,
                            MemPoolRemovalReason reason) {
        if (reason == MemPoolRemovalReason::CONFLICT) {
            conflictedTxs.push_back(txRemoved);
        }
    }

    ~MemPoolConflictRemovalTracker() {
        pool.NotifyEntryRemoved.disconnect(boost::bind(
            &MemPoolConflictRemovalTracker::NotifyEntryRemoved, this, _1, _2));
        for (const auto &tx : conflictedTxs) {
            GetMainSignals().SyncTransaction(
                *tx, nullptr, CMainSignals::SYNC_TRANSACTION_NOT_IN_BLOCK);
        }
        conflictedTxs.clear();
    }
};

CBlockIndex *FindForkInGlobalIndex(const CChain &chain,
                                   const CBlockLocator &locator) {
    // Find the first block the caller has in the main chain
    for (const uint256 &hash : locator.vHave) {
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end()) {
            CBlockIndex *pindex = (*mi).second;
            if (chain.Contains(pindex)) return pindex;
            if (pindex->GetAncestor(chain.Height()) == chain.Tip()) {
                return chain.Tip();
            }
        }
    }
    return chain.Genesis();
}

CCoinsViewCache *pcoinsTip = nullptr;
CBlockTreeDB *pblocktree = nullptr;

enum FlushStateMode {
    FLUSH_STATE_NONE,
    FLUSH_STATE_IF_NEEDED,
    FLUSH_STATE_PERIODIC,
    FLUSH_STATE_ALWAYS
};

// See definition for documentation
static bool FlushStateToDisk(CValidationState &state, FlushStateMode mode,
                             int nManualPruneHeight = 0);
static void FindFilesToPruneManual(std::set<int> &setFilesToPrune,
                                   int nManualPruneHeight);
static uint32_t GetBlockScriptFlags(const Config &config,
                                    const CBlockIndex *pChainTip);

static bool IsFinalTx(const CTransaction &tx, int nBlockHeight,
                      int64_t nBlockTime) {
    if (tx.nLockTime == 0) {
        return true;
    }

    int64_t lockTime = tx.nLockTime;
    int64_t lockTimeLimit =
        (lockTime < LOCKTIME_THRESHOLD) ? nBlockHeight : nBlockTime;
    if (lockTime < lockTimeLimit) {
        return true;
    }

    for (const auto &txin : tx.vin) {
        if (txin.nSequence != CTxIn::SEQUENCE_FINAL) {
            return false;
        }
    }
    return true;
}

/**
 * Calculates the block height and previous block's median time past at
 * which the transaction will be considered final in the context of BIP 68.
 * Also removes from the vector of input heights any entries which did not
 * correspond to sequence locked inputs as they do not affect the calculation.
 */
static std::pair<int, int64_t>
CalculateSequenceLocks(const CTransaction &tx, int flags,
                       std::vector<int> *prevHeights,
                       const CBlockIndex &block) {
    assert(prevHeights->size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    // tx.nVersion is signed integer so requires cast to unsigned otherwise
    // we would be doing a signed comparison and half the range of nVersion
    // wouldn't support BIP 68.
    bool fEnforceBIP68 = static_cast<uint32_t>(tx.nVersion) >= 2 &&
                         flags & LOCKTIME_VERIFY_SEQUENCE;

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to
    if (!fEnforceBIP68) {
        return std::make_pair(nMinHeight, nMinTime);
    }

    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
        const CTxIn &txin = tx.vin[txinIndex];

        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            // The height of this input is not relevant for sequence locks
            (*prevHeights)[txinIndex] = 0;
            continue;
        }

        int nCoinHeight = (*prevHeights)[txinIndex];

        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) {
            int64_t nCoinTime = block.GetAncestor(std::max(nCoinHeight - 1, 0))
                                    ->GetMedianTimePast();
            // NOTE: Subtract 1 to maintain nLockTime semantics.
            // BIP 68 relative lock times have the semantics of calculating the
            // first block or time at which the transaction would be valid. When
            // calculating the effective block time or height for the entire
            // transaction, we switch to using the semantics of nLockTime which
            // is the last invalid block time or height. Thus we subtract 1 from
            // the calculated time or height.

            // Time-based relative lock-times are measured from the smallest
            // allowed timestamp of the block containing the txout being spent,
            // which is the median time past of the block prior.
            nMinTime = std::max(
                nMinTime,
                nCoinTime +
                    (int64_t)((txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK)
                              << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) -
                    1);
        } else {
            nMinHeight = std::max(
                nMinHeight,
                nCoinHeight +
                    (int)(txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
        }
    }

    return std::make_pair(nMinHeight, nMinTime);
}

static bool EvaluateSequenceLocks(const CBlockIndex &block,
                                  std::pair<int, int64_t> lockPair) {
    assert(block.pprev);
    int64_t nBlockTime = block.pprev->GetMedianTimePast();
    if (lockPair.first >= block.nHeight || lockPair.second >= nBlockTime)
        return false;

    return true;
}

bool SequenceLocks(const CTransaction &tx, int flags,
                   std::vector<int> *prevHeights, const CBlockIndex &block) {
    return EvaluateSequenceLocks(
        block, CalculateSequenceLocks(tx, flags, prevHeights, block));
}

bool TestLockPointValidity(const LockPoints *lp) {
    AssertLockHeld(cs_main);
    assert(lp);
    // If there are relative lock times then the maxInputBlock will be set
    // If there are no relative lock times, the LockPoints don't depend on the
    // chain
    if (lp->maxInputBlock) {
        // Check whether chainActive is an extension of the block at which the
        // LockPoints
        // calculation was valid.  If not LockPoints are no longer valid
        if (!chainActive.Contains(lp->maxInputBlock)) {
            return false;
        }
    }

    // LockPoints still valid
    return true;
}

bool CheckSequenceLocks(const CTransaction &tx, int flags, LockPoints *lp,
                        bool useExistingLockPoints) {
    AssertLockHeld(cs_main);
    AssertLockHeld(mempool.cs);

    CBlockIndex *tip = chainActive.Tip();
    CBlockIndex index;
    index.pprev = tip;
    // CheckSequenceLocks() uses chainActive.Height()+1 to evaluate height based
    // locks because when SequenceLocks() is called within ConnectBlock(), the
    // height of the block *being* evaluated is what is used. Thus if we want to
    // know if a transaction can be part of the *next* block, we need to use one
    // more than chainActive.Height()
    index.nHeight = tip->nHeight + 1;

    std::pair<int, int64_t> lockPair;
    if (useExistingLockPoints) {
        assert(lp);
        lockPair.first = lp->height;
        lockPair.second = lp->time;
    } else {
        // pcoinsTip contains the UTXO set for chainActive.Tip()
        CCoinsViewMemPool viewMemPool(pcoinsTip, mempool);
        std::vector<int> prevheights;
        prevheights.resize(tx.vin.size());
        for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
            const CTxIn &txin = tx.vin[txinIndex];
            Coin coin;
            if (!viewMemPool.GetCoin(txin.prevout, coin)) {
                return error("%s: Missing input", __func__);
            }
            if (coin.GetHeight() == MEMPOOL_HEIGHT) {
                // Assume all mempool transaction confirm in the next block
                prevheights[txinIndex] = tip->nHeight + 1;
            } else {
                prevheights[txinIndex] = coin.GetHeight();
            }
        }
        lockPair = CalculateSequenceLocks(tx, flags, &prevheights, index);
        if (lp) {
            lp->height = lockPair.first;
            lp->time = lockPair.second;
            // Also store the hash of the block with the highest height of all
            // the blocks which have sequence locked prevouts. This hash needs
            // to still be on the chain for these LockPoint calculations to be
            // valid.
            // Note: It is impossible to correctly calculate a maxInputBlock if
            // any of the sequence locked inputs depend on unconfirmed txs,
            // except in the special case where the relative lock time/height is
            // 0, which is equivalent to no sequence lock. Since we assume input
            // height of tip+1 for mempool txs and test the resulting lockPair
            // from CalculateSequenceLocks against tip+1. We know
            // EvaluateSequenceLocks will fail if there was a non-zero sequence
            // lock on a mempool input, so we can use the return value of
            // CheckSequenceLocks to indicate the LockPoints validity
            int maxInputHeight = 0;
            for (int height : prevheights) {
                // Can ignore mempool inputs since we'll fail if they had
                // non-zero locks
                if (height != tip->nHeight + 1) {
                    maxInputHeight = std::max(maxInputHeight, height);
                }
            }
            lp->maxInputBlock = tip->GetAncestor(maxInputHeight);
        }
    }
    return EvaluateSequenceLocks(index, lockPair);
}

uint64_t GetSigOpCountWithoutP2SH(const CTransaction &tx) {
    uint64_t nSigOps = 0;
    for (const auto &txin : tx.vin) {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    for (const auto &txout : tx.vout) {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

uint64_t GetP2SHSigOpCount(const CTransaction &tx,
                           const CCoinsViewCache &inputs) {
    if (tx.IsCoinBase()) {
        return 0;
    }

    uint64_t nSigOps = 0;
    for (auto &i : tx.vin) {
        const CTxOut &prevout = inputs.GetOutputFor(i);
        if (prevout.scriptPubKey.IsPayToScriptHash()) {
            nSigOps += prevout.scriptPubKey.GetSigOpCount(i.scriptSig);
        }
    }
    return nSigOps;
}

uint64_t GetTransactionSigOpCount(const CTransaction &tx,
                                  const CCoinsViewCache &inputs, int flags) {
    uint64_t nSigOps = GetSigOpCountWithoutP2SH(tx);
    if (tx.IsCoinBase()) {
        return nSigOps;
    }

    if (flags & SCRIPT_VERIFY_P2SH) {
        nSigOps += GetP2SHSigOpCount(tx, inputs);
    }

    return nSigOps;
}

static bool CheckTransactionCommon(const CTransaction &tx,
                                   CValidationState &state) {
    // Basic checks that don't depend on any context
    if (tx.vin.empty()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-vin-empty");
    }

    if (tx.vout.empty()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-vout-empty");
    }

    // Size limit
    if (::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION) > MAX_TX_SIZE) {
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-oversize");
    }

    // Check for negative or overflow output values
    Amount nValueOut(0);
    for (const auto &txout : tx.vout) {
        if (txout.nValue < Amount(0)) {
            return state.DoS(100, false, REJECT_INVALID,
                             "bad-txns-vout-negative");
        }

        if (txout.nValue > MAX_MONEY) {
            return state.DoS(100, false, REJECT_INVALID,
                             "bad-txns-vout-toolarge");
        }

        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut)) {
            return state.DoS(100, false, REJECT_INVALID,
                             "bad-txns-txouttotal-toolarge");
        }
    }

    if (GetSigOpCountWithoutP2SH(tx) > MAX_TX_SIGOPS_COUNT) {
        return state.DoS(100, false, REJECT_INVALID, "bad-txn-sigops");
    }



    return true;
}

bool CheckCoinbase(const CTransaction &tx, CValidationState &state) {
    if (!tx.IsCoinBase()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-cb-missing", false,
                         "first tx is not coinbase");
    }

    if (!CheckTransactionCommon(tx, state)) {
        // CheckTransactionCommon fill in the state.
        return false;
    }

    if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() > 100) {
        return state.DoS(100, false, REJECT_INVALID, "bad-cb-length");
    }

    return true;
}

bool CheckRegularTransaction(const CTransaction &tx, CValidationState &state) {
    if (tx.IsCoinBase()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-tx-coinbase");
    }

    if (!CheckTransactionCommon(tx, state)) {
        // CheckTransactionCommon fill in the state.
        return false;
    }

    std::unordered_set<COutPoint, SaltedOutpointHasher> vInOutPoints;
    for (const auto &txin : tx.vin) {
        if (txin.prevout.IsNull()) {
            return state.DoS(10, false, REJECT_INVALID,
                             "bad-txns-prevout-null");
        }
        if (!vInOutPoints.insert(txin.prevout).second) {
            return state.DoS(100, false, REJECT_INVALID,
                             "bad-txns-inputs-duplicate");
        }
    }

    return true;
}

void LimitMempoolSize(CTxMemPool &pool, size_t limit, unsigned long age) {
    int expired = pool.Expire(GetTime() - age);
    if (expired != 0) {
        LogPrint("mempool", "Expired %i transactions from the memory pool\n",
                 expired);
    }

    std::vector<COutPoint> vNoSpendsRemaining;
    pool.TrimToSize(limit, &vNoSpendsRemaining);
    for (const COutPoint &removed : vNoSpendsRemaining) {
        pcoinsTip->Uncache(removed);
    }
}

/** Convert CValidationState to a human-readable message for logging */
std::string FormatStateMessage(const CValidationState &state) {
    return strprintf(
        "%s%s (code %i)", state.GetRejectReason(),
        state.GetDebugMessage().empty() ? "" : ", " + state.GetDebugMessage(),
        state.GetRejectCode());
}

static bool IsCurrentForFeeEstimation() {
    AssertLockHeld(cs_main);
    if (IsInitialBlockDownload()) {
        return false;
    }
    if (chainActive.Tip()->GetBlockTime() <
        (GetTime() - MAX_FEE_ESTIMATION_TIP_AGE)) {
        return false;
    }
    if (chainActive.Height() < pindexBestHeader->nHeight - 1) {
        return false;
    }
    return true;
}

static bool IsCDHFenabled(const Config &config, int nHeight) {
    return nHeight >= config.GetChainParams().GetConsensus().cdyHeight;
}

bool IsCDHFenabled(const Config &config, const CBlockIndex *pindexPrev) {
    if (pindexPrev == nullptr) {
        return false;
    }

    return IsCDHFenabled(config, pindexPrev->nHeight);
}

static bool IsUAHFenabled(const Config &config, int nHeight) {
    return nHeight >= config.GetChainParams().GetConsensus().uahfHeight;
}

bool IsUAHFenabled(const Config &config, const CBlockIndex *pindexPrev) {
    if (pindexPrev == nullptr) {
        return false;
    }

    return IsUAHFenabled(config, pindexPrev->nHeight);
}

static bool IsDAAEnabled(const Config &config, int nHeight) {
    return nHeight >= config.GetChainParams().GetConsensus().daaHeight;
}

bool IsDAAEnabled(const Config &config, const CBlockIndex *pindexPrev) {
    if (pindexPrev == nullptr) {
        return false;
    }

    return IsDAAEnabled(config, pindexPrev->nHeight);
}

static bool IsMonolithEnabled(const Config &config, int64_t nMedianTimePast) {
    return nMedianTimePast >=
             config.GetChainParams().GetConsensus().monolithActivationTime;
}

bool IsMonolithEnabled(const Config &config, const CBlockIndex *pindexPrev) {
    if (pindexPrev == nullptr) {
        return false;
    }

    return IsMonolithEnabled(config, pindexPrev->GetMedianTimePast());
}


// Used to avoid mempool polluting consensus critical paths if CCoinsViewMempool
// were somehow broken and returning the wrong scriptPubKeys
static bool CheckInputsFromMempoolAndCache(const CTransaction &tx,
                                           CValidationState &state,
                                           const CCoinsViewCache &view,
                                           CTxMemPool &pool, uint32_t flags,
                                           bool cacheSigStore,
                                           PrecomputedTransactionData &txdata) {
    AssertLockHeld(cs_main);

    // pool.cs should be locked already, but go ahead and re-take the lock here
    // to enforce that mempool doesn't change between when we check the view and
    // when we actually call through to CheckInputs
    LOCK(pool.cs);

    assert(!tx.IsCoinBase());
    for (const CTxIn &txin : tx.vin) {
        const Coin &coin = view.AccessCoin(txin.prevout);

        // At this point we haven't actually checked if the coins are all
        // available (or shouldn't assume we have, since CheckInputs does). So
        // we just return failure if the inputs are not available here, and then
        // only have to check equivalence for available inputs.
        if (coin.IsSpent()) {
            return false;
        }

        const CTransactionRef &txFrom = pool.get(txin.prevout.hash);
        if (txFrom) {
            assert(txFrom->GetHash() == txin.prevout.hash);
            assert(txFrom->vout.size() > txin.prevout.n);
            assert(txFrom->vout[txin.prevout.n] == coin.GetTxOut());
        } else {
            const Coin &coinFromDisk = pcoinsTip->AccessCoin(txin.prevout);
            assert(!coinFromDisk.IsSpent());
            assert(coinFromDisk.GetTxOut() == coin.GetTxOut());
        }
    }

    return CheckInputs(tx, state, view, true, flags, cacheSigStore, true,
                       txdata);
}

static bool AcceptToMemoryPoolWorker(
    const Config &config, CTxMemPool &pool, CValidationState &state,
    const CTransactionRef &ptx, bool fLimitFree, bool *pfMissingInputs,
    int64_t nAcceptTime, bool fOverrideMempoolLimit, const Amount nAbsurdFee,
    std::vector<COutPoint> &coins_to_uncache) {
    AssertLockHeld(cs_main);

    const CTransaction &tx = *ptx;
    const uint256 txid = tx.GetId();
    if (pfMissingInputs) {
        *pfMissingInputs = false;
    }

    // Coinbase is only valid in a block, not as a loose transaction.
    if (!CheckRegularTransaction(tx, state)) {
        // state filled in by CheckRegularTransaction.
        return false;
    }

    // After the May, 15 hard fork, we start accepting larger op_return.
    const bool hasMonolith = IsMonolithEnabled(config, chainActive.Tip());

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    std::string reason;
    if (fRequireStandard && !IsStandardTx(tx, reason, hasMonolith)) {
        return state.DoS(0, false, REJECT_NONSTANDARD, reason);
    }

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    CValidationState ctxState;
    if (!ContextualCheckTransactionForCurrentBlock(
            config, tx, ctxState, STANDARD_LOCKTIME_VERIFY_FLAGS)) {
        // We copy the state from a dummy to ensure we don't increase the
        // ban score of peer for transaction that could be valid in the future.
        return state.DoS(
            0, false, REJECT_NONSTANDARD, ctxState.GetRejectReason(),
            ctxState.CorruptionPossible(), ctxState.GetDebugMessage());
    }

    // Is it already in the memory pool?
    if (pool.exists(txid)) {
        return state.Invalid(false, REJECT_ALREADY_KNOWN,
                             "txn-already-in-mempool");
    }

    // Check for conflicts with in-memory transactions
    {
        // Protect pool.mapNextTx
        LOCK(pool.cs);
        for (const CTxIn &txin : tx.vin) {
            auto itConflicting = pool.mapNextTx.find(txin.prevout);
            if (itConflicting != pool.mapNextTx.end()) {
                // Disable replacement feature for good
                return state.Invalid(false, REJECT_CONFLICT,
                                     "txn-mempool-conflict");
            }
        }
    }

    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        Amount nValueIn(0);
        LockPoints lp;
        {
            LOCK(pool.cs);
            CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
            view.SetBackend(viewMemPool);

            // Do we already have it?
            for (size_t out = 0; out < tx.vout.size(); out++) {
                COutPoint outpoint(txid, out);
                bool had_coin_in_cache = pcoinsTip->HaveCoinInCache(outpoint);
                if (view.HaveCoin(outpoint)) {
                    if (!had_coin_in_cache) {
                        coins_to_uncache.push_back(outpoint);
                    }

                    return state.Invalid(false, REJECT_ALREADY_KNOWN,
                                         "txn-already-known");
                }
            }

            // Do all inputs exist?
            for (const CTxIn txin : tx.vin) {
                if (!pcoinsTip->HaveCoinInCache(txin.prevout)) {
                    coins_to_uncache.push_back(txin.prevout);
                }

                if (!view.HaveCoin(txin.prevout)) {
                    if (pfMissingInputs) {
                        *pfMissingInputs = true;
                    }

                    // fMissingInputs and !state.IsInvalid() is used to detect
                    // this condition, don't set state.Invalid()
                    return false;
                }
            }

            // Are the actual inputs available?
            if (!view.HaveInputs(tx)) {
                return state.Invalid(false, REJECT_DUPLICATE,
                                     "bad-txns-inputs-spent");
            }

            // Bring the best block into scope.
            view.GetBestBlock();

            nValueIn = view.GetValueIn(tx);

            // We have all inputs cached now, so switch back to dummy, so we
            // don't need to keep lock on mempool.
            view.SetBackend(dummy);

            // Only accept BIP68 sequence locked transactions that can be mined
            // in the next block; we don't want our mempool filled up with
            // transactions that can't be mined yet. Must keep pool.cs for this
            // unless we change CheckSequenceLocks to take a CoinsViewCache
            // instead of create its own.
            if (!CheckSequenceLocks(tx, STANDARD_LOCKTIME_VERIFY_FLAGS, &lp)) {
                return state.DoS(0, false, REJECT_NONSTANDARD,
                                 "non-BIP68-final");
            }
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (fRequireStandard && !AreInputsStandard(tx, view)) {
            return state.Invalid(false, REJECT_NONSTANDARD,
                                 "bad-txns-nonstandard-inputs");
        }

        int64_t nSigOpsCount =
            GetTransactionSigOpCount(tx, view, STANDARD_SCRIPT_VERIFY_FLAGS);

        Amount nValueOut = tx.GetValueOut();
        Amount nFees = nValueIn - nValueOut;
        // nModifiedFees includes any fee deltas from PrioritiseTransaction
        Amount nModifiedFees = nFees;
        double nPriorityDummy = 0;
        pool.ApplyDeltas(txid, nPriorityDummy, nModifiedFees);

        Amount inChainInputValue;
        double dPriority =
            view.GetPriority(tx, chainActive.Height(), inChainInputValue);

        // Keep track of transactions that spend a coinbase, which we re-scan
        // during reorgs to ensure COINBASE_MATURITY is still met.
        bool fSpendsCoinbase = false;
        for (const CTxIn &txin : tx.vin) {
            const Coin &coin = view.AccessCoin(txin.prevout);
            if (coin.IsCoinBase()) {
                fSpendsCoinbase = true;
                break;
            }
        }

        CTxMemPoolEntry entry(ptx, nFees, nAcceptTime, dPriority,
                              chainActive.Height(), inChainInputValue,
                              fSpendsCoinbase, nSigOpsCount, lp);
        unsigned int nSize = entry.GetTxSize();

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_STANDARD_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS_PER_MB; we still consider this an invalid rather
        // than merely non-standard transaction.
        if (nSigOpsCount > MAX_STANDARD_TX_SIGOPS) {
            return state.DoS(0, false, REJECT_NONSTANDARD,
                             "bad-txns-too-many-sigops", false,
                             strprintf("%d", nSigOpsCount));
        }

        Amount mempoolRejectFee =
            pool.GetMinFee(GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) *
                           1000000)
                .GetFee(nSize);
        if (mempoolRejectFee > Amount(0) && nModifiedFees < mempoolRejectFee) {
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE,
                             "mempool min fee not met", false,
                             strprintf("%d < %d", nFees, mempoolRejectFee));
        }

        if (GetBoolArg("-relaypriority", DEFAULT_RELAYPRIORITY) &&
            nModifiedFees < ::minRelayTxFee.GetFee(nSize) &&
            !AllowFree(entry.GetPriority(chainActive.Height() + 1))) {
            // Require that free transactions have sufficient priority to be
            // mined in the next block.
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE,
                             "insufficient priority");
        }

        // Continuously rate-limit free (really, very-low-fee) transactions.
        // This mitigates 'penny-flooding' -- sending thousands of free
        // transactions just to be annoying or make others' transactions take
        // longer to confirm.
        if (fLimitFree && nModifiedFees < ::minRelayTxFee.GetFee(nSize)) {
            static CCriticalSection csFreeLimiter;
            static double dFreeCount;
            static int64_t nLastTime;
            int64_t nNow = GetTime();

            LOCK(csFreeLimiter);

            // Use an exponentially decaying ~10-minute window:
            dFreeCount *= pow(1.0 - 1.0 / 600.0, double(nNow - nLastTime));
            nLastTime = nNow;
            // -limitfreerelay unit is thousand-bytes-per-minute
            // At default rate it would take over a month to fill 1GB
            if (dFreeCount + nSize >=
                GetArg("-limitfreerelay", DEFAULT_LIMITFREERELAY) * 10 * 1000) {
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE,
                                 "rate limited free transaction");
            }

            LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount,
                     dFreeCount + nSize);
            dFreeCount += nSize;
        }

        if (nAbsurdFee != Amount(0) && nFees > nAbsurdFee) {
            return state.Invalid(false, REJECT_HIGHFEE, "absurdly-high-fee",
                                 strprintf("%d > %d", nFees, nAbsurdFee));
        }

        // Calculate in-mempool ancestors, up to a limit.
        CTxMemPool::setEntries setAncestors;
        size_t nLimitAncestors =
            GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT);
        size_t nLimitAncestorSize =
            GetArg("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT) * 1000;
        size_t nLimitDescendants =
            GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT);
        size_t nLimitDescendantSize =
            GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT) *
            1000;
        std::string errString;
        if (!pool.CalculateMemPoolAncestors(
                entry, setAncestors, nLimitAncestors, nLimitAncestorSize,
                nLimitDescendants, nLimitDescendantSize, errString)) {
            return state.DoS(0, false, REJECT_NONSTANDARD,
                             "too-long-mempool-chain", false, errString);
        }

        // Set extraFlags as a set of flags that needs to be activated.
        uint32_t extraFlags = SCRIPT_VERIFY_NONE;
        if (hasMonolith) {
            extraFlags |= SCRIPT_ENABLE_MONOLITH_OPCODES;
        }

        // Check inputs based on the set of flags we activate.
        uint32_t scriptVerifyFlags = STANDARD_SCRIPT_VERIFY_FLAGS;
        if (!Params().RequireStandard()) {
            scriptVerifyFlags =
                GetArg("-promiscuousmempoolflags", scriptVerifyFlags);
        }

        // Make sure whatever we need to activate is actually activated.
        scriptVerifyFlags |= extraFlags;

        // Check against previous transactions. This is done last to help
        // prevent CPU exhaustion denial-of-service attacks.
        PrecomputedTransactionData txdata(tx);
        if (!CheckInputs(tx, state, view, true, scriptVerifyFlags, true, false,
                         txdata)) {
            // State filled in by CheckInputs.
            return false;
        }

        // Check again against the current block tip's script verification flags
        // to cache our script execution flags. This is, of course, useless if
        // the next block has different script flags from the previous one, but
        // because the cache tracks script flags for us it will auto-invalidate
        // and we'll just have a few blocks of extra misses on soft-fork
        // activation.
        //
        // This is also useful in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain CHECKSIG
        // NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks (using TestBlockValidity), however allowing such
        // transactions into the mempool can be exploited as a DoS attack.
        uint32_t currentBlockScriptVerifyFlags =
            GetBlockScriptFlags(config, chainActive.Tip());

        if (!CheckInputsFromMempoolAndCache(tx, state, view, pool,
                                            currentBlockScriptVerifyFlags, true,
                                            txdata)) {
            // If we're using promiscuousmempoolflags, we may hit this normally.
            // Check if current block has some flags that scriptVerifyFlags does
            // not before printing an ominous warning.
            if (!(~scriptVerifyFlags & currentBlockScriptVerifyFlags)) {
                return error(
                    "%s: BUG! PLEASE REPORT THIS! ConnectInputs failed against "
                    "MANDATORY but not STANDARD flags %s, %s",
                    __func__, txid.ToString(), FormatStateMessage(state));
            }

            if (!CheckInputs(tx, state, view, true,
                             MANDATORY_SCRIPT_VERIFY_FLAGS | extraFlags, true, false,
                             txdata)) {
                return error(
                    "%s: ConnectInputs failed against MANDATORY but not "
                    "STANDARD flags due to promiscuous mempool %s, %s",
                    __func__, txid.ToString(), FormatStateMessage(state));
            }

            LogPrintf("Warning: -promiscuousmempool flags set to not include "
                      "currently enforced soft forks, this may break mining or "
                      "otherwise cause instability!\n");
        }

        // This transaction should only count for fee estimation if
        // the node is not behind and it is not dependent on any other
        // transactions in the mempool.
        bool validForFeeEstimation =
            IsCurrentForFeeEstimation() && pool.HasNoInputsOf(tx);

        // Store transaction in memory.
        pool.addUnchecked(txid, entry, setAncestors, validForFeeEstimation);

        // Trim mempool and check if tx was trimmed.
        if (!fOverrideMempoolLimit) {
            LimitMempoolSize(
                pool, GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000,
                GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
            if (!pool.exists(txid)) {
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE,
                                 "mempool full");
            }
        }
    }

    GetMainSignals().SyncTransaction(
        tx, nullptr, CMainSignals::SYNC_TRANSACTION_NOT_IN_BLOCK);

    return true;
}

/**
 * (try to) add transaction to memory pool with a specified acceptance time.
 */
static bool AcceptToMemoryPoolWithTime(const Config &config, CTxMemPool &pool,
                                       CValidationState &state,
                                       const CTransactionRef &tx,
                                       bool fLimitFree, bool *pfMissingInputs,
                                       int64_t nAcceptTime,
                                       bool fOverrideMempoolLimit = false,
                                       const Amount nAbsurdFee = Amount(0)) {
    std::vector<COutPoint> coins_to_uncache;
    bool res = AcceptToMemoryPoolWorker(
        config, pool, state, tx, fLimitFree, pfMissingInputs, nAcceptTime,
        fOverrideMempoolLimit, nAbsurdFee, coins_to_uncache);
    if (!res) {
        for (const COutPoint &outpoint : coins_to_uncache) {
            pcoinsTip->Uncache(outpoint);
        }
    }

    // After we've (potentially) uncached entries, ensure our coins cache is
    // still within its size limits
    CValidationState stateDummy;
    FlushStateToDisk(stateDummy, FLUSH_STATE_PERIODIC);
    return res;
}

bool AcceptToMemoryPool(const Config &config, CTxMemPool &pool,
                        CValidationState &state, const CTransactionRef &tx,
                        bool fLimitFree, bool *pfMissingInputs,
                        bool fOverrideMempoolLimit, const Amount nAbsurdFee) {
    return AcceptToMemoryPoolWithTime(config, pool, state, tx, fLimitFree,
                                      pfMissingInputs, GetTime(),
                                      fOverrideMempoolLimit, nAbsurdFee);
}

/** Return transaction in txOut, and if it was found inside a block, its hash is
 * placed in hashBlock */
bool GetTransaction(const Config &config, const uint256 &txid,
                    CTransactionRef &txOut, uint256 &hashBlock,
                    bool fAllowSlow) {
    CBlockIndex *pindexSlow = nullptr;

    LOCK(cs_main);

    CTransactionRef ptx = mempool.get(txid);
    if (ptx) {
        txOut = ptx;
        return true;
    }

    if (fTxIndex) {
        CDiskTxPos postx;
        if (pblocktree->ReadTxIndex(txid, postx)) {
            CAutoFile file(OpenBlockFile(postx, true), SER_DISK,
                           CLIENT_VERSION);
            if (file.IsNull())
                return error("%s: OpenBlockFile failed", __func__);
            CBlockHeader header;
            try {
                file >> header;
                fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
                file >> txOut;
            } catch (const std::exception &e) {
                return error("%s: Deserialize or I/O error - %s", __func__,
                             e.what());
            }
            hashBlock = header.GetHash();
            if (txOut->GetId() != txid)
                return error("%s: txid mismatch", __func__);
            return true;
        }
    }

    // use coin database to locate block that contains transaction, and scan it
    if (fAllowSlow) {
        const Coin &coin = AccessByTxid(*pcoinsTip, txid);
        if (!coin.IsSpent()) {
            pindexSlow = chainActive[coin.GetHeight()];
        }
    }

    if (pindexSlow) {
        CBlock block;
        if (ReadBlockFromDisk(block, pindexSlow, config)) {
            for (const auto &tx : block.vtx) {
                if (tx->GetId() == txid) {
                    txOut = tx;
                    hashBlock = pindexSlow->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}

//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

bool WriteBlockToDisk(const CBlock &block, CDiskBlockPos &pos,
                      const CMessageHeader::MessageMagic &messageStart) {
    // Open history file to append
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull()) {
        return error("WriteBlockToDisk: OpenBlockFile failed");
    }

    // Write index header
    unsigned int nSize = GetSerializeSize(fileout, block);
    fileout << FLATDATA(messageStart) << nSize;

    // Write block
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0) {
        return error("WriteBlockToDisk: ftell failed");
    }

    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool ReadBlockFromDisk(CBlock &block, const CDiskBlockPos &pos,
                       const Config &config) {
    block.SetNull();

    // Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        return error("ReadBlockFromDisk: OpenBlockFile failed for %s",
                     pos.ToString());
    }

    // Read block
    try {
        filein >> block;
    } catch (const std::exception &e) {
        return error("%s: Deserialize or I/O error - %s at %s", __func__,
                     e.what(), pos.ToString());
    }

    const Consensus::Params& consensusParams = Params().GetConsensus();
    // Check Equihash solution
    bool postfork = block.nHeight >= (uint32_t)consensusParams.cdyHeight;
    if (postfork && !CheckEquihashSolution(&block, Params())) {
        return error("ReadBlockFromDisk: Errors in block header at %s (bad Equihash solution)", pos.ToString());
    }
    // Check the header
    if (!CheckProofOfWork(block.GetHash(), block.nBits, postfork, config))
        return error("ReadBlockFromDisk: Errors in block header at %s", pos.ToString());

    return true;
}

bool ReadBlockFromDisk(CBlock &block, const CBlockIndex *pindex,
                       const Config &config) {
    if (!ReadBlockFromDisk(block, pindex->GetBlockPos(), config)) {
        return false;
    }

    if (block.GetHash() != pindex->GetBlockHash()) {
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*): GetHash() "
                     "doesn't match index for %s at %s",
                     pindex->ToString(), pindex->GetBlockPos().ToString());
    }

    return true;
}

Amount GetBlockSubsidy(int nHeight, const Consensus::Params &consensusParams) {
    int halvings;
    if(nHeight>=consensusParams.cdyHeight) {
        halvings = (consensusParams.cdyHeight + (nHeight - consensusParams.cdyHeight)/5) /  consensusParams.nSubsidyHalvingInterval;
    }
    else halvings = nHeight / consensusParams.nSubsidyHalvingInterval;


    if (nHeight == consensusParams.cdyHeight) {
		return 210000 * COIN;// * COIN_SCALE;
    }
    
    if (nHeight == consensusParams.nCompenseHeight) {
        return 1000000 * COIN;
    }
    // Force block reward to zero when right shift is undefined. 
    if (halvings >= 61) return Amount(0);  //change it from 64 to 61

    Amount nSubsidy = 50 * COIN;// * COIN_SCALE;
    
    if(nHeight>=consensusParams.cdyHeight) {
        nSubsidy = 10 * COIN;// * COIN_SCALE;
    }  
    //if(IsMonolithEnabled(GetConfig(), chainActive.Tip())) 
    //{
    //    nSubsidy = COIN;
    //}
    // Subsidy is cut in half every 210,000 blocks which will occur
    // approximately every 4 years.
    return Amount(nSubsidy.GetSatoshis() >> halvings);
}

bool IsInitialBlockDownload() {
    const CChainParams &chainParams = Params();

    // Once this function has returned false, it must remain false.
    static std::atomic<bool> latchToFalse{false};
    // Optimization: pre-test latch before taking the lock.
    if (latchToFalse.load(std::memory_order_relaxed)) return false;

    LOCK(cs_main);
    if (latchToFalse.load(std::memory_order_relaxed)) return false;
    if (fImporting || fReindex) return true;
    if (chainActive.Tip() == nullptr) return true;
    if (chainActive.Tip()->nChainWork <
        UintToArith256(chainParams.GetConsensus().nMinimumChainWork))
        return true;
    if (fSkipHardforkIBD && chainActive.Tip()->nHeight + 1 >= (int)chainParams.GetConsensus().cdyHeight)
        return false;
    int64_t target_time = fCDYBootstrapping ? (int64_t)chainParams.GetConsensus().BitcoinPostforkTime : GetTime();
    if (chainActive.Tip()->GetBlockTime() < (target_time - nMaxTipAge))
        return true;
    LogPrintf("Leaving InitialBlockDownload (latching to false)\n");
    latchToFalse.store(true, std::memory_order_relaxed);
    return false;
}

CBlockIndex *pindexBestForkTip = nullptr, *pindexBestForkBase = nullptr;

static void AlertNotify(const std::string &strMessage) {
    uiInterface.NotifyAlertChanged();
    std::string strCmd = GetArg("-alertnotify", "");
    if (strCmd.empty()) return;

    // Alert text should be plain ascii coming from a trusted source, but to be
    // safe we first strip anything not in safeChars, then add single quotes
    // around the whole string before passing it to the shell:
    std::string singleQuote("'");
    std::string safeStatus = SanitizeString(strMessage);
    safeStatus = singleQuote + safeStatus + singleQuote;
    boost::replace_all(strCmd, "%s", safeStatus);

    boost::thread t(runCommand, strCmd); // thread runs free
}

void CheckForkWarningConditions() {
    AssertLockHeld(cs_main);
    // Before we get past initial download, we cannot reliably alert about forks
    // (we assume we don't get stuck on a fork before finishing our initial
    // sync)
    if (IsInitialBlockDownload()) return;

    // If our best fork is no longer within 72 blocks (+/- 12 hours if no one
    // mines it) of our head, drop it
    if (pindexBestForkTip &&
        chainActive.Height() - pindexBestForkTip->nHeight >= 72)
        pindexBestForkTip = nullptr;

    if (pindexBestForkTip ||
        (pindexBestInvalid &&
         pindexBestInvalid->nChainWork >
             chainActive.Tip()->nChainWork +
                 (GetBlockProof(*chainActive.Tip()) * 6))) {
        if (!GetfLargeWorkForkFound() && pindexBestForkBase) {
            std::string warning =
                std::string("'Warning: Large-work fork detected, forking after "
                            "block ") +
                pindexBestForkBase->phashBlock->ToString() + std::string("'");
            AlertNotify(warning);
        }
        if (pindexBestForkTip && pindexBestForkBase) {
            LogPrintf("%s: Warning: Large valid fork found\n  forking the "
                      "chain at height %d (%s)\n  lasting to height %d "
                      "(%s).\nChain state database corruption likely.\n",
                      __func__, pindexBestForkBase->nHeight,
                      pindexBestForkBase->phashBlock->ToString(),
                      pindexBestForkTip->nHeight,
                      pindexBestForkTip->phashBlock->ToString());
            SetfLargeWorkForkFound(true);
        } else {
            LogPrintf("%s: Warning: Found invalid chain at least ~6 blocks "
                      "longer than our best chain.\nChain state database "
                      "corruption likely.\n",
                      __func__);
            SetfLargeWorkInvalidChainFound(true);
        }
    } else {
        SetfLargeWorkForkFound(false);
        SetfLargeWorkInvalidChainFound(false);
    }
}

void CheckForkWarningConditionsOnNewFork(CBlockIndex *pindexNewForkTip) {
    AssertLockHeld(cs_main);
    // If we are on a fork that is sufficiently large, set a warning flag
    CBlockIndex *pfork = pindexNewForkTip;
    CBlockIndex *plonger = chainActive.Tip();
    while (pfork && pfork != plonger) {
        while (plonger && plonger->nHeight > pfork->nHeight)
            plonger = plonger->pprev;
        if (pfork == plonger) break;
        pfork = pfork->pprev;
    }

    // We define a condition where we should warn the user about as a fork of at
    // least 7 blocks with a tip within 72 blocks (+/- 12 hours if no one mines
    // it) of ours. We use 7 blocks rather arbitrarily as it represents just
    // under 10% of sustained network hash rate operating on the fork, or a
    // chain that is entirely longer than ours and invalid (note that this
    // should be detected by both). We define it this way because it allows us
    // to only store the highest fork tip (+ base) which meets the 7-block
    // condition and from this always have the most-likely-to-cause-warning fork
    if (pfork && (!pindexBestForkTip ||
                  (pindexBestForkTip &&
                   pindexNewForkTip->nHeight > pindexBestForkTip->nHeight)) &&
        pindexNewForkTip->nChainWork - pfork->nChainWork >
            (GetBlockProof(*pfork) * 7) &&
        chainActive.Height() - pindexNewForkTip->nHeight < 72) {
        pindexBestForkTip = pindexNewForkTip;
        pindexBestForkBase = pfork;
    }

    CheckForkWarningConditions();
}

static void InvalidChainFound(CBlockIndex *pindexNew) {
    if (!pindexBestInvalid ||
        pindexNew->nChainWork > pindexBestInvalid->nChainWork)
        pindexBestInvalid = pindexNew;

    LogPrintf(
        "%s: invalid block=%s  height=%d  log2_work=%.8g  date=%s\n", __func__,
        pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
        log(pindexNew->nChainWork.getdouble()) / log(2.0),
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexNew->GetBlockTime()));
    CBlockIndex *tip = chainActive.Tip();
    assert(tip);
    LogPrintf("%s:  current best=%s  height=%d  log2_work=%.8g  date=%s\n",
              __func__, tip->GetBlockHash().ToString(), chainActive.Height(),
              log(tip->nChainWork.getdouble()) / log(2.0),
              DateTimeStrFormat("%Y-%m-%d %H:%M:%S", tip->GetBlockTime()));
    CheckForkWarningConditions();
}

static void InvalidBlockFound(CBlockIndex *pindex,
                              const CValidationState &state) {
    if (!state.CorruptionPossible()) {
        pindex->nStatus = pindex->nStatus.withFailed();
        setDirtyBlockIndex.insert(pindex);
        setBlockIndexCandidates.erase(pindex);
        InvalidChainFound(pindex);
    }
}

void UpdateCoins(const CTransaction &tx, CCoinsViewCache &inputs,
                 CTxUndo &txundo, int nHeight) {
    // Mark inputs spent.
    if (!tx.IsCoinBase()) {
        txundo.vprevout.reserve(tx.vin.size());
        for (const CTxIn &txin : tx.vin) {
            txundo.vprevout.emplace_back();
            bool is_spent =
                inputs.SpendCoin(txin.prevout, &txundo.vprevout.back());
            assert(is_spent);
        }
    }

    // Add outputs.
    AddCoins(inputs, tx, nHeight);
}

void UpdateCoins(const CTransaction &tx, CCoinsViewCache &inputs, int nHeight) {
    CTxUndo txundo;
    UpdateCoins(tx, inputs, txundo, nHeight);
}

bool CScriptCheck::operator()() {
    const CScript &scriptSig = ptxTo->vin[nIn].scriptSig;
    if (!VerifyScript(scriptSig, scriptPubKey, nFlags,
                      CachingTransactionSignatureChecker(ptxTo, nIn, amount,
                                                         cacheStore, txdata),
                      &error)) {
        return false;
    }
    return true;
}

int GetSpendHeight(const CCoinsViewCache &inputs) {
    LOCK(cs_main);
    CBlockIndex *pindexPrev = mapBlockIndex.find(inputs.GetBestBlock())->second;
    return pindexPrev->nHeight + 1;
}

namespace Consensus {
bool CheckTxInputs(const CTransaction &tx, CValidationState &state,
                   const CCoinsViewCache &inputs, int nSpendHeight) {
    // This doesn't trigger the DoS code on purpose; if it did, it would make it
    // easier for an attacker to attempt to split the network.
    if (!inputs.HaveInputs(tx)) {
        return state.Invalid(false, 0, "", "Inputs unavailable");
    }

    Amount nValueIn(0);
    Amount nFees(0);
    for (size_t i = 0; i < tx.vin.size(); i++) {
        const COutPoint &prevout = tx.vin[i].prevout;
        const Coin &coin = inputs.AccessCoin(prevout);
        assert(!coin.IsSpent());

        // If prev is coinbase, check that it's matured
        if (coin.IsCoinBase()) {
            if (nSpendHeight - coin.GetHeight() < COINBASE_MATURITY) {
                return state.Invalid(
                    false, REJECT_INVALID,
                    "bad-txns-premature-spend-of-coinbase",
                    strprintf("tried to spend coinbase at depth %d",
                              nSpendHeight - coin.GetHeight()));
            }
        }

        // Check for negative or overflow input values
        nValueIn += coin.GetTxOut().nValue;
        if (!MoneyRange(coin.GetTxOut().nValue) || !MoneyRange(nValueIn)) {
            return state.DoS(100, false, REJECT_INVALID,
                             "bad-txns-inputvalues-outofrange");
        }
    }

    if (nValueIn < tx.GetValueOut()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-in-belowout",
                         false, strprintf("value in (%s) < value out (%s)",
                                          FormatMoney(nValueIn),
                                          FormatMoney(tx.GetValueOut())));
    }

    // Tally transaction fees
    Amount nTxFee = nValueIn - tx.GetValueOut();
    if (nTxFee < Amount(0)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-negative");
    }
    nFees += nTxFee;
    if (!MoneyRange(nFees)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-outofrange");
    }

    return true;
}
} // namespace Consensus

bool CheckInputs(const CTransaction &tx, CValidationState &state,
                 const CCoinsViewCache &inputs, bool fScriptChecks,
                 const uint32_t flags, bool sigCacheStore,
                 bool scriptCacheStore,
                 const PrecomputedTransactionData &txdata,
                 std::vector<CScriptCheck> *pvChecks) {
    assert(!tx.IsCoinBase());

    if (!Consensus::CheckTxInputs(tx, state, inputs, GetSpendHeight(inputs))) {
        return false;
    }

    if (pvChecks) {
        pvChecks->reserve(tx.vin.size());
    }

    // The first loop above does all the inexpensive checks. Only if ALL inputs
    // pass do we perform expensive ECDSA signature checks. Helps prevent CPU
    // exhaustion attacks.

    // Skip script verification when connecting blocks under the assumedvalid
    // block. Assuming the assumedvalid block is valid this is safe because
    // block merkle hashes are still computed and checked, of course, if an
    // assumed valid block is invalid due to false scriptSigs this optimization
    // would allow an invalid chain to be accepted.
    if (!fScriptChecks) {
        return true;
    }

    // First check if script executions have been cached with the same flags.
    // Note that this assumes that the inputs provided are correct (ie that the
    // transaction hash which is in tx's prevouts properly commits to the
    // scriptPubKey in the inputs view of that transaction).
    uint256 hashCacheEntry = GetScriptCacheKey(tx, flags);
    if (IsKeyInScriptCache(hashCacheEntry, !scriptCacheStore)) {
        return true;
    }

    for (size_t i = 0; i < tx.vin.size(); i++) {
        const COutPoint &prevout = tx.vin[i].prevout;
        const Coin &coin = inputs.AccessCoin(prevout);
        assert(!coin.IsSpent());

        // We very carefully only pass in things to CScriptCheck which are
        // clearly committed to by tx' witness hash. This provides a sanity
        // check that our caching is not introducing consensus failures through
        // additional data in, eg, the coins being spent being checked as a part
        // of CScriptCheck.
        const CScript &scriptPubKey = coin.GetTxOut().scriptPubKey;
        const Amount amount = coin.GetTxOut().nValue;

        // Verify signature
        CScriptCheck check(scriptPubKey, amount, tx, i, flags, sigCacheStore,
                           txdata);
        if (pvChecks) {
            pvChecks->push_back(std::move(check));
        } else if (!check()) {
            const bool hasNonMandatoryFlags =
                (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) != 0;
            const bool doesNotHaveMonolith =
                (flags & SCRIPT_ENABLE_MONOLITH_OPCODES) == 0;
            if (hasNonMandatoryFlags || doesNotHaveMonolith) {
                // Check whether the failure was caused by a non-mandatory
                // script verification check, such as non-standard DER encodings
                // or non-null dummy arguments; if so, don't trigger DoS
                // protection to avoid splitting the network between upgraded
                // and non-upgraded nodes.
                //
                // We also check activating the monolith opcodes as it is a
                // strictly additive change and we would not like to ban some of
                // our peer that are ahead of us and are considering the fork
                // as activated.
                CScriptCheck check2(
                    scriptPubKey, amount, tx, i,
                    (flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS) |
                        SCRIPT_ENABLE_MONOLITH_OPCODES,
                                    sigCacheStore, txdata);
                if (check2()) {
                    return state.Invalid(
                        false, REJECT_NONSTANDARD,
                        strprintf("non-mandatory-script-verify-flag (%s)",
                                  ScriptErrorString(check.GetScriptError())));
                }
            }

            // Failures of other flags indicate a transaction that is invalid in
            // new blocks, e.g. a invalid P2SH. We DoS ban such nodes as they
            // are not following the protocol. That said during an upgrade
            // careful thought should be taken as to the correct behavior - we
            // may want to continue peering with non-upgraded nodes even after
            // soft-fork super-majority signaling has occurred.
            return state.DoS(
                100, false, REJECT_INVALID,
                strprintf("mandatory-script-verify-flag-failed (%s)",
                          ScriptErrorString(check.GetScriptError())));
        }
    }

    if (scriptCacheStore && !pvChecks) {
        // We executed all of the provided scripts, and were told to cache the
        // result. Do so now.
        AddKeyInScriptCache(hashCacheEntry);
    }

    return true;
}

namespace {

bool UndoWriteToDisk(const CBlockUndo &blockundo, CDiskBlockPos &pos,
                     const uint256 &hashBlock,
                     const CMessageHeader::MessageMagic &messageStart) {
    // Open history file to append
    CAutoFile fileout(OpenUndoFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull()) return error("%s: OpenUndoFile failed", __func__);

    // Write index header
    unsigned int nSize = GetSerializeSize(fileout, blockundo);
    fileout << FLATDATA(messageStart) << nSize;

    // Write undo data
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0) return error("%s: ftell failed", __func__);
    pos.nPos = (unsigned int)fileOutPos;
    fileout << blockundo;

    // calculate & write checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << blockundo;
    fileout << hasher.GetHash();

    return true;
}

bool UndoReadFromDisk(CBlockUndo &blockundo, const CDiskBlockPos &pos,
                      const uint256 &hashBlock) {
    // Open history file to read
    CAutoFile filein(OpenUndoFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        return error("%s: OpenUndoFile failed", __func__);
    }

    // Read block
    uint256 hashChecksum;
    // We need a CHashVerifier as reserializing may lose data
    CHashVerifier<CAutoFile> verifier(&filein);
    try {
        verifier << hashBlock;
        verifier >> blockundo;
        filein >> hashChecksum;
    } catch (const std::exception &e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    // Verify checksum
    if (hashChecksum != verifier.GetHash()) {
        return error("%s: Checksum mismatch", __func__);
    }

    return true;
}

/** Abort with a message */
bool AbortNode(const std::string &strMessage,
               const std::string &userMessage = "") {
    SetMiscWarning(strMessage);
    LogPrintf("*** %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occurred, see "
                                "debug.log for details")
                            : userMessage,
        "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

bool AbortNode(CValidationState &state, const std::string &strMessage,
               const std::string &userMessage = "") {
    AbortNode(strMessage, userMessage);
    return state.Error(strMessage);
}

} // namespace

/** Restore the UTXO in a Coin at a given COutPoint. */
DisconnectResult UndoCoinSpend(const Coin &undo, CCoinsViewCache &view,
                               const COutPoint &out) {
    bool fClean = true;

    if (view.HaveCoin(out)) {
        // Overwriting transaction output.
        fClean = false;
    }

    if (undo.GetHeight() == 0) {
        // Missing undo metadata (height and coinbase). Older versions included
        // this information only in undo records for the last spend of a
        // transactions' outputs. This implies that it must be present for some
        // other output of the same tx.
        const Coin &alternate = AccessByTxid(view, out.hash);
        if (alternate.IsSpent()) {
            // Adding output for transaction without known metadata
            return DISCONNECT_FAILED;
        }

        // This is somewhat ugly, but hopefully utility is limited. This is only
        // useful when working from legacy on disck data. In any case, putting
        // the correct information in there doesn't hurt.
        const_cast<Coin &>(undo) = Coin(undo.GetTxOut(), alternate.GetHeight(),
                                        alternate.IsCoinBase());
    }

    // The potential_overwrite parameter to AddCoin is only allowed to be false
    // if we know for sure that the coin did not already exist in the cache. As
    // we have queried for that above using HaveCoin, we don't need to guess.
    // When fClean is false, a coin already existed and it is an overwrite.
    view.AddCoin(out, std::move(undo), !fClean);

    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}

/**
 * Undo the effects of this block (with given index) on the UTXO set represented
 * by coins. When UNCLEAN or FAILED is returned, view is left in an
 * indeterminate state.
 */
static DisconnectResult DisconnectBlock(const CBlock &block,
                                        const CBlockIndex *pindex,
                                        CCoinsViewCache &view) {
    CBlockUndo blockUndo;
    CDiskBlockPos pos = pindex->GetUndoPos();
    if (pos.IsNull()) {
        error("DisconnectBlock(): no undo data available");
        return DISCONNECT_FAILED;
    }

    if (!UndoReadFromDisk(blockUndo, pos, pindex->pprev->GetBlockHash())) {
        error("DisconnectBlock(): failure reading undo data");
        return DISCONNECT_FAILED;
    }

    return ApplyBlockUndo(blockUndo, block, pindex, view);
}

DisconnectResult ApplyBlockUndo(const CBlockUndo &blockUndo,
                                const CBlock &block, const CBlockIndex *pindex,
                                CCoinsViewCache &view) {
    bool fClean = true;

    if (blockUndo.vtxundo.size() + 1 != block.vtx.size()) {
        error("DisconnectBlock(): block and undo data inconsistent");
        return DISCONNECT_FAILED;
    }

    // Undo transactions in reverse order.
    size_t i = block.vtx.size();
    while (i-- > 0) {
        const CTransaction &tx = *(block.vtx[i]);
        uint256 txid = tx.GetId();

        // Check that all outputs are available and match the outputs in the
        // block itself exactly.
        for (size_t o = 0; o < tx.vout.size(); o++) {
            if (tx.vout[o].scriptPubKey.IsUnspendable()) {
                continue;
            }

            COutPoint out(txid, o);
            Coin coin;
            bool is_spent = view.SpendCoin(out, &coin);
            if (!is_spent || tx.vout[o] != coin.GetTxOut()) {
                // transaction output mismatch
                fClean = false;
            }
        }

        // Restore inputs.
        if (i < 1) {
            // Skip the coinbase.
            continue;
        }

        const CTxUndo &txundo = blockUndo.vtxundo[i - 1];
        if (txundo.vprevout.size() != tx.vin.size()) {
            error("DisconnectBlock(): transaction and undo data inconsistent");
            return DISCONNECT_FAILED;
        }

        for (size_t j = tx.vin.size(); j-- > 0;) {
            const COutPoint &out = tx.vin[j].prevout;
            const Coin &undo = txundo.vprevout[j];
            DisconnectResult res = UndoCoinSpend(undo, view, out);
            if (res == DISCONNECT_FAILED) {
                return DISCONNECT_FAILED;
            }
            fClean = fClean && res != DISCONNECT_UNCLEAN;
        }
    }

    // Move best block pointer to previous block.
    view.SetBestBlock(block.hashPrevBlock);

    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}

static void FlushBlockFile(bool fFinalize = false) {
    LOCK(cs_LastBlockFile);

    CDiskBlockPos posOld(nLastBlockFile, 0);

    FILE *fileOld = OpenBlockFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }

    fileOld = OpenUndoFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nUndoSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos,
                 unsigned int nAddSize);

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);

void ThreadScriptCheck() {
    RenameThread("bitcoin-scriptch");
    scriptcheckqueue.Thread();
}

// Protected by cs_main
VersionBitsCache versionbitscache;

int32_t ComputeBlockVersion(const CBlockIndex *pindexPrev,
                            const Consensus::Params &params) {
    LOCK(cs_main);
    int32_t nVersion = VERSIONBITS_TOP_BITS;

    for (int i = 0; i < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; i++) {
        ThresholdState state = VersionBitsState(
            pindexPrev, params, (Consensus::DeploymentPos)i, versionbitscache);
        if (state == THRESHOLD_LOCKED_IN || state == THRESHOLD_STARTED) {
            nVersion |= VersionBitsMask(params, (Consensus::DeploymentPos)i);
        }
    }

    return nVersion;
}

/**
 * Threshold condition checker that triggers when unknown versionbits are seen
 * on the network.
 */
class WarningBitsConditionChecker : public AbstractThresholdConditionChecker {
private:
    int bit;

public:
    WarningBitsConditionChecker(int bitIn) : bit(bitIn) {}

    int64_t BeginTime(const Consensus::Params &params) const override {
        return 0;
    }
    int64_t EndTime(const Consensus::Params &params) const override {
        return std::numeric_limits<int64_t>::max();
    }
    int Period(const Consensus::Params &params) const override {
        return params.nMinerConfirmationWindow;
    }
    int Threshold(const Consensus::Params &params) const override {
        return params.nRuleChangeActivationThreshold;
    }

    bool Condition(const CBlockIndex *pindex,
                   const Consensus::Params &params) const override {
        return ((pindex->nVersion & VERSIONBITS_TOP_MASK) ==
                VERSIONBITS_TOP_BITS) &&
               ((pindex->nVersion >> bit) & 1) != 0 &&
               ((ComputeBlockVersion(pindex->pprev, params) >> bit) & 1) == 0;
    }
};

// Protected by cs_main
static ThresholdConditionCache warningcache[VERSIONBITS_NUM_BITS];

// Returns the script flags which should be checked for a given block
static uint32_t GetBlockScriptFlags(const Config &config,
                                    const CBlockIndex *pChainTip) {
    AssertLockHeld(cs_main);
    const Consensus::Params &consensusparams =
        config.GetChainParams().GetConsensus();

    // BIP16 didn't become active until Apr 1 2012
    int64_t nBIP16SwitchTime = 1333238400;
    bool fStrictPayToScriptHash = (pChainTip->GetBlockTime() >= nBIP16SwitchTime);

    uint32_t flags =
        fStrictPayToScriptHash ? SCRIPT_VERIFY_P2SH : SCRIPT_VERIFY_NONE;

    // Start enforcing the DERSIG (BIP66) rule
    if ((pChainTip->nHeight + 1) >= consensusparams.BIP66Height) {
        flags |= SCRIPT_VERIFY_DERSIG;
    }

    // Start enforcing CHECKLOCKTIMEVERIFY (BIP65) rule
    if ((pChainTip->nHeight + 1) >= consensusparams.BIP65Height) {
        flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    }

    // Start enforcing BIP112 (CHECKSEQUENCEVERIFY) using versionbits logic.
    if (VersionBitsState(pChainTip, consensusparams, Consensus::DEPLOYMENT_CSV,
                         versionbitscache) == THRESHOLD_ACTIVE) {
        flags |= SCRIPT_VERIFY_CHECKSEQUENCEVERIFY;
    }

    // If the UAHF is enabled, we start accepting replay protected txns
    if(IsCDHFenabled(config, pChainTip)){
        flags |= SCRIPT_VERIFY_STRICTENC;
        flags |= SCRIPT_ENABLE_SIGHASH_FORKID;
        flags |= SCRIPT_ENABLE_CHANGE_FORKID;
    } else if (IsUAHFenabled(config, pChainTip)) {
        flags |= SCRIPT_VERIFY_STRICTENC;
        flags |= SCRIPT_ENABLE_SIGHASH_FORKID;
    }

    // If the DAA HF is enabled, we start rejecting transaction that use a high
    // s in their signature. We also make sure that signature that are supposed
    // to fail (for instance in multisig or other forms of smart contracts) are
    // null.
    if (IsDAAEnabled(config, pChainTip)) {
        flags |= SCRIPT_VERIFY_LOW_S;
        flags |= SCRIPT_VERIFY_NULLFAIL;
    }

    // The monolith HF enable a set of opcodes.
    if (IsMonolithEnabled(config, pChainTip)) {
        flags |= SCRIPT_ENABLE_MONOLITH_OPCODES;
    }

    return flags;
}

static int64_t nTimeCheck = 0;
static int64_t nTimeForks = 0;
static int64_t nTimeVerify = 0;
static int64_t nTimeConnect = 0;
static int64_t nTimeIndex = 0;
static int64_t nTimeCallbacks = 0;
static int64_t nTimeTotal = 0;

/**
 * Apply the effects of this block (with given index) on the UTXO set
 * represented by coins. Validity checks that depend on the UTXO set are also
 * done; ConnectBlock() can fail if those validity checks fail (among other
 * reasons).
 */
static bool ConnectBlock(const Config &config, const CBlock &block,
                         CValidationState &state, CBlockIndex *pindex,
                         CCoinsViewCache &view, const CChainParams &chainparams,
                         bool fJustCheck = false) {
    AssertLockHeld(cs_main);

    int64_t nTimeStart = GetTimeMicros();

    // Check it again in case a previous version let a bad block in
    if (!CheckBlock(config, block, state, !fJustCheck, !fJustCheck)) {
        return error("%s: Consensus::CheckBlock: %s", __func__,
                     FormatStateMessage(state));
    }

    // Verify that the view's current state corresponds to the previous block
    uint256 hashPrevBlock =
        pindex->pprev == nullptr ? uint256() : pindex->pprev->GetBlockHash();
    assert(hashPrevBlock == view.GetBestBlock());

    // Special case for the genesis block, skipping connection of its
    // transactions (its coinbase is unspendable)
    if (block.GetHash() == chainparams.GetConsensus().hashGenesisBlock) {
        if (!fJustCheck) {
            view.SetBestBlock(pindex->GetBlockHash());
        }
        return true;
    }

    bool fScriptChecks = true;
    if (!hashAssumeValid.IsNull()) {
        // We've been configured with the hash of a block which has been
        // externally verified to have a valid history. A suitable default value
        // is included with the software and updated from time to time. Because
        // validity relative to a piece of software is an objective fact these
        // defaults can be easily reviewed. This setting doesn't force the
        // selection of any particular chain but makes validating some faster by
        // effectively caching the result of part of the verification.
        BlockMap::const_iterator it = mapBlockIndex.find(hashAssumeValid);
        if (it != mapBlockIndex.end()) {
            if (it->second->GetAncestor(pindex->nHeight) == pindex &&
                pindexBestHeader->GetAncestor(pindex->nHeight) == pindex &&
                pindexBestHeader->nChainWork >=
                    UintToArith256(
                        chainparams.GetConsensus().nMinimumChainWork)) {
                // This block is a member of the assumed verified chain and an
                // ancestor of the best header. The equivalent time check
                // discourages hashpower from extorting the network via DOS
                // attack into accepting an invalid block through telling users
                // they must manually set assumevalid. Requiring a software
                // change or burying the invalid block, regardless of the
                // setting, makes it hard to hide the implication of the demand.
                // This also avoids having release candidates that are hardly
                // doing any signature verification at all in testing without
                // having to artificially set the default assumed verified block
                // further back. The test against nMinimumChainWork prevents the
                // skipping when denied access to any chain at least as good as
                // the expected chain.
                fScriptChecks =
                    (GetBlockProofEquivalentTime(
                         *pindexBestHeader, *pindex, *pindexBestHeader,
                         chainparams.GetConsensus()) <= 60 * 60 * 24 * 7 * 2);
            }
        }
    }

    int64_t nTime1 = GetTimeMicros();
    nTimeCheck += nTime1 - nTimeStart;
    LogPrint("bench", "    - Sanity checks: %.2fms [%.2fs]\n",
             0.001 * (nTime1 - nTimeStart), nTimeCheck * 0.000001);

    // Do not allow blocks that contain transactions which 'overwrite' older
    // transactions, unless those are already completely spent. If such
    // overwrites are allowed, coinbases and transactions depending upon those
    // can be duplicated to remove the ability to spend the first instance --
    // even after being sent to another address. See BIP30 and
    // http://r6.ca/blog/20120206T005236Z.html for more information. This logic
    // is not necessary for memory pool transactions, as AcceptToMemoryPool
    // already refuses previously-known transaction ids entirely. This rule was
    // originally applied to all blocks with a timestamp after March 15, 2012,
    // 0:00 UTC. Now that the whole chain is irreversibly beyond that time it is
    // applied to all blocks except the two in the chain that violate it. This
    // prevents exploiting the issue against nodes during their initial block
    // download.
    bool fEnforceBIP30 = (!pindex->phashBlock) || // Enforce on CreateNewBlock
                                                  // invocations which don't
                                                  // have a hash.
                         !((pindex->nHeight == 91842 &&
                            pindex->GetBlockHash() ==
                                uint256S("0x00000000000a4d0a398161ffc163c503763"
                                         "b1f4360639393e0e4c8e300e0caec")) ||
                           (pindex->nHeight == 91880 &&
                            pindex->GetBlockHash() ==
                                uint256S("0x00000000000743f190a18c5577a3c2d2a1f"
                                         "610ae9601ac046a38084ccb7cd721")));

    // Once BIP34 activated it was not possible to create new duplicate
    // coinbases and thus other than starting with the 2 existing duplicate
    // coinbase pairs, not possible to create overwriting txs. But by the time
    // BIP34 activated, in each of the existing pairs the duplicate coinbase had
    // overwritten the first before the first had been spent. Since those
    // coinbases are sufficiently buried its no longer possible to create
    // further duplicate transactions descending from the known pairs either. If
    // we're on the known chain at height greater than where BIP34 activated, we
    // can save the db accesses needed for the BIP30 check.
    CBlockIndex *pindexBIP34height =
        pindex->pprev->GetAncestor(chainparams.GetConsensus().BIP34Height);
    // Only continue to enforce if we're below BIP34 activation height or the
    // block hash at that height doesn't correspond.
    fEnforceBIP30 = fEnforceBIP30 && (!pindexBIP34height ||
                                      !(pindexBIP34height->GetBlockHash() ==
                                        chainparams.GetConsensus().BIP34Hash));

    if (fEnforceBIP30) {
        for (const auto &tx : block.vtx) {
            for (size_t o = 0; o < tx->vout.size(); o++) {
                if (view.HaveCoin(COutPoint(tx->GetHash(), o))) {
                    return state.DoS(
                        100,
                        error("ConnectBlock(): tried to overwrite transaction"),
                        REJECT_INVALID, "bad-txns-BIP30");
                }
            }
        }
    }

    // Start enforcing BIP68 (sequence locks) using versionbits logic.
    int nLockTimeFlags = 0;
    if (VersionBitsState(pindex->pprev, chainparams.GetConsensus(),
                         Consensus::DEPLOYMENT_CSV,
                         versionbitscache) == THRESHOLD_ACTIVE) {
        nLockTimeFlags |= LOCKTIME_VERIFY_SEQUENCE;
    }

    const uint32_t flags = GetBlockScriptFlags(config, pindex->pprev);

    int64_t nTime2 = GetTimeMicros();
    nTimeForks += nTime2 - nTime1;
    LogPrint("bench", "    - Fork checks: %.2fms [%.2fs]\n",
             0.001 * (nTime2 - nTime1), nTimeForks * 0.000001);

    CBlockUndo blockundo;

    CCheckQueueControl<CScriptCheck> control(fScriptChecks ? &scriptcheckqueue
                                                           : nullptr);

    std::vector<int> prevheights;
    Amount nFees(0);
    int nInputs = 0;

    // Sigops counting. We need to do it again because of P2SH.
    uint64_t nSigOpsCount = 0;
    const uint64_t currentBlockSize =
        ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    const uint64_t nMaxSigOpsCount = GetMaxBlockSigOpsCount(currentBlockSize);

    CDiskTxPos pos(pindex->GetBlockPos(),
                   GetSizeOfCompactSize(block.vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos>> vPos;
    vPos.reserve(block.vtx.size());
    blockundo.vtxundo.reserve(block.vtx.size() - 1);

    for (size_t i = 0; i < block.vtx.size(); i++) {
        const CTransaction &tx = *(block.vtx[i]);
        nInputs += tx.vin.size();
        if (!tx.IsCoinBase()) {
            if (!view.HaveInputs(tx)) {
                return state.DoS(
                    100, error("ConnectBlock(): inputs missing/spent"),
                    REJECT_INVALID, "bad-txns-inputs-missingorspent");
            }

            // Check that transaction is BIP68 final BIP68 lock checks (as
            // opposed to nLockTime checks) must be in ConnectBlock because they
            // require the UTXO set.
            prevheights.resize(tx.vin.size());
            for (size_t j = 0; j < tx.vin.size(); j++) {
                prevheights[j] = view.AccessCoin(tx.vin[j].prevout).GetHeight();
            }

            if (!SequenceLocks(tx, nLockTimeFlags, &prevheights, *pindex)) {
                return state.DoS(
                    100, error("%s: contains a non-BIP68-final transaction",
                               __func__),
                    REJECT_INVALID, "bad-txns-nonfinal");
            }
        }

        // GetTransactionSigOpCount counts 2 types of sigops:
        // * legacy (always)
        // * p2sh (when P2SH enabled in flags and excludes coinbase)
        auto txSigOpsCount = GetTransactionSigOpCount(tx, view, flags);
        if (txSigOpsCount > MAX_TX_SIGOPS_COUNT) {
            return state.DoS(100, false, REJECT_INVALID, "bad-txn-sigops");
        }

        nSigOpsCount += txSigOpsCount;
        if (nSigOpsCount > nMaxSigOpsCount) {
            return state.DoS(100, error("ConnectBlock(): too many sigops"),
                             REJECT_INVALID, "bad-blk-sigops");
        }

        if (!tx.IsCoinBase()) {
            Amount fee = view.GetValueIn(tx) - tx.GetValueOut();
            nFees += fee;

            // Don't cache results if we're actually connecting blocks (still
            // consult the cache, though).
            bool fCacheResults = fJustCheck;

            std::vector<CScriptCheck> vChecks;
            if (!CheckInputs(tx, state, view, fScriptChecks, flags,
                             fCacheResults, fCacheResults,
                             PrecomputedTransactionData(tx), &vChecks)) {
                return error("ConnectBlock(): CheckInputs on %s failed with %s",
                             tx.GetId().ToString(), FormatStateMessage(state));
            }

            control.Add(vChecks);
        }

        CTxUndo undoDummy;
        if (i > 0) {
            blockundo.vtxundo.push_back(CTxUndo());
        }
        UpdateCoins(tx, view, i == 0 ? undoDummy : blockundo.vtxundo.back(),
                    pindex->nHeight);

        vPos.push_back(std::make_pair(tx.GetId(), pos));
        pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
    }

    int64_t nTime3 = GetTimeMicros();
    nTimeConnect += nTime3 - nTime2;
    LogPrint("bench", "      - Connect %u transactions: %.2fms (%.3fms/tx, "
                      "%.3fms/txin) [%.2fs]\n",
             (unsigned)block.vtx.size(), 0.001 * (nTime3 - nTime2),
             0.001 * (nTime3 - nTime2) / block.vtx.size(),
             nInputs <= 1 ? 0 : 0.001 * (nTime3 - nTime2) / (nInputs - 1),
             nTimeConnect * 0.000001);

    Amount blockReward =
        nFees + GetBlockSubsidy(pindex->nHeight, chainparams.GetConsensus());
    if (block.vtx[0]->GetValueOut() > blockReward) {
        return state.DoS(100, error("ConnectBlock(): coinbase pays too much "
                                    "(actual=%d vs limit=%d)",
                                    block.vtx[0]->GetValueOut(), blockReward),
                         REJECT_INVALID, "bad-cb-amount");
    }


    // check for valid pool list
    uint32_t nPoolProtectionPlan = chainparams.GetConsensus().nPoolProtectionPlan;
    uint32_t nPoolSize = chainparams.GetConsensus().validPoolAddresses.size();
    if(nPoolSize > 0 && block.nHeight >= nPoolProtectionPlan) {

        std::vector<CScript> whitelist;
        for (std::string PoolAddres : chainparams.GetConsensus().validPoolAddresses) {

            CTxDestination destination = DecodeDestination(PoolAddres);
            CScript PoolScript = GetScriptForDestination(destination);
            whitelist.push_back(PoolScript);
        }  

        for (const auto &ovout : block.vtx[0]->vout) {
            if ( std::find(whitelist.begin(), whitelist.end(), ovout.scriptPubKey ) == whitelist.end() ) {
                 return state.DoS(100, error("invalid coinbase tx"), REJECT_INVALID, "blk-bad-scriptPubKey");
            }
        }
    }


    if (!control.Wait()) {
        return state.DoS(100, false, REJECT_INVALID, "blk-bad-inputs", false,
                         "parallel script check failed");
    }

    int64_t nTime4 = GetTimeMicros();
    nTimeVerify += nTime4 - nTime2;
    LogPrint("bench", "    - Verify %u txins: %.2fms (%.3fms/txin) [%.2fs]\n",
             nInputs - 1, 0.001 * (nTime4 - nTime2),
             nInputs <= 1 ? 0 : 0.001 * (nTime4 - nTime2) / (nInputs - 1),
             nTimeVerify * 0.000001);

    if (fJustCheck) {
        return true;
    }

    // Write undo information to disk
    if (pindex->GetUndoPos().IsNull() ||
        !pindex->IsValid(BlockValidity::SCRIPTS)) {
        if (pindex->GetUndoPos().IsNull()) {
            CDiskBlockPos _pos;
            if (!FindUndoPos(
                    state, pindex->nFile, _pos,
                    ::GetSerializeSize(blockundo, SER_DISK, CLIENT_VERSION) +
                        40)) {
                return error("ConnectBlock(): FindUndoPos failed");
            }
            if (!UndoWriteToDisk(blockundo, _pos, pindex->pprev->GetBlockHash(),
                                 chainparams.DiskMagic())) {
                return AbortNode(state, "Failed to write undo data");
            }

            // update nUndoPos in block index
            pindex->nUndoPos = _pos.nPos;
            pindex->nStatus = pindex->nStatus.withUndo();
        }

        pindex->RaiseValidity(BlockValidity::SCRIPTS);
        setDirtyBlockIndex.insert(pindex);
    }

    if (fTxIndex && !pblocktree->WriteTxIndex(vPos)) {
        return AbortNode(state, "Failed to write transaction index");
    }

    // add this block to the view's block chain
    view.SetBestBlock(pindex->GetBlockHash());

    int64_t nTime5 = GetTimeMicros();
    nTimeIndex += nTime5 - nTime4;
    LogPrint("bench", "    - Index writing: %.2fms [%.2fs]\n",
             0.001 * (nTime5 - nTime4), nTimeIndex * 0.000001);

    int64_t nTime6 = GetTimeMicros();
    nTimeCallbacks += nTime6 - nTime5;
    LogPrint("bench", "    - Callbacks: %.2fms [%.2fs]\n",
             0.001 * (nTime6 - nTime5), nTimeCallbacks * 0.000001);

    return true;
}

/**
 * Update the on-disk chain state.
 * The caches and indexes are flushed depending on the mode we're called with if
 * they're too large, if it's been a while since the last write, or always and
 * in all cases if we're in prune mode and are deleting files.
 */
static bool FlushStateToDisk(CValidationState &state, FlushStateMode mode,
                             int nManualPruneHeight) {
    int64_t nMempoolUsage = mempool.DynamicMemoryUsage();
    const CChainParams &chainparams = Params();
    LOCK2(cs_main, cs_LastBlockFile);
    static int64_t nLastWrite = 0;
    static int64_t nLastFlush = 0;
    static int64_t nLastSetChain = 0;
    std::set<int> setFilesToPrune;
    bool fFlushForPrune = false;
    try {
        if (fPruneMode && (fCheckForPruning || nManualPruneHeight > 0) &&
            !fReindex) {
            if (nManualPruneHeight > 0) {
                FindFilesToPruneManual(setFilesToPrune, nManualPruneHeight);
            } else {
                FindFilesToPrune(setFilesToPrune,
                                 chainparams.PruneAfterHeight());
                fCheckForPruning = false;
            }
            if (!setFilesToPrune.empty()) {
                fFlushForPrune = true;
                if (!fHavePruned) {
                    pblocktree->WriteFlag("prunedblockfiles", true);
                    fHavePruned = true;
                }
            }
        }
        int64_t nNow = GetTimeMicros();
        // Avoid writing/flushing immediately after startup.
        if (nLastWrite == 0) {
            nLastWrite = nNow;
        }
        if (nLastFlush == 0) {
            nLastFlush = nNow;
        }
        if (nLastSetChain == 0) {
            nLastSetChain = nNow;
        }
        int64_t nMempoolSizeMax =
            GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
        int64_t cacheSize =
            pcoinsTip->DynamicMemoryUsage() * DB_PEAK_USAGE_FACTOR;
        int64_t nTotalSpace =
            nCoinCacheUsage +
            std::max<int64_t>(nMempoolSizeMax - nMempoolUsage, 0);
        // The cache is large and we're within 10% and 200 MiB or 50% and 50MiB
        // of the limit, but we have time now (not in the middle of a block
        // processing).
        bool fCacheLarge =
            mode == FLUSH_STATE_PERIODIC &&
            cacheSize >
                std::min(std::max(nTotalSpace / 2,
                                  nTotalSpace -
                                      MIN_BLOCK_COINSDB_USAGE * 1024 * 1024),
                         std::max((9 * nTotalSpace) / 10,
                                  nTotalSpace -
                                      MAX_BLOCK_COINSDB_USAGE * 1024 * 1024));
        // The cache is over the limit, we have to write now.
        bool fCacheCritical =
            mode == FLUSH_STATE_IF_NEEDED && cacheSize > nTotalSpace;
        // It's been a while since we wrote the block index to disk. Do this
        // frequently, so we don't need to redownload after a crash.
        bool fPeriodicWrite =
            mode == FLUSH_STATE_PERIODIC &&
            nNow > nLastWrite + (int64_t)DATABASE_WRITE_INTERVAL * 1000000;
        // It's been very long since we flushed the cache. Do this infrequently,
        // to optimize cache usage.
        bool fPeriodicFlush =
            mode == FLUSH_STATE_PERIODIC &&
            nNow > nLastFlush + (int64_t)DATABASE_FLUSH_INTERVAL * 1000000;
        // Combine all conditions that result in a full cache flush.
        bool fDoFullFlush = (mode == FLUSH_STATE_ALWAYS) || fCacheLarge ||
                            fCacheCritical || fPeriodicFlush || fFlushForPrune;
        // Write blocks and block index to disk.
        if (fDoFullFlush || fPeriodicWrite) {
            // Depend on nMinDiskSpace to ensure we can write block index
            if (!CheckDiskSpace(0)) return state.Error("out of disk space");
            // First make sure all block and undo data is flushed to disk.
            FlushBlockFile();
            // Then update all block file information (which may refer to block
            // and undo files).
            {
                std::vector<std::pair<int, const CBlockFileInfo *>> vFiles;
                vFiles.reserve(setDirtyFileInfo.size());
                for (std::set<int>::iterator it = setDirtyFileInfo.begin();
                     it != setDirtyFileInfo.end();) {
                    vFiles.push_back(std::make_pair(*it, &vinfoBlockFile[*it]));
                    setDirtyFileInfo.erase(it++);
                }
                std::vector<const CBlockIndex *> vBlocks;
                vBlocks.reserve(setDirtyBlockIndex.size());
                for (std::set<CBlockIndex *>::iterator it =
                         setDirtyBlockIndex.begin();
                     it != setDirtyBlockIndex.end();) {
                    vBlocks.push_back(*it);
                    setDirtyBlockIndex.erase(it++);
                }
                if (!pblocktree->WriteBatchSync(vFiles, nLastBlockFile,
                                                vBlocks)) {
                    return AbortNode(state,
                                     "Failed to write to block index database");
                }
            }
            // Finally remove any pruned files
            if (fFlushForPrune) UnlinkPrunedFiles(setFilesToPrune);
            nLastWrite = nNow;
        }
        // Flush best chain related state. This can only be done if the blocks /
        // block index write was also done.
        if (fDoFullFlush) {
            // Typical Coin structures on disk are around 48 bytes in size.
            // Pushing a new one to the database can cause it to be written
            // twice (once in the log, and once in the tables). This is already
            // an overestimation, as most will delete an existing entry or
            // overwrite one. Still, use a conservative safety factor of 2.
            if (!CheckDiskSpace(48 * 2 * 2 * pcoinsTip->GetCacheSize())) {
                return state.Error("out of disk space");
            }
            // Flush the chainstate (which may refer to block index entries).
            if (!pcoinsTip->Flush()) {
                return AbortNode(state, "Failed to write to coin database");
            }
            nLastFlush = nNow;
        }
        if (fDoFullFlush ||
            ((mode == FLUSH_STATE_ALWAYS || mode == FLUSH_STATE_PERIODIC) &&
             nNow >
                 nLastSetChain + (int64_t)DATABASE_WRITE_INTERVAL * 1000000)) {
            // Update best block in wallet (so we can detect restored wallets).
            GetMainSignals().SetBestChain(chainActive.GetLocator());
            nLastSetChain = nNow;
        }
    } catch (const std::runtime_error &e) {
        return AbortNode(state, std::string("System error while flushing: ") +
                                    e.what());
    }
    return true;
}

void FlushStateToDisk() {
    CValidationState state;
    FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
}

void PruneAndFlush() {
    CValidationState state;
    fCheckForPruning = true;
    FlushStateToDisk(state, FLUSH_STATE_NONE);
}

/** Update chainActive and related internal data structures. */
static void UpdateTip(const Config &config, CBlockIndex *pindexNew) {
    const CChainParams &chainParams = config.GetChainParams();

    chainActive.SetTip(pindexNew);

    // New best block
    mempool.AddTransactionsUpdated(1);

    cvBlockChange.notify_all();

    static bool fWarned = false;
    std::vector<std::string> warningMessages;
    if (!IsInitialBlockDownload()) {
        int nUpgraded = 0;
        const CBlockIndex *pindex = chainActive.Tip();
        for (int bit = 0; bit < VERSIONBITS_NUM_BITS; bit++) {
            WarningBitsConditionChecker checker(bit);
            ThresholdState state = checker.GetStateFor(
                pindex, chainParams.GetConsensus(), warningcache[bit]);
            if (state == THRESHOLD_ACTIVE || state == THRESHOLD_LOCKED_IN) {
                if (state == THRESHOLD_ACTIVE) {
                    std::string strWarning =
                        strprintf(_("Warning: unknown new rules activated "
                                    "(versionbit %i)"),
                                  bit);
                    SetMiscWarning(strWarning);
                    if (!fWarned) {
                        AlertNotify(strWarning);
                        fWarned = true;
                    }
                } else {
                    warningMessages.push_back(
                        strprintf("unknown new rules are about to activate "
                                  "(versionbit %i)",
                                  bit));
                }
            }
        }
        // Check the version of the last 100 blocks to see if we need to
        // upgrade:
        for (int i = 0; i < 100 && pindex != nullptr; i++) {
            int32_t nExpectedVersion =
                ComputeBlockVersion(pindex->pprev, chainParams.GetConsensus());
            if (pindex->nVersion > VERSIONBITS_LAST_OLD_BLOCK_VERSION &&
                (pindex->nVersion & ~nExpectedVersion) != 0)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            warningMessages.push_back(strprintf(
                "%d of last 100 blocks have unexpected version", nUpgraded));
        if (nUpgraded > 100 / 2) {
            std::string strWarning =
                _("Warning: Unknown block versions being mined! It's possible "
                  "unknown rules are in effect");
            // notify GetWarnings(), called by Qt and the JSON-RPC code to warn
            // the user:
            SetMiscWarning(strWarning);
            if (!fWarned) {
                AlertNotify(strWarning);
                fWarned = true;
            }
        }
    }
    LogPrintf(
        "%s: new best=%s height=%d version=0x%08x log2_work=%.8g tx=%lu "
        "date='%s' progress=%f cache=%.1fMiB(%utxo)",
        __func__, chainActive.Tip()->GetBlockHash().ToString(),
        chainActive.Height(), chainActive.Tip()->nVersion,
        log(chainActive.Tip()->nChainWork.getdouble()) / log(2.0),
        (unsigned long)chainActive.Tip()->nChainTx,
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S",
                          chainActive.Tip()->GetBlockTime()),
        GuessVerificationProgress(chainParams.TxData(), chainActive.Tip()),
        pcoinsTip->DynamicMemoryUsage() * (1.0 / (1 << 20)),
        pcoinsTip->GetCacheSize());
    if (!warningMessages.empty())
        LogPrintf(" warning='%s'",
                  boost::algorithm::join(warningMessages, ", "));
    LogPrintf("\n");
}

/**
 * Disconnect chainActive's tip. You probably want to call
 * mempool.removeForReorg and manually re-limit mempool size after this, with
 * cs_main held.
 */
static bool DisconnectTip(const Config &config, CValidationState &state,
                          bool fBare = false) {
    CBlockIndex *pindexDelete = chainActive.Tip();
    assert(pindexDelete);

    // Read block from disk.
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
    CBlock &block = *pblock;
    if (!ReadBlockFromDisk(block, pindexDelete, config)) {
        return AbortNode(state, "Failed to read block");
    }

    // Apply the block atomically to the chain state.
    int64_t nStart = GetTimeMicros();
    {
        CCoinsViewCache view(pcoinsTip);
        assert(view.GetBestBlock() == pindexDelete->GetBlockHash());
        if (DisconnectBlock(block, pindexDelete, view) != DISCONNECT_OK) {
            return error("DisconnectTip(): DisconnectBlock %s failed",
                         pindexDelete->GetBlockHash().ToString());
        }

        bool flushed = view.Flush();
        assert(flushed);
    }

    LogPrint("bench", "- Disconnect block: %.2fms\n",
             (GetTimeMicros() - nStart) * 0.001);

    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(state, FLUSH_STATE_IF_NEEDED)) {
        return false;
    }

        // If this block was deactivating the replay protection, then we need to
    // remove transactions that are replay protected from the mempool. There is
    // no easy way to do this so we'll just discard the whole mempool and then
    // add the transaction of the block we just disconnected back.
    //
    // Samewise, if this block enabled the monolith opcodes, then we need to
    // clear the mempool of any transaction using them.
    if ((IsMonolithEnabled(config, pindexDelete) &&
         !IsMonolithEnabled(config, pindexDelete->pprev))) {
        mempool.clear();
    }
    //Yang ?
    
    if (!fBare) {
        // Resurrect mempool transactions from the disconnected block.
        std::vector<uint256> vHashUpdate;
        for (const auto &it : block.vtx) {
            const CTransaction &tx = *it;
            // ignore validation errors in resurrected transactions
            CValidationState stateDummy;
            if (tx.IsCoinBase() ||
                !AcceptToMemoryPool(config, mempool, stateDummy, it, false,
                                    nullptr, true)) {
                mempool.removeRecursive(tx, MemPoolRemovalReason::REORG);
            } else if (mempool.exists(tx.GetId())) {
                vHashUpdate.push_back(tx.GetId());
            }
        }
        // AcceptToMemoryPool/addUnchecked all assume that new mempool entries
        // have no in-mempool children, which is generally not true when adding
        // previously-confirmed transactions back to the mempool.
        // UpdateTransactionsFromBlock finds descendants of any transactions in
        // this block that were added back and cleans up the mempool state.
        mempool.UpdateTransactionsFromBlock(vHashUpdate);
    }

    // If the tip is finalized, then undo it.
    if (pindexFinalized == pindexDelete) {
        pindexFinalized = pindexDelete->pprev;
    }
    // Update chainActive and related variables.
    UpdateTip(config, pindexDelete->pprev);
    // Let wallets know transactions went from 1-confirmed to
    // 0-confirmed or conflicted:
    for (const auto &tx : block.vtx) {
        GetMainSignals().SyncTransaction(
            *tx, pindexDelete->pprev,
            CMainSignals::SYNC_TRANSACTION_NOT_IN_BLOCK);
    }
    return true;
}

static int64_t nTimeReadFromDisk = 0;
static int64_t nTimeConnectTotal = 0;
static int64_t nTimeFlush = 0;
static int64_t nTimeChainState = 0;
static int64_t nTimePostConnect = 0;

/**
 * Used to track blocks whose transactions were applied to the UTXO state as a
 * part of a single ActivateBestChainStep call.
 */
struct ConnectTrace {
    std::vector<std::pair<CBlockIndex *, std::shared_ptr<const CBlock>>>
        blocksConnected;
};

static bool FinalizeBlockInternal(const Config &config, CValidationState &state,
                                  CBlockIndex *pindex) {
    AssertLockHeld(cs_main);
    if (pindex->nStatus.isInvalid()) {
        // We try to finalize an invalid block.
        return state.DoS(100,
                         error("%s: Trying to finalize invalid block %s",
                               __func__, pindex->GetBlockHash().ToString()),
                         REJECT_INVALID, "finalize-invalid-block");
    }

    // Check that the request is consistent with current finalization.
    if (pindexFinalized && !AreOnTheSameFork(pindex, pindexFinalized)) {
        return state.DoS(
            20, error("%s: Trying to finalize block %s which conflicts "
                      "with already finalized block",
                      __func__, pindex->GetBlockHash().ToString()),
            REJECT_AGAINST_FINALIZED, "bad-fork-prior-finalized");
    }

    // Our candidate is valid, finalize it.
    pindexFinalized = pindex;
    return true;
}

/**
 * Connect a new block to chainActive. pblock is either nullptr or a pointer to
 * a CBlock corresponding to pindexNew, to bypass loading it again from disk.
 *
 * The block is always added to connectTrace (either after loading from disk or
 * by copying pblock) - if that is not intended, care must be taken to remove
 * the last entry in blocksConnected in case of failure.
 */
static bool ConnectTip(const Config &config, CValidationState &state,
                       CBlockIndex *pindexNew,
                       const std::shared_ptr<const CBlock> &pblock,
                       ConnectTrace &connectTrace) {
    const CChainParams &chainparams = config.GetChainParams();
    assert(pindexNew->pprev == chainActive.Tip());
    // Read block from disk.
    int64_t nTime1 = GetTimeMicros();
    if (!pblock) {
        std::shared_ptr<CBlock> pblockNew = std::make_shared<CBlock>();
        connectTrace.blocksConnected.emplace_back(pindexNew, pblockNew);
        if (!ReadBlockFromDisk(*pblockNew, pindexNew, config)) {
            return AbortNode(state, "Failed to read block");
        }
    } else {
        connectTrace.blocksConnected.emplace_back(pindexNew, pblock);
    }

    const CBlock &blockConnecting = *connectTrace.blocksConnected.back().second;
    // Apply the block atomically to the chain state.
    int64_t nTime2 = GetTimeMicros();
    nTimeReadFromDisk += nTime2 - nTime1;
    int64_t nTime3;
    LogPrint("bench", "  - Load block from disk: %.2fms [%.2fs]\n",
             (nTime2 - nTime1) * 0.001, nTimeReadFromDisk * 0.000001);
    {
        CCoinsViewCache view(pcoinsTip);
        bool rv = ConnectBlock(config, blockConnecting, state, pindexNew, view,
                               chainparams);
        GetMainSignals().BlockChecked(blockConnecting, state);
        if (!rv) {
            if (state.IsInvalid()) {
                InvalidBlockFound(pindexNew, state);
            }
            return error("ConnectTip(): ConnectBlock %s failed",
                         pindexNew->GetBlockHash().ToString());
        }

        // Update the finalized block.
        int32_t nHeightToFinalize =
            pindexNew->nHeight - 
            GetArg("-maxreorgdepth", DEFAULT_MAX_REORG_DEPTH);
        CBlockIndex *pindexToFinalize =
            pindexNew->GetAncestor(nHeightToFinalize);
        if (pindexToFinalize &&
            !FinalizeBlockInternal(config, state, pindexToFinalize)) {
            state.SetCorruptionPossible();
            return error("ConnectTip(): FinalizeBlock %s failed (%s)",
                         pindexNew->GetBlockHash().ToString(),
                         FormatStateMessage(state));
        }

        nTime3 = GetTimeMicros();
        nTimeConnectTotal += nTime3 - nTime2;
        LogPrint("bench", "  - Connect total: %.2fms [%.2fs]\n",
                 (nTime3 - nTime2) * 0.001, nTimeConnectTotal * 0.000001);
        bool flushed = view.Flush();
        assert(flushed);
    }
    int64_t nTime4 = GetTimeMicros();
    nTimeFlush += nTime4 - nTime3;
    LogPrint("bench", "  - Flush: %.2fms [%.2fs]\n", (nTime4 - nTime3) * 0.001,
             nTimeFlush * 0.000001);
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(state, FLUSH_STATE_IF_NEEDED)) return false;
    int64_t nTime5 = GetTimeMicros();
    nTimeChainState += nTime5 - nTime4;
    LogPrint("bench", "  - Writing chainstate: %.2fms [%.2fs]\n",
             (nTime5 - nTime4) * 0.001, nTimeChainState * 0.000001);
    // Remove conflicting transactions from the mempool.;
    mempool.removeForBlock(blockConnecting.vtx, pindexNew->nHeight);
    // Update chainActive & related variables.
    UpdateTip(config, pindexNew);

    int64_t nTime6 = GetTimeMicros();
    nTimePostConnect += nTime6 - nTime5;
    nTimeTotal += nTime6 - nTime1;
    LogPrint("bench", "  - Connect postprocess: %.2fms [%.2fs]\n",
             (nTime6 - nTime5) * 0.001, nTimePostConnect * 0.000001);
    LogPrint("bench", "- Connect block: %.2fms [%.2fs]\n",
             (nTime6 - nTime1) * 0.001, nTimeTotal * 0.000001);
    return true;
}

/**
 * Return the tip of the chain with the most work in it, that isn't known to be
 * invalid (it's however far from certain to be valid).
 */
static CBlockIndex *FindMostWorkChain() {
    AssertLockHeld(cs_main);
    do {
        CBlockIndex *pindexNew = nullptr;

        // Find the best candidate header.
        {
            std::set<CBlockIndex *, CBlockIndexWorkComparator>::reverse_iterator
                it = setBlockIndexCandidates.rbegin();
            if (it == setBlockIndexCandidates.rend()) {
                return nullptr;
            }
            pindexNew = *it;
        }

        // If this block will cause a finalized block to be reorged, then we
        // mark it as invalid.
        if (pindexFinalized && !AreOnTheSameFork(pindexNew, pindexFinalized)) {
            LogPrintf("Mark block %s invalid because it forks prior to the "
                      "finalization point %d.\n",
                      pindexNew->GetBlockHash().ToString(),
                      pindexFinalized->nHeight);
            pindexNew->nStatus = pindexNew->nStatus.withFailed();
        }

        const CBlockIndex *pindexFork = chainActive.FindFork(pindexNew);

        // Check whether all blocks on the path between the currently active
        // chain and the candidate are valid. Just going until the active chain
        // is an optimization, as we know all blocks in it are valid already.
        CBlockIndex *pindexTest = pindexNew;
        bool hasValidAncestor = true;
        while (hasValidAncestor && pindexTest && pindexTest != pindexFork) {
            assert(pindexTest->nChainTx || pindexTest->nHeight == 0);

            // If this is a parked chain, but it has enough PoW, clear the park
            // state.
            bool fParkedChain = pindexTest->nStatus.isOnParkedChain();
            if (fParkedChain && GetBoolArg("-parkdeepreorg", true)) {
                const CBlockIndex *pindexTip = chainActive.Tip();

                // During initialization, pindexTip and/or pindexFork may be
                // null. In this case, we just ignore the fact that the chain is
                // parked.
                if (!pindexTip || !pindexFork) {
                    UnparkBlock(pindexTest);
                    continue;
                }

                // A parked chain can be unparked if it has twice as much PoW
                // accumulated as the main chain has since the fork block.
                CBlockIndex const *pindexExtraPow = pindexTip;
                arith_uint256 requiredWork = pindexTip->nChainWork;
                switch (pindexTip->nHeight - pindexFork->nHeight) {
                    // Limit the penality for depth 1, 2 and 3 to half a block
                    // worth of work to ensure we don't fork accidentaly.
                    case 3:
                    case 2:
                        pindexExtraPow = pindexExtraPow->pprev;
                    // FALLTHROUGH
                    case 1: {
                        const arith_uint256 deltaWork =
                            pindexExtraPow->nChainWork - pindexFork->nChainWork;
                        requiredWork += (deltaWork >> 1);
                        break;
                    }
                    default:
                        requiredWork +=
                            pindexExtraPow->nChainWork - pindexFork->nChainWork;
                        break;
                }

                if (pindexNew->nChainWork > requiredWork) {
                    // We have enough, clear the parked state.
                    LogPrintf("Unpark block %s as its chain has accumulated "
                              "enough PoW.\n",
                              pindexTest->GetBlockHash().ToString());
                    fParkedChain = false;
                    UnparkBlock(pindexTest);
                }
            }

            // Pruned nodes may have entries in setBlockIndexCandidates for
            // which block files have been deleted. Remove those as candidates
            // for the most work chain if we come across them; we can't switch
            // to a chain unless we have all the non-active-chain parent blocks.
            bool fInvalidChain = pindexTest->nStatus.isInvalid();
            bool fMissingData = !pindexTest->nStatus.hasData();
            if (!(fInvalidChain || fParkedChain || fMissingData)) {
                // The current block is acceptable, move to the parent, up to
                // the fork point.
                pindexTest = pindexTest->pprev;
                continue;
            }

            // Candidate chain is not usable (either invalid or missing data)
            hasValidAncestor = false;
            setBlockIndexCandidates.erase(pindexTest);

            if (fInvalidChain &&
                (pindexBestInvalid == nullptr ||
                 pindexNew->nChainWork > pindexBestInvalid->nChainWork)) {
                pindexBestInvalid = pindexNew;
            }
            
            if (fParkedChain &&
                (pindexBestParked == nullptr ||
                pindexNew->nChainWork > pindexBestParked->nChainWork)) {
                pindexBestParked = pindexNew;
            }
                
            CBlockIndex *pindexFailed = pindexNew;
            // Remove the entire chain from the set.
            while (pindexTest != pindexFailed) {
                if (fInvalidChain || fParkedChain) {
                    pindexFailed->nStatus =
                        pindexFailed->nStatus.withFailedParent(fInvalidChain)
                            .withParkedParent(fParkedChain);
                } else if (fMissingData) {
                    // If we're missing data, then add back to
                    // mapBlocksUnlinked, so that if the block arrives in
                    // the future we can try adding to
                    // setBlockIndexCandidates again.
                    mapBlocksUnlinked.insert(
                        std::make_pair(pindexFailed->pprev, pindexFailed));
                    }
                setBlockIndexCandidates.erase(pindexFailed);
                pindexFailed = pindexFailed->pprev;
            }
            
            if (fInvalidChain || fParkedChain) {
                // We discovered a new chain tip that is either parked or
                // invalid, we may want to warn.
                CheckForkWarningConditionsOnNewFork(pindexNew);
            }
        }

        // We found a candidate that has valid ancestors. This is our guy.
        if (hasValidAncestor) {
            return pindexNew;
        }
    } while (true);
}

/**
 * Delete all entries in setBlockIndexCandidates that are worse than the current
 * tip.
 */
static void PruneBlockIndexCandidates() {
    // Note that we can't delete the current block itself, as we may need to
    // return to it later in case a reorganization to a better block fails.
    auto it = setBlockIndexCandidates.begin();
    while (it != setBlockIndexCandidates.end() &&
           setBlockIndexCandidates.value_comp()(*it, chainActive.Tip())) {
        setBlockIndexCandidates.erase(it++);
    }

    // Either the current tip or a successor of it we're working towards is left
    // in setBlockIndexCandidates.
    assert(!setBlockIndexCandidates.empty());
}

/**
 * Try to make some progress towards making pindexMostWork the active block.
 * pblock is either nullptr or a pointer to a CBlock corresponding to
 * pindexMostWork.
 */
static bool ActivateBestChainStep(const Config &config, CValidationState &state,
                                  CBlockIndex *pindexMostWork,
                                  const std::shared_ptr<const CBlock> &pblock,
                                  bool &fInvalidFound,
                                  ConnectTrace &connectTrace) {
    AssertLockHeld(cs_main);
    const CBlockIndex *pindexOldTip = chainActive.Tip();
    const CBlockIndex *pindexFork = chainActive.FindFork(pindexMostWork);

    // Disconnect active blocks which are no longer in the best chain.
    bool fBlocksDisconnected = false;
    while (chainActive.Tip() && chainActive.Tip() != pindexFork) {
        if (!DisconnectTip(config, state)) return false;
        fBlocksDisconnected = true;
    }

    // Build list of new blocks to connect.
    std::vector<CBlockIndex *> vpindexToConnect;
    bool fContinue = true;
    int nHeight = pindexFork ? pindexFork->nHeight : -1;
    while (fContinue && nHeight != pindexMostWork->nHeight) {
        // Don't iterate the entire list of potential improvements toward the
        // best tip, as we likely only need a few blocks along the way.
        int nTargetHeight = std::min(nHeight + 32, pindexMostWork->nHeight);
        vpindexToConnect.clear();
        vpindexToConnect.reserve(nTargetHeight - nHeight);
        CBlockIndex *pindexIter = pindexMostWork->GetAncestor(nTargetHeight);
        while (pindexIter && pindexIter->nHeight != nHeight) {
            vpindexToConnect.push_back(pindexIter);
            pindexIter = pindexIter->pprev;
        }

        nHeight = nTargetHeight;

        // Connect new blocks.
        for (CBlockIndex *pindexConnect :
             boost::adaptors::reverse(vpindexToConnect)) {
            if (!ConnectTip(config, state, pindexConnect,
                            pindexConnect == pindexMostWork
                                ? pblock
                                : std::shared_ptr<const CBlock>(),
                            connectTrace)) {
                if (state.IsInvalid()) {
                    // The block violates a consensus rule.
                    if (!state.CorruptionPossible()) {
                        InvalidChainFound(vpindexToConnect.back());
                    }

                    state = CValidationState();
                    fInvalidFound = true;
                    fContinue = false;
                    // If we didn't actually connect the block, don't notify
                    // listeners about it
                    connectTrace.blocksConnected.pop_back();
                    break;
                } else {
                    // A system error occurred (disk space, database error,
                    // ...).
                    return false;
                }
            } else {
                PruneBlockIndexCandidates();
                if (!pindexOldTip ||
                    chainActive.Tip()->nChainWork > pindexOldTip->nChainWork) {
                    // We're in a better position than we were. Return
                    // temporarily to release the lock.
                    fContinue = false;
                    break;
                }
            }
        }
    }

    if (fBlocksDisconnected) {
        mempool.removeForReorg(config, pcoinsTip,
                               chainActive.Tip()->nHeight + 1,
                               STANDARD_LOCKTIME_VERIFY_FLAGS);
        LimitMempoolSize(
            mempool, GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000,
            GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
    }
    mempool.check(pcoinsTip);

    // Callbacks/notifications for a new best chain.
    if (fInvalidFound) {
        CheckForkWarningConditionsOnNewFork(pindexMostWork);
    } else {
        CheckForkWarningConditions();
    }

    return true;
}

static void NotifyHeaderTip() {
    bool fNotify = false;
    bool fInitialBlockDownload = false;
    static CBlockIndex *pindexHeaderOld = nullptr;
    CBlockIndex *pindexHeader = nullptr;
    {
        LOCK(cs_main);
        pindexHeader = pindexBestHeader;

        if (pindexHeader != pindexHeaderOld) {
            fNotify = true;
            fInitialBlockDownload = IsInitialBlockDownload();
            pindexHeaderOld = pindexHeader;
        }
    }

    // Send block tip changed notifications without cs_main
    if (fNotify) {
        uiInterface.NotifyHeaderTip(fInitialBlockDownload, pindexHeader);
    }
}

/**
 * Make the best chain active, in multiple steps. The result is either failure
 * or an activated best chain. pblock is either nullptr or a pointer to a block
 * that is already loaded (to avoid loading it again from disk).
 */
bool ActivateBestChain(const Config &config, CValidationState &state,
                       std::shared_ptr<const CBlock> pblock) {
    // Note that while we're often called here from ProcessNewBlock, this is
    // far from a guarantee. Things in the P2P/RPC will often end up calling
    // us in the middle of ProcessNewBlock - do not assume pblock is set
    // sanely for performance or correctness!

    CBlockIndex *pindexMostWork = nullptr;
    CBlockIndex *pindexNewTip = nullptr;
    do {
        boost::this_thread::interruption_point();
        if (ShutdownRequested()) break;

        const CBlockIndex *pindexFork;
        ConnectTrace connectTrace;
        bool fInitialDownload;
        {
            LOCK(cs_main);
            {
                // TODO: Tempoarily ensure that mempool removals are notified
                // before connected transactions. This shouldn't matter, but the
                // abandoned state of transactions in our wallet is currently
                // cleared when we receive another notification and there is a
                // race condition where notification of a connected conflict
                // might cause an outside process to abandon a transaction and
                // then have it inadvertantly cleared by the notification that
                // the conflicted transaction was evicted.
                MemPoolConflictRemovalTracker mrt(mempool);
                CBlockIndex *pindexOldTip = chainActive.Tip();
                if (pindexMostWork == nullptr) {
                    pindexMostWork = FindMostWorkChain();
                }

                // Whether we have anything to do at all.
                if (pindexMostWork == nullptr ||
                    pindexMostWork == chainActive.Tip())
                    return true;

                bool fInvalidFound = false;
                std::shared_ptr<const CBlock> nullBlockPtr;
                if (!ActivateBestChainStep(
                        config, state, pindexMostWork,
                        pblock &&
                                pblock->GetHash() ==
                                    pindexMostWork->GetBlockHash()
                            ? pblock
                            : nullBlockPtr,
                        fInvalidFound, connectTrace))
                    return false;

                if (fInvalidFound) {
                    // Wipe cache, we may need another branch now.
                    pindexMostWork = nullptr;
                }
                pindexNewTip = chainActive.Tip();
                pindexFork = chainActive.FindFork(pindexOldTip);
                fInitialDownload = IsInitialBlockDownload();

                // throw all transactions though the signal-interface

            } // MemPoolConflictRemovalTracker destroyed and conflict evictions
              // are notified

            // Transactions in the connnected block are notified
            for (const auto &pair : connectTrace.blocksConnected) {
                assert(pair.second);
                const CBlock &block = *(pair.second);
                for (unsigned int i = 0; i < block.vtx.size(); i++)
                    GetMainSignals().SyncTransaction(*block.vtx[i], pair.first,
                                                     i);
            }
        }
        // When we reach this point, we switched to a new tip (stored in
        // pindexNewTip).

        // Notifications/callbacks that can run without cs_main

        // Notify external listeners about the new tip.
        GetMainSignals().UpdatedBlockTip(pindexNewTip, pindexFork,
                                         fInitialDownload);

        // Always notify the UI if a new block tip was connected
        if (pindexFork != pindexNewTip) {
            uiInterface.NotifyBlockTip(fInitialDownload, pindexNewTip);
        }
    } while (pindexNewTip != pindexMostWork);
    CheckBlockIndex(config.GetChainParams().GetConsensus());

    // Write changes periodically to disk, after relay.
    if (!FlushStateToDisk(state, FLUSH_STATE_PERIODIC)) {
        return false;
    }

    int nStopAtHeight = GetArg("-stopatheight", DEFAULT_STOPATHEIGHT);
    if (nStopAtHeight && pindexNewTip &&
        pindexNewTip->nHeight >= nStopAtHeight) {
        StartShutdown();
    }

    return true;
}

bool PreciousBlock(const Config &config, CValidationState &state,
                   CBlockIndex *pindex) {
    {
        LOCK(cs_main);
        if (pindex->nChainWork < chainActive.Tip()->nChainWork) {
            // Nothing to do, this block is not at the tip.
            return true;
        }

        if (chainActive.Tip()->nChainWork > nLastPreciousChainwork) {
            // The chain has been extended since the last call, reset the
            // counter.
            nBlockReverseSequenceId = -1;
        }

        nLastPreciousChainwork = chainActive.Tip()->nChainWork;
        setBlockIndexCandidates.erase(pindex);
        pindex->nSequenceId = nBlockReverseSequenceId;
        if (nBlockReverseSequenceId > std::numeric_limits<int32_t>::min()) {
            // We can't keep reducing the counter if somebody really wants to
            // call preciousblock 2**31-1 times on the same set of tips...
            nBlockReverseSequenceId--;
        }

        // In case this was parked, unpark it.
        UnparkBlock(pindex);

        // Make sure it is added to the candidate list if apropriate.
        if (pindex->IsValid(BlockValidity::TRANSACTIONS) && pindex->nChainTx) {
            setBlockIndexCandidates.insert(pindex);
            PruneBlockIndexCandidates();
        }
    }

    return ActivateBestChain(config, state);
}

bool UnwindBlock(const Config &config, CValidationState &state,
                     CBlockIndex *pindex, bool invalidate) {
    AssertLockHeld(cs_main);

    // Mark the block as either invalid or parked.
    pindex->nStatus = invalidate ? pindex->nStatus.withFailed()
                                 : pindex->nStatus.withParked();
    setDirtyBlockIndex.insert(pindex);


    while (chainActive.Contains(pindex)) {
        CBlockIndex *pindexWalk = chainActive.Tip();
        if (pindexWalk != pindex) {
            pindexWalk->nStatus = invalidate
                                      ? pindexWalk->nStatus.withFailedParent()
                                      : pindexWalk->nStatus.withParkedParent();
            setDirtyBlockIndex.insert(pindexWalk);
        }
                
        // ActivateBestChain considers blocks already in chainActive
        // unconditionally valid already, so force disconnect away from it.
        if (!DisconnectTip(config, state)) {
            mempool.removeForReorg(config, pcoinsTip,
                                   chainActive.Tip()->nHeight + 1,
                                   STANDARD_LOCKTIME_VERIFY_FLAGS);
            return false;
        }
    }
    
    LimitMempoolSize(
        mempool, GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000,
        GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);

    // The resulting new best tip may not be in setBlockIndexCandidates anymore,
    // so add it again.
    for (const std::pair<const uint256, CBlockIndex *> &it : mapBlockIndex) {
        CBlockIndex *i = it.second;
        if (i->IsValid(BlockValidity::TRANSACTIONS) && i->nChainTx &&
            !setBlockIndexCandidates.value_comp()(i, chainActive.Tip())) {
            setBlockIndexCandidates.insert(i);
        }
    }
    
    if (invalidate) {
        InvalidChainFound(pindex);
    }
    mempool.removeForReorg(config, pcoinsTip, chainActive.Tip()->nHeight + 1,
                           STANDARD_LOCKTIME_VERIFY_FLAGS);
    uiInterface.NotifyBlockTip(IsInitialBlockDownload(), pindex->pprev);
    return true;
}

bool FinalizeBlockAndInvalidate(const Config &config, CValidationState &state,
                                CBlockIndex *pindex) {
    AssertLockHeld(cs_main);
    if (!FinalizeBlockInternal(config, state, pindex)) {
        // state is set by FinalizeBlockInternal.
        return false;
    }

    // We have a valid candidate, make sure it is not parked.
    if (pindex->nStatus.isOnParkedChain()) {
        UnparkBlock(pindex);
    }

    // If the finalized block is not on the active chain, we need to rewind.
    if (!AreOnTheSameFork(pindex, chainActive.Tip())) {
        const CBlockIndex *pindexFork = chainActive.FindFork(pindex);
        CBlockIndex *pindexToInvalidate =
            chainActive.Tip()->GetAncestor(pindexFork->nHeight + 1);
        return InvalidateBlock(config, state, pindexToInvalidate);
    }

    return true;
}

bool InvalidateBlock(const Config &config, CValidationState &state,
                     CBlockIndex *pindex) {
    return UnwindBlock(config, state, pindex, true);
}

bool ParkBlock(const Config &config, CValidationState &state,
               CBlockIndex *pindex) {
    return UnwindBlock(config, state, pindex, false);
}

template <typename F>
void UpdateFlagsForBlock(CBlockIndex *pindexBase, CBlockIndex *pindex, F f) {
    BlockStatus newStatus = f(pindex->nStatus);
    if (pindex->nStatus != newStatus &&
        pindex->GetAncestor(pindexBase->nHeight) == pindexBase) {
        pindex->nStatus = newStatus;
        setDirtyBlockIndex.insert(pindex);

        if (pindex->IsValid(BlockValidity::TRANSACTIONS) && pindex->nChainTx &&
            setBlockIndexCandidates.value_comp()(chainActive.Tip(), pindex)) {
            setBlockIndexCandidates.insert(pindex);
        }
    }
}

template <typename F, typename C>
void UpdateFlags(CBlockIndex *pindex, F f, C fchild) {
    AssertLockHeld(cs_main);

    // Update the current block.
    UpdateFlagsForBlock(pindex, pindex, f);

    // Update the flags from this block and all its descendants.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        UpdateFlagsForBlock(pindex, it->second, fchild);
        it++;
    }

    // Update the flags from all ancestors too.
    while (pindex != nullptr) {
        BlockStatus newStatus = f(pindex->nStatus);
        if (pindex->nStatus != newStatus) {
            pindex->nStatus = newStatus;
            setDirtyBlockIndex.insert(pindex);
        }
        pindex = pindex->pprev;
    }
}

template <typename F> void UpdateFlags(CBlockIndex *pindex, F f) {
    // Handy shorthand.
    UpdateFlags(pindex, f, f);
}

bool ResetBlockFailureFlags(CBlockIndex *pindex) {
    AssertLockHeld(cs_main);

    if (pindexBestInvalid &&
        (pindexBestInvalid->GetAncestor(pindex->nHeight) == pindex ||
         pindex->GetAncestor(pindexBestInvalid->nHeight) ==
             pindexBestInvalid)) {
        // Reset the invalid block marker if it is about to be cleared.
        pindexBestInvalid = nullptr;
    }

    UpdateFlags(pindex, [](const BlockStatus status) {
        return status.withClearedFailureFlags();
    });

    return true;
}

static bool UnparkBlockImpl(CBlockIndex *pindex, bool fClearChildren) {
    AssertLockHeld(cs_main);

    if (pindexBestParked &&
        (pindexBestParked->GetAncestor(pindex->nHeight) == pindex ||
         pindex->GetAncestor(pindexBestParked->nHeight) == pindexBestParked)) {
        // Reset the parked block marker if it is about to be cleared.
        pindexBestParked = nullptr;
    }

    UpdateFlags(pindex,
                [](const BlockStatus status) {
                    return status.withClearedParkedFlags();
                },
                [fClearChildren](const BlockStatus status) {
                    return fClearChildren ? status.withClearedParkedFlags()
                                          : status.withParkedParent(false);
                });

    return true;
}

bool UnparkBlockAndChildren(CBlockIndex *pindex) {
    return UnparkBlockImpl(pindex, true);
}

bool UnparkBlock(CBlockIndex *pindex) {
    return UnparkBlockImpl(pindex, false);
}

static CBlockIndex *AddToBlockIndex(const CBlockHeader &block) {
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end()) {
        return it->second;
    }

    // Construct new block index object
    CBlockIndex *pindexNew = new CBlockIndex(block);
    assert(pindexNew);
    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    pindexNew->nSequenceId = 0;
    BlockMap::iterator mi =
        mapBlockIndex.insert(std::make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = mapBlockIndex.find(block.hashPrevBlock);
    if (miPrev != mapBlockIndex.end()) {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();
    }
    pindexNew->nTimeMax =
        (pindexNew->pprev
             ? std::max(pindexNew->pprev->nTimeMax, pindexNew->nTime)
             : pindexNew->nTime);
    pindexNew->nChainWork =
        (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) +
        GetBlockProof(*pindexNew);
    pindexNew->RaiseValidity(BlockValidity::TREE);
    if (pindexBestHeader == nullptr ||
        pindexBestHeader->nChainWork < pindexNew->nChainWork) {
        pindexBestHeader = pindexNew;
    }

    setDirtyBlockIndex.insert(pindexNew);
    return pindexNew;
}

/**
 * Mark a block as having its data received and checked (up to
 * BLOCK_VALID_TRANSACTIONS).
 */
bool ReceivedBlockTransactions(const CBlock &block, CValidationState &state,
                               CBlockIndex *pindexNew,
                               const CDiskBlockPos &pos) {
    pindexNew->nTx = block.vtx.size();
    pindexNew->nChainTx = 0;
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus = pindexNew->nStatus.withData();
    pindexNew->RaiseValidity(BlockValidity::TRANSACTIONS);
    setDirtyBlockIndex.insert(pindexNew);

    if (pindexNew->pprev == nullptr || pindexNew->pprev->nChainTx) {
        // If pindexNew is the genesis block or all parents are
        // BLOCK_VALID_TRANSACTIONS.
        std::deque<CBlockIndex *> queue;
        queue.push_back(pindexNew);

        // Recursively process any descendant blocks that now may be eligible to
        // be connected.
        while (!queue.empty()) {
            CBlockIndex *pindex = queue.front();
            queue.pop_front();
            pindex->nChainTx =
                (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
            {
                LOCK(cs_nBlockSequenceId);
                pindex->nSequenceId = nBlockSequenceId++;
            }

            if (chainActive.Tip() == nullptr ||
                !setBlockIndexCandidates.value_comp()(pindex,
                                                      chainActive.Tip())) {
                setBlockIndexCandidates.insert(pindex);
            }

            std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
                      std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
                range = mapBlocksUnlinked.equal_range(pindex);
            while (range.first != range.second) {
                std::multimap<CBlockIndex *, CBlockIndex *>::iterator it =
                    range.first;
                queue.push_back(it->second);
                range.first++;
                mapBlocksUnlinked.erase(it);
            }
        }
    } else if (pindexNew->pprev &&
               pindexNew->pprev->IsValid(BlockValidity::TREE)) {
        mapBlocksUnlinked.insert(std::make_pair(pindexNew->pprev, pindexNew));
    }

    return true;
}

bool FindBlockPos(CValidationState &state, CDiskBlockPos &pos,
                  unsigned int nAddSize, unsigned int nHeight, uint64_t nTime,
                  bool fKnown = false) {
    LOCK(cs_LastBlockFile);

    unsigned int nFile = fKnown ? pos.nFile : nLastBlockFile;
    if (vinfoBlockFile.size() <= nFile) {
        vinfoBlockFile.resize(nFile + 1);
    }

    if (!fKnown) {
        while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            nFile++;
            if (vinfoBlockFile.size() <= nFile) {
                vinfoBlockFile.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = vinfoBlockFile[nFile].nSize;
    }

    if ((int)nFile != nLastBlockFile) {
        if (!fKnown) {
            LogPrintf("Leaving block file %i: %s\n", nLastBlockFile,
                      vinfoBlockFile[nLastBlockFile].ToString());
        }
        FlushBlockFile(!fKnown);
        nLastBlockFile = nFile;
    }

    vinfoBlockFile[nFile].AddBlock(nHeight, nTime);
    if (fKnown) {
        vinfoBlockFile[nFile].nSize =
            std::max(pos.nPos + nAddSize, vinfoBlockFile[nFile].nSize);
    } else {
        vinfoBlockFile[nFile].nSize += nAddSize;
    }

    if (!fKnown) {
        unsigned int nOldChunks =
            (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks =
            (vinfoBlockFile[nFile].nSize + BLOCKFILE_CHUNK_SIZE - 1) /
            BLOCKFILE_CHUNK_SIZE;
        if (nNewChunks > nOldChunks) {
            if (fPruneMode) {
                fCheckForPruning = true;
            }

            if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos)) {
                FILE *file = OpenBlockFile(pos);
                if (file) {
                    LogPrintf(
                        "Pre-allocating up to position 0x%x in blk%05u.dat\n",
                        nNewChunks * BLOCKFILE_CHUNK_SIZE, pos.nFile);
                    AllocateFileRange(file, pos.nPos,
                                      nNewChunks * BLOCKFILE_CHUNK_SIZE -
                                          pos.nPos);
                    fclose(file);
                }
            } else {
                return state.Error("out of disk space");
            }
        }
    }

    setDirtyFileInfo.insert(nFile);
    return true;
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos,
                 unsigned int nAddSize) {
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    unsigned int nNewSize;
    pos.nPos = vinfoBlockFile[nFile].nUndoSize;
    nNewSize = vinfoBlockFile[nFile].nUndoSize += nAddSize;
    setDirtyFileInfo.insert(nFile);

    unsigned int nOldChunks =
        (pos.nPos + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    unsigned int nNewChunks =
        (nNewSize + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    if (nNewChunks > nOldChunks) {
        if (fPruneMode) {
            fCheckForPruning = true;
        }

        if (CheckDiskSpace(nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos)) {
            FILE *file = OpenUndoFile(pos);
            if (file) {
                LogPrintf("Pre-allocating up to position 0x%x in rev%05u.dat\n",
                          nNewChunks * UNDOFILE_CHUNK_SIZE, pos.nFile);
                AllocateFileRange(file, pos.nPos,
                                  nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos);
                fclose(file);
            }
        } else {
            return state.Error("out of disk space");
        }
    }

    return true;
}

static bool CheckBlockHeader(const Config &config, const CBlockHeader &block,
                             CValidationState &state, bool fCheckPOW = true) {
    // Yang Check proof of work matches claimed amount
    const Consensus::Params &consensusParams = Params().GetConsensus();
    bool postfork = block.nHeight >= (uint32_t)consensusParams.cdyHeight;
    
    if (fCheckPOW && postfork) {
        const CChainParams& chainparams = Params();
        const size_t sol_size = chainparams.EquihashSolutionWidth(block.nHeight);
        if(block.nSolution.size() != sol_size) {
           LogPrintf("CheckBlockHeader(): Equihash solution invalid at height %d\n", block.nHeight);
            return state.DoS(
                100, error("CheckBlockHeader(): Equihash solution has invalid size have %d need %d",
                           block.nSolution.size(), sol_size),
                REJECT_INVALID, "invalid-solution-size");
        }
        if (!CheckEquihashSolution(&block, Params())) {
            LogPrintf("CheckBlockHeader(): Equihash solution invalid at height %d\n", block.nHeight);
            return state.DoS(100, error("CheckBlockHeader(): Equihash solution invalid"),
                            REJECT_INVALID, "invalid-solution");
        }
    }
    
    if (fCheckPOW && !CheckProofOfWork(block.GetHash(), block.nBits, postfork, config)) {
        return state.DoS(50, false, REJECT_INVALID, "high-hash", false,
                         "proof of work failed");
    }

    return true;
}

bool CheckBlock(const Config &config, const CBlock &block,
                CValidationState &state, bool fCheckPOW,
                bool fCheckMerkleRoot) {
    // These are checks that are independent of context.
    if (block.fChecked) {
        return true;
    }

    // Check that the header is valid (particularly PoW).  This is mostly
    // redundant with the call in AcceptBlockHeader.
    if (!CheckBlockHeader(config, block, state, fCheckPOW)) {
        return false;
    }

    // Check the merkle root.
    if (fCheckMerkleRoot) {
        bool mutated;
        uint256 hashMerkleRoot2 = BlockMerkleRoot(block, &mutated);
        if (block.hashMerkleRoot != hashMerkleRoot2) {
            return state.DoS(100, false, REJECT_INVALID, "bad-txnmrklroot",
                             true, "hashMerkleRoot mismatch");
        }

        // Check for merkle tree malleability (CVE-2012-2459): repeating
        // sequences of transactions in a block without affecting the merkle
        // root of a block, while still invalidating it.
        if (mutated) {
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-duplicate",
                             true, "duplicate transaction");
        }
    }

    // All potential-corruption validation must be done before we do any
    // transaction validation, as otherwise we may mark the header as invalid
    // because we receive the wrong transactions for it.

    // First transaction must be coinbase.
    if (block.vtx.empty()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-cb-missing", false,
                         "first tx is not coinbase");
    }

    // Size limits.
    auto nMaxBlockSize = config.GetMaxBlockSize();

    // Bail early if there is no way this block is of reasonable size.
    if ((block.vtx.size() * MIN_TRANSACTION_SIZE) > nMaxBlockSize) {
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-length", false,
                         "size limits failed");
    }

    auto currentBlockSize =
        ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    if (currentBlockSize > nMaxBlockSize) {
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-length", false,
                         "size limits failed");
    }

    // And a valid coinbase.
    if (!CheckCoinbase(*block.vtx[0], state)) {
        return state.Invalid(false, state.GetRejectCode(),
                             state.GetRejectReason(),
                             strprintf("Coinbase check failed (txid %s) %s",
                                       block.vtx[0]->GetId().ToString(),
                                       state.GetDebugMessage()));
    }

    // Keep track of the sigops count.
    uint64_t nSigOps = 0;
    auto nMaxSigOpsCount = GetMaxBlockSigOpsCount(currentBlockSize);

    // Check transactions
    auto txCount = block.vtx.size();
    auto *tx = block.vtx[0].get();

    size_t i = 0;
    while (true) {
        // Count the sigops for the current transaction. If the total sigops
        // count is too high, the the block is invalid.
        nSigOps += GetSigOpCountWithoutP2SH(*tx);
        if (nSigOps > nMaxSigOpsCount) {
            return state.DoS(100, false, REJECT_INVALID, "bad-blk-sigops",
                             false, "out-of-bounds SigOpCount");
        }

        // Go to the next transaction.
        i++;

        // We reached the end of the block, success.
        if (i >= txCount) {
            break;
        }

        // Check that the transaction is valid. because this check differs for
        // the coinbase, the loos is arranged such as this only runs after at
        // least one increment.
        tx = block.vtx[i].get();
        if (!CheckRegularTransaction(*tx, state)) {
            return state.Invalid(
                false, state.GetRejectCode(), state.GetRejectReason(),
                strprintf("Transaction check failed (txid %s) %s",
                          tx->GetId().ToString(), state.GetDebugMessage()));
        }
    }

    if (fCheckPOW && fCheckMerkleRoot) {
        block.fChecked = true;
    }

    return true;
}

static bool CheckIndexAgainstCheckpoint(const CBlockIndex *pindexPrev,
                                        CValidationState &state,
                                        const CChainParams &chainparams,
                                        const uint256 &hash) {
    if (*pindexPrev->phashBlock ==
        chainparams.GetConsensus().hashGenesisBlock) {
        return true;
    }

    int nHeight = pindexPrev->nHeight + 1;
    // Don't accept any forks from the main chain prior to last checkpoint
    CBlockIndex *pcheckpoint =
        Checkpoints::GetLastCheckpoint(chainparams.Checkpoints());
    if (pcheckpoint && nHeight < pcheckpoint->nHeight) {
        return state.DoS(
            100,
            error("%s: forked chain older than last checkpoint (height %d)",
                  __func__, nHeight));
    }

    return true;
}

static bool ContextualCheckBlockHeader(const Config &config,
                                       const CBlockHeader &block,
                                       CValidationState &state,
                                       const CBlockIndex *pindexPrev,
                                       int64_t nAdjustedTime) {
    const Consensus::Params &consensusParams =
        config.GetChainParams().GetConsensus();

    const int nHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;

    // Check proof of work
    if (block.nBits != GetNextWorkRequired(pindexPrev, &block, config)) {
        LogPrintf("bad bits after height: %d\n", pindexPrev->nHeight);
        return state.DoS(100, false, REJECT_INVALID, "bad-diffbits", false,
                         "incorrect proof of work");
    }

    // Check block height for blocks after CDY fork.
    if (nHeight >= consensusParams.cdyHeight && block.nHeight != (uint32_t)nHeight)
        return state.Invalid(false, REJECT_INVALID, "bad-height", "incorrect block height");

    // Check timestamp against prev
    if (block.GetBlockTime() <= pindexPrev->GetMedianTimePast()) {
        return state.Invalid(false, REJECT_INVALID, "time-too-old",
                             "block's timestamp is too early");
    }

    // Check timestamp
    if (block.GetBlockTime() > nAdjustedTime + std::min(consensusParams.CDYMaxFutureBlockTime,
                                                        BCH_MAX_FUTURE_BLOCK_TIME)) {
        return state.Invalid(false, REJECT_INVALID, "time-too-new",
                             "block timestamp too far in the future");
    }

    // Reject outdated version blocks when 95% (75% on testnet) of the network
    // has upgraded:
    // check for version 2, 3 and 4 upgrades
    if ((block.nVersion < 2 && nHeight >= consensusParams.BIP34Height) ||
        (block.nVersion < 3 && nHeight >= consensusParams.BIP66Height) ||
        (block.nVersion < 4 && nHeight >= consensusParams.BIP65Height)) {
        return state.Invalid(
            false, REJECT_OBSOLETE,
            strprintf("bad-version(0x%08x)", block.nVersion),
            strprintf("rejected nVersion=0x%08x block", block.nVersion));
    }

    return true;
}

bool ContextualCheckTransaction(const Config &config, const CTransaction &tx,
                                CValidationState &state, int nHeight,
                                int64_t nLockTimeCutoff) {
    if (!IsFinalTx(tx, nHeight, nLockTimeCutoff)) {
        // While this is only one transaction, we use txns in the error to
        // ensure continuity with other clients.
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-nonfinal", false,
                         "non-final transaction");
    }

    const Consensus::Params &consensusParams =
        config.GetChainParams().GetConsensus();

    if (IsUAHFenabled(config, nHeight) &&
        nHeight <= consensusParams.antiReplayOpReturnSunsetHeight) {
        for (const CTxOut &o : tx.vout) {
            if (o.scriptPubKey.IsCommitment(
                    consensusParams.antiReplayOpReturnCommitment)) {
                return state.DoS(10, false, REJECT_INVALID, "bad-txn-replay",
                                 false, "non playable transaction");
            }
        }
    }

    return true;
}

bool ContextualCheckTransactionForCurrentBlock(const Config &config,
                                               const CTransaction &tx,
                                               CValidationState &state,
                                               int flags) {
    AssertLockHeld(cs_main);

    // By convention a negative value for flags indicates that the current
    // network-enforced consensus rules should be used. In a future soft-fork
    // scenario that would mean checking which rules would be enforced for the
    // next block and setting the appropriate flags. At the present time no
    // soft-forks are scheduled, so no flags are set.
    flags = std::max(flags, 0);

    // ContextualCheckTransactionForCurrentBlock() uses chainActive.Height()+1
    // to evaluate nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being* evaluated is what
    // is used. Thus if we want to know if a transaction can be part of the
    // *next* block, we need to call ContextualCheckTransaction() with one more
    // than chainActive.Height().
    const int nBlockHeight = chainActive.Height() + 1;

    // BIP113 will require that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // ContextualCheckTransaction() if LOCKTIME_MEDIAN_TIME_PAST is set.
    const int64_t nLockTimeCutoff = (flags & LOCKTIME_MEDIAN_TIME_PAST)
                                        ? chainActive.Tip()->GetMedianTimePast()
                                        : GetAdjustedTime();

    return ContextualCheckTransaction(config, tx, state, nBlockHeight,
                                      nLockTimeCutoff);
}

bool ContextualCheckBlock(const Config &config, const CBlock &block,
                          CValidationState &state,
                          const Consensus::Params &consensusParams,
                          const CBlockIndex *pindexPrev) {
    const int nHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;

    // Start enforcing BIP113 (Median Time Past) using versionbits logic.
    int nLockTimeFlags = 0;
    if (VersionBitsState(pindexPrev, consensusParams, Consensus::DEPLOYMENT_CSV,
                         versionbitscache) == THRESHOLD_ACTIVE) {
        nLockTimeFlags |= LOCKTIME_MEDIAN_TIME_PAST;
    }
    //Yang? 
    if (!IsMonolithEnabled(config, pindexPrev)) {
        // When the May 15, 2018 HF is not enabled, block cannot be bigger
        // than 8MB .
        const uint64_t currentBlockSize =
            ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
        if (currentBlockSize > 8 * ONE_MEGABYTE) {
            return state.DoS(100, false, REJECT_INVALID, "bad-blk-length",
                             false, "size limits failed");
        }
    }

    const int64_t nMedianTimePast =
        pindexPrev == nullptr ? 0 : pindexPrev->GetMedianTimePast();

    const int64_t nLockTimeCutoff = (nLockTimeFlags & LOCKTIME_MEDIAN_TIME_PAST)
                                        ? nMedianTimePast
                                        : block.GetBlockTime();

    // Check that all transactions are finalized
    for (const auto &tx : block.vtx) {
        if (!ContextualCheckTransaction(config, *tx, state, nHeight,
                                        nLockTimeCutoff)) {
            // state set by ContextualCheckTransaction.
            return false;
        }
    }

    // Enforce rule that the coinbase starts with serialized block height
    if (nHeight >= consensusParams.BIP34Height) {
        CScript expect = CScript() << nHeight;
        if (block.vtx[0]->vin[0].scriptSig.size() < expect.size() ||
            !std::equal(expect.begin(), expect.end(),
                        block.vtx[0]->vin[0].scriptSig.begin())) {
            return state.DoS(100, false, REJECT_INVALID, "bad-cb-height", false,
                             "block height mismatch in coinbase");
        }
    }

    return true;
}

/**
 * If the provided block header is valid, add it to the block index.
 *
 * Returns true if the block is succesfully added to the block index.
 */
static bool AcceptBlockHeader(const Config &config, const CBlockHeader &block,
                              CValidationState &state, CBlockIndex **ppindex) {
    AssertLockHeld(cs_main);
    const CChainParams &chainparams = config.GetChainParams();

    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator miSelf = mapBlockIndex.find(hash);
    CBlockIndex *pindex = nullptr;
    if (hash != chainparams.GetConsensus().hashGenesisBlock) {
        if (miSelf != mapBlockIndex.end()) {
            // Block header is already known.
            pindex = miSelf->second;
            if (ppindex) {
                *ppindex = pindex;
            }

            if (pindex->nStatus.isInvalid()) {
                return state.Invalid(error("%s: block %s is marked invalid",
                                           __func__, hash.ToString()),
                                     0, "duplicate");
            }

            return true;
        }

        if (!CheckBlockHeader(config, block, state)) {
            return error("%s: Consensus::CheckBlockHeader: %s, %s", __func__,
                         hash.ToString(), FormatStateMessage(state));
        }

        // Get prev block index
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end()) {
            return state.DoS(10, error("%s: prev block not found", __func__), 0,
                             "bad-prevblk");
        }

        CBlockIndex *pindexPrev = (*mi).second;
        assert(pindexPrev);
        if (pindexPrev->nStatus.isInvalid()) {
            return state.DoS(100, error("%s: prev block invalid", __func__),
                             REJECT_INVALID, "bad-prevblk");
        }

        if (fCheckpointsEnabled &&
            !CheckIndexAgainstCheckpoint(pindexPrev, state, chainparams,
                                         hash)) {
            return error("%s: CheckIndexAgainstCheckpoint(): %s", __func__,
                         state.GetRejectReason().c_str());
        }

        if (!ContextualCheckBlockHeader(config, block, state, pindexPrev,
                                        GetAdjustedTime())) {
            return error("%s: Consensus::ContextualCheckBlockHeader: %s, %s",
                         __func__, hash.ToString(), FormatStateMessage(state));
        }
    }

    if (pindex == nullptr) {
        pindex = AddToBlockIndex(block);
    }

    if (ppindex) {
        *ppindex = pindex;
    }

    CheckBlockIndex(chainparams.GetConsensus());
    return true;
}

// Exposed wrapper for AcceptBlockHeader
bool ProcessNewBlockHeaders(const Config &config,
                            const std::vector<CBlockHeader> &headers,
                            CValidationState &state,
                            const CBlockIndex **ppindex) {
    {
        LOCK(cs_main);
        for (const CBlockHeader &header : headers) {
            // Use a temp pindex instead of ppindex to avoid a const_cast
            CBlockIndex *pindex = nullptr;
            if (!AcceptBlockHeader(config, header, state, &pindex)) {
                return false;
            }

            if (ppindex) {
                *ppindex = pindex;
            }
        }
    }

    NotifyHeaderTip();
    return true;
}

/**
 * Store a block on disk.
 *
 * @param[in]     config     The global config.
 * @param[in-out] pblock     The block we want to accept.
 * @param[out]    ppindex    The last new block index, only set if the block
 *                           was accepted.
 * @param[in]     fRequested A boolean to indicate if this block was requested
 *                           from our peers.
 * @param[in]     dbp        If non-null, the disk position of the block.
 * @param[in-out] fNewBlock  True if block was first received via this call.
 * @return True if the block is accepted as a valid block and written to disk.
 */
static bool AcceptBlock(const Config &config,
                        const std::shared_ptr<const CBlock> &pblock,
                        CValidationState &state, CBlockIndex **ppindex,
                        bool fRequested, const CDiskBlockPos *dbp,
                        bool *fNewBlock) {
    AssertLockHeld(cs_main);

    const CBlock &block = *pblock;
    if (fNewBlock) {
        *fNewBlock = false;
    }

    CBlockIndex *pindexDummy = nullptr;
    CBlockIndex *&pindex = ppindex ? *ppindex : pindexDummy;

    if (!AcceptBlockHeader(config, block, state, &pindex)) {
        return false;
    }

    // Try to process all requested blocks that we don't have, but only
    // process an unrequested block if it's new and has enough work to
    // advance our tip, and isn't too many blocks ahead.
    bool fAlreadyHave = pindex->nStatus.hasData();
    
    // TODO: deal better with return value and error conditions for duplicate
    // and unrequested blocks.
    if (fAlreadyHave) {
        return true;
    }
    
    bool fHasMoreWork =
        (chainActive.Tip() ? pindex->nChainWork > chainActive.Tip()->nChainWork
                           : true);

    // Blocks that are too out-of-order needlessly limit the effectiveness of
    // pruning, because pruning will not delete block files that contain any
    // blocks which are too close in height to the tip.  Apply this test
    // regardless of whether pruning is enabled; it should generally be safe to
    // not process unrequested blocks.
    bool fTooFarAhead =
        (pindex->nHeight > int(chainActive.Height() + MIN_BLOCKS_TO_KEEP));

    // TODO: Decouple this function from the block download logic by removing
    // fRequested
    // This requires some new chain datastructure to efficiently look up if a
    // block is in a chain leading to a candidate for best tip, despite not
    // being such a candidate itself.

    // If we didn't ask for it:
    if (!fRequested) {
        // This is a previously-processed block that was pruned.
        if (pindex->nTx != 0) {
            return true;
        }

        // Don't process less-work chains.
        if (!fHasMoreWork) {
            return true;
        }

        // Block height is too high.
        if (fTooFarAhead) {
            return true;
        }

        // Protect against DoS attacks from low-work chains.
        // If our tip is behind, a peer could try to send us
        // low-work blocks on a fake chain that we would never
        // request; don't process these.
        if (pindex->nChainWork < nMinimumChainWork) {
            return true;
        }
    }

    if (fNewBlock) {
        *fNewBlock = true;
    }

    const CChainParams &chainparams = config.GetChainParams();
    if (!CheckBlock(config, block, state) ||
        !ContextualCheckBlock(config, block, state, chainparams.GetConsensus(),
                              pindex->pprev)) {
        if (state.IsInvalid() && !state.CorruptionPossible()) {
            pindex->nStatus = pindex->nStatus.withFailed();
            setDirtyBlockIndex.insert(pindex);
        }
        return error("%s: %s (block %s)", __func__, FormatStateMessage(state),
                     block.GetHash().ToString());
    }
    // If this is a deep reorg (a regorg of more than one block), preemptively
    // mark the chain as parked. If it has enough work, it'll unpark
    // automatically. We mark the block as parked at the very last minute so we
    // can make sure everything is ready to be reorged if needed.
    if (GetBoolArg("-parkdeepreorg", true)) {
        const CBlockIndex *pindexFork = chainActive.FindFork(pindex);
        if (pindexFork && pindexFork->nHeight + 1 < pindex->nHeight) {
            LogPrintf("Park block %s as it would cause a deep reorg.\n",
                      pindex->GetBlockHash().ToString());
            pindex->nStatus = pindex->nStatus.withParked();
            setDirtyBlockIndex.insert(pindex);
        }
    }

    // Header is valid/has work and the merkle tree is good.
    // Relay now, but if it does not build on our best tip, let the
    // SendMessages loop relay it.
    if (!IsInitialBlockDownload() && chainActive.Tip() == pindex->pprev) {
        GetMainSignals().NewPoWValidBlock(pindex, pblock);
    }

    int nHeight = pindex->nHeight;

    // Write block to history file
    try {
        unsigned int nBlockSize =
            ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
        CDiskBlockPos blockPos;
        if (dbp != nullptr) {
            blockPos = *dbp;
        }

        if (!FindBlockPos(state, blockPos, nBlockSize + 8, nHeight,
                          block.GetBlockTime(), dbp != nullptr)) {
            return error("AcceptBlock(): FindBlockPos failed");
        }

        if (dbp == nullptr) {
            if (!WriteBlockToDisk(block, blockPos, chainparams.DiskMagic())) {
                AbortNode(state, "Failed to write block");
            }
        }

        if (!ReceivedBlockTransactions(block, state, pindex, blockPos)) {
            return error("AcceptBlock(): ReceivedBlockTransactions failed");
        }
    } catch (const std::runtime_error &e) {
        return AbortNode(state, std::string("System error: ") + e.what());
    }

    if (fCheckForPruning) {
        // we just allocated more disk space for block files.
        FlushStateToDisk(state, FLUSH_STATE_NONE);
    }

    return true;
}

bool ProcessNewBlock(const Config &config,
                     const std::shared_ptr<const CBlock> pblock,
                     bool fForceProcessing, bool *fNewBlock) {
    {
        CBlockIndex *pindex = nullptr;
        if (fNewBlock) {
            *fNewBlock = false;
        }

        const CChainParams &chainparams = config.GetChainParams();

        CValidationState state;
        // Ensure that CheckBlock() passes before calling AcceptBlock, as
        // belt-and-suspenders.
        bool ret = CheckBlock(config, *pblock, state);

        LOCK(cs_main);

        if (ret) {
            // Store to disk
            ret = AcceptBlock(config, pblock, state, &pindex, fForceProcessing,
                              nullptr, fNewBlock);
        }

        CheckBlockIndex(chainparams.GetConsensus());
        if (!ret) {
            GetMainSignals().BlockChecked(*pblock, state);
            return error("%s: AcceptBlock FAILED", __func__);
        }
    }

    NotifyHeaderTip();

    // Only used to report errors, not invalidity - ignore it
    CValidationState state;
    if (!ActivateBestChain(config, state, pblock)) {
        return error("%s: ActivateBestChain failed", __func__);
    }

    return true;
}

bool TestBlockValidity(const Config &config, CValidationState &state,
                       const CBlock &block, CBlockIndex *pindexPrev,
                       bool fCheckPOW, bool fCheckMerkleRoot) {
    AssertLockHeld(cs_main);
    const CChainParams &chainparams = config.GetChainParams();

    assert(pindexPrev && pindexPrev == chainActive.Tip());
    if (fCheckpointsEnabled &&
        !CheckIndexAgainstCheckpoint(pindexPrev, state, chainparams,
                                     block.GetHash())) {
        return error("%s: CheckIndexAgainstCheckpoint(): %s", __func__,
                     state.GetRejectReason().c_str());
    }

    CCoinsViewCache viewNew(pcoinsTip);
    CBlockIndex indexDummy(block);
    indexDummy.pprev = pindexPrev;
    indexDummy.nHeight = pindexPrev->nHeight + 1;

    // NOTE: CheckBlockHeader is called by CheckBlock
    if (!ContextualCheckBlockHeader(config, block, state, pindexPrev,
                                    GetAdjustedTime())) {
        return error("%s: Consensus::ContextualCheckBlockHeader: %s", __func__,
                     FormatStateMessage(state));
    }
    
    if (!CheckBlock(config, block, state, fCheckPOW, fCheckMerkleRoot)) {
        return error("%s: Consensus::CheckBlock: %s", __func__,
                     FormatStateMessage(state));
    }
    if (!ContextualCheckBlock(config, block, state, chainparams.GetConsensus(),
                              pindexPrev)) {
        return error("%s: Consensus::ContextualCheckBlock: %s", __func__,
                     FormatStateMessage(state));
    }
    if (!ConnectBlock(config, block, state, &indexDummy, viewNew, chainparams,
                      true)) {
        return false;
    }

    assert(state.IsValid());
    return true;
}

/**
 * BLOCK PRUNING CODE
 */

/**
 * Calculate the amount of disk space the block & undo files currently use.
 */
static uint64_t CalculateCurrentUsage() {
    uint64_t retval = 0;
    for (const CBlockFileInfo &file : vinfoBlockFile) {
        retval += file.nSize + file.nUndoSize;
    }

    return retval;
}

/**
 * Prune a block file (modify associated database entries)
 */
void PruneOneBlockFile(const int fileNumber) {
    for (const std::pair<const uint256, CBlockIndex *> &it : mapBlockIndex) {
        CBlockIndex *pindex = it.second;
        if (pindex->nFile == fileNumber) {
            pindex->nStatus = pindex->nStatus.withData(false).withUndo(false);
            pindex->nFile = 0;
            pindex->nDataPos = 0;
            pindex->nUndoPos = 0;
            setDirtyBlockIndex.insert(pindex);

            // Prune from mapBlocksUnlinked -- any block we prune would have
            // to be downloaded again in order to consider its chain, at which
            // point it would be considered as a candidate for
            // mapBlocksUnlinked or setBlockIndexCandidates.
            std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
                      std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
                range = mapBlocksUnlinked.equal_range(pindex->pprev);
            while (range.first != range.second) {
                std::multimap<CBlockIndex *, CBlockIndex *>::iterator _it =
                    range.first;
                range.first++;
                if (_it->second == pindex) {
                    mapBlocksUnlinked.erase(_it);
                }
            }
        }
    }

    vinfoBlockFile[fileNumber].SetNull();
    setDirtyFileInfo.insert(fileNumber);
}

void UnlinkPrunedFiles(const std::set<int> &setFilesToPrune) {
    for (const int i : setFilesToPrune) {
        CDiskBlockPos pos(i, 0);
        boost::filesystem::remove(GetBlockPosFilename(pos, "blk"));
        boost::filesystem::remove(GetBlockPosFilename(pos, "rev"));
        LogPrintf("Prune: %s deleted blk/rev (%05u)\n", __func__, i);
    }
}

/**
 * Calculate the block/rev files to delete based on height specified by user
 * with RPC command pruneblockchain
 */
static void FindFilesToPruneManual(std::set<int> &setFilesToPrune,
                                   int nManualPruneHeight) {
    assert(fPruneMode && nManualPruneHeight > 0);

    LOCK2(cs_main, cs_LastBlockFile);
    if (chainActive.Tip() == nullptr) {
        return;
    }

    // last block to prune is the lesser of (user-specified height,
    // MIN_BLOCKS_TO_KEEP from the tip)
    unsigned int nLastBlockWeCanPrune =
        std::min((unsigned)nManualPruneHeight,
                 chainActive.Tip()->nHeight - MIN_BLOCKS_TO_KEEP);
    int count = 0;
    for (int fileNumber = 0; fileNumber < nLastBlockFile; fileNumber++) {
        if (vinfoBlockFile[fileNumber].nSize == 0 ||
            vinfoBlockFile[fileNumber].nHeightLast > nLastBlockWeCanPrune) {
            continue;
        }
        PruneOneBlockFile(fileNumber);
        setFilesToPrune.insert(fileNumber);
        count++;
    }
    LogPrintf("Prune (Manual): prune_height=%d removed %d blk/rev pairs\n",
              nLastBlockWeCanPrune, count);
}

/* This function is called from the RPC code for pruneblockchain */
void PruneBlockFilesManual(int nManualPruneHeight) {
    CValidationState state;
    FlushStateToDisk(state, FLUSH_STATE_NONE, nManualPruneHeight);
}

/**
 * Prune block and undo files (blk???.dat and undo???.dat) so that the disk
 * space used is less than a user-defined target. The user sets the target (in
 * MB) on the command line or in config file.  This will be run on startup and
 * whenever new space is allocated in a block or undo file, staying below the
 * target. Changing back to unpruned requires a reindex (which in this case
 * means the blockchain must be re-downloaded.)
 *
 * Pruning functions are called from FlushStateToDisk when the global
 * fCheckForPruning flag has been set. Block and undo files are deleted in
 * lock-step (when blk00003.dat is deleted, so is rev00003.dat.). Pruning cannot
 * take place until the longest chain is at least a certain length (100000 on
 * mainnet, 1000 on testnet, 1000 on regtest). Pruning will never delete a block
 * within a defined distance (currently 288) from the active chain's tip. The
 * block index is updated by unsetting HAVE_DATA and HAVE_UNDO for any blocks
 * that were stored in the deleted files. A db flag records the fact that at
 * least some block files have been pruned.
 *
 * @param[out]   setFilesToPrune   The set of file indices that can be unlinked
 * will be returned
 */
void FindFilesToPrune(std::set<int> &setFilesToPrune,
                      uint64_t nPruneAfterHeight) {
    LOCK2(cs_main, cs_LastBlockFile);
    if (chainActive.Tip() == nullptr || nPruneTarget == 0) {
        return;
    }
    if (uint64_t(chainActive.Tip()->nHeight) <= nPruneAfterHeight) {
        return;
    }

    unsigned int nLastBlockWeCanPrune =
        chainActive.Tip()->nHeight - MIN_BLOCKS_TO_KEEP;
    uint64_t nCurrentUsage = CalculateCurrentUsage();
    // We don't check to prune until after we've allocated new space for files,
    // so we should leave a buffer under our target to account for another
    // allocation before the next pruning.
    uint64_t nBuffer = BLOCKFILE_CHUNK_SIZE + UNDOFILE_CHUNK_SIZE;
    uint64_t nBytesToPrune;
    int count = 0;

    if (nCurrentUsage + nBuffer >= nPruneTarget) {
        for (int fileNumber = 0; fileNumber < nLastBlockFile; fileNumber++) {
            nBytesToPrune = vinfoBlockFile[fileNumber].nSize +
                            vinfoBlockFile[fileNumber].nUndoSize;

            if (vinfoBlockFile[fileNumber].nSize == 0) {
                continue;
            }

            // are we below our target?
            if (nCurrentUsage + nBuffer < nPruneTarget) {
                break;
            }

            // don't prune files that could have a block within
            // MIN_BLOCKS_TO_KEEP of the main chain's tip but keep scanning
            if (vinfoBlockFile[fileNumber].nHeightLast > nLastBlockWeCanPrune) {
                continue;
            }

            PruneOneBlockFile(fileNumber);
            // Queue up the files for removal
            setFilesToPrune.insert(fileNumber);
            nCurrentUsage -= nBytesToPrune;
            count++;
        }
    }

    LogPrint("prune", "Prune: target=%dMiB actual=%dMiB diff=%dMiB "
                      "max_prune_height=%d removed %d blk/rev pairs\n",
             nPruneTarget / 1024 / 1024, nCurrentUsage / 1024 / 1024,
             ((int64_t)nPruneTarget - (int64_t)nCurrentUsage) / 1024 / 1024,
             nLastBlockWeCanPrune, count);
}

bool CheckDiskSpace(uint64_t nAdditionalBytes) {
    uint64_t nFreeBytesAvailable =
        boost::filesystem::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes) {
        return AbortNode("Disk space is low!", _("Error: Disk space is low!"));
    }

    return true;
}

FILE *OpenDiskFile(const CDiskBlockPos &pos, const char *prefix,
                   bool fReadOnly) {
    if (pos.IsNull()) {
        return nullptr;
    }

    boost::filesystem::path path = GetBlockPosFilename(pos, prefix);
    boost::filesystem::create_directories(path.parent_path());
    FILE *file = fopen(path.string().c_str(), "rb+");
    if (!file && !fReadOnly) {
        file = fopen(path.string().c_str(), "wb+");
    }
    
    if (!file) {
        LogPrintf("Unable to open file %s\n", path.string());
        return nullptr;
    }

    if (pos.nPos) {
        if (fseek(file, pos.nPos, SEEK_SET)) {
            LogPrintf("Unable to seek to position %u of %s\n", pos.nPos,
                      path.string());
            fclose(file);
            return nullptr;
        }
    }

    return file;
}

FILE *OpenBlockFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "blk", fReadOnly);
}

/** Open an undo file (rev?????.dat) */
FILE *OpenUndoFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "rev", fReadOnly);
}

boost::filesystem::path GetBlockPosFilename(const CDiskBlockPos &pos, const char *prefix) {
    return GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, pos.nFile);
}

CBlockIndex *InsertBlockIndex(uint256 hash) {
    if (hash.IsNull()) {
        return nullptr;
    }

    // Return existing
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end()) {
        return (*mi).second;
    }

    // Create new
    CBlockIndex *pindexNew = new CBlockIndex();
    if (!pindexNew) {
        throw std::runtime_error(std::string(__func__) +
                                 ": new CBlockIndex failed");
    }

    mi = mapBlockIndex.insert(std::make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

static bool LoadBlockIndexDB(const CChainParams &chainparams) {
    if (!pblocktree->LoadBlockIndexGuts(InsertBlockIndex)) {
        return false;
    }

    boost::this_thread::interruption_point();

    // Calculate nChainWork
    std::vector<std::pair<int, CBlockIndex *>> vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    for (const std::pair<uint256, CBlockIndex *> &item : mapBlockIndex) {
        CBlockIndex *pindex = item.second;
        vSortedByHeight.push_back(std::make_pair(pindex->nHeight, pindex));
    }

    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    for (const std::pair<int, CBlockIndex *> &item : vSortedByHeight) {
        CBlockIndex *pindex = item.second;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) +
                             GetBlockProof(*pindex);
        pindex->nTimeMax =
            (pindex->pprev ? std::max(pindex->pprev->nTimeMax, pindex->nTime)
                           : pindex->nTime);
        // We can link the chain of blocks for which we've received transactions
        // at some point. Pruned nodes may have deleted the block.
        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx) {
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                } else {
                    pindex->nChainTx = 0;
                    mapBlocksUnlinked.insert(
                        std::make_pair(pindex->pprev, pindex));
                }
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }

        if (pindex->IsValid(BlockValidity::TRANSACTIONS) &&
            (pindex->nChainTx || pindex->pprev == nullptr)) {
            setBlockIndexCandidates.insert(pindex);
        }

        if (pindex->nStatus.isInvalid() &&
            (!pindexBestInvalid ||
             pindex->nChainWork > pindexBestInvalid->nChainWork)) {
            pindexBestInvalid = pindex;
        }

        if (pindex->nStatus.isOnParkedChain() &&
            (!pindexBestParked ||
             pindex->nChainWork > pindexBestParked->nChainWork)) {
            pindexBestParked = pindex;
        }

        if (pindex->pprev) {
            pindex->BuildSkip();
        }

        if (pindex->IsValid(BlockValidity::TREE) &&
            (pindexBestHeader == nullptr ||
             CBlockIndexWorkComparator()(pindexBestHeader, pindex))) {
            pindexBestHeader = pindex;
        }
    }

    // Load block file info
    pblocktree->ReadLastBlockFile(nLastBlockFile);
    vinfoBlockFile.resize(nLastBlockFile + 1);
    LogPrintf("%s: last block file = %i\n", __func__, nLastBlockFile);
    for (int nFile = 0; nFile <= nLastBlockFile; nFile++) {
        pblocktree->ReadBlockFileInfo(nFile, vinfoBlockFile[nFile]);
    }

    LogPrintf("%s: last block file info: %s\n", __func__,
              vinfoBlockFile[nLastBlockFile].ToString());

    for (int nFile = nLastBlockFile + 1; true; nFile++) {
        CBlockFileInfo info;
        if (pblocktree->ReadBlockFileInfo(nFile, info)) {
            vinfoBlockFile.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    std::set<int> setBlkDataFiles;
    for (const std::pair<uint256, CBlockIndex *> &item : mapBlockIndex) {
        CBlockIndex *pindex = item.second;
        if (pindex->nStatus.hasData()) {
            setBlkDataFiles.insert(pindex->nFile);
        }
    }

    for (const int i : setBlkDataFiles) {
        CDiskBlockPos pos(i, 0);
        if (CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION)
                .IsNull()) {
            return false;
        }
    }

    // Check whether we have ever pruned block & undo files
    pblocktree->ReadFlag("prunedblockfiles", fHavePruned);
    if (fHavePruned) {
        LogPrintf(
            "LoadBlockIndexDB(): Block files have previously been pruned\n");
    }

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    pblocktree->ReadReindexing(fReindexing);
    fReindex |= fReindexing;

    // Check whether we have a transaction index
    pblocktree->ReadFlag("txindex", fTxIndex);
    LogPrintf("%s: transaction index %s\n", __func__,
              fTxIndex ? "enabled" : "disabled");

    // Load pointer to end of best chain
    BlockMap::iterator it = mapBlockIndex.find(pcoinsTip->GetBestBlock());
    if (it == mapBlockIndex.end()) {
        return true;
    }
    chainActive.SetTip(it->second);

    PruneBlockIndexCandidates();

    LogPrintf(
        "%s: hashBestChain=%s height=%d date=%s progress=%f\n", __func__,
        chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(),
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S",
                          chainActive.Tip()->GetBlockTime()),
        GuessVerificationProgress(chainparams.TxData(), chainActive.Tip()));

    return true;
}

CVerifyDB::CVerifyDB() {
    uiInterface.ShowProgress(_("Verifying blocks..."), 0);
}

CVerifyDB::~CVerifyDB() {
    uiInterface.ShowProgress("", 100);
}

bool CVerifyDB::VerifyDB(const Config &config, CCoinsView *coinsview,
                         int nCheckLevel, int nCheckDepth) {
    LOCK(cs_main);
    if (chainActive.Tip() == nullptr || chainActive.Tip()->pprev == nullptr) {
        return true;
    }

    // Verify blocks in the best chain
    if (nCheckDepth <= 0) {
        // suffices until the year 19000
        nCheckDepth = 1000000000;
    }

    if (nCheckDepth > chainActive.Height()) {
        nCheckDepth = chainActive.Height();
    }

    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    LogPrintf("Verifying last %i blocks at level %i\n", nCheckDepth,
              nCheckLevel);

    const CChainParams &chainparams = config.GetChainParams();

    CCoinsViewCache coins(coinsview);
    CBlockIndex *pindexState = chainActive.Tip();
    CBlockIndex *pindexFailure = nullptr;
    int nGoodTransactions = 0;
    CValidationState state;
    int reportDone = 0;
    LogPrintf("[0%%]...");
    for (CBlockIndex *pindex = chainActive.Tip(); pindex && pindex->pprev;
         pindex = pindex->pprev) {
        boost::this_thread::interruption_point();
        int percentageDone = std::max(
            1, std::min(
                   99,
                   (int)(((double)(chainActive.Height() - pindex->nHeight)) /
                         (double)nCheckDepth * (nCheckLevel >= 4 ? 50 : 100))));

        if (reportDone < percentageDone / 10) {
            // report every 10% step
            LogPrintf("[%d%%]...", percentageDone);
            reportDone = percentageDone / 10;
        }

        uiInterface.ShowProgress(_("Verifying blocks..."), percentageDone);
        if (pindex->nHeight < chainActive.Height() - nCheckDepth) {
            break;
        }

        if (fPruneMode && !pindex->nStatus.hasData()) {
            // If pruning, only go back as far as we have data.
            LogPrintf("VerifyDB(): block verification stopping at height %d "
                      "(pruning, no data)\n",
                      pindex->nHeight);
            break;
        }

        CBlock block;

        // check level 0: read from disk
        if (!ReadBlockFromDisk(block, pindex, config)) {
            return error(
                "VerifyDB(): *** ReadBlockFromDisk failed at %d, hash=%s",
                pindex->nHeight, pindex->GetBlockHash().ToString());
        }

        // check level 1: verify block validity
        if (nCheckLevel >= 1 && !CheckBlock(config, block, state)) {
            return error("%s: *** found bad block at %d, hash=%s (%s)\n",
                         __func__, pindex->nHeight,
                         pindex->GetBlockHash().ToString(),
                         FormatStateMessage(state));
        }

        // check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            CDiskBlockPos pos = pindex->GetUndoPos();
            if (!pos.IsNull()) {
                if (!UndoReadFromDisk(undo, pos,
                                      pindex->pprev->GetBlockHash())) {
                    return error(
                        "VerifyDB(): *** found bad undo data at %d, hash=%s\n",
                        pindex->nHeight, pindex->GetBlockHash().ToString());
                }
            }
        }

        // check level 3: check for inconsistencies during memory-only
        // disconnect of tip blocks
        if (nCheckLevel >= 3 && pindex == pindexState &&
            (coins.DynamicMemoryUsage() + pcoinsTip->DynamicMemoryUsage()) <=
                nCoinCacheUsage) {
            assert(coins.GetBestBlock() == pindex->GetBlockHash());
            DisconnectResult res = DisconnectBlock(block, pindex, coins);
            if (res == DISCONNECT_FAILED) {
                return error("VerifyDB(): *** irrecoverable inconsistency in "
                             "block data at %d, hash=%s",
                             pindex->nHeight,
                             pindex->GetBlockHash().ToString());
            }

            pindexState = pindex->pprev;
            if (res == DISCONNECT_UNCLEAN) {
                nGoodTransactions = 0;
                pindexFailure = pindex;
            } else {
                nGoodTransactions += block.vtx.size();
            }
        }

        if (ShutdownRequested()) {
            return true;
        }
    }

    if (pindexFailure) {
        return error("VerifyDB(): *** coin database inconsistencies found "
                     "(last %i blocks, %i good transactions before that)\n",
                     chainActive.Height() - pindexFailure->nHeight + 1,
                     nGoodTransactions);
    }

    // check level 4: try reconnecting blocks
    if (nCheckLevel >= 4) {
        CBlockIndex *pindex = pindexState;
        while (pindex != chainActive.Tip()) {
            boost::this_thread::interruption_point();
            uiInterface.ShowProgress(
                _("Verifying blocks..."),
                std::max(1,
                         std::min(99,
                                  100 - (int)(((double)(chainActive.Height() -
                                                          pindex->nHeight)) /
                                                (double)nCheckDepth * 50))));
            pindex = chainActive.Next(pindex);
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex, config)) {
                return error(
                    "VerifyDB(): *** ReadBlockFromDisk failed at %d, hash=%s",
                    pindex->nHeight, pindex->GetBlockHash().ToString());
            }
            if (!ConnectBlock(config, block, state, pindex, coins,
                              chainparams)) {
                return error(
                    "VerifyDB(): *** found unconnectable block at %d, hash=%s",
                    pindex->nHeight, pindex->GetBlockHash().ToString());
            }
        }
    }

    LogPrintf("[DONE].\n");
    LogPrintf("No coin database inconsistencies in last %i blocks (%i "
              "transactions)\n",
              chainActive.Height() - pindexState->nHeight, nGoodTransactions);

    return true;
}

bool RewindBlockIndex(const Config &config) {
    LOCK(cs_main);

    int nHeight = chainActive.Height() + 1;

    // nHeight is now the height of the first insufficiently-validated block, or
    // tipheight + 1
    CValidationState state;
    CBlockIndex *pindex = chainActive.Tip();
    while (chainActive.Height() >= nHeight) {
        if (fPruneMode && !chainActive.Tip()->nStatus.hasData()) {
            // If pruning, don't try rewinding past the HAVE_DATA point; since
            // older blocks can't be served anyway, there's no need to walk
            // further, and trying to DisconnectTip() will fail (and require a
            // needless reindex/redownload of the blockchain).
            break;
        }
        
        if (!DisconnectTip(config, state, true)) {
            return error(
                "RewindBlockIndex: unable to disconnect block at height %i",
                pindex->nHeight);
        }

        // Occasionally flush state to disk.
        if (!FlushStateToDisk(state, FLUSH_STATE_PERIODIC)) {
            return false;
        }
    }

    // Reduce validity flag and have-data flags.
    // We do this after actual disconnecting, otherwise we'll end up writing the
    // lack of data to disk before writing the chainstate, resulting in a
    // failure to continue if interrupted.
    for (const std::pair<uint256, CBlockIndex *> &p : mapBlockIndex) {
        CBlockIndex *pindexIter = p.second;
        if (pindexIter->IsValid(BlockValidity::TRANSACTIONS) &&
            pindexIter->nChainTx) {
            setBlockIndexCandidates.insert(pindexIter);
        }
    }

    if (chainActive.Tip() != nullptr) {
        // We can't prune block index candidates based on our tip if we have
        // no tip due to chainActive being empty!
        PruneBlockIndexCandidates();
        
        CheckBlockIndex(config.GetChainParams().GetConsensus());
        
        // FlushStateToDisk can possibly read chainActive. Be conservative
        // and skip it here, we're about to -reindex-chainstate anyway, so
        // it'll get called a bunch real soon.
        if (!FlushStateToDisk(state, FLUSH_STATE_ALWAYS)) {
            return false;
        }
    }

    return true;
}

// May NOT be used after any connections are up as much of the peer-processing
// logic assumes a consistent block index state
void UnloadBlockIndex() {
    LOCK(cs_main);
    setBlockIndexCandidates.clear();
    chainActive.SetTip(nullptr);
    pindexFinalized = nullptr;
    pindexBestInvalid = nullptr;
    pindexBestParked = nullptr;
    pindexBestHeader = nullptr;
    mempool.clear();
    mapBlocksUnlinked.clear();
    vinfoBlockFile.clear();
    nLastBlockFile = 0;
    nBlockSequenceId = 1;
    setDirtyBlockIndex.clear();
    setDirtyFileInfo.clear();
    versionbitscache.Clear();
    for (int b = 0; b < VERSIONBITS_NUM_BITS; b++) {
        warningcache[b].clear();
    }

    for (BlockMap::value_type &entry : mapBlockIndex) {
        delete entry.second;
    }

    mapBlockIndex.clear();
    fHavePruned = false;
}

bool LoadBlockIndex(const CChainParams &chainparams) {
    // Load block index from databases
    if (!fReindex && !LoadBlockIndexDB(chainparams)) {
        return false;
    }
    return true;
}

bool InitBlockIndex(const Config &config) {
    LOCK(cs_main);

    // Check whether we're already initialized
    if (chainActive.Genesis() != nullptr) {
        return true;
    }

    // Use the provided setting for -txindex in the new database
    fTxIndex = GetBoolArg("-txindex", DEFAULT_TXINDEX);
    pblocktree->WriteFlag("txindex", fTxIndex);
    LogPrintf("Initializing databases...\n");

    // Only add the genesis block if not reindexing (in which case we reuse the
    // one already on disk)
    if (!fReindex) {
        try {
            const CChainParams &chainparams = config.GetChainParams();
            CBlock &block = const_cast<CBlock &>(chainparams.GenesisBlock());
            // Start new block file
            unsigned int nBlockSize =
                ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
            CDiskBlockPos blockPos;
            CValidationState state;
            if (!FindBlockPos(state, blockPos, nBlockSize + 8, 0,
                              block.GetBlockTime())) {
                return error("LoadBlockIndex(): FindBlockPos failed");
            }
            if (!WriteBlockToDisk(block, blockPos, chainparams.DiskMagic())) {
                return error(
                    "LoadBlockIndex(): writing genesis block to disk failed");
            }
            CBlockIndex *pindex = AddToBlockIndex(block);
            if (!ReceivedBlockTransactions(block, state, pindex, blockPos)) {
                return error("LoadBlockIndex(): genesis block not accepted");
            }
            // Force a chainstate write so that when we VerifyDB in a moment, it
            // doesn't check stale data
            return FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
        } catch (const std::runtime_error &e) {
            return error(
                "LoadBlockIndex(): failed to initialize block database: %s",
                e.what());
        }
    }

    return true;
}

bool LoadExternalBlockFile(const Config &config, FILE *fileIn,
                           CDiskBlockPos *dbp) {
    // Map of disk positions for blocks with unknown parent (only used for
    // reindex)
    static std::multimap<uint256, CDiskBlockPos> mapBlocksUnknownParent;
    int64_t nStart = GetTimeMillis();

    const CChainParams &chainparams = config.GetChainParams();

    int nLoaded = 0;
    try {
        // This takes over fileIn and calls fclose() on it in the CBufferedFile
        // destructor. Make sure we have at least 2*MAX_TX_SIZE space in there
        // so any transaction can fit in the buffer.
        CBufferedFile blkdat(fileIn, 2 * MAX_TX_SIZE, MAX_TX_SIZE + 8, SER_DISK,
                             CLIENT_VERSION);
        uint64_t nRewind = blkdat.GetPos();
        while (!blkdat.eof()) {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            // Start one byte further next time, in case of failure.
            nRewind++;
            // Remove former limit.
            blkdat.SetLimit();
            unsigned int nSize = 0;
            try {
                // Locate a header.
                uint8_t buf[CMessageHeader::MESSAGE_START_SIZE];
                blkdat.FindByte(chainparams.DiskMagic()[0]);
                nRewind = blkdat.GetPos() + 1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, std::begin(chainparams.DiskMagic()),
                           CMessageHeader::MESSAGE_START_SIZE)) {
                    continue;
                }

                // Read size.
                blkdat >> nSize;
                if (nSize < 80) {
                    continue;
                }
            } catch (const std::exception &) {
                // No valid block header found; don't complain.
                break;
            }

            try {
                // read block
                uint64_t nBlockPos = blkdat.GetPos();
                if (dbp) {
                    dbp->nPos = nBlockPos;
                }
                blkdat.SetLimit(nBlockPos + nSize);
                blkdat.SetPos(nBlockPos);
                std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
                CBlock &block = *pblock;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // detect out of order blocks, and store them for later
                uint256 hash = block.GetHash();
                if (hash != chainparams.GetConsensus().hashGenesisBlock &&
                    mapBlockIndex.find(block.hashPrevBlock) ==
                        mapBlockIndex.end()) {
                    LogPrint("reindex",
                             "%s: Out of order block %s, parent %s not known\n",
                             __func__, hash.ToString(),
                             block.hashPrevBlock.ToString());
                    if (dbp) {
                        mapBlocksUnknownParent.insert(
                            std::make_pair(block.hashPrevBlock, *dbp));
                    }
                    continue;
                }

                // process in case the block isn't known yet
                if (mapBlockIndex.count(hash) == 0 ||
                    !mapBlockIndex[hash]->nStatus.hasData()) {
                    LOCK(cs_main);
                    CValidationState state;
                    if (AcceptBlock(config, pblock, state, nullptr, true, dbp,
                                    nullptr)) {
                        nLoaded++;
                    }

                    if (state.IsError()) {
                        break;
                    }
                } else if (hash !=
                               chainparams.GetConsensus().hashGenesisBlock &&
                           mapBlockIndex[hash]->nHeight % 1000 == 0) {
                    LogPrint(
                        "reindex",
                        "Block Import: already had block %s at height %d\n",
                        hash.ToString(), mapBlockIndex[hash]->nHeight);
                }

                // Activate the genesis block so normal node progress can
                // continue
                if (hash == chainparams.GetConsensus().hashGenesisBlock) {
                    CValidationState state;
                    if (!ActivateBestChain(config, state)) {
                        break;
                    }
                }

                NotifyHeaderTip();

                // Recursively process earlier encountered successors of this
                // block
                std::deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty()) {
                    uint256 head = queue.front();
                    queue.pop_front();
                    std::pair<std::multimap<uint256, CDiskBlockPos>::iterator,
                              std::multimap<uint256, CDiskBlockPos>::iterator>
                        range = mapBlocksUnknownParent.equal_range(head);
                    while (range.first != range.second) {
                        std::multimap<uint256, CDiskBlockPos>::iterator it =
                            range.first;
                        std::shared_ptr<CBlock> pblockrecursive =
                            std::make_shared<CBlock>();
                        if (ReadBlockFromDisk(*pblockrecursive, it->second,
                                              config)) {
                            LogPrint(
                                "reindex",
                                "%s: Processing out of order child %s of %s\n",
                                __func__, pblockrecursive->GetHash().ToString(),
                                head.ToString());
                            LOCK(cs_main);
                            CValidationState dummy;
                            if (AcceptBlock(config, pblockrecursive, dummy,
                                            nullptr, true, &it->second,
                                            nullptr)) {
                                nLoaded++;
                                queue.push_back(pblockrecursive->GetHash());
                            }
                        }
                        range.first++;
                        mapBlocksUnknownParent.erase(it);
                        NotifyHeaderTip();
                    }
                }
            } catch (const std::exception &e) {
                LogPrintf("%s: Deserialize or I/O error - %s\n", __func__,
                          e.what());
            }
        }
    } catch (const std::runtime_error &e) {
        AbortNode(std::string("System error: ") + e.what());
    }

    if (nLoaded > 0) {
        LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded,
                  GetTimeMillis() - nStart);
    }

    return nLoaded > 0;
}

static void CheckBlockIndex(const Consensus::Params &consensusParams) {
    if (!fCheckBlockIndex) {
        return;
    }

    LOCK(cs_main);

    // During a reindex, we read the genesis block and call CheckBlockIndex
    // before ActivateBestChain, so we have the genesis block in mapBlockIndex
    // but no active chain. (A few of the tests when iterating the block tree
    // require that chainActive has been initialized.)
    if (chainActive.Height() < 0) {
        assert(mapBlockIndex.size() <= 1);
        return;
    }

    // Build forward-pointing map of the entire block tree.
    std::multimap<CBlockIndex *, CBlockIndex *> forward;
    for (const std::pair<const uint256, CBlockIndex *> &it : mapBlockIndex) {
        forward.emplace(it.second->pprev, it.second);
    }

    assert(forward.size() == mapBlockIndex.size());

    std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
              std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
        rangeGenesis = forward.equal_range(nullptr);
    CBlockIndex *pindex = rangeGenesis.first->second;
    rangeGenesis.first++;
    // There is only one index entry with parent nullptr.
    assert(rangeGenesis.first == rangeGenesis.second);

    // Iterate over the entire block tree, using depth-first search.
    // Along the way, remember whether there are blocks on the path from genesis
    // block being explored which are the first to have certain properties.
    size_t nNodes = 0;
    int nHeight = 0;
    // Oldest ancestor of pindex which is invalid.
    CBlockIndex *pindexFirstInvalid = nullptr;
    // Oldest ancestor of pindex which is parked.
    CBlockIndex *pindexFirstParked = nullptr;
    // Oldest ancestor of pindex which does not have data available.
    CBlockIndex *pindexFirstMissing = nullptr;
    // Oldest ancestor of pindex for which nTx == 0.
    CBlockIndex *pindexFirstNeverProcessed = nullptr;
    // Oldest ancestor of pindex which does not have BLOCK_VALID_TREE
    // (regardless of being valid or not).
    CBlockIndex *pindexFirstNotTreeValid = nullptr;
    // Oldest ancestor of pindex which does not have BLOCK_VALID_TRANSACTIONS
    // (regardless of being valid or not).
    CBlockIndex *pindexFirstNotTransactionsValid = nullptr;
    // Oldest ancestor of pindex which does not have BLOCK_VALID_CHAIN
    // (regardless of being valid or not).
    CBlockIndex *pindexFirstNotChainValid = nullptr;
    // Oldest ancestor of pindex which does not have BLOCK_VALID_SCRIPTS
    // (regardless of being valid or not).
    CBlockIndex *pindexFirstNotScriptsValid = nullptr;
    while (pindex != nullptr) {
        nNodes++;
        if (pindexFirstInvalid == nullptr && pindex->nStatus.hasFailed()) {
            pindexFirstInvalid = pindex;
        }
        if (pindexFirstParked == nullptr && pindex->nStatus.isParked()) {
            pindexFirstParked = pindex;
        }
        if (pindexFirstMissing == nullptr && !pindex->nStatus.hasData()) {
            pindexFirstMissing = pindex;
        }
        if (pindexFirstNeverProcessed == nullptr && pindex->nTx == 0) {
            pindexFirstNeverProcessed = pindex;
        }
        if (pindex->pprev != nullptr && pindexFirstNotTreeValid == nullptr &&
            pindex->nStatus.getValidity() < BlockValidity::TREE) {
            pindexFirstNotTreeValid = pindex;
        }
        if (pindex->pprev != nullptr &&
            pindexFirstNotTransactionsValid == nullptr &&
            pindex->nStatus.getValidity() < BlockValidity::TRANSACTIONS) {
            pindexFirstNotTransactionsValid = pindex;
        }
        if (pindex->pprev != nullptr && pindexFirstNotChainValid == nullptr &&
            pindex->nStatus.getValidity() < BlockValidity::CHAIN) {
            pindexFirstNotChainValid = pindex;
        }
        if (pindex->pprev != nullptr && pindexFirstNotScriptsValid == nullptr &&
            pindex->nStatus.getValidity() < BlockValidity::SCRIPTS) {
            pindexFirstNotScriptsValid = pindex;
        }

        // Begin: actual consistency checks.
        if (pindex->pprev == nullptr) {
            // Genesis block checks.
            // Genesis block's hash must match.
            assert(pindex->GetBlockHash() == consensusParams.hashGenesisBlock);
            // The current active chain's genesis block must be this block.
            assert(pindex == chainActive.Genesis());
        }
        if (pindex->nChainTx == 0) {
            // nSequenceId can't be set positive for blocks that aren't linked
            // (negative is used for preciousblock)
            assert(pindex->nSequenceId <= 0);
        }
        // VALID_TRANSACTIONS is equivalent to nTx > 0 for all nodes (whether or
        // not pruning has occurred). HAVE_DATA is only equivalent to nTx > 0
        // (or VALID_TRANSACTIONS) if no pruning has occurred.
        if (!fHavePruned) {
            // If we've never pruned, then HAVE_DATA should be equivalent to nTx
            // > 0
            assert(pindex->nStatus.hasData() == (pindex->nTx > 0));
            assert(pindexFirstMissing == pindexFirstNeverProcessed);
        } else if (pindex->nStatus.hasData()) {
            // If we have pruned, then we can only say that HAVE_DATA implies
            // nTx > 0
            assert(pindex->nTx > 0);
        }
        if (pindex->nStatus.hasUndo()) {
            assert(pindex->nStatus.hasData());
        }
        // This is pruning-independent.
        assert((pindex->nStatus.getValidity() >= BlockValidity::TRANSACTIONS) ==
               (pindex->nTx > 0));
        // All parents having had data (at some point) is equivalent to all
        // parents being VALID_TRANSACTIONS, which is equivalent to nChainTx
        // being set.
        // nChainTx != 0 is used to signal that all parent blocks have been
        // processed (but may have been pruned).
        assert((pindexFirstNeverProcessed != nullptr) ==
               (pindex->nChainTx == 0));
        assert((pindexFirstNotTransactionsValid != nullptr) ==
               (pindex->nChainTx == 0));
        // nHeight must be consistent.
        assert(pindex->nHeight == nHeight);
        // For every block except the genesis block, the chainwork must be
        // larger than the parent's.
        assert(pindex->pprev == nullptr ||
               pindex->nChainWork >= pindex->pprev->nChainWork);
        // The pskip pointer must point back for all but the first 2 blocks.
        assert(nHeight < 2 ||
               (pindex->pskip && (pindex->pskip->nHeight < nHeight)));
        // All mapBlockIndex entries must at least be TREE valid
        assert(pindexFirstNotTreeValid == nullptr);
        if (pindex->nStatus.getValidity() >= BlockValidity::TREE) {
            // TREE valid implies all parents are TREE valid
            assert(pindexFirstNotTreeValid == nullptr);
        }
        if (pindex->nStatus.getValidity() >= BlockValidity::CHAIN) {
            // CHAIN valid implies all parents are CHAIN valid
            assert(pindexFirstNotChainValid == nullptr);
        }
        if (pindex->nStatus.getValidity() >= BlockValidity::SCRIPTS) {
            // SCRIPTS valid implies all parents are SCRIPTS valid
            assert(pindexFirstNotScriptsValid == nullptr);
        }
        if (pindexFirstInvalid == nullptr) {
            // Checks for not-invalid blocks.
            // The failed mask cannot be set for blocks without invalid parents.
            assert(!pindex->nStatus.isInvalid());
        }
        if (pindexFirstParked == nullptr) {
            // Checks for not-invalid blocks.
            // The failed mask cannot be set for blocks without invalid parents.
            assert(!pindex->nStatus.isOnParkedChain());
        }
        if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) &&
            pindexFirstNeverProcessed == nullptr) {
            if (pindexFirstInvalid == nullptr) {
                // If this block sorts at least as good as the current tip and
                // is valid and we have all data for its parents, it must be in
                // setBlockIndexCandidates or be parked.
                if (pindexFirstMissing == nullptr) {
                    assert(pindex->nStatus.isOnParkedChain() ||
                           setBlockIndexCandidates.count(pindex));
                }
                // chainActive.Tip() must also be there even if some data has
                // been pruned.
                if (pindex == chainActive.Tip()) {
                    assert(setBlockIndexCandidates.count(pindex));
                }
                // If some parent is missing, then it could be that this block
                // was in setBlockIndexCandidates but had to be removed because
                // of the missing data. In this case it must be in
                // mapBlocksUnlinked -- see test below.
            }
        } else {
            // If this block sorts worse than the current tip or some ancestor's
            // block has never been seen, it cannot be in
            // setBlockIndexCandidates.
            assert(setBlockIndexCandidates.count(pindex) == 0);
        }
        // Check whether this block is in mapBlocksUnlinked.
        std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
                  std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
            rangeUnlinked = mapBlocksUnlinked.equal_range(pindex->pprev);
        bool foundInUnlinked = false;
        while (rangeUnlinked.first != rangeUnlinked.second) {
            assert(rangeUnlinked.first->first == pindex->pprev);
            if (rangeUnlinked.first->second == pindex) {
                foundInUnlinked = true;
                break;
            }
            rangeUnlinked.first++;
        }
        if (pindex->pprev && pindex->nStatus.hasData() &&
            pindexFirstNeverProcessed != nullptr &&
            pindexFirstInvalid == nullptr) {
            // If this block has block data available, some parent was never
            // received, and has no invalid parents, it must be in
            // mapBlocksUnlinked.
            assert(foundInUnlinked);
        }
        if (!pindex->nStatus.hasData()) {
            // Can't be in mapBlocksUnlinked if we don't HAVE_DATA
            assert(!foundInUnlinked);
        }
        if (pindexFirstMissing == nullptr) {
            // We aren't missing data for any parent -- cannot be in
            // mapBlocksUnlinked.
            assert(!foundInUnlinked);
        }
        if (pindex->pprev && pindex->nStatus.hasData() &&
            pindexFirstNeverProcessed == nullptr &&
            pindexFirstMissing != nullptr) {
            // We HAVE_DATA for this block, have received data for all parents
            // at some point, but we're currently missing data for some parent.
            // We must have pruned.
            assert(fHavePruned);
            // This block may have entered mapBlocksUnlinked if:
            //  - it has a descendant that at some point had more work than the
            //    tip, and
            //  - we tried switching to that descendant but were missing
            //    data for some intermediate block between chainActive and the
            //    tip.
            // So if this block is itself better than chainActive.Tip() and it
            // wasn't in
            // setBlockIndexCandidates, then it must be in mapBlocksUnlinked.
            if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) &&
                setBlockIndexCandidates.count(pindex) == 0) {
                if (pindexFirstInvalid == nullptr) {
                    assert(foundInUnlinked);
                }
            }
        }
        // Perhaps too slow
        // assert(pindex->GetBlockHash() == pindex->GetBlockHeader().GetHash());
        // End: actual consistency checks.

        // Try descending into the first subnode.
        std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
                  std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
            range = forward.equal_range(pindex);
        if (range.first != range.second) {
            // A subnode was found.
            pindex = range.first->second;
            nHeight++;
            continue;
        }
        // This is a leaf node. Move upwards until we reach a node of which we
        // have not yet visited the last child.
        while (pindex) {
            // We are going to either move to a parent or a sibling of pindex.
            // If pindex was the first with a certain property, unset the
            // corresponding variable.
            if (pindex == pindexFirstInvalid) {
                pindexFirstInvalid = nullptr;
            }
            if (pindex == pindexFirstParked) {
                pindexFirstParked = nullptr;
            }
            if (pindex == pindexFirstMissing) {
                pindexFirstMissing = nullptr;
            }
            if (pindex == pindexFirstNeverProcessed) {
                pindexFirstNeverProcessed = nullptr;
            }
            if (pindex == pindexFirstNotTreeValid) {
                pindexFirstNotTreeValid = nullptr;
            }
            if (pindex == pindexFirstNotTransactionsValid) {
                pindexFirstNotTransactionsValid = nullptr;
            }
            if (pindex == pindexFirstNotChainValid) {
                pindexFirstNotChainValid = nullptr;
            }
            if (pindex == pindexFirstNotScriptsValid) {
                pindexFirstNotScriptsValid = nullptr;
            }
            // Find our parent.
            CBlockIndex *pindexPar = pindex->pprev;
            // Find which child we just visited.
            std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
                      std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
                rangePar = forward.equal_range(pindexPar);
            while (rangePar.first->second != pindex) {
                // Our parent must have at least the node we're coming from as
                // child.
                assert(rangePar.first != rangePar.second);
                rangePar.first++;
            }
            // Proceed to the next one.
            rangePar.first++;
            if (rangePar.first != rangePar.second) {
                // Move to the sibling.
                pindex = rangePar.first->second;
                break;
            } else {
                // Move up further.
                pindex = pindexPar;
                nHeight--;
                continue;
            }
        }
    }

    // Check that we actually traversed the entire map.
    assert(nNodes == forward.size());
}

std::string CBlockFileInfo::ToString() const {
    return strprintf(
        "CBlockFileInfo(blocks=%u, size=%u, heights=%u...%u, time=%s...%s)",
        nBlocks, nSize, nHeightFirst, nHeightLast,
        DateTimeStrFormat("%Y-%m-%d", nTimeFirst),
        DateTimeStrFormat("%Y-%m-%d", nTimeLast));
}

CBlockFileInfo *GetBlockFileInfo(size_t n) {
    return &vinfoBlockFile.at(n);
}

ThresholdState VersionBitsTipState(const Consensus::Params &params,
                                   Consensus::DeploymentPos pos) {
    LOCK(cs_main);
    return VersionBitsState(chainActive.Tip(), params, pos, versionbitscache);
}

int VersionBitsTipStateSinceHeight(const Consensus::Params &params,
                                   Consensus::DeploymentPos pos) {
    LOCK(cs_main);
    return VersionBitsStateSinceHeight(chainActive.Tip(), params, pos,
                                       versionbitscache);
}

static const uint64_t MEMPOOL_DUMP_VERSION = 1;

bool LoadMempool(const Config &config) {
    int64_t nExpiryTimeout =
        GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60;
    FILE *filestr = fopen((GetDataDir() / "mempool.dat").string().c_str(), "rb");
    CAutoFile file(filestr, SER_DISK, CLIENT_VERSION);
    if (file.IsNull()) {
        LogPrintf(
            "Failed to open mempool file from disk. Continuing anyway.\n");
        return false;
    }

    int64_t count = 0;
    int64_t skipped = 0;
    int64_t failed = 0;
    int64_t nNow = GetTime();

    try {
        uint64_t version;
        file >> version;
        if (version != MEMPOOL_DUMP_VERSION) {
            return false;
        }

        uint64_t num;
        file >> num;
        double prioritydummy = 0;
        while (num--) {
            CTransactionRef tx;
            int64_t nTime;
            int64_t nFeeDelta;
            file >> tx;
            file >> nTime;
            file >> nFeeDelta;

            Amount amountdelta(nFeeDelta);
            if (amountdelta != Amount(0)) {
                mempool.PrioritiseTransaction(tx->GetId(),
                                              tx->GetId().ToString(),
                                              prioritydummy, amountdelta);
            }
            CValidationState state;
            if (nTime + nExpiryTimeout > nNow) {
                LOCK(cs_main);
                AcceptToMemoryPoolWithTime(config, mempool, state, tx, true,
                                           nullptr, nTime);
                if (state.IsValid()) {
                    ++count;
                } else {
                    ++failed;
                }
            } else {
                ++skipped;
            }

            if (ShutdownRequested()) {
                return false;
            }
        }
        std::map<uint256, Amount> mapDeltas;
        file >> mapDeltas;

        for (const auto &i : mapDeltas) {
            mempool.PrioritiseTransaction(i.first, i.first.ToString(),
                                          prioritydummy, i.second);
        }
    } catch (const std::exception &e) {
        LogPrintf("Failed to deserialize mempool data on disk: %s. Continuing "
                  "anyway.\n",
                  e.what());
        return false;
    }

    LogPrintf("Imported mempool transactions from disk: %i successes, %i "
              "failed, %i expired\n",
              count, failed, skipped);
    return true;
}

void DumpMempool(void) {
    int64_t start = GetTimeMicros();

    std::map<uint256, Amount> mapDeltas;
    std::vector<TxMempoolInfo> vinfo;

    {
        LOCK(mempool.cs);
        for (const auto &i : mempool.mapDeltas) {
            mapDeltas[i.first] = i.second.second;
        }

        vinfo = mempool.infoAll();
    }

    int64_t mid = GetTimeMicros();

    try {
        FILE *filestr = fopen((GetDataDir() / "mempool.dat.new").string().c_str(), "wb");
        if (!filestr) {
            return;
        }

        CAutoFile file(filestr, SER_DISK, CLIENT_VERSION);

        uint64_t version = MEMPOOL_DUMP_VERSION;
        file << version;

        file << uint64_t(vinfo.size());
        for (const auto &i : vinfo) {
            file << *(i.tx);
            file << int64_t(i.nTime);
            file << i.nFeeDelta; // int64_t(i.nFeeDelta.GetSatoshis());
            mapDeltas.erase(i.tx->GetId());
        }

        file << mapDeltas;
        FileCommit(file.Get());
        file.fclose();
        RenameOver(GetDataDir() / "mempool.dat.new",
                   GetDataDir() / "mempool.dat");
        int64_t last = GetTimeMicros();
        LogPrintf("Dumped mempool: %gs to copy, %gs to dump\n",
                  (mid - start) * 0.000001, (last - mid) * 0.000001);
    } catch (const std::exception &e) {
        LogPrintf("Failed to dump mempool: %s. Continuing anyway.\n", e.what());
    }
}

//! Guess how far we are in the verification process at the given block index
double GuessVerificationProgress(const ChainTxData &data, CBlockIndex *pindex) {
    if (pindex == nullptr) {
        return 0.0;
    }

    int64_t nNow = time(nullptr);

    double fTxTotal;
    if (pindex->nChainTx <= data.nTxCount) {
        fTxTotal = data.nTxCount + (nNow - data.nTime) * data.dTxRate;
    } else {
        fTxTotal =
            pindex->nChainTx + (nNow - pindex->GetBlockTime()) * data.dTxRate;
    }

    return pindex->nChainTx / fTxTotal;
}

class CMainCleanup {
public:
    CMainCleanup() {}
    ~CMainCleanup() {
        // block headers
        for (const std::pair<const uint256, CBlockIndex *> &it :
             mapBlockIndex) {
            delete it.second;
        }
        mapBlockIndex.clear();
    }
} instance_of_cmaincleanup;
