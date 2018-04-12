// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pow.h"

#include "arith_uint256.h"
#include "chain.h"
#include "chainparams.h"
#include "config.h"
#include "consensus/params.h"
//#include "chainparams.h"
#include "crypto/equihash.h"
#include "primitives/block.h"
#include "streams.h"
#include "uint256.h"
#include "util.h"
#include "validation.h"

unsigned int BitcoinGetNextWorkRequired(const CBlockIndex* pindexPrev, const CBlockHeader *pblock, const Consensus::Params& params);

/**
 * Compute the next required proof of work using the legacy Bitcoin difficulty
 * adjustment + Emergency Difficulty Adjustment (EDA).
 */
static uint32_t GetNextEDAWorkRequired(const CBlockIndex *pindexPrev,
                                       const CBlockHeader *pblock,
                                       const Config &config) {
    const Consensus::Params &params = config.GetChainParams().GetConsensus();

    // Only change once per difficulty adjustment interval
    uint32_t nHeight = pindexPrev->nHeight + 1;
    if (nHeight % params.DifficultyAdjustmentInterval() == 0) {
        // Go back by what we want to be 14 days worth of blocks
        assert(nHeight >= params.DifficultyAdjustmentInterval());
        uint32_t nHeightFirst = nHeight - params.DifficultyAdjustmentInterval();
        const CBlockIndex *pindexFirst = pindexPrev->GetAncestor(nHeightFirst);
        assert(pindexFirst);

        return CalculateBCCNextWorkRequired(pindexPrev,
                                         pindexFirst->GetBlockTime(), params);
    }

    const uint32_t nProofOfWorkLimit =
        UintToArith256(params.PowLimit(false)).GetCompact();

    if (params.fPowAllowMinDifficultyBlocks) {
        // Special difficulty rule for testnet:
        // If the new block's timestamp is more than 2* 10 minutes then allow
        // mining of a min-difficulty block.
        if (pblock->GetBlockTime() >
            pindexPrev->GetBlockTime() + 2 * params.nPowTargetSpacing) {
            return nProofOfWorkLimit;
        }

        // Return the last non-special-min-difficulty-rules-block
        const CBlockIndex *pindex = pindexPrev;
        while (pindex->pprev &&
               pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 &&
               pindex->nBits == nProofOfWorkLimit) {
            pindex = pindex->pprev;
        }

        return pindex->nBits;
    }

    // We can't go below the minimum, so bail early.
    uint32_t nBits = pindexPrev->nBits;
    if (nBits == nProofOfWorkLimit) {
        return nProofOfWorkLimit;
    }

    // If producing the last 6 blocks took less than 12h, we keep the same
    // difficulty.
    const CBlockIndex *pindex6 = pindexPrev->GetAncestor(nHeight - 7);
    assert(pindex6);
    int64_t mtp6blocks =
        pindexPrev->GetMedianTimePast() - pindex6->GetMedianTimePast();
    if (mtp6blocks < 12 * 3600) {
        return nBits;
    }

    // If producing the last 6 blocks took more than 12h, increase the
    // difficulty target by 1/4 (which reduces the difficulty by 20%).
    // This ensures that the chain does not get stuck in case we lose
    // hashrate abruptly.
    arith_uint256 nPow;
    nPow.SetCompact(nBits);
    nPow += (nPow >> 2);

    // Make sure we do not go below allowed values.
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    if (nPow > bnPowLimit) nPow = bnPowLimit;

    return nPow.GetCompact();
}


unsigned int LwmaGetNextWorkRequired(const CBlockIndex* pindexPrev, const CBlockHeader *pblock, const Consensus::Params& params)
{
    // Special difficulty rule for testnet:
    // If the new block's timestamp is more than 2 * 10 minutes
    // then allow mining of a min-difficulty block.
//    if (params.fPowAllowMinDifficultyBlocks &&
  //      pblock->GetBlockTime() > pindexPrev->GetBlockTime() + params.nPowTargetSpacingCDY * 2) {
    //    return UintToArith256(params.PowLimit(true)).GetCompact();
   // }
    return LwmaCalculateNextWorkRequired(pindexPrev, params);
}

