# 赛博喵伴 V2.1（CyberAI Vocat）实机开发笔记

记录日期：2026-11-23。代码基线：本仓库 `main` @ `e3a09527`（`PROJECT_VER 2.6.0`）。
实机：CyberVoc-Board-V2_1，`SKU=cyber-vocat`，出厂固件 `2.1.0.5`（Brookesia UI），MAC `44:bd:8d:7c:cc:ac`。

这份文档只记录**实机测出来的事实**和**在本仓库上继续开发的落点**。上游板卡说明见
[`main/boards/cyberai/cyber-vocat-v2.1/README.md`](../main/boards/cyberai/cyber-vocat-v2.1/README.md)，
引脚见同目录 `config.h`。

## 1. 结论

1. 这块板本仓库**已经收录**：目录 `main/boards/cyberai/cyber-vocat-v2.1/`，Kconfig
   `BOARD_TYPE_CYBERAI_VOCAT_V2_1`（`Cyberai-Vocat-V2.1`），`scripts/build.py --list-boards` 可见
   `cyberai/cyber-vocat-v2.0` / `cyberai/cyber-vocat-v2.1`。不需要从零移植。
2. 出厂固件与本仓库板卡的差距集中在五块：**TF 卡 / 本地 MP3 播放 / 板级 MCP 工具 / Brookesia 环形菜单 UI /
   赛博授权链（deviceBind + ota.cyberai.top）**。本仓库这五样都没有（§7）。
3. **分区表不同**（§5）。首次烧本仓库固件必须整片烧（`0x0`），不能沿用只烧 `0x20000` 的习惯；
   回原厂固件用 §6 的整片备份。
4. 上板第一件要验的不是 UI，而是**麦克风槽位**：本仓库板卡走 `BoxAudioCodec` 的默认通道 0
   = TDM slot 0 = MIC1，而出厂固件把可用麦克风放在**槽 2（MIC2）**（§4）。

## 2. 实机基线（串口 115200 抓取）

| 项目 | 值 |
|---|---|
| App version | `2.1.0.5`（本仓库编译产物会是 `2.6.0`） |
| Project name | `xiaozhi` |
| Board | `UUID=5a29bbee-769b-4326-bd58-ea4ac973b5ed SKU=cyber-vocat` |
| 芯片 / Flash / PSRAM | ESP32-S3 (QFN56) rev v0.2、16 MB、8 MB OCT @80 MHz |
| 唤醒词 | `wn9_nihaoxiaozhi_tts`（"你好小智"），不是"你好喵伴" |
| UI | Brookesia：`BrookesiaDisplay/Font/Icon` + `VocatLvglPort`，9 个环形图标 |
| 激活 | `Activation done`，绑定走 `https://xiaozhi.cyberai.top/deviceBind/?m=<MAC>&...` |
| OTA | 检查 `http://ota.cyberai.top`（返回 `available: false`）；只在开机做一次 |
| MCP 工具 | 23 个，含 `self.sdcard.search_music_files`、`self.sdcard.Play_music_from_sdcard`、`self.music.search_music`、`self.echo_base.*`、`self.head_touch.*`、`self.gyro_shake.*`、`self.device.power_off` 等 |
| TF 卡 | `SD16G / SDHC / 20 MHz / capacity=7864320 / bus_width=1`，索引 `TF_METHOD: MP3 cache initialized, tracks=105` |
| 运行水位 | `free sram 44507 / largest 43008 / minimal 39163`，`free psram ≈6.83 MB` |
| 电池 | BQ 系列电量计（I2C 0x55），`92% 4226 mV`，充电 232 mA→6 mA |

## 3. 硬件事实

