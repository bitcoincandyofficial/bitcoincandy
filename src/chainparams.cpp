// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"
#include "consensus/merkle.h"

#include "uint256.h"
#include "arith_uint256.h"

#include "tinyformat.h"
#include "util.h"
#include "utilstrencodings.h"

#include <cassert>
// For equihash_parameters_acceptable.
#include "crypto/equihash.h"
#include "net.h"
#include "validation.h"
#define equihash_parameters_acceptable(N, K) \
    ((CBlockHeader::HEADER_SIZE + equihash_solution_size(N, K))*MAX_HEADERS_RESULTS < \
     MAX_PROTOCOL_MESSAGE_LENGTH-1000)

#include "base58.h"
#include <boost/assign/list_of.hpp>
#include <limits>

#include "chainparamsseeds.h"

// Far into the future.
static const std::string ANTI_REPLAY_COMMITMENT =
    "Bitcoin: A Peer-to-Peer Electronic Cash System";

static std::vector<uint8_t> GetAntiReplayCommitment() {
    return std::vector<uint8_t>(std::begin(ANTI_REPLAY_COMMITMENT),
                                std::end(ANTI_REPLAY_COMMITMENT));
}

static CBlock CreateGenesisBlock(const char *pszTimestamp,
                                 const CScript &genesisOutputScript,
                                 uint32_t nTime, uint32_t nNonce,
                                 uint32_t nBits, int32_t nVersion,
                                 const Amount genesisReward) {
    CMutableTransaction txNew;
    txNew.nVersion = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig =
        CScript() << 486604799 << CScriptNum(4)
                  << std::vector<uint8_t>((const uint8_t *)pszTimestamp,
                                          (const uint8_t *)pszTimestamp +
                                              strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = ArithToUint256(arith_uint256(nNonce));
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.nHeight  = 0;
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Build the genesis block. Note that the output of its generation transaction
 * cannot be spent since it did not originally exist in the database.
 *
 * CBlock(hash=000000000019d6, ver=1, hashPrevBlock=00000000000000,
 * hashMerkleRoot=4a5e1e, nTime=1231006505, nBits=1d00ffff, nNonce=2083236893,
 * vtx=1)
 *   CTransaction(hash=4a5e1e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase
 * 04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73)
 *     CTxOut(nValue=50.00000000, scriptPubKey=0x5F1DF16B2B704C8A578D0B)
 *   vMerkleTree: 4a5e1e
 */
static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce,
                                 uint32_t nBits, int32_t nVersion,
                                 const Amount genesisReward) {
    const char *pszTimestamp =
        "The Times 03/Jan/2009 Chancellor on brink of second bailout for banks";
    const CScript genesisOutputScript =
        CScript() << ParseHex("04678afdb0fe5548271967f1a67130b7105cd6a828e03909"
                              "a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112"
                              "de5c384df7ba0b8d578a4c702b6bf11d5f")
                  << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce,
                              nBits, nVersion, genesisReward);
}

const arith_uint256 maxUint = UintToArith256(uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));

/**
 * Main network
 */
/**
 * What makes a good checkpoint block?
 * + Is surrounded by blocks with reasonable timestamps
 *   (no blocks before with a timestamp after, none after with
 *    timestamp before)
 * + Contains no strange transactions
 */