unsigned int LwmaCalculateNextWorkRequired(const CBlockIndex* pindexPrev, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting) {
        return pindexPrev->nBits;
    }

    const int N = params.nZawyLwmaAveragingWindow;  
    const int T = params.nPowTargetSpacingCDY; //2 minutes
    const int height = pindexPrev->nHeight + 1;
    double adjust = 1;//0.998;
    
    assert(height > N);

    arith_uint256 sum_target;
    int sum_time = 0, nWeight = 0;

    // Loop through N most recent blocks.
    for (int i = height - N; i < height; i++) {
        const CBlockIndex* block = pindexPrev->GetAncestor(i);
        const CBlockIndex* block_Prev = block->GetAncestor(i - 1);
        int64_t solvetime = block->GetBlockTime() - block_Prev->GetBlockTime();

        nWeight++;
        sum_time += solvetime * nWeight;  // Weighted solvetime sum. The nearsest blocks get the most weight. 
        
        // Target sum divided by a factor, (k N^2).
        // The factor is a part of the final equation. However we divide sum_target here to avoid
        // potential overflow.
        arith_uint256 target;
        target.SetCompact(block->nBits);
        sum_target += target ;   // (k * N * N);
    }
    
    
    // Keep t reasonable in case strange solvetimes occurred.
    if (sum_time < N * N * T / 20) {
        sum_time = N * N * T / 20;
    }

    const arith_uint256 pow_limit = UintToArith256(params.PowLimit(true));

    arith_uint256 next_target = 2 * (sum_time/(N*(N+1)))* (sum_target/N) * adjust/T; // next_target = LWMA * avgTarget * adjust /T;
    
    
    if (next_target > pow_limit ){
        return pow_limit.GetCompact();
    }
     

    return next_target.GetCompact();
}

uint32_t GetNextWorkRequired(const CBlockIndex *pindexPrev,
                             const CBlockHeader *pblock, const Config &config) {
    const Consensus::Params &params = config.GetChainParams().GetConsensus();
    assert(pindexPrev != nullptr);
    int nHeight = pindexPrev->nHeight + 1;
    bool postfork = nHeight >= params.cdyHeight;
    unsigned int nProofOfWorkLimit = UintToArith256(params.PowLimit(postfork)).GetCompact();
    // Genesis block
    if (pindexPrev == nullptr) {
        return UintToArith256(params.powLimit).GetCompact();
    }
    
    if (postfork == false) {
       // LogPrintf("Before hard fork, get bitcoin NextWorkRequired.\n");
        if (IsDAAEnabled(config, pindexPrev)) {
            return GetNextCashWorkRequired(pindexPrev, pblock, config);
        }
        else if (IsUAHFenabled(config, pindexPrev))
            return GetNextEDAWorkRequired(pindexPrev, pblock, config);
        else
            return BitcoinGetNextWorkRequired(pindexPrev, pblock, params);
    }
    else if (nHeight< params.cdyHeight + params.nDigishieldAveragingWindow)  //Yang our fork start with small pow
    {
        return nProofOfWorkLimit;
    }
    else if (nHeight < params.CDYZawyLWMAHeight) {
        // Regular Digishield v3.
        return DigishieldGetNextWorkRequired(pindexPrev, pblock, params);
    } else {
        // Zawy's LWMA.
        return LwmaGetNextWorkRequired(pindexPrev, pblock, params);
    }
    
    // Special rule for regtest: we never retarget.
}


unsigned int DigishieldGetNextWorkRequired(const CBlockIndex* pindexPrev, const CBlockHeader *pblock,
                                           const Consensus::Params& params)
{
    assert(pindexPrev != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.PowLimit(true)).GetCompact();  // Always postfork.

   // Special rule for regtest: we never retarget.
    if (params.fPowNoRetargeting) {
        return pindexPrev->nBits;
    }

    const CBlockIndex* pindexFirst = pindexPrev;
    arith_uint256 bnTot {0};
    for (int i = 0; pindexFirst && i < params.nDigishieldAveragingWindow; i++) {
        arith_uint256 bnTmp;
        bnTmp.SetCompact(pindexFirst->nBits);
        bnTot += bnTmp;
        pindexFirst = pindexFirst->pprev;
    }

    if (pindexFirst == NULL)
        return nProofOfWorkLimit;

    arith_uint256 bnAvg {bnTot / params.nDigishieldAveragingWindow};


    return DigishieldCalculateNextWorkRequired(bnAvg, pindexPrev->GetMedianTimePast(), pindexFirst->GetMedianTimePast(), params);
}

