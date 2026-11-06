// 桥接应用状态机 —— 不依赖小智 Application / Protocol，只依赖 bridge_hal + bridge_client。
// 状态：IDLE →（上拨短按）→ RECORDING →（下拨短按 / 30 秒到）→ UPLOADING
//       → WAITING（每秒轮询 /esp/reply）→ PLAYING（放合成音频 + 显示文字）→ IDLE
// 打断：PLAYING 中上拨短按 = 停播报并直接进 RECORDING；WAITING 中下拨短按 = 取消等待回 IDLE。
// 音量：上拨长按 +10、下拨长按 -10（软件增益，不动 codec）。
#ifndef _BRIDGE_APP_H_
#define _BRIDGE_APP_H_

#ifdef __cplusplus
extern "C" {
#endif

// 启动按键轮询任务 + 主状态机任务。调用前 bridge_hal_init() 必须已完成，
// 且 WiFi 已连上（否则 UPLOADING / WAITING 会一直报 HTTP 失败并回 IDLE）。
void bridge_app_start(void);

// 供按键任务 / 外部调用的状态查询（调试日志用）。
int bridge_app_volume(void);

#ifdef __cplusplus
}
#endif

#endif  // _BRIDGE_APP_H_
