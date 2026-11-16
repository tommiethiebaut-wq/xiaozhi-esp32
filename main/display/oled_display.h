#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

class OledDisplay : public LvglDisplay {
private:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* content_left_ = nullptr;
    lv_obj_t* content_right_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* emotion_label_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;

    // EVE-inspired animated eyes.
    lv_obj_t* eve_eye_left_ = nullptr;
    lv_obj_t* eve_eye_right_ = nullptr;

    // Eye animation timers and current expression/gaze state.
    lv_timer_t* eve_blink_timer_ = nullptr;
    lv_timer_t* eve_gaze_timer_ = nullptr;
    int eve_eye_width_ = 25;
    int eve_eye_height_ = 44;
    int eve_eye_y_ = 0;
    int eve_eye_x_ = 0;
    int eve_gaze_x_ = 0;
    int eve_gaze_y_ = 0;
    int eve_gaze_step_ = 0;
    bool eve_eyes_closed_ = false;
    bool eve_waking_up_ = false;
    bool eve_blink_reopening_ = false;
    bool eve_double_blink_pending_ = false;

    static void EveBlinkTimerCallback(lv_timer_t* timer);
    static void EveGazeTimerCallback(lv_timer_t* timer);

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    void SetupUI_128x64();
    void SetupUI_128x32();

public:
    OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, bool mirror_x, bool mirror_y);
    ~OledDisplay();

    virtual void SetupUI() override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void RemoveChatMessage(const char* role, const char* content) override;
    virtual bool SetEmotion(const char* emotion) override;
    virtual void SetTheme(Theme* theme) override;
    virtual bool IsMonochrome() const override { return true; }
    void SetPowerSaveMode(bool on) override;
};

#endif  // OLED_DISPLAY_H
