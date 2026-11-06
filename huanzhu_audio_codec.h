// 幻珠 HuanZhu 定制 codec：ADC 模拟麦（ZTS6216 -> ADC1_CH5/GPIO6）+ I2S MAX98357A 功放
// 复用上游 NoAudioCodec 的输出通路（音量曲线 + i2s_channel_write），
// 只把输入从 I2S-RX 换成 ADC 连续转换。
// 接口依据 main/audio/audio_codec.h 与 main/boards/common/no_audio_codec.h。
#ifndef _HUANZHU_AUDIO_CODEC_H_
#define _HUANZHU_AUDIO_CODEC_H_

#include "audio_codec.h"
#include "codecs/no_audio_codec.h"

#include <driver/gpio.h>
#include <hal/adc_types.h>
#include <esp_adc/adc_continuous.h>

#include <vector>

class HuanzhuAudioCodec : public NoAudioCodec {
public:
    HuanzhuAudioCodec(int input_sample_rate, int output_sample_rate,
                      gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                      gpio_num_t spk_sd_pin,
                      adc_unit_t mic_adc_unit, adc_channel_t mic_adc_channel,
                      gpio_num_t mic_adc_gpio, int mic_gain);
    ~HuanzhuAudioCodec() override;

public:
    // 桥接版公开包装（bridge_hal 用）：基类 Read/Write 是 protected，外部直接调编不过
    int ReadPcm(int16_t* dest, int samples) { return Read(dest, samples); }
    int WritePcm(const int16_t* data, int samples) { return Write(data, samples); }

protected:
    // Write / EnableOutput 沿用 NoAudioCodec（只操作 tx_handle_）
    int Read(int16_t* dest, int samples) override;
    void EnableInput(bool enable) override;

private:
    // 从 ADC 取一帧并解码进 pending_（含直流剔除与增益）
    bool FillPending(int timeout_ms);

    gpio_num_t spk_sd_pin_;
    adc_unit_t adc_unit_;
    adc_channel_t adc_channel_;
    gpio_num_t adc_gpio_;
    int mic_gain_;

    adc_continuous_handle_t adc_handle_ = nullptr;
    bool adc_running_ = false;

    std::vector<int16_t> pending_;            // 已解码待取走的样本
    size_t consumed_ = 0;                     // pending_ 中已取走的数量
    std::vector<adc_continuous_data_t> parsed_;  // 单批解析缓冲（堆上，避免栈溢出）
    int32_t dc_avg_ = 2048;                   // 泄漏积分估计的直流偏置
    bool dc_seeded_ = false;                  // 首帧用原始值直接播种直流
};

#endif  // _HUANZHU_AUDIO_CODEC_H_
