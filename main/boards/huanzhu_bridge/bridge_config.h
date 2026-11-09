// 桥接版板端配置 —— 幻珠 HuanZhu_360_360 V01
// ⚠️ 未编译草稿（2026-11-05）：由 MOSS 按 dsh fork + 原理图写出，dsh 用 ESP-IDF v6.1
//    编译验证后才能刷机。原厂固件备份仍在，刷坏可回滚。
//
// 本文件只放「一处可改的配置表」：引脚、按键映射、桥接服务地址、行为阈值。
// fork 里已经验证过的硬件事实（屏 188 条初始化、ADC 麦、圆屏字幕修正）不在这里
// 重写 —— 集成时把 fork 的 huanzhu_board.cc / huanzhu_display.{h,cc} /
// huanzhu_audio_codec.{h,cc} 原样保留，只换掉应用层（见 README_BRIDGE_BOARD.md）。
#ifndef _BRIDGE_CONFIG_H_
#define _BRIDGE_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <hal/adc_types.h>

// 构建参数注入点：CI 在 main/boards/huanzhu_bridge/ 下生成 bridge_token_override.h
// （内容形如 #define BRIDGE_TOKEN "xxx"），文件不进 git，本地编译用默认值。
#if __has_include("bridge_token_override.h")
#include "bridge_token_override.h"
#endif

// ---------------- 屏 / 音频引脚（原理图确认，与 fork config.h 一致） ----------------
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_PIN_SCLK    GPIO_NUM_12
#define LCD_PIN_MOSI    GPIO_NUM_13
#define LCD_PIN_CS      GPIO_NUM_10
#define LCD_PIN_DC      GPIO_NUM_15
#define LCD_PIN_RST     GPIO_NUM_14
#define LCD_PIN_BL      GPIO_NUM_8
#define LCD_WIDTH       360
#define LCD_HEIGHT      360
#define AUDIO_INPUT_SAMPLE_RATE  16000   // 麦：ADC 采样，桥接听写要求正好是 16k
#define AUDIO_OUTPUT_SAMPLE_RATE 24000   // 喇叭：桥接合成音频就是 24k，无需转采样
#define AUDIO_I2S_SPK_GPIO_DOUT  GPIO_NUM_17
#define AUDIO_I2S_SPK_GPIO_BCLK  GPIO_NUM_18
#define AUDIO_I2S_SPK_GPIO_LRCK  GPIO_NUM_21
#define AUDIO_SPK_SD_GPIO        GPIO_NUM_16  // 只由 codec 认领，board 不要再配一次
#define AUDIO_MIC_ADC_UNIT         ADC_UNIT_1
#define AUDIO_MIC_ADC_CHANNEL      ADC_CHANNEL_5
#define AUDIO_MIC_ADC_GPIO       GPIO_NUM_6   // ADC1_CH5，ZTS6216 模拟麦
#define AUDIO_MIC_GAIN           24

// ---------------- 按键：GPIO 号是原理图证明的，方向映射是待复测的假设 ----------------
// 原理图（QS-301-AGS5P）：KEY3→GPIO3、KEY2→GPIO4、KEY1→GPIO5，三路都低有效、
// 静止时 3.3V。原理图证明不了「物理方向对应哪一路」，下面这张映射表是假设，
// 以 2026-11-04 dsh 的实测说法为初值（上=GPIO3、按=GPIO4、下=GPIO5），
// 但 dsh 的下拨结论已被原厂行为证伪，所以复测前不要把它当真理。
// 复测方法见 KEY_RETEST.md；若复测发现上下拨反了，只换下面两行，别动语义代码。
#define KEY_UP_GPIO     GPIO_NUM_3   // 假设：上拨
#define KEY_PRESS_GPIO  GPIO_NUM_4   // 按中间（这一路 dsh 实测可靠，争议最小）
#define KEY_DOWN_GPIO   GPIO_NUM_5   // 假设：下拨（dsh 曾称恒低，原厂行为证明三路都好用）

