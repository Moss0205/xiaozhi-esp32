// 幻珠圆屏显示子类实现 —— 把字幕条收进 360x360 内切圆，同时校正顶部状态栏位置
//
// 刻意不调用 LvglTheme（板级代码的 include 路径不保证有 lvgl_theme.h），
// 只改几何：尺寸/位置/内边距，颜色与字体沿用上游 SetupUI 的结果。
#include "huanzhu_display.h"

#include <esp_log.h>
#include <esp_random.h>

#include <cmath>
#include <cstring>

#define TAG "huanzhu_display"

// 内切圆几何：圆心 (180,180)，半径 180。
// 顶部状态栏：原贴顶边（y=0 处），圆内半宽≈0 → 图标与文字被切角。
// LV_ALIGN_TOP_MID 的 offset_y 正值 = 向下移。移到 y=60：
//   条高 ~24px，底边 y≈84 → 半宽 sqrt(180²-96²)≈152 → 可见 ~304px
// 图标条收窄到 300px（x=30..330）后左右图标都在圆内。
static constexpr int kTopBarOffsetY = 60;     // 向下移 60px（TOP_MID 正方向是下）
static constexpr int kTopBarWidth = 300;      // 图标条宽度（原 360 两端出圆）

// 底部字幕条（v1/v2 旧几何）：条中心放圆心下方 120px（y=300）：
//   圆内半宽 = sqrt(180^2 - 120^2) ≈ 134 → 可见 ~268px
//   条底边 y≈312（半宽 ≈122 → 可见 ~244px）
// 所以条宽取 240px 时整条（含底角）都在圆内。
// 表情图 64x61 居中（y≈149..210），与字幕条不重叠。
// v3：字幕条下移、收窄，为嘴巴腾地方（见下方无牙仔脸几何）：
//   条宽 220、底边 y=320：最差点 (76,318) 离圆心 ≈172.8 < 180，整条在圆内；
//   单行条顶边 y≈296，嘴巴底边 y=292，留 4px；多行时条往上长，
//   脸层在字幕条之上，鼻子嘴巴仍盖在条上面，不会被字幕遮住。
static constexpr int kBarWidth = 220;
static constexpr int kBarOffsetY = 120;
static constexpr int kBarPad = 4;
// 多行字幕底边安全线：条宽 220px 时，底边 y=320 处圆内半宽 ≈113 → 可见 ~226px，
// 整条（含底角）都在圆内。行数越多条越往上收，底边始终钉在 320。
static constexpr int kSubtitleBottomY = 320;

// ---- 无牙仔脸几何（圆心 180,180，半径 180），v3：眼睛居中、黑背景、鼻子嘴巴 ----
// 眼睛：两只眼中心 x=180±80，y=180（屏幕正中心）；眼 138x168，顶 y=96、底 y=264，
// 椭圆上离圆心最远点约 149 < 180，全在内切圆内，左右边缘 x=31..329。
// 顶部状态栏在 y=60..84（待机眼顶 y=96，留 12px；聆听眼高 178 时顶 y=91，留 7px），
// 字幕条中心 y=308（待机眼底 y=264，留 32px），都不与眼睛重叠。
// 鼻子：两只小鼻孔 12x16，中心 (171,274)/(189,274)，眼底之下、嘴巴之上，离圆心最远角约 103。
// 嘴巴：宽而浅的微笑弧线，端点 (95,284)/(265,284)、最低点 (180,292)，
//   拟合圆半径 455.6、圆心 (180,-163.6)，18 点折线；字幕条单行顶边 y≈296，留 4px。
// 脸背景：360x360 纯黑矩形，压在 container_ 之上、其余控件之下，只在脸显示时出现；
//   脸显示时状态栏/字幕文字改白色（亮色主题黑字在黑底上看不见），藏起时恢复主题色。
static constexpr int kEyeW = 138;         // 单只眼宽（v1 92 × 1.5）
static constexpr int kEyeH = 168;         // 单只眼高（v1 112 × 1.5；表情决定：聆听 178，开心 117）
static constexpr int kEyeCenterY = 180;   // 眼中心 y（屏幕正中心；v2 174）
static constexpr int kEyeCenterXOfs = 80; // 眼中心 x = 180 ± 80（眼变宽，间距拉开，内隙 22px）
static constexpr int kPupilW = 33;        // 竖瞳宽（v1 22 × 1.5）
static constexpr int kPupilH = 117;       // 竖瞳高（v1 78 × 1.5）
static constexpr int kHighlightSize = 18; // 高光白点直径（v1 12 × 1.5）
static constexpr uint32_t kEyeGreen = 0x7ED321;  // 无牙仔眼睛的亮绿色
static constexpr int kNoseW = 12;         // 单只鼻孔宽
static constexpr int kNoseH = 16;         // 单只鼻孔高
static constexpr int kNoseCenterY = 274;  // 鼻孔中心 y（眼底 264 之下、嘴巴之上）
static constexpr int kNoseCenterXOfs = 9; // 鼻孔中心 x = 180 ± 9
static constexpr uint32_t kFaceGray = 0x808080;  // 鼻子/嘴巴的灰色（黑底上看得见的深灰）
static constexpr int kMouthPointCount = 18;      // 嘴巴弧线点数（x=95..265，步长 10）
static constexpr double kMouthCircleR = 455.6;   // 嘴巴拟合圆半径
static constexpr double kMouthCircleCy = -163.6; // 嘴巴拟合圆圆心 y
static constexpr int kMouthLineWidth = 3;        // 嘴巴线宽

void HuanzhuRoundDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();

    // 主题文字色（亮色主题黑字 / 深色主题白字）：脸显示时文字改白，藏起时恢复。
    lv_obj_t* screen = lv_screen_active();
    if (screen != nullptr) {
        theme_text_color_ = lv_obj_get_style_text_color(screen, LV_PART_MAIN);
    }

    FitSubtitleToCircle();
    FitTopBarToCircle();
    CreateToothlessFace();
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
    FitSubtitleToCircle();  // 宽 220 + 居中（内部拿锁）
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
// 无牙仔脸（LVGL 手绘，2026-11-23 v1，2026-11-24 v2/v3）
// ---------------------------------------------------------------------------
// 为什么不用 GIF：上游表情是 5 张 64x61 的小 GIF（delicious/silly/funny/
// sleepy/confused），要换成无牙仔眼睛就得重做 assets 分区里的 GIF 包；
// 而手绘只需要 LVGL 图元：绿椭圆（虹膜本身）+ 黑色竖瞳 + 白色高光，
// 画在全屏透明层上，眨眼/游移/表情全部用 LVGL 动画和定时器做，
// 不动上游 Display、不用改 assets、不动任何桥接逻辑。
// 尺寸验算（v3，数值验算过）：眼睛居中 y=180，待机眼顶 y=96、眼底 y=264，
// 椭圆离圆心最远约 149 < 180，眼左右边缘 x=31..329，全在内切圆内，360 圆屏上无切角；
// 眼顶距状态栏 12px（聆听 7px），眼底距字幕条 32px。
// 鼻子两只鼻孔 12x16，中心 (171,274)/(189,274)，离圆心最远角约 103；
// 嘴巴宽 170 的浅弧线，端点 y=284、最低点 y=292，字幕条单行顶边 y≈296，留 4px。
// v3 脸背景：360x360 纯黑矩形，压在 container_ 之上、其余控件之下，
//   只在脸显示时出现（信息屏上藏起，恢复主题底色）；脸显示时状态栏/
//   字幕文字改白色（亮色主题黑字在黑底上看不见），藏起时恢复主题色。
//   这样整张脸跟玩偶的黑毛融为一体，信息屏配色一点不动。
// 表情映射（bridge_app.cc emotion_for_state 的四个名字）：
//   sleepy   → 待机：眼高 168，瞳孔基线 +6，慢眨眼
//   funny    → 聆听：眼高 178（睁大，v1 的 +10 绝对量；×1.5 会顶到状态栏），瞳孔基线 -9，眨眼更快、偶尔扫视
//   confused → 思考/等回复：眼高 168，眼珠随机游移
//   silly    → 说话：眼高 117（开心眯眯眼），瞳孔基线 -18
// 鼻子和嘴巴在各表情下保持不动（只眼睛动），简单可靠。
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

