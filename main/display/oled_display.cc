#include "oled_display.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <material_symbols.h>
#include <noto_emoji.h>

#define TAG "OledDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_1);
LV_FONT_DECLARE(font_noto_emoji_30_1);

namespace {

// Fixed canvas dimensions for each eye.
constexpr int EVE_CANVAS_WIDTH = 32;
constexpr int EVE_CANVAS_HEIGHT = 50;

// Draw a vertically elongated, softly rounded white eye on a black canvas.
void EveRenderEye(uint16_t* buffer, int eye_width, int eye_height) {
    if (buffer == nullptr) return;

    // Start with a completely black canvas.
    std::fill(buffer, buffer + EVE_CANVAS_WIDTH * EVE_CANVAS_HEIGHT, 0x0000);

    eye_width = std::clamp(eye_width, 1, EVE_CANVAS_WIDTH);
    eye_height = std::clamp(eye_height, 1, EVE_CANVAS_HEIGHT);

    const int start_x = (EVE_CANVAS_WIDTH - eye_width) / 2;
    const int start_y = (EVE_CANVAS_HEIGHT - eye_height) / 2;

    for (int y = 0; y < eye_height; ++y) {
        const float position = (static_cast<float>(y) + 0.5f) / eye_height;
        const float curve = std::pow(std::sin(position * 3.14159265f), 0.35f);
        const int half_width = std::max(
            1, static_cast<int>((eye_width * curve) / 2.0f));

        for (int x = 0; x < eye_width; ++x) {
            const int relative_x = x - eye_width / 2;

            if (std::abs(relative_x) <= half_width) {
                const int px = start_x + x;
                const int py = start_y + y;

                if (px >= 0 && px < EVE_CANVAS_WIDTH &&
                    py >= 0 && py < EVE_CANVAS_HEIGHT) {
                    buffer[py * EVE_CANVAS_WIDTH + px] = 0xFFFF;
                }
            }
        }
    }
}

// Position callbacks for the eye canvases.
void EveSetEyeX(void* object, int32_t value) {
    if (object != nullptr) {
        lv_obj_set_x(static_cast<lv_obj_t*>(object), value);
    }
}

void EveSetEyeY(void* object, int32_t value) {
    if (object != nullptr) {
        lv_obj_set_y(static_cast<lv_obj_t*>(object), value);
    }
}

// Generic LVGL animation helper.
void EveAnimateValue(void* object, int32_t start, int32_t end,
                     uint32_t duration, lv_anim_exec_xcb_t callback) {
    if (object == nullptr) return;

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_values(&animation, start, end);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&animation, callback);
    lv_anim_start(&animation);
}

}  // namespace

OledDisplay::OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_1);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_1);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding OLED display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }
    // SetupUI() is called by Application::Initialize() after display initialization.
}

void OledDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();
    if (height_ == 64) {
        SetupUI_128x64();
    } else {
        SetupUI_128x32();
    }
}

OledDisplay::~OledDisplay() {
    if (eve_blink_timer_ != nullptr) {
        lv_timer_del(eve_blink_timer_);
        eve_blink_timer_ = nullptr;
    }
    if (eve_gaze_timer_ != nullptr) {
        lv_timer_del(eve_gaze_timer_);
        eve_gaze_timer_ = nullptr;
    }

    if (content_ != nullptr) lv_obj_del(content_);

    const bool is_128x64_layout = (top_bar_ != nullptr);
    if (status_bar_ != nullptr && is_128x64_layout) {
        status_label_ = nullptr;
        notification_label_ = nullptr;
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        network_label_ = nullptr;
        mute_label_ = nullptr;
        battery_label_ = nullptr;
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        if (!is_128x64_layout) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
        }
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) lv_obj_del(container_);

    if (panel_ != nullptr) esp_lcd_panel_del(panel_);
    if (panel_io_ != nullptr) esp_lcd_panel_io_del(panel_io_);
    lvgl_port_deinit();
}

bool OledDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void OledDisplay::Unlock() { lvgl_port_unlock(); }

void OledDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) return;

    std::string content_str = content ? content : "";
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');

    lv_anim_delete(chat_message_label_, nullptr);
    if (content_right_ == nullptr) {
        lv_label_set_text(chat_message_label_, content_str.c_str());
    } else if (content_str.empty()) {
        lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(chat_message_label_, content_str.c_str());
        lv_obj_remove_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
    }
}

void OledDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || content == nullptr) return;

    std::string content_str = content;
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');
    if (content_str != lv_label_get_text(chat_message_label_)) return;

    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, "");
    if (content_right_ != nullptr) lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
}

void OledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);

    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(content_, LV_FLEX_ALIGN_CENTER, 0);

    content_left_ = lv_obj_create(content_);
    lv_obj_set_size(content_left_, 32, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content_left_, 0, 0);
    lv_obj_set_style_border_width(content_left_, 0, 0);

    emotion_label_ = lv_label_create(content_left_);
    lv_obj_set_style_text_font(emotion_label_, large_icon_font, 0);
    lv_label_set_text(emotion_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(emotion_label_);
    lv_obj_set_style_pad_top(emotion_label_, 8, 0);

    content_right_ = lv_obj_create(content_);
    lv_obj_set_size(content_right_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content_right_, 0, 0);
    lv_obj_set_style_border_width(content_right_, 0, 0);
    lv_obj_set_flex_grow(content_right_, 1);
    lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);

    chat_message_label_ = lv_label_create(content_right_);
    lv_label_set_text(chat_message_label_, "");
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(chat_message_label_, width_ - 32);
    lv_obj_set_style_pad_top(chat_message_label_, 14, 0);

    static lv_anim_t subtitle_anim;
    lv_anim_init(&subtitle_anim);
    lv_anim_set_delay(&subtitle_anim, 1000);
    lv_anim_set_repeat_count(&subtitle_anim, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &subtitle_anim, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_,
                                   lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

        // Full-screen black EVE face.
    lv_obj_t* eve_face = lv_obj_create(screen);
    lv_obj_set_size(eve_face, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(eve_face, 0, 0);
    lv_obj_set_style_bg_color(eve_face, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(eve_face, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(eve_face, 0, 0);
    lv_obj_set_style_pad_all(eve_face, 0, 0);
    lv_obj_set_style_radius(eve_face, 0, 0);
    lv_obj_clear_flag(eve_face, LV_OBJ_FLAG_SCROLLABLE);

    // Separate RGB565 drawing buffers for the two eyes.
    static uint16_t eve_left_buffer[EVE_CANVAS_WIDTH * EVE_CANVAS_HEIGHT];
    static uint16_t eve_right_buffer[EVE_CANVAS_WIDTH * EVE_CANVAS_HEIGHT];

    eve_left_state_.buffer = eve_left_buffer;
    eve_right_state_.buffer = eve_right_buffer;

    eve_left_state_.width = 25;
    eve_left_state_.height = 3;
    eve_right_state_.width = 25;
    eve_right_state_.height = 3;

    // Create the left eye canvas.
    eve_eye_left_ = lv_canvas_create(eve_face);
    eve_left_state_.canvas = eve_eye_left_;

    lv_canvas_set_buffer(
        eve_eye_left_,
        eve_left_state_.buffer,
        EVE_CANVAS_WIDTH,
        EVE_CANVAS_HEIGHT,
        LV_COLOR_FORMAT_RGB565
    );

    lv_obj_set_size(eve_eye_left_, EVE_CANVAS_WIDTH, EVE_CANVAS_HEIGHT);
    lv_obj_set_pos(eve_eye_left_, 24, 7);
    EveRenderEye(eve_left_state_.buffer, eve_left_state_.width,
                 eve_left_state_.height);

    // Create the right eye canvas.
    eve_eye_right_ = lv_canvas_create(eve_face);
    eve_right_state_.canvas = eve_eye_right_;

    lv_canvas_set_buffer(
        eve_eye_right_,
        eve_right_state_.buffer,
        EVE_CANVAS_WIDTH,
        EVE_CANVAS_HEIGHT,
        LV_COLOR_FORMAT_RGB565
    );

    lv_obj_set_size(eve_eye_right_, EVE_CANVAS_WIDTH, EVE_CANVAS_HEIGHT);
    lv_obj_set_pos(eve_eye_right_, 73, 7);
    EveRenderEye(eve_right_state_.buffer, eve_right_state_.width,
                 eve_right_state_.height);

    // Start with closed eyes, then smoothly open them.
    eve_waking_up_ = true;
    eve_eyes_closed_ = true;

    EveAnimateValue(&eve_left_state_, 3, eve_eye_height_, 900,
                    EveSetEyeHeight);
    EveAnimateValue(&eve_right_state_, 3, eve_eye_height_, 900,
                    EveSetEyeHeight);

    eve_blink_timer_ = lv_timer_create(EveBlinkTimerCallback, 3300, this);
    eve_gaze_timer_ = lv_timer_create(EveGazeTimerCallback, 4100, this);

void OledDisplay::SetupUI_128x32() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);

    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_column(container_, 0, 0);

    content_ = lv_obj_create(container_);
    lv_obj_set_size(content_, 32, 32);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_radius(content_, 0, 0);

    emotion_label_ = lv_label_create(content_);
    lv_obj_set_style_text_font(emotion_label_, large_icon_font, 0);
    lv_label_set_text(emotion_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(emotion_label_);

    side_bar_ = lv_obj_create(container_);
    lv_obj_set_size(side_bar_, width_ - 32, 32);
    lv_obj_set_flex_flow(side_bar_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(side_bar_, 0, 0);
    lv_obj_set_style_border_width(side_bar_, 0, 0);
    lv_obj_set_style_radius(side_bar_, 0, 0);
    lv_obj_set_style_pad_row(side_bar_, 0, 0);

    status_bar_ = lv_obj_create(side_bar_);
    lv_obj_set_size(status_bar_, width_ - 32, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_obj_set_style_pad_left(status_label_, 2, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_pad_left(notification_label_, 2, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    chat_message_label_ = lv_label_create(side_bar_);
    lv_obj_set_size(chat_message_label_, width_ - 32, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(chat_message_label_, 2, 0);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(chat_message_label_, "");

    static lv_anim_t subtitle_anim;
    lv_anim_init(&subtitle_anim);
    lv_anim_set_delay(&subtitle_anim, 1000);
    lv_anim_set_repeat_count(&subtitle_anim, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &subtitle_anim, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_,
                                   lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);
}


void OledDisplay::EveBlinkTimerCallback(lv_timer_t* timer) {
   void OledDisplay::EveSetEyeWidth(void* state, int32_t value) {
    auto* eye = static_cast<EveEyeState*>(state);
    if (eye == nullptr || eye->buffer == nullptr) return;

    eye->width = std::clamp(static_cast<int>(value), 1, 32);
    EveRenderEye(eye->buffer, eye->width, eye->height);
    if (eye->canvas != nullptr) lv_obj_invalidate(eye->canvas);
}

void OledDisplay::EveSetEyeHeight(void* state, int32_t value) {
    auto* eye = static_cast<EveEyeState*>(state);
    if (eye == nullptr || eye->buffer == nullptr) return;

    eye->height = std::clamp(static_cast<int>(value), 1, 50);
    EveRenderEye(eye->buffer, eye->width, eye->height);
    if (eye->canvas != nullptr) lv_obj_invalidate(eye->canvas);
}

void OledDisplay::EveBlinkTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));

    if (display == nullptr || display->eve_eye_left_ == nullptr ||
        display->eve_eye_right_ == nullptr) {
        return;
    }

    if (display->eve_waking_up_) {
        display->eve_waking_up_ = false;
        display->eve_eyes_closed_ = false;
        lv_timer_set_period(timer, 2600);
        lv_timer_reset(timer);
        return;
    }

    if (display->eve_blink_reopening_) {
        display->eve_blink_reopening_ = false;
        display->eve_eyes_closed_ = false;

        const uint32_t timing = lv_tick_get();

        if (display->eve_double_blink_pending_) {
            display->eve_double_blink_pending_ = false;
            lv_timer_set_period(timer, 2400 + (timing % 2600));
        } else {
            display->eve_double_blink_pending_ = (timing % 7 == 0);
            lv_timer_set_period(
                timer,
                display->eve_double_blink_pending_ ? 320 : 2400 + (timing % 2600));
        }

        lv_timer_reset(timer);
        return;
    }

    // Close both eyes.
    if (!display->eve_eyes_closed_) {
        lv_anim_delete(&display->eve_left_state_, nullptr);
        lv_anim_delete(&display->eve_right_state_, nullptr);

        display->eve_eyes_closed_ = true;

        EveAnimateValue(&display->eve_left_state_,
                        display->eve_left_state_.height, 3, 100,
                        EveSetEyeHeight);

        EveAnimateValue(&display->eve_right_state_,
                        display->eve_right_state_.height, 3, 100,
                        EveSetEyeHeight);

        lv_timer_set_period(timer, 150);
        lv_timer_reset(timer);
        return;
    }

    // Reopen both eyes using the latest expression.
    const int target_left_x =
        24 + display->eve_eye_x_ + display->eve_gaze_x_;

    const int target_right_x =
        73 + display->eve_eye_x_ + display->eve_gaze_x_;

    const int target_y =
        7 + display->eve_eye_y_ + display->eve_gaze_y_;

    lv_anim_delete(&display->eve_left_state_, nullptr);
    lv_anim_delete(&display->eve_right_state_, nullptr);
    lv_anim_delete(display->eve_eye_left_, nullptr);
    lv_anim_delete(display->eve_eye_right_, nullptr);

    EveAnimateValue(&display->eve_left_state_,
                    display->eve_left_state_.height,
                    display->eve_eye_height_, 130, EveSetEyeHeight);

    EveAnimateValue(&display->eve_right_state_,
                    display->eve_right_state_.height,
                    display->eve_eye_height_, 130, EveSetEyeHeight);

    EveAnimateValue(&display->eve_left_state_,
                    display->eve_left_state_.width,
                    display->eve_eye_width_, 130, EveSetEyeWidth);

    EveAnimateValue(&display->eve_right_state_,
                    display->eve_right_state_.width,
                    display->eve_eye_width_, 130, EveSetEyeWidth);

    EveAnimateValue(display->eve_eye_left_,
                    lv_obj_get_x(display->eve_eye_left_),
                    target_left_x, 130, EveSetEyeX);

    EveAnimateValue(display->eve_eye_right_,
                    lv_obj_get_x(display->eve_eye_right_),
                    target_right_x, 130, EveSetEyeX);

    EveAnimateValue(display->eve_eye_left_,
                    lv_obj_get_y(display->eve_eye_left_),
                    target_y, 130, EveSetEyeY);

    EveAnimateValue(display->eve_eye_right_,
                    lv_obj_get_y(display->eve_eye_right_),
                    target_y, 130, EveSetEyeY);

    display->eve_blink_reopening_ = true;
    lv_timer_set_period(timer, 150);
    lv_timer_reset(timer);
}

void OledDisplay::EveGazeTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));

    if (display == nullptr || display->eve_eye_left_ == nullptr ||
        display->eve_eye_right_ == nullptr || display->eve_eyes_closed_) {
        return;
    }

    static const int8_t gaze_x[] = {0, -4, 0, 4, 0, 0, 0, 0};
    static const int8_t gaze_y[] = {0, 0, -2, 0, 0, 2, 0, 0};

    const int phase = display->eve_gaze_step_ % 8;
    display->eve_gaze_step_ = (display->eve_gaze_step_ + 1) % 8;

    display->eve_gaze_x_ = gaze_x[phase];
    display->eve_gaze_y_ = gaze_y[phase];

    const int left_x =
        24 + display->eve_eye_x_ + display->eve_gaze_x_;

    const int right_x =
        73 + display->eve_eye_x_ + display->eve_gaze_x_;

    const int target_y =
        7 + display->eve_eye_y_ + display->eve_gaze_y_;

    EveAnimateValue(display->eve_eye_left_,
                    lv_obj_get_x(display->eve_eye_left_),
                    left_x, 420, EveSetEyeX);

    EveAnimateValue(display->eve_eye_right_,
                    lv_obj_get_x(display->eve_eye_right_),
                    right_x, 420, EveSetEyeX);

    EveAnimateValue(display->eve_eye_left_,
                    lv_obj_get_y(display->eve_eye_left_),
                    target_y, 420, EveSetEyeY);

    EveAnimateValue(display->eve_eye_right_,
                    lv_obj_get_y(display->eve_eye_right_),
                    target_y, 420, EveSetEyeY);

    lv_timer_set_period(timer, 2800 + (lv_tick_get() % 2600));
}

bool OledDisplay::SetEmotion(const char* emotion) {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    const char* utf8 = noto_emoji_get_utf8(emotion);
    const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
    if (utf8 == nullptr) {
        utf8 = material_symbols_get_utf8(emotion);
        emotion_font = lvgl_theme->large_icon_font()->font();
    }

    DisplayLockGuard lock(this);

    if (eve_eye_left_ != nullptr && eve_eye_right_ != nullptr) {
        const std::string mood = emotion ? emotion : "";

                int target_width = 25;
        int target_height = 44;
        int target_y = 0;
        int target_x = 0;

        // Happy and amused: broad, softly squinted eyes.
        if (mood == "happy" || mood == "laughing" ||
            mood == "joy" || mood == "amused") {
            target_width = 31;
            target_height = 20;
            target_y = 9;

        // Sad: narrower, slightly lowered eyes.
        } else if (mood == "sad" || mood == "crying") {
            target_width = 25;
            target_height = 30;
            target_y = 4;

        // Surprise and excitement: tall, open eyes.
        } else if (mood == "surprised" || mood == "excited") {
            target_width = 21;
            target_height = 48;

        // Angry and determined: wider, narrowed eyes.
        } else if (mood == "angry" || mood == "determined") {
            target_width = 29;
            target_height = 21;
            target_y = 8;

        // Sleepy and bored: very relaxed, half-closed eyes.
        } else if (mood == "sleepy" || mood == "relaxed" ||
                   mood == "bored") {
            target_width = 31;
            target_height = 10;
            target_y = 12;

        } else if (mood == "sleeping") {
            target_width = 31;
            target_height = 3;
            target_y = 12;

        // Thinking and focused: slightly shifted, attentive eyes.
        } else if (mood == "thinking" || mood == "confused" ||
                   mood == "focused" || mood == "pondering") {
            target_width = 23;
            target_height = 35;
            target_x = 2;

        // Curious: tall, alert eyes.
        } else if (mood == "curious" || mood == "embarrassed") {
            target_width = 22;
            target_height = 44;

        } else if (mood == "listening" || mood == "attentive") {
            target_height = 43;

        } else if (mood == "speaking") {
            target_width = 29;
            target_height = 34;
        }

        eve_eye_width_ = target_width;
        eve_eye_height_ = target_height;
        eve_eye_y_ = target_y;
        eve_eye_x_ = target_x;

               if (!eve_eyes_closed_) {
            const int left_x =
                24 + eve_eye_x_ + eve_gaze_x_;

            const int right_x =
                73 + eve_eye_x_ + eve_gaze_x_;

            const int target_eye_y =
                7 + eve_eye_y_ + eve_gaze_y_;

            // Stop previous animations before applying the new expression.
            lv_anim_delete(&eve_left_state_, nullptr);
            lv_anim_delete(&eve_right_state_, nullptr);
            lv_anim_delete(eve_eye_left_, nullptr);
            lv_anim_delete(eve_eye_right_, nullptr);

            // Animate the actual eye shapes.
            EveAnimateValue(&eve_left_state_,
                            eve_left_state_.width,
                            target_width, 300, EveSetEyeWidth);

            EveAnimateValue(&eve_right_state_,
                            eve_right_state_.width,
                            target_width, 300, EveSetEyeWidth);

            EveAnimateValue(&eve_left_state_,
                            eve_left_state_.height,
                            target_height, 300, EveSetEyeHeight);

            EveAnimateValue(&eve_right_state_,
                            eve_right_state_.height,
                            target_height, 300, EveSetEyeHeight);

            // Move the eye canvases for gaze and expression positioning.
            EveAnimateValue(eve_eye_left_,
                            lv_obj_get_x(eve_eye_left_),
                            left_x, 300, EveSetEyeX);

            EveAnimateValue(eve_eye_right_,
                            lv_obj_get_x(eve_eye_right_),
                            right_x, 300, EveSetEyeX);

            EveAnimateValue(eve_eye_left_,
                            lv_obj_get_y(eve_eye_left_),
                            target_eye_y, 300, EveSetEyeY);

            EveAnimateValue(eve_eye_right_,
                            lv_obj_get_y(eve_eye_right_),
                            target_eye_y, 300, EveSetEyeY);
        }
        return true;
    }

    if (emotion_label_ == nullptr) return false;
    if (utf8 != nullptr) {
        lv_obj_set_style_text_font(emotion_label_, emotion_font, 0);
        lv_label_set_text(emotion_label_, utf8);
        return true;
    }
    lv_obj_set_style_text_font(emotion_label_, lvgl_theme->emoji_font()->font(), 0);
    lv_label_set_text(emotion_label_, NOTO_EMOJI_NEUTRAL);
    return false;
}

void OledDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font = lvgl_theme->text_font()->font();
    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
}

void OledDisplay::SetPowerSaveMode(bool on) {
    if (panel_) {
        Settings settings("wifi", false);
        if (settings.GetBool("power_save_display_off", false)) {
            esp_lcd_panel_disp_on_off(panel_, !on);
        }
    }
    LvglDisplay::SetPowerSaveMode(on);
}
