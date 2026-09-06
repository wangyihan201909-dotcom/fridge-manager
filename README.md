# 冰箱管家 · 便利贴固件（fridge-manager）

给 **ZECTRIX NOTE4**（ESP32-S3 + 4.2" 黑白墨水屏）写的固件：它既是一台
[小智 AI](https://github.com/78/xiaozhi-esp32) 语音终端，也是一张贴在冰箱上的
电子便利贴 —— 平时显示家里有什么食材、放了多久，可以直接说话增删。

本仓库是 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的 fork，
在它之上加了两样东西：

| | |
|---|---|
| **板级支持** `main/boards/zectrix-note4/` | 上游 138 块板子里**没有任何墨水屏板**，这是第一个 |
| **冰箱管家应用** | 同目录下的 `fridge_app.*` / `device.*` / `net.*` / `proto.*` 等 |

> 上游的 README 保留在 [README.upstream.md](README.upstream.md)。

> 本仓库从上游 `5df5b7f`（2026-08-31）浅克隆而来，所以 git 历史里上游那棵树
> 会显示成一个根提交 —— 它不是我们写的，出处见根目录 LICENSE 与 README.upstream.md。

## 只想要板级支持？

**可以只拿板子那部分。** `Board::GetDisplay()` 有默认实现，删掉
`fridge_app.*` 也能完整跑通语音对话 —— 唤醒词、ASR、大模型、TTS 都是上游的能力，
这块板子上都实测跑通了。

板级部分解决的问题：

- **ES8311 全双工**：收发共用一个 I2S 口，采样率**必须相等**，
  上游构造函数里直接 assert，不等就开机重启循环。这里是 24000/24000。
  抄别的板子的 `config.h` 前先看它是 simplex 还是 duplex。
- **功放使能脚 GPIO46 要自己配成输出**。`Es8311AudioCodec` 只调
  `gpio_set_level`，不做 `gpio_config` —— 没配方向的话 `set_level` 是空操作，
  **一切日志正常但就是不出声**。
- **三个不能当普通 GPIO 看的脚**：GPIO17 电源锁存（放开=整机断电）、
  GPIO42 音频域电源（不开则 ES8311 在 I2C 上不应答）、GPIO46 功放使能。
- **墨水屏刷新调度**：全刷一秒多且肉眼可见地闪，聊天字幕**绝不能来一句刷一次**。
  `CustomLcdDisplay` 做刷新合并。

## 冰箱管家部分

**需要自建服务端**，本仓库不含。设备侧协议是五个 HTTPS 端点 + HMAC-SHA256
鉴权（`signature = HMAC(secret, ts + method + route + body)`，±5 分钟窗口），
配套的微信云函数没有开源。

几条设计约束，改代码前值得知道：

- **返回 304 时一个像素都不许动。** 这是判断实现是否正确的最快指标。
- **排版全部由云端渲染成 1bpp 位图下发**，固件不做排版，只 `WriteRaw1bpp` 灌图。
- **右下角 `{256,264,136,20}` 由固件自绘**（时间 + 电量）并只局刷这一块 ——
  云端保证不往这块画。两端各硬编码一份，改错了没有编译期提示。
  x 和 w 必须是 8 的倍数，局刷按字节走。
- **删除是破坏性操作，语音只认唯一命中**，零命中或多命中一律交回模型追问。

云端 AI 通过 MCP 工具操作冰箱：`self.fridge.list / expiring / add / eaten /
remove / stats`、`self.shopping.add / move_to_fridge`。

## 构建

需要 **ESP-IDF v6.0+**。

```bash
cd main/boards/zectrix-note4
cp fridge_config.example.h fridge_config.h    # 填自己的服务端地址
cd -
idf.py set-target esp32s3
idf.py build
```

不填 `fridge_config.h` 会缺头文件编不过 —— 这是故意的，那里面是你自己的服务端地址。

### Windows 上的四个坑

项目路径**同时含空格和中文时 ESP-IDF 会以四种不同的方式失败**，每种报错都指向错误的方向。
最省事的做法是把仓库放在纯 ASCII 无空格路径下。真要放，对应的绕法：

| 报错 | 真因 | 绕法 |
|---|---|---|
| `Both 'XTENSA_GNU_CONFIG' and "-dynconfig=" ... pointed different files` | 工具链装在带空格的路径 → CMake 改用 8.3 短名调 gcc → 多目标驱动靠 argv[0] 认目标，短名里没有 esp32s3 | `IDF_TOOLS_PATH=C:\esp\tools` |
| `UnicodeDecodeError: 'gbk' codec` | IDF 的 Python 工具用区域编码读自己生成的 UTF-8 JSON | `PYTHONUTF8=1` |
| `filesystem error: Cannot convert character sequence` | ccache 的 std::filesystem 处理不了 GBK 区域下的中文路径 | `--no-ccache` |
| `objdump.exe: .../<乱码>/build/...: No such file` | ldgen 把 build 目录路径传给 MinGW 的 objdump，它按 ANSI 解 argv | `idf.py -B C:\某个ASCII路径` |

另外：**改 `sdkconfig.defaults` 里已存在于 `sdkconfig` 的符号不会生效** ——
defaults 只对 sdkconfig 里没有的符号起作用，要让新值生效得删掉 `sdkconfig` 重新生成。

### 版本号为什么是 999

`CMakeLists.txt` 里 `PROJECT_VER` 刻意抬到 999 段。上游每次开机会拿这个版本去问
OTA 服务器，对方版本更高就**静默下载刷写并重启，不问用户** —— 抬高之后上游的发布
永远追不上，这台设备不会被刷成没有冰箱管家的构建。理由与边界写在那一行上方。

## 授权

MIT。本仓库继承上游 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的授权，
另含两份第三方代码，均为 MIT 且随附授权文件：

- `components/zectrix_epd/` —— 墨水屏驱动，Zectrix Lab
- `main/boards/zectrix-note4/custom_lcd_display.*` —— 移植自
  [cattei/xiaozhi-zectrix](https://github.com/cattei/xiaozhi-zectrix)，
  改动列在文件头
