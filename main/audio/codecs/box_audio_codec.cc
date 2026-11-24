#include "box_audio_codec.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>

#include <vector>

#define TAG "BoxAudioCodec"

#if CONFIG_BOARD_TYPE_CYBERAI_VOCAT_V2_1
// ES7210 TDM 槽序为 MIC1, MIC3, MIC2, MIC4。本板 MIC1 几乎收不到人声，
// MIC2 是可用主麦，MIC3 是喇叭回采（AEC 参考），因此取 slot2/slot1。
static constexpr size_t kV2_1MicTdmSlot = 2;
static constexpr size_t kV2_1RefTdmSlot = 1;
static std::vector<int16_t> s_v2_1_tdm_buffer;
#endif

BoxAudioCodec::BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
                             gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout,
                             gpio_num_t din, gpio_num_t pa_pin, uint8_t es8311_addr,
                             uint8_t es7210_addr, bool input_reference, float input_gain,
                             int reference_gain_channel, float reference_gain, float pa_voltage,
                             int i2c_clock_speed_hz) {
    duplex_ = true;                              // 是否双工
    input_reference_ = input_reference;          // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1;  // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = input_gain;
    reference_gain_channel_ = reference_gain_channel;
    reference_gain_ = reference_gain;

    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
        // 0 表示沿用组件默认（100kHz）。个别板卡（I2C 上有电平转换）必须 400kHz，
        // 否则写寄存器会中途失败、codec 回退 dummy。输入侧复用同一份 i2c_cfg。
        .clock_speed_hz = i2c_clock_speed_hz,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(out_ctrl_if_ != NULL);

    if (i2c_clock_speed_hz != 0) {
        // 诊断：绕过 codec 驱动，直接用组件自己的 ctrl_if 读写一次寄存器。
        // 用于区分故障在 ctrl_if 层（总线/速率/地址）还是 codec 驱动层。
        uint8_t probe = 0;
        int rret = out_ctrl_if_->read_reg(out_ctrl_if_, 0x00, 1, &probe, 1);
        ESP_LOGI(TAG, "ctrl_if.read_reg(0x00) ret=%d val=0x%02X", rret, probe);
        uint8_t back[1] = {probe};
        int wret = out_ctrl_if_->write_reg(out_ctrl_if_, 0x00, 1, back, 1);
        ESP_LOGI(TAG, "ctrl_if.write_reg(0x00,0x%02X) ret=%d", probe, wret);
    }

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = pa_voltage;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    out_codec_if_ = es8311_codec_new(&es8311_cfg);
    assert(out_codec_if_ != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(output_dev_ != NULL);

    // Input
    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != NULL);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != NULL);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != NULL);

    ESP_LOGI(TAG, "BoxAudioDevice initialized");
}

BoxAudioCodec::~BoxAudioCodec() {
    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void BoxAudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        // 缓冲时长按采样率保持不变（基准 16k / 240 帧 ≈ 90ms）。
        // 否则 24k 板卡缓冲时长会掉到 60ms，TTS 播放容易欠载破音。
        .dma_frame_num = static_cast<uint32_t>(AUDIO_CODEC_DMA_FRAME_NUM) *
                         output_sample_rate_ / 16000,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex channels created");
}

void BoxAudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

void BoxAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
#if CONFIG_BOARD_TYPE_CYBERAI_VOCAT_V2_1
        // 主麦在 slot2，必须采满 4 个 TDM 槽才能拿到，读回后在 Read() 里重排。
        fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) |
                           ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3);
#endif
        ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
#if CONFIG_BOARD_TYPE_CYBERAI_VOCAT_V2_1
        // 通道 0 是坏麦，增益必须下发到全部通道。
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(input_dev_, input_gain_));
#else
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(
            input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), input_gain_));
#endif
        if (input_reference_ && reference_gain_channel_ >= 0) {
            // ES7210 gain masks use physical MIC numbering, which differs
            // from the TDM slot order (MIC1, MIC3, MIC2, MIC4).
            ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(
                input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(reference_gain_channel_),
                reference_gain_));
        }
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
}

void BoxAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        // Play 16bit 1 channel
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, output_volume_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    }
    AudioCodec::EnableOutput(enable);
}

int BoxAudioCodec::Read(int16_t* dest, int samples) {
#if CONFIG_BOARD_TYPE_CYBERAI_VOCAT_V2_1
    if (input_enabled_ && input_reference_ && (samples % 2) == 0) {
        const size_t frames = static_cast<size_t>(samples) / 2;
        s_v2_1_tdm_buffer.resize(frames * 4);
        const esp_err_t ret = esp_codec_dev_read(input_dev_, s_v2_1_tdm_buffer.data(),
                                                 s_v2_1_tdm_buffer.size() * sizeof(int16_t));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "V2.1 TDM read failed: %s", esp_err_to_name(ret));
            return 0;
        }
        for (size_t frame = 0; frame < frames; ++frame) {
            dest[frame * 2] = s_v2_1_tdm_buffer[frame * 4 + kV2_1MicTdmSlot];
            dest[frame * 2 + 1] = s_v2_1_tdm_buffer[frame * 4 + kV2_1RefTdmSlot];
        }
        return samples;
    }
#endif
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

int BoxAudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(output_dev_, (void*)data, samples * sizeof(int16_t)));
    }
    return samples;
}
