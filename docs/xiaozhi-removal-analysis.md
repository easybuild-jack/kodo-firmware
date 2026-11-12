# xiaozhi 相关元素移除分析（kodo-firmware）

本文档盘点仓库中所有与上游 xiaozhi-esp32（小智）相关的元素，逐项说明：它是什么、在哪里、能否移除或改名、风险是什么。

适用范围：kodo-firmware 只使用 `main/boards/bread-compact-wifi-s3cam` 一块板子，用 ESP-IDF v6.1 的 `idf.py build` 编译，后端为独立仓库 kodo-server，后端沿用 xiaozhi 的 WebSocket / MQTT+UDP / MCP / OTA 协议。

> 原则：只要 kodo-server 可能依赖、或会改变设备运行行为的元素，一律**不改**，只在本文档中给出建议。本次 PR 只做了零行为变化的清理，见文末“本次已执行的清理”。

风险等级说明：

- **高**：会破坏与 kodo-server 的通信、OTA、已保存配置，或改变设备行为。
- **中**：不影响协议，但会改变用户可见行为、构建产物名称或工具链，需要协调修改。
- **低**：纯文档、标签、注释或不参与本板构建的文件。

---

## 1. 与 kodo-server 的协议契约（禁止修改）

这些字段由服务端解析，名字里带不带 xiaozhi 都属于协议，改动后服务端将无法识别设备。若将来要改，必须先在 kodo-server 中做兼容（新旧同时接受），再改固件，并做 OTA 灰度。

| 元素 | 位置 | 说明 | 能否修改 | 风险 |
|---|---|---|---|---|
| OTA 路径 `/xiaozhi/ota/` | `main/boards/bread-compact-wifi-s3cam/config.json`（`CONFIG_OTA_URL="http://192.168.8.11:8000/xiaozhi/ota/"`）；实际生效值在本地 `sdkconfig` 的 `CONFIG_OTA_URL`，或 NVS `wifi/ota_url` | 设备启动检查版本、获取 websocket/mqtt 配置的入口 | **不能** | 高：kodo-server 按此路径提供 OTA 接口 |
| 激活接口 `<OTA_URL>/activate` | `main/ota.cc` `Ota::Activate()` 在 OTA URL 后拼接 `activate` | 设备激活（`/xiaozhi/ota/activate`） | **不能** | 高 |
| OTA 请求头 `Activation-Version`、`Device-Id`、`Client-Id`、`Serial-Number`、`User-Agent`、`Accept-Language`、`Content-Type` | `main/ota.cc` `Ota::SetupHttp()` | 服务端用 `Device-Id`（MAC）/`Client-Id`（UUID）识别设备；`User-Agent` 为 `BOARD_NAME/版本`，即 `bread-compact-wifi-s3cam/2.5.3` | **不能** | 高 |
| OTA 请求体（系统信息 JSON） | `main/boards/common/board.cc` `GetSystemInfoJson()`，`wifi_board.cc` `GetBoardJson()` | 含 `application.name`（= 工程名 `xiaozhi`，见第 2 节）、`board.type`（= `bread-compact-wifi-s3cam`）、`mac_address`、`uuid` 等 | **不能** | 高 |
| OTA 响应字段 `firmware`、`websocket`、`mqtt`、`activation`、`server_time` | `main/ota.cc` `CheckVersion()` | 服务端下发 | **不能** | 高 |
| WebSocket 握手头 `Authorization`、`Protocol-Version`、`Device-Id`、`Client-Id` | `main/protocols/websocket_protocol.cc` | 服务端鉴权与识别 | **不能** | 高 |
| `hello` 消息字段 `type`、`version`、`transport`（`websocket` / `udp`）、`features`、`audio_params` | `websocket_protocol.cc`、`mqtt_protocol.cc` | 两种传输共用的握手语义 | **不能** | 高 |
| 消息类型 `hello`、`listen`、`abort`、`tts`、`stt`、`llm`、`mcp`、`system`、`custom`、`goodbye` 等 | `main/protocols/protocol.cc`、`main/protocols/mqtt_protocol.cc`、`main/application.cc`；文档 `docs/websocket.md`、`docs/mqtt-udp.md` | 共享协议语义 | **不能** | 高 |
| MCP 工具名：`self.get_device_status`、`self.audio_speaker.set_volume`、`self.screen.set_brightness`、`self.screen.set_theme`、`self.screen.get_info`、`self.screen.snapshot`、`self.screen.preview_image`、`self.camera.take_photo`、`self.get_system_info`、`self.reboot`、`self.upgrade_firmware`、`self.assets.set_download_url`、`self.set_press_to_talk`，以及 Kodo 自定义的 `self.light.set`、`self.light.get_state`、`self.device.sleep` | `main/mcp_server.cc`、`main/boards/common/*.cc`、`main/boards/bread-compact-wifi-s3cam/*.cc` | LLM 通过工具名调用设备能力，服务端/提示词可能写死工具名 | **不能** | 高 |
| NVS 命名空间与键：`wifi`（含 `ota_url`）、`websocket`、`mqtt`、`board`、`display`、`audio`、`assets`、`vendor`、`kodo_light` | 各 `Settings("...")` 调用处 | 持久化 API，改名会丢失已保存配置，需要迁移代码 | **不能**（除非写迁移） | 高 |
| 板子身份 `BOARD_TYPE` / `BOARD_NAME` = `bread-compact-wifi-s3cam` | `config.json` 的 `type`、板子目录名 | 上报给服务端，参与 OTA 固件匹配 | **不能**（目录名也不要改） | 高 |
| 固件版本 `PROJECT_VER "2.5.3"` | 根目录 `CMakeLists.txt` | OTA 版本比较依据 | 可以按发版规则递增，但不是 xiaozhi 品牌问题 | 中 |

