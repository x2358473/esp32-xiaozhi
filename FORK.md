# 本仓库说明（相对上游的改动）

本仓库 `x2358473/esp32-xiaozhi` 是 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的个人 fork，
基线是上游官方 **v2.6.0** 提交 `4de00aed`（`upstream/v2.6.0`），本仓库的提交直接接在它之上，
因此以后可以用 `git merge upstream/main` 跟进上游，不再做整树搬运。

- 上游板卡、协议、音频链路保持原样，所有自研内容都带独立 Kconfig 开关或只落在板卡目录里；
- 板卡身份（`config.json` 的 `type`、板卡目录名、`BOARD_TYPE_*`）一经发布不再改名，否则服务端会把设备当新机器。

## 维护的板卡

| 板卡 | `type` | 目录 | 文档 |
| --- | --- | --- | --- |
| 微雪 ESP32-S3-RLCD-4.2（400×300 单色 RLCD） | `waveshare-s3-rlcd-4.2` | `main/boards/waveshare/esp32-s3-rlcd-4.2/` | [README](main/boards/waveshare/esp32-s3-rlcd-4.2/README.md) |
| 微雪 ESP32-S3-Touch-LCD-1.54（240×240 触摸彩屏） | `esp32-s3-touch-lcd-1.54` | `main/boards/waveshare/esp32-s3-touch-lcd-1.54/` | [README](main/boards/waveshare/esp32-s3-touch-lcd-1.54/README.md) |
| luobo ESP32-S3-Nano RLCD 4.2（ST7305 + CST836U 触摸） | `esp32-s3-nano-rlcd` | `main/boards/luobo/esp32-s3-nano-rlcd/` | [README](main/boards/luobo/esp32-s3-nano-rlcd/README.md) |
| 格子派 AI-2E（无屏，ES8311/ES7210，4G 预留） | `gezipai` | `main/boards/gezipai/` | [README](main/boards/gezipai/README.md) |
| 赛博喵伴 V2.1 / CyberAI Vocat（ESP32-S3，360×360 圆屏，实机为开发目标） | `cyber-vocat-v2.1` | `main/boards/cyberai/cyber-vocat-v2.1/` | [README](main/boards/cyberai/cyber-vocat-v2.1/README.md)、[实机笔记](docs/cyber-vocat-v2.1-notes.md) |

> 赛博喵伴 V2.1 是上游板卡，本仓库已在实机上验证并做了最小改动（TDM 麦克风槽位、设备端 AEC），
> 其余保持上游原样。实机测试、与出厂固件的差距、TF 卡/MP3 的落点都记在
> [docs/cyber-vocat-v2.1-notes.md](docs/cyber-vocat-v2.1-notes.md)。

## 相对上游的公共改动

| 位置 | 改动 | 影响面 |
| --- | --- | --- |
| `main/Kconfig.projbuild` + `main/display/lcd_display.cc` | 新增 `DISPLAY_COMPACT_CHAT_BAND`：小方屏上把表情移到状态栏下方、消息区限高（5 行）并竖向滚动 | 默认 `n`，只影响显式打开的板卡（本仓库目前只有 1.54 触摸屏） |
| `main/audio/codecs/box_audio_codec.{h,cc}` | ① 构造函数新增第 17 个参数 `int i2c_clock_speed_hz = 0`（0 = 沿用组件默认 100 kHz），非 0 时额外打印一次 ctrl_if 读/写诊断日志；② `.dma_frame_num` 改为按输出采样率缩放 `AUDIO_CODEC_DMA_FRAME_NUM * output_sample_rate_ / 16000` | 16 kHz 板卡上表达式恒等（240），24 kHz 板卡缓冲时长保持约 90 ms；默认参数保证其它板卡零影响 |
| `main/boards/common/rlcd_driver.{h,cc}`（原来的 waveshare 私有驱动） | ST7305 400×300 1 bpp 的 SPI 驱动上收为公共文件，构造函数新增 `spi_host` 与 `pclk_hz` 参数 | 被本仓库两块 RLCD 板共用 |
| `main/boards/common/rlcd_display.{h,cc}` | 新增 `RlcdLcdDisplay : LcdDisplay`：自己 `lv_init` + `lvgl_port_init` + 定长 PSRAM 帧缓冲，RGB565 转灰度阈值 160 二值化 | 可选实现，标准 UI 的 RLCD 板直接用它 |
| `.github/workflows/build.yml` | 产物改名 `xiaozhi_<board>_<sha7>.bin`，附带 assets 分区镜像与 `_full.bin`、`xiaozhi.elf/.map`，`main` 分支或手动触发时自动发 Release；新增 `workflow_dispatch` 的 `board` 输入；`push`/`pull_request` 加 `paths-ignore`（纯文档改动不启用 workflow）；`push` 触发时只编 `MAINTAINED_BOARDS`（本仓库四块板） | 只影响 CI，不影响固件 |
| `scripts/make_full_image.py`、`scripts/decode_backtrace.py` | 新增：把 app 与 assets 拼成整片镜像（按 `flash_args` 的偏移覆盖，先清 0xFF）；用 `pyelftools` 还原固件崩溃回溯 | 工具脚本 |
| `scripts/build.py` | 支持环境变量 `XIAOZHI_BOARD_FILTER`（逗号分隔多个板卡目录，如 `gezipai,luobo/esp32-s3-nano-rlcd`），把「按改动文件选变体」的结果再按板卡过滤；公共文件改动导致交集为空时改编该列表里的全部板卡，避免退回上游 18 个代表性变体 | 默认未设置时行为完全不变 |
| `main/boards/waveshare/esp32-s3-rlcd-4.2/managers/sensor_manager.cc` | `secret_config.h` 的 include 去掉 `../`（靠板卡目录在 include 路径里解析） | 修上游静态检查 `test_relative_board_includes_exist` |

