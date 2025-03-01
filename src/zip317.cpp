// Copyright (c) 2023-2023 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "zip317.h"

#include <algorithm>

static size_t ceil_div(size_t num, size_t den) {
    return (num + den - 1)/den;
}

CAmount CalculateConventionalFee(size_t logicalActionCount) {
    return MARGINAL_FEE * std::max(GRACE_ACTIONS, logicalActionCount);
}

size_t CalculateLogicalActionCount(
        const std::vector<CTxIn>& vin,
        const std::vector<CTxOut>& vout,
        unsigned int joinSplitCount,
        unsigned int saplingSpendCount,
        unsigned int saplingOutputCount,
        unsigned int orchardActionCount) {
    const size_t tx_in_total_size = GetSerializeSize(vin, SER_NETWORK, PROTOCOL_VERSION);
    const size_t tx_out_total_size = GetSerializeSize(vout, SER_NETWORK, PROTOCOL_VERSION);

    return std::max(ceil_div(tx_in_total_size, P2PKH_STANDARD_INPUT_SIZE),
                    ceil_div(tx_out_total_size, P2PKH_STANDARD_OUTPUT_SIZE)) +
           2 * joinSplitCount +
           std::max(saplingSpendCount, saplingOutputCount) +
           orchardActionCount;
}
