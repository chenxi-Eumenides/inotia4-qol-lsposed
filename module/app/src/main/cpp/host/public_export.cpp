#include "host/public_export.h"

#include <cstring>

namespace qol::host {
namespace {

QolResult fail(int32_t code, const char* text) {
    return contracts::error(code, text, static_cast<uint32_t>(std::strlen(text)));
}

QolResult export_crash_logs() {
    return public_export_call_crash_logs()
               ? contracts::ok()
               : fail(kQolErrNotReady, "public export bridge unavailable");
}

QolResult export_backup(const char* source_path, const char* display_name) {
    if (source_path == nullptr || display_name == nullptr)
        return fail(kQolErrInvalidArgument, "public export backup argument missing");
    return public_export_call_backup(source_path, display_name)
               ? contracts::ok()
               : fail(kQolErrNotReady, "public export bridge unavailable");
}

QolResult pick_import_file() {
    return public_export_call_pick_file()
               ? contracts::ok()
               : fail(kQolErrNotReady, "public export bridge unavailable");
}

}  // namespace

const contracts::QolPublicExportApi& public_export_api() {
    static const contracts::QolPublicExportApi api{
        sizeof(contracts::QolPublicExportApi), &export_crash_logs, &export_backup,
        &pick_import_file};
    return api;
}

}  // namespace qol::host
