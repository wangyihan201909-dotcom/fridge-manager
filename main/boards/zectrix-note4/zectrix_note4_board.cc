/*
 * ZECTRIX NOTE4 —— 冰箱管家 / 小智
 *
 * 板级只做四件事：电源、I2C、音频编解码、按键与电池。
 * 屏幕（EpaperDisplay）单独一块，还没接上 —— Board::GetDisplay() 的默认实现
 * 返回 NoDisplay，所以没有屏也能完整跑通语音，先验唤醒词再做显示。
 *
 * 顺序在这里是有意义的，不能重排：
 *   1. 电源锁存（GPIO17）—— 放开就整机断电，必须最先拉住
 *   2. 音频域电源（GPIO42）—— 不开则 ES8311 在 I2C 上根本不应答，
 *      「codec 创建失败」会长得像接线问题
 *   3. I2C 总线 → 4. 编解码器
 */

#include <algorithm>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_codec_dev_defaults.h>
#include <esp_log.h>

#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "custom_lcd_display.h"
#include "fridge_app.h"
#include "wifi_board.h"
#include "ssid_manager.h"
#include <esp_system.h>

#define TAG "zectrix-note4"

/* 电池 ADC：GPIO4 = ADC1_CH3，12dB 衰减，分压 1:2。
 * 百分比用 Zectrix 给这颗电芯拟合的二次曲线，别换成线性 ——
 * 锂电放电曲线中段很平，线性映射会让「80%」停很久然后突然掉。 */
static constexpr adc_channel_t kBatteryAdcChannel = ADC_CHANNEL_3;

