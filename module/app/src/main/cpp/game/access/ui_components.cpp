#include "game/access/ui_components.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "game/access/game_access.h"
#include "game/access/ui_container_frame.h"
#include "game/access/ui_container_frame_geometry.h"
#include "game/access/qol_log_seam.h"
#include "game/adapter_ports/game_host_port.h"
#include "game/symbols/game_symbols.h"

namespace {

constexpr int32_t kOptionImageUnit = 0x59;
constexpr int32_t kBackPressedLoc = 0x2;
constexpr int32_t kBackNormalLoc = 0x3;
constexpr int32_t kToggleDimLoc = 0x9;
constexpr int32_t kToggleOnLoc = 0x0f;
constexpr int32_t kToggleOffLoc = 0x10;

bool control_center(void* ctrl, int32_t* x, int32_t* y) {
    if (ctrl == nullptr || x == nullptr || y == nullptr) return false;
    UiRect rect{};
    if (!ui_get_rect(ctrl, &rect)) return false;
    int64_t absolute_x = rect.x + rect.w / 2;
    int64_t absolute_y = rect.y + rect.h / 2;
    for (void* parent = *reinterpret_cast<void**>(static_cast<uint8_t*>(ctrl) + CO_PARENT);
         parent != nullptr;
         parent = *reinterpret_cast<void**>(static_cast<uint8_t*>(parent) + CO_PARENT)) {
        UiRect parent_rect{};
        if (!ui_get_rect(parent, &parent_rect)) return false;
        absolute_x += parent_rect.x;
        absolute_y += parent_rect.y;
    }
    *x = static_cast<int32_t>(absolute_x);
    *y = static_cast<int32_t>(absolute_y);
    return true;
}

}  // namespace

namespace ui_original {

bool load_option_images() {
    return ui_load_image_unit(kOptionImageUnit);
}

void unload_option_images() {
    ui_unload_image_unit(kOptionImageUnit);
}

bool draw_back_button(void* ctrl, bool pressed) {
    int32_t center_x = 0;
    int32_t center_y = 0;
    if (!control_center(ctrl, &center_x, &center_y)) return false;
    if (!ui_draw_image_part(kOptionImageUnit, kBackNormalLoc, center_x, center_y, 2, 1)) {
        return false;
    }
    // 原版返回按钮按下亮光 (loc 2) 相对常态 (loc 3) 的偏移是 (−3, −3)。
    if (pressed && !ui_draw_image_part(kOptionImageUnit, kBackPressedLoc,
                                       center_x - 3, center_y - 3, 2, 1)) {
        return false;
    }
    return true;
}

bool draw_toggle(void* ctrl, bool enabled) {
    const int32_t loc = enabled ? kToggleOnLoc : kToggleOffLoc;
    if (!ui_draw_control_title_image_part(ctrl, loc)) return false;
    if (!enabled) ui_draw_control_title_image_part(ctrl, kToggleDimLoc);
    return true;
}

bool draw_item_icon(void* ctrl, const ItemIcon& icon) {
    if (icon.centered) {
        return ui_draw_control_image_part_centered(ctrl, icon.image_unit, icon.image_loc,
                                                   icon.draw_type, icon.flip);
    }
    return ui_draw_control_image_part(ctrl, icon.image_unit, icon.image_loc,
                                      icon.draw_type, icon.flip);
}

}  // namespace ui_original

namespace ui_custom {

void draw_button(void* ctrl, UiRect size, uint32_t background, uint32_t border, int border_thickness,
                 const char* text, uint32_t text_color, int text_x, int text_y) {
    ui_draw_button_background(ctrl, size, background);
    ui_draw_button_border(ctrl, size, border, border_thickness);
    if (text != nullptr) ui_draw_text(ctrl, text_x, text_y, text_color);
}

}  // namespace ui_custom