## 编译

```bash
python scripts/build.py --list-boards                            # 看所有板卡与变体
python scripts/build.py waveshare/esp32-s3-rlcd-4.2 --language zh-CN
python scripts/build.py waveshare/esp32-s3-touch-lcd-1.54 --language zh-CN
python scripts/build.py luobo/esp32-s3-nano-rlcd --language zh-CN
python scripts/build.py gezipai --name gezipai                    # 或 --name gezipai-public
```

- 需要 ESP-IDF `release/v6.1`（最低 v6.0.1）；
- `--wake-word` 可以指定 ESP-SR 模型，例如 `--wake-word wn9l_nihaoxiaozhi_tts3`（中文默认是
  `wn9_nihaoxiaozhi_tts`）；唤醒词不允许写进板卡的 `config.json`；
- 烧写整片：`esptool --chip esp32s3 write_flash 0x0 <merged.bin>`（换基线时分区表可能变，不要只写 app）。

## 跟进上游

```bash
git fetch upstream                     # upstream = https://github.com/78/xiaozhi-esp32.git
git merge upstream/main                # 冲突热点见文末
```

`upstream` 的 tag 抓到了 `refs/tags/upstream/<版本>` 命名空间下，避免和本地 tag 同名冲突；
要看官方某个版本用 `git diff upstream/v2.7.0 -- main/`。

硬性规则见 `AGENTS.md` 的「Git workflow (hard rule for this fork)」：`main` 只 merge、绝不 rebase /
amend / force-push，否则以后所有同步都会失效。

## 仓库历史

- 2026-11-22：把历史重建到官方基线上。原来的 v2.6.0 是整树 squash 的 `428e985`（父提交为另一个
  fork 的 UI 分支），与官方 `4de00aed` 内容一致但祖先断裂，无法 merge。现在 `main` 直接从
  `upstream/v2.6.0` 起，12 个自有提交逐个 cherry-pick 上来，内容与旧树零差异（仅文件 mode 随之取官方值）。
  旧历史保留在分支 `port/v2.6.0`（`4fde6b8`）与 `archive/xqy-2.6.0`（`0f82781`）上。
- 2026-11：把两个 2.5.2 时代的 fork 合并进本仓库（gezipai 板卡、24 kHz 采样、nano-rlcd 板卡、CI 工具链）。
  旧仓库已打 tag `final-v2.5.2` 归档：
  - `esp32-s3-xiaozhi` → `final-v2.5.2` = `f6d1e3a`
  - `xiaozhi-esp32` → `final-v2.5.2` = `978e2d2`
- 仓库原名 `xiaozhi-esp32-rlcd`，因为现在不只维护 RLCD 板，改名为 `esp32-xiaozhi`。
  改名只影响 GitHub 地址与本地 clone 目录，**板卡身份与 OTA 记录不受影响**。

## 升级上游时的冲突热点

`main/audio/codecs/box_audio_codec.{h,cc}`、`main/display/lcd_display.cc`、`main/CMakeLists.txt`、
`main/Kconfig.projbuild`、`scripts/build.py`、`.github/workflows/build.yml`。
合并上游新版本时优先保留本仓库的默认关闭语义（新行为一律放在 `default n` 的开关后面）。
