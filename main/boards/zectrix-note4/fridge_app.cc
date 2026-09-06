/*
 * fridge_app.cc —— 冰箱管家页面 + 云端同步任务。
 *
 * 与云端的分工（硬约束 3）：**版面全部由云端渲染**。/device/sync 下发
 * 400×300 的 1bpp 位图（RLE 压缩），固件解开直接灌屏，一行排版都不做。
 * 版面长什么样是 cloudfunctions/device/screen.js 说了算 —— 那边已经接了
 * 小程序的设计语言（反白顶栏、大数字 hero、分段方块 meter、5×5 点阵图标）。
 *
 * 唯一由固件自绘的是右下角状态条带（时间 + 电量），原因见下面 STATUS_RECT
 * 那段注释。
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <ctime>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "board.h"
#include "config.h"
#include "custom_lcd_display.h"
#include "mcp_server.h"
#include "fridge_app.h"

extern "C" {
#include "device.h"
#include "net.h"
#include "nvs_store.h"
#include "rle.h"
}

#include "fridge_config.h"

#define TAG "fridge"

/* 云端地址在 fridge_config.h（已 gitignore）。首次构建先：
 *     cp fridge_config.example.h fridge_config.h
 * 然后填自己的微信云开发 HTTP 访问服务域名。 */
static const char *kBase = FRIDGE_CLOUD_BASE;


/* ── 状态条带 ───────────────────────────────────────────────
 * **必须与 cloudfunctions/device/screen.js 的 STATUS_RECT 逐字对齐**：
 * 云端保证不往这块画，固件只往这块写。两端各硬编码一份，改错了没有任何
 * 编译期提示 —— 云端那边有一条测试专门断言这块区域全白。
 *
 * x 与 w 必须是 8 的倍数：墨水屏局刷按字节走，WriteRaw1bpp 里的 align_x8
 * 会把不对齐的矩形扩大，扩过界就会啃掉云端画的内容。
 *
 * 为什么这块不让云端画：位图只在 rev 变化时才下发（rev 没变就是 304，
 * 304 不带 body），所以云端画进位图里的钟必然停在「上一次库存变化的时刻」。
 * 而设备自己知道时间（SNTP 对过）也知道电量，跑一趟云端往返只为显示
 * 「现在几点」本身就是荒谬的。 */
#define ST_X 256
#define ST_Y 264
#define ST_W 136
#define ST_H 20
#define ST_STRIDE (ST_W / 8) /* 17 字节/行 */

/* 8×16 点阵，只有这条带用得到的 12 个字形。固件里放整套汉字库没有意义，
 * 这块永远只画数字和三个符号。与云端 ascii8x16.js 同源（GNU Unifont）。 */
typedef struct {
    char ch;
    uint8_t rows[16];
} glyph_t;

