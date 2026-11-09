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
// 再按实测高度把底边收到 y=312，避免多行时底角出圆被切。
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

private:
    void FitSubtitleToCircle();
    void FitTopBarToCircle();
};

#endif  // _HUANZHU_DISPLAY_H_
