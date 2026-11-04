// 幻珠 HuanZhu_360_360 V01 —— 小智定制板（原理图实测版）
// 屏：ST77916 360x360 SPI（CS10/DC15/RST14/MOSI13/SCLK12/BL8），初始化表取自厂商点亮示例
// 音：MAX98357A I2S（DOUT17/BCLK18/WS21，SD_MODE16 拉高）+ ZTS6216 模拟麦 ADC1_CH5/GPIO6
// 麦：由本仓库 huanzhu_audio_codec.{h,cc} 提供 ADC 连续采样输入通路，原机模拟麦直接可用
#include "wifi_board.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "power_save_timer.h"
#include "huanzhu_audio_codec.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>

#define TAG "huanzhu_360"

// 厂商实测初始化序列（188 条，由 360示例代码/main.cpp 自动转换；
// 0x11/0x29/0x2A/0x2B/0x3A/0x36 交由 esp_lcd 驱动与下面的 mirror 调用处理）
static const st77916_lcd_init_cmd_t vendor_specific_init_huanzhu[] = {
    {0x01, (uint8_t []){0x00}, 0, 150},  // SWRESET：对齐厂商实测时序，驱动不自动发
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x76, (uint8_t []){0x0F}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x52}, 1, 0},
    {0xB1, (uint8_t []){0x49}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB3, (uint8_t []){0x01}, 1, 0},
    {0xB4, (uint8_t []){0x66}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0xC5}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xB9, (uint8_t []){0x15}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x07}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xCC, (uint8_t []){0x7F}, 1, 0},
    {0xCD, (uint8_t []){0x7F}, 1, 0},
    {0xCE, (uint8_t []){0xFF}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0E, 0x15, 0x0A, 0x0B, 0x07, 0x3E, 0x43, 0x53, 0x39, 0x17, 0x17, 0x35, 0x38}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0D, 0x15, 0x0A, 0x0A, 0x06, 0x3D, 0x33, 0x52, 0x39, 0x16, 0x16, 0x35, 0x37}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x09}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x03}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x00}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x82}, 1, 0},
    {0xEA, (uint8_t []){0xDE}, 1, 0},
    {0xEB, (uint8_t []){0xC0}, 1, 0},
    {0xEC, (uint8_t []){0x40}, 1, 0},
    {0xED, (uint8_t []){0x84}, 1, 0},
    {0xEE, (uint8_t []){0xFF}, 1, 0},
    {0xEF, (uint8_t []){0x71}, 1, 0},
    {0xF8, (uint8_t []){0xFF}, 1, 0},
    {0xF9, (uint8_t []){0x50}, 1, 0},
    {0xFA, (uint8_t []){0xFF}, 1, 0},
    {0xFB, (uint8_t []){0xF3}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x42}, 1, 0},
    {0x61, (uint8_t []){0xDF}, 1, 0},
    {0x62, (uint8_t []){0x40}, 1, 0},
    {0x63, (uint8_t []){0x40}, 1, 0},
    {0x64, (uint8_t []){0x02}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x42}, 1, 0},
    {0x71, (uint8_t []){0xDF}, 1, 0},
    {0x72, (uint8_t []){0x40}, 1, 0},
    {0x73, (uint8_t []){0x40}, 1, 0},
    {0x74, (uint8_t []){0x01}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x04}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xDC}, 1, 0},
    {0x85, (uint8_t []){0x00}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x06}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xDE}, 1, 0},
    {0x8D, (uint8_t []){0x00}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x08}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xE0}, 1, 0},
    {0x95, (uint8_t []){0x00}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0A}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xE2}, 1, 0},
    {0x9D, (uint8_t []){0x00}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x03}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xDB}, 1, 0},
    {0xA5, (uint8_t []){0x00}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x05}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xDD}, 1, 0},
    {0xAD, (uint8_t []){0x00}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x07}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xDF}, 1, 0},
    {0xB5, (uint8_t []){0x00}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x09}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xE1}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x65}, 1, 0},
    {0xC1, (uint8_t []){0x74}, 1, 0},
    {0xC2, (uint8_t []){0x47}, 1, 0},
    {0xC3, (uint8_t []){0x56}, 1, 0},
    {0xC4, (uint8_t []){0xAA}, 1, 0},
    {0xC5, (uint8_t []){0x11}, 1, 0},
    {0xC6, (uint8_t []){0x00}, 1, 0},
    {0xC7, (uint8_t []){0x2A}, 1, 0},
    {0xC8, (uint8_t []){0xA2}, 1, 0},
    {0xC9, (uint8_t []){0x33}, 1, 0},
    {0xD0, (uint8_t []){0x65}, 1, 0},
    {0xD1, (uint8_t []){0x74}, 1, 0},
    {0xD2, (uint8_t []){0x47}, 1, 0},
    {0xD3, (uint8_t []){0x56}, 1, 0},
    {0xD4, (uint8_t []){0xAA}, 1, 0},
    {0xD5, (uint8_t []){0x11}, 1, 0},
    {0xD6, (uint8_t []){0x00}, 1, 0},
    {0xD7, (uint8_t []){0x2A}, 1, 0},
    {0xD8, (uint8_t []){0xA2}, 1, 0},
    {0xD9, (uint8_t []){0x33}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){0x00}, 0, 0},   // INVERSION ON（厂商实测需要）
    {0x11, (uint8_t []){0x00}, 0, 120},  // SLPOUT+120ms：驱动不自动发，缺它面板一直睡眠
    {0x29, (uint8_t []){0x00}, 0, 0},    // DISPON（与官方 1.85B 表尾对齐）
};

