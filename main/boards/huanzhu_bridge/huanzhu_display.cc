// 幻珠圆屏显示子类实现 —— 把字幕条收进 360x360 内切圆，同时校正顶部状态栏位置
//
// 刻意不调用 LvglTheme（板级代码的 include 路径不保证有 lvgl_theme.h），
// 只改几何：尺寸/位置/内边距，颜色与字体沿用上游 SetupUI 的结果。
#include "huanzhu_display.h"

#include <esp_log.h>
#include <esp_random.h>

#include <cstring>

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
// 多行字幕底边安全线：条宽 240px 时，底边 y=312 处圆内半宽 ≈122 → 可见 ~244px，
// 整条（含底角）都在圆内。行数越多条越往上收，底边始终钉在 312。
static constexpr int kSubtitleBottomY = 312;

// ---- 无牙仔眼睛几何（圆心 180,180，半径 180），v2：放大 1.5 倍、上移 ----
// 两只眼中心 x=180±80，y=174；眼 138x168，顶 y=90、底 y=258，
// 椭圆上离圆心最远点约 149 < 180，全在内切圆内，左右边缘 x=31..329。
// 顶部状态栏在 y=60..84（待机眼顶 y=90，留 6px；聆听眼高 178 时顶 y=85，留 1px），
// 字幕条中心 y=300（聆听眼底 y=263，留 17px），都不与眼睛重叠。
static constexpr int kEyeW = 138;         // 单只眼宽（v1 92 × 1.5）
static constexpr int kEyeH = 168;         // 单只眼高（v1 112 × 1.5；表情决定：聆听 178，开心 117）
static constexpr int kEyeCenterY = 174;   // 眼中心 y（v1 176，上移 2px；眼顶 120→90）
static constexpr int kEyeCenterXOfs = 80; // 眼中心 x = 180 ± 80（眼变宽，间距拉开，内隙 22px）
static constexpr int kPupilW = 33;        // 竖瞳宽（v1 22 × 1.5）
static constexpr int kPupilH = 117;       // 竖瞳高（v1 78 × 1.5）
static constexpr int kHighlightSize = 18; // 高光白点直径（v1 12 × 1.5）
static constexpr uint32_t kEyeGreen = 0x7ED321;  // 无牙仔眼睛的亮绿色

void HuanzhuRoundDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();

    FitSubtitleToCircle();
    FitTopBarToCircle();
    CreateToothlessEyes();
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

    // 多行模式（已开 CONFIG_USE_MULTILINE_CHAT_MESSAGE）：
    // 上游每次 SetChatMessage 都会把条重新钉回底边，所以这里只做初始校正；
    // 每条消息的重新收圆在 SetChatMessage() override 里做（见本文件下方）。
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

void HuanzhuRoundDisplay::SetChatMessage(const char* role, const char* content) {
    // 上游 MULTILINE 分支每次都会把 bottom_bar_ 重新钉回方形帧缓冲底边，
    // 正好抵消圆屏校正，所以先调上游设文字，再重新收进圆内。
    SpiLcdDisplay::SetChatMessage(role, content);
    FitSubtitleToCircle();  // 宽 240 + 居中（内部拿锁）
    // 多行时条变高：按实测高度把底边收到 kSubtitleBottomY，避免底角出圆被切。
    // 中心 y = 180 + offset；一行时 offset=120（与 FitSubtitleToCircle 一致）。
    DisplayLockGuard lock(this);
    if (bottom_bar_ != nullptr) {
        lv_obj_update_layout(bottom_bar_);
        int h = lv_obj_get_height(bottom_bar_);
        int offset_y = (kSubtitleBottomY - 180) - h / 2;
        if (offset_y < 0) offset_y = 0;
        lv_obj_align(bottom_bar_, LV_ALIGN_CENTER, 0, offset_y);
    }
}

