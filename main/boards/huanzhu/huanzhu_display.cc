// 幻珠圆屏显示子类实现 —— 把字幕条收进 360x360 内切圆，同时校正顶部状态栏位置
//
// 刻意不调用 LvglTheme（板级代码的 include 路径不保证有 lvgl_theme.h），
// 只改几何：尺寸/位置/内边距，颜色与字体沿用上游 SetupUI 的结果。
#include "huanzhu_display.h"

#include <esp_log.h>

#define TAG "huanzhu_display"

// 内切圆几何：圆心 (180,180)，半径 180。
// 顶部状态栏：原贴顶边（y=0 处），圆内半宽≈0 → 图标与文字被切角。
// LV_ALIGN_TOP_MID 的 offset_y 正值 = 向下移。移到 y=60：
//   条高 ~24px，底边 y≈84 → 半宽 sqrt(180²-96²)≈152 → 可见 ~304px
// 图标条收窄到 300px（x=30..330）后左右图标都在圆内。
static constexpr int kTopBarOffsetY = 60;     // 向下移 60px（TOP_MID 正方向是下）
static constexpr int kTopBarWidth = 300;      // 图标条宽度（原 360 两端出圆）

// 底部字幕条：条中心放圆心下方 120px（y=300）：
//   圆内半宽 = sqrt(180^2 - 120^2) ≈ 134 → 可见 ~268px
//   条底边 y≈312（半宽 ≈122 → 可见 ~244px）
// 所以条宽取 240px 时整条（含底角）都在圆内。
// 表情图 64x61 居中（y≈149..210），与字幕条不重叠。
static constexpr int kBarWidth = 240;
static constexpr int kBarOffsetY = 120;
static constexpr int kBarPad = 4;

void HuanzhuRoundDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();

    FitSubtitleToCircle();
    FitTopBarToCircle();
}

void HuanzhuRoundDisplay::FitTopBarToCircle() {
    DisplayLockGuard lock(this);

    // 顶部图标条 top_bar_：原贴顶边且占满 360 宽，两端（网络图标/电量图标）都在圆外。
    // 下移 + 收窄：条移到 y=60..84，该区间圆内 x 范围 ≈ 28..332（半宽 sqrt(180²-96²)≈152），
    // 取宽 300 → x=30..330，左右图标都落进圆内。
    if (top_bar_ != nullptr) {
        lv_obj_set_width(top_bar_, kTopBarWidth);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, kTopBarOffsetY);
    }

    // 状态文字条 status_bar_（与 top_bar_ 重叠，装 status_label_/notification_label_）：
    // 内部 label 宽 0.75*360=270 居中 → x=45..315，本就在圆内，只需同样下移避开切角。
    if (status_bar_ != nullptr) {
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, kTopBarOffsetY);
    }

    ESP_LOGI(TAG, "Top bar/status fitted into circle: offset_y=%d w=%d (was full-width at screen top)",
             kTopBarOffsetY, kTopBarWidth);
}

void HuanzhuRoundDisplay::FitSubtitleToCircle() {
    DisplayLockGuard lock(this);
    if (bottom_bar_ == nullptr) {
        ESP_LOGW(TAG, "bottom_bar_ is nullptr, skip circle fit");
        return;
    }

    // 单行模式（未开 CONFIG_USE_MULTILINE_CHAT_MESSAGE）：
    // 上游只在 MULTILINE 分支里每次 SetChatMessage 重新 align 回底边，
    // 保持单行 → 这里的一次性校正不会被抵消。
    lv_obj_set_size(bottom_bar_, kBarWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, kBarPad, 0);
    lv_obj_set_style_pad_right(bottom_bar_, kBarPad, 0);
    lv_obj_set_style_radius(bottom_bar_, kBarWidth / 2, 0);  // 胶囊形，贴合圆屏
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_align(bottom_bar_, LV_ALIGN_CENTER, 0, kBarOffsetY);

    if (chat_message_label_ != nullptr) {
        lv_obj_set_width(chat_message_label_, kBarWidth - 2 * kBarPad);
        lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    }

    ESP_LOGI(TAG, "Subtitle fitted into circle: w=%d @ y+%d (was full-width at screen bottom)",
             kBarWidth, kBarOffsetY);
}