说明：以上判断基于固件侧代码。kodo-server 源码不在本仓库，标为“不能”的项目在修改前都应先到 kodo-server 中确认实际依赖。

---

## 2. 构建与工程标识

| 元素 | 位置 | 说明 | 能否修改 | 风险 |
|---|---|---|---|---|
| 工程名 `project(xiaozhi)` | 根目录 `CMakeLists.txt` | 决定产物名 `build/xiaozhi.bin` / `xiaozhi.elf`，以及写入固件的 `esp_app_desc.project_name`；后者作为 `application.name` 上报给服务端 | 技术上可以改成 `kodo`，**但本次不改** | 中-高：服务端可能校验 `application.name`；`scripts/release.py`（检查 `project == "xiaozhi"`）、`scripts/versions.py`、`docker/firmware-builder` 写死了 `xiaozhi.bin`；烧录/OTA 发布脚本需要同步修改 |
| Kconfig 菜单标题 `menu "Xiaozhi Assistant"` | `main/Kconfig.projbuild` 第 1 行 | 只是 menuconfig 中的显示文字，不影响任何 `CONFIG_*` 符号 | **可以安全改名**（如 `Kodo Assistant`） | 低：只影响 sdkconfig 中的注释行；但几十份板子 README 和 `docs/custom-board*.md` 引用该名字，建议与文档一起改 |
| Kconfig 符号 `CONFIG_XIAOZHI_*`（`XIAOZHI_CAMERA_ALLOW_JPEG_INPUT`、`XIAOZHI_CAMERA_MIRROR_CONFIGURED`、`XIAOZHI_CAMERA_HMIRROR/VFLIP`、`XIAOZHI_ENABLE_HARDWARE_JPEG_*`、`XIAOZHI_ENABLE_CAMERA_DEBUG_MODE`、`XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE`、`XIAOZHI_NETWORK_*`、`XIAOZHI_ETH_*` 等） | `main/Kconfig.projbuild`，被 `esp32_camera.cc`、`esp_video.cc`、`image_to_jpeg.cpp`、`jpeg_to_image.c`、`system_info.cc`、`CMakeLists.txt`、`config.json` 引用 | 本板依赖 `CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT=y` | **不建议** | 高：直接改名会让现有 sdkconfig 中的值被静默丢弃（例如 JPEG 输入被关掉，摄像头行为改变）。若一定要改，必须同时在 `main/sdkconfig.rename` 中添加旧名→新名映射，并改 `build.py` 和所有引用 |
| 宏 `XIAOZHI_I2S_PORT` | `main/audio/audio_codec.h`，`no_audio_codec.cc` 等 | 内部宏 | 可以改名，但收益很小且要动核心音频代码 | 低（纯改名），不建议为品牌单独改 |
| 组件 `78/xiaozhi-fonts` | `main/idf_component.yml`、`main/CMakeLists.txt` | 上游字体/表情资源组件名 | **不能**改名（第三方组件标识） | 高：改了组件无法解析 |
| 组件 `78/esp-wifi-connect`、`78/esp-ml307` 等 | `main/idf_component.yml` | 上游维护的第三方组件，配网网页等可能带有 xiaozhi 风格界面 | 不能在本仓库改（`managed_components/` 禁止手改） | 高 |
| esp-sr 唤醒词模型名 `wn9_nihaoxiaozhi_tts` / `nihaoxiaozhi` | `scripts/build.py`、`docker/firmware-builder` | esp-sr 上游模型标识；本板使用 MultiNet `mn7_cn` 自定义唤醒词“小睿同学”，不用这个模型 | **不能**改名（上游库标识） | 高 |
| `main/sdkconfig.rename` 中的 `XIAOZHI_*` 映射 | `main/sdkconfig.rename` | 兼容旧 sdkconfig 的迁移表（P4-NANO 以太网） | 保留；只有删除以太网支持时才一起删 | 低 |