// ---------------------------------------------------------------------------
// 无牙仔眼睛（LVGL 手绘，2026-11-23）
// ---------------------------------------------------------------------------
// 为什么不用 GIF：上游表情是 5 张 64x61 的小 GIF（delicious/silly/funny/
// sleepy/confused），要换成无牙仔眼睛就得重做 assets 分区里的 GIF 包；
// 而手绘只需要 LVGL 图元：绿椭圆（虹膜本身）+ 黑色竖瞳 + 白色高光，
// 画在全屏透明层上，眨眼/游移/表情全部用 LVGL 动画和定时器做，
// 不动上游 Display、不用改 assets、不动任何桥接逻辑。
// 尺寸验算（v2，数值验算过）：待机眼顶 y=90、眼底 y=258，椭圆离圆心最远约 149 < 180，
// 眼左右边缘 x=31..329，全在内切圆内，360 圆屏上无切角；眼顶距状态栏 6px，眼底距字幕条 22px。
// 表情映射（bridge_app.cc emotion_for_state 的四个名字）：
//   sleepy   → 待机：眼高 168，瞳孔基线 +6，慢眨眼
//   funny    → 聆听：眼高 178（睁大，v1 的 +10 绝对量；×1.5 会顶到状态栏），瞳孔基线 -9，眨眼更快、偶尔扫视
//   confused → 思考/等回复：眼高 168，眼珠随机游移
//   silly    → 说话：眼高 117（开心眯眯眼），瞳孔基线 -18
// 方向感：硬件只有一个模拟麦克风，没有麦克风阵列，做不了声源定位，
// 「看向说话方向」做不到；聆听时改成偶尔的小扫视（注意力感），如实说明。
// 注意：本文件所有 LVGL 调用都遵循上游的锁规则 ——
//   SetupUI/定时器回调在 LVGL 任务上下文（已持锁）里直接调；
//   SetEmotion/SetToothlessEyesVisible 从应用任务调，先拿 DisplayLockGuard。

