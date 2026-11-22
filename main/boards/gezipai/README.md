# 格子派 AI-2E（gezipai）

ESP32-S3 双芯片音频板（ES8311 DAC + ES7210 ADC），**整机不接屏幕**，板上预留 4G 模组位。
本板源自 2.5.2 fork，合并到 2.6.0 基线时移植过来。

- 板卡身份：`type = "gezipai"`（`main/boards/gezipai/config.json`）
- 源码：`main/boards/gezipai/{config.h, config.json, gezipai_board.cc}`
- 基类：`DualNetworkBoard`（WiFi / ML307 4G 双通道，见 `main/boards/common/dual_network_board.h`）

## 硬件引脚

| 功能 | 引脚 |
| --- | --- |
| I2S MCLK / WS / BCLK / DIN / DOUT | 40 / 45 / 39 / 21 / 48 |
| 功放 PA（高有效） | 46 |
| Codec I2C SDA / SCL | 38 / 47 |
| ES8311 / ES7210 地址 | `ES8311_CODEC_DEFAULT_ADDR`（0x30）/ `ES7210_CODEC_DEFAULT_ADDR`（0x80） |
| 电池 ADC | 3（`ADC_CHANNEL_2`，3:1 分压，读数 ×3） |
| 充电状态 CHRG / DONE（LGS4056） | 8 / 19（低有效；DONE 与 USB D- 复用，插 USB 时不可信） |
| 状态灯 | 13 |
| 4G 电源使能 | 17（板上无模块，固件拉低断电） |
| 4G 串口 RX / TX / DTR | 42 / 1 / 未接 |
| 耳机检测 | 16 |
| BOOT / RESET | 0 / EN |
| 预留焊盘（无实体键） | 2（`BOOT_BUTTON_2_GPIO`）、18（`VOLUME_UP_BUTTON_GPIO`） |

`config.h` 里仍保留 ST7789 240×240 的屏幕宏（SPI 4/5/6/7、RST 15、背光 9），但整机没有屏，
固件走 `Board` 默认的 `NoDisplay`。

## 编译

```bash
python scripts/build.py gezipai --name gezipai          # 内网 OTA 变体
python scripts/build.py gezipai --name gezipai-public    # 公网 OTA 变体
```

两个变体只差 `CONFIG_OTA_URL`：

| 变体 | OTA 地址 |
| --- | --- |
| `gezipai` | `http://192.168.22.153:8002/xiaozhi/ota/` |
| `gezipai-public` | `https://xiaozhi.luobonas.cn:18002/xiaozhi/ota/` |

两个变体都带 `CONFIG_USE_DEVICE_AEC=y`。唤醒词不要写进 `config.json`
（`scripts/build.py` 的 `test_language_and_wake_word_are_not_board_config_options` 会直接报错），
中文默认按 `wn9_nihaoxiaozhi_tts` 编译。

## 按键

| 按键 | 动作 |
| --- | --- |
| BOOT（GPIO0）单击 | `kDeviceStateStarting` 时进 WiFi 配网，否则 `ToggleChatState()` |
| BOOT 长按 | 进 WiFi 配网（4G 模式下跳过并打日志） |
| VOL+（GPIO18）单击 / 长按 | 音量 +10（上限 100）/ 直接 100 |
| VOL−（`GPIO_NUM_NC`）单击 / 长按 | 音量 −10 / 静音（板上无此实体键，代码保留） |

## 移植要点

1. **24 kHz 采样**：`AUDIO_INPUT/OUTPUT_SAMPLE_RATE = 24000`，与服务端 TTS 对齐，省掉一次重采样。
   为此把 `main/audio/codecs/box_audio_codec.cc` 的 DMA 帧数改成按输出采样率缩放：
   `.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM * output_sample_rate_ / 16000`。
   16k 板卡上该表达式恒等（240×16000/16000=240），24k 板卡维持约 90 ms 缓冲，代价约 +1.4 KB RAM。
2. **无屏**：`GetDisplay()` 不 override。2.6.0 的 `main/assets.cc` 在 `LvglStrategy::Apply` 里已有守卫
   （`display == nullptr || !display->SupportsGuiOperations()` → 跳过 UI 资源），
   所以 2.5.2 时代那个「提前 `lv_init` 只为加载字体」的 `EnsureLvglInitialized()` **不再需要**，移植时已删除。
3. **电池**：1 秒一次的 `esp_timer` 翻状态灯、检测 CHRG/DONE 边沿，每 10 拍（10 秒）读一次电池；
   电压取 10 次平均后做校准换算再 ×3，按 3300–4200 mV **线性**映射成 0–100%。
   充电判定只看 CHRG 低电平（DONE 不可信）。
4. **网络**：`DualNetworkBoard(Module_4G_TX_PIN, Module_4G_RX_PIN, Module_4G_DTR_PIN, 0)`，默认 WiFi。

## 已知问题

**Codec 不应答会无限重启。** `BoxAudioCodec` 的构造函数对 `ctrl_if` / `out_codec_if` 直接 `assert`，
一旦 ES8311 在 I2C 0x30 上不应答，设备会：

```
E I2C_If: Fail to write to dev 30
E ES8311: Open fail
assert failed: BoxAudioCodec::BoxAudioCodec(...) box_audio_codec.cc:55
rst:0xc (RTC_SW_CPU_RST)
```

然后进入重启循环（2.5.2 固件现场日志就是这样）。修复方向：`GetAudioCodec()` 先用
`i2c_master_probe()` 探 ES8311 / ES7210，探不到就回退 `DummyAudioCodec`——没有声音但设备能启动、
能配网、能看日志；可参考 `main/boards/luobo/esp32-s3-nano-rlcd/esp32-s3-nano-rlcd.cc` 里
`ReadCodecReg()` + `write_ok` 计数 + `DummyAudioCodec` 回退的写法。**目前尚未实施。**