| 子系统 | 事实 |
|---|---|
| 电源 | `PG1_POWER_KEY_GPIO=40`（active 1）为开关机键；`PG2_HOLD_GPIO=38`（hold active 1、running 0）为自锁/断电；关机流程：长按 → 提示 2500 ms → PG2 输出 `PG2_POWER_OFF_PULSE_MS=200` 高脉冲。`POWER_KEY_LONG_PRESS_MS` 官方宏 2000，出厂固件日志实测 1500 |
| 音频 | 24 kHz 采集与播放，ES8311（放）+ ES7210（采），满双工 I2S，`AUDIO_INPUT_REFERENCE=true` |
| TDM 槽序 | ES7210 四麦克风经 TDM 输出，**槽序是 MIC1, MIC3, MIC2, MIC4**，与物理编号不同（`main/audio/codecs/box_audio_codec.cc:231-232` 注释） |
| 显示 | ST77916 QSPI 360×360 16bpp：PCLK 21 / CS 10 / D0 14 / D1 9 / D2 13 / D3 46 / RST 47 / TE 12 / BL 3 |
| 触摸 | CST816S，`TP_PORT=I2C_NUM_0`，INT 11，RST 与 LCD 共用 47；官方实现只有"点击切对话状态" |
| 头部触摸 | `TOUCH_PAD2=GPIO20`，active 1；触摸显示开心表情 |
| 说话指示灯 | `SPEAKING_LED_GPIO=19`，active 0 |
| IMU | QMA6100P（I2C 0x12），摇晃显示困惑表情 |
| TF 卡 | 1 线 SDMMC：CLK 48 / CMD 45 / D0 39，`MOUNT_POINT="/sdcard"`；**板级代码里没有任何挂载实现** |
| 底座 | `UART1_TX=41 / UART1_RX=42`，出厂固件用它跑"赛博底座"协议（角度/动作/校准/心跳） |
| BOOT 键 | 无独立按键，GPIO0 接 CH340 自动下载电路（`config.h:39` 注释） |
| 4G | ML307：TX 8 / RX 18 / EN 17 / UART2（本仓库未使用） |

## 4. 音频通道：上板第一件要验的事

本仓库板卡构造 codec 时只传基础参数（`main/boards/cyberai/cyber-vocat-v2.1/cyber_vocat.cc:1044-1051`，
`reference_gain_channel` 取默认 `-1`），`BoxAudioCodec::EnableInput()` 打开 4 通道输入后声明
`channel_mask = CH0`（有参考时再 `| CH1`），`Read()` 把 codec 采到的数据原样返回
（`main/audio/codecs/box_audio_codec.cc:217-236`、`:265-270`）。也就是说：

- **通道 0 = TDM slot 0 = MIC1**，通道 1 = slot 1 = MIC3（喇叭回采，做 AEC 参考）；
- 但同一块板子在出厂固件里的做法是把**可用麦克风取 slot 2（MIC2）**、参考取 slot 1，并在
  扫描时把 4 槽交错数据重排成 `mic, ref`（赛博另一套源码 `cyberkit` 的
  `main/audio/codecs/box_audio_codec.cc` 里显式写了 `dest[frame*2]   = tdm[frame*4+2];`
  `dest[frame*2+1] = tdm[frame*4+1];`）；其 README 明确写"V2.1 MIC1 几乎收不到人声，只用 MIC2"。

结论：本仓库这块板第一次上板必须验证**识别率**。若明显偏聋，改法是在板卡里换掉默认取槽方式
（最小改动：给 `BoxAudioCodec` 增加麦克风槽位参数，或板级派生一层重排），**不要**动通用
`channel_mask` 语义去影响其它板。

另外，出厂固件日志里长期存在
`Resampler output abnormal: mic=256 ref=513 expected=384 cap=832`、
`Resampler channel length mismatch: mic=256 ref=257 use=256`：24 kHz 回采被塞进 16 kHz 的 AEC 参考路径。
本仓库的 24 kHz 路径（`box_audio_codec` 的 `dma_frame_num` 缩放）是否也会触发，需要上板复测。

## 5. 分区表：本仓库 vs 实机（关键）

实机分区表由备份 `0x8000` 处解析（magic `aa50`）：

| 分区 | 实机 offset / size | 本仓库 `partitions/v2/16m.csv` |
|---|---|---|
| nvs | `0x009000` / 16 KB | `0x009000` / `0x4000` |
| otadata | `0x00D000` / 8 KB | `0x00D000` / `0x2000` |
| phy_init | `0x00F000` / 4 KB | `0x00F000` / `0x1000` |
| ota_0 | `0x020000` / **5120 KB** | `0x020000` / **4032 KB** |
| ota_1 | `0x520000` / **5120 KB** | `0x410000` / **4032 KB** |
| assets | `0xA20000` / **6016 KB** | `0x800000` / **8 MB** |