namespace {

// 建一只眼：绿椭圆容器 + 黑竖瞳 + 白高光。瞳孔与高光是眼的子对象，
// 相对眼中心对齐，眨眼时被眼容器裁剪（眼高变 8 时只剩一条绿缝）。
lv_obj_t* CreateOneEye(lv_obj_t* parent, int x_ofs, lv_obj_t** pupil_out, lv_obj_t** hl_out) {
    lv_obj_t* eye = lv_obj_create(parent);
    lv_obj_set_size(eye, kEyeW, kEyeH);
    lv_obj_align(eye, LV_ALIGN_CENTER, x_ofs, kEyeCenterY - 180);
    lv_obj_set_style_bg_color(eye, lv_color_hex(kEyeGreen), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);  // 138x168 → 竖椭圆
    lv_obj_set_style_border_width(eye, 0, 0);
    lv_obj_set_style_pad_all(eye, 0, 0);
    lv_obj_set_scrollbar_mode(eye, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(eye, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(eye, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* pupil = lv_obj_create(eye);
    lv_obj_set_size(pupil, kPupilW, kPupilH);
    lv_obj_align(pupil, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(pupil, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);  // 33x117 → 竖缝
    lv_obj_set_style_border_width(pupil, 0, 0);
    lv_obj_set_style_pad_all(pupil, 0, 0);
    lv_obj_remove_flag(pupil, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(pupil, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hl = lv_obj_create(eye);
    lv_obj_set_size(hl, kHighlightSize, kHighlightSize);
    lv_obj_align(hl, LV_ALIGN_CENTER, -15, -36);
    lv_obj_set_style_bg_color(hl, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(hl, 0, 0);
    lv_obj_set_style_pad_all(hl, 0, 0);
    lv_obj_remove_flag(hl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(hl, LV_OBJ_FLAG_SCROLLABLE);

    *pupil_out = pupil;
    *hl_out = hl;
    return eye;
}

}  // namespace

void HuanzhuRoundDisplay::CreateToothlessEyes() {
    if (eyes_layer_ != nullptr) return;  // 幂等：SetupUI 只调一次，防重复
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) return;

    eyes_layer_ = lv_obj_create(screen);
    lv_obj_set_size(eyes_layer_, 360, 360);
    lv_obj_center(eyes_layer_);
    lv_obj_set_style_bg_opa(eyes_layer_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eyes_layer_, 0, 0);
    lv_obj_set_style_pad_all(eyes_layer_, 0, 0);
    lv_obj_set_style_radius(eyes_layer_, 0, 0);
    lv_obj_set_scrollbar_mode(eyes_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(eyes_layer_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(eyes_layer_, LV_OBJ_FLAG_SCROLLABLE);
    // 初始隐藏：第一帧 SetEmotion 之前仍显示上游开机 logo（robot_2），
    // 避免黑屏；SetEmotion("sleepy") 时 ShowEyesLocked(true) 亮起。
    lv_obj_add_flag(eyes_layer_, LV_OBJ_FLAG_HIDDEN);

    eye_l_ = CreateOneEye(eyes_layer_, -kEyeCenterXOfs, &pupil_l_, &hl_l_);
    eye_r_ = CreateOneEye(eyes_layer_, kEyeCenterXOfs, &pupil_r_, &hl_r_);

    // 眨眼定时器：首次 3s 后眨，之后每次回调里随机化下一次间隔。
    blink_timer_ = lv_timer_create(BlinkTimerCb, 3000, this);
    // 眼珠游移定时器：850ms 一次，只在思考/聆听表情下真的动瞳孔。
    look_timer_ = lv_timer_create(LookTimerCb, 850, this);

    ESP_LOGI(TAG, "Toothless eyes v2 created: 138x168 @ y=%d, green=0x%06x",
             kEyeCenterY, (unsigned)kEyeGreen);
}

void HuanzhuRoundDisplay::ShowEyesLocked(bool show) {
    if (eyes_layer_ == nullptr) return;
    if (show) {
        lv_obj_remove_flag(eyes_layer_, LV_OBJ_FLAG_HIDDEN);
        eyes_shown_ = true;
    } else {
        lv_obj_add_flag(eyes_layer_, LV_OBJ_FLAG_HIDDEN);
        eyes_shown_ = false;
    }
}

void HuanzhuRoundDisplay::SetEyeHeightLocked(lv_obj_t* eye, int h) {
    if (eye == nullptr) return;
    if (h < 6) h = 6;
    lv_obj_set_height(eye, h);
    lv_obj_set_y(eye, kEyeCenterY - h / 2);  // 改高时保持眼中心 y 不动
}

void HuanzhuRoundDisplay::EyeHeightAnimCb(void* var, int32_t h) {
    auto* eye = static_cast<lv_obj_t*>(var);
    if (eye == nullptr) return;
    if (h < 6) h = 6;
    lv_obj_set_height(eye, h);
    lv_obj_set_y(eye, kEyeCenterY - h / 2);
}

void HuanzhuRoundDisplay::DoBlink() {
    if (eye_l_ == nullptr || eye_r_ == nullptr) return;
    // 闭眼 90ms + 睁开 130ms（reverse 回播），总 ~220ms；起点用表情眼高 eye_h_，
    // 不用当前实测高度，避免与未播完的眨眼叠加时播回一个中间值。
    for (lv_obj_t* eye : {eye_l_, eye_r_}) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, eye);
        lv_anim_set_values(&a, eye_h_, 8);
        lv_anim_set_duration(&a, 90);
        lv_anim_set_reverse_duration(&a, 130);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, EyeHeightAnimCb);
        lv_anim_start(&a);  // 同 var+回调的旧动画会被自动删掉
    }
}

void HuanzhuRoundDisplay::BlinkTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<HuanzhuRoundDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr || !self->eyes_shown_) return;
    self->DoBlink();
    // 下次眨眼间隔：聆听时 1.8~3.3s（更机灵），其余 2.4~4.6s 随机。
    uint32_t base = (self->expr_ == ToothlessExpr::LISTEN) ? 1800 : 2400;
    uint32_t span = (self->expr_ == ToothlessExpr::LISTEN) ? 1500 : 2200;
    lv_timer_set_period(timer, base + (esp_random() % span));
}

void HuanzhuRoundDisplay::ApplyGaze() {
    if (pupil_l_ == nullptr) return;
    int dx = gaze_dx_;
    int dy = gaze_base_dy_ + gaze_dy_;
    lv_obj_align(pupil_l_, LV_ALIGN_CENTER, dx, dy);
    lv_obj_align(pupil_r_, LV_ALIGN_CENTER, dx, dy);
    lv_obj_align(hl_l_, LV_ALIGN_CENTER, dx - 15, dy - 36);
    lv_obj_align(hl_r_, LV_ALIGN_CENTER, dx - 15, dy - 36);
}

void HuanzhuRoundDisplay::LookTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<HuanzhuRoundDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr || !self->eyes_shown_) return;
    switch (self->expr_) {
        case ToothlessExpr::THINK: {
            // 思考：眼珠慢慢随机游移（随机游走，限制在 ±21x/±15y 内，按 1.5 倍放大）
            int dx = self->gaze_dx_ + (int)(esp_random() % 25) - 12;
            int dy = self->gaze_dy_ + (int)(esp_random() % 19) - 9;
            if (dx > 21) dx = 21;
            if (dx < -21) dx = -21;
            if (dy > 15) dy = 15;
            if (dy < -15) dy = -15;
            self->gaze_dx_ = dx;
            self->gaze_dy_ = dy;
            self->ApplyGaze();
            break;
        }
        case ToothlessExpr::LISTEN: {
            // 聆听：偶尔（约 1/3 概率）小扫视一下。
            // 单麦克风做不了声源定位，看向说话方向不成立；扫视只给注意力感。
            if ((esp_random() % 3) == 0) {
                self->gaze_dx_ = (int)(esp_random() % 19) - 9;
                self->gaze_dy_ = (int)(esp_random() % 11) - 5;
                self->ApplyGaze();
            }
            break;
        }
        default: {
            // 待机/开心：瞳孔回基线
            if (self->gaze_dx_ != 0 || self->gaze_dy_ != 0) {
                self->gaze_dx_ = 0;
                self->gaze_dy_ = 0;
                self->ApplyGaze();
            }
            break;
        }
    }
}

void HuanzhuRoundDisplay::ApplyExpressionLocked() {
    // 先停掉进行中的眨眼动画，避免它把高度播回旧表情的值。
    if (eye_l_ != nullptr) lv_anim_delete(eye_l_, EyeHeightAnimCb);
    if (eye_r_ != nullptr) lv_anim_delete(eye_r_, EyeHeightAnimCb);
    int h = 168;
    int base_dy = 0;
    switch (expr_) {
        case ToothlessExpr::LISTEN:
            h = 178;   // 聆听：睁大（+10，与 v1 绝对量相同；×1.5 会顶到状态栏）
            base_dy = -9;
            break;
        case ToothlessExpr::THINK:
            h = 168;   // 思考：正常，眼珠游移
            base_dy = 0;
            break;
        case ToothlessExpr::HAPPY:
            h = 117;   // 说话：开心眯眯眼
            base_dy = -18;
            break;
        case ToothlessExpr::IDLE:
        default:
            h = 168;   // 待机：放松
            base_dy = 6;
            break;
    }
    eye_h_ = h;
    gaze_base_dy_ = base_dy;
    gaze_dx_ = 0;
    gaze_dy_ = 0;
    SetEyeHeightLocked(eye_l_, h);
    SetEyeHeightLocked(eye_r_, h);
    ApplyGaze();
}

bool HuanzhuRoundDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) return false;
    ToothlessExpr expr;
    if (strcmp(emotion, "sleepy") == 0) {
        expr = ToothlessExpr::IDLE;
    } else if (strcmp(emotion, "funny") == 0) {
        expr = ToothlessExpr::LISTEN;
    } else if (strcmp(emotion, "confused") == 0) {
        expr = ToothlessExpr::THINK;
    } else if (strcmp(emotion, "silly") == 0) {
        expr = ToothlessExpr::HAPPY;
    } else {
        // 不在无牙仔映射表里的名字：藏起眼睛，回退给上游（GIF/字体表情）。
        // 先放锁再调上游：上游 SetEmotion 自己拿锁，避免嵌套拿同一把锁。
        {
            DisplayLockGuard lock(this);
            ShowEyesLocked(false);
        }
        return SpiLcdDisplay::SetEmotion(emotion);
    }

    DisplayLockGuard lock(this);
    expr_ = expr;
    // 上游的 GIF/字体表情不要和眼睛叠在一起：藏起 emoji 图层。
    // gif_controller_ 是上游私有的、这里停不掉；藏起后它继续在后台解码
    // 被隐藏的图，CPU 开销可忽略；表情切回上游名字时由上游重新接管。
    if (emoji_image_ != nullptr) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_label_ != nullptr) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    ApplyExpressionLocked();
    ShowEyesLocked(eyes_visible_);
    return true;
}

void HuanzhuRoundDisplay::SetToothlessEyesVisible(bool visible) {
    DisplayLockGuard lock(this);
    eyes_visible_ = visible;
    ShowEyesLocked(visible);
}