// 最终按键语义（chen 2026-11-04 晚定稿，BRIDGE_PLAN.md 旧的「中间开关对话」段落已作废）：
//   上拨短按  = 开始对话（开始录音）
//   下拨短按  = 结束对话（停录音并上传；播放中按 = 打断播报并开始录音）
//   上拨长按  = 音量 +10
//   下拨长按  = 音量 -10
//   中间短按  = 进入 WiFi 配网（必须保留，否则丢了不拆机重配网的入口）
//   中间长按  = 重启（逃生口）
#define KEY_LONG_PRESS_MS   800   // 按住 >=800ms 算长按，否则算短按
#define KEY_DEBOUNCE_MS     20    // 消抖
#define KEY_POLL_MS         10    // 按键轮询间隔

// 复测模式：置 1 时，任何拨轮动作只打印三路 GPIO 原始电平，不触发任何语义，
// 专门用来做 KEY_RETEST.md 的方向复测。复测完改回 0 才能正常用。
#ifndef BRIDGE_KEY_LOG_ONLY
#define BRIDGE_KEY_LOG_ONLY 0   // 复测版：构建时 -DBRIDGE_KEY_LOG_ONLY=1 覆盖（见 KEY_RETEST.md）
#endif

// ---------------- 桥接服务（单位 Mac mini 上的 bridge_v2.py） ----------------
// HOST：Mac mini 在单位局域网里的 IP（2026-11-07 测得 192.168.1.198）。
// 板子在单位和 Mac 同一个局域网，用这个 IP 直连。Tailscale 设备名 mac-mini
// 板端 DNS 解析不到，不要再改回去。注意这是 DHCP 地址，变了要同步改这里，
// 长期办法是给 Mac 做 DHCP 保留。
#ifndef BRIDGE_HOST
#define BRIDGE_HOST "192.168.1.198"
#endif
#ifndef BRIDGE_PORT
#define BRIDGE_PORT 8765
#endif
// TOKEN：编译时从构建参数传入，不要把真实 token 提交到公开仓库。
// 例如：idf.py build -DBRIDGE_TOKEN='"你的token"'，或在 sdkconfig 里定义。
#ifndef BRIDGE_TOKEN
#define BRIDGE_TOKEN "change-me"
#endif

// ---------------- 行为阈值 ----------------
#define BRIDGE_RECORD_MAX_S    30    // 单次录音上限 30 秒（听写上限 60 秒，留余量）
#define BRIDGE_POLL_INTERVAL_MS 1000 // 等回复时每 1 秒轮询一次 /esp/reply
#define BRIDGE_HTTP_TIMEOUT_MS  15000 // 单次 HTTP 超时
#define BRIDGE_VOLUME_DEFAULT  80    // 播放音量初值 0-100（软件增益，不动 codec）
#define BRIDGE_VOLUME_STEP     10

// ---------------- 表情 + 自动睡眠 + 字幕停留（2026-11-09） ----------------
// 表情：bridge_app 在状态变化时调 bridge_hal_set_emotion()，名字必须与
// assets 分区 index.json 的 emoji_collection 一致（精简包 5 个大脸：
// delicious、silly、funny、sleepy、confused；待机 sleepy、录音 funny、
// 等回复 confused、播放 silly）。
// 自动睡眠：IDLE 下无按键超过下面分钟数就关背光（GPIO8），任意按键唤醒；
// 只关屏不断 WiFi（真深睡会断桥），0 = 不启用。
#define BRIDGE_IDLE_SLEEP_MINUTES 5
// 字幕停留：回复播完后，字幕保留下面分钟数再消失（回"你好MOSS"）；
// 新对话开始会直接替换它。0 = 不停留（播完立刻回"你好MOSS"，旧行为）。
#define BRIDGE_SUBTITLE_HOLD_MINUTES 3

#endif  // _BRIDGE_CONFIG_H_