两者**不兼容**：本仓库固件的 assets 从 `0x800000` 开始，实机的 assets 在 `0xA20000`。

- 首次把本仓库固件烧到这台实机 → **必须整片烧 `0x0`**（bootloader + 分区表 + app + assets），
  或至少"分区表 + app + assets"三件一起；只写 `0x20000` 会读到错的 assets 偏移（无字体无表情）。
- 想留一条后路就别 `erase_flash`：整片备份见 §6。
- 整片镜像可用 `scripts/make_full_image.py` 生成（本仓库自带）。

## 6. 备份与恢复（已完成）

备份于 2026-11-23 在本机（Windows, COM5, CH340K）用 esptool v5.6.0 standalone 完成：

| 文件 | 大小 | SHA256 |
|---|---|---|
| `E:\gitea\backups\cybervoc-v2.1-app2.1.0.5-full-16m.bin` | 16,777,216 B | `E3E7CAB025D557996E8326C4F3E28D380001014A1BE86B064D8F222CF8676CC4` |
| `cybervoc-v2.1-nvs-0x9000.bin` | 16,384 B | `64C1890CCA881FAE7D4286075D99C47EF2C745E4B630BBEA9087C3A127B9F40E` |
| `cybervoc-v2.1-otadata-0xd000.bin` | 8,192 B | `8BA3B110139F45443D4F268D1A3373EF99A1718B71D51664531B83EE2D4B91A3` |
| `efuse_summary.txt` / `read-flash.log` / `verify-flash.log` | 35 KB / 707 KB / — | — |

第一次 `verify_flash` 报 `Verification failed (digest mismatch)`：重读整片后逐字节比对，只有 NVS 工作区
`0x00C02F–0x00C85B`（2093 B）不同——两次读取之间设备启动过并改写了 NVS，其余全部一致，备份可用。
**结论：整片校验对"运行中的设备"必然失败，验证要按分区看，或先让设备停在下载模式。**

eFuse 现状（只读，未做任何写操作）：`SECURE_BOOT_EN=False`、`SPI_BOOT_CRYPT_CNT=Disable`、
`DIS_DOWNLOAD_MODE=False`、`SECURE_BOOT_KEY_REVOKE0/1/2=False` → 没有一次性熔断，可以自由刷写。

恢复：

```bash
esptool --chip esp32s3 -p <PORT> -b 921600 write_flash 0x0 cybervoc-v2.1-app2.1.0.5-full-16m.bin
esptool --chip esp32s3 -p <PORT> -b 921600 verify_flash 0x0 cybervoc-v2.1-app2.1.0.5-full-16m.bin
```

授权不在 flash 里：绑定按 MAC、`Serial-Number` 来自 eFuse USER_DATA，刷写不会掉授权；`espefuse` 的写操作才会。

## 7. 与出厂固件 2.1.0.5 的能力差距

| 能力 | 出厂 2.1.0.5 | 本仓库 `cyberai/cyber-vocat-v2.1` |
|---|---|---|
| Wi-Fi 对话、唤醒、AEC | 有 | 有（上游通用实现） |
| 24 kHz 采集/播放 | 有 | 有（`AUDIO_INPUT/OUTPUT_SAMPLE_RATE 24000`） |
| ST77916 显示 + 表情动画 | 有（Brookesia） | 有（Emote animation style） |
| CST816S 触摸 | 点按切对话 | 点按切对话 |
| 头部触摸 / 摇晃互动 | 有 | 有 |
| 电量显示 / 低电关机 | 有 | 有（`LOW_BATTERY_SHUTDOWN_*`） |
| **TF 卡挂载 + MP3 索引** | 有（105 tracks） | **无**（`sdcard`/`sdmmc`/`fatfs`/`mp3` 在板卡源码里 0 命中） |
| **对话点播 TF 卡音乐** | 有（`self.sdcard.*`） | **无** |
| 板级 MCP 工具 | 23 个 | 无（核心只有通用工具；板级可用 `McpServer::GetInstance().AddTool()` 注册，参考 `main/boards/chd/.../chd_esp32s3_box_board.cc:704`） |
| Brookesia 环形菜单 / 手势 | 有 | 无（要做需另写 `LcdDisplay` 子类） |
| 赛博底座 UART 协议 | 有 | 无 |
| 赛博授权链 | 有 | 无（`CONFIG_OTA_URL` 默认 `https://api.tenclass.net/xiaozhi/ota/`） |

