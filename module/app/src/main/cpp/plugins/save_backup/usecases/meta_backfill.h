#pragma once

#include <string>
#include <vector>

#include "plugins/save_backup/bundle.h"

namespace save_backup::usecases::meta_backfill {

// Repair legacy entry metadata from the immutable save payload. Bundle version, checksum, and
// payload sections remain unchanged; only the metadata section and CRC are rewritten.
bool backfill_entry_meta_from_plain(const std::string& path,
                                   const std::vector<uint8_t>& bytes,
                                   save_backup::ParsedBundle& parsed);

}  // namespace save_backup::usecases::meta_backfill