---

## 3. 设备上用户可见的字符串

| 元素 | 位置 | 说明 | 能否修改 | 风险 |
|---|---|---|---|---|
| 配网热点 SSID 前缀 `"Xiaozhi"` | `main/boards/common/wifi_board.cc`（`config.ssid_prefix = "Xiaozhi"`） | 进入配网模式时手机看到的热点名 `Xiaozhi-XXXX` | 可以改成 `Kodo`，**但属于用户可见行为变化，本次不改** | 中：说明书、用户习惯、可能的配网 App 需同步；`common` 代码被所有板子共享，建议改为板级可配置（例如由板子传入前缀）而不是直接改共享默认值 |
| BluFi 设备名 `"Xiaozhi-Blufi"` | `main/boards/common/blufi.cpp` | 仅在启用 `CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING` 时编译；本板默认未启用 | 可以改，本板当前不受影响 | 低（本板），中（若启用 BluFi） |
| Kconfig `OTA_URL` 默认值 `https://api.tenclass.net/xiaozhi/ota/` | `main/Kconfig.projbuild` 第 5 行 | 上游官方服务器。若本地 sdkconfig 没有覆盖 `CONFIG_OTA_URL`，设备会连到小智官方服务器 | 建议改为 kodo-server 地址，**本次不改** | 中-高：改变全新配置时的默认服务器，属于行为变化；当前实际地址由开发者本地 sdkconfig 决定（`config.json` 的 `sdkconfig_append` 只有 `build.py` 会应用） |
| 表情样式中的 `xiaozhi.me` 判断 | `main/display/emote_display.cc` | 仅 Emote 显示样式使用；本板使用 `KodoFaceDisplay`，不走这段代码 | 可删，但属于共享显示代码 | 低 |
| 中文/英文语言包 | `main/assets/locales/*/language.json` | 已检查，不含 “xiaozhi / 小智” 字样 | — | — |

---

## 4. 仓库文档、工具与 CI

