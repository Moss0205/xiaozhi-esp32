// 桥接硬件适配层 —— 把「板子能做的事」收成 9 个函数，应用层（bridge_app）
// 只调这 9 个，不直接碰 codec / display / WiFi 的上游 API。
// 这样 dsh 集成时只需在 huanzhu_hal.cc 里把这 9 个函数接到 fork 已有的类上，
// 应用层代码一行不用改；上游 API 若有出入，也只改 hal 文件。
#ifndef _BRIDGE_HAL_H_
#define _BRIDGE_HAL_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 在圆屏上显示一行文字（走 fork 的圆屏字幕修正，字会落在内切圆里）。
void bridge_hal_show_text(const char* text);

// 设置顶部状态栏文字（上游 LvglDisplay::SetStatus，更新 status_label_）。
// 桥接版用它显示空闲时钟（"14:35 11月17日 周二"）和对话阶段（"聆听中"等），
// 替代 SetupUI 后一直卡住的“正在初始化...”。与 bridge_hal_show_text 不同：
// show_text 写的是底部字幕条（对话文字），set_status 写的是顶部状态条。
void bridge_hal_set_status(const char* status);

// 按名字切表情（assets 分区 index.json 里的名字，如 "neutral"）。
// 表情图由上游 LcdDisplay::SetEmotion 渲染；分区里没有对应表情时回退字体表情。
void bridge_hal_set_emotion(const char* emotion);

// 背光开关（PwmBacklight，GPIO8）：true=亮（100），false=灭。自动睡眠用。
void bridge_hal_set_backlight(bool on);

// 无牙仔眼睛开关（2026-11-23）：true=显示眼睛，false=藏起眼睛。
// 信息屏（用量/天气）上眼睛会挡住文字，切屏时由 bridge_app 调它藏起；
// 切回聊天屏时再调 true 显示。内部调 HuanzhuRoundDisplay::
// SetToothlessEyesVisible；若绑定的 display 不是圆屏子类，则什么也不做。
void bridge_hal_set_eyes_visible(bool visible);

// 从麦克风读 samples 个 16k 样本进 buf，返回实际读到的样本数。
// fork 的 HuanzhuAudioCodec::Read(int16_t*, int) 就是这个语义。
int bridge_hal_mic_read(int16_t* buf, int samples);

// 把 samples 个 24k 样本送喇叭。实现见 huanzhu_hal.cc（ADAPT 点）。
void bridge_hal_spk_write(const int16_t* buf, int samples);

// 进入 WiFi 配网模式（原厂「中间短按进配网」的入口）。
// Xiaozhi 集成版：接到 board 的 EnterWifiConfigMode()；
// 未接线前：hal 里的默认实现只打印日志，不会真进配网 —— dsh 接线后删掉该日志。
void bridge_hal_enter_wifi_config(void);

// 重启（中间长按的逃生口），esp_restart()。
void bridge_hal_restart(void);

// 供 hal 内部用的初始化：显示、codec、背光等由 board 初始化完成后调用一次。
// 集成时在 HuanzhuBoard 构造函数末尾（或 app_main 开头）调一次即可。
void bridge_hal_init(void);

// 集成接线（dsh 在 board 侧调一次）：
//   bridge_hal_bind(board->GetDisplay(), board->GetAudioCodec());
//   bridge_hal_set_wifi_config_handler([]{ board->EnterWifiConfigMode(); });
void bridge_hal_bind(void* display, void* codec);
void bridge_hal_set_wifi_config_handler(void (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif  // _BRIDGE_HAL_H_