static const glyph_t kGlyphs[] = {
    {'0', {0, 0, 0, 0, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0, 0, 0, 0}},
    {'1', {0, 0, 0, 0, 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0, 0, 0, 0}},
    {'2', {0, 0, 0, 0, 0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0, 0, 0, 0}},
    {'3', {0, 0, 0, 0, 0x3C, 0x66, 0x06, 0x1C, 0x06, 0x06, 0x66, 0x3C, 0, 0, 0, 0}},
    {'4', {0, 0, 0, 0, 0x0C, 0x1C, 0x2C, 0x4C, 0x7E, 0x0C, 0x0C, 0x0C, 0, 0, 0, 0}},
    {'5', {0, 0, 0, 0, 0x7E, 0x60, 0x7C, 0x06, 0x06, 0x06, 0x66, 0x3C, 0, 0, 0, 0}},
    {'6', {0, 0, 0, 0, 0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x3C, 0, 0, 0, 0}},
    {'7', {0, 0, 0, 0, 0x7E, 0x06, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x18, 0, 0, 0, 0}},
    {'8', {0, 0, 0, 0, 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0, 0, 0, 0}},
    {'9', {0, 0, 0, 0, 0x3C, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0, 0, 0, 0}},
    {':', {0, 0, 0, 0, 0, 0x18, 0x18, 0, 0, 0x18, 0x18, 0, 0, 0, 0, 0}},
    {'-', {0, 0, 0, 0, 0, 0, 0, 0x7E, 0, 0, 0, 0, 0, 0, 0, 0}},
    {'%', {0, 0, 0, 0, 0x62, 0x66, 0x0C, 0x18, 0x30, 0x66, 0x46, 0, 0, 0, 0, 0}},
    {' ', {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
};

static const uint8_t *glyph_for(char ch) {
    for (size_t i = 0; i < sizeof(kGlyphs) / sizeof(kGlyphs[0]); i++) {
        if (kGlyphs[i].ch == ch) return kGlyphs[i].rows;
    }
    return nullptr;
}

namespace {

class FridgeApp {
public:
    explicit FridgeApp(CustomLcdDisplay *display) : display_(display) {}

    void Run() {
        /* **开机时我们并不知道屏上是什么。**
         *
         * 墨水屏断电保画，而这一刻上面多半是小智的 LVGL 开机界面
         * （配网提示、激活码）刚画过的东西 —— 我们手里又没有帧缓冲。
         * NVS 里存着的 rev 和 screenHash 此刻是在替一张不存在的画面作证。
         *
         * 清掉，逼云端下一次回 200 把位图重发一遍。
         * 代价是每次开机多一次全量 sync —— 刚开机本来就该取一次当前状态。
         *
         * **rev 和 hash 必须一起清。** 云端的 304 只看 rev，位图下不下发才看
         * hash，两道闸门用两个信号：只清 hash 的话 rev 相等就直接 304 了，
         * 位图那道判断根本走不到。 */
        nvs_sync_set_rev(0);
        nvs_sync_set_screen_hash("");

        WaitForNetwork();
        net_time_sync();
        EnsureBound();

        while (true) {
            int poll = nvs_sync_get_poll_interval();
            if (poll <= 0) poll = 60;
            
            /* **插着电时本地钳到 60 秒，不等云端下发。**
             *
             * 间隔是云端给的、存在 NVS 里。云端把默认值从 1800 改成 60 之后，
             * 设备仍要等**下一次轮询**才知道 —— 而那次轮询正是 1800 秒之后。
             * 鸡生蛋：手机上删了东西，半小时内怎么等都不动。
             * 本地钳一道就跳出这个循环，也不依赖云端有没有部署。 */
            int lvl = -1; bool charging = false, discharging = false;
            Board::GetInstance().GetBatteryLevel(lvl, charging, discharging);
            if (charging && poll > 60) poll = 60;

            SyncOnce();
            UpdateStatusStrip(false);

            /* 睡到下次轮询，但每秒醒来看一眼状态条带要不要更新 ——
             * 分钟跳变和电量变化不该等一整个轮询周期才上屏。 */
            for (int i = 0; i < poll; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                UpdateStatusStrip(false);
                ApplyPage(false);
            }
        }
    }

private:
    CustomLcdDisplay *display_;
    char last_strip_[32] = "";
    int shown_battery_ = -1;   /* 屏上那个电量值，带迟滞，见 UpdateStatusStrip */
    bool was_busy_ = false;
    bool page_fridge_ = true;  /* 用户选的页面：true=冰箱页 */
    bool have_real_frame_ = false;  /* last_frame_ 里是云端位图还是占位页 */

    /* 库存缓存。sync 只在 rev 变化时下发（304 不带 body），所以这份
     * 一直沿用到下次变化 —— 正是我们要的：语音提问不该触发一次云端往返。
     * MCP 回调跑在另一个任务上，所以要加锁。 */
    SemaphoreHandle_t items_mutex_ = xSemaphoreCreateMutex();
    sync_item_t items_[MAX_SYNC_ITEMS];
    int item_count_ = 0;
    bool items_truncated_ = false;
    /* 统计与 items 同源：都只在 sync 回 200 时更新，304 沿用上一份。 */
    int st_days_ = 0, st_eaten_ = 0, st_lost_ = 0, st_top_count_ = 0;
    char st_top_name_[48] = "";
    uint8_t *last_frame_ = nullptr; /* 屏上那张冰箱位图，聊完要拿它复原 */

    void WaitForNetwork() {
        /* 小智的 WifiBoard 负责连接，我们只等它连上。
         * 首次配网可能要几分钟，所以这里不设超时。 */
        while (!net_wifi_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        ESP_LOGI(TAG, "网络就绪");
    }

    void EnsureBound() {
        if (device_is_bound()) {
            ESP_LOGI(TAG, "已绑定，device=%s", device_id());
            return;
        }

        /* 屏幕配对：设备要一个 6 位码显示在屏上，用户在小程序里输入认领，
         * 设备轮询到认领后拿到密钥。
         *
         * 反过来做（手机把码送进设备）需要一条 BLE 之类的通道，
         * 而屏幕本来就有 —— 这一步不需要任何新硬件能力。
         *
         * 这里会一直转到绑定成功为止：没绑定的设备本来也没有别的事可做。 */
        char code[16] = "";
        char shown[16] = "";
        bool claimed = false;

        while (!claimed) {
            if (!device_pair(kBase, code, sizeof(code), &claimed)) {
                ESP_LOGW(TAG, "配对：要码失败，5 秒后重试");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            if (claimed) break;

            /* **码没变就不重画。** 墨水屏刷一次一秒多且肉眼可见地闪，
             * 而这个循环每 3 秒转一圈。 */
            if (strcmp(code, shown) != 0) {
                DrawPairScreen(code);
                snprintf(shown, sizeof(shown), "%s", code);
                ESP_LOGI(TAG, "配对码 %s —— 在小程序「便利贴设备」页输入", code);
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        ESP_LOGI(TAG, "配对成功，device=%s", device_id());
        return;
    }

    /* 一次同步。rev 没变云端回 304，那时**什么都不做** —— 这是硬约束 4，
     * 现在的理由不再是省电，而是墨水屏刷一次要一秒多且肉眼可见地闪。 */
    void SyncOnce() {
        if (!device_is_bound() || !net_wifi_connected()) return;

        int battery = -1;
        bool charging = false, discharging = false;
        Board::GetInstance().GetBatteryLevel(battery, charging, discharging);

        static sync_result_t res; /* 15KB，不能放栈上（v1 栽过一次） */
        memset(&res, 0, sizeof(res));
        if (!device_sync(kBase, &res, battery, charging ? "usb" : "battery")) return;

        /* changed=false 就是云端回了 304 —— 此时 has_screen 也一定是 false，
         * 一个像素都不许动。 */
        /* changed 为真才有 items（304 时云端什么都不带），
         * 所以只在这里覆盖缓存，别在 304 分支清空它。 */
        if (res.changed) {
            xSemaphoreTake(items_mutex_, portMAX_DELAY);
            memcpy(items_, res.items, sizeof(items_));
            item_count_ = res.item_count;
            items_truncated_ = res.items_truncated;
            st_days_ = res.stats.days;
            st_eaten_ = res.stats.eaten;
            st_lost_ = res.stats.lost;
            st_top_count_ = res.stats.top_lost_count;
            snprintf(st_top_name_, sizeof(st_top_name_), "%s", res.stats.top_lost_name);
            xSemaphoreGive(items_mutex_);
            ESP_LOGI(TAG, "库存已更新：%d 条%s", item_count_,
                     items_truncated_ ? "（云端截断了）" : "");
        }

        if (res.has_screen) {
            DrawFridgeBitmap(res.framebuf.data, sizeof(res.framebuf.data));
            /* **画上屏之后才记 hash。** 记早了的话，万一绘制失败或断电，
             * 设备会以为屏上是新内容、云端也不再下发，屏幕就永远停在旧图。 */
            nvs_sync_set_screen_hash(res.screen_hash);
            ESP_LOGI(TAG, "冰箱位图已上屏 rev=%lld hash=%s",
                     (long long)res.rev, res.screen_hash);
        }

        /* **rev 要在画完屏之后才推进。**
         *
         * 云端的 304 只看 rev。屏没画成就把 rev 记下来的话，下次就是 304，
         * 位图再也不会来 —— 屏幕永远停在旧图，而日志里一切正常。
         *
         * 不存 rev 的后果同样实在：永远发 rev=0，每次轮询都是全量 200，
         * 硬约束 4 那条 304 等于没实现。 */
        nvs_sync_set_rev(res.rev);
        if (res.next_poll_sec > 0) nvs_sync_set_poll_interval(res.next_poll_sec);
    }

    void DrawFridgeBitmap(const uint8_t *bits, size_t len) {
        if (display_ == nullptr || bits == nullptr) return;
        /* WriteRaw1bpp 的约定是 1=黑，与云端 screen.js 的 1=墨点一致，
         * 取反在它内部做（驱动那边是 1=白）—— 别在这里重复取反。 */
        display_->WriteRaw1bpp(0, 0, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT, bits, len);
        if (last_frame_ == nullptr) last_frame_ = (uint8_t *)malloc(len);
        if (last_frame_ != nullptr) {
            memcpy(last_frame_, bits, len);
            have_real_frame_ = true;
        }
    }

    /* 决定此刻屏幕归谁。
     *
     * 冰箱页只在**待命**时独占屏幕：一说话就让位给小智画对话字幕，
     * 聊完自动回来。说话时还占着屏的话，用户就看不见自己说了什么。
     *
     * 原始模式一开，LVGL 的 flush 回调直接返回，不再往帧缓冲落笔 ——
     * 两套 UI 写同一个缓冲，不拦住就是谁后写谁赢。 */
    /* 还没拿到云端位图时的占位页：白底 + 一圈边框。
     *
     * 存在的意义是**让设备如实显示自己的状态**：切到冰箱页却什么都没有，
     * 好过「切屏没反应」——后者会让人以为是切页坏了，而真正的原因
     * （sync 拿不到位图）根本没出现在屏幕上。
     * 边框还顺便证明了原始模式确实挡住了 LVGL。 */
    /* ── 冰箱管家的语音能力：self.fridge.* ───────────────────────
     *
     * 工具在设备上执行，再打我们现有的 HMAC 端点 —— 云端 AI 只负责
     * 把「把牛奶划掉」翻译成一次工具调用。
     *
     * 返回值一律是给模型念的自然语言，不是 JSON：模型拿到什么就说什么，
     * 中间少一层转述就少一层走样。 */
public:
    /* **注册必须早，不能等网络。**
     *
     * 实测：服务端在 MQTT 连上后 50ms 就来问 tools/list（9748 → 9798ms），
     * 而冰箱任务要先等网络、对时、绑定，10028ms 才注册完 —— **晚了 230ms，
     * 整批工具就没赶上那次查询**，模型于是完全不知道它们存在。
     * 而且这是竞态：偶尔抢赢就能用一次，表现成「时灵时不灵」。
     *
     * 注册只是声明，不需要网络；回调被调用时才需要。所以放到 board 构造期，
     * 那时距离 MQTT 连上还有九秒多。
     *
     * **描述里的例句不能删。**
     *
     * 我曾以为 tools/list 的 8000 字节分页把这些工具截掉了，于是把描述压短、
     * 删掉了「用户问…时调用」那些例句。加日志一测，分页根本没截
     * （12 个工具 2116 字节，一页装得下）—— 假设是错的，而压短之后
     * **原本能用的语音调用坏了**：模型靠这些例句把用户的说法映射到工具上。
     *
     * 简洁在这里不是美德。这段文字是给模型读的，不是给人读的。 */
    void RegisterMcpTools() {
        auto &mcp = McpServer::GetInstance();

        mcp.AddTool("self.fridge.list",
            "列出冰箱里现有的食材。用户问「冰箱里有什么」「还有没有鸡蛋」「剩多少」时调用",
            PropertyList(), [this](const PropertyList &) -> ReturnValue {
                return DescribeAll();
            });

        mcp.AddTool("self.fridge.expiring",
            "列出放得久、该处理的食材。用户问「什么快过期了」「有什么该吃了」「哪些要坏了」时调用",
            PropertyList(), [this](const PropertyList &) -> ReturnValue {
                return DescribeExpiring();
            });

        mcp.AddTool("self.fridge.eaten",
            "把某样食材标记为吃完/用完并移走。用户说「牛奶喝完了」「鸡蛋用完了」时调用。name 是食材名",
            PropertyList({ Property("name", kPropertyTypeString) }),
            [this](const PropertyList &p) -> ReturnValue {
                return DoOp("eaten", p["name"].value<std::string>());
            });

        mcp.AddTool("self.fridge.remove",
            "把某样食材从冰箱里删掉（丢掉了或录错了）。用户说「把牛奶划掉」「删掉那盒鸡蛋」时调用。name 是食材名",
            PropertyList({ Property("name", kPropertyTypeString) }),
            [this](const PropertyList &p) -> ReturnValue {
                return DoOp("delete", p["name"].value<std::string>());
            });

        mcp.AddTool("self.fridge.add",
            "往冰箱里加一样东西。用户说「加两盒牛奶」「买了一斤排骨」时调用。name 食材名，qty 数量，unit 单位（盒/袋/斤等）",
            PropertyList({
                Property("name", kPropertyTypeString),
                Property("qty", kPropertyTypeInteger, 1, 1, 99),
                Property("unit", kPropertyTypeString, std::string("份")),
            }),
            [this](const PropertyList &p) -> ReturnValue {
                return DoAdd(p["name"].value<std::string>(),
                             p["qty"].value<int>(),
                             p["unit"].value<std::string>());
            });

        mcp.AddTool("self.fridge.stats",
            "最近一个月吃掉多少、丢掉多少、哪样丢得最多。用户问「我最近浪费得多吗」「消费报告」「哪样老是吃不完」时调用",
            PropertyList(), [this](const PropertyList &) -> ReturnValue {
                return DescribeStats();
            });

        /* 购物清单不属于「冰箱里有什么」，所以单独一个命名空间。 */
        mcp.AddTool("self.shopping.add",
            "把一批要买的东西加进购物清单。用户说「都加到购物清单」「记下来下次买」"
            "「把这些加进去」时调用。names 用顿号或逗号分隔，例如「青菜、鸡蛋、豆腐」",
            PropertyList({ Property("names", kPropertyTypeString) }),
            [this](const PropertyList &p) -> ReturnValue {
                return DoShop(p["names"].value<std::string>());
            });

        mcp.AddTool("self.shopping.move_to_fridge",
            "把购物清单里已经勾选买到的东西一次性放进冰箱。"
            "用户说「买到的都放进冰箱」「把清单里买好的入库」「东西买回来了」时调用。无参数",
            PropertyList(), [this](const PropertyList &) -> ReturnValue {
                return DoStock();
            });

        ESP_LOGI(TAG, "已注册 self.fridge.* 与 self.shopping.* 工具");
    }

    /* **只给数字，不下结论。**
     *
     * 「你青菜买多了」这种话该由正在对话的模型说 —— 它有上下文、
     * 知道用户刚问了什么。我们再套一层自己的模型（小程序里的 advisor）
     * 是浪费一次调用，而且两个模型的口径会打架。
     * 小程序那边保留 advisor 是对的：手机上没有对话，才需要先嚼一遍。 */
    std::string DescribeStats() {
        xSemaphoreTake(items_mutex_, portMAX_DELAY);
        int d = st_days_, e = st_eaten_, l = st_lost_, tc = st_top_count_;
        std::string top = st_top_name_;
        xSemaphoreGive(items_mutex_);

        if (d <= 0) return "还没拿到统计数据，等下次同步再问我。";

        char buf[220];
        if (tc > 0 && !top.empty()) {
            snprintf(buf, sizeof(buf),
                     "最近 %d 天：吃掉 %d 样，丢掉 %d 样。丢得最多的是%s，%d 次。",
                     d, e, l, top.c_str(), tc);
        } else {
            snprintf(buf, sizeof(buf),
                     "最近 %d 天：吃掉 %d 样，一样都没丢。", d, e);
        }
        return buf;
    }

    /* 把清单里已勾选买到的一次性入库。
     *
     * **不接参数** —— 入库哪些由数据决定（在超市勾了但还没录进冰箱的那批），
     * 不是用户说出来的。让模型传名字反而会漏掉或编造。
     *
     * 保质期同样走云端的知识库先验；拍照和扫码仍然只能在手机上做，
     * 所以这批进来的是「没有照片的快速录入」，和手机上手动加是同一种。 */
    std::string DoStock() {
        char names[192] = "";
        int n = 0;
        if (!device_op_stock(kBase, names, sizeof(names), &n)) {
            return "没能入库，可能是网络问题，等会儿再试。";
        }
        if (n == 0) {
            return "购物清单里没有勾选买到、还没入库的东西。";
        }
        SyncOnce();
        ApplyPage(true);
        return "好，" + std::string(names) + " 已经放进冰箱了，一共 " +
               std::to_string(n) + " 样。保质期是按常识估的，包装上有日期的话在手机上补更准。";
    }

    /* 一句话把一批菜加进购物清单。
     *
     * **一次调用加一批**，不让模型连调三次 —— 三次往返在语音里能听出停顿，
     * 而用户说的是一句「都加进去」，本来就是一个动作。
     *
     * 清单里已有同名待购项由云端跳过（和「吃完顺手加」同一条去重规则）。
     * 跳过了要说出来：用户以为加了三样、实际只进了一样，不说就成了静默失败。 */
    std::string DoShop(const std::string &names) {
        if (names.empty()) return "没听清要买什么，再说一次？";

        int added = 0, skipped = 0;
        if (!device_op_shop(kBase, names.c_str(), &added, &skipped)) {
            return "没能加进购物清单，可能是网络问题，等会儿再试。";
        }
        if (added == 0 && skipped > 0) return "这些清单里都已经有了，没重复加。";
        if (added == 0) return "一样都没加上，再说一次试试？";

        std::string r = "好，加了 " + std::to_string(added) + " 样到购物清单。";
        if (skipped > 0) r += "另外 " + std::to_string(skipped) + " 样清单里已经有了。";
        return r;
    }

    /* 语音录入。**保质期不在这里算** —— 云端按知识库先验给，
     * 设备不带那份知识库，也不该带：同一样东西从手机加和用嘴加
     * 必须落成同一个结果（tests/shelflife-sync.test.js 守着这一点）。 */
    std::string DoAdd(const std::string &name, int qty, const std::string &unit) {
        if (name.empty()) return "没听清要加什么，再说一次名字？";
        if (!device_op_add(kBase, name.c_str(), qty, unit.c_str())) {
            return "没能加进去，可能是网络问题，等会儿再试。";
        }
        /* 立刻同步，让屏幕和后续提问都看到新条目 ——
         * 否则用户刚说完「加了牛奶」，再问「冰箱里有什么」却听不到它。 */
        SyncOnce();
        ApplyPage(true);
        return "好，" + name + " 加进去了。保质期是按常识估的，"
               "包装上有日期的话在手机上补一下更准。";
    }

private:
    /** 放了几天 —— 这是**事实**（从 createdAt 数出来的），不是估计。 */
    int DaysIn(const sync_item_t &it, int64_t now_ms) const {
        if (it.created_at <= 0) return -1;
        return (int)((now_ms - it.created_at) / 86400000LL);
    }

    /** 走完了多少比例的预计寿命。>= 1 就该看看了。
     *  按比例而不是绝对天数：一瓶放了 30 天的酱油不该排在放了 5 天的菠菜前面。 */
    double Progress(const sync_item_t &it, int64_t now_ms) const {
        int64_t span = it.expire_at - it.created_at;
        if (span <= 0) return 0;
        return (double)(now_ms - it.created_at) / (double)span;
    }

    std::string DescribeAll() {
        xSemaphoreTake(items_mutex_, portMAX_DELAY);
        int n = item_count_;
        bool tr = items_truncated_;
        std::string out;
        int64_t now_ms = (int64_t)time(nullptr) * 1000;
        for (int i = 0; i < n; i++) {
            char line[128];
            int days = DaysIn(items_[i], now_ms);
            snprintf(line, sizeof(line), "%s %d%s，放了 %d 天; ",
                     items_[i].name, items_[i].qty,
                     items_[i].unit[0] ? items_[i].unit : "份",
                     days < 0 ? 0 : days);
            out += line;
        }
        xSemaphoreGive(items_mutex_);

        if (n == 0) return "冰箱里现在是空的。";
        /* 截断了就明说 —— 把不完整的清单当成全部念出来是在撒谎。 */
        if (tr) out += "（只列了前面这些，还有更多，完整清单看手机。）";
        return out;
    }

    std::string DescribeExpiring() {
        xSemaphoreTake(items_mutex_, portMAX_DELAY);
        std::string out;
        int hit = 0;
        int64_t now_ms = (int64_t)time(nullptr) * 1000;
        for (int i = 0; i < item_count_; i++) {
            if (Progress(items_[i], now_ms) < 1.0) continue;
            char line[128];
            int days = DaysIn(items_[i], now_ms);
            snprintf(line, sizeof(line), "%s（放了 %d 天）; ",
                     items_[i].name, days < 0 ? 0 : days);
            out += line;
            hit++;
        }
        xSemaphoreGive(items_mutex_);

        if (hit == 0) return "现在没有特别需要处理的，都还早。";
        /* **措辞不能断言「已过期」**：系统并不掌握这个事实，expireAt 只是先验
         * （知识库或 spoilage 学出来的估计）。只报事实 + 提醒去看一眼。 */
        return "这些放得比较久了，建议看一眼：" + out;
    }

    /* 按名字定位并执行。**唯一命中才动手** —— 硬约束 6：
     * 语音解析失败一律追问，禁止猜测执行，删除是破坏性操作。
     * 零命中和多命中都把情况原样交回给模型，让它开口问用户。 */
    std::string DoOp(const char *op, const std::string &name) {
        if (name.empty()) return "没听清是哪一样，请再说一次名字。";

        xSemaphoreTake(items_mutex_, portMAX_DELAY);
        int hits = 0, oldest = -1;
        bool all_same_name = true;
        std::string first_name, distinct;
        for (int i = 0; i < item_count_; i++) {
            if (std::string(items_[i].name).find(name) == std::string::npos) continue;
            if (hits == 0) {
                first_name = items_[i].name;
            } else if (first_name != items_[i].name) {
                all_same_name = false;
            }
            /* 同名时取**最早放进去**的那个：它离预计寿命最近，
             * 也是人真的会先拿走的那一份。 */
            if (oldest < 0 || items_[i].created_at < items_[oldest].created_at) oldest = i;
            if (distinct.find(items_[i].name) == std::string::npos) {
                distinct += items_[i].name;
                distinct += "、";
            }
            hits++;
        }
        std::string id = (oldest >= 0) ? items_[oldest].id : "";
        std::string hit_name = (oldest >= 0) ? items_[oldest].name : "";
        xSemaphoreGive(items_mutex_);

        if (hits == 0) return "冰箱里没找到「" + name + "」，要不要换个说法？";

        /* **名字不同才追问。**
         *
         * 硬约束 6 的用意是「用户的意图不明确时不要猜」。而两样东西名字
         * 一模一样时意图恰恰是明确的 —— 丢哪个都一样，此时再问
         * 「说具体一点是哪个」就是死胡同：用户根本没有别的说法。
         * 真正该追问的是说「牛奶」而冰箱里有鲜牛奶和酸牛奶那种情况。 */
        if (hits > 1 && !all_same_name) {
            return "有好几样都对得上：" + distinct + "说具体一点是哪个？";
        }

        if (!device_op(kBase, op, id.c_str())) {
            return "没能改成功，可能是网络问题，等会儿再试。";
        }
        /* 立刻同步一次，让屏幕跟上 —— 否则要等下一个轮询周期才看得到变化。 */
        SyncOnce();
        ApplyPage(true);
        std::string reply = "好，" + hit_name +
                            (strcmp(op, "eaten") == 0 ? " 标记成吃完了。" : " 已经移走了。");
        /* 有多份时说清楚还剩几份 —— 否则用户不知道刚才动的是哪一个，
         * 也不知道要不要再说一次。 */
        if (hits > 1) {
            reply += "冰箱里还有 " + std::to_string(hits - 1) + " 份同名的。";
        }
        return reply;
    }

    /* 配对界面：白底 + 边框 + 一个大号的 6 位数字。
     *
     * **只画数字**，不画中文 —— 固件里的 8×16 字模只有 0-9 和几个符号，
     * 为一个一次性的配对界面另加一整套汉字库不值得。
     * 用户在小程序里会看到「在便利贴屏幕上找 6 位数字」，够了。 */
    void DrawPairScreen(const char *code) {
        if (display_ == nullptr) return;
        const size_t len = (size_t)EXAMPLE_LCD_WIDTH * EXAMPLE_LCD_HEIGHT / 8;
        if (last_frame_ == nullptr) last_frame_ = (uint8_t *)malloc(len);
        if (last_frame_ == nullptr) return;
        memset(last_frame_, 0, len);
        const int stride = EXAMPLE_LCD_WIDTH / 8;
        auto px = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= EXAMPLE_LCD_WIDTH || y >= EXAMPLE_LCD_HEIGHT) return;
            last_frame_[y * stride + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
        };

        for (int x = 20; x < EXAMPLE_LCD_WIDTH - 20; x++) { px(x, 20); px(x, 21);
            px(x, EXAMPLE_LCD_HEIGHT - 22); px(x, EXAMPLE_LCD_HEIGHT - 21); }
        for (int y = 20; y < EXAMPLE_LCD_HEIGHT - 20; y++) { px(20, y); px(21, y);
            px(EXAMPLE_LCD_WIDTH - 22, y); px(EXAMPLE_LCD_WIDTH - 21, y); }

        /* 点阵放大只能取整数倍，非整数倍会糊。5 倍 → 每个字 40×80。 */
        const int S = 5, GAP = 8;
        int n = (int)strlen(code);
        int w = n * 8 * S + (n - 1) * GAP;
        int cx = (EXAMPLE_LCD_WIDTH - w) / 2;
        int cy = (EXAMPLE_LCD_HEIGHT - 16 * S) / 2;
        for (int i = 0; i < n; i++) {
            const uint8_t *g = glyph_for(code[i]);
            if (g != nullptr) {
                for (int r = 0; r < 16; r++) {
                    for (int c = 0; c < 8; c++) {
                        if (!(g[r] & (0x80 >> c))) continue;
                        for (int dy = 0; dy < S; dy++)
                            for (int dx = 0; dx < S; dx++)
                                px(cx + c * S + dx, cy + r * S + dy);
                    }
                }
            }
            cx += 8 * S + GAP;
        }

        /* 配对期间屏幕归我们 —— 否则 LVGL 的配网提示会盖住这个码。 */
        display_->SetRawMode(true);
        display_->WriteRaw1bpp(0, 0, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT, last_frame_, len);
        have_real_frame_ = false;   /* 这不是云端位图，别被 ApplyPage 当成冰箱页 */
    }

    void EnsurePlaceholderFrame() {
        const size_t len = (size_t)EXAMPLE_LCD_WIDTH * EXAMPLE_LCD_HEIGHT / 8;
        if (last_frame_ == nullptr) last_frame_ = (uint8_t *)malloc(len);
        if (last_frame_ == nullptr) return;
        memset(last_frame_, 0, len);   /* 0 = 白 */

        const int stride = EXAMPLE_LCD_WIDTH / 8;
        for (int x = 0; x < EXAMPLE_LCD_WIDTH; x++) {
            for (int y : {8, 9, EXAMPLE_LCD_HEIGHT - 10, EXAMPLE_LCD_HEIGHT - 9}) {
                last_frame_[y * stride + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
            }
        }
        for (int y = 8; y < EXAMPLE_LCD_HEIGHT - 8; y++) {
            for (int x : {8, 9, EXAMPLE_LCD_WIDTH - 10, EXAMPLE_LCD_WIDTH - 9}) {
                last_frame_[y * stride + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
            }
        }
        have_real_frame_ = false;
    }

    void ApplyPage(bool force) {
        auto state = Application::GetInstance().GetDeviceState();
        bool idle = (state == kDeviceStateIdle);
        /* **不再要求已有云端位图。** 有就画位图，没有就画占位页 ——
         * 原来那个 last_frame_ != nullptr 的守卫让原始模式在 sync 失败时
         * 永远打不开，表现成「切屏没反应」。 */
        if (page_fridge_ && last_frame_ == nullptr) EnsurePlaceholderFrame();
        /* **说话时冰箱页不让位。** 原来是 `&& idle`，让位给小智画字幕，
         * 实测一次对话十几次刷屏 —— 每句 TTS 一次 SetChatMessage，加上状态和
         * 表情切换，墨水屏一直在闪。
         *
         * 而这是一张贴在冰箱上的便利贴：**对话是听的，不是读的**。为了一段
         * 没人会去读的字幕让整屏闪十几次，不划算。想看对话界面按上键切过去 ——
         * 那是用户主动要看，闪也认了。 */
        /* **有了真实位图之后，屏幕就永久归冰箱页。**
         *
         * 之前留了个上键切到小智页，实测是个坑：小智的界面不是全屏不透明的，
         * 它只画状态栏、表情和字幕，底下的冰箱位图会透出来 —— 两套 UI 叠在
         * 一起，比哪一套单独显示都难看。而用户要的就是「别显示小智的界面」。
         *
         * 但**拿到位图之前**要让 LVGL 画：配网提示、激活码这些只在它那儿，
         * 挡住的话首次配置就没法做了。 */
        bool want_raw = have_real_frame_;

        if (!force && want_raw == display_->raw_mode()) {
            was_busy_ = !idle;
            return;
        }
        display_->SetRawMode(want_raw);
        if (want_raw) {
            display_->WriteRaw1bpp(0, 0, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT,
                                   last_frame_,
                                   (size_t)EXAMPLE_LCD_WIDTH * EXAMPLE_LCD_HEIGHT / 8);
            UpdateStatusStrip(true);
        }
        was_busy_ = !idle;
    }

public:
    void TogglePage() {
        page_fridge_ = !page_fridge_;
        ESP_LOGI(TAG, "切页 -> %s%s", page_fridge_ ? "冰箱页" : "小智页",
                 (page_fridge_ && !have_real_frame_) ? "（占位页：还没拿到云端位图）" : "");
        ApplyPage(true);
    }

private:

    /* 时间 + 电量，右对齐画进状态条带，**只局刷这一块**。
     * 内容和上次一样就完全不碰屏幕 —— 不做这个判断等于每秒局刷一次，
     * 残影很快积起来。 */
    void UpdateStatusStrip(bool force) {
        if (display_ == nullptr) return;
        /* **只在冰箱页画。** 小智页由 LVGL 管，它自己的状态栏就有时间和电量
         * （TZ 修好后显示是对的）。两边都画的话我们的条带会被对话字幕盖住，
         * 还白白多刷一次屏。 */
        if (!display_->raw_mode()) return;

        int battery = -1;
        bool charging = false, discharging = false;
        Board::GetInstance().GetBatteryLevel(battery, charging, discharging);

        /* 时区由 board 构造时 setenv("TZ","CST-8")+tzset() 设好，这里直接用
         * localtime_r —— 和小智状态栏走同一套，一块屏上不会出现两个时间。
         * **别改回手工 +8 小时**：同一件事两套实现必然漂移。
         * 与云端 screen.js 的 TZ_MINUTES=480 是同一个约定。 */
        time_t nowsec = time(nullptr);
        struct tm t;
        localtime_r(&nowsec, &t);

        /* 全部钳位再格式化。不钳的话 GCC 无法为 %d 定界，报
         * -Werror=format-truncation；而且时钟没对准时 tm 字段本来就可能离谱，
         * 与其让屏上出现乱码，不如显示一个明显不对但有界的值。 */
        int mon = t.tm_mon + 1, day = t.tm_mday, hh = t.tm_hour, mm = t.tm_min;
        if (mon < 1 || mon > 12) mon = 0;
        if (day < 1 || day > 31) day = 0;
        if (hh < 0 || hh > 23) hh = 0;
        if (mm < 0 || mm > 59) mm = 0;
        if (battery > 100) battery = 100;

        /* **电量要迟滞。** ADC 读数在 98/99 之间来回抖，而「内容没变就不刷」
         * 比的是整串 —— 1% 的抖动就能让屏幕每秒局刷一次，残影很快积起来
         * （实测日志里 98/99/98/99 一路刷下去）。
         * 差 2% 以内一律沿用屏上那个值；真的在掉电时 2% 一样会跟上。 */
        if (battery >= 0) {
            if (shown_battery_ < 0 || battery <= shown_battery_ - 2 || battery >= shown_battery_ + 2) {
                shown_battery_ = battery;
            }
            battery = shown_battery_;
        }

        char text[32];
        if (battery >= 0) {
            snprintf(text, sizeof(text), "%02d-%02d %02d:%02d  %d%%",
                     mon, day, hh, mm, battery);
        } else {
            snprintf(text, sizeof(text), "%02d-%02d %02d:%02d", mon, day, hh, mm);
        }
        if (!force && strcmp(text, last_strip_) == 0) return;

        uint8_t strip[ST_STRIDE * ST_H];
        memset(strip, 0, sizeof(strip)); /* 0 = 白 */

        int w = (int)strlen(text) * 8;
        int cx = ST_W - w;
        if (cx < 0) cx = 0;

        for (const char *p = text; *p; p++) {
            const uint8_t *g = glyph_for(*p);
            if (g != nullptr) {
                for (int r = 0; r < 16; r++) {
                    for (int c = 0; c < 8; c++) {
                        if (!(g[r] & (0x80 >> c))) continue;
                        int px = cx + c, py = 2 + r;
                        if (px < 0 || px >= ST_W || py < 0 || py >= ST_H) continue;
                        strip[py * ST_STRIDE + (px >> 3)] |= (uint8_t)(0x80 >> (px & 7));
                    }
                }
            }
            cx += 8;
        }

        display_->WriteRaw1bpp(ST_X, ST_Y, ST_W, ST_H, strip, sizeof(strip));
        snprintf(last_strip_, sizeof(last_strip_), "%s", text);
        ESP_LOGI(TAG, "状态条带局刷 %s", text);
    }
};

/* 按键回调是 C 风格入口，需要一个全局把手。状态机单任务串行，无并发问题。 */
FridgeApp *g_app = nullptr;

void FridgeTask(void *arg) {
    auto *app = static_cast<FridgeApp *>(arg);
    app->Run();
    vTaskDelete(nullptr);
}

}  // namespace

void fridge_app_start(CustomLcdDisplay *display) {
    auto *app = new FridgeApp(display);
    g_app = app;
    /* 先注册工具再起任务 —— 见 RegisterMcpTools 的注释。 */
    app->RegisterMcpTools();
    /* 栈给足：HTTPS + mbedTLS 握手吃栈，v1 在这里栽过一次（主任务 3584 字节
     * 装不下 15KB 的 sync_result_t，崩在拿到 IP 之后，看起来像 TLS 的问题）。 */
    xTaskCreate(FridgeTask, "fridge", 8192, app, 4, nullptr);
}

void fridge_app_toggle_page(void) {
    if (g_app != nullptr) g_app->TogglePage();
}
