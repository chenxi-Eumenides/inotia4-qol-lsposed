#pragma once

#include "result.h"

namespace qol::contracts {

// 在途资源句柄只携带登记表中的静态资源 id，不暴露宿主登记表或其存储布局。
struct InFlightHandle {
    const char* resource_id;
};

using QolInFlightEnterFn = QolResult (*)(InFlightHandle handle);
using QolInFlightLeaveFn = QolResult (*)(InFlightHandle handle);

struct QolInFlightApi {
    QolInFlightEnterFn enter;
    QolInFlightLeaveFn leave;
};

// 插件 wrapper 可用的类型化 RAII 形式。宿主通过 QolInFlightApi 注入实际计数操作。
class InFlightGuard {
public:
    InFlightGuard(const QolInFlightApi* api, InFlightHandle handle)
        : api_(api), handle_(handle), entered_(false) {
        if (api_ != nullptr && api_->enter != nullptr && handle_.resource_id != nullptr) {
            entered_ = succeeded(api_->enter(handle_));
        }
    }

    ~InFlightGuard() {
        if (entered_ && api_ != nullptr && api_->leave != nullptr) {
            (void)api_->leave(handle_);
        }
    }

    InFlightGuard(const InFlightGuard&) = delete;
    InFlightGuard& operator=(const InFlightGuard&) = delete;

    bool entered() const { return entered_; }

private:
    const QolInFlightApi* api_;
    InFlightHandle handle_;
    bool entered_;
};

}  // namespace qol::contracts