unsigned int DigishieldCalculateNextWorkRequired(arith_uint256 bnAvg, int64_t nLastBlockTime, int64_t nFirstBlockTime, const Consensus::Params& params)
{

    // Limit adjustment
    int64_t nActualTimespan = nLastBlockTime - nFirstBlockTime;

    if (nActualTimespan < params.DigishieldMinActualTimespan())
        nActualTimespan = params.DigishieldMinActualTimespan();
    if (nActualTimespan > params.DigishieldMaxActualTimespan())
        nActualTimespan = params.DigishieldMaxActualTimespan();

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.PowLimit(true));
    arith_uint256 bnNew {bnAvg};
    bnNew /= params.DigishieldAveragingWindowTimespan();
    bnNew *= nActualTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

/**
 * Compute a target based on the work done between 2 blocks and the time
 * required to produce that work.
 */
static arith_uint256 ComputeTarget(const CBlockIndex *pindexFirst,
                                   const CBlockIndex *pindexPrev,
                                   const Consensus::Params &params) {
    assert(pindexPrev->nHeight > pindexFirst->nHeight);

    /**
     * From the total work done and the time it took to produce that much work,
     * we can deduce how much work we expect to be produced in the targeted time
     * between blocks.
     */
    arith_uint256 work = pindexPrev->nChainWork - pindexFirst->nChainWork;
    work *= params.nPowTargetSpacing;

    // In order to avoid difficulty cliffs, we bound the amplitude of the
    // adjustment we are going to do to a factor in [0.5, 2].
    int64_t nActualTimespan =
        int64_t(pindexPrev->nTime) - int64_t(pindexFirst->nTime);
    if (nActualTimespan > 288 * params.nPowTargetSpacing) {
        nActualTimespan = 288 * params.nPowTargetSpacing;
    } else if (nActualTimespan < 72 * params.nPowTargetSpacing) {
        nActualTimespan = 72 * params.nPowTargetSpacing;
    }

    work /= nActualTimespan;

    /**
     * We need to compute T = (2^256 / W) - 1 but 2^256 doesn't fit in 256 bits.
     * By expressing 1 as W / W, we get (2^256 - W) / W, and we can compute
     * 2^256 - W as the complement of W.
     */
    return (-work) / work;
}

/**
 * To reduce the impact of timestamp manipulation, we select the block we are
 * basing our computation on via a median of 3.
 */
static const CBlockIndex *GetSuitableBlock(const CBlockIndex *pindex) {
    assert(pindex->nHeight >= 3);

    /**
     * In order to avoid a block with a very skewed timestamp having too much
     * influence, we select the median of the 3 top most blocks as a starting
     * point.
     */
    const CBlockIndex *blocks[3];
    blocks[2] = pindex;
    blocks[1] = pindex->pprev;
    blocks[0] = blocks[1]->pprev;

    // Sorting network.
    if (blocks[0]->nTime > blocks[2]->nTime) {
        std::swap(blocks[0], blocks[2]);
    }

    if (blocks[0]->nTime > blocks[1]->nTime) {
        std::swap(blocks[0], blocks[1]);
    }

    if (blocks[1]->nTime > blocks[2]->nTime) {
        std::swap(blocks[1], blocks[2]);
    }

    // We should have our candidate in the middle now.
    return blocks[1];
}

/**
 * Compute the next required proof of work using a weighted average of the
 * estimated hashrate per block.
 *
 * Using a weighted average ensure that the timestamp parameter cancels out in
 * most of the calculation - except for the timestamp of the first and last
 * block. Because timestamps are the least trustworthy information we have as
 * input, this ensures the algorithm is more resistant to malicious inputs.
 */