namespace qol::game {
namespace {

using ui_container_frame_geometry::ImageLocation;
using ui_container_frame_geometry::SourceRect;

constexpr int32_t kNativePositionType = 0;
constexpr int32_t kNativeDrawMode = 1;
constexpr int32_t kNativeAlpha = 0;
// The audited unit-0x0f SGL texture path reads these fields from the image-group pointer.
constexpr size_t kImageGroupReadOffsets[] = {0x40, 0x44, 0x50, 0x60};
constexpr uint8_t kAllImageGroupFieldsReadable = 0x0f;

struct FrameLocation {
    void* native = nullptr;
    ImageLocation metadata{};
};

bool rect_fits(UiRect rect) {
    return ui_container_frame_geometry::valid_rect(rect);
}

bool read_location(int32_t id, FrameLocation* out) {
    if (out == nullptr) return false;
    out->native = nullptr;
    out->metadata = {};
    if (fn_imgsys_get_loc == nullptr) return false;
    void* native = fn_imgsys_get_loc(ui_container_frame_geometry::kFrameImageUnit, id);
    if (native == nullptr) return false;
    out->native = native;
    if (!game_memory_accessible(native, 0x18, 'r')) return false;

    const auto* bytes = static_cast<const uint8_t*>(native);
    ImageLocation metadata{};
    std::memcpy(&metadata.source_x, bytes + IMAGE_LOC_SOURCE_X, sizeof(metadata.source_x));
    std::memcpy(&metadata.source_y, bytes + IMAGE_LOC_SOURCE_Y, sizeof(metadata.source_y));
    std::memcpy(&metadata.width, bytes + IMAGE_LOC_SOURCE_WIDTH, sizeof(metadata.width));
    std::memcpy(&metadata.height, bytes + IMAGE_LOC_SOURCE_HEIGHT, sizeof(metadata.height));
    std::memcpy(&metadata.offset_x, bytes + IMAGE_LOC_OFFSET_X, sizeof(metadata.offset_x));
    std::memcpy(&metadata.offset_y, bytes + IMAGE_LOC_OFFSET_Y, sizeof(metadata.offset_y));
    out->metadata = metadata;
    return true;
}

struct ImageGroupReadability {
    uint8_t field_mask = 0;

