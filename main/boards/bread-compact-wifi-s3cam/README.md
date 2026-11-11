Kodo 桌面英语陪练机器人使用的板子（唤醒词：小睿同学）。

硬件基于 GOOUUU ESP32-S3-CAM N16R8 开发板，代码基于 bread-compact-wifi-lcd 修改：

- 摄像头：OV2640（传感器直接输出 JPEG，XCLK 10 MHz）
- 麦克风：INMP441；功放：MAX98357A
- 屏幕：ST7789 2.0 寸 320x240，显示 Kodo 矢量表情（`kodo_face_display.*`，设计稿见 `docs/kodo-face-design/`）
- 灯环：16 颗 WS2812，接 GPIO14（`kodo_light.*`，MCP 工具 `self.light.set` / `self.light.get_state`）

注意因为摄像头占用 IO 较多，所以占用了 ESP32S3 的 USB 19 20 两个引脚。
连线方式参考 config.h 文件中对引脚的定义。


# 编译配置命令

本项目使用 `idf.py build` 编译。不要用 `scripts/build.py`，它会重置自定义唤醒词相关的 sdkconfig。

**配置编译目标为 ESP32S3：**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig：**

```bash
idf.py menuconfig
```

**选择板子：**

```bash
Xiaozhi Assistant -> Target Board -> Bread Compact Wi-Fi + LCD + Camera (面包板)
Xiaozhi Assistant -> LCD Type -> ST7789 240*320, IPS
```

`config.json` 里的 `sdkconfig_append`（OTA 地址、`CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT` 等）只有 `scripts/build.py` 会自动应用，用 `idf.py` 时需要在 menuconfig / sdkconfig 中保持相同设置。

**编译烧入：**

```bash
idf.py build flash
```

增删 `main/assets/common/` 下的音频文件后，需要先执行一次 `idf.py reconfigure`（或 `idf.py fullclean`）再编译。
