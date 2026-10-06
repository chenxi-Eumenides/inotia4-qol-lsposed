#pragma once

// Extension source protection remains coupled to extension_bag; move_merge configuration is
// owned by the move_merge plugin and queried through its capability.

bool move_merge_enabled();
bool set_extension_source_protection_enabled(bool enabled);
