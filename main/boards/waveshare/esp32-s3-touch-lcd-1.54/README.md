# 产品链接

[微雪电子 ESP32-S3-Touch-LCD-1.54](https://www.waveshare.net/shop/ESP32-S3-Touch-LCD-1.54.htm)
[微雪电子 ESP32-S3-LCD-1.54](https://www.waveshare.net/shop/ESP32-S3-LCD-1.54.htm)

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
python scripts/build.py waveshare/esp32-s3-touch-lcd-1.54 --language zh-CN
```

**或手动配置编译目标为 ESP32S3**

```bash
idf.py set-target esp32s3
idf.py menuconfig     # Xiaozhi Assistant -> Board Type -> Waveshare ESP32-S3-Touch-LCD-1.54
idf.py build flash monitor
```

---

# 本 fork 追加的功能

硬件是 ESP32-S3R8（8 MB PSRAM）+ **16 MB Flash**（默认分区表就是 `partitions/v2/16m.csv`，无需改配置），
1.54 寸 240×240 电容触摸 IPS（CST816）。

## 硬件引脚

| 功能 | 引脚 |
| --- | --- |
| I2S MCLK / WS / BCLK / DIN / DOUT | 8 / 10 / 9 / 11 / 12 |
| 功放 PA | 7 |
| Codec I2C SDA / SCL | 42 / 41 |
| 屏幕 SPI CS / MOSI / CLK / DC / RST | 21 / 39 / 38 / 45 / 40（mode 3） |
| 触摸 INT / RST | 48 / 47（I2C 与 codec 共用） |
| 背光 | 46 |
| 电池使能 / 电池 ADC / 充电检测 | 2 / 1 / 3 |
| PWR / VOL+ / VOL− | 5 / 4 / 0（VOL− 与 BOOT strapping 复用） |

## 触摸 UI（`touch_display.{h,cc}`，`TouchLcdDisplay : SpiLcdDisplay`）

CST816 由上游的 `esp_lcd_touch_new_i2c_cst816s` + `lvgl_port_add_touch` 采集，手势逻辑全部在这一层：

| 手势 | 面板关闭时 | 面板打开时 |
| --- | --- | --- |
| 单击 | 切换对话状态（`ToggleChatState`） | 忽略 |
| 下滑 | 打开设置面板 | 关闭设置面板 |
| 上滑 | **刻意忽略**（点击已经能切对话，避免重复） | 忽略 |
| 左右滑（≥40 px 且横向占主导） | 切换明/暗主题 | 忽略 |

- 点击判定在 indev 层做：位移 < 15 px、时长 < 700 ms 才算单击。
- **按下点落在控件上一律不算手势**（音量/亮度滑条、AEC 开关、按钮行都有 12 px 容差），
  这样在滑条上横着拖不会误切主题。
- 设置面板：AEC 开关（`#if CONFIG_USE_DEVICE_AEC`）、音量滑条、亮度滑条、配网 / 关机 / 关闭三个按钮。
  - 音量只在**松手**时应用一次（`AudioCodec::SetOutputVolume` 每次都写 NVS，不能连续调用）；
  - 亮度实时生效，松手时写 NVS `display/brightness`。
  - 滑条是 160×14、旋钮 22×22 圆角、`ext_click_area` 16，并且**整行都可点可拖**：LVGL 9.5 的 slider
    做了「只认旋钮」的高级命中测试（`lv_slider.c` 的 `LV_EVENT_HIT_TEST`），点轨道根本不会进 widget，
    所以由行对象接管 `PRESSED` / `PRESSING` / `RELEASED`，按触摸点 x 反算值再回写给滑条。

## 小屏消息区（`CONFIG_DISPLAY_COMPACT_CHAT_BAND`）

240×240 上默认的单行循环滚动几乎没法读，所以这块板同时打开：

- `CONFIG_USE_MULTILINE_CHAT_MESSAGE=y`（上游开关，换行显示）
- `CONFIG_DISPLAY_COMPACT_CHAT_BAND=y`（**本 fork 新增**，见 `main/Kconfig.projbuild`）：
  - 表情框移到状态栏下方（`text_font->line_height + spacing(6)` = 40 px，状态栏占 4..32）；
  - 消息带上限高 `line_height * 5 + spacing(4) * 2` = 156 px（内容区正好 140 px = 5 行整），
    超出后竖向滚动，只有真的溢出时才滚到底，短消息时把偏移归零（否则会「削头」）；
- 表情素材从 `noto-color-emoji_64` 换成 `noto-color-emoji_32`（`main/CMakeLists.txt` 里本板那行）。

这两个开关默认都是 `n`，上游其它板卡零影响。

## 按键

| 按键 | 动作 |
| --- | --- |
| PWR（GPIO5）单击 | 切换对话状态 |
| PWR 长按（≥2 s） | 关机（拉低 `BATTERY_EN_PIN`，软锁存断电） |
| PWR 双击 | 背光 0 ↔ 50 切换 |
| PWR 三击 | 进 WiFi 配网 |
| VOL+（GPIO4）单击 / 长按 | 音量 +10（上限 100）/ 直接 100 |
| VOL−（GPIO0）单击 / 长按 | 音量 −10 / 静音 |
| VOL− 双击 | 切换 AEC（仅空闲时生效） |

## 省电

`PowerSaveTimer(-1, 60, 300)`：60 秒无活动背光降到 20（表情切 `sleepy`），300 秒直接关机。
`cpu_max_freq = -1` 表示**不降频、不关麦、不关唤醒词**。定时器只在**放电**时启用，插着 USB 永远不变暗/关机。

## 已知问题

1. **双击亮度恢复的 bug**：`esp32-s3-touch-lcd-1.54.cc` 里 `brightness_last = 0; if (brightness_last == 0) ... else ...`，
   else 分支是死代码，「恢复上次亮度」永远不生效（一直是 0 ↔ 50）。
2. **充电检测没有去抖**：CHRG 直接读电平，插拔瞬间可能抖一下。
3. **电量只有 6 档**：`power_manager.h` 按 3.52 / 3.64 / 3.76 / 3.88 / 4.00 V 阈值返回 1/20/40/60/80/100，
   状态栏也只能画图标（`battery_label_` 用的是图标字体）。想把右上角改成数字需要新增
   `DISPLAY_BATTERY_PERCENT` 之类开关，目前**没做**。
4. 触摸目前只驱动本文件的设置面板与手势，`main/display/` 里没有其它控件响应触摸。
