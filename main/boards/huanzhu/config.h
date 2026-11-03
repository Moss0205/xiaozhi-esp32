// 幻珠 HuanZhu_360_360 V01 —— 小智定制板 config.h（配套 huanzhu_board.cc）
// 依据：厂商清晰原理图（HuanZhu_360_360 V01 / 嘉立创EDA）+ 点亮示例 main.cpp + 原厂固件日志
#ifndef _HUANZHU_BOARD_CONFIG_H_
#define _HUANZHU_BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <hal/adc_types.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// ---- 屏幕：ST77916，360x360，4 线 SPI ----
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
#define AUDIO_SPK_SD_GPIO        GPIO_NUM_16  // SD_MODE，功放使能，拉高

// ---- 麦克风：ZTS6216 模拟麦 -> ADC1_CH5 / GPIO6（原理图确认） ----
#define AUDIO_MIC_ADC_UNIT       ADC_UNIT_1
#define AUDIO_MIC_ADC_CHANNEL    ADC_CHANNEL_5
#define AUDIO_MIC_ADC_GPIO       GPIO_NUM_6
// 12bit ADC 语音摆幅较小，先给 24 倍；若底噪过大调小、声音太小调大
#define AUDIO_MIC_GAIN           24

// ---- 按键与指示（原理图：拨轮开关 KEY1/2/3） ----
#define KEY1_GPIO        GPIO_NUM_4
#define KEY2_GPIO        GPIO_NUM_3
#define KEY3_GPIO        GPIO_NUM_2
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define BUILTIN_LED_GPIO GPIO_NUM_1   // 运行指示灯 LED2

#endif  // _HUANZHU_BOARD_CONFIG_H_