> 注意：换了本仓库固件就不再走赛博的激活/OTA 体系。语音服务要自己指定 OTA 地址（xiaozhi 官方或自建），
> 设备不会再被 `xiaozhi.cyberai.top` 认领。这是"能自己改代码"的代价，先想清楚再刷。

## 8. 本仓库现状与构建

- 板卡目录：`main/boards/cyberai/cyber-vocat-v2.1/`（`config.h` / `config.json` / `cyber_vocat.cc` 1067 行 / `README.md`），
  公共件在 `main/boards/cyberai/common/`（`dma_panel.h`、`low_battery_shutdown.h`）。
- 构建链已通：`config.json`(`type: cyber-vocat-v2.1`) → `scripts/build.py` → `main/Kconfig.projbuild`
  （`BOARD_TYPE_CYBERAI_VOCAT_V2_1`）→ `main/CMakeLists.txt` → 板卡源码。
- 本机**没有 ESP-IDF**（`IDF_PATH` 未设置、无 `idf.py`），编译必须在装了 IDF `release/v6.1` 的机器上做：

```bash
python3 scripts/build.py cyberai/cyber-vocat-v2.1 --name cyber-vocat-v2.1 --language zh-CN
```

- CI 目前只编四块维护板（`.github/workflows/build.yml` 的 `MAINTAINED_BOARDS`），这块板**不在其中**；
  `FORK.md` 的"维护的板卡"表也没有它。

## 9. 开发路线（建议顺序）

**阶段 0 — 能编出来**：装 IDF 6.1，`scripts/build.py` 出 `merged-binary`，`python3 -m unittest discover -s scripts/tests -v`。

**阶段 1 — 官方板先跑通**（整片烧，验四件事，别急着改代码）：
1. 服务连通：OTA/激活走哪个地址、能不能对话（换服务器要改 `CONFIG_OTA_URL`）；
2. **识别率**（§4 麦克风槽位）；
3. 显示方向/颜色/TE 与 12 MHz QSPI 是否稳定；
4. 长按关机时长（2000 ms 宏 vs 实机 1500 ms 手感）。

**阶段 2 — TF 卡**：板级 `InitializeSdCard()`（1 线、20 MHz、`SDMMC_SLOT_FLAG_INTERNAL_PULLUP`、
挂载失败重试 + 声光提示），扫描结果缓存到 PSRAM（避免每次点播都全盘遍历），
再注册 `self.sdcard.search_music_files` / `self.sdcard.Play_music_from_sdcard`。

**阶段 3 — 本地播放**：MP3 解码（helix）→ 送入 `AudioService` 播放路径，注意 §10 的三个坑；
加"下一首/上一首/停止"工具，否则点播体验不如手动播放器。

**阶段 4 — 可选**：手势/环形菜单、赛博底座 UART、低电/充电动画细化。

## 10. 三个必须记住的坑（都有实测证据）

1. **MCP 工具在主事件循环上执行**。本仓库 `main/mcp_server.cc:745-760` 与赛博那套一样：
   `Application::GetInstance().Schedule([...]{ auto result = tool->Call(...); ReplyResult(...); })`，
   工具不返回就不回包、主循环也停摆。出厂固件实测：说了"播放TF中的故事"，模型选了
   `self.music.search_music`（在线音乐），该工具去连第三方 `n.shybot.top:80`（DNS+TCP 全失败），
   结果 `STATE: speaking` 卡 **81.45 秒**、编码队列 100% 满（`Encode queue full, drop oldest PCM frame
   (encode=1 send=40)`，期间设备完全"聋"），直到 `TTS stop without UDP audio packets` 才回落。
   → 板级工具里**不要**做扫描/网络/等待；耗时活儿另起任务，先回 `{"success":true,"message":"正在查找"}`。