// 建一只鼻孔：小灰椭圆。鼻孔是眼睛层的子对象，跟眼睛一起显示/藏起。
lv_obj_t* CreateOneNostril(lv_obj_t* parent, int x_ofs) {
    lv_obj_t* nose = lv_obj_create(parent);
    lv_obj_set_size(nose, kNoseW, kNoseH);
    lv_obj_align(nose, LV_ALIGN_CENTER, x_ofs, kNoseCenterY - 180);
    lv_obj_set_style_bg_color(nose, lv_color_hex(kFaceGray), 0);
    lv_obj_set_style_bg_opa(nose, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose, LV_RADIUS_CIRCLE, 0);  // 12x16 → 小竖椭圆
    lv_obj_set_style_border_width(nose, 0, 0);
    lv_obj_set_style_pad_all(nose, 0, 0);
    lv_obj_remove_flag(nose, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(nose, LV_OBJ_FLAG_SCROLLABLE);
    return nose;
}

// 建嘴巴：宽而浅的微笑弧线，18 点折线拟合圆心 (180,-163.6)、半径 455.6 的圆，
// 端点 (95,284)/(265,284)、最低点 (180,292)。点坐标相对线对象左上角 (93,284)。
lv_obj_t* CreateMouth(lv_obj_t* parent) {
    lv_obj_t* mouth = lv_line_create(parent);
    lv_obj_set_size(mouth, 174, 12);
    lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 110);  // 对象中心 (180,290)，左上 (93,284)
    lv_point_precise_t pts[kMouthPointCount];
    for (int i = 0; i < kMouthPointCount; i++) {
        double xa = 95.0 + i * 10.0;
        double ya = kMouthCircleCy + std::sqrt(kMouthCircleR * kMouthCircleR - (xa - 180.0) * (xa - 180.0));
        pts[i].x = (lv_value_precise_t)llround(xa - 93.0);
        pts[i].y = (lv_value_precise_t)llround(ya - 284.0);
    }
    lv_line_set_points(mouth, pts, kMouthPointCount);
    lv_obj_set_style_line_color(mouth, lv_color_hex(kFaceGray), 0);
    lv_obj_set_style_line_width(mouth, kMouthLineWidth, 0);
    lv_obj_set_style_line_rounded(mouth, true, 0);
    lv_obj_set_style_bg_opa(mouth, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mouth, 0, 0);
    lv_obj_set_style_pad_all(mouth, 0, 0);
    lv_obj_remove_flag(mouth, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(mouth, LV_OBJ_FLAG_SCROLLABLE);
    return mouth;
}

// 递归改一棵子树里所有 label 的文字色（状态栏/字幕的 label 是上游 SetupUI 的
// 局部变量，子类拿不到指针，只能从 top_bar_/status_bar_/bottom_bar_ 往下找）。
void SetLabelsTextColorRecursive(lv_obj_t* obj, lv_color_t color) {
    if (obj == nullptr) return;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_obj_set_style_text_color(obj, color, 0);
    }
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        SetLabelsTextColorRecursive(lv_obj_get_child(obj, i), color);
    }
}

}  // namespace

void HuanzhuRoundDisplay::CreateToothlessFace() {
    if (eyes_layer_ != nullptr) return;  // 幂等：SetupUI 只调一次，防重复
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) return;

    // 脸背景：360x360 纯黑，压到 container_ 之上（index 1）、其余控件之下，
    // 只在脸显示时出现；信息屏上藏起，恢复主题底色。
    face_bg_ = lv_obj_create(screen);
    lv_obj_set_size(face_bg_, 360, 360);
    lv_obj_center(face_bg_);
    lv_obj_set_style_bg_color(face_bg_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(face_bg_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(face_bg_, 0, 0);
    lv_obj_set_style_pad_all(face_bg_, 0, 0);
    lv_obj_set_style_radius(face_bg_, 0, 0);
    lv_obj_set_scrollbar_mode(face_bg_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(face_bg_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(face_bg_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_to_index(face_bg_, 1);
    lv_obj_add_flag(face_bg_, LV_OBJ_FLAG_HIDDEN);

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
    nose_l_ = CreateOneNostril(eyes_layer_, -kNoseCenterXOfs);
    nose_r_ = CreateOneNostril(eyes_layer_, kNoseCenterXOfs);
    mouth_ = CreateMouth(eyes_layer_);

    // 眨眼定时器：首次 3s 后眨，之后每次回调里随机化下一次间隔。
    blink_timer_ = lv_timer_create(BlinkTimerCb, 3000, this);
    // 眼珠游移定时器：850ms 一次，只在思考/聆听表情下真的动瞳孔。
    look_timer_ = lv_timer_create(LookTimerCb, 850, this);

    ESP_LOGI(TAG, "Toothless face v3 created: eyes 138x168 @ y=%d, black bg, nose+mouth",
             kEyeCenterY);
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
    // 脸背景与文字颜色跟眼睛同进退：脸显示时背景纯黑、状态栏/字幕文字改白色；
    // 信息屏上藏起眼睛时恢复主题底色与文字色，信息屏配色不动。
    if (face_bg_ != nullptr) {
        if (show) {
            lv_obj_remove_flag(face_bg_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(face_bg_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    SetFaceTextColorLocked(show ? lv_color_white() : theme_text_color_);
}

void HuanzhuRoundDisplay::SetFaceTextColorLocked(lv_color_t color) {
    SetLabelsTextColorRecursive(top_bar_, color);
    SetLabelsTextColorRecursive(status_bar_, color);
    SetLabelsTextColorRecursive(bottom_bar_, color);
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
