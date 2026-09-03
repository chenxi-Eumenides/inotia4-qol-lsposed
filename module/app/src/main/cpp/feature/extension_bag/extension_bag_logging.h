#pragma once

#include <android/log.h>

constexpr char kVirtBagLogTag[] = "Inotia4VirtBag";
#define VIRTBAG_TAG kVirtBagLogTag

void extension_bag_log(const char* format, ...);

#define VIRTBAG_LOG(...) extension_bag_log(__VA_ARGS__)