2. **工具描述会打架**。`self.music.search_music` 的描述写死了"无论什么时候播放音乐都只用该工具"，
   SD 卡工具只是弱条件，模型十次有几次选错。板级工具的描述要写得更"硬"。
3. **I2C 总线是共享的**（CST816S、QMA6100P、ES8311、ES7210、电量计）。出厂固件开机约 20 分钟后开始
   周期性超时，60 分钟窗口累计 `i2c.master: transaction timeout` 905 次、`bq27220 ... ESP_ERR_TIMEOUT`
   447 次（另一次 60 分钟窗口只有 59 / 75 次，速率随状态波动）。本仓库可参考
   `main/boards/espressif/esp-vocat/README.md` 的 `Peripheral I2C error handling`：重试 3 次、单次 50 ms
   超时、失败退避、mutex 保护的"最后成功快照"，触摸失败时跳过手势。

## 11. 参考实现（另一套源码 `cyberkit`，基线 2.0.5.6，不是这块板的官方代码）

| 文件 | 作用 | 可借鉴 / 风险 |
|---|---|---|
| `main/boards/CyberVoc-Board-V2_1/audio_analysis.cc` | DOA（esp_apa，2 mic）与底座角度联动 | 槽位常数与 TDM 槽序耦合，移植时先对齐 §4 |
| `.../base_control.cc` | 底座 UART1 协议 + 心跳 + 校准 | 协议常量在 `components/espressif2022__cybervoc_base_control`，闭源二进制协议需实测 |
| `.../ui_bridge.cc` | 环形菜单页面、边缘起手滑动 | 依赖 Brookesia 风格页面，不适合直接搬 |
| `boards/common/TF_menthod.cc` | TF 扫描/索引/匹配 | 匹配是单向"文件名含用户串"，多词必失败；每首 16 KB 探测首帧，慢 |
| `boards/common/esp32_music.cc` | helix 解码 + 流式播放线程 | `WaitForThreadsToFinish()` 无超时 join，容易卡主循环 |
| `main/mcp_server.cc` | `self.sdcard.*` 工具 | 与 §10.1 同样的阻塞写法，别照抄 |

## 12. 下一步清单

- [x] 装 IDF `release/v6.1`，编出 `cyber-vocat-v2.1` 整片镜像（见 §13，已成功）
- [ ] 整片烧写并验 §9 阶段 1 的四件事，重点确认麦克风槽位
- [ ] 确认服务和 OTA 地址策略（继续用赛博 / 换官方 / 自建）
- [ ] TF 卡挂载 + 索引缓存
- [ ] 本地播放 + `self.sdcard.*` 工具（异步、带超时）
- [x] 把这块板加入 CI `MAINTAINED_BOARDS` 与 `FORK.md` 维护表

## 13. 编译环境与已验证的构建（2026-11-23）

编译机：`ssh xz`（Ubuntu 6.8 / 6 核 / 15 GB / 95 GB 可用），`/data/root/esp/esp-idf` = **ESP-IDF v6.1**（`idf.py --version` → `v6.1-dirty`），
工具链 `xtensa-esp-elf 15.2.0_20251204`。

```bash
cd /root/build/esp32-xiaozhi
. /root/esp/esp-idf/export.sh
export IDF_SKIP_CHECK_SUBMODULES=1
python3 scripts/build.py cyberai/cyber-vocat-v2.1 --name cyber-vocat-v2.1 --language zh-CN   # 实测退出码 0
```

产物（`build/`）：

| 文件 | 大小 |
|---|---|
| `xiaozhi.bin`（app） | 2,433,680 B |
| `merged-binary.bin`（整片） | 11,352,986 B |
| `bootloader/bootloader.bin` | 16,736 B |
| `partition_table/partition-table.bin` | 3,072 B |

