// 幻珠 HuanZhu_360_360 V01 —— 小智定制板 config.h（配套 huanzhu_board.cc）
// 依据：厂商原理图（HuanZhu_360_360 V01 / 嘉立创EDA）+ 点亮示例 main.cpp + 原厂固件日志
//      拨轮三个脚的实测见 esp32-flash\STATUS.md「2026-11-04 键位实测」。
//
// ⚠️ 2026-11-04 修正：旧版把 KEY1/2/3 写成 GPIO4/3/2，与原理图（5/4/3）整体错位一位，
//    且 BOOT_BUTTON_GPIO=GPIO0 在本机根本没有按键（改装壳没引出 BOOT），
//    所以原固件里 boot_button_ 挂在一个空脚上 —— 按拨轮任何一档都不会有反应。
//    实测：上拨=GPIO3 稳定拉低；按中间=GPIO4 稳定拉低；下拨=GPIO5 恒为 0（硬件未接/对地短路）。
#ifndef _HUANZHU_BOARD_CONFIG_H_
#define _HUANZHU_BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <hal/adc_types.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// ---- 屏幕：ST77916，360x360 圆屏，4 线 SPI ----
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_PIN_SCLK    GPIO_NUM_12
#define LCD_PIN_MOSI    GPIO_NUM_13
#define LCD_PIN_CS      GPIO_NUM_10
#define LCD_PIN_DC      GPIO_NUM_15
#define LCD_PIN_RST     GPIO_NUM_14
#define LCD_PIN_BL      GPIO_NUM_8
#define LCD_WIDTH       360
#define LCD_HEIGHT      360

// ---- 喇叭：MAX98357A，I2S ----
#define AUDIO_I2S_SPK_GPIO_DOUT  GPIO_NUM_17  // I2S_SD -> 功放 DIN
#define AUDIO_I2S_SPK_GPIO_BCLK  GPIO_NUM_18  // I2S_BCLK
#define AUDIO_I2S_SPK_GPIO_LRCK  GPIO_NUM_21  // I2S_WS
// SD_MODE 现在**只**由 huanzhu_audio_codec 认领（旧版 board 与 codec 各 gpio_config 一次，
// 开机日志报 W gpio: conflict found for GPIO[16]）。
#define AUDIO_SPK_SD_GPIO        GPIO_NUM_16

// ---- 麦克风：ZTS6216 模拟麦 -> ADC1_CH5 / GPIO6（原理图确认） ----
#define AUDIO_MIC_ADC_UNIT       ADC_UNIT_1
#define AUDIO_MIC_ADC_CHANNEL    ADC_CHANNEL_5
#define AUDIO_MIC_ADC_GPIO       GPIO_NUM_6
// 12bit ADC 语音摆幅较小，先给 24 倍；若底噪过大调小、声音太小调大
#define AUDIO_MIC_GAIN           24

// ---- 拨轮（原理图 KEY1/2/3，均带 10k 上拉 + 100nF，动作时拉低 = 低有效） ----
#define KEY1_GPIO        GPIO_NUM_5   // 下拨：实测无信号（硬件断/短路），保留定义但不绑定
#define KEY2_GPIO        GPIO_NUM_4   // 按中间：可靠
#define KEY3_GPIO        GPIO_NUM_3   // 上拨：可靠

// board.cc 用的语义别名（与实测一致）
#define KNOB_UP_GPIO     KEY3_GPIO    // 上拨 → GPIO3 ✅
#define KNOB_PRESS_GPIO  KEY2_GPIO    // 按中间 → GPIO4 ✅
#define KNOB_DOWN_GPIO   KEY1_GPIO    // 下拨 → GPIO5 ❌ 不绑定

// 本机没有 GPIO0 BOOT 键，把「主按键」语义交给真实存在的上拨（GPIO3）。
// 开机时拨上拨 = 进配网；其余时候 = 开/关对话。
#define BOOT_BUTTON_GPIO KNOB_UP_GPIO
#define BUILTIN_LED_GPIO GPIO_NUM_1   // 运行指示灯 LED2

#endif  // _HUANZHU_BOARD_CONFIG_H_
