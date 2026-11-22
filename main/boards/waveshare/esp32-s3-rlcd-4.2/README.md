# 产品链接

[微雪电子 ESP32-S3-RLCD-4.2](https://www.waveshare.net/shop/ESP32-S3-RLCD-4.2.htm)

# 编译配置命令

**克隆工程**

```bash
git clone https://github.com/78/xiaozhi-esp32.git
```

**进入工程**

```bash
cd xiaozhi-esp32
```

**使用脚本编译（推荐，会自动带好板卡参数）**

```bash
python scripts/build.py waveshare/esp32-s3-rlcd-4.2 --language zh-CN
```

**或手动配置编译目标为 ESP32S3**

```bash
idf.py set-target esp32s3
idf.py menuconfig     # Xiaozhi Assistant -> Board Type -> Waveshare ESP32-S3-RLCD-4.2
idf.py build flash monitor
```

---

# 本 fork 追加的功能

以下内容都是在本仓库（`esp32-xiaozhi`）里加的，不属于上游。

## 硬件引脚

| 功能 | 引脚 |
| --- | --- |
| RLCD DC / CS / SCK / MOSI / RST / TE | 5 / 40 / 11 / 12 / 41 / 6 |
| I2S MCLK / WS / BCLK / DIN / DOUT | 16 / 45 / 9 / 10 / 8 |
| 功放 PA（高有效） | 46 |
| Codec I2C SDA / SCL | 13 / 14 |
| BOOT / USER（屏右侧 KEY） | 0 / 18 |
| 电池 ADC | 4（`ADC_CHANNEL_3`，200K/100K 分压） |

## 显示驱动

- 屏幕是 **ST7305 400×300 单色（1 bpp）**，LUT 结构见 `PixelIndexLUT[300]` / `PixelBitLUT[300]`。
- SPI 驱动已上收到公共目录：`main/boards/common/rlcd_driver.{h,cc}`（`RlcdDriver`，默认 SPI3 / 40 MHz，可通过
  ctor 的 `pclk_hz` 参数改）。本板和 `luobo/esp32-s3-nano-rlcd` 共用同一份驱动。
- 标准 UI 的显示层在 `main/boards/common/rlcd_display.{h,cc}`（`RlcdLcdDisplay`，RGB565 → 灰度阈值 160 硬二值化），
  nano-rlcd 用它；**本板仍用自绘的卡片式 UI**（`custom_lcd_display.{h,cc}` + `weather_ui.cc` / `music_ui.cc` /
  `pomodoro_ui.cc`），直接持有 `RlcdDriver`。全部中文用 `font_puhui_14_1`（7415 常用汉字，避免缺字）。

## 按键

| 按键 | 动作 |
| --- | --- |
| BOOT（GPIO0）单击 | 唤醒到 L0；`kDeviceStateStarting` 时进 WiFi 配网，否则切换对话状态 |
| BOOT 双击 | 切换设备端 AEC（仅空闲时生效，否则打日志忽略） |
| USER（GPIO18）单击 | 切换天气页 / 音乐页（`CycleDisplayMode()`） |
| USER 双击 | 刷新天气、传感器、时间 |
| USER 长按 | 在对话区显示系统信息（内存 / CPU / 电池 / WiFi / IP） |

## 分级省电

`config.json` 打开 `CONFIG_PM_ENABLE=y`，板级 `pwr_tier` 任务（核心 1，4 KB 栈，5 秒轮询）按静置时长升降档：

| 档位 | 触发 | 动作 |
| --- | --- | --- |
| L0 | 有交互 / 唤醒 / 开机 | CPU 240/160 MHz，AEC 恢复用户选择，WiFi 恢复原 `listen_interval` |
| L1 | 静置 3 分钟 | CPU 160/160 MHz，**关设备端 AEC** |
| L2 | 静置 15 分钟 | CPU 80/80 MHz，关 AEC，WiFi `listen_interval = 10`（约 1 秒才醒一次） |

- 三档都保留 AFE 唤醒词，随时能喊「小智」；只做调频，`light_sleep_enable = false`（避免打断 I2S/AFE 取帧）。
- 设备状态不是 `kDeviceStateIdle`（对话/配网/升级中）一律按「正在使用」处理，不降档。
- 档位切换日志：`PWR L0 -> L1 (cpu<=160MHz aec=off)`。
- 省电档关掉的 AEC 会记在 `aec_overridden_` 里，回 L0 时恢复用户原来的选择，不会把 L2→L1 的中间态覆盖掉。

## AEC 开关

- BOOT 双击切换，存 NVS；MCP 工具 `self.system.set_aec` 也能切。
- 三个页面的状态栏都有常驻徽标，显示真实 AEC 状态（省电档临时关掉的也会如实显示）。

## 电量

板载充电芯片 ETA6098 的 STAT 只驱 LED、**没有 GPIO 可读**，所以充电状态靠软件推断：

- 60 个样本 × 10 秒 = 10 分钟环形窗口，算端点斜率（mV/min）与突降；
- 电压经 EMA 平滑后按本板实测曲线分段线性映射（4120 mV = 100%，4100→95，4000→86，3900→74，3800→58，
  3700→40，3600→21，3500→11，3300→3，3000→0）；
- ±1% 迟滞，避免分段边界抖动（RLCD 每次刷新都是整屏 SPI 写入，容易留残影）；
- 日志：`BAT %umV -> %d%% (chg=%d full=%d slope=%dmV/min drop=%dmV up=%d down=%d)`。

## 板级 MCP 工具（13 个）

| 工具 | 说明 |
| --- | --- |
| `self.system.info` | 内存 / CPU / 电池 / WiFi 摘要 |
| `self.system.set_aec` | 开关设备端 AEC |
| `self.weather.update` | 刷新天气 |
| `self.disp.network` | 重新配网 |
| `self.disp.switch` | 切换显示页面 |
| `self.pomodoro.start` / `stop` / `status` / `pause` | 番茄钟 |
| `self.memo.add` / `list` / `done` / `clear` | 备忘 |

## 天气 API Key

```bash
cd main/boards/waveshare/esp32-s3-rlcd-4.2
cp secret_config.h.example secret_config.h    # 填入 WEATHER_API_KEY，本文件已被 gitignore
```

## 历史遗留

- **SD 卡音乐已经放弃并移除**：`managers/music_manager.{h,cc}` 与相关 MCP 工具不在本仓库中。
  但 `custom_lcd_display.cc` 里仍调用 `SetupMusicUI()`（`music_ui.cc` 的音乐页 UI 还在），后端没有播放器，
  点进去是静态页面 —— 待清理。