| 元素 | 位置 | 说明 | 能否修改 | 风险 |
|---|---|---|---|---|
| 主 README | `README.md`、`README_zh.md`、`README_ja.md` | 上游小智项目介绍、板子列表、图片链接（`docs/v0`、`docs/v1`） | 建议重写为 Kodo 项目说明 | 低：纯文档；重写后 `docs/v0`、`docs/v1` 中仅被 README 引用的图片可一并删除 |
| 协议文档 | `docs/websocket*.md`、`docs/mqtt-udp*.md`、`docs/mcp-protocol*.md`、`docs/mcp-usage*.md` | 是 kodo-server 与固件之间协议的参考，示例文本中有 “Hi XiaoZhi” 等字样 | **保留**；可以替换示例里的称呼，但不要改字段名 | 低（示例文字），高（字段） |
| 板子开发与发布文档 | `docs/custom-board*.md`、`docs/releasing.md`、`docs/glyph-push*.md`、`docs/blufi*.md`、`docs/code_style*.md`、`docs/notify.md`、`docs/audio-codec-input-audit.md` | 上游开发文档，`releasing.md` 指向 `78/xiaozhi-esp32` 仓库 | 可按需删改 | 低 |
| `AGENTS.md` | 仓库根目录 | 写的是 “XiaoZhi ... firmware” 与 `build.py` 流程 | 建议补充 Kodo 的 `idf.py build` 约定 | 低 |
| `LICENSE` | 仓库根目录 | 上游 MIT 许可证（Shenzhen Xinzhi Future Technology Co., Ltd. 与贡献者） | **不能删除**；MIT 要求保留原版权声明，可以追加 Kodo 自己的版权行 | 高（法律） |
| GitHub 模板与工作流 | `.github/ISSUE_TEMPLATE/*`、`.github/SUPPORT.md`、`.github/workflows/build.yml`、`release.yml`、`test-mcp.yml` | 上游的问题模板和多板子 CI；`build.yml` 只在推送/PR 到 `main` 时触发，产物名 `xiaozhi_*` | 可删改；若保留 CI 建议改成只构建本板 | 低 |
| 固件构建 Docker | `docker/firmware-builder/` | 上游多板子云端构建镜像，依赖 `build.py` 和 `xiaozhi.bin` | 不用可删除 | 低 |
| 构建/发布脚本 | `scripts/build.py`、`scripts/release.py`、`scripts/versions.py`、`scripts/ci/`、`scripts/tests/` | 多板子构建工具与其主机测试；所有者使用 `idf.py build`，不使用 `build.py` | 可保留；删除其他板子时必须同步修改（见第 5 节） | 中 |
| 资源工具 | `scripts/ogg_converter/xiaozhi_ogg_converter.py`、`scripts/p3_tools/`、`scripts/Image_Converter/`、`scripts/spiffs_assets/`、`scripts/mp3_to_ogg.sh`、`scripts/download_github_runs.py`、`scripts/audio_debug_server.py` | 独立的手动工具，不参与构建 | 可按需删除或改名 | 低 |

---

## 5. 其他板子目录（建议单独 PR 处理）

`main/boards/` 下除 `common/` 和 `bread-compact-wifi-s3cam/` 外还有 51 个顶层目录（厂商目录下含多块板子，共 166 份 `config.json`、约 840 个文件、7 MB）。`main/CMakeLists.txt` 只通过 `file(GLOB)` 收集所选板子目录的源码，所以这些目录**不参与本板编译**，删除它们不会改变本板固件。

本次没有删除，原因是它们与多处构建配置耦合，删除需要成套修改，超出“零行为变化清理”的范围：

