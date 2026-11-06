// 桥接硬件适配层 —— 把「板子能做的事」收成 6 个函数，应用层（bridge_app）
// 只调这 6 个，不直接碰 codec / display / WiFi 的上游 API。
// 这样 dsh 集成时只需在 huanzhu_hal.cc 里把这 6 个函数接到 fork 已有的类上，
// 应用层代码一行不用改；上游 API 若有出入，也只改 hal 文件。
#ifndef _BRIDGE_HAL_H_
#define _BRIDGE_HAL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 在圆屏上显示一行文字（走 fork 的圆屏字幕修正，字会落在内切圆里）。
void bridge_hal_show_text(const char* text);

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
