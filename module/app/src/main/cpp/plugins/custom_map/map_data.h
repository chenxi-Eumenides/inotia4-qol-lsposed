#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qol::features::custom_map {

enum class CustomMapFileKind : uint8_t { kDat, kDatJpg };

struct CustomMapFile {
    uint32_t id;
    std::string name;
    std::string path;
    uint64_t size;
    int64_t modified_sec;
    int64_t modified_nsec;
    CustomMapFileKind kind = CustomMapFileKind::kDat;
};

enum class CustomMapConversionResult : uint8_t {
    kConverted,
    kDatAlreadyExists,
    kInvalidContainer,
    kReadFailed,
    kSourceChanged,
    kDecodeFailed,
    kWriteFailed,
};

struct CustomMapSummary {
    uint32_t id;
    uint32_t width;
    uint32_t height;
};

struct CustomMapExit {
    uint8_t x;
    uint8_t y;
    uint8_t target_x;
    uint8_t target_y;
    uint16_t target_map_and_flags;
};

struct CustomMapDetail {
    uint32_t id;
    uint32_t width;
    uint32_t height;
    std::vector<uint16_t> base_tiles;
    std::vector<CustomMapExit> exits;
};

bool custom_map_list_files(std::vector<CustomMapFile>* out);
bool custom_map_read_summary(const CustomMapFile& file, CustomMapSummary* out);
bool custom_map_read_detail(const CustomMapFile& file, CustomMapDetail* out);
bool custom_map_file_unchanged(const CustomMapFile& file);
CustomMapConversionResult custom_map_decode_jpg(const std::string& path,
                                                 std::vector<uint8_t>* out_decoded);
CustomMapConversionResult custom_map_write_decoded_dat(
    const std::string& jpg_path, const std::string& dat_path, uint32_t id,
    const std::vector<uint8_t>& decoded, CustomMapFile* out_dat);
CustomMapConversionResult custom_map_convert_jpg(const CustomMapFile& file,
                                                  CustomMapFile* out_dat);

}  // namespace qol::features::custom_map