1. `main/Kconfig.projbuild`：`BOARD_TYPE` 选项列表，以及依赖具体板子的选项（`RESPEAKER_*`、`XIAOZHI_NETWORK_TYPE`/以太网菜单、`ESP32_S3_LCD_EV_BOARD_VERSION`、`WK_*`、`DISPLAY_OLED_TYPE`、`DISPLAY_CHD_*`、`DISPLAY_ESP32S3_*`、`JC4827W543_EXTERNAL_AMP`、`TAIJIPAI_S3_CONFIG`、`USE_EMOTE_MESSAGE_STYLE`、`USE_DEVICE_AEC` 的 depends 列表等）。
2. `main/CMakeLists.txt`：板子 `elseif` 链，以及 ESP-VoCat 资源暂存、ESP-HI 资源下载、M5Stack/Spot/P4-EV 额外依赖、Maza 墨水屏源码等板级分支。
3. `scripts/build.py`：会直接解析 Kconfig 中的 `XIAOZHI_NETWORK_TYPE`、`DISPLAY_OLED_TYPE`、`DISPLAY_LCD_TYPE`、`USE_EMOTE_MESSAGE_STYLE`、`USE_DEVICE_AEC`，找不到就抛异常；另有若干写死的板子集合。
4. `scripts/tests/`：`test_build.py`、`test_ci_selection.py`、`test_esp_vocat_i2c.py`、`test_rgb_matrix_emotion.py`、`test_power_save_timer.py` 等引用了具体板子。
5. `scripts/ci/representative-variants.json`、`.github/workflows/build.yml`。
6. `main/idf_component.yml`：有两条规则引用 `$CONFIG{BOARD_TYPE_WAVESHARE_ESP32_C5_*}` / `C6_*`。**修改 manifest 会让组件管理器重新求解依赖，本地 `dependencies.lock`（未提交）可能升级组件版本，从而改变固件**，必须在构建前后对比组件版本。
7. 其他芯片的 `sdkconfig.defaults.esp32*`（esp32、c3、c5、c6、p4、s31）和只被其他板子使用的分区表（`partitions/v1/*`、`partitions/v2/4m.csv`、`8m.csv`、`32m.csv`、`16m_c3.csv`）。
8. README 与 `docs/custom-board*.md` 中指向这些目录的链接。

建议做法（单独 PR，按提交拆分，便于回滚）：

1. 先在本地保存一次基线：`idf.py build` 后备份 `sdkconfig`、`dependencies.lock`、`build/xiaozhi.bin`、`build/generated_assets.bin`。
2. 删除板子目录，并按上面清单同步修改 Kconfig、CMake、`build.py`、测试、CI。
3. 不改 `main/idf_component.yml`，或改完后确认 `dependencies.lock` 中各组件版本不变。
4. 重新 `idf.py reconfigure && idf.py build`，确认 `sdkconfig` 只少了被删板子的选项，`generated_assets.bin` 字节一致，固件函数级差异为零。
5. 运行 `python3 -m unittest discover -s scripts/tests -v`。

`main/boards/common/` 中只有其他板子使用的公共代码（`ml307_board.cc`、`nt26_board.cc`、`dual_network_board.cc`、`axp2101.cc`、`sy6970.cc`、`knob.cc`、`rndis_board.cc`、`esp_video.cc`、`xpower.h`、`lamp_controller.h` 等）目前**仍被编译进本板固件**（`main` 组件使用 `WHOLE_ARCHIVE`）。从源码列表中移除它们会改变固件内容，需要逐个确认没有静态初始化副作用后再做，建议放在删除板子之后的另一个 PR。

---

## 6. 本次已执行的清理（零行为变化）

| 类别 | 内容 |
|---|---|
| 改名 | `xiaorui_face_display.{cc,h}` → `kodo_face_display.{cc,h}`，类 `XiaoruiFaceDisplay` → `KodoFaceDisplay`；`docs/xiaorui-face-design/` → `docs/kodo-face-design/` |
| 死代码 | 本板 `LampController` 从未构造（GPIO14 用于灯环），删除永远为 `nullptr` 的 `lamp_` 成员、对应 include、注释掉的初始化代码和两处空指针判断分支 |
| 未引用资源 | `main/assets/common/wozai.ogg`（唤醒应答使用的是 `wozaime` / `shike` / `laila`） |
| 未引用文件 | `partitions/v1/16m_custom_wakeword.csv`、`16m_echoear.csv`、`4m_esp-hi.csv`；`docs/v0/` 中 4 张、`docs/v1/` 中 1 张未被任何文档引用的图片 |
| 文档 | 更新本板 `README.md`（Kodo 硬件说明、正确的 menuconfig 路径、`idf.py` 构建注意事项）；新增本文档 |

唤醒词配置（`CONFIG_USE_CUSTOM_WAKE_WORD`、`CONFIG_CUSTOM_WAKE_WORD="xiao rui tong xue"`、esp-sr `mn7_cn` 模型选择）和所有 xiaozhi 协议元素均未改动。