class CMainParams : public CChainParams {
public:
    CMainParams() {
        strNetworkID = "main";
        consensus.nSubsidyHalvingInterval = 210000;
        consensus.BIP34Height = 227931;
        consensus.BIP34Hash = uint256S("0x000000000000024b89b42a942fe0d9fea3bb4"
                                       "4ab7bd1b19115dd6a759c0808b8");
        // 000000000000000004c2b624ed5d7756c508d90fd0da2c7c679febfa6c4735f0
        consensus.BIP65Height = 388381;
        // 00000000000000000379eaa19dce8c9b722d46ae6a57c2f1a988119488b50931
        consensus.BIP66Height = 363725;
        consensus.antiReplayOpReturnSunsetHeight = 530000;
        consensus.antiReplayOpReturnCommitment = GetAntiReplayCommitment();
        consensus.powLimit = uint256S("0007ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.powLimitLegacy = uint256S("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

        consensus.nDigishieldAveragingWindow = 30;
        assert(maxUint/UintToArith256(consensus.powLimit) >= consensus.nDigishieldAveragingWindow);
        consensus.nDigishieldMaxAdjustDown = 32;
        consensus.nDigishieldMaxAdjustUp = 16;

        consensus.nZawyLwmaAveragingWindow = 60;
        consensus.CDYMaxFutureBlockTime = 240; //
        
        // two weeks
        consensus.nPowTargetTimespanLegacy = 14 * 24 * 60 * 60;
        consensus.nPowTargetSpacing = 10 * 60;
        
        consensus.nPowTargetSpacingCDY = 2 * 60; 
        
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        // 95% of 2016
        consensus.nRuleChangeActivationThreshold = 1916;
        // nPowTargetTimespan / nPowTargetSpacing
        consensus.nMinerConfirmationWindow = 2016;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        // January 1, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime =
            1199145601;
        // December 31, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout =
            1230767999;
        // Deployment of BIP68, BIP112, and BIP113.
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
        // May 1st, 2016
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime =
            1462060800;
        // May 1st, 2017
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 1493596800;
        // The best chain should have at least this much work.
        consensus.nMinimumChainWork =
            uint256S("0x00000000000000000000000000000000000000000082d89e5b8963ba7d734c61");
        // By default assume that the signatures in ancestors of this block are
        // valid.
        consensus.defaultAssumeValid = uint256S("0x000000000000000003d0f45045742bb557f1a6c633cdb9de53564eb7ec4459d0"); // block hash 512665

        // Aug, 1 hard fork
        consensus.uahfHeight = 478559;
        
        // Nov, 13 hard fork
        consensus.daaHeight = 504031;

        //  hard fork-----add by hmc
        consensus.cdyHeight = 512666;

        //  hard fork
        consensus.CDYZawyLWMAHeight = 573123;  
        consensus.nNewRuleHeight = 592447;
        consensus.CDYEquihashForkHeight = 656960; // Around 09/01/2018
 
        /** Height to publish compensing coins*/
        consensus.nCompenseHeight = 758000;


        /** Height to publish cdy community pool protection plan to prevent attacks, decrease sell pressure */
        consensus.nPoolProtectionPlan = 1447999;
        
        consensus.validPoolAddresses.push_back("CRxRgjnz3MPi7FCeV3oXjALLXbnesz8v3F"); // euro pool 1
        consensus.validPoolAddresses.push_back("CawyQoHbTxaprmcnJeuCnucTZ3hCCyJ1dZ"); // euro pool 1
        consensus.validPoolAddresses.push_back("CZsrKKX7y8iPjrJ4rrE4vkooFgMKNagAWV"); // euro pool 2
        consensus.validPoolAddresses.push_back("CewxiZ1kWzagDMoXPhFTwbQxkyirVibZe9"); // euro pool 2

        // future/backup approved pools list
        consensus.validPoolAddresses.push_back("CPRLv43zoswdjVeWZbnAZiL9p2xys7EyAv");
        consensus.validPoolAddresses.push_back("CcDQ4cedyzRMbckhjo3XCGFbQjPXNxiyF8");
        consensus.validPoolAddresses.push_back("Ce4KfkQZSqQbBX2i5ckD7hW22Qoabud5UC");
        consensus.validPoolAddresses.push_back("CdZWiyU9TkCneDjSokkwXohrrMeTuuijoK");
        consensus.validPoolAddresses.push_back("CbVrvUDdDuWKmRSeY6ND4v22snvG8tHG4L");
        consensus.validPoolAddresses.push_back("CHo4h1zVTrNYoZmBkC46ah2HZF8RjdWoVZ");
        consensus.validPoolAddresses.push_back("CcZ674T8iric5vbrwHvni7pMaxyBSV1gXG");
        consensus.validPoolAddresses.push_back("CMY25Y59xxo9JcLrzEcBN4zue75p4jacy1");
        consensus.validPoolAddresses.push_back("CauVZ9oFNoq8v6Mg2pqnifxpn3t3CPoNL1");
        consensus.validPoolAddresses.push_back("CLFsEBc6nZ2iFt5dKwJsgXYauaCx6HyYVW");
        consensus.validPoolAddresses.push_back("CQc8X5R78yiwtVPbcpWxyXiuTRzMx17RiT");
        consensus.validPoolAddresses.push_back("CZVcLZPHcvqUzryzGE6NBvuPB87qDbW7eN");
        consensus.validPoolAddresses.push_back("CZGTBPNvocPAtsRNY5r3g6gt5iBwEbPUe1");
        consensus.validPoolAddresses.push_back("CU3LRuHJpoGfoWr84yEMqB5uajnTJSgoUc");


        consensus.BitcoinPostforkBlock = uint256S("0000000000000000007b746068bd08ba4089f97636690e9dc758774e7db21f17");	// 512666 block hash
        consensus.BitcoinPostforkTime = 1515799972;

        /**
         * The message start string is designed to be unlikely to occur in
         * normal data. The characters are rarely used upper ASCII, not valid as
         * UTF-8, and produce a large 32-bit integer with any alignment.
         */
        diskMagic[0] = 0xf9;
        diskMagic[1] = 0xbe;
        diskMagic[2] = 0xb4;
        diskMagic[3] = 0xd9;
        netMagic[0] = 0xe3;
        netMagic[1] = 0xc3;	// 0x80 + 0x43 'C'
        netMagic[2] = 0xc4; // 0x80 + 0x44 'D'
        netMagic[3] = 0xd9; // 0x80 + 0x59 'Y'
        netMagicLegacy[0] = 0xe3; // BCH
        netMagicLegacy[1] = 0xe1; // BCH
        netMagicLegacy[2] = 0xf3; // BCH
        netMagicLegacy[3] = 0xe8; // BCH

        // use different default
        nDefaultPort = 8367;
        nBitcoinDefaultPort = 8333;
        nPruneAfterHeight = 100000;
        const size_t N = 200, K = 9;
        const size_t N2 = 144, K2 = 5;
        BOOST_STATIC_ASSERT(equihash_parameters_acceptable(N, K));
        BOOST_STATIC_ASSERT(equihash_parameters_acceptable(N2, K2));
        nEquihashN = N;
        nEquihashK = K;
        nEquihashNnew = N2;
        nEquihashKnew = K2;

        genesis = CreateGenesisBlock(1231006505, 2083236893, 0x1d00ffff, 1,
                                     50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash(consensus);
        assert(consensus.hashGenesisBlock ==
               uint256S("0x000000000019d6689c085ae165831e934ff763ae46a2a6c172b3"
                        "f1b60a8ce26f"));
        assert(genesis.hashMerkleRoot ==
               uint256S("0x4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab212"
                        "7b7afdeda33b"));

        // Note that of those with the service bits flag, most only support a
        // subset of possible options.
        // Bitcoin ABC seeder
        vSeeds.push_back(CDNSSeedData("seed.cdy.one", "seed.cdy.one", true));
        vSeeds.push_back(CDNSSeedData("cdyseed1.awmlite.com", "cdyseed1.awmlite.com", true));
        vSeeds.push_back(CDNSSeedData("cdyseed2.awmlite.com", "cdyseed2.awmlite.com", true));
        vSeeds.push_back(CDNSSeedData("block.cdy.one", "block.cdy.one", true));
        vSeeds.push_back(CDNSSeedData("seed.bitcdy.com", "seed.bitcdy.com", true));

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<uint8_t>(1, 0x1c); // 'C'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<uint8_t>(1, 0x58); // 'c'
        base58Prefixes[SECRET_KEY] = std::vector<uint8_t>(1, 128);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};
        cashaddrPrefix = "bitcoincandy";

        vFixedSeeds = std::vector<SeedSpec6>(
            pnSeed6_main, pnSeed6_main + ARRAYLEN(pnSeed6_main));

        fMiningRequiresPeers = true;
        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        fMineBlocksOnDemand = false;

         // May 15, 2018 hard fork
        consensus.monolithActivationTime = 1526389200;

        checkpointData = {
            .mapCheckpoints = {
                {11111, uint256S("0x0000000069e244f73d78e8fd29ba2fd2ed618bd6fa2"
                                 "ee92559f542fdb26e7c1d")},
                {33333, uint256S("0x000000002dd5588a74784eaa7ab0507a18ad16a236e"
                                 "7b1ce69f00d7ddfb5d0a6")},
                {74000, uint256S("0x0000000000573993a3c9e41ce34471c079dcf5f52a0"
                                 "e824a81e7f953b8661a20")},
                {105000, uint256S("0x00000000000291ce28027faea320c8d2b054b2e0fe"
                                  "44a773f3eefb151d6bdc97")},
                {134444, uint256S("0x00000000000005b12ffd4cd315cd34ffd4a594f430"
                                  "ac814c91184a0d42d2b0fe")},
                {168000, uint256S("0x000000000000099e61ea72015e79632f216fe6cb33"
                                  "d7899acb35b75c8303b763")},
                {193000, uint256S("0x000000000000059f452a5f7340de6682a977387c17"
                                  "010ff6e6c3bd83ca8b1317")},
                {210000, uint256S("0x000000000000048b95347e83192f69cf0366076336"
                                  "c639f9b7228e9ba171342e")},
                {216116, uint256S("0x00000000000001b4f4b433e81ee46494af945cf960"
                                  "14816a4e2370f11b23df4e")},
                {225430, uint256S("0x00000000000001c108384350f74090433e7fcf79a6"
                                  "06b8e797f065b130575932")},
                {250000, uint256S("0x000000000000003887df1f29024b06fc2200b55f8a"
                                  "f8f35453d7be294df2d214")},
                {279000, uint256S("0x0000000000000001ae8c72a0b0c301f67e3afca10e"
                                  "819efa9041e458e9bd7e40")},
                {295000, uint256S("0x00000000000000004d9b4ef50f0f9d686fd69db2e0"
                                  "3af35a100370c64632a983")},
                // UAHF fork block.
                {478559, uint256S("0x000000000000000000651ef99cb9fcbe0dadde1d42"
                                  "4bd9f15ff20136191a5eec")},
                // Nov, 13 DAA activation block.
                {504031, uint256S("0x0000000000000000011ebf65b60d0a3de80b8175be"
                                  "709d653b4c1a1beeb6ab9c")},					  
                // Hard fork for bitcoin candy
                {512666, uint256S("0x00043bfba38c60e8b283d4e507e5785b8cd5f72b8b"
                                  "72f66542b2e4c62bd2ed79")},
                //roll back block for coin infaltion
                {671663, uint256S("0x00000241636f7d345b239a2dd785b8e834b494de4d"
                                  "307535d7afb7b1cb84a641")},
                // pool protected
                {1219400, uint256S("0x00024e9dba1c7b44a73775ef82664ecd75463dd508"
                                  "952cdb91c9a74fcc6c5ef4")},
                // extended pool protection
                {1448000, uint256S("0x00004d8dffdebd96050a51d0b0318eab6363b1289a"
                                  "8a8497bd8ac142c4f4c97f")},
            }};

        // Data as of block
        // 00000000000000000166d612d5595e2b1cd88d71d695fc580af64d8da8658c23
        // (height 446482).
        chainTxData = ChainTxData{
            // UNIX timestamp of last known number of transactions.
            1483472411,
            // Total number of transactions between genesis and that timestamp
            // (the tx=... number in the SetBestChain debug.log lines)
            184495391,
            // Estimated number of transactions per second after that timestamp.
            3.2};
    }
};
static CMainParams mainParams;

/**
 * Testnet (v3)
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams() {
        strNetworkID = "test";
        consensus.nSubsidyHalvingInterval = 210000;
        consensus.BIP34Height = 21111;
        consensus.BIP34Hash = uint256S("0x0000000023b3a96d3484e5abb3755c413e7d4"
                                       "1500f8e2a5c3f0dd01299cd8ef8");
        // 00000000007f6655f22f98e72ed80d8b06dc761d5da09df0fa1dc4be4f861eb6
        consensus.BIP65Height = 581885;
        // 000000002104c8c45e99a8853285a3b592602a3ccde2b832481da85e9e4ba182
        consensus.BIP66Height = 330776;
        consensus.antiReplayOpReturnSunsetHeight = 1250000;
        consensus.antiReplayOpReturnCommitment = GetAntiReplayCommitment();
        consensus.powLimit = uint256S("0007ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
	consensus.powLimitLegacy = uint256S("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

        consensus.nDigishieldAveragingWindow= 30;
        assert(maxUint/UintToArith256(consensus.powLimit) >= consensus.nDigishieldAveragingWindow);
        consensus.nDigishieldMaxAdjustDown = 32;
        consensus.nDigishieldMaxAdjustUp = 16;

        consensus.nZawyLwmaAveragingWindow = 60;
        consensus.CDYMaxFutureBlockTime = 240;
        // two weeks
        consensus.nPowTargetTimespanLegacy = 14 * 24 * 60 * 60;
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.nPowTargetSpacingCDY = 2 * 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = false;
        // 75% for testchains
        consensus.nRuleChangeActivationThreshold = 1512;
        // nPowTargetTimespan / nPowTargetSpacing
        consensus.nMinerConfirmationWindow = 2016;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        // January 1, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime =
            1199145601;
        // December 31, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout =
            1230767999;

        // Deployment of BIP68, BIP112, and BIP113.
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
        // March 1st, 2016
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime =
            1456790400;
        // May 1st, 2017
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 1493596800;

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork =
            uint256S("0000000000000000000000000000000000000000000000000453e926d09ebe87");

        // By default assume that the signatures in ancestors of this block are
        // valid.
        consensus.defaultAssumeValid =
            uint256S("0x000000000000b41f1f2ddf130df8824e2b61c0af809ff86dd5cadb3"
                     "61d984ca7");

        // Aug, 1 hard fork
        consensus.uahfHeight = 1155876;

        // Nov, 13 hard fork
        consensus.daaHeight = 1188697;
        
        //  hard fork-----add by hmc
        consensus.cdyHeight = 201601;
        consensus.CDYZawyLWMAHeight = 201641;
        consensus.nNewRuleHeight = 201651;
        consensus.CDYEquihashForkHeight = 201671;

        consensus.nCompenseHeight = 202136;
        
        consensus.BitcoinPostforkBlock = uint256S("00000000d16d6c2aecc7436eea0c54a53741fee9abf265606aa465d6fd3f3d8a"); // block 201601
        consensus.BitcoinPostforkTime = 1393815074;

        diskMagic[0] = 0x0b;
        diskMagic[1] = 0x11;
        diskMagic[2] = 0x09;
        diskMagic[3] = 0x07;
        netMagic[0] = 0xf4;
        netMagic[1] = 0x43;	// 'C'
        netMagic[2] = 0x44; // 'D'
        netMagic[3] = 0x59; // 'Y'
        // BTC
        netMagicLegacy[0] = 0x0b;
        netMagicLegacy[1] = 0x11;
        netMagicLegacy[2] = 0x09;
        netMagicLegacy[3] = 0x07;
        // BCH
        // netMagicLegacy[0] = 0xf4;
        // netMagicLegacy[1] = 0xe5;
        // netMagicLegacy[2] = 0xf3;
        // netMagicLegacy[3] = 0xf4;

        // use different default
        nDefaultPort = 18367;
	nBitcoinDefaultPort = 18333;
        nPruneAfterHeight = 1000;
        const size_t N = 200, K = 9;
        const size_t N2 = 144, K2 = 5;
        BOOST_STATIC_ASSERT(equihash_parameters_acceptable(N, K));
        BOOST_STATIC_ASSERT(equihash_parameters_acceptable(N2, K2));
        nEquihashN = N;
        nEquihashK = K;
        nEquihashNnew = N2;
        nEquihashKnew = K2;

        genesis =
            CreateGenesisBlock(1296688602, 414098458, 0x1d00ffff, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash(consensus);
        assert(consensus.hashGenesisBlock ==
               uint256S("0x000000000933ea01ad0ee984209779baaec3ced90fa3f4087195"
                        "26f8d77f4943"));
        assert(genesis.hashMerkleRoot ==
               uint256S("0x4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab212"
                        "7b7afdeda33b"));

        vFixedSeeds.clear();
        vSeeds.clear();
        // nodes with support for servicebits filtering should be at the top
        vSeeds.push_back(CDNSSeedData("bitcoincandy.one", "testnet-seed.bitcoincandy.one", true));
        base58Prefixes[PUBKEY_ADDRESS] = std::vector<uint8_t>(1, 111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<uint8_t>(1, 196);
        base58Prefixes[SECRET_KEY] = std::vector<uint8_t>(1, 239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};
        cashaddrPrefix = "cdytest";
        vFixedSeeds = std::vector<SeedSpec6>(
            pnSeed6_test, pnSeed6_test + ARRAYLEN(pnSeed6_test));

        fMiningRequiresPeers = true;
        fDefaultConsistencyChecks = false;
        fRequireStandard = false;
        fMineBlocksOnDemand = false;


        consensus.monolithActivationTime = 1525609241;

        checkpointData = {
            .mapCheckpoints = {
                {546, uint256S("000000002a936ca763904c3c35fce2f3556c559c0214345"
                               "d31b1bcebf76acb70")},
                // UAHF fork block.
                {1155876, uint256S("00000000000e38fef93ed9582a7df43815d5c2ba9fd"

                                   "37ef70c9a0ea4a285b8f5")},
                // Nov, 13. DAA activation block.
                {1188697, uint256S("0000000000170ed0918077bde7b4d36cc4c91be69fa"
                                   "09211f748240dabe047fb")},
            }};

        // Data as of block
        // 00000000c2872f8f8a8935c8e3c5862be9038c97d4de2cf37ed496991166928a
        // (height 1063660)
        chainTxData = ChainTxData{1483546230, 12834668, 0.15};
    }
};
static CTestNetParams testNetParams;

/**
 * Regression test
 */
class CRegTestParams : public CChainParams {
public:
    CRegTestParams() {
        strNetworkID = "regtest";
        consensus.nSubsidyHalvingInterval = 150;
        // BIP34 has not activated on regtest (far in the future so block v1 are
        // not rejected in tests)
        consensus.BIP34Height = 100000000;
        consensus.BIP34Hash = uint256();
        // BIP65 activated on regtest (Used in rpc activation tests)
        consensus.BIP65Height = 1351;
        // BIP66 activated on regtest (Used in rpc activation tests)
        consensus.BIP66Height = 1251;
        consensus.antiReplayOpReturnSunsetHeight = 530000;
        consensus.antiReplayOpReturnCommitment = GetAntiReplayCommitment();
        consensus.powLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
		consensus.powLimitLegacy = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

        consensus.nDigishieldAveragingWindow = 30;
        consensus.nDigishieldMaxAdjustDown = 32;
        consensus.nDigishieldMaxAdjustUp = 16;

        consensus.nZawyLwmaAveragingWindow = 45;
        consensus.nZawyLwmaAjustedWeight = 13632;
        consensus.CDYMaxFutureBlockTime = 7 * 10 * 60; // 70 mins

        consensus.nPowTargetTimespanLegacy = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.nPowTargetSpacingCDY = 2 * 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = true;
        // 75% for testchains
        consensus.nRuleChangeActivationThreshold = 108;
        // Faster than normal for regtest (144 instead of 2016)
        consensus.nMinerConfirmationWindow = 144;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout =
            999999999999ULL;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout =
            999999999999ULL;

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = uint256S("0x00");

        // By default assume that the signatures in ancestors of this block are
        // valid.
        consensus.defaultAssumeValid = uint256S("0x00");

        // Hard fork is always enabled on regtest.
        consensus.uahfHeight = 2017;

        //  hard fork-----add by hmc
        consensus.cdyHeight = 2260;

        consensus.nNewRuleHeight = 201836;
        consensus.CDYEquihashForkHeight = 201876;

        consensus.nCompenseHeight = 202000; 

        // Nov, 13 hard fork is always on on regtest.
        consensus.daaHeight = 2250;
         
        consensus.CDYZawyLWMAHeight = -1;
        
        diskMagic[0] = 0xfa;
        diskMagic[1] = 0xbf;
        diskMagic[2] = 0xb5;
        diskMagic[3] = 0xda;
        netMagic[0] = 0xda;
        netMagic[1] = 0xb5;
        netMagic[2] = 0xbf;
        netMagic[3] = 0xfa;
        nDefaultPort = 18444;
        nPruneAfterHeight = 1000;
        const size_t N = 48, K = 5;
        const size_t N2 = 96, K2 = 5;
        BOOST_STATIC_ASSERT(equihash_parameters_acceptable(N, K));
        BOOST_STATIC_ASSERT(equihash_parameters_acceptable(N2, K2));
        nEquihashN = N;
        nEquihashK = K;
        nEquihashNnew = N2;
        nEquihashKnew = K2;

        genesis = CreateGenesisBlock(1296688602, 2, 0x207fffff, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash(consensus);
        assert(consensus.hashGenesisBlock ==
               uint256S("0x0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b"
                        "1a11466e2206"));
        assert(genesis.hashMerkleRoot ==
               uint256S("0x4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab212"
                        "7b7afdeda33b"));

        //!< Regtest mode doesn't have any fixed seeds.
        vFixedSeeds.clear();
        //!< Regtest mode doesn't have any DNS seeds.
        vSeeds.clear();

        fMiningRequiresPeers = false;
        fDefaultConsistencyChecks = true;
        fRequireStandard = false;
        fMineBlocksOnDemand = true;

         // May 1, 2018 hard fork
        consensus.monolithActivationTime = 1526400000;

        checkpointData = {.mapCheckpoints = {
                              {0, uint256S("0f9188f13cb7b2c71f2a335e3a4fc328bf5"
                                           "beb436012afca590b1a11466e2206")},
                          }};

        chainTxData = ChainTxData{0, 0, 0};

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<uint8_t>(1, 111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<uint8_t>(1, 196);
        base58Prefixes[SECRET_KEY] = std::vector<uint8_t>(1, 239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};
        cashaddrPrefix = "cdyreg";
    }

    void UpdateBIP9Parameters(Consensus::DeploymentPos d, int64_t nStartTime,
                              int64_t nTimeout) {
        consensus.vDeployments[d].nStartTime = nStartTime;
        consensus.vDeployments[d].nTimeout = nTimeout;
    }
};

static CRegTestParams regTestParams;

static CChainParams *pCurrentParams = 0;

const CChainParams &Params() {
    assert(pCurrentParams);
    return *pCurrentParams;
}

CChainParams &Params(const std::string &chain) {
    if (chain == CBaseChainParams::MAIN) {
        return mainParams;
    }

    if (chain == CBaseChainParams::TESTNET) {
        return testNetParams;
    }

    if (chain == CBaseChainParams::REGTEST) {
        return regTestParams;
    }

    throw std::runtime_error(
        strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectParams(const std::string &network) {
    SelectBaseParams(network);
    pCurrentParams = &Params(network);
}

void UpdateRegtestBIP9Parameters(Consensus::DeploymentPos d, int64_t nStartTime,
                                 int64_t nTimeout) {
    regTestParams.UpdateBIP9Parameters(d, nStartTime, nTimeout);
}

unsigned int CChainParams::EquihashSolutionWidth(int height) const
{
    return EhSolutionWidth(EquihashN(height), EquihashK(height));
}