    bool drawable() const { return field_mask == kAllImageGroupFieldsReadable; }
};

ImageGroupReadability image_group_readability(void* group) {
    ImageGroupReadability result{};
    if (group == nullptr) return result;
    const uintptr_t base = reinterpret_cast<uintptr_t>(group);
    for (size_t i = 0; i < sizeof(kImageGroupReadOffsets) / sizeof(kImageGroupReadOffsets[0]); ++i) {
        const uintptr_t offset = kImageGroupReadOffsets[i];
        if (base > std::numeric_limits<uintptr_t>::max() - offset - sizeof(uint32_t)) continue;
        const auto* field = reinterpret_cast<const void*>(base + offset);
        if (game_memory_accessible(field, sizeof(uint32_t), 'r')) {
            result.field_mask |= static_cast<uint8_t>(1u << i);
        }
    }
    return result;
}

#if !defined(NDEBUG)
bool g_container_frame_result_logged = false;

void log_container_frame_result(bool succeeded, const char* stage) {
    if (g_container_frame_result_logged) return;
    g_container_frame_result_logged = true;
    if (succeeded) {
        QOL_LOG_SEAM_INFO(kQolLogDomainUi, "container frame first result=ok");
    } else {
        QOL_LOG_SEAM_WARN(kQolLogDomainUi, "container frame first result=failed stage=%s", stage);
    }
}
#else
void log_container_frame_result(bool, const char*) {}
#endif

// The native caller supplies absolute atlas source coordinates; loc2 is not part of this range.
bool validate_horizontal_edge(const ui_container_frame_geometry::ContainerFrameProfile& profile,
                              UiRect rect, const FrameLocation& location, int32_t* out_tile,
                              SourceRect* out_crop,
                              const char** out_failure_stage) {
    const int64_t length = rect.w - 2 * profile.border_inset;
    if (out_tile == nullptr || out_crop == nullptr || out_failure_stage == nullptr) return false;
    *out_failure_stage = "horizontal-source-window-invalid";
    const int32_t available_source = ui_container_frame_geometry::horizontal_source_extent(
        profile, location.metadata);
    *out_tile = ui_container_frame_geometry::bounded_tile_length(
        length, available_source, profile.horizontal.tile_limit);
    if (length == 0) return true;
    if (!ui_container_frame_geometry::horizontal_source_rect(
            profile, location.metadata, *out_tile, out_crop)) return false;
    *out_failure_stage = "horizontal-destination-overflow";
    const int64_t start_x = ui_container_frame_geometry::horizontal_start_x(profile, rect);
    const int64_t bottom_y = ui_container_frame_geometry::bottom_edge_anchor_y(rect);
    return ui_container_frame_geometry::edge_anchors_fit(
               start_x, length, *out_tile, 0) &&
        ui_container_frame_geometry::fits_int32(rect.y) &&
        ui_container_frame_geometry::fits_int32(bottom_y);
}

bool validate_vertical_edge(const ui_container_frame_geometry::ContainerFrameProfile& profile,
                            UiRect rect, const FrameLocation& location, int32_t* out_tile,
                            SourceRect* out_crop, const char** out_failure_stage) {
    const int64_t length = rect.h - 2 * profile.border_inset;
    if (out_tile == nullptr || out_crop == nullptr || out_failure_stage == nullptr) return false;
    *out_failure_stage = "vertical-source-window-invalid";
    const int32_t available_source = ui_container_frame_geometry::vertical_source_extent(
        profile, location.metadata);
    *out_tile = ui_container_frame_geometry::bounded_tile_length(
        length, available_source, profile.vertical.tile_limit);
    if (length == 0) return true;
    if (*out_tile <= 0 || !ui_container_frame_geometry::vertical_source_rect(
            profile, location.metadata, *out_tile, out_crop)) {
        return false;
    }
    *out_failure_stage = "vertical-destination-overflow";
    const int64_t start_y = ui_container_frame_geometry::vertical_start_y(profile, rect);
    const int64_t right_x = ui_container_frame_geometry::right_edge_anchor_x(rect);
    return ui_container_frame_geometry::edge_anchors_fit(
               start_y, length, *out_tile, 0) &&
        ui_container_frame_geometry::fits_int32(rect.x) &&
        ui_container_frame_geometry::fits_int32(right_x);
}

// Argument order and flip selectors match the audited shared UI_003 renderer profile.
void draw_horizontal_edge(void* group,
                          const ui_container_frame_geometry::ContainerFrameProfile& profile,
                          UiRect rect, ui_container_frame_geometry::Edge edge,
                          int32_t tile_length, const SourceRect& source_template) {
    const int64_t destination_x = ui_container_frame_geometry::horizontal_start_x(profile, rect);
    const int64_t destination_y = edge == ui_container_frame_geometry::Edge::kTop
        ? rect.y
        : ui_container_frame_geometry::bottom_edge_anchor_y(rect);
    const int64_t length = rect.w - 2 * profile.border_inset;
    for (int64_t offset = 0; offset < length;) {
        const int32_t width = static_cast<int32_t>(std::min<int64_t>(tile_length, length - offset));
        SourceRect source = source_template;
        source.width = width;
        const int32_t x = static_cast<int32_t>(destination_x + offset);
        const int32_t flip = ui_container_frame_geometry::native_flip_selector(edge);
        if (flip >= 0) {
            fn_grpx_draw_part_flip_ex(group, x, static_cast<int32_t>(destination_y), source.x,
                                      source.y, source.width, source.height, 0, 0,
                                      kNativePositionType, kNativeDrawMode, kNativeAlpha, flip);
        } else {
            fn_grpx_draw_part_ex(group, x, static_cast<int32_t>(destination_y), source.x,
                                 source.y, source.width, source.height, 0, 0,
                                 kNativePositionType, kNativeDrawMode, kNativeAlpha);
        }
        offset += width;
    }
}

void draw_vertical_edge(void* group,
                        const ui_container_frame_geometry::ContainerFrameProfile& profile,
                        UiRect rect, ui_container_frame_geometry::Edge edge,
                        int32_t tile_length, const SourceRect& source_template) {
    const int64_t destination_x = edge == ui_container_frame_geometry::Edge::kLeft
        ? rect.x
        : ui_container_frame_geometry::right_edge_anchor_x(rect);
    const int64_t destination_y = ui_container_frame_geometry::vertical_start_y(profile, rect);
    const int64_t length = rect.h - 2 * profile.border_inset;
    for (int64_t offset = 0; offset < length;) {
        const int32_t height = static_cast<int32_t>(std::min<int64_t>(tile_length, length - offset));
        SourceRect source = source_template;
        source.height = height;
        const int32_t y = static_cast<int32_t>(destination_y + offset);
        const int32_t flip = ui_container_frame_geometry::native_flip_selector(edge);
        if (flip >= 0) {
            fn_grpx_draw_part_flip_ex(group, static_cast<int32_t>(destination_x), y, source.x,
                                      source.y, source.width, source.height, 0, 0,
                                      kNativePositionType, kNativeDrawMode, kNativeAlpha, flip);
        } else {
            fn_grpx_draw_part_ex(group, static_cast<int32_t>(destination_x), y, source.x,
                                 source.y, source.width, source.height, 0, 0,
                                 kNativePositionType, kNativeDrawMode, kNativeAlpha);
        }
        offset += height;
    }
}

void submit_fill_rect(UiRect rect, uint32_t fill_rgb565, uint32_t alpha) {
    fn_grpx_fill_rect_alpha(static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y),
                            static_cast<int32_t>(rect.w), static_cast<int32_t>(rect.h),
                            fill_rgb565, alpha);
}

void draw_profile_fill_rows(
    const ui_container_frame_geometry::ContainerFrameProfile& profile, UiRect rect,
    int64_t local_y_base, int32_t first_row, int32_t end_row,
    uint32_t fill_rgb565, uint32_t alpha) {
    UiRect pending{};
    bool has_pending = false;
    const auto flush = [&]() {
        if (has_pending) submit_fill_rect(pending, fill_rgb565, alpha);
        has_pending = false;
    };

    for (int32_t profile_row = first_row; profile_row < end_row; ++profile_row) {
        UiRect row_span{};
        if (!ui_container_frame_geometry::profile_fill_row_span(
                profile, rect, local_y_base + profile_row, &row_span)) {
            flush();
            continue;
        }
        if (has_pending && pending.x == row_span.x && pending.w == row_span.w &&
            pending.y + pending.h == row_span.y) {
            ++pending.h;
        } else {
            flush();
            pending = row_span;
            has_pending = true;
        }
    }
    flush();
}

void draw_profile_fill(const ui_container_frame_geometry::ContainerFrameProfile& profile,
                       UiRect rect, uint32_t fill_rgb565, uint32_t alpha) {
    if (alpha == 0) return;
    draw_profile_fill_rows(profile, rect, 0, profile.straight_decoration_thickness,
                           profile.border_inset, fill_rgb565, alpha);
    UiRect body{};
    if (ui_container_frame_geometry::profile_body_fill_rect(profile, rect, &body)) {
        submit_fill_rect(body, fill_rgb565, alpha);
    }
    draw_profile_fill_rows(profile, rect, rect.h - profile.border_inset, 0,
                           profile.border_inset - profile.straight_decoration_thickness,
                           fill_rgb565, alpha);
}

struct CornerDrawCall {
    int32_t destination_x;
    int32_t destination_y;
    SourceRect source;
};

struct CornerDrawPlan {
    CornerDrawCall calls[4 * 24]{};
    size_t count = 0;
};

bool build_corner_plan(const ui_container_frame_geometry::ContainerFrameProfile& profile,
                       UiRect rect, const FrameLocation (&corners)[4],
                       CornerDrawPlan* out_plan) {
    if (out_plan == nullptr) return false;
    CornerDrawPlan plan{};
    using ui_container_frame_geometry::Corner;
    constexpr Corner kCorners[] = {
        Corner::kTopLeft, Corner::kTopRight, Corner::kBottomLeft, Corner::kBottomRight};
    for (size_t corner = 0; corner < 4; ++corner) {
        const bool right = kCorners[corner] == Corner::kTopRight ||
            kCorners[corner] == Corner::kBottomRight;
        const bool bottom = kCorners[corner] == Corner::kBottomLeft ||
            kCorners[corner] == Corner::kBottomRight;
        const int64_t base_x = right ? rect.x + rect.w - profile.border_inset
                                     : rect.x;
        const int64_t base_y = bottom ? rect.y + rect.h - profile.border_inset
                                      : rect.y;
        for (int32_t first_row = 0; first_row < profile.border_inset;) {
            const int32_t inset = ui_container_frame_geometry::corner_profile_inset(
                profile, kCorners[corner], first_row);
            int32_t end_row = first_row + 1;
            while (end_row < profile.border_inset &&
                   ui_container_frame_geometry::corner_profile_inset(
                       profile, kCorners[corner], end_row) == inset) {
                ++end_row;
            }
            ui_container_frame_geometry::CornerSourceRun run{};
            if (!ui_container_frame_geometry::corner_source_run(
                    profile, corners[corner].metadata, kCorners[corner], first_row,
                    end_row - first_row, &run) ||
                !ui_container_frame_geometry::fits_int32(base_x + run.destination_offset_x) ||
                !ui_container_frame_geometry::fits_int32(base_y + run.destination_offset_y)) {
                return false;
            }
            if (plan.count >= sizeof(plan.calls) / sizeof(plan.calls[0])) return false;
            plan.calls[plan.count++] = {
                static_cast<int32_t>(base_x + run.destination_offset_x),
                static_cast<int32_t>(base_y + run.destination_offset_y),
                {run.source_x, run.source_y, run.source_width, run.source_height}};
            first_row = end_row;
        }
    }
    *out_plan = plan;
    return true;
}

void draw_corner_runs(void* group, const CornerDrawPlan& plan) {
    for (size_t i = 0; i < plan.count; ++i) {
        const CornerDrawCall& call = plan.calls[i];
        fn_grpx_draw_part_ex(group, call.destination_x, call.destination_y,
                             call.source.x, call.source.y, call.source.width,
                             call.source.height, 0, 0, kNativePositionType,
                             kNativeDrawMode, kNativeAlpha);
    }
}

}  // namespace