class ZectrixNote4Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t adc_cali_ = nullptr;
    Es8311AudioCodec* codec_ = nullptr;
    CustomLcdDisplay* display_ = nullptr;

    Button confirm_button_{BOOT_BUTTON_GPIO};
    Button up_button_{UP_BUTTON_GPIO};
    Button down_button_{DOWN_BUTTON_GPIO};

    /* 电源锁存。掉了就整机断电，所以第一件事做，并且 gpio_hold_en
     * 让它跨深睡保持。关机就是反过来把它放开。 */
    void InitializePowerLatch() {
        /* 功放使能脚（GPIO46）必须在这里配成输出。
         *
         * Es8311AudioCodec 只调 gpio_set_level(pa_pin_, ...)，**自己一个
         * gpio_config 都没有** —— 它假定板子已经把方向配好了（esp-spot
         * 就是在 board 里显式配的）。漏掉的话 set_level 是空操作，
         * 功放永远不开：唤醒词、ASR、大模型、TTS 全都正常，
         * 状态机也照常进 speaking，**就是一点声音都没有**。
         *
         * Zectrix 参考实现的输出掩码里同样没有这个脚 —— 别照抄。
         * 那份参考的音频从没真正出过声（v1 的音频是 stub），所以这个洞
         * 一直没暴露出来。 */
        /* **先清掉所有 pad hold。**
         *
         * pad hold 跨软复位不丢（只有断电才清），而这块板子上跑过的原厂固件
         * （小智）到处用 gpio_hold_en 让引脚跨深睡保持。设备一直插着 USB、
         * esptool 又只做 CPU 复位 —— 原厂设的 hold 能一路活到我们的固件里。
         *
         * 被 hold 住的脚，gpio_config + gpio_set_level 全是空操作。
         * 上游的 Es8311AudioCodec::UpdateDeviceState 只调 gpio_set_level，
         * 不解除 hold（社区版 zectrix 的 PowerAmpOn 每次都先 hold_dis，
         * 说明他们踩过），所以功放会永远打不开：
         * 唤醒词、ASR、大模型、TTS 全正常，就是不出声。 */
        gpio_deep_sleep_hold_dis();
        for (gpio_num_t p : {static_cast<gpio_num_t>(VBAT_LATCH_PIN),
                             static_cast<gpio_num_t>(AUDIO_POWER_EN_PIN),
                             static_cast<gpio_num_t>(AUDIO_CODEC_PA_PIN),
                             static_cast<gpio_num_t>(BUILTIN_LED_GPIO),
                             static_cast<gpio_num_t>(EPD_POWER_EN_PIN)}) {
            gpio_hold_dis(p);
        }

        gpio_config_t cfg = {};
        cfg.pin_bit_mask = (1ULL << VBAT_LATCH_PIN) | (1ULL << AUDIO_POWER_EN_PIN) |
                           (1ULL << BUILTIN_LED_GPIO) | (1ULL << AUDIO_CODEC_PA_PIN) |
                           (1ULL << EPD_POWER_EN_PIN);
        cfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&cfg);
        gpio_set_level(AUDIO_CODEC_PA_PIN, 0);   /* 先关着，codec 要用时自己拉高 */

        gpio_hold_dis(static_cast<gpio_num_t>(VBAT_LATCH_PIN));
        gpio_set_level(static_cast<gpio_num_t>(VBAT_LATCH_PIN), 1);
        gpio_hold_en(static_cast<gpio_num_t>(VBAT_LATCH_PIN));

        /* 音频域上电。ES8311 要等电稳，这里给足时间再碰 I2C。 */
        gpio_hold_dis(static_cast<gpio_num_t>(AUDIO_POWER_EN_PIN));
        gpio_set_level(static_cast<gpio_num_t>(AUDIO_POWER_EN_PIN), 1);
        gpio_hold_en(static_cast<gpio_num_t>(AUDIO_POWER_EN_PIN));
        vTaskDelay(pdMS_TO_TICKS(50));

        gpio_set_level(static_cast<gpio_num_t>(BUILTIN_LED_GPIO), 1);  /* 低有效，1=灭 */

        /* 充电状态两个输入。CHRG_L 低 = USB 供电中。 */
        gpio_config_t in = {};
        in.pin_bit_mask = (1ULL << CHRG_L_PIN) | (1ULL << STDBY_H_PIN);
        in.mode = GPIO_MODE_INPUT;
        in.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&in);
    }

    void InitializeI2c() {
        i2c_master_bus_config_t cfg = {};
        cfg.i2c_port = I2C_NUM_0;
        cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        cfg.glitch_ignore_cnt = 7;
        cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus_));
    }

    void InitializeAdc() {
        adc_oneshot_unit_init_cfg_t unit = {};
        unit.unit_id = ADC_UNIT_1;
        if (adc_oneshot_new_unit(&unit, &adc_handle_) != ESP_OK) {
            adc_handle_ = nullptr;
            ESP_LOGW(TAG, "电池 ADC 起不来，电量将报不可用");
            return;
        }
        adc_oneshot_chan_cfg_t ch = {};
        ch.atten = ADC_ATTEN_DB_12;
        ch.bitwidth = ADC_BITWIDTH_12;
        adc_oneshot_config_channel(adc_handle_, kBatteryAdcChannel, &ch);

        adc_cali_curve_fitting_config_t cali = {};
        cali.unit_id = ADC_UNIT_1;
        cali.atten = ADC_ATTEN_DB_12;
        cali.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_curve_fitting(&cali, &adc_cali_) != ESP_OK) {
            adc_cali_ = nullptr;
            ESP_LOGW(TAG, "电池 ADC 校准不可用");
        }
    }

    /* 确认键：按住说话。上下键：音量。下键长按：关机。
     *
     * 用按住而不是点一下切换，是因为这块设备同时开着唤醒词 ——
     * 免唤醒负责「随口一说」，按住负责「不想喊出声」。两者语义不重叠。 */
    void InitializeButtons() {
        confirm_button_.OnPressDown([]() {
            Application::GetInstance().StartListening();
        });
        confirm_button_.OnPressUp([]() {
            Application::GetInstance().StopListening();
        });

        /* 上键 = 音量+。切页已去掉 —— 见 fridge_app.cc 里 want_raw 的注释：
         * 小智的界面不是全屏不透明的，切过去只会和底下的冰箱位图叠在一起，
         * 比任何一套单独显示都难看。 */
        up_button_.OnClick([this]() { AdjustVolume(+10); });

        /* **上键长按暂时不接任何动作。**
         *
         * 原来接的是「清 Wi-Fi 凭据并重启」，实测它在没人按的情况下自己触发了 ——
         * GPIO39 被持续拉低（按键组件给低有效按键开了上拉，仍压不住），
         * 于是形成循环：开机 → 长按判定成立 → 擦 Wi-Fi → 重启 → 再来一次。
         *
         * 教训：**把破坏性动作挂到一个电平没验证过的引脚上，是我的疏忽。**
         * 先用下面这个探测把 GPIO39 的真实电平打出来，确认了再谈接什么。 */
        xTaskCreate([](void *) {
            int last = -1;
            while (true) {
                int up = gpio_get_level(UP_BUTTON_GPIO);
                int down = gpio_get_level(DOWN_BUTTON_GPIO);
                int ok = gpio_get_level(BOOT_BUTTON_GPIO);
                int cur = up * 100 + down * 10 + ok;
                if (cur != last) {
                    ESP_LOGW(TAG, "[按键电平] 上=%d 下=%d 确认=%d（低有效，静止时应全为 1）",
                             up, down, ok);
                    last = cur;
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }, "btnprobe", 2560, nullptr, 1, nullptr);
        down_button_.OnClick([this]() { AdjustVolume(-10); });

        /* 关机 = 放开电源锁存。gpio_hold_dis 必须先做，
         * 否则 hold 还生效着，set_level 不起作用。 */
        down_button_.OnLongPress([]() {
            ESP_LOGI(TAG, "长按下键：关机");
            gpio_hold_dis(static_cast<gpio_num_t>(VBAT_LATCH_PIN));
            gpio_set_level(static_cast<gpio_num_t>(VBAT_LATCH_PIN), 0);
        });
    }

    /* 墨水屏。显示实现是 custom_lcd_display.cc，从社区移植版原样拿来的
     * （见 config.h 里的兼容层注释）。它自带 SPI 驱动、LVGL flush 回调，
     * 以及刷新合并调度 —— 墨水屏全刷要一秒多且肉眼可见地闪，
     * 聊天字幕绝不能来一句刷一次。 */
    void InitializeDisplay() {
        /* 屏幕电源先上，等电稳再碰 SPI */
        gpio_set_level(static_cast<gpio_num_t>(EPD_POWER_EN_PIN), 1);
        vTaskDelay(pdMS_TO_TICKS(50));

        custom_lcd_spi_t spi = {};
        spi.cs         = EPD_CS_PIN;
        spi.dc         = EPD_DC_PIN;
        spi.rst        = EPD_RST_PIN;
        spi.busy       = EPD_BUSY_PIN;
        spi.mosi       = EPD_MOSI_PIN;
        spi.scl        = EPD_SCK_PIN;
        spi.power      = EPD_PWR_PIN;
        spi.spi_host   = EPD_SPI_NUM;
        spi.buffer_len = EXAMPLE_LCD_WIDTH * EXAMPLE_LCD_HEIGHT / 8;

        display_ = new CustomLcdDisplay(nullptr, nullptr,
                                        EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT,
                                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                        DISPLAY_SWAP_XY, spi);
    }

    void AdjustVolume(int delta) {
        auto codec = GetAudioCodec();
        if (codec == nullptr) return;
        int v = std::clamp(codec->output_volume() + delta, 0, 100);
        codec->SetOutputVolume(v);
    }

public:
    ZectrixNote4Board() {
/* **时区必须在这里设一次。** 小智的状态栏（lvgl_display.cc）用 localtime()，
         * 而 ESP-IDF 默认没有时区 —— 不设就等于 UTC，屏上会显示比北京时间早 8
         * 小时的钟（实测 15:40 显示成 07:40），和我们自绘的状态条带并排出现，
         * 一块屏上两个时间。
         * POSIX 的符号是反的：UTC+8 写作 CST-8。中国不用夏令时，无需 DST 段。
         * fridge_app 的状态条带也依赖这里 —— 删掉它两处都会错。 */
        setenv("TZ", "CST-8", 1);
        tzset();

        InitializePowerLatch();
        InitializeI2c();
        InitializeAdc();
        InitializeButtons();
        InitializeDisplay();
        fridge_app_start(display_);
    }

    virtual std::string GetBoardType() override { return "zectrix-note4"; }

    virtual Display* GetDisplay() override { return display_; }

    virtual AudioCodec* GetAudioCodec() override {
        if (codec_ == nullptr) {
            codec_ = new Es8311AudioCodec(
                i2c_bus_, I2C_NUM_0,
                AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
                AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        }
        return codec_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        /* **两个脚一起看才是「有没有外部供电」。**
         *
         *   CHRG_L == 0   正在给电池充电
         *   STDBY_H == 1  充满/待机（仍然插着，只是不充了）
         *
         * 只看 CHRG_L 的话，电池一充满它就变高，设备会以为自己在用电池 ——
         * 于是 60 秒的轮询钳位不生效，手机上改了东西要等半小时才同步。
         * CLAUDE.md 里 v1 就记过这一条，我又踩了一遍：那次是 charge.charging
         * 与 power_present 之分，这次是只取了两个来源里的一个。 */
        bool charging_now = gpio_get_level(static_cast<gpio_num_t>(CHRG_L_PIN)) == 0;
        bool full_standby = gpio_get_level(static_cast<gpio_num_t>(STDBY_H_PIN)) == 1;
        bool powered = charging_now || full_standby;
        charging = powered;
        discharging = !powered;

        if (adc_handle_ == nullptr || adc_cali_ == nullptr) return false;

        int sum = 0;
        for (int i = 0; i < 10; ++i) {
            int raw = 0, pin_mv = 0;
            if (adc_oneshot_read(adc_handle_, kBatteryAdcChannel, &raw) != ESP_OK ||
                adc_cali_raw_to_voltage(adc_cali_, raw, &pin_mv) != ESP_OK) {
                return false;
            }
            sum += pin_mv * 2;   /* 分压 1:2 */
        }
        const int mv = sum / 10;
        if (mv <= 0) return false;
        const int pct = (-mv * mv + 9016 * mv - 19189000) / 10000;
        level = std::clamp(pct, 0, 100);
        return true;
    }
};

DECLARE_BOARD(ZectrixNote4Board);
