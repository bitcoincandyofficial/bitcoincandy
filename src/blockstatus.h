// Copyright (c) 2018 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BLOCKSTATUS_H
#define BITCOIN_BLOCKSTATUS_H

#include "serialize.h"

#include <cstdint>

enum class BlockValidity : uint32_t {
    /**
     * Unused.
     */
    UNKNOWN = 0,

    /**
     * Parsed, version ok, hash satisfies claimed PoW, 1 <= vtx count <= max,
     * timestamp not in future.
     */
    HEADER = 1,

    /**
     * All parent headers found, difficulty matches, timestamp >= median
     * previous, checkpoint. Implies all parents are also at least TREE.
     */
    TREE = 2,

    /**
     * Only first tx is coinbase, 2 <= coinbase input script length <= 100,
     * transactions valid, no duplicate txids, sigops, size, merkle root.
     * Implies all parents are at least TREE but not necessarily TRANSACTIONS.
     * When all parent blocks also have TRANSACTIONS, CBlockIndex::nChainTx will
     * be set.
     */
    TRANSACTIONS = 3,

    /**
     * Outputs do not overspend inputs, no double spends, coinbase output ok, no
     * immature coinbase spends, BIP30.
     * Implies all parents are also at least CHAIN.
     */
    CHAIN = 4,

    /**
     * Scripts & signatures ok. Implies all parents are also at least SCRIPTS.
     */
    SCRIPTS = 5,
};

struct BlockStatus {
private:
    uint32_t status;

    explicit constexpr BlockStatus(uint32_t nStatusIn) : status(nStatusIn) {}

    static const uint32_t VALIDITY_MASK = 0x07;

    // Full block available in blk*.dat
    static const uint32_t HAS_DATA_FLAG = 0x08;
    // Undo data available in rev*.dat
    static const uint32_t HAS_UNDO_FLAG = 0x10;

    // The block is invalid.
    static const uint32_t FAILED_FLAG = 0x20;
    // The block has an invalid parent.
    static const uint32_t FAILED_PARENT_FLAG = 0x40;

    // Mask used to check if the block failed.
    static const uint32_t INVALID_MASK = FAILED_FLAG | FAILED_PARENT_FLAG;

    // The block is being parked for some reason. It will be reconsidered if its
    // chains grows.
    static const uint32_t PARKED_FLAG = 0x80;
    // One of the block's parent is parked.
    static const uint32_t PARKED_PARENT_FLAG = 0x100;

    // Mask used to check for parked blocks.
    static const uint32_t PARKED_MASK = PARKED_FLAG | PARKED_PARENT_FLAG;

public:
    explicit constexpr BlockStatus() : status(0) {}

    BlockValidity getValidity() const {
        return BlockValidity(status & VALIDITY_MASK);
    }

    BlockStatus withValidity(BlockValidity validity) const {
        return BlockStatus((status & ~VALIDITY_MASK) | uint32_t(validity));
    }

    bool hasData() const { return status & HAS_DATA_FLAG; }
    BlockStatus withData(bool hasData = true) const {
        return BlockStatus((status & ~HAS_DATA_FLAG) |
                           (hasData ? HAS_DATA_FLAG : 0));
    }

    bool hasUndo() const { return status & HAS_UNDO_FLAG; }
    BlockStatus withUndo(bool hasUndo = true) const {
        return BlockStatus((status & ~HAS_UNDO_FLAG) |
                           (hasUndo ? HAS_UNDO_FLAG : 0));
    }

    bool hasFailed() const { return status & FAILED_FLAG; }
    BlockStatus withFailed(bool hasFailed = true) const {
        return BlockStatus((status & ~FAILED_FLAG) |
                           (hasFailed ? FAILED_FLAG : 0));
    }

    bool hasFailedParent() const { return status & FAILED_PARENT_FLAG; }
    BlockStatus withFailedParent(bool hasFailedParent = true) const {
        return BlockStatus((status & ~FAILED_PARENT_FLAG) |
                           (hasFailedParent ? FAILED_PARENT_FLAG : 0));
    }

    bool isParked() const { return status & PARKED_FLAG; }
    BlockStatus withParked(bool parked = true) const {
        return BlockStatus((status & ~PARKED_FLAG) |
                           (parked ? PARKED_FLAG : 0));
    }

    bool hasParkedParent() const { return status & PARKED_PARENT_FLAG; }
    BlockStatus withParkedParent(bool parkedParent = true) const {
        return BlockStatus((status & ~PARKED_PARENT_FLAG) |
                           (parkedParent ? PARKED_PARENT_FLAG : 0));
    }

    /**
     * Check whether this block index entry is valid up to the passed validity
     * level.
     */
    bool isValid(enum BlockValidity nUpTo = BlockValidity::TRANSACTIONS) const {
        if (isInvalid()) {
            return false;
        }

        return getValidity() >= nUpTo;
    }

    bool isInvalid() const { return status & INVALID_MASK; }
    BlockStatus withClearedFailureFlags() const {
        return BlockStatus(status & ~INVALID_MASK);
    }

    bool isOnParkedChain() const { return status & PARKED_MASK; }
    BlockStatus withClearedParkedFlags() const {
        return BlockStatus(status & ~PARKED_MASK);
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream &s, Operation ser_action) {
        READWRITE(VARINT(status));
    }

    friend constexpr bool operator==(const BlockStatus a, const BlockStatus b) {
        return a.status == b.status;
    }

    friend constexpr bool operator!=(const BlockStatus a, const BlockStatus b) {
        return !(a == b);
    }
};

#endif // BITCOIN_BLOCKSTATUS_H
