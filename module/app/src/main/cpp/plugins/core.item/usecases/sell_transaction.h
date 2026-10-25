#pragma once

#include "contracts/inventory_action.h"

namespace qol::features::inventory_hooks {

namespace contracts = qol::contracts;

struct SaleTransactionOps {
    void* ctx;
    QolResult (*credit)(void* ctx, int64_t price);
    QolResult (*remove)(void* ctx);
    QolResult (*refund)(void* ctx, int64_t price);
};

// The caller validates identity and price before entering this money/remove transaction.
inline contracts::InventorySellStatus run_sale_transaction(int64_t price,
                                                           const SaleTransactionOps& ops) {
    if (price < 0 || ops.credit == nullptr || ops.remove == nullptr || ops.refund == nullptr)
        return contracts::InventorySellStatus::kPriceInvalid;
    if (!contracts::succeeded(ops.credit(ops.ctx, price)))
        return contracts::InventorySellStatus::kCreditFailed;
    if (contracts::succeeded(ops.remove(ops.ctx)))
        return contracts::InventorySellStatus::kOk;
    return contracts::succeeded(ops.refund(ops.ctx, price))
               ? contracts::InventorySellStatus::kRemoveFailed
               : contracts::InventorySellStatus::kRollbackFailed;
}

}  // namespace qol::features::inventory_hooks
