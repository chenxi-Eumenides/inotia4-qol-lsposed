#pragma once

#include "contracts/feature.h"

#include <string>

namespace qol::host {

inline std::string feature_receipt_json(const contracts::FeatureReceipt& receipt) {
    return "{\"ok\":true,\"request_id\":" + std::to_string(receipt.request_id) +
        ",\"revision\":" + std::to_string(receipt.revision) +
        ",\"admission\":\"" +
        (receipt.admission == contracts::FeatureAdmission::kAdmitted ? "admitted" : "rejected") +
        "\",\"persistence\":\"" +
        (receipt.persistence == contracts::FeaturePersistence::kPending ? "pending" :
         receipt.persistence == contracts::FeaturePersistence::kPersisted ? "persisted" : "failed") +
        "\",\"actual\":\"" +
        (receipt.actual == contracts::FeatureActual::kPending ? "pending" : "not_submitted") +
        "\",\"error_code\":" + std::to_string(receipt.error_code) + "}";
}

}  // namespace qol::host