class HuanzhuBoard : public WifiBoard {
private:
    Button boot_button_;   // 拨轮上拨 KNOB_UP_GPIO：等效 BOOT 键（启动中进配网 / 平时切换对话）
    Button knob_press_button_;  // 拨轮中间按下 KNOB_PRESS_GPIO：对话开关
    Display* display_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeSpeakerEnable() {
        // MAX98357A 的 SD_MODE 经 R31 上拉，这里再主动拉高一次确保开声
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = 1ULL << (int)AUDIO_SPK_SD_GPIO;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));
        ESP_ERROR_CHECK(gpio_set_level(AUDIO_SPK_SD_GPIO, 1));
    }

    void InitializePanelLongReset() {
        // 厂商点亮示例实测时序：RST 低 500ms -> 高 1000ms，等电源泵稳定
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = 1ULL << (int)LCD_PIN_RST;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));
        ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_RST, 0));
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_RST, 1));
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_reset_pin(LCD_PIN_RST);  // 交还给 esp_lcd 驱动接管
        ESP_LOGI(TAG, "Panel long reset done (500ms/1000ms)");
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus: SCLK=%d MOSI=%d", LCD_PIN_SCLK, LCD_PIN_MOSI);
        spi_bus_config_t bus_config = {};
        bus_config.mosi_io_num = LCD_PIN_MOSI;
        bus_config.miso_io_num = GPIO_NUM_NC;
        bus_config.sclk_io_num = LCD_PIN_SCLK;
        bus_config.quadwp_io_num = GPIO_NUM_NC;
        bus_config.quadhd_io_num = GPIO_NUM_NC;
        bus_config.max_transfer_sz = LCD_WIDTH * 80 * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void InitializeSt77916Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Install panel IO (SPI 4-wire)");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = LCD_PIN_CS;
        io_config.dc_gpio_num = LCD_PIN_DC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 20 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &panel_io));

        ESP_LOGI(TAG, "Install ST77916 panel driver (SPI mode)");
        st77916_vendor_config_t vendor_config = {};
        vendor_config.flags.use_qspi_interface = 0;
        vendor_config.init_cmds = vendor_specific_init_huanzhu;
        vendor_config.init_cmds_size = sizeof(vendor_specific_init_huanzhu) / sizeof(st77916_lcd_init_cmd_t);

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.reset_gpio_num = LCD_PIN_RST;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        // 厂商 MADCTL 0x36=0xC0 等价于 MX+MY 双镜像
        esp_lcd_panel_mirror(panel, true, true);
        esp_lcd_panel_swap_xy(panel, false);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new SpiLcdDisplay(panel_io, panel,
                                     LCD_WIDTH, LCD_HEIGHT, 0, 0, true, true, false);
    }

    void InitializeButtons() {
        // 拨轮上拨 = BOOT 等效键：启动中按下进配网模式，平时切换对话状态
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        // 拨轮中间按下 = 对话开关（原 BOOT 键职责，这块板没有可按的 BOOT 键）
        knob_press_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    HuanzhuBoard() : boot_button_(KNOB_UP_GPIO), knob_press_button_(KNOB_PRESS_GPIO) {
        InitializePowerSaveTimer();
        InitializeSpeakerEnable();
        InitializePanelLongReset();
        InitializeSpi();
        InitializeSt77916Display();
        InitializeButtons();
        GetBacklight()->SetBrightness(100);  // 厂商为纯高电平背光，100% 占空比等效直流
    }

    virtual AudioCodec* GetAudioCodec() override {
        static HuanzhuAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_SPK_SD_GPIO,
            AUDIO_MIC_ADC_UNIT, AUDIO_MIC_ADC_CHANNEL, AUDIO_MIC_ADC_GPIO,
            AUDIO_MIC_GAIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        // 背光经三极管驱动，BLK 高电平=亮，不反相
        static PwmBacklight backlight(LCD_PIN_BL, false);
        return &backlight;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(HuanzhuBoard);
