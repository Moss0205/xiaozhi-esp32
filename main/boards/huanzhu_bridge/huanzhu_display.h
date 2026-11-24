// 幻珠圆屏显示子类 —— 修「对话时屏幕不显示文字」
//
// 病因（2026-11-04 实测确认）：文字**确实渲染出来了**（日志有 `>> 你好MOSS`、`>> 你好，报。`），
// 但上游 LcdDisplay::SetupUI() 把字幕条 bottom_bar_ 用 LV_ALIGN_BOTTOM_MID, 0, 0
// 贴到 360x360 **方形帧缓冲的最底边**。圆屏可见区是内切圆（圆心 180,180 半径 180）：
//   y=360 处圆内半宽 = 0（整条都在圆外）
//   y=336 处半宽 ≈ 84px → 可见宽仅 ~168px
// 而单行字幕 label 宽 = LV_HOR_RES - spacing(8) ≈ 344px 且横向滚动，
// 于是绝大部分时间字在圆外 → 观感「表情看得见、对话文字看不见」。
//
// 修法：不动上游，派生一层，SetupUI 之后把字幕条整体收进内切圆。
//
// 2026-11-05 追加：顶部状态栏（top_bar_/status_bar_）同样贴矩形帧缓冲顶边被圆切，
// 整体下移 kTopBarOffsetY（见 .cc）。
//
// 多行字幕（2026-11-09 改）：CONFIG_USE_MULTILINE_CHAT_MESSAGE 已开，
// 字幕换行一次全显示。注意上游 SetChatMessage() 在 MULTILINE 分支里每次
// 消息都会把条重新钉回底边（lcd_display.cc:1143-1149），正好抵消本文件
// 的几何校正，所以本类 override 了 SetChatMessage()：调完上游先收回圆内，
// 再按实测高度把底边收到 y=320，避免多行时底角出圆被切。
//
// 无牙仔脸（2026-11-23 v1，2026-11-24 v2/v3）：板子要塞进无牙仔毛绒玩偶里，圆屏当它的脸。
// 本类 override SetEmotion()，把桥接应用的四个表情名映射成一双 LVGL 手绘的
// 绿眼睛（竖瞳）：待机 sleepy、聆听 funny、思考 confused、说话 silly。
// v3：眼睛移到屏幕正中间（眼中心 y=180）、脸背景纯黑、再加鼻子（两只小鼻孔）
// 和嘴巴（宽而浅的微笑弧线），照着玩偶的脸画。
// v4（2026-11-24）：修了 v1–v3 眼睛一直“在下面”的根因——SetEyeHeightLocked /
// EyeHeightAnimCb 里 lv_obj_set_y(eye, kEyeCenterY - h/2) 的语义错了：眼睛是
// LV_ALIGN_CENTER 对齐的，LVGL 9 会把 style Y 当偏移量加到居中位置上，实际
// 眼中心落到了 y=276 而不是 180。v4 删掉这两行 set_y，只改高度，中心自动
// 保持在 180，眼睛才真正居中。旋转没动：面板 MADCTL 180° 与 LVGL rotation
// 180° 相互抵消，90° 偏转只是面板物理安装方向，时钟转到上方能读时就是正面。
// v4 脸显示时状态栏（时钟+图标）与底部字幕条都不画（先不做，隐藏起来），只留
// 黑底+眼睛+鼻子+嘴巴；切到信息屏时文字照常显示。
// 眼睛/鼻子/嘴巴用 LVGL 图元画，不用 GIF 素材，所以不用改 assets 分区；
// 眨眼和眼珠转动由两个 lv_timer 驱动。脸背景与文字颜色跟眼睛同进退：
// 脸显示时背景纯黑、状态栏/字幕文字改白色；切到信息屏藏起眼睛时恢复主题配色。
// v4：脸显示时状态栏与字幕条直接隐藏（先不做），信息屏上再显示。
// 其他表情名（以及任何不在映射表里的名字）回退给上游 SetEmotion()
// （GIF/字体表情），行为与原来一致。
// 信息屏（用量/天气）上眼睛会挡住文字：bridge_app 切屏时通过
// SetToothlessEyesVisible(false) 藏起眼睛，切回聊天屏时再显示。
#ifndef _HUANZHU_DISPLAY_H_
#define _HUANZHU_DISPLAY_H_

#include "display/lcd_display.h"