bool ui_draw_container_frame(UiRect rect, uint32_t fill_rgb565, uint32_t alpha) {
    using namespace ui_container_frame_geometry;
    if (!rect_fits(rect) || fill_rgb565 > 0xffffu || alpha > 100u) {
        log_container_frame_result(false, "invalid-arguments");
        return false;
    }
    if (!game_host_is_main_thread()) {
        log_container_frame_result(false, "main-thread-unavailable");
        return false;
    }
    if (fn_imgsys_unit_load == nullptr || fn_imgsys_get_group == nullptr ||
        fn_imgsys_get_loc == nullptr) {
        log_container_frame_result(false, "required-symbol-missing");
        return false;
    }

    fn_imgsys_unit_load(kFrameImageUnit);  // Synchronous wrapper, shared atlas is never unloaded.
    void* group = fn_imgsys_get_group(kFrameImageUnit);
    if (group == nullptr) {
        log_container_frame_result(false, "image-group-unavailable");
        return false;
    }
    // IMGSYS_GetGroup returns the raw unit-table image-group pointer consumed by SGL.
    const ImageGroupReadability group_probe = image_group_readability(group);
    if (!group_probe.drawable()) {
        log_container_frame_result(false, "image-group-fields-unreadable");
        return false;
    }

    FrameLocation horizontal{};
    FrameLocation vertical{};
    if (!read_location(kHorizontalLocId, &horizontal)) {
        log_container_frame_result(false, "horizontal-source-loc-unavailable");
        return false;
    }
    if (!read_location(kVerticalLocId, &vertical)) {
        log_container_frame_result(false, "vertical-source-loc-unavailable");
        return false;
    }

    FrameLocation corners[4]{};
    for (size_t i = 0; i < 4; ++i) {
        if (!read_location(kFrameCornerLocIds[i], &corners[i])) {
            log_container_frame_result(false, "corner-loc-unavailable");
            return false;
        }
    }
    const ImageLocation corner_metadata[4] = {
        corners[0].metadata, corners[1].metadata, corners[2].metadata, corners[3].metadata};
    const ContainerFrameProfile* profile = select_frame_profile(
        horizontal.metadata, vertical.metadata, corner_metadata);
    if (profile == nullptr || profile->image_unit != kFrameImageUnit ||
        profile->horizontal_loc != kHorizontalLocId || profile->vertical_loc != kVerticalLocId) {
        log_container_frame_result(false, "frame-profile-metadata-mismatch");
        return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (profile->corner_loc_ids[i] != kFrameCornerLocIds[i]) {
            log_container_frame_result(false, "frame-profile-loc-schema-mismatch");
            return false;
        }
    }
    if (!valid_rect(*profile, rect) || !profile_fill_layout_valid(*profile, rect)) {
        log_container_frame_result(false, "frame-fill-profile-invalid");
        return false;
    }
    CornerDrawPlan corner_plan{};
    if (!build_corner_plan(*profile, rect, corners, &corner_plan)) {
        log_container_frame_result(false, "corner-source-profile-invalid");
        return false;
    }

    const int64_t horizontal_length = rect.w - 2 * profile->border_inset;
    const int64_t vertical_length = rect.h - 2 * profile->border_inset;
    int32_t horizontal_tile = 0;
    int32_t vertical_tile = 0;
    SourceRect horizontal_source{};
    SourceRect vertical_source{};
    const char* failure_stage = nullptr;
    if (!validate_horizontal_edge(*profile, rect, horizontal, &horizontal_tile,
                                  &horizontal_source, &failure_stage)) {
        log_container_frame_result(false, failure_stage);
        return false;
    }
    if (!validate_vertical_edge(*profile, rect, vertical, &vertical_tile,
                                &vertical_source, &failure_stage)) {
        log_container_frame_result(false, failure_stage);
        return false;
    }
    if (fn_grpx_draw_part_ex == nullptr) {
        log_container_frame_result(false, "grpx-draw-part-ex-missing");
        return false;
    }
    if ((horizontal_length > 0 || vertical_length > 0) &&
        fn_grpx_draw_part_flip_ex == nullptr) {
        log_container_frame_result(false, "grpx-draw-part-flip-ex-missing");
        return false;
    }
    const bool draw_fill = profile_fill_enabled(alpha) && profile_has_fill(*profile, rect);
    if (draw_fill && fn_grpx_fill_rect_alpha == nullptr) {
        log_container_frame_result(false, "grpx-fill-rect-alpha-missing");
        return false;
    }

    if (draw_fill) draw_profile_fill(*profile, rect, fill_rgb565, alpha);
    if (horizontal_length > 0) {
        draw_horizontal_edge(group, *profile, rect, Edge::kTop, horizontal_tile,
                             horizontal_source);
        draw_horizontal_edge(group, *profile, rect, Edge::kBottom, horizontal_tile,
                             horizontal_source);
    }
    if (vertical_length > 0) {
        draw_vertical_edge(group, *profile, rect, Edge::kLeft, vertical_tile, vertical_source);
        draw_vertical_edge(group, *profile, rect, Edge::kRight, vertical_tile, vertical_source);
    }
    draw_corner_runs(group, corner_plan);

    log_container_frame_result(true, "draw-submitted");
    return true;
}

}  // namespace qol::game