> 官方 `v2.6.0_cyberai-cyber-vocat-v2.1.zip` 里的 `merged-binary.bin` 是 11,353,086 B，只差 100 B —— 结构对得上，说明构建链没跑偏。

### 13.1 这台 IDF 检出特有的三个坑（不是项目问题）

1. `.gitmodules` 里的子模块 URL 全指向 `gitee.com`，而该机**访问 gitee 需要凭据**、GitHub 反而通：
   `fatal: could not read Username for 'https://gitee.com'` → `Git submodule init failed for components/bt/controller/lib_esp32`。
   解法：`git -c url.https://github.com/.insteadOf=https://gitee.com/ submodule update ...`。
2. 多数子模块**内容本来就在**（`esp_wifi/lib` 35 MB、`mbedtls/mbedtls` 55 MB…），但 `git submodule status` 仍标 `-`，
   于是 CMake 逐个 clone 时报 `destination path ... already exists and is not an empty directory`。
   解法：`export IDF_SKIP_CHECK_SUBMODULES=1`（`tools/cmake/git_submodules.cmake:16` 支持）。
3. 若真的缺内容（本机曾缺 `components/heap/tlsf`、`components/lwip/lwip`），用 HTTPS tarball 按 pinned SHA 补最快：
   `https://codeload.github.com/espressif/tlsf/tar.gz/2867f6883a12920b1969ff9624c0ab0e4185c2ce`、
   `https://codeload.github.com/espressif/esp-lwip/tar.gz/c6f2f878e7b0f86033214b85547d579be43351e3`，
   解到对应目录（`--strip-components=1`）即可，无需 git 元数据。
   ⚠️ 别对子模块目录执行 `rm -rf` 后再 `submodule update`：残留的 `submodule--helper` 进程会再删一次，形成"刚补好又没了"的假象。

配置阶段那批 `Private include directory ... belongs to component esp_wifi but is being used by component wpa_supplicant`
是 IDF 6.1 的**校验警告**，可忽略。

### 13.2 烧写注意（分区表不一致）

本仓库 `partitions/v2/16m.csv` 与出厂固件不同：

| 分区 | 本仓库 | 实机（备份解析） |
|---|---|---|
| `ota_0` | 0x20000 / 4032 KB | 0x20000 / **5120 KB** |
| `ota_1` | 0x410000 / 4032 KB | **0x520000** / 5120 KB |
| `assets` | 0x800000 / 8 MB | **0xA20000 / 6016 KB** |

两条路，选一条：

- **整片烧** `0x0`（`merged-binary.bin`）——分区表跟着换成本仓库的，assets 也一起写；代价是 NVS 被清（WiFi/MQTT 凭证要重配，服务端仍按 MAC 认设备）。
- **对齐分区表后再只烧 app** —— 把 `partitions/v2/16m.csv` 改成实机的 `ota_0 5120K / ota_1 0x520000 / assets 0xA20000 6016K`，
  这样 `idf.py -p COM5 app-flash` 就够了，NVS 与已激活状态全保留。开发期推荐这条。

## 14. 麦克风槽位修复与首轮实机延迟实测（2026-11-23）

### 14.1 修复内容（只对本板生效）

`main/audio/codecs/box_audio_codec.cc`，全部包在 `#if CONFIG_BOARD_TYPE_CYBERAI_VOCAT_V2_1` 内，不影响其他板卡：

1. 新增常量 `kV2_1MicTdmSlot = 2`、`kV2_1RefTdmSlot = 1`，并加一个 `std::vector<int16_t>` 复用缓冲；
2. `EnableInput()`：`channel_mask` 补上 slot2/slot3（ES7210 四槽全采），输入增益改用 `esp_codec_dev_set_in_gain()`
   下发到**全部通道**（原来只给通道 0，也就是那颗坏麦）；
3. `Read()`：2 通道模式下按 4 槽读回，再重排为 `dest[frame*2] = tdm[frame*4+2]`（MIC2 主麦）、
   `dest[frame*2+1] = tdm[frame*4+1]`（MIC3 喇叭回采作 AEC 参考）。

