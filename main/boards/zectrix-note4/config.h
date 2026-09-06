/*
 * ZECTRIX NOTE4 —— ESP32-S3 (N16R8) + 4.2" 黑白墨水屏 + ES8311 + PCF8563 + NT3H
 *
 * 引脚来自冰箱管家 v1 固件的 board.h，已在真机上验证过（屏幕点亮、音频供电、
 * 按键唤醒、电池 ADC）。原厂固件同样是小智，所以这份映射与原厂一致。
 *
 * 两个「不设就掉电/没声音」的脚，别当普通 GPIO 看待：
 *   GPIO17 VBAT_LATCH  —— 主板电源锁存，必须持续拉高并 gpio_hold_en，
 *                          放开 = 整机断电（这也是关机的实现方式）
 *   GPIO42 AUDIO_POWER —— 音频域电源，不开则 ES8311 在 I2C 上根本不应答
 */

#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/* ── 音频 ────────────────────────────────────────────────
 * ES8311 全双工（唤醒词要边放边收做 AEC，不能用 simplex）。
 *
 * **收发必须同频。** 一颗 ES8311 收发共用一个 I2S 口，上游的
 * Es8311AudioCodec 构造里直接 assert(input == output)，不等就 panic
 * 重启 —— 表现是开机后 82 次/25 秒的重启循环，不是「没声音」。
 * 抄 bread-compact-wifi 的 16k/24k 会中招：那块板子是 simplex，
 * 两个独立 I2S 口，两边不同频是合法的。
 *
 * 取 24k 是因为服务器下发的 TTS 就是 24k —— 输出设 16k 会被降采样，
 * 声音明显发闷发怪（日志会警告 "resampling may cause distortion"）。
 * WakeNet 要的 16k 由 AudioService 的 input_resampler_ 从 24k 降下来，
 * 这条路上游支持，atk-dnesp32s3-box 就是 24k/24k 出货的。 */
#define AUDIO_INPUT_SAMPLE_RATE   24000
#define AUDIO_OUTPUT_SAMPLE_RATE  24000

#define AUDIO_I2S_GPIO_MCLK   GPIO_NUM_14
#define AUDIO_I2S_GPIO_BCLK   GPIO_NUM_15
#define AUDIO_I2S_GPIO_WS     GPIO_NUM_38   /* LRCK */
#define AUDIO_I2S_GPIO_DIN    GPIO_NUM_16   /* 编解码器 DOUT → ESP 收 */
#define AUDIO_I2S_GPIO_DOUT   GPIO_NUM_45   /* ESP 发 → 编解码器 DIN */

#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_47
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_48
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_46   /* 扬声器功放使能 */
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR  /* 0x18 */
#define AUDIO_POWER_EN_PIN       GPIO_NUM_42   /* 音频域电源，见文件头 */

/* ── 墨水屏（SPI，400×300 1bpp）────────────────────────── */
#define EPD_POWER_EN_PIN  GPIO_NUM_6
#define EPD_BUSY_PIN      GPIO_NUM_8
#define EPD_RESET_PIN     GPIO_NUM_9
#define EPD_DC_PIN        GPIO_NUM_10
#define EPD_CS_PIN        GPIO_NUM_11
#define EPD_SCK_PIN       GPIO_NUM_12
#define EPD_MOSI_PIN      GPIO_NUM_13
#define DISPLAY_WIDTH     400
#define DISPLAY_HEIGHT    300

/* ── 按键 ────────────────────────────────────────────────
 * 三个键都是低有效。GPIO39 不能作深睡唤醒源 —— ESP32-S3 的 RTC GPIO
 * 只到 GPIO21，这是硬件事实，不是配置问题。 */
#define BOOT_BUTTON_GPIO       GPIO_NUM_0    /* 确认 / 按住说话，可唤醒 */
#define DOWN_BUTTON_GPIO       GPIO_NUM_18   /* 下键 / 电源键，可唤醒 */
#define UP_BUTTON_GPIO         GPIO_NUM_39   /* 上键，仅翻页，不可唤醒 */

/* ── 电源 ────────────────────────────────────────────────
 * CHRG_L 低 = USB 正在供电。注意别用「正在充电」那个指示，
 * 电池充满或没装电池时它是假，而 USB 明明还插着（v1 踩过）。 */
#define VBAT_LATCH_PIN    GPIO_NUM_17   /* 电源锁存，见文件头 */
#define STDBY_H_PIN       GPIO_NUM_1    /* 充满 / 待机 */
#define CHRG_L_PIN        GPIO_NUM_2    /* 低 = USB 供电中 */
#define BUILTIN_LED_GPIO  GPIO_NUM_3
#define BAT_ADC_PIN       GPIO_NUM_4    /* 电池分压 ADC */

/* ── 其它外设 ─────────────────────────────────────────── */
#define RTC_INT_PIN       GPIO_NUM_5    /* PCF8563 闹钟中断 */
#define NFC_FD_PIN        GPIO_NUM_7    /* NT3H 场检测 */
#define NFC_POWER_PIN     GPIO_NUM_21
#define I2C_ADDR_PCF8563  0x51
#define I2C_ADDR_NT3H     0x55


/* ── 兼容层：custom_lcd_display.cc 用的宏名 ──────────────
 * 那个文件从社区移植版 cattei/xiaozhi-zectrix 原样拿过来（MIT，极趣），
 * 1152 行，**只改了一行 include**（社区版仓库缺 common/sleep_manager.h，
 * 改指向本目录的垫片）—— 其余原样保留，将来跟他们同步才好 diff。
 * 它的引脚宏名和本文件上面那套不完全一样，这里补上别名。
 * 改引脚时两边都要改，或者干脆只改上面那套、这里引用它。 */
#define EPD_SPI_NUM        SPI3_HOST
#define EPD_RST_PIN        EPD_RESET_PIN
#define EPD_PWR_PIN        EPD_POWER_EN_PIN
#define EXAMPLE_LCD_WIDTH   DISPLAY_WIDTH
#define EXAMPLE_LCD_HEIGHT  DISPLAY_HEIGHT
#define DISPLAY_OFFSET_X    0
#define DISPLAY_OFFSET_Y    0
#define DISPLAY_MIRROR_X    false
#define DISPLAY_MIRROR_Y    false
#define DISPLAY_SWAP_XY     false

#endif  /* _BOARD_CONFIG_H_ */