uint32_t GetNextCashWorkRequired(const CBlockIndex *pindexPrev,
                                 const CBlockHeader *pblock,
                                 const Config &config) {
    const Consensus::Params &params = config.GetChainParams().GetConsensus();

    // This cannot handle the genesis block and early blocks in general.
    assert(pindexPrev);

    // Special difficulty rule for testnet:
    // If the new block's timestamp is more than 2* 10 minutes then allow
    // mining of a min-difficulty block.
    if (params.fPowAllowMinDifficultyBlocks &&
        (pblock->GetBlockTime() >
         pindexPrev->GetBlockTime() + 2 * params.nPowTargetSpacing)) {
        return UintToArith256(params.powLimit).GetCompact();
    }

    // Compute the difficulty based on the full adjustment interval.
    const uint32_t nHeight = pindexPrev->nHeight;
    assert(nHeight >= params.DifficultyAdjustmentInterval());

    // Get the last suitable block of the difficulty interval.
    const CBlockIndex *pindexLast = GetSuitableBlock(pindexPrev);
    assert(pindexLast);

    // Get the first suitable block of the difficulty interval.
    uint32_t nHeightFirst = nHeight - 144;
    const CBlockIndex *pindexFirst =
        GetSuitableBlock(pindexPrev->GetAncestor(nHeightFirst));
    assert(pindexFirst);

    // Compute the target based on time and work done during the interval.
    const arith_uint256 nextTarget =
        ComputeTarget(pindexFirst, pindexLast, params);

    const arith_uint256 powLimit = UintToArith256(params.powLimit);
    if (nextTarget > powLimit) {
        return powLimit.GetCompact();
    }

    return nextTarget.GetCompact();
}

// Depricated for Bitcoin Cash X
unsigned int CalculateBCCNextWorkRequired(const CBlockIndex* pindexPrev, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexPrev->nBits;

    
    // Limit adjustment step
    int64_t nActualTimespan = pindexPrev->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespanLegacy / 4) {
        nActualTimespan = params.nPowTargetTimespanLegacy / 4;
    }

    if (nActualTimespan > params.nPowTargetTimespanLegacy * 4) {
        nActualTimespan = params.nPowTargetTimespanLegacy * 4;
    }

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimitLegacy);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexPrev->nBits);
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespanLegacy;

    if (bnNew > bnPowLimit) bnNew = bnPowLimit;
    return bnNew.GetCompact();
}

bool CheckEquihashSolution(const CBlockHeader *pblock, const CChainParams& params)
{
    unsigned int n = params.EquihashN();
    unsigned int k = params.EquihashK();

    // Hash state
    crypto_generichash_blake2b_state state;
    EhInitialiseState(n, k, state);

    // I = the block header minus nonce and solution.
    CEquihashInput I{*pblock};
    // I||V
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << I;
    ss << pblock->nNonce;

    // H(I||V||...
    crypto_generichash_blake2b_update(&state, (unsigned char*)&ss[0], ss.size());

    bool isValid;
    EhIsValidSolution(n, k, state, pblock->nSolution, isValid);
    if (!isValid)
        return error("CheckEquihashSolution(): invalid solution");

    return true;
}

bool CheckProofOfWork(uint256 hash, uint32_t nBits, bool postfork, const Config &config) {
    const Consensus::Params &params = config.GetChainParams().GetConsensus();
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;
    
    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);
    // if (postfork) printf("Params hash  :%s\n", hash.GetHex().c_str());
    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.PowLimit(postfork)))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget) 
        return false;

    return true;
}

// Depricated for Bitcoin CDY
unsigned int BitcoinCalculateNextWorkRequired(const CBlockIndex* pindexPrev, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexPrev->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexPrev->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespanLegacy/4)
        nActualTimespan = params.nPowTargetTimespanLegacy/4;
    if (nActualTimespan > params.nPowTargetTimespanLegacy*4)
        nActualTimespan = params.nPowTargetTimespanLegacy*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.PowLimit(false));
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexPrev->nBits);
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespanLegacy;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

// Deprecated for Bitcoin CDY
unsigned int BitcoinGetNextWorkRequired(const CBlockIndex* pindexPrev, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexPrev != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.PowLimit(false)).GetCompact();

    // Only change once per difficulty adjustment interval
    if ((pindexPrev->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexPrev->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexPrev;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexPrev->nBits;
    }

    // Go back by what we want to be 14 days worth of blocks
    int nHeightFirst = pindexPrev->nHeight - (params.DifficultyAdjustmentInterval()-1);
    assert(nHeightFirst >= 0);
    const CBlockIndex* pindexFirst = pindexPrev->GetAncestor(nHeightFirst);
    assert(pindexFirst);

    return BitcoinCalculateNextWorkRequired(pindexPrev, pindexFirst->GetBlockTime(), params);
}
