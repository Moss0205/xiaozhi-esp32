// 幻珠 HuanZhu 定制 codec 实现
//   输出：I2S TX -> MAX98357A（复用上游 NoAudioCodec 的 Write/EnableOutput）
//   输入：ADC1 连续转换 <- ZTS6216 模拟麦（GPIO6 = ADC1_CH5）
// API 依据 ESP-IDF v6.1 components/esp_adc/include/esp_adc/adc_continuous.h
#include "huanzhu_audio_codec.h"

#include <esp_log.h>
#include <esp_check.h>
#include <soc/soc_caps.h>
#include <driver/i2s_std.h>
#include <esp_adc/adc_continuous.h>

#include <cmath>
#include <cstring>

#define TAG "HuanzhuCodec"

// 单次 adc_continuous_read_parse 的最大样本数（4 字节/样本）
#define HZ_ADC_BATCH_SAMPLES 256

HuanzhuAudioCodec::HuanzhuAudioCodec(int input_sample_rate, int output_sample_rate,
                                     gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                     gpio_num_t spk_sd_pin,
                                     adc_unit_t mic_adc_unit, adc_channel_t mic_adc_channel,
                                     gpio_num_t mic_adc_gpio, int mic_gain)
    : spk_sd_pin_(spk_sd_pin),
      adc_unit_(mic_adc_unit),
      adc_channel_(mic_adc_channel),
      adc_gpio_(mic_adc_gpio),
      mic_gain_(mic_gain > 0 ? mic_gain : 16) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_reference_ = false;

    // ---------------- 输出：I2S TX（与 NoAudioCodecSimplex 一致） ----------------
    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
#if SOC_I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
#if SOC_I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));

    // MAX98357A 的 SD_MODE 使能脚（原理图经 R31 上拉，这里主动拉高确保开声）
    if (spk_sd_pin_ != GPIO_NUM_NC) {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << (int)spk_sd_pin_;
        io.mode = GPIO_MODE_OUTPUT;
        io.pull_up_en = GPIO_PULLUP_DISABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io));
        ESP_ERROR_CHECK(gpio_set_level(spk_sd_pin_, 1));
    }

    // ---------------- 输入：ADC1 连续转换 ----------------
    adc_continuous_handle_cfg_t adc_hdl_cfg = {
        .max_store_buf_size = (uint32_t)input_sample_rate_ * SOC_ADC_DIGI_RESULT_BYTES,  // 1 秒
        .conv_frame_size = HZ_ADC_BATCH_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES,
        .flags = { .flush_pool = 0 },
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_hdl_cfg, &adc_handle_));

    // 注意：IDF 6.1 的衰减枚举是 ADC_ATTEN_DB_12（旧文档/示例里的 DB_11 已改名）
    adc_digi_pattern_config_t pattern = {
        .atten = ADC_ATTEN_DB_12,
        .channel = (uint8_t)adc_channel_,
        .unit = (uint8_t)adc_unit_,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,   // S3 = 12，必须显式给（DEFAULT=0 会被拒）
    };
    adc_continuous_config_t adc_cfg = {
        .pattern_num = 1,
        .adc_pattern = &pattern,
        .sample_freq_hz = (uint32_t)input_sample_rate_,   // 16000 在 S3 合法区间 611~83333
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,              // S3 仅 ADC1 支持连续模式
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle_, &adc_cfg));

    // 解析缓冲放堆上：256 * sizeof(adc_continuous_data_t) 不宜占用任务栈
    parsed_.resize(HZ_ADC_BATCH_SAMPLES);

    ESP_LOGI(TAG, "Huanzhu codec ready: spk I2S %dHz, mic ADC1_CH%d@GPIO%d %dHz gain=%d",
             output_sample_rate_, (int)adc_channel_, (int)adc_gpio_, input_sample_rate_, mic_gain_);
}

HuanzhuAudioCodec::~HuanzhuAudioCodec() {
    if (adc_running_ && adc_handle_) {
        adc_continuous_stop(adc_handle_);
        adc_running_ = false;
    }
    if (adc_handle_) {
        adc_continuous_deinit(adc_handle_);
        adc_handle_ = nullptr;
    }
    // tx_handle_ 由基类 ~NoAudioCodec 负责 disable；rx_handle_ 始终为 nullptr
}

void HuanzhuAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        // flush_pool 只能在非运行态调用：先停 -> 清池 -> 再起
        if (adc_running_) {
            adc_continuous_stop(adc_handle_);
            adc_running_ = false;
        }
        ESP_ERROR_CHECK(adc_continuous_flush_pool(adc_handle_));
        ESP_ERROR_CHECK(adc_continuous_start(adc_handle_));
        adc_running_ = true;
        dc_seeded_ = false;
    } else {
        if (adc_running_) {
            adc_continuous_stop(adc_handle_);
            adc_running_ = false;
        }
    }
    // 跳过 NoAudioCodec::EnableInput（它会动 rx_handle_），直接落到 AudioCodec
    AudioCodec::EnableInput(enable);
}

bool HuanzhuAudioCodec::FillPending(int timeout_ms) {
    uint32_t got = 0;
    esp_err_t err = adc_continuous_read_parse(adc_handle_, parsed_.data(), HZ_ADC_BATCH_SAMPLES, &got, timeout_ms);
    if (err != ESP_OK || got == 0) {
        return false;
    }
    pending_.clear();
    pending_.reserve(got);
    for (uint32_t i = 0; i < got; i++) {
        if (!parsed_[i].valid || parsed_[i].channel != (uint8_t)adc_channel_) {
            continue;
        }
        int32_t raw = (int32_t)parsed_[i].raw_data;
        if (!dc_seeded_) {
            // 首帧直接用原始值播种直流，避免从 2048 慢慢爬升导致的开机爆音
            dc_avg_ = raw;
            dc_seeded_ = true;
        } else {
            // 泄漏积分估计直流：dc += (raw - dc) / 64
            dc_avg_ += (raw - dc_avg_) >> 6;
        }
        int32_t v = (raw - dc_avg_) * mic_gain_;
        if (v > INT16_MAX) v = INT16_MAX;
        if (v < -INT16_MAX) v = -INT16_MAX;
        pending_.push_back((int16_t)v);
    }
    return !pending_.empty();
}

int HuanzhuAudioCodec::Read(int16_t* dest, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (!adc_running_) {
        return 0;
    }
    int done = 0;
    while (done < samples) {
        size_t avail = consumed_ < pending_.size() ? pending_.size() - consumed_ : 0;
        if (avail == 0) {
            // 等待下一批；30ms 兜底（16kHz 下 256 样本约 16ms 一帧）
            if (!FillPending(30)) {
                break;   // 超时：返回已取到的部分
            }
            consumed_ = 0;
            continue;
        }
        size_t take = avail < (size_t)(samples - done) ? avail : (size_t)(samples - done);
        memcpy(dest + done, pending_.data() + consumed_, take * sizeof(int16_t));
        consumed_ += take;
        done += take;
    }
    return done;
}