class HuanzhuRoundDisplay : public SpiLcdDisplay {
public:
    HuanzhuRoundDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                        int width, int height, int offset_x, int offset_y,
                        bool mirror_x, bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                        mirror_x, mirror_y, swap_xy) {}

    void SetupUI() override;
    void SetChatMessage(const char* role, const char* content) override;

    // 无牙仔眼睛：拦截桥接应用的表情名，画手绘绿眼睛；未知名字回退上游。
    bool SetEmotion(const char* emotion) override;

    // 信息屏切屏时藏起/显示眼睛（bridge_app 经 bridge_hal 调用）。
    // 注意：藏起只是不显示；SetEmotion 仍会更新表情状态，切回聊天屏即现形。
    void SetToothlessEyesVisible(bool visible);

private:
    void FitSubtitleToCircle();
    void FitTopBarToCircle();

    // ---- 无牙仔眼睛（LVGL 手绘） ----
    // 表情：只映射桥接应用实际用的四个名字（见 bridge_app.cc emotion_for_state）。
    enum class ToothlessExpr : uint8_t {
        IDLE = 0,  // sleepy：待机，放松，慢眨眼
        LISTEN,    // funny：聆听，睁大，偶尔小扫视
        THINK,     // confused：思考/等回复，眼珠慢慢游移
        HAPPY,     // silly：说话，开心眯眯眼
    };

    void CreateToothlessFace();      // SetupUI 后建脸层（幂等）：黑背景 + 眼睛 + 鼻子 + 嘴巴
    void ShowEyesLocked(bool show);  // 显示/隐藏脸层（调用者持锁）
    void SetFaceTextColorLocked(lv_color_t color);  // 脸显示时文字改白、藏起时恢复主题色（调用者持锁）
    void SetFaceTextHiddenLocked(bool hidden);  // v4：脸显示时藏起状态栏与字幕条、信息屏上再显示（调用者持锁）
    void ApplyExpressionLocked();    // 按当前表情设眼高/瞳孔基线（调用者持锁）
    void SetEyeHeightLocked(lv_obj_t* eye, int h);  // 设单只眼高度；CENTER 对齐下改高度时中心自动不动（v4：不要调 set_y，见 .cc）
    void ApplyGaze();                // 按 gaze_dx_/gaze_dy_ 重摆瞳孔与高光（LVGL 任务内调用，不拿锁）
    void DoBlink();                  // 触发一次眨眼动画（LVGL 任务内调用，不拿锁）

    static void BlinkTimerCb(lv_timer_t* timer);
    static void LookTimerCb(lv_timer_t* timer);
    static void EyeHeightAnimCb(void* var, int32_t h);

    lv_obj_t* face_bg_ = nullptr;    // 脸背景：360x360 纯黑，压在 container_ 之上、其余控件之下
    lv_obj_t* eyes_layer_ = nullptr;  // 全屏透明层，装两只眼 + 鼻子 + 嘴巴
    lv_obj_t* eye_l_ = nullptr;       // 左眼容器（绿椭圆本身）
    lv_obj_t* eye_r_ = nullptr;       // 右眼容器
    lv_obj_t* pupil_l_ = nullptr;     // 左竖瞳（黑椭圆）
    lv_obj_t* pupil_r_ = nullptr;     // 右竖瞳
    lv_obj_t* hl_l_ = nullptr;        // 左高光（白点）
    lv_obj_t* hl_r_ = nullptr;        // 右高光
    lv_obj_t* nose_l_ = nullptr;      // 左鼻孔（小灰椭圆）
    lv_obj_t* nose_r_ = nullptr;      // 右鼻孔
    lv_obj_t* mouth_ = nullptr;       // 嘴巴（宽而浅的微笑弧线，lv_line）
    lv_color_t theme_text_color_ = lv_color_black();  // 主题文字色（SetupUI 时从屏幕样式取）
    lv_timer_t* blink_timer_ = nullptr;  // 眨眼定时器
    lv_timer_t* look_timer_ = nullptr;   // 眼珠游移定时器
    volatile ToothlessExpr expr_ = ToothlessExpr::IDLE;
    volatile bool eyes_visible_ = true;  // 用户层可见（信息屏上为 false）
    volatile bool eyes_shown_ = false;   // 眼睛层当前是否未隐藏
    int eye_h_ = 168;                    // 当前眼高（表情决定，眨眼动画会临时改；v3 基准 168）
    int gaze_dx_ = 0;                    // 瞳孔游移 x（相对眼中心）
    int gaze_dy_ = 0;                    // 瞳孔游移 y（相对眼中心）
    int gaze_base_dy_ = 0;               // 瞳孔基线 y（表情决定）
};

#endif  // _HUANZHU_DISPLAY_H_