依据：ES7210 TDM 槽序是 `MIC1, MIC3, MIC2, MIC4`（`main/audio/codecs/box_audio_codec.cc` 原注释与 `cyberkit` 的
`kMrMicTdmSlot=2 / kMrRefTdmSlot=1` 一致），而本板 MIC1 几乎收不到人声。

**实测验证**（app 单烧 `0x20000`，SHA256 `2CB671392D8EA095ECE48A8A0AE38F9A3F8F0873F3BD57BF5F61302AC5F2353E`）：

```
修复前：I (6518) I2S_IF: TDM: RX, data_bit: 16, slot_bit: 16, ws_width: 32, total_slot: 4, slot_mask: 0x3
修复后：I (6639) I2S_IF: TDM: RX, data_bit: 16, slot_bit: 16, ws_width: 32, total_slot: 4, slot_mask: 0xf
```

修复后第一次测试即成功唤醒：`I (315139) Application: Wake word detected: 你好小智 (state: 3)`。

### 14.2 串口抓包的坑（重要）

**不要用 DTR 拉低当复位**：这块板的 CH340K DTR/RTS 经三极管接 `ESP_BOOT`/`CHIP_PU`，直接 `DtrEnable=true→false`
等于把 IO0 按住，芯片停在 ROM 下载模式，一个字都不输出（现象是"端口正常打开但 0 字节"）。

正确姿势：先让 esptool 做标准硬复位，再立刻打开端口读：

```powershell
esptool.exe --chip esp32s3 -p COM5 -b 921600 --after hard_reset read_mac
# 紧接着 New-Object System.IO.Ports.SerialPort COM5,115200,None,8,one
#          $p.DtrEnable = $false; $p.RtsEnable = $false; $p.Open()
```

另：esptool 的 PyInstaller 版在本机需要 `$env:TEMP/$env:TMP` 指到可写目录（如 `E:\gitea\.tmp-esptool`），
否则报 `[PYI-...:ERROR] Could not create temporary directory!`。

### 14.3 首轮对话延迟实测（走赛博云，单位 ms）

| 环节 | 日志锚点 | 耗时 |
|---|---|---|
| 唤醒命中 → 进入聆听 | `315139 Wake word detected` → `315459 listening` | 320 |
| 唤醒词音频编码上行 | `315449 Encoded wake word into 33 packets in 316 ms` | 316 |
| 进入聆听 → 服务端回 ASR | `315459` → `315489 >> 你好小智` | 30 |
| 话音刚落 → 服务端判定回合结束并开始说话 | `315489` → `317999 listening->speaking` | 2510 |
| 开始说话 → TTS 首句文本 | `317999` → `319349 << 你好呀！` | 1350 |
| 异常样本：已出 ASR → 才开始说话 | `370009 >> 嗯` → `401339 speaking` | **31330** |

结论：**设备端合计只有约 350 ms，其余 3.9 s 全在服务端**（静音判停 + LLM + TTS 首包）；
另有一次 31 s 的异常空等，是服务端迟迟不结束回合，期间设备一直正常在听。

### 14.4 协议方言不一致（延迟的潜在来源）

赛博服务端会下发官方固件不认识的消息，设备直接丢弃：

```
W (315329) Application: Unknown message type: juphoon.config
W (315489) Application: Unknown message type: listen
```

`juphoon.config` 是菊风 RTC 的配置，`listen` 在官方协议里是**设备→服务端**的消息，这里由服务端下发。
官方客户端很可能用这些消息做更快的回合切换；现在这条捷径是断的。同时日志显示
`MQTT: Server has no udp_probe support; using legacy UDP channel`，即服务端不支持 UDP 探测，走的是旧通道。

### 14.5 待做实验

- **T（未执行，等确认）**：让 `main/ota.cc` 忽略 NVS 里缓存的 `ota_url`、强制使用 `CONFIG_OTA_URL`（`api.tenclass.net`），
  重新激活到官方服务，用同一套抓包对比延迟。可分辨"赛博链路慢"还是"设备/固件慢"；
  代价是离开赛博授权链（固件与 NVS 均有整片备份，可回滚）。
- 唤醒召回率统计：修复后需要用连续 10 次以上唤醒样本计算命中率，目前只有 1 次成功样本。
