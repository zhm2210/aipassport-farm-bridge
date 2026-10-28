/*
 * aipassport_farm.ino — AI Passport (ESP32-C3) 拓竹农场遥控器
 *
 * 硬件：FoloToy AI Passport
 *   - ESP32-C3 160MHz / 8MB Flash / 无 PSRAM
 *   - ST7789P3 240x320 竖屏 (CS=1, SCLK=8, MOSI=9, DC=20, 背光=21, RST 未接)
 *   - 三键共享 GPIO0 ADC1_CH0 电阻梯：UP<=150mV / DOWN<=447mV / OK<=1900mV
 *   - USB Serial/JTAG (GPIO18/19) = 与 PC 桥接通信
 *
 * 中文显示：cnfont.h（由 tools/genfont.py 从系统字体生成的点阵字库）
 *
 * 依赖库：TFT_eSPI (Bodmer)、ArduinoJson (v7)
 *
 * 串口协议（115200，\n 分隔 JSON）：
 *   收 PC->工牌：
 *     {"t":"state","srv":{...},"n":{...},"d":[[名,状态,进度,任务,余时,喷嘴,热床,在线],...],"q":[[名,剩余,已派],...]}
 *     {"t":"event","kind":"pause","dev":"A1-01","reason":"..."}
 *     {"t":"ack","ok":true,"msg":"...","res":[["A1-01",0,""],...]}
 *   发 工牌->PC：
 *     {"t":"hello"} / {"t":"get_state"} / {"t":"ok","devs":33,...}
 *     {"t":"launch","task_id":"1_plate_5","dev_ids":["A1-01"],"ams_id":255,"slot_id":0}
 *     {"t":"opt","dev_id":"A1-01","opt":"pause"}
 */
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <vector>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include "cnfont.h"

// ================= 版本标记 =================
// 改固件就改这里，屏幕上和串口 ack 里都能看到，用来确认刷进去的是哪一版
#define BUILD_TAG "B1935"

// ================= 硬件引脚 =================
#define PIN_BTN_ADC   0
#define PIN_BL        21

// 按键电压阈值（mV）—— 2026-10-25 实机测得：
//   UP ≈ 10 mV / DOWN ≈ 315 mV / OK ≈ 604 mV / 松开 ≈ 3300 mV
//   （规格书写 "OK≤1900" 是上限，实际只有 604，之前用 1100 当界就把 OK 判成 DOWN 了）
#define BTN_UP_MAX    200
#define BTN_DOWN_MAX  450
#define BTN_OK_MAX    1200

// ================= 颜色 =================
#define C_BG     0x0863
#define C_CARD   0x18C4
#define C_CARD2  0x20E6
#define C_TEXT   0xE71C
#define C_DIM    0x8C71
#define C_ACCENT 0x5D9F
#define C_OK     0x2E58
#define C_WARN   0xFD20
#define C_ERR    0xF1C7
#define C_LINE   0x39E8
// 首页四张卡各用一个颜色（RGB565）
#define C_TEAL   0x1633   // 打印中：青绿 #16C79A
#define C_BLUE   0x3C5F   // 空闲：蓝   #3D8BFD
#define C_RED    0xFA94   // 异常：红   #FF5252
#define C_PURPLE 0xA35F   // 队列：紫   #A66BFF

TFT_eSPI tft;

// ================= 数据 =================
struct Dev {
  char name[12];
  char state[12];
  int  progress;
  char task[24];
  int  remain;
  int  nozzle;
  int  bed;
  bool online;
};
struct Task {
  char name[24];
  int  remaining;
  int  working;
  int  flags;      // bit0 自动调平 bit1 流量校准 bit2 延时摄影 bit3 AMS映射模型
};
struct Ev {
  char kind[10];
  char dev[12];
  char reason[56];
  char from[12];
  char to[12];
};
struct LRes {
  char name[12];
  int  code;
  char msg[40];
};

static int  g_run = 0, g_pause = 0, g_idle = 0, g_offline = 0, g_total = 0, g_queue = 0;
static char g_srvName[16] = "---";
static bool g_hasState = false;
static std::vector<Dev>   g_devs;
static std::vector<Task>  g_tasks;
static std::vector<Ev>    g_events;
static std::vector<LRes>  g_results;

// WiFi 凭据：优先用本地 wifi_config.h（不入库，见 .gitignore）；
// 没有该文件时回退到占位符 —— 保证 clone 下来不改名也能编译通过（只是连不上网）。
// 想连自己的 WiFi：把 wifi_config.h.example 复制成 wifi_config.h 再填即可。
#if defined(__has_include)
  #if __has_include("wifi_config.h")
    #include "wifi_config.h"
  #else
    #define WIFI_SSID   "YOUR_WIFI_SSID"
    #define WIFI_PASS   "YOUR_WIFI_PASSWORD"
  #endif
#else
  #include "wifi_config.h"
#endif
#define BRIDGE_DISC_PORT 8897
#define BRIDGE_TCP_PORT  8897

// ---- 无线链路状态 ----
#define WIFI_IDLE 0
#define WIFI_CONNECTING 1
#define WIFI_OK 2
#define WIFI_FAIL 3

static uint8_t  g_wifiState = WIFI_IDLE;
static unsigned long g_wifiT0 = 0;
static unsigned long g_wifiRetryAt = 0;
static char     g_bridgeIP[16] = "";
static WiFiClient g_tcp;
static WiFiServer g_tcpServer(BRIDGE_TCP_PORT);
static WiFiUDP   g_udp;
static unsigned long g_lastDisc = 0;
static bool     g_udpReady = false;
static bool     g_otaReady = false;

// ---- 走业务链路的无线升级（OTA over link）----
// 电脑防火墙拦入站，标准 espota 要"板子回连 PC"根本连不上；
// 所以改成顺着已有的「PC → 桥接 → 工牌」这条出站链路把固件推下来。
static bool      g_linkOta = false;    // 正在收固件
static size_t    g_otaTotal = 0;
static size_t    g_otaDone  = 0;
static uint8_t   g_otaBuf[1600];
static int       g_otaPct   = -1;
static unsigned long g_otaLastMs = 0;   // 最后一次收到数据的时间（超时自救用）
static int           g_rxChan = 0;      // 当前命令是从哪来的：0=USB串口 1=WiFi TCP
static unsigned long g_pongAt = 0;      // 最近一次收到桥接回应的时间（判断连接死没死）
static unsigned long g_pongGot = 0;     // 收到过几次 pong（排错用）
static unsigned long g_pingSent = 0;    // 发出去几个 ping
static unsigned long g_pongSeenAt = 0;  // 收到最近一个 pong 时，已发了几个 ping

// 无线升级初始化（定义在本文件后面，因为要用到 drawCNCenter）
static void otaBegin();


static unsigned long g_rxBytes = 0, g_rxLines = 0, g_rxFail = 0;

// ================= 页面 =================
enum { P_HOME, P_DEVICES, P_DEV_ACTION, P_CLEAN, P_TASKS, P_SELECT, P_OPTIONS, P_PRINTOPT, P_RESULT, P_EVENTS, P_SETTINGS, P_SLEEP, P_RING, P_TONESEL };
static int  g_page = P_HOME;
static int  g_tab = 0;
static int  g_sel = 0;
static int  g_selDev = -1;
static int  g_selTask = -1;
static bool g_needRedraw = true;

// 首页焦点：0..3 = 四张统计卡（打印中/空闲/异常/队列），4=事件 5=设置 6=全部设备
static int  g_focus = 0;
#define HOME_FOCUS_MAX 6
// 下发时的打印设置（默认：自动调平开 / 流量校准开 / 延时摄影关）
static bool g_poLevel = true;
static bool g_poFlow  = true;
static bool g_poTime  = false;

// 设备列表过滤：-1=全部 0=打印中 1=空闲 2=异常（暂停/失败/离线）
static int  g_devFilter = -1;
static int  g_devView[64];     // 当前视图里的设备下标
static int  g_devViewN = 0;

static std::vector<uint8_t> g_picked;
static int  g_amsIdx = 0;
static int  g_launching = 0;

static char g_toast[48] = "";
static unsigned long g_toastAt = 0;
static void showToast(const char* msg) {
  strlcpy(g_toast, msg, sizeof(g_toast));
  g_toastAt = millis();
  g_needRedraw = true;
}

// ================= 电池（CW2017 电量计，I2C0）=================
#define BAT_I2C_ADDR 0x63
#define PIN_I2C_SDA  10
#define PIN_I2C_SCL  7
static int   g_battPct = -1;     // -1 = 读不到
static float g_battV = 0;
static bool  g_battReady = false;

static void batteryInit() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  delay(60);
  // 唤醒：把 MODE 写成 0x00（正常模式）
  Wire.beginTransmission(BAT_I2C_ADDR);
  Wire.write(0x08); Wire.write(0x00);
  Wire.endTransmission();
  delay(120);
  g_battReady = true;
}

static uint16_t batRead16(uint8_t reg) {
  Wire.beginTransmission(BAT_I2C_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  uint8_t n = Wire.requestFrom(BAT_I2C_ADDR, (uint8_t)2);
  if (n != 2) return 0xFFFF;
  uint8_t hi = Wire.read(), lo = Wire.read();
  return (uint16_t)((hi << 8) | lo);
}
static uint8_t batRead8(uint8_t reg) {
  Wire.beginTransmission(BAT_I2C_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  uint8_t n = Wire.requestFrom(BAT_I2C_ADDR, (uint8_t)1);
  return (n == 1) ? Wire.read() : 0xFF;
}

/** 单节锂电（4.2V 满充）放电曲线 → 百分比 */
static int socFromVoltage(float mv) {
  static const float cv[] = { 4200, 4100, 4000, 3900, 3800, 3700, 3600, 3500, 3400 };
  static const int   cp[] = {  100,   92,   84,   72,   55,   35,   18,    8,    0 };
  if (mv >= 4200) return 100;
  if (mv <= 3400) return 0;
  for (int i = 0; i < 8; i++) {
    if (mv >= cv[i + 1] && mv <= cv[i]) {
      float f = (mv - cv[i + 1]) / (cv[i] - cv[i + 1]);
      return cp[i + 1] + (int)(f * (cp[i] - cp[i + 1]));
    }
  }
  return -1;
}

/** 读一次电量。
 *  实测：0x02-0x03 = VCELL(0.3125mV/LSB) 可靠；0x04 的 SOC 字节在这块芯片上无效(0xFE)，
 *  所以百分比按放电曲线换算。 */
static void batteryRead() {
  if (!g_battReady) return;
  uint16_t r02 = batRead16(0x02);
  uint16_t r04 = batRead16(0x04);
  if (r02 == 0xFFFF && r04 == 0xFFFF) { g_battPct = -1; return; }

  float v02 = (r02 & 0x3FFF) * 0.3125f;
  float v04 = (r04 & 0x3FFF) * 0.3125f;
  uint8_t socRaw = batRead8(0x04);

  if (v02 > 2500 && v02 < 4600) g_battV = v02;
  else if (v04 > 2500 && v04 < 4600) g_battV = v04;
  else g_battV = 0;

  int byCurve = (g_battV > 2500) ? socFromVoltage(g_battV) : -1;
  if (socRaw <= 100 && socRaw > 0 && byCurve >= 0) {
    // 原始 SOC 可用就用原始值，否则用曲线
    g_battPct = (socRaw >= byCurve - 25 && socRaw <= byCurve + 25) ? socRaw : byCurve;
  } else {
    g_battPct = byCurve;
  }

  // 把 0x00-0x1F 原始寄存器打出来，方便对出这块芯片的真实布局
  {
    uint8_t buf[32];
    Wire.beginTransmission(BAT_I2C_ADDR);
    Wire.write(0x00);
    Wire.endTransmission(false);
    uint8_t n = Wire.requestFrom(BAT_I2C_ADDR, (uint8_t)32);
    for (uint8_t i = 0; i < n && i < 32; i++) buf[i] = Wire.read();
    char hex[80];
    int k = 0;
    for (uint8_t i = 0; i < n && i < 24 && k < 60; i++)
      k += snprintf(hex + k, sizeof(hex) - k, "%02x", buf[i]);
    hex[k] = 0;
    char d[100];
    snprintf(d, sizeof(d), "{\"t\":\"batdump\",\"d\":\"%s\"}", hex);
    sendJson(d);
  }
}

// ================= 中文渲染（cnfont.h 点阵字库）=================
static uint16_t g_glyphBuf[16 * 16];

static const CnGlyph* cnLookup(uint16_t cp) {
  int lo = 0, hi = CN16_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (CN16_MAP[mid].cp == cp) return &CN16_MAP[mid];
    if (CN16_MAP[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

static uint16_t utf8Next(const char** p) {
  const uint8_t* s = (const uint8_t*)*p;
  uint16_t cp;
  if (s[0] < 0x80) { cp = s[0]; *p += 1; }
  // 续字节必须校验：字符串被按字节截断（snprintf 常见）时会读到半个汉字，
  // 不校验就会把后面的字节当汉字首字节 → 画出乱码。
  else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); *p += 2; }
  else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); *p += 3;
  }
  else { *p += 1; return '?'; }
  return cp;
}

/** 丢掉末尾被截断的半个汉字（snprintf 按字节截断会切断 UTF-8 → 显示成乱码/乱框） */
static void trimUtf8Tail(char* s) {
  int n = (int)strlen(s);
  if (n == 0) return;
  int i = n - 1, cont = 0;
  while (i >= 0 && ((unsigned char)s[i] & 0xC0) == 0x80) { i--; cont++; }
  if (i < 0) { s[0] = 0; return; }
  unsigned char c = (unsigned char)s[i];
  int need = 0;
  if ((c & 0xE0) == 0xC0) need = 1;
  else if ((c & 0xF0) == 0xE0) need = 2;
  else if ((c & 0xF8) == 0xF0) need = 3;
  if (cont < need) s[i] = 0;      // 这个字被切了，整个丢掉
}

static int cnWidth(const char* s, int scale) {
  int w = 0;
  const char* p = s;
  while (*p) {
    uint16_t cp = utf8Next(&p);
    const CnGlyph* g = cnLookup(cp);
    w += (g ? g->w : 8) * scale;
  }
  return w;
}

static void drawCN(int x, int y, const char* s, uint16_t fg, uint16_t bg, int scale = 1) {
  const char* p = s;
  int cx = x;
  while (*p) {
    uint16_t cp = utf8Next(&p);
    const CnGlyph* g = cnLookup(cp);
    if (!g) {
      // 字库里没有这个字：画个空框占位，一眼能看出是缺字（而不是重叠/覆盖）
      tft.drawRect(cx + 1, y + 1, 8 * scale - 2, 16 * scale - 2, fg);
      cx += 8 * scale;
      continue;
    }
    int gw = g->w, gh = g->h, stride = (gw + 7) / 8;
    int n = gw * gh;
    for (int i = 0; i < n; i++) {
      int row = i / gw, col = i % gw;
      bool on = CN16_BITS[g->off + row * stride + col / 8] & (0x80 >> (col & 7));
      // ★ pushImage 按大端推字节，RGB565 存的是小端 → 这里手动交换
      uint16_t c = on ? fg : bg;
      g_glyphBuf[i] = (uint16_t)((c >> 8) | (c << 8));
    }
    if (scale == 1) {
      tft.pushImage(cx, y, gw, gh, g_glyphBuf);
      cx += gw;
    } else {
      // 放大路径：缓冲里存的是交换过的字节，这里换回来再画
      for (int row = 0; row < gh; row++)
        for (int col = 0; col < gw; col++) {
          uint16_t c = g_glyphBuf[row * gw + col];
          c = (uint16_t)((c >> 8) | (c << 8));
          tft.fillRect(cx + col * scale, y + row * scale, scale, scale, c);
        }
      cx += gw * scale;
    }
  }
}
static void drawCNCenter(int cx, int y, const char* s, uint16_t fg, uint16_t bg, int scale = 1) {
  drawCN(cx - cnWidth(s, scale) / 2, y, s, fg, bg, scale);
}
static void drawCNRight(int rx, int y, const char* s, uint16_t fg, uint16_t bg, int scale = 1) {
  drawCN(rx - cnWidth(s, scale), y, s, fg, bg, scale);
}

// ================= 息屏 / 休眠 =================
// 屏幕是最耗电的（背光 + LCD）。息屏时关背光 + 清屏，但 WiFi/TCP 保持在线，
// 这样远程下发任务仍然有效，任意按键即可唤醒。
static bool g_screenOff = false;
static unsigned long g_lastAct = 0;

// 自动息屏时间（分钟），可在「设置」里改，存 NVS 掉电不丢
static int  g_sleepMin = 10;
static bool g_alertOn  = true;   // 声音告警开关（设置页可调，掉电保存）
static int  g_alertVol = 80;     // 告警音量档位：40/60/80/100/120，120 = 超大
// 音量档位表（画界面和切档都要用，放前面）
static const int VOL_STEPS[5] = { 40, 60, 80, 100, 120 };
static int  g_toneFinish = 0;    // 完成音效选择（0..3）
static int  g_toneFault  = 0;    // 故障音效选择（0..3）
static int  g_toneSel    = 0;    // 正在选哪一类：0=完成 1=故障
// 音效名字（铃声页显示用）
static const char* FINISH_NAMES[4] = { "琶音", "双音", "清脆", "欢快" };
static const char* FAULT_NAMES[4]  = { "三连", "长鸣", "四短", "低沉" };
static int  g_customMin = 15;     // 「自定义」档的数值
static bool g_editSleep = false;  // 自定义档是否处于调节中
// 前向声明：播告警音（定义在文件后面的音频段，铃声页要用）
// 0=完成 1=异常 2=提示；force=true 无视开关（手动试听用）
static void playAlert(int kind, bool force = false);
static const int SLEEP_PRESETS[4] = { 10, 20, 30, 60 };

static unsigned long idleSleepMs() {
  if (g_sleepMin < 1) return 600000UL;   // 兜底
  return (unsigned long)g_sleepMin * 60000UL;
}

static void screenOff() {
  if (g_screenOff) return;
  g_screenOff = true;
  ledcWrite(0, 0);          // 背光关
  tft.fillScreen(TFT_BLACK);
  Serial.println("[sleep] screen off");
}
static void screenOn() {
  g_lastAct = millis();
  if (!g_screenOff) return;
  g_screenOff = false;
  ledcWrite(0, 900);         // 背光开
  g_needRedraw = true;
  Serial.println("[sleep] screen on");
}
static bool isScreenOff() { return g_screenOff; }

// ================= 串口 =================
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  #define FARM_SERIAL Serial
#else
  #define FARM_SERIAL USBSerial
#endif
static String g_lineSer;   // USB 串口的行缓冲
static String g_lineTcp;   // WiFi TCP 的行缓冲（必须和串口分开，否则会互相搅乱）
// 传输层：有 TCP 就走无线，否则走 USB 串口
static void sendJson(const char* j) {
  if (g_tcp.connected()) {
    g_tcp.print(j);
    g_tcp.print("\n");
  } else {
    FARM_SERIAL.println(j);
  }
}
static void sendGetState() { sendJson("{\"t\":\"get_state\"}"); }

// ================= 状态中文 =================
static const char* stateCN(const char* s) {
  if (!strcmp(s, "RUNNING")) return "打印中";
  if (!strcmp(s, "PREPARE")) return "准备中";
  if (!strcmp(s, "PAUSE") || !strcmp(s, "PAUSED")) return "已暂停";
  if (!strcmp(s, "FAILED"))  return "打印失败";
  if (!strcmp(s, "IDLE"))    return "空闲";
  if (!strcmp(s, "FINISH"))  return "已完成";
  if (!strcmp(s, "SLICING")) return "切片中";
  return "未知状态";
}
static uint16_t stateColor(const char* s) {
  if (!strcmp(s, "RUNNING") || !strcmp(s, "PREPARE")) return C_OK;
  if (!strcmp(s, "PAUSE") || !strcmp(s, "PAUSED"))    return C_WARN;
  if (!strcmp(s, "FAILED"))                            return C_ERR;
  if (!strcmp(s, "IDLE") || !strcmp(s, "FINISH"))      return C_DIM;
  return C_ACCENT;
}
static bool isIdle(const Dev& d) {
  return d.online && (!strcmp(d.state, "IDLE") || !strcmp(d.state, "FINISH"));
}

// ================= 通用绘制 =================
static void drawHeader(const char* title, const char* right = nullptr) {
  tft.fillRect(0, 0, 240, 28, C_CARD);
  drawCN(8, 6, title, C_TEXT, C_CARD);
  if (right) drawCNRight(232, 6, right, C_DIM, C_CARD);
  else drawCNRight(232, 6, g_srvName, C_DIM, C_CARD);
  tft.drawFastHLine(0, 28, 240, C_LINE);
}

/** 顶部右上角：电池图标 + 电量（会清掉该区域，覆盖服务器名） */
static void drawBatteryBadge() {
  int pct = g_battPct;
  char b[12] = "??%";
  if (pct >= 0) snprintf(b, sizeof(b), "%d%%", pct);
  int tw = cnWidth(b, 1);
  tft.fillRect(150, 0, 90, 28, C_CARD);      // 清掉服务器名
  drawBatteryIcon(232 - tw - 20, 9, pct, C_DIM, C_CARD);
  drawCNRight(232, 10, b, pct < 0 ? C_DIM : (pct > 20 ? C_TEXT : C_WARN), C_CARD);
}
static void drawFooter(const char* hint) {
  tft.fillRect(0, 296, 240, 24, C_CARD);
  tft.drawFastHLine(0, 296, 240, C_LINE);
  if (g_toast[0] && millis() - g_toastAt < 2500) {
    drawCNCenter(120, 299, g_toast, C_ACCENT, C_CARD);
  } else {
    drawCNCenter(120, 299, hint, C_DIM, C_CARD);
  }
}
static void drawBar(int x, int y, int w, int h, int pct, uint16_t col) {
  tft.drawRoundRect(x, y, w, h, 2, C_DIM);
  int fill = (w - 4) * (pct < 0 ? 0 : (pct > 100 ? 100 : pct)) / 100;
  if (fill > 0) tft.fillRoundRect(x + 2, y + 2, fill, h - 4, 1, col);
}

/** 画电池图标 */
static void drawBatteryIcon(int x, int y, int pct, uint16_t fg, uint16_t bg) {
  if (pct < 0) {
    tft.drawRect(x, y, 16, 9, C_DIM);
    tft.fillRect(x + 16, y + 2, 2, 5, C_DIM);
    return;
  }
  uint16_t col = (pct > 20) ? C_OK : C_WARN;
  tft.drawRect(x, y, 16, 9, fg);
  tft.fillRect(x + 16, y + 2, 2, 5, fg);
  int fill = (16 - 4) * pct / 100;
  if (fill > 0) tft.fillRect(x + 2, y + 2, fill, 5, col);
}

// ================= 页：总览 =================
/** 统计各类设备数量（离线优先判定，不再计入其它类） */
static void statCounts(int& run, int& idle, int& pause, int& failed, int& offline) {
  run = idle = pause = failed = offline = 0;
  for (auto& d : g_devs) {
    if (!d.online) { offline++; continue; }
    if (!strcmp(d.state, "RUNNING") || !strcmp(d.state, "PREPARE")) run++;
    else if (!strcmp(d.state, "IDLE") || !strcmp(d.state, "FINISH")) idle++;
    else if (!strcmp(d.state, "PAUSE") || !strcmp(d.state, "PAUSED")) pause++;
    else if (!strcmp(d.state, "FAILED")) failed++;
  }
}
/** 正在打印的机台里最快还要多少分钟 */
static int minRemain() {
  int m = 0;
  for (auto& d : g_devs) {
    if ((!strcmp(d.state, "RUNNING") || !strcmp(d.state, "PREPARE")) && d.remain > 0)
      if (m == 0 || d.remain < m) m = d.remain;
  }
  return m;
}

static void drawHome() {
  tft.fillScreen(C_BG);
  drawHeader("拓竹农场");
  drawBatteryBadge();

  if (!g_hasState) {
    drawCNCenter(120, 96, "正在连接电脑端服务…", C_TEXT, C_BG);
    drawCNCenter(120, 120, "请确认桥接正在运行", C_DIM, C_BG);
    char buf[64];
    snprintf(buf, sizeof(buf), "已收 %lu 字节 %lu 行", g_rxBytes, g_rxLines);
    drawCNCenter(120, 150, buf, C_WARN, C_BG);
    drawCNCenter(120, 178, "工牌会自动连局域网", C_DIM, C_BG);
    drawCNCenter(120, 206, "固件版本 " BUILD_TAG, C_ACCENT, C_BG);
    drawFooter("按 OK 重新请求");
    return;
  }

  int run = 0, idle = 0, pause = 0, failed = 0, offline = 0;
  statCounts(run, idle, pause, failed, offline);
  int abnormal = pause + failed + offline;

  // ---- 2x2 四张统计卡：打印中 / 空闲 / 异常 / 队列 ----
  // 四张卡四种颜色，一眼分得清：青绿=在跑 蓝=闲着 红=出事 紫=排队
  const int cw = 112, cardH = 72, gap = 8, x0 = 8, y0 = 34;
  const char* labels[4] = { "打印中", "空闲", "异常", "队列" };
  int vals[4] = { run, idle, abnormal, g_queue };
  uint16_t numCol[4] = { C_TEAL, C_BLUE, abnormal ? C_RED : C_DIM, C_PURPLE };
  for (int i = 0; i < 4; i++) {
    int x = x0 + (i & 1) * (cw + gap);
    int y = y0 + (i >> 1) * (cardH + gap);
    bool foc = (g_focus == i);
    uint16_t bg = foc ? C_CARD2 : C_CARD;
    tft.fillRoundRect(x, y, cw, cardH, 6, bg);
    // 聚焦时边框用这张卡自己的颜色（不再是统一的强调色）
    if (foc) tft.drawRoundRect(x, y, cw, cardH, 6, numCol[i]);
    // 左侧竖色条：不聚焦也能一眼看出这张卡是什么
    tft.fillRoundRect(x + 4, y + 8, 5, cardH - 16, 2, numCol[i]);
    drawCN(x + 14, y + 5, labels[i], foc ? C_TEXT : C_DIM, bg);
    tft.setTextColor(numCol[i], bg);
    tft.setTextDatum(TC_DATUM);
    tft.drawNumber(vals[i], x + cw / 2, y + 21, 4);
    char sub[18];
    if (i == 0) { int m = minRemain(); if (m > 0) snprintf(sub, sizeof(sub), "最快%d分", m); else strlcpy(sub, "工作中", sizeof(sub)); }
    else if (i == 1) strlcpy(sub, "可下发", sizeof(sub));
    else if (i == 2) snprintf(sub, sizeof(sub), "停%d 障%d", pause, failed + offline);
    else snprintf(sub, sizeof(sub), "%d个任务", g_queue);
    drawCN(x + 14, y + 52, sub, C_DIM, bg);
  }

  // ---- 最近动态（最多 2 条）----
  int ly = 192;
  drawCN(8, ly, "最近动态", C_DIM, C_BG);
  ly += 20;
  if (!g_events.empty()) {
    for (int i = 0; i < 2 && i < (int)g_events.size(); i++) {
      Ev& e = g_events[i];
      uint16_t c = !strcmp(e.kind, "pause") ? C_WARN
                 : (!strcmp(e.kind, "fail") || !strcmp(e.kind, "hms") ? C_ERR
                 : (!strcmp(e.kind, "finish") ? C_OK : C_ACCENT));
      char b[44];
      if (!strcmp(e.kind, "pause")) snprintf(b, sizeof(b), "%s 暂停 %s", e.dev, e.reason);
      else if (!strcmp(e.kind, "finish")) snprintf(b, sizeof(b), "%s 打印完成", e.dev);
      else if (!strcmp(e.kind, "resume")) snprintf(b, sizeof(b), "%s 已恢复", e.dev);
      else if (!strcmp(e.kind, "hms")) snprintf(b, sizeof(b), "%s 异常", e.dev);
      else if (!strcmp(e.kind, "new")) snprintf(b, sizeof(b), "%s 已上线", e.dev);
      else if (!strcmp(e.kind, "launch")) snprintf(b, sizeof(b), "%s 任务下发", e.dev);
      else if (e.from[0] && e.to[0]) snprintf(b, sizeof(b), "%s %s→%s", e.dev, stateCN(e.from), stateCN(e.to));
      else snprintf(b, sizeof(b), "%s 通知", e.dev);   // 兜底：未知类型也用中文，不露原始英文 kind
      trimUtf8Tail(b);            // 原因文本是服务器下发的，按字节截断会切出半个汉字
      drawCN(8, ly, b, c, C_BG);
      ly += 18;
    }
  } else {
    drawCN(8, ly, "暂无动态", C_DIM, C_BG);
  }

  // ---- 底部三个入口：事件 / 设置 / 全部设备 ----
  const char* tabs2[3] = { "事件", "设置", "全部设备" };
  int tw2 = 80;
  for (int i = 0; i < 3; i++) {
    int x = i * tw2;
    bool act = (g_focus == 4 + i);
    tft.fillRect(x, 272, tw2, 22, act ? C_CARD : C_BG);
    drawCNCenter(x + tw2 / 2, 275, tabs2[i], act ? C_ACCENT : C_DIM, act ? C_CARD : C_BG);
    if (act) tft.drawFastHLine(x + 8, 291, tw2 - 16, C_ACCENT);
  }
  drawFooter("上下选择  OK 进入");
}

// ================= 页：设备列表（支持分类过滤 + 翻页）=================
// g_devFilter: -1=全部 0=打印中 1=空闲 2=异常
static const char* FILTER_NAMES[3] = { "打印中", "空闲", "异常" };

static bool devInFilter(const Dev& d, int f) {
  if (f < 0) return true;
  if (f == 0) return d.online && (!strcmp(d.state, "RUNNING") || !strcmp(d.state, "PREPARE"));
  if (f == 1) return d.online && (!strcmp(d.state, "IDLE") || !strcmp(d.state, "FINISH"));
  if (f == 2) return !d.online || !strcmp(d.state, "PAUSE") || !strcmp(d.state, "PAUSED") || !strcmp(d.state, "FAILED");
  return true;
}
static void buildDevView() {
  g_devViewN = 0;
  for (int i = 0; i < (int)g_devs.size() && g_devViewN < 64; i++) {
    if (devInFilter(g_devs[i], g_devFilter)) g_devView[g_devViewN++] = i;
  }
}

static void drawDeviceList() {
  tft.fillScreen(C_BG);
  buildDevView();
  char title[24];
  if (g_devFilter >= 0) snprintf(title, sizeof(title), "%s %d台", FILTER_NAMES[g_devFilter], g_devViewN);
  else snprintf(title, sizeof(title), "设备 %d台", g_total);
  drawHeader(title);

  if (g_devs.empty()) {
    drawCNCenter(120, 128, "没有设备数据", C_TEXT, C_BG);
    drawCNCenter(120, 152, "检查桥接是否连上服务器", C_DIM, C_BG);
    drawFooter("长按OK 返回");
    return;
  }
  if (g_devViewN == 0) {
    drawCNCenter(120, 128, "这一类当前没有机台", C_TEXT, C_BG);
    drawCNCenter(120, 152, "按 OK 返回总览", C_DIM, C_BG);
    drawFooter("OK 返回");
    return;
  }
  const int rowH = 38, vis = 6;
  int total = g_devViewN + 1;                 // 第 0 项是「返回」
  if (g_sel > total - 1) g_sel = total - 1;
  int scroll = (g_sel >= vis) ? g_sel - vis + 1 : 0;
  for (int r = 0; r < vis && r + scroll < total; r++) {
    int idx = r + scroll, y = 30 + r * rowH;
    bool sel = (idx == g_sel);
    if (sel) tft.fillRoundRect(4, y, 232, rowH - 2, 4, C_CARD);
    if (idx == 0) {
      drawCN(12, y + 9, "‹ 返回总览", C_ACCENT, sel ? C_CARD : C_BG);
      continue;
    }
    Dev& d = g_devs[g_devView[idx - 1]];
    drawCN(10, y + 2, d.name, d.online ? C_TEXT : C_DIM, sel ? C_CARD : C_BG);
    drawCN(10, y + 20, stateCN(d.state), stateColor(d.state), sel ? C_CARD : C_BG);
    if (!strcmp(d.state, "RUNNING") || !strcmp(d.state, "PREPARE")) {
      drawBar(96, y + 22, 66, 7, d.progress, C_OK);
      char b[16];
      snprintf(b, sizeof(b), "%d%%", d.progress);
      drawCNRight(228, y + 2, b, C_TEXT, sel ? C_CARD : C_BG);
      snprintf(b, sizeof(b), "%d分", d.remain);
      drawCNRight(228, y + 20, b, C_DIM, sel ? C_CARD : C_BG);
    } else if (!strcmp(d.state, "PAUSE") || !strcmp(d.state, "PAUSED")) {
      drawCNRight(228, y + 10, "等待恢复", C_WARN, sel ? C_CARD : C_BG);
    } else if (!d.online) {
      drawCNRight(228, y + 10, "离线", C_DIM, sel ? C_CARD : C_BG);
    }
  }
  if (total > vis) {   // 滚动条：能翻页
    int sh = 228 * vis / total;
    if (sh < 14) sh = 14;
    int sy = 30 + (228 - sh) * scroll / (total - vis);
    tft.fillRoundRect(234, sy, 4, sh, 2, C_DIM);
  }
  drawFooter("上下翻页  OK 详情");
}

// ================= 页：设备操作 =================
// 操作项：0=暂停/恢复  1=清盘  2=返回
static int buildDeviceActs(char labels[3][24], uint16_t colors[3], const Dev& d) {
  int n = 0;
  bool printing = !strcmp(d.state, "RUNNING") || !strcmp(d.state, "PREPARE");
  bool paused = !strcmp(d.state, "PAUSE") || !strcmp(d.state, "PAUSED");
  if (printing || paused) {
    strlcpy(labels[n], printing ? "暂停打印" : "恢复打印", 24);
    colors[n] = printing ? C_WARN : C_OK;
    n++;
  }
  // 清盘：打印结束/空闲时清理打印盘，清理后该机台才能接下一个任务
  if (!strcmp(d.state, "IDLE") || !strcmp(d.state, "FINISH")) {
    strlcpy(labels[n], "清盘（清理打印盘）", 24);
    colors[n] = C_ACCENT;
    n++;
  }
  strlcpy(labels[n], "‹ 返回设备列表", 24);
  colors[n] = C_ACCENT;
  n++;
  return n;
}

static void drawDevAction() {
  tft.fillScreen(C_BG);
  if (g_selDev < 0 || g_selDev >= (int)g_devs.size()) { g_page = P_DEVICES; g_sel = 0; g_needRedraw = true; return; }
  Dev& d = g_devs[g_selDev];
  drawHeader(d.name, d.online ? nullptr : "离线");

  drawCN(10, 36, d.task[0] ? d.task : "（无任务）", C_TEXT, C_BG);
  tft.fillRoundRect(10, 58, 88, 22, 4, C_CARD);
  drawCN(18, 61, stateCN(d.state), stateColor(d.state), C_CARD);

  char buf[40];
  drawBar(10, 88, 220, 10, d.progress, stateColor(d.state));
  snprintf(buf, sizeof(buf), "%d%%", d.progress);
  drawCN(10, 102, buf, C_DIM, C_BG);
  snprintf(buf, sizeof(buf), "剩余%d分", d.remain);
  drawCNRight(230, 102, buf, C_DIM, C_BG);

  tft.fillRoundRect(8, 122, 224, 40, 4, C_CARD);
  snprintf(buf, sizeof(buf), "喷嘴 %d℃", d.nozzle);
  drawCN(14, 132, buf, C_TEXT, C_CARD);
  snprintf(buf, sizeof(buf), "热床 %d℃", d.bed);
  drawCN(126, 132, buf, C_TEXT, C_CARD);

  char labels[3][24];
  uint16_t colors[3];
  int n = buildDeviceActs(labels, colors, d);
  int by = 176;
  for (int i = 0; i < n; i++) {
    bool sel = (g_sel == i);
    if (sel) tft.fillRoundRect(8, by, 224, 34, 6, C_CARD);
    drawCNCenter(120, by + 9, labels[i], colors[i], sel ? C_CARD : C_BG);
    by += 40;
  }
  drawFooter("上下选择  OK 执行");
}

// ================= 页：清盘确认 =================
static void drawCleanConfirm() {
  tft.fillScreen(C_BG);
  drawHeader("清盘确认");
  if (g_selDev < 0 || g_selDev >= (int)g_devs.size()) { g_page = P_DEVICES; g_sel = 0; g_needRedraw = true; return; }

  drawCNCenter(120, 46, "请确认打印件已取走", C_TEXT, C_BG);
  drawCNCenter(120, 70, "并已清理打印盘", C_TEXT, C_BG);
  drawCNCenter(120, 102, "清理后该机台可接新任务", C_DIM, C_BG);

  bool sel = (g_sel == 0);
  if (sel) tft.fillRoundRect(16, 150, 208, 40, 6, C_CARD);
  drawCNCenter(120, 161, "确认清盘", sel ? C_OK : C_DIM, sel ? C_CARD : C_BG);

  bool sel2 = (g_sel == 1);
  if (sel2) tft.fillRoundRect(16, 200, 208, 40, 6, C_CARD);
  drawCNCenter(120, 211, "取消", sel2 ? C_ACCENT : C_DIM, sel2 ? C_CARD : C_BG);

  drawFooter("上下选择  OK 确认");
}

// ================= 页：任务列表 =================
static void drawTaskList() {
  tft.fillScreen(C_BG);
  char t[24];
  snprintf(t, sizeof(t), "任务队列 %d个", g_queue);
  drawHeader(t);

  if (g_tasks.empty()) {
    drawCNCenter(120, 112, "没有排队中的任务", C_TEXT, C_BG);
    drawCNCenter(120, 138, "在电脑端创建任务后会出现在这里", C_DIM, C_BG);
    drawCNCenter(120, 158, "这里可以直接下发到打印机", C_DIM, C_BG);
    drawFooter("长按OK 返回");
    return;
  }
  const int rowH = 36, vis = 7;
  int total = (int)g_tasks.size() + 1;
  int scroll = (g_sel >= vis) ? g_sel - vis + 1 : 0;
  for (int r = 0; r < vis && r + scroll < total; r++) {
    int idx = r + scroll, y = 30 + r * rowH;
    bool sel = (idx == g_sel);
    if (sel) tft.fillRoundRect(4, y, 232, rowH - 2, 4, C_CARD);
    if (idx == 0) {
      drawCN(12, y + 9, "‹ 返回总览", C_ACCENT, sel ? C_CARD : C_BG);
      continue;
    }
    Task& tk = g_tasks[idx - 1];
    drawCN(10, y + 2, tk.name, C_TEXT, sel ? C_CARD : C_BG);
    char b[40];
    snprintf(b, sizeof(b), "剩%d件  已派%d台", tk.remaining, tk.working);
    drawCN(10, y + 20, b, C_DIM, sel ? C_CARD : C_BG);
    if (sel) drawCNRight(228, y + 10, "OK 下发", C_ACCENT, C_CARD);
  }
  drawFooter("上下移动  OK 选择下发");
}

// ================= 页：选机台 =================
static void ensurePicked() {
  if ((int)g_picked.size() != (int)g_devs.size()) g_picked.assign(g_devs.size(), 0);
}
static int pickedCount() {
  int n = 0;
  for (auto v : g_picked) n += v ? 1 : 0;
  return n;
}
/** 可勾选（空闲）且已勾选的设备，按原顺序 */
/** 返回可选设备数量（调用方必须接返回值！） */
static int selectableDevices(int* out, int max) {
  int n = 0;
  for (int i = 0; i < (int)g_devs.size() && n < max; i++) {
    if (isIdle(g_devs[i]) || g_picked[i]) out[n++] = i;
  }
  return n;
}
static void drawSelect() {
  tft.fillScreen(C_BG);
  ensurePicked();
  char t[28];
  snprintf(t, sizeof(t), "选机台 已选%d台", pickedCount());
  drawHeader(t, (g_selTask >= 0 && g_selTask < (int)g_tasks.size()) ? g_tasks[g_selTask].name : "");

  if (g_devs.empty()) {
    drawCNCenter(120, 128, "没有可用机台", C_TEXT, C_BG);
    drawFooter("长按OK 返回");
    return;
  }
  // 索引：0 = 下一步（置顶），1..N = 可选设备，N+1 = 返回（置底）
  static int selIdx[40];
  int nSel = selectableDevices(selIdx, 40);
  int total = nSel + 2;

  const int rowH = 30, vis = 7;
  int scroll = (g_sel >= vis) ? g_sel - vis + 1 : 0;
  if (scroll > total - vis) scroll = total > vis ? total - vis : 0;
  for (int r = 0; r < vis && r + scroll < total; r++) {
    int idx = r + scroll, y = 30 + r * rowH;
    bool sel = (idx == g_sel);
    if (sel) tft.fillRoundRect(4, y, 232, rowH - 2, 4, C_CARD);
    if (idx == 0) {
      char b[40];
      snprintf(b, sizeof(b), "下一步  已选%d台 ›", pickedCount());
      drawCN(12, y + 7, b, pickedCount() ? C_OK : C_DIM, sel ? C_CARD : C_BG);
      continue;
    }
    if (idx == total - 1) {
      drawCN(12, y + 7, "‹ 返回任务", C_ACCENT, sel ? C_CARD : C_BG);
      continue;
    }
    int di = selIdx[idx - 1];
    Dev& d = g_devs[di];
    bool pick = g_picked[di];
    bool ok = isIdle(d);
    uint16_t bg = sel ? C_CARD : C_BG;
    tft.drawRect(10, y + 9, 12, 12, ok ? C_ACCENT : C_DIM);
    if (pick) tft.fillRect(12, y + 11, 8, 8, C_OK);
    drawCN(28, y + 7, d.name, ok ? (pick ? C_OK : C_TEXT) : C_DIM, bg);
    drawCN(104, y + 7, stateCN(d.state), ok ? stateColor(d.state) : C_DIM, bg);
    if (!strcmp(d.state, "RUNNING") || !strcmp(d.state, "PREPARE")) {
      char b[16];
      snprintf(b, sizeof(b), "%d%%", d.progress);
      drawCNRight(228, y + 7, b, C_DIM, bg);
    } else {
      drawCNRight(228, y + 7, "可勾选", C_DIM, bg);
    }
  }
  if (total > vis) {
    int sh = 210 * vis / total;
    int sy = 30 + (210 - sh) * scroll / (total - vis);
    tft.fillRoundRect(234, sy, 4, sh, 2, C_DIM);
  }
  drawFooter("OK 勾选  长按返回");
}

// ================= 页：下发选项 =================
static const char* AMS_NAMES[] = { "外挂料盘", "AMS 第1槽", "AMS 第2槽", "AMS 第3槽", "AMS 第4槽" };
static void drawOptions() {
  tft.fillScreen(C_BG);
  drawHeader("确认下发");

  if (g_selTask < 0 || g_selTask >= (int)g_tasks.size()) { g_page = P_TASKS; g_sel = 0; g_needRedraw = true; return; }
  Task& tk = g_tasks[g_selTask];

  drawCN(10, 34, tk.name, C_TEXT, C_BG);
  char b[48];
  snprintf(b, sizeof(b), "剩%d件  本次派%d台", tk.remaining, pickedCount());
  drawCN(10, 54, b, C_DIM, C_BG);

  const int rowH = 38;
  int y = 70;
  // ① 料盘映射
  bool sel = (g_sel == 0);
  if (sel) tft.fillRoundRect(6, y, 228, rowH - 4, 5, C_CARD);
  drawCN(14, y + 2, "料盘映射", C_DIM, sel ? C_CARD : C_BG);
  drawCN(14, y + 18, AMS_NAMES[g_amsIdx], C_ACCENT, sel ? C_CARD : C_BG);
  drawCNRight(226, y + 10, sel ? "OK 切换" : "›", sel ? C_ACCENT : C_DIM, sel ? C_CARD : C_BG);
  y += rowH;

  // ② 已选机台
  sel = (g_sel == 1);
  if (sel) tft.fillRoundRect(6, y, 228, rowH - 4, 5, C_CARD);
  drawCN(14, y + 2, "已选机台", C_DIM, sel ? C_CARD : C_BG);
  char names[44] = "";
  int n = 0;
  for (int i = 0; i < (int)g_devs.size() && n < 3; i++) {
    if (!g_picked[i]) continue;
    if (n) strlcat(names, " ", sizeof(names));
    strlcat(names, g_devs[i].name, sizeof(names));
    n++;
  }
  if (pickedCount() > 3) strlcat(names, " 等", sizeof(names));
  drawCN(14, y + 18, pickedCount() ? names : "（未选机台）", pickedCount() ? C_TEXT : C_WARN, sel ? C_CARD : C_BG);
  y += rowH;

  // ③ 打印设置（单独一页）
  sel = (g_sel == 2);
  if (sel) tft.fillRoundRect(6, y, 228, rowH - 4, 5, C_CARD);
  drawCN(14, y + 2, "打印设置", C_DIM, sel ? C_CARD : C_BG);
  char po[48];
  snprintf(po, sizeof(po), "调平%s 校准%s 延时%s",
           g_poLevel ? "开" : "关", g_poFlow ? "开" : "关", g_poTime ? "开" : "关");
  drawCN(14, y + 18, po, C_TEXT, sel ? C_CARD : C_BG);
  drawCNRight(226, y + 10, "›", sel ? C_ACCENT : C_DIM, sel ? C_CARD : C_BG);
  y += rowH;

  // ④ 确认下发
  sel = (g_sel == 3);
  bool can = pickedCount() > 0 && g_launching == 0;
  if (sel) tft.fillRoundRect(6, y, 228, 34, 5, can ? C_CARD : C_CARD2);
  drawCNCenter(120, y + 9, g_launching ? "正在下发…" : "确认下发",
               can ? (sel ? C_OK : C_TEXT) : C_DIM, sel ? C_CARD : C_CARD2);

  drawCN(10, 234, "清盘由电脑端建任务时确定", C_DIM, C_BG);
  drawFooter("上下选择  OK 执行");
}

// ================= 页：打印设置 =================
// 0=自动调平 1=流量校准 2=延时摄影 3=返回
static void drawPrintOpt() {
  tft.fillScreen(C_BG);
  drawHeader("打印设置");

  const char* items[3] = { "自动调平", "流量校准", "延时摄影" };
  bool* vals[3] = { &g_poLevel, &g_poFlow, &g_poTime };
  const int rowH = 40, y0 = 42;
  for (int i = 0; i < 3; i++) {
    int y = y0 + i * rowH;
    bool sel = (g_sel == i);
    if (sel) tft.fillRoundRect(8, y, 224, rowH - 6, 5, C_CARD);
    uint16_t bg = sel ? C_CARD : C_BG;
    drawCN(18, y + 8, items[i], sel ? C_TEXT : C_DIM, bg);
    bool on = *vals[i];
    // 开关：用 ✓/✗ 加文字，配色区分
    uint16_t col = on ? C_OK : C_DIM;
    tft.drawRoundRect(150, y + 5, 62, 24, 4, on ? C_OK : C_DIM);
    drawCNCenter(181, y + 9, on ? "开" : "关", col, bg);
    drawCN(18, y + 8, items[i], sel ? C_TEXT : C_DIM, bg);
  }
  bool sel = (g_sel == 3);
  if (sel) tft.fillRoundRect(8, y0 + 3 * rowH, 224, 34, 5, C_CARD);
  drawCNCenter(120, y0 + 3 * rowH + 9, "‹ 返回确认页", C_ACCENT, sel ? C_CARD : C_BG);

  drawCN(10, 232, "默认：调平开 校准开 延时关", C_DIM, C_BG);
  drawCN(10, 250, "OK 切换开关", C_DIM, C_BG);
  drawFooter("上下选择  OK 切换");
}

// ================= 页：下发结果 =================
static void drawResult() {
  tft.fillScreen(C_BG);
  int ok = 0;
  for (auto& r : g_results) if (r.code == 0) ok++;
  char t[24];
  snprintf(t, sizeof(t), "下发结果 %d/%d 成功", ok, (int)g_results.size());
  drawHeader(t);

  if (g_results.empty()) {
    drawCNCenter(120, 128, "暂无结果", C_DIM, C_BG);
    drawFooter("长按OK 返回");
    return;
  }
  int y = 34;
  for (auto& r : g_results) {
    if (y > 246) break;
    bool good = (r.code == 0);
    tft.fillRoundRect(6, y, 228, 34, 4, C_CARD);
    drawCN(12, y + 3, r.name, C_TEXT, C_CARD);
    drawCNRight(226, y + 3, good ? "成功" : "失败", good ? C_OK : C_ERR, C_CARD);
    if (!good && r.msg[0]) drawCN(12, y + 19, r.msg, C_DIM, C_CARD);
    y += 38;
  }
  drawFooter("长按OK 返回总览");
}

// ================= 页：事件 =================
static void drawEvents() {
  tft.fillScreen(C_BG);
  drawHeader("事件记录");
  if (g_events.empty()) {
    drawCNCenter(120, 112, "还没有事件", C_TEXT, C_BG);
    drawCNCenter(120, 138, "打印机暂停/完成/失败时", C_DIM, C_BG);
    drawCNCenter(120, 158, "会在这里提醒并说明原因", C_DIM, C_BG);
    drawFooter("长按OK 返回");
    return;
  }
  const int rowH = 36, vis = 7;
  int total = (int)g_events.size() + 1;
  int scroll = (g_sel >= vis) ? g_sel - vis + 1 : 0;
  for (int r = 0; r < vis && r + scroll < total; r++) {
    int idx = r + scroll, y = 30 + r * rowH;
    bool sel = (idx == g_sel);
    if (sel) tft.fillRoundRect(4, y, 232, rowH - 2, 4, C_CARD);
    if (idx == 0) {
      drawCN(12, y + 9, "‹ 返回总览", C_ACCENT, sel ? C_CARD : C_BG);
      continue;
    }
    Ev& e = g_events[idx - 1];
    uint16_t c = !strcmp(e.kind, "pause") ? C_WARN : (!strcmp(e.kind, "fail") || !strcmp(e.kind, "hms") ? C_ERR : (!strcmp(e.kind, "finish") ? C_OK : C_ACCENT));
    char b[60];
    if (!strcmp(e.kind, "pause")) snprintf(b, sizeof(b), "%s 暂停", e.dev);
    else if (!strcmp(e.kind, "fail")) snprintf(b, sizeof(b), "%s 失败", e.dev);
    else if (!strcmp(e.kind, "finish")) snprintf(b, sizeof(b), "%s 完成", e.dev);
    else if (!strcmp(e.kind, "resume")) snprintf(b, sizeof(b), "%s 恢复", e.dev);
    else if (!strcmp(e.kind, "hms")) snprintf(b, sizeof(b), "%s 异常", e.dev);
    else if (!strcmp(e.kind, "new")) snprintf(b, sizeof(b), "%s 上线", e.dev);
    else if (!strcmp(e.kind, "launch")) snprintf(b, sizeof(b), "%s 任务下发", e.dev);
    else if (e.from[0] && e.to[0]) snprintf(b, sizeof(b), "%s %s→%s", e.dev, stateCN(e.from), stateCN(e.to));
    else snprintf(b, sizeof(b), "%s 通知", e.dev);   // 兜底：未知类型也用中文，不露原始英文 kind
    trimUtf8Tail(b);
    drawCN(10, y + 2, b, c, sel ? C_CARD : C_BG);
    if (e.reason[0]) {
      // 一行放不下的原因就截断（同样要丢掉半个汉字）
      char r[56];
      strlcpy(r, e.reason, sizeof(r));
      trimUtf8Tail(r);
      drawCN(10, y + 19, r, C_DIM, sel ? C_CARD : C_BG);
    }
  }
  drawFooter("上下移动  长按返回");
}

// ================= 页：设置 =================
// 0=自动息屏 1=铃声与告警 2=立即息屏 3=返回
static void drawSettings() {
  tft.fillScreen(C_BG);
  drawHeader("设置");
  drawBatteryBadge();

  const int rows[4] = { 46, 90, 134, 178 };
  const int ty = 12;
  // 0 自动息屏
  {
    bool sel = (g_sel == 0);
    if (sel) tft.fillRoundRect(8, rows[0], 224, 40, 6, C_CARD);
    drawCN(18, rows[0] + ty, "自动息屏", C_TEXT, sel ? C_CARD : C_BG);
    char v[24];
    snprintf(v, sizeof(v), "%d 分钟 ›", g_sleepMin);
    drawCNRight(216, rows[0] + ty, v, C_ACCENT, sel ? C_CARD : C_BG);
  }
  // 1 铃声与告警（进去可以开关 + 调音量 + 单独试听）
  {
    bool sel = (g_sel == 1);
    if (sel) tft.fillRoundRect(8, rows[1], 224, 40, 6, C_CARD);
    drawCN(18, rows[1] + ty, "铃声与告警", C_TEXT, sel ? C_CARD : C_BG);
    uint16_t onCol = g_alertOn ? C_OK : C_DIM;
    tft.drawRect(150, rows[1] + 8, 66, 24, onCol);
    tft.fillRect(151, rows[1] + 9, 64, 22, sel ? C_CARD : C_BG);
    drawCNCenter(183, rows[1] + 11, g_alertOn ? "已开启" : "已关闭", onCol, sel ? C_CARD : C_BG);
  }
  // 2 立即息屏
  {
    bool sel = (g_sel == 2);
    if (sel) tft.fillRoundRect(8, rows[2], 224, 40, 6, C_CARD);
    drawCNCenter(120, rows[2] + ty, "立即息屏", sel ? C_OK : C_TEXT, sel ? C_CARD : C_BG);
  }
  // 3 返回
  {
    bool sel = (g_sel == 3);
    if (sel) tft.fillRoundRect(8, rows[3], 224, 40, 6, C_CARD);
    drawCNCenter(120, rows[3] + ty, "‹ 返回总览", C_ACCENT, sel ? C_CARD : C_BG);
  }

  drawCN(10, 226, "告警：完成响两声 异常响三声", C_DIM, C_BG);
  drawCN(10, 244, "息屏后 WiFi 仍在线 按键唤醒", C_DIM, C_BG);
  drawFooter("上下选择  OK 确认");
}

// ================= 页：铃声（开关 + 音量 + 单独试听）=================
// 0=开关 1=音量 2=试听成功音效 3=试听故障音效 4=返回
// 试听不受"告警开关"限制：关了也能听，方便确认喇叭和音量是否正常
static void drawRing() {
  tft.fillScreen(C_BG);
  drawHeader("铃声");
  drawBatteryBadge();

  // 每行只放一行字，行距 44，别挤
  const int rows[5] = { 46, 90, 134, 178, 222 };
  const int rowH = 40, ty = 12;

  // 0 告警开关
  {
    bool sel = (g_sel == 0);
    if (sel) tft.fillRoundRect(8, rows[0], 224, rowH, 6, C_CARD);
    drawCN(18, rows[0] + ty, "告警开关", C_TEXT, sel ? C_CARD : C_BG);
    uint16_t onCol = g_alertOn ? C_OK : C_DIM;
    tft.drawRect(150, rows[0] + 8, 66, 24, onCol);
    tft.fillRect(151, rows[0] + 9, 64, 22, sel ? C_CARD : C_BG);
    drawCNCenter(183, rows[0] + 11, g_alertOn ? "已开启" : "已关闭", onCol, sel ? C_CARD : C_BG);
  }
  // 1 音量
  {
    bool sel = (g_sel == 1);
    if (sel) tft.fillRoundRect(8, rows[1], 224, rowH, 6, C_CARD);
    drawCN(18, rows[1] + ty, "音量", C_TEXT, sel ? C_CARD : C_BG);
    char v[12];
    if (g_alertVol >= 120) snprintf(v, sizeof(v), "超大");
    else                   snprintf(v, sizeof(v), "%d%%", g_alertVol);
    drawCNRight(216, rows[1] + ty, v,
                (g_alertVol >= 120) ? C_WARN : (sel ? C_ACCENT : C_DIM),
                sel ? C_CARD : C_BG);
    // 5 格小音量条，一眼看得出大小；最后一格（超大）用警示色
    for (int i = 0; i < 5; i++) {
      int bx = 92 + i * 12;
      bool on = (g_alertVol >= VOL_STEPS[i]);
      uint16_t c = C_CARD2;
      if (on) c = (i == 4) ? C_WARN : C_ACCENT;
      tft.fillRect(bx, rows[1] + 17, 9, 8, c);
    }
  }
  // 2 成功音效（进去挑一种）
  {
    bool sel = (g_sel == 2);
    if (sel) tft.fillRoundRect(8, rows[2], 224, rowH, 6, C_CARD);
    drawCN(18, rows[2] + ty, "成功音效", sel ? C_OK : C_TEXT, sel ? C_CARD : C_BG);
    char v[24]; snprintf(v, sizeof(v), "%s ›", FINISH_NAMES[g_toneFinish]);
    drawCNRight(216, rows[2] + ty, v, sel ? C_OK : C_DIM, sel ? C_CARD : C_BG);
  }
  // 3 故障音效（进去挑一种）
  {
    bool sel = (g_sel == 3);
    if (sel) tft.fillRoundRect(8, rows[3], 224, rowH, 6, C_CARD);
    drawCN(18, rows[3] + ty, "故障音效", sel ? C_WARN : C_TEXT, sel ? C_CARD : C_BG);
    char v[24]; snprintf(v, sizeof(v), "%s ›", FAULT_NAMES[g_toneFault]);
    drawCNRight(216, rows[3] + ty, v, sel ? C_WARN : C_DIM, sel ? C_CARD : C_BG);
  }
  // 4 返回
  {
    bool sel = (g_sel == 4);
    if (sel) tft.fillRoundRect(8, rows[4], 224, rowH, 6, C_CARD);
    drawCNCenter(120, rows[4] + ty, "‹ 返回设置", C_ACCENT, sel ? C_CARD : C_BG);
  }

  drawCN(10, 268, "试听不受开关限制 关了也能试", C_DIM, C_BG);
  drawFooter("上下选择  OK 播放/切换");
}

// ================= 页：挑音效 =================
// g_toneSel: 0=完成音效 1=故障音效
// 0..3 = 四种音效，4 = 返回；点哪个立刻播哪个并选中（方便一个个试听对比）
static void drawToneSel() {
  tft.fillScreen(C_BG);
  drawHeader(g_toneSel == 0 ? "完成音效" : "故障音效");
  drawBatteryBadge();

  const int rows[5] = { 46, 90, 134, 178, 222 };
  const int ty = 12;
  const char** names = (g_toneSel == 0) ? FINISH_NAMES : FAULT_NAMES;
  int cur = (g_toneSel == 0) ? g_toneFinish : g_toneFault;

  for (int i = 0; i < 4; i++) {
    bool sel = (g_sel == i);
    if (sel) tft.fillRoundRect(8, rows[i], 224, 40, 6, C_CARD);
    char lab[24];
    snprintf(lab, sizeof(lab), "%d %s", i + 1, names[i]);
    drawCN(18, rows[i] + ty, lab, sel ? C_TEXT : C_DIM, sel ? C_CARD : C_BG);
    if (i == cur) drawCNRight(216, rows[i] + ty, "当前 · 试听", C_OK, sel ? C_CARD : C_BG);
    else          drawCNRight(216, rows[i] + ty, "试听 ›", C_DIM, sel ? C_CARD : C_BG);
  }
  {
    bool sel = (g_sel == 4);
    if (sel) tft.fillRoundRect(8, rows[4], 224, 40, 6, C_CARD);
    drawCNCenter(120, rows[4] + ty, "‹ 返回铃声", C_ACCENT, sel ? C_CARD : C_BG);
  }

  drawCN(10, 268, "点一下就播 方便挨个对比", C_DIM, C_BG);
  drawFooter("上下选择  OK 试听并选中");
}

// ================= 页：自动息屏时间 =================
// 0..3=预设 10/20/30/60  4=自定义  5=返回
static void drawSleepOpt() {
  tft.fillScreen(C_BG);
  drawHeader("自动息屏");

  const int rowH = 34, y0 = 34;
  for (int i = 0; i < 6; i++) {
    int y = y0 + i * rowH;
    bool sel = (g_sel == i);
    if (sel) tft.fillRoundRect(6, y, 228, rowH - 4, 5, C_CARD);
    uint16_t bg = sel ? C_CARD : C_BG;
    if (i < 4) {
      char lb[20];
      snprintf(lb, sizeof(lb), "%d 分钟", SLEEP_PRESETS[i]);
      drawCN(16, y + 7, lb, C_TEXT, bg);
      if (g_sleepMin == SLEEP_PRESETS[i]) drawCNRight(220, y + 7, "当前", C_OK, bg);
      else drawCNRight(220, y + 7, "›", C_DIM, bg);
    } else if (i == 4) {
      bool editing = (sel && g_editSleep);
      char lb[24];
      snprintf(lb, sizeof(lb), "自定义 %d 分钟", g_customMin);
      uint16_t col = editing ? C_WARN : (g_sleepMin == g_customMin ? C_OK : C_TEXT);
      if (editing) tft.drawRoundRect(6, y, 228, rowH - 4, 5, C_WARN);
      drawCN(16, y + 7, lb, col, bg);
      drawCNRight(220, y + 7, editing ? "OK保存" : "上下调", editing ? C_WARN : C_DIM, bg);
    } else {
      drawCN(16, y + 7, "‹ 返回设置", C_ACCENT, bg);
    }
  }
  drawFooter(g_editSleep ? "上下调数值 OK保存" : "上下选择  OK 确认");
}

// ================= 调度 =================
static void drawPage() {
  switch (g_page) {
    case P_HOME:       drawHome(); break;
    case P_DEVICES:    drawDeviceList(); break;
    case P_DEV_ACTION: drawDevAction(); break;
    case P_TASKS:      drawTaskList(); break;
    case P_SELECT:     drawSelect(); break;
    case P_OPTIONS:    drawOptions(); break;
    case P_PRINTOPT:   drawPrintOpt(); break;
    case P_CLEAN:      drawCleanConfirm(); break;
    case P_RESULT:     drawResult(); break;
    case P_EVENTS:     drawEvents(); break;
    case P_SETTINGS:   drawSettings(); break;
    case P_SLEEP:      drawSleepOpt(); break;
    case P_RING:       drawRing(); break;
    case P_TONESEL:    drawToneSel(); break;
  }
  g_needRedraw = false;
}

// ================= 按键 =================
#define B_NONE 0
#define B_UP   1
#define B_DOWN 2
#define B_OK   3
#define LONG_OK_MS   1000   // 长按 OK 1 秒 = 一路退回总览
#define IDLE_SLEEP_MS 60000 // 兜底值，实际用 idleSleepMs()（设置页可改）

static int readButton() {
  int mv = analogReadMilliVolts(PIN_BTN_ADC);
  if (mv < BTN_UP_MAX)   return B_UP;
  if (mv < BTN_DOWN_MAX) return B_DOWN;
  if (mv < BTN_OK_MAX)   return B_OK;
  return B_NONE;
}

/** 短按：按住 50ms 立即触发（响应及时，回到最初的键位手感） */
static int pollButton() {
  static int last = B_NONE;
  static unsigned long lastMs = 0;
  static bool fired = false;
  int b = readButton();
  if (b == B_NONE) { last = B_NONE; fired = false; return B_NONE; }
  if (b != last) { last = b; lastMs = millis(); fired = false; return B_NONE; }
  if (!fired && millis() - lastMs > 50) { fired = true; return b; }
  return B_NONE;
}

/** 长按检测（按住期间触发一次） */
static bool longPressed(int btn, unsigned long threshold) {
  static int last = B_NONE;
  static unsigned long startMs = 0;
  static bool fired = false;
  int b = readButton();
  if (b != btn) {
    last = b;
    startMs = (b == btn) ? millis() : 0;
    fired = false;
    return false;
  }
  if (last != btn) {
    last = b;
    startMs = millis();
    fired = false;
  }
  if (!fired && millis() - startMs >= threshold) {
    fired = true;
    return true;
  }
  return false;
}

static int listLen() {
  switch (g_page) {
    case P_DEVICES: { buildDevView(); return g_devViewN; }
    case P_TASKS:   return (int)g_tasks.size();
    case P_EVENTS:  return (int)g_events.size();
    case P_SELECT: {
      static int selIdx[40];
      int nSel = selectableDevices(selIdx, 40);
      return nSel + 1;          // 0=下一步, 1..N=设备, N+1=返回
    }
    case P_DEV_ACTION: {
      if (g_selDev < 0 || g_selDev >= (int)g_devs.size()) return 2;
      char labels[3][24]; uint16_t colors[3];
      return buildDeviceActs(labels, colors, g_devs[g_selDev]) - 1;   // 最后一项是返回
    }
    case P_CLEAN: return 1;
    case P_OPTIONS: return 3;   // 0=料盘 1=机台 2=打印设置 3=确认下发
    case P_PRINTOPT: return 3;  // 0=调平 1=校准 2=延时 3=返回
    case P_SETTINGS: return 3;  // 0=自动息屏 1=铃声与告警 2=立即息屏 3=返回
    case P_RING: return 4;      // 0=开关 1=音量 2=成功音效 3=故障音效 4=返回
    case P_TONESEL: return 4;   // 0..3=四种音效 4=返回
    case P_SLEEP: return 5;     // 0..3=预设 4=自定义 5=返回
    default: return 0;
  }
}

/** 把息屏时间存进 NVS（掉电不丢） */
static void saveSleepPref() {
  Preferences p;
  p.begin("farm", false);
  p.putInt("sleep_min", g_sleepMin);
  p.putInt("custom_min", g_customMin);
  p.end();
}
static void loadSleepPref() {
  Preferences p;
  p.begin("farm", true);
  g_sleepMin = p.getInt("sleep_min", 10);
  g_customMin = p.getInt("custom_min", 15);
  p.end();
  if (g_sleepMin < 1 || g_sleepMin > 120) g_sleepMin = 10;
  if (g_customMin < 1 || g_customMin > 120) g_customMin = 15;
}

/** 声音告警开关 + 音量存进 NVS */
static void saveAlertPref() {
  Preferences p;
  p.begin("farm", false);
  p.putBool("alert_on", g_alertOn);
  p.putInt("alert_vol", g_alertVol);
  p.putInt("tone_fin", g_toneFinish);
  p.putInt("tone_err", g_toneFault);
  p.end();
}
static void loadAlertPref() {
  Preferences p;
  p.begin("farm", true);
  g_alertOn = p.getBool("alert_on", true);
  g_alertVol = p.getInt("alert_vol", 80);
  g_toneFinish = p.getInt("tone_fin", 0);
  g_toneFault  = p.getInt("tone_err", 0);
  p.end();
  if (g_alertVol < 10) g_alertVol = 10;
  if (g_alertVol > 120) g_alertVol = 120;
  if (g_toneFinish < 0 || g_toneFinish > 3) g_toneFinish = 0;
  if (g_toneFault  < 0 || g_toneFault  > 3) g_toneFault  = 0;
}

// ---- WiFi 凭据：桥接运行时下发，存 NVS，优先级高于编译期的 wifi_config.h ----
static void saveWifiPref(const char* ssid, const char* pass) {
  Preferences p;
  p.begin("farm", false);
  p.putString("wifi_ssid", ssid);
  p.putString("wifi_pass", pass);
  p.end();
}
/** 读出下发的凭据；没下发过返回 false（此时用编译期的） */
static bool loadWifiPref(char* ssid, size_t ns, char* pass, size_t np) {
  Preferences p;
  p.begin("farm", true);
  String s = p.getString("wifi_ssid", "");
  String k = p.getString("wifi_pass", "");
  p.end();
  if (s.length() == 0) return false;   // 没配置过
  strlcpy(ssid, s.c_str(), ns);
  strlcpy(pass, k.c_str(), np);
  return true;
}

/** 音量档位：OK 循环切换，每档都会响一声（方便直接听出差别）
 *  120 = 「超大」：DAC 仍锁 0dB（不削顶），靠数字端 tanh 软饱和再压响度
 *  （档位表 VOL_STEPS 定义在文件前面，画界面时也要用）
 */
static void volStepNext() {
  int n = (int)(sizeof(VOL_STEPS) / sizeof(VOL_STEPS[0]));
  int idx = n - 1;
  for (int i = 0; i < n; i++) if (g_alertVol <= VOL_STEPS[i]) { idx = i; break; }
  g_alertVol = VOL_STEPS[(idx + 1) % n];
  saveAlertPref();
}

static void actUp() {
  if (g_page == P_HOME) { g_focus = (g_focus + HOME_FOCUS_MAX) % (HOME_FOCUS_MAX + 1); g_needRedraw = true; return; }
  // 自定义息屏处于调节中：上下改数值，不移动光标
  if (g_page == P_SLEEP && g_editSleep) {
    int step = (g_customMin > 20) ? 5 : 1;
    g_customMin -= step;
    if (g_customMin < 1) g_customMin = 1;
    g_needRedraw = true;
    return;
  }
  if (g_sel > 0) { g_sel--; g_needRedraw = true; }
}
static void actDown() {
  if (g_page == P_HOME) { g_focus = (g_focus + 1) % (HOME_FOCUS_MAX + 1); g_needRedraw = true; return; }
  if (g_page == P_SLEEP && g_editSleep) {
    int step = (g_customMin >= 20) ? 5 : 1;
    g_customMin += step;
    if (g_customMin > 120) g_customMin = 120;
    g_needRedraw = true;
    return;
  }
  if (g_sel < listLen()) { g_sel++; g_needRedraw = true; }
}

static void doLaunch() {
  if (g_selTask < 0 || g_selTask >= (int)g_tasks.size()) return;
  int ams = (g_amsIdx == 0) ? 255 : 0;
  int slot = (g_amsIdx == 0) ? 0 : (g_amsIdx - 1);
  g_launching = 1;
  g_needRedraw = true;
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "{\"t\":\"launch\",\"task_id\":\"%s\",\"dev_ids\":[", g_tasks[g_selTask].name);
  bool first = true;
  for (int i = 0; i < (int)g_devs.size(); i++) {
    if (!g_picked[i]) continue;
    n += snprintf(buf + n, sizeof(buf) - n, "%s\"%s\"", first ? "" : ",", g_devs[i].name);
    first = false;
    if (n > (int)sizeof(buf) - 40) break;
  }
  snprintf(buf + n, sizeof(buf) - n, "],\"ams_id\":%d,\"slot_id\":%d,\"po\":[%d,%d,%d]}",
           ams, slot, g_poLevel ? 1 : 0, g_poFlow ? 1 : 0, g_poTime ? 1 : 0);
  sendJson(buf);
}

static void actOk() {
  switch (g_page) {
    case P_HOME:
      if (!g_hasState) { sendGetState(); return; }
      // 0=打印中 1=空闲 2=异常 → 进对应分类的设备列表（可翻页）
      // 3=队列 → 任务列表   4=事件 5=设置 6=全部设备
      if (g_focus <= 2) { g_devFilter = g_focus; g_page = P_DEVICES; g_sel = 0; }
      else if (g_focus == 3) { g_page = P_TASKS; g_sel = 0; }
      else if (g_focus == 4) { g_page = P_EVENTS; g_sel = 0; }
      else if (g_focus == 5) { g_page = P_SETTINGS; g_sel = 0; }
      else { g_devFilter = -1; g_page = P_DEVICES; g_sel = 0; }
      g_needRedraw = true;
      break;

    case P_SETTINGS:
      if (g_sel == 0) { g_page = P_SLEEP; g_sel = 0; g_editSleep = false; }
      else if (g_sel == 1) { g_page = P_RING; g_sel = 0; }   // 进铃声页
      else if (g_sel == 2) { screenOff(); }
      else { g_page = P_HOME; g_sel = 0; g_focus = 5; }
      g_needRedraw = true;
      break;

    case P_RING:
      if (g_sel == 0) {                            // 开关
        g_alertOn = !g_alertOn;
        saveAlertPref();
        if (g_alertOn) playAlert(2, true);         // 开的时候响一声，顺便试听
        showToast(g_alertOn ? "告警声音已开启" : "告警声音已关闭");
      }
      else if (g_sel == 1) {                       // 音量：循环切档并立刻响一声
        volStepNext();
        char tb[24];
        if (g_alertVol >= 120) snprintf(tb, sizeof(tb), "音量 超大");
        else                   snprintf(tb, sizeof(tb), "音量 %d%%", g_alertVol);
        showToast(tb);
        g_needRedraw = true; drawPage();
        playAlert(2, true);
      }
      else if (g_sel == 2) { g_toneSel = 0; g_page = P_TONESEL; g_sel = g_toneFinish; }  // 挑完成音效
      else if (g_sel == 3) { g_toneSel = 1; g_page = P_TONESEL; g_sel = g_toneFault;  }  // 挑故障音效
      else { g_page = P_SETTINGS; g_sel = 1; }
      g_needRedraw = true;
      break;

    case P_TONESEL:
      if (g_sel < 4) {                             // 选中并立刻试听
        if (g_toneSel == 0) g_toneFinish = g_sel; else g_toneFault = g_sel;
        saveAlertPref();
        g_needRedraw = true; drawPage();           // 先刷新（把"当前"标出来）再播
        playAlert(g_toneSel == 0 ? 0 : 1, true);
      } else {                                     // 返回铃声页
        g_page = P_RING; g_sel = (g_toneSel == 0) ? 2 : 3;
      }
      g_needRedraw = true;
      break;

    case P_SLEEP:
      if (g_editSleep) {                       // 自定义调节中 → OK 保存
        g_editSleep = false;
        g_sleepMin = g_customMin;
        saveSleepPref();
        showToast("已保存");
        g_page = P_SETTINGS; g_sel = 0;
      } else if (g_sel < 4) {                  // 预设档
        g_sleepMin = SLEEP_PRESETS[g_sel];
        saveSleepPref();
        char tb[32];
        snprintf(tb, sizeof(tb), "%d分钟后自动息屏", g_sleepMin);
        showToast(tb);
        g_page = P_SETTINGS; g_sel = 0;
      } else if (g_sel == 4) {                 // 进自定义调节
        g_editSleep = true;
      } else {                                 // 返回
        g_page = P_SETTINGS; g_sel = 0;
      }
      g_needRedraw = true;
      break;

    case P_DEVICES:
      buildDevView();
      if (g_sel == 0 || g_devViewN == 0) { g_page = P_HOME; g_sel = 0; g_focus = 0; }
      else { g_selDev = g_devView[g_sel - 1]; g_page = P_DEV_ACTION; g_sel = 0; }
      g_needRedraw = true;
      break;

    case P_DEV_ACTION:
      if (g_selDev >= 0 && g_selDev < (int)g_devs.size()) {
        Dev& d = g_devs[g_selDev];
        bool printing = !strcmp(d.state, "RUNNING") || !strcmp(d.state, "PREPARE");
        bool paused = !strcmp(d.state, "PAUSE") || !strcmp(d.state, "PAUSED");
        char labels[3][24]; uint16_t colors[3];
        int n = buildDeviceActs(labels, colors, d);
        if (g_sel == 0 && n > 0 && (printing || paused)) {
          char buf[80];
          snprintf(buf, sizeof(buf), "{\"t\":\"opt\",\"dev_id\":\"%s\",\"opt\":\"%s\"}", d.name, printing ? "pause" : "resume");
          sendJson(buf);
          showToast(printing ? "已发送暂停指令" : "已发送恢复指令");
        } else if (g_sel == 1 && n > 1 && !strcmp(labels[1], "清盘（清理打印盘）")) {
          g_page = P_CLEAN;      // 去清盘确认页
          g_sel = 0;
        } else if (g_sel == n - 1) {
          g_page = P_DEVICES;
        }
      }
      g_needRedraw = true;
      break;

    case P_CLEAN:
      if (g_selDev >= 0 && g_selDev < (int)g_devs.size()) {
        if (g_sel == 0) {
          char buf[80];
          snprintf(buf, sizeof(buf), "{\"t\":\"opt\",\"dev_id\":\"%s\",\"opt\":\"bed_clean\"}", g_devs[g_selDev].name);
          sendJson(buf);
          showToast("已提交清盘，请清理打印盘");
        }
        g_page = P_DEV_ACTION;
        g_sel = 0;
      }
      g_needRedraw = true;
      break;

    case P_TASKS:
      if (g_sel == 0) g_page = P_HOME;
      else {
        g_selTask = g_sel - 1;
        g_page = P_SELECT;
        g_sel = 0;
        ensurePicked();
        // 自动勾选第一台空闲机（大多数情况用户就想派给一台）
        for (int i = 0; i < (int)g_devs.size(); i++) {
          if (isIdle(g_devs[i])) { g_picked[i] = 1; break; }
        }
      }
      g_needRedraw = true;
      break;

    case P_SELECT: {
      ensurePicked();
      static int selIdx[40];
      int nSel = selectableDevices(selIdx, 40);
      int total = nSel + 2;
      if (g_sel == 0) {
        if (pickedCount() == 0) showToast("先勾选至少一台机台");
        else { g_page = P_OPTIONS; g_sel = 0; }
      } else if (g_sel == total - 1) {
        g_page = P_TASKS; g_sel = 0;
      } else {
        int di = selIdx[g_sel - 1];
        if (isIdle(g_devs[di])) g_picked[di] = !g_picked[di];
        else showToast("该机台正在使用中");
      }
      g_needRedraw = true;
      break;
    }

    case P_OPTIONS:
      if (g_sel == 0) g_amsIdx = (g_amsIdx + 1) % 5;
      else if (g_sel == 1) { g_page = P_SELECT; g_sel = 0; }
      else if (g_sel == 2) { g_page = P_PRINTOPT; g_sel = 0; }
      else if (g_sel == 3 && pickedCount() > 0 && !g_launching) doLaunch();
      g_needRedraw = true;
      break;

    case P_PRINTOPT:
      if (g_sel == 0) g_poLevel = !g_poLevel;
      else if (g_sel == 1) g_poFlow = !g_poFlow;
      else if (g_sel == 2) g_poTime = !g_poTime;
      else { g_page = P_OPTIONS; g_sel = 2; }
      g_needRedraw = true;
      break;

    case P_RESULT:
    case P_EVENTS:
      if (g_sel == 0) { g_page = P_HOME; g_sel = 0; g_needRedraw = true; }
      break;
  }
}

// ================= 无线升级（走业务链路）=================
// base64 解码：桥接把固件切片成 1024 字节，用 b64 塞进 JSON 发过来
static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
static int b64decode(const char* in, uint8_t* out, int outMax) {
  int len = 0;
  unsigned acc = 0;
  int bits = 0;
  for (const char* p = in; *p; p++) {
    if (*p == '=') break;
    int v = b64val(*p);
    if (v < 0) continue;
    acc = (acc << 6) | (unsigned)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (len < outMax) out[len++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return len;
}

/** 升级进度界面 */
static void drawOtaScreen(int pct) {
  tft.fillScreen(TFT_BLACK);
  drawCNCenter(120, 104, "正在无线升级固件", C_WARN, TFT_BLACK);
  drawCNCenter(120, 128, "请勿断电或断开", C_DIM, TFT_BLACK);
  int x = 24, y = 166, w = 192, h = 12;
  tft.drawRect(x - 1, y - 1, w + 2, h + 2, C_DIM);
  if (pct > 0) tft.fillRect(x, y, w * pct / 100, h, C_OK);
  tft.setTextColor(C_TEXT, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  char b[16];
  snprintf(b, sizeof(b), "%d%%", pct);
  tft.drawString(b, 120, y + 20, 2);
}

static void otaReply(bool ok, const char* cmd, const char* m) {
  char b[160];
  snprintf(b, sizeof(b),
           "{\"t\":\"ota\",\"cmd\":\"%s\",\"ok\":%d,\"done\":%u,\"m\":\"%s\"}",
           cmd, ok ? 1 : 0, (unsigned)g_otaDone, m ? m : "");
  size_t need = strlen(b);
  size_t wrote = 0;
  // ★ 回执必须走"命令来的那条路"：命令走串口却从 TCP 回，桥接在串口上永远等不到
  if (g_rxChan == 1 && g_tcp.connected()) {
    wrote = g_tcp.write((const uint8_t*)b, need);
    if (wrote == need) g_tcp.write((const uint8_t*)"\n", 1);
  }
  if (wrote == 0) FARM_SERIAL.println(b);   // TCP 没写出去就走串口兜底
}

/** 处理 {"t":"ota",...} 系列命令 */
static void handleOta(DynamicJsonDocument& doc) {
  const char* cmd = doc["cmd"] | "";

  if (!strcmp(cmd, "abort")) {
    // 放弃升级：上次失败会把 Update 卡在"传输中"，不主动松手就再也起不来
    if (g_linkOta) { Update.abort(); g_linkOta = false; }
    g_otaDone = 0; g_otaTotal = 0; g_otaPct = -1;
    Serial.println("[ota] abort");
    otaReply(true, "abort", "aborted");
    return;
  }

  if (!strcmp(cmd, "begin")) {
    // 先把可能残留的上一轮清干净，否则 Update 会报 already running
    if (g_linkOta || Update.isRunning()) { Update.abort(); g_linkOta = false; }
    size_t sz = (size_t)(doc["size"] | 0);
    g_otaDone = 0;
    g_otaTotal = sz;
    g_otaPct = -1;
    g_otaLastMs = millis();
    Serial.printf("[ota] begin size=%u\n", (unsigned)sz);
    if (!sz) { otaReply(false, "begin", "size=0"); return; }
    // U_FLASH：写进另一个 app 分区（分区表 app0/app1 两个槽）
    if (!Update.begin(sz)) {
      Update.printError(Serial);
      otaReply(false, "begin", "begin failed");
      return;
    }
    g_linkOta = true;
    g_lastAct = millis();          // 升级期间别息屏
    drawOtaScreen(0);
    otaReply(true, "begin", "");
    return;
  }

  if (!strcmp(cmd, "chunk")) {
    if (!g_linkOta) { otaReply(false, "chunk", "not begun"); return; }
    const char* d = doc["b64"] | "";
    int n = b64decode(d, g_otaBuf, (int)sizeof(g_otaBuf));
    g_otaLastMs = millis();
    if (n <= 0) { otaReply(false, "chunk", "empty"); return; }
    size_t w = Update.write(g_otaBuf, (size_t)n);
    if (w != (size_t)n) {
      g_linkOta = false;
      Update.abort();
      otaReply(false, "chunk", "write failed");
      return;
    }
    g_otaDone += w;
    g_lastAct = millis();
    int pct = g_otaTotal ? (int)((100UL * g_otaDone) / g_otaTotal) : 0;
    if (pct != g_otaPct) { g_otaPct = pct; drawOtaScreen(pct); }
    otaReply(true, "chunk", "");
    return;
  }

  if (!strcmp(cmd, "end")) {
    if (!g_linkOta) { otaReply(false, "end", "not begun"); return; }
    bool ok = Update.end(true);
    g_linkOta = false;
    otaReply(ok, "end", ok ? "ok" : "end failed");
    if (ok) {
      Serial.println("[ota] done, reboot");
      tft.fillScreen(TFT_BLACK);
      drawCNCenter(120, 140, "升级完成 正在重启", C_OK, TFT_BLACK);
      delay(800);
      ESP.restart();
    }
    return;
  }

  otaReply(false, "?", "bad cmd");
}

/** 升级看门狗：中途断了不能把工牌永久卡在升级态 */
static void otaWatchdog() {
  if (!g_linkOta) return;
  if (millis() - g_otaLastMs < 15000) return;
  Serial.printf("[ota] 超时自救（已收 %u/%u）\n", (unsigned)g_otaDone, (unsigned)g_otaTotal);
  Update.abort();
  g_linkOta = false;
  g_otaDone = 0; g_otaTotal = 0; g_otaPct = -1;
  otaReply(false, "chunk", "timeout");
}

// ================= 声音告警（板载喇叭）=================
// AI Passport 板载 ES8311 codec + 小喇叭，喇叭挂在 codec 上，必须先把 codec 配好、再走 I2S 送数据。
//   ES8311：I2C 地址 0x18（和电量计共用 Wire），MCU 做 I2S 主、codec 做从
//   I2S  ：MCLK=GPIO6  BCLK=GPIO5  WS=GPIO3  DOUT=GPIO2
//   功放常开，不需要额外的使能 GPIO
#include <driver/i2s.h>
#define ES8311_I2C  0x18
#define PIN_MCLK    6
#define PIN_BCLK    5
#define PIN_WS      3
#define PIN_DOUT    2
// ★ 采样率就锁 16kHz，别改：
//   试过 32kHz（MCLK 8.192MHz），MCLK/BCLK 比例虽然没变，但 8.192MHz 不是
//   160MHz 主频的整数分频 → MCLK 抖动，codec 出来的声音发毛、卡卡响。
//   16kHz = MCLK 4.096MHz，是官方 coeff 表里那一档，实测干净。
#define TONE_SR     16000
// g_alertOn 声明在文件前面的全局区（设置页要用）

static bool es8311Wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_I2C);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static uint8_t es8311Rd(uint8_t reg) {
  Wire.beginTransmission(ES8311_I2C);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)ES8311_I2C, (uint8_t)1);
  return Wire.available() ? (uint8_t)Wire.read() : 0;
}
/** 保留 mask 那些位，再写入 val */
static void es8311Set(uint8_t reg, uint8_t mask, uint8_t val) {
  es8311Wr(reg, (es8311Rd(reg) & mask) | val);
}
static void es8311Or(uint8_t reg, uint8_t val) { es8311Wr(reg, es8311Rd(reg) | val); }

/** 配好 codec：16kHz，MCLK = 256×fs = 4.096MHz */
static bool es8311Init() {
  Wire.beginTransmission(ES8311_I2C);
  if (Wire.endTransmission() != 0) { Serial.println("[audio] 找不到 ES8311"); return false; }
  es8311Wr(0x01, 0x30); es8311Wr(0x02, 0x00); es8311Wr(0x03, 0x10);
  es8311Wr(0x16, 0x24); es8311Wr(0x04, 0x10); es8311Wr(0x05, 0x00);
  es8311Wr(0x0B, 0x00); es8311Wr(0x0C, 0x00);
  es8311Wr(0x10, 0x1F); es8311Wr(0x11, 0x7F);
  es8311Wr(0x00, 0x80);                            // 复位
  es8311Wr(0x00, es8311Rd(0x00) & 0xBF);           // 从模式（时钟由 MCU 给）
  es8311Wr(0x01, 0x3F);
  es8311Wr(0x01, es8311Rd(0x01) & 0x7F);           // MCLK 取自 MCLK 引脚
  es8311Set(0x02, 0x07, 0x00);                     // 预分频 1、倍频 ×1
  es8311Set(0x05, 0x00, 0x00);                     // ADC/DAC 分频 1
  es8311Set(0x03, 0x80, 0x10);                     // 单倍速
  es8311Set(0x04, 0x80, 0x10);                     // DAC 过采样
  es8311Set(0x07, 0xC0, 0x00);                     // LRCK 高字节
  es8311Set(0x08, 0x00, 0xFF);                     // LRCK 低字节
  es8311Set(0x06, 0xE0, 0x03);                     // BCLK = MCLK/4
  es8311Or(0x09, 0x0C);                            // DAC 串口 16bit
  es8311Or(0x0A, 0x0C);                            // ADC 串口 16bit
  es8311Wr(0x13, 0x10); es8311Wr(0x1B, 0x0A); es8311Wr(0x1C, 0x6A);
  return true;
}

/** 音量档位(40/60/80/100) → REG32 的值
 *  ★ ES8311 的 0x32 不是"0~255 线性百分比"，是 **dB 刻度**：
 *      0x00 = -95.5dB（基本静音）
 *      0xBF = 0dB     ← 干净的最大值
 *      0xC0 = +0.5dB
 *      0xFF = +32dB   ← 直接削顶
 *    每 1 LSB = 0.5dB。
 *  ★ 之前写成 `g_alertVol * 255 / 100` 是个大坑：
 *      80%(默认) → 0xCC = **+6.5dB** → 波形被硬削顶 → 听感就是"沙哑、破音"
 *      100%      → 0xFF = **+32dB** → 彻底炸
 *    所以无论怎么调音色都没用，问题出在增益。
 *  现在按 dB 映射，并且**封顶在 0dB（0xBF）**：
 *      40% → -18dB(0x9B)  60% → -12dB(0xA7)
 *      80% →  -6dB(0xB3) 100% →   0dB(0xBF)
 */
static uint8_t volToReg(int pct) {
  if (pct >= 100) return 0xBF;                 // 0dB 封顶，绝不再往上
  if (pct < 0)    pct = 0;
  int step = (100 - pct) * 3 / 5;              // 0.5dB/LSB → 每 1% 约 0.6 LSB
  int reg  = 0xBF - step;
  if (reg < 0) reg = 0;
  return (uint8_t)reg;
}

/** 开 DAC（出声前必须调） */
static void es8311Start() {
  uint8_t dac = (es8311Rd(0x09) & 0xBF) | 0x40;    // 先置"关"位
  dac &= ~0x40;                                     // 再清掉 = 打开 DAC
  es8311Wr(0x09, dac);
  es8311Wr(0x17, 0xBF);
  es8311Wr(0x0E, 0x02);
  es8311Wr(0x12, 0x00);
  es8311Wr(0x14, 0x1A);
  es8311Wr(0x14, es8311Rd(0x14) & ~0x40);
  es8311Wr(0x0D, 0x01);
  es8311Wr(0x15, 0x40);
  es8311Wr(0x37, 0x08);                            // DAC EQ 旁路（官方默认值；
                                                   // 之前误写成 0x48 = 开了 EQ，会染色）
  es8311Wr(0x45, 0x00);
  es8311Wr(0x31, es8311Rd(0x31) & 0x9F);            // 明确解除静音（bit5/6）
  uint8_t vol = volToReg(g_alertVol);
  es8311Wr(0x32, vol);
  Serial.printf("[audio] 音量 %s -> REG32=0x%02X (%+.1fdB)  REG31=0x%02X\n",
                (g_alertVol >= 120) ? "超大" : "档", vol,
                (float)((int)vol - 0xBF) * 0.5f, es8311Rd(0x31));
  if (g_alertVol >= 120)
    Serial.printf("[audio] 超大档：数字端预增益 +5dB + tanh 软饱和（%d%%）\n", g_alertVol);
}
/** 播完进待机，省电 */
static void es8311Stop() {
  es8311Wr(0x32, 0x00); es8311Wr(0x17, 0x00);
  es8311Wr(0x0E, 0xFF); es8311Wr(0x12, 0x02); es8311Wr(0x14, 0x00);
  es8311Wr(0x0D, 0xFA); es8311Wr(0x15, 0x00);
  es8311Wr(0x37, 0x08); es8311Wr(0x45, 0x01);
}

static bool toneStart() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = TONE_SR;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  // 必须立体声（左右各一个时隙）：ES8311 在 I2S 16bit 下按 32bit 一帧取样，
  // 只发一个时隙(ONLY_RIGHT)数据会落错位置 → 喇叭完全没声音。
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  // ★ 缓冲给足：3×256 只有 24ms，WiFi 一忙就会欠载 → 断音"咔咔"响。
  //   8×512 ≈ 256ms，播的时候就算被别的任务打断也不会断流。
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 512;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  cfg.bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num   = PIN_MCLK;
  pins.bck_io_num   = PIN_BCLK;
  pins.ws_io_num    = PIN_WS;
  pins.data_out_num = PIN_DOUT;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) { i2s_driver_uninstall(I2S_NUM_0); return false; }
  return true;
}

/* ---- 正弦查表 ----
 * ★ 为什么不用 sin() / powf()：
 *   ESP32-C3 是 RV32IMC，**没有硬件 FPU**，所有 float/double 全靠软件模拟。
 *   原来每个采样点都要算一次 double 的 sin()，B1929 还加了 double 的 powf()，
 *   16kHz 立体声下 CPU 被吃到 50% 以上 → WiFi 一抢占，I2S DMA 就欠载：
 *   表现就是**断音「卡卡」+ 波形断续「沙哑」**。
 *   改成整数查表 + 定点包络后，每个采样只剩几次乘加，CPU 几乎可以忽略。
 *   （所以之前调频率、换波形都没用 —— 问题根本不在音色上。）
 */
static int16_t g_sine[257];            // 多留 1 个，插值时 idx+1 不用取模
static bool    g_sineReady = false;
static void sineInit() {
  if (g_sineReady) return;
  for (int i = 0; i <= 256; i++)
    g_sine[i] = (int16_t)(sinf(2.0f * 3.14159265f * (float)(i & 255) / 256.0f) * 32000.0f);
  g_sineReady = true;
}

/* ---- 软饱和表（只给「超大」音量档用）----
 * DAC 已经锁在 0dB 了，想更响只能从数字端压。但直接乘增益 = 硬削顶，
 * 就是之前"沙哑"的来历。所以改成 tanh 软限幅：
 *   - 先乘 2.2 倍预增益（+7dB，小信号真放大）
 *   - 再走 tanh 曲线，大信号平滑压到满量程，而不是一刀切平
 * 失真只剩温和的奇次谐波 → 听感是"更厚更冲"，不是破裂。
 * 表索引 = 幅度 >> 8，覆盖 0..71680（2.2 × 32000 = 70400，留点余量）。
 */
#define BOOST_PG   1.8f
#define SOFT_N     280
static int16_t g_soft[SOFT_N];
static bool    g_softReady = false;
static void softInit() {
  if (g_softReady) return;
  for (int i = 0; i < SOFT_N; i++) {
    float in = (float)(i << 8);
    float y  = tanhf(in / 32767.0f) * 32767.0f;    // 小信号≈1:1，大信号平滑饱和
    int v = (int)(y + 0.5f);
    if (v > 32767) v = 32767;
    g_soft[i] = (int16_t)v;
  }
  g_softReady = true;
}

/** 播一个音调（整数运算，无浮点热路径）
 *  wave:    0=纯正弦（唯一在用的） 1=方波（保留，未使用）
 *  envMode: 1=钟形包络（8ms 起音 + 指数衰减 + 10ms 收尾） 0=平顶
 */
static void tone(int freq, int ms, float amp, int wave = 0, int envMode = 1) {
  sineInit();
  const int CH = 128;
  int16_t buf[CH * 2];                 // 立体声：左右时隙都填同样的采样
  int total = (long)TONE_SR * ms / 1000;
  if (total < 8) return;
  int atk  = (long)TONE_SR * 8 / 1000;                  // 8ms 起音
  int tail = (long)TONE_SR * 10 / 1000;                 // 10ms 收尾（防咔哒）
  if (atk  > total / 3) atk  = total / 3;
  if (tail > total / 3) tail = total / 3;
  int body = total - atk - tail;
  if (body < 1) body = 1;

  bool boost = (g_alertVol >= 120);            // 「超大」档
  if (boost) softInit();                       // 只有这一档才用软饱和表

  // 总幅度：amp=1.0 → 243/256 ≈ 0.95 满量程；超大档顶到 256（≈0.98）
  // （DAC 现在封顶 0dB，不会再有 +6.5dB 的削顶，所以数字端可以推到这么高）
  int ampQ = (int)(amp * (boost ? 256.0f : 243.0f));
  if (ampQ > 256) ampQ = 256;

  // 相位步进：32 位累加器转满一圈 = 2^32
  uint32_t step = (uint32_t)(((uint64_t)(uint32_t)freq << 32) / (uint64_t)TONE_SR);

  // 每采样的衰减乘数（Q16）——整段只算一次 powf，不在热路径里
  int32_t mulQ = 65536;
  if (envMode != 0) {
    // 超大档把包络拖平（衰得少），同样峰值下能量更多 → 更响，且不用削顶
    float endLvl = boost ? ((ms > 400) ? 0.70f : 0.55f)
                         : ((ms > 400) ? 0.45f : 0.28f);
    mulQ = (int32_t)(powf(endLvl, 1.0f / (float)body) * 65536.0f);
    if (mulQ < 0)     mulQ = 0;
    if (mulQ > 65536) mulQ = 65536;
  }

  uint32_t phase = 0;
  int32_t envQ = 0, tailStep = 0;
  int done = 0;
  while (done < total) {
    int n = total - done;
    if (n > CH) n = CH;
    for (int i = 0; i < n; i++) {
      int p = done + i;
      if (p < atk) {                                    // 起音：线性爬升
        envQ = (int32_t)((int64_t)65536 * p / atk);
      } else if (p < atk + body) {                      // 主体：定点指数衰减
        if (p == atk) envQ = 65536;
        else { envQ = (int32_t)(((int64_t)envQ * mulQ) >> 16); if (envQ < 0) envQ = 0; }
      } else {                                          // 收尾：线性降到 0
        if (p == atk + body) { tailStep = envQ / tail; if (tailStep < 1) tailStep = 1; }
        envQ -= tailStep;
        if (envQ < 0) envQ = 0;
      }
      int idx  = (int)(phase >> 24);                    // 0..255
      int frac = (int)((phase >> 16) & 0xFF);           // 8 位小数
      int32_t a = g_sine[idx], b = g_sine[idx + 1];
      int32_t s = a + (((b - a) * frac) >> 8);          // 线性插值
      if (wave == 1) s = (s >= 0 ? 22000 : -22000);     // 方波（未使用）
      int32_t v = (s * envQ) >> 16;                     // 乘包络
      v = (v * ampQ) >> 8;                              // 乘总幅度
      if (boost) {
        // 超大档：+5dB 预增益 → tanh 软饱和（不是硬削顶）
        int32_t g2 = v * 18 / 10;
        int32_t a2 = (g2 < 0) ? -g2 : g2;
        int idx2 = (int)(a2 >> 8);
        if (idx2 >= SOFT_N) idx2 = SOFT_N - 1;
        int32_t o = g_soft[idx2];
        v = (g2 < 0) ? -o : o;
      } else {
        if (v > 32000) v = 32000;
        else if (v < -32000) v = -32000;
      }
      int16_t sv = (int16_t)v;
      buf[i * 2] = sv; buf[i * 2 + 1] = sv;
      phase += step;
    }
    size_t w = 0;
    i2s_write(I2S_NUM_0, buf, n * 4, &w, portMAX_DELAY);
    done += n;
  }
}
/** 静音一小段（把 DMA 里剩的推完，保证节奏） */
static void toneGap(int ms) {
  int16_t buf[128];
  memset(buf, 0, sizeof(buf));
  int total = (long)TONE_SR * ms / 1000, done = 0;
  while (done < total) {
    int n = total - done;
    if (n > 64) n = 64;                // 64 帧 × 2 声道 = 128 个 int16
    size_t w = 0;
    i2s_write(I2S_NUM_0, buf, n * 4, &w, portMAX_DELAY);
    done += n;
  }
}

/* ---- 音效库 ----
 * ★ 这块小喇叭的频响很窄（约 900Hz 以上就开始掉，2kHz 基本没了）：
 *   988/1319Hz 几乎听不见；方波的谐波也被削平 → 方波和正弦在喇叭上听着差不多。
 *   所以区分音效**只能靠节奏**（几声、多长、怎么停），音高只在 494~784Hz 里微调。
 *   音符数刻意错开：完成 3 / 2 / 4 / 7 声，故障 6 / 2 / 10 / 3 声。
 * ★ 全部用纯正弦 + 钟形包络（8ms 起音、指数衰减到 28%/45%、10ms 收尾）。
 *   踩过的坑（改之前先看）：
 *   - 双音和弦（f+纯五度）= 老式手机铃声 → 废弃
 *   - 平顶持续音 = 老式手机鸣笛 → 必须衰减
 *   - 衰减到 10% 太狠 → 每声变成"嗒"，连起来卡卡的
 *   - 采样率 32kHz → MCLK 抖动，声音发毛
 *   - 音高压到 440/392Hz → 小喇叭推不动，破音沙哑
 *   - 幅度顶到 30000 → 后级过载，破音
 * 想加新音效就在这两个函数里加 case，同时把名字加进 FINISH_NAMES / FAULT_NAMES，
 * 并同步改 tools/preview_tones.py（那个脚本会模拟喇叭频响，方便在电脑上试听）。
 */
static void playFinishTone(int i) {
  switch (i) {
    case 1:                              // 双音：高→低两声，大跳下行（2 声）
      tone(784, 200, 1.0f); toneGap(110);
      tone(523, 430, 1.0f);
      break;
    case 2:                              // 清脆：三声极短 + 一声收尾（4 声，最干脆）
      tone(784, 80, 1.0f); toneGap(45);
      tone(784, 80, 1.0f); toneGap(45);
      tone(659, 80, 1.0f); toneGap(45);
      tone(523, 400, 1.0f);
      break;
    case 3:                              // 欢快：来回抖三下 + 长收尾（7 声，最多最快）
      for (int k = 0; k < 3; k++) {
        tone(659, 75, 1.0f); toneGap(40);
        tone(784, 75, 1.0f); toneGap(40);
      }
      tone(784, 420, 1.0f);
      break;
    default:                             // 琶音：三声上行钟声「叮—叮—咚」（3 声）
      tone(523, 160, 1.0f); toneGap(65);
      tone(659, 160, 1.0f); toneGap(65);
      tone(784, 460, 1.0f);
      break;
  }
}
static void playFaultTone(int i) {
  switch (i) {
    case 1:                              // 长鸣：两声拖长的下行低音（更慢、更低，跟"三连"拉开）
      tone(587, 650, 1.0f); toneGap(160);
      tone(523, 720, 1.0f);
      break;
    case 2:                              // 四短：急促四连击 + 一落（10 声，最急）
      for (int r = 0; r < 2; r++) {
        tone(784, 85, 1.0f); toneGap(50);
        tone(784, 85, 1.0f); toneGap(50);
        tone(784, 85, 1.0f); toneGap(50);
        tone(784, 85, 1.0f); toneGap(100);
        tone(587, 300, 1.0f); toneGap(240);
      }
      break;
    case 3:                              // 低沉：三声闷响，音高压到最低（3 声，最闷）
      // ★ 别再往下压：试过 440/392Hz，这块小喇叭推不动那么低，
      //   只会破音发沙哑。494Hz 是它还能干净出声的下限。
      tone(523, 300, 1.0f); toneGap(150);
      tone(494, 300, 1.0f); toneGap(150);
      tone(494, 460, 1.0f);
      break;
    default:                             // 三连：正弦急促下行 784/784/587，来两轮
      for (int r = 0; r < 2; r++) {
        tone(784, 110, 1.0f); toneGap(60);
        tone(784, 110, 1.0f); toneGap(60);
        tone(587, 220, 1.0f); toneGap(240);
      }
      break;
  }
}

/** 播一段告警音：0=任务完成  1=异常  2=提示
 *  force=true 时无视"声音告警"开关（铃声页里手动试听用，关了也能听） */
static void playAlert(int kind, bool force) {
  if (!force && !g_alertOn) return;
  Serial.printf("[audio] 播告警 kind=%d%s 选择=%d 音量=%d%%\n", kind, force ? " (试听)" : "",
                (kind == 0 ? g_toneFinish : (kind == 1 ? g_toneFault : 0)), g_alertVol);
  if (!es8311Init()) return;
  Serial.println("[audio] ES8311 已就绪");
  if (!toneStart()) { Serial.println("[audio] I2S 起不来"); return; }
  es8311Start();
  if (kind == 0) playFinishTone(g_toneFinish);
  else if (kind == 1) playFaultTone(g_toneFault);
  else tone(784, 120, 1.0f);             // 提示：一声短音
  toneGap(80);
  es8311Stop();
  i2s_driver_uninstall(I2S_NUM_0);
  // 播放期间 loop 是卡住的（I2S 阻塞写），收不到 pong；
  // 不刷新的话刚播完就会被保活误判成"连接死了"而踢掉重连。
  g_pongAt = millis();
  g_pongSeenAt = g_pingSent;
}

// ================= 串口解析 =================
static void handleLine(const String& line) {
  g_rxLines++;
  DynamicJsonDocument doc(12288);
  if (deserializeJson(doc, line)) {
    g_rxFail++;
    // 解析失败往往意味着两路数据串在一起了，打印出来好定位（限流，别刷屏）
    static unsigned long lastBadLog = 0;
    if (millis() - lastBadLog > 20000) {
      lastBadLog = millis();
      char b[100];
      int n = (int)line.length();
      if (n > 90) n = 90;
      for (int i = 0; i < n; i++) b[i] = (line[i] == '"' || line[i] == '\\') ? '.' : line[i];
      b[n] = 0;
      Serial.printf("[rx] 解析失败 chan=%d len=%d : %s\n", g_rxChan, (int)line.length(), b);
    }
    return;
  }
  const char* t = doc["t"] | "";
  // 桥接的心跳回应：证明这条连接还活着
  if (!strcmp(t, "pong")) {
    g_pongGot++;
    g_pongSeenAt = g_pingSent;
    g_pongAt = millis();
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 20000) {
      lastLog = millis();
      Serial.printf("[tcp] pong ok (第 %lu 个 / 已发 ping %lu)\n", g_pongGot, g_pingSent);
    }
    return;
  }
  // 无线升级：独立于业务命令处理，升级期间不解析别的
  if (!strcmp(t, "ota")) { handleOta(doc); return; }
  if (g_linkOta) return;

  // 桥接下发 WiFi 凭据：存 NVS 后立刻用新凭据重连 —— 换 WiFi 不用重新烧录
  if (!strcmp(t, "wificfg")) {
    const char* ss = doc["ssid"] | "";
    const char* pk = doc["pass"] | "";
    if (ss[0]) {
      saveWifiPref(ss, pk);
      Serial.printf("[wifi] 收到下发凭据 ssid=%s，保存并重连\n", ss);
      sendJson("{\"t\":\"wificfg\",\"ok\":1}");   // 先回 ack，再断链路
      showToast("WiFi 已更新，重连中");
      WiFi.disconnect(true);
      g_wifiState = WIFI_IDLE;      // 回到 IDLE，状态机会用新凭据重新 wifiStart()
      g_wifiRetryAt = 0;
      g_udpReady = false;
    } else {
      sendJson("{\"t\":\"wificfg\",\"ok\":0,\"msg\":\"缺少 ssid\"}");
    }
    return;
  }
  if (!strcmp(t, "state")) {
    // 诊断：首次收到 state 时回显前 160 字节，确认报文内容和 bid 字段
    static bool echoed = false;
    if (!echoed) {
      echoed = true;
      char eb[200];
      int n = (int)line.length();
      if (n > 160) n = 160;
      for (int i = 0; i < n; i++) eb[i] = (line[i] == '"' || line[i] == '\\') ? '.' : line[i];
      eb[n] = 0;
      char e2[220];
      snprintf(e2, sizeof(e2), "{\"t\":\"raw\",\"len\":%d,\"s\":\"%s\"}", (int)line.length(), eb);
      sendJson(e2);
    }
    JsonObject srv = doc["srv"];
    if (srv) strlcpy(g_srvName, srv["name"] | "---", sizeof(g_srvName));
    // 桥接把自己的候选 IP 告诉我们，挑同网段的记下来
    JsonArray bids = doc["bid"].as<JsonArray>();
    if (!bids.isNull()) {
      String myIp = WiFi.localIP().toString();
      // 用下标遍历：ArduinoJson v7 的 as<JsonArray>() 代理对象 range-for 不迭代
      for (size_t bi = 0; bi < bids.size(); bi++) {
        const char* b = bids[bi].as<const char*>();
        if (!b || !b[0]) continue;
        if (sameSubnet24(b, myIp.c_str())) {
          if (strcmp(b, g_bridgeIP) != 0) {
            strlcpy(g_bridgeIP, b, sizeof(g_bridgeIP));
            char bb[80];
            snprintf(bb, sizeof(bb), "{\"t\":\"wifi\",\"tag\":\"bridge\",\"ip\":\"%s\"}", g_bridgeIP);
            sendJson(bb);
          }
          break;
        }
        if (!g_bridgeIP[0]) strlcpy(g_bridgeIP, b, sizeof(g_bridgeIP));
      }
    }
    JsonObject n = doc["n"];
    if (n) {
      g_run = n["run"] | 0; g_pause = n["pause"] | 0; g_idle = n["idle"] | 0;
      g_offline = n["offline"] | 0; g_total = n["total"] | 0; g_queue = n["queue"] | 0;
    }
    g_devs.clear();
    for (JsonArray d : doc["d"].as<JsonArray>()) {
      Dev dev{};
      strlcpy(dev.name, d[0] | "", sizeof(dev.name));
      strlcpy(dev.state, d[1] | "", sizeof(dev.state));
      dev.progress = d[2] | 0;
      strlcpy(dev.task, d[3] | "", sizeof(dev.task));
      dev.remain = d[4] | 0;
      dev.nozzle = d[5] | 0;
      dev.bed = d[6] | 0;
      dev.online = (d[7] | 0) != 0;
      g_devs.push_back(dev);
    }
    g_tasks.clear();
    for (JsonArray q : doc["q"].as<JsonArray>()) {
      Task tk{};
      strlcpy(tk.name, q[0] | "", sizeof(tk.name));
      tk.remaining = q[1] | 0;
      tk.working = q[2] | 0;
      tk.flags = q[3] | 0;
      g_tasks.push_back(tk);
    }
    if ((int)g_picked.size() != (int)g_devs.size()) g_picked.assign(g_devs.size(), 0);
    g_hasState = true;
    g_needRedraw = true;
    char ack[96];
    snprintf(ack, sizeof(ack),
             "{\"t\":\"ok\",\"devs\":%d,\"tasks\":%d,\"rx\":%lu,\"v\":\"" BUILD_TAG "\",\"bat\":%d,\"mv\":%d}",
             (int)g_devs.size(), (int)g_tasks.size(), g_rxBytes, g_battPct, (int)(g_battV * 1000));
    sendJson(ack);

  } else if (!strcmp(t, "event")) {
    Ev ev{};
    strlcpy(ev.kind, doc["kind"] | "state", sizeof(ev.kind));
    strlcpy(ev.dev, doc["dev"] | "", sizeof(ev.dev));
    strlcpy(ev.reason, doc["reason"] | "", sizeof(ev.reason));
    strlcpy(ev.from, doc["from"] | "", sizeof(ev.from));
    strlcpy(ev.to, doc["to"] | "", sizeof(ev.to));
    g_events.insert(g_events.begin(), ev);
    if (g_events.size() > 20) g_events.pop_back();
    g_needRedraw = true;

    // ---- 声音告警 ----
    // 完成：两声上行；异常（失败/暂停/HMS 报警）：三声急促下行
    bool isFinish = !strcmp(ev.kind, "finish");
    bool isAlarm  = !strcmp(ev.kind, "fail") || !strcmp(ev.kind, "pause") || !strcmp(ev.kind, "hms");
    // 每个事件都留一行日志：排查"该响没响"时先看这里有没有收到对应 kind
    {
      static unsigned long lastEvtLog = 0;
      if (millis() - lastEvtLog > 3000) {   // 限流，别把串口刷爆
        lastEvtLog = millis();
        Serial.printf("[evt] kind=%s dev=%s to=%s\n", ev.kind, ev.dev, ev.to);
      }
    }
    if (isFinish || isAlarm) {
      // 冷却：30 台机器同时出状况时不至于响个没完
      static unsigned long lastFinish = 0, lastAlarm = 0;
      unsigned long now = millis();
      bool canFinish = isFinish && (now - lastFinish > 3000);
      bool canAlarm  = isAlarm  && (now - lastAlarm  > 20000);
      if (!g_alertOn) {
        Serial.printf("[alarm] 开关关闭，不响 kind=%s\n", ev.kind);
      } else if (canFinish || canAlarm) {
        if (isFinish) lastFinish = now; else lastAlarm = now;
        if (g_screenOff) screenOn();     // 顺手点亮屏幕，好看到底哪台出事了
        Serial.printf("[alarm] 响铃 %s kind=%s\n", isFinish ? "完成" : "异常", ev.kind);
        playAlert(isFinish ? 0 : 1);
      } else {
        Serial.printf("[alarm] 冷却中，不响 kind=%s\n", ev.kind);
      }
    }
  } else if (!strcmp(t, "ack")) {
    g_launching = 0;
    const char* msg = doc["msg"] | "";
    JsonArray res = doc["res"].as<JsonArray>();
    if (!res.isNull() && g_page == P_OPTIONS) {
      g_results.clear();
      for (JsonArray r : res) {
        LRes lr{};
        strlcpy(lr.name, r[0] | "", sizeof(lr.name));
        lr.code = r[1] | 0;
        strlcpy(lr.msg, r[2] | "", sizeof(lr.msg));
        g_results.push_back(lr);
      }
      g_page = P_RESULT;
      g_sel = 0;
    } else {
      showToast(msg[0] ? msg : ((doc["ok"] | false) ? "完成" : "失败"));
    }
    g_needRedraw = true;

  } else if (!strcmp(t, "err")) {
    g_launching = 0;
    showToast(doc["msg"] | "桥接报错");
  }
}

// ================= 无线链路（工牌 ↔ PC 桥接）=================
// 工牌连本地 WiFi（路由器同时支持 WPA2-PSK/CCMP，C3 可用），用 UDP 广播找桥接，
// 再用 TCP 长连接收发 JSON —— 和 USB 串口同一套协议。


static void wifiStart() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname("ai-passport");
  // 凭据优先级：桥接运行时下发的（NVS） > 编译期的 wifi_config.h
  char ss[33] = {0}, pk[65] = {0};
  if (loadWifiPref(ss, sizeof(ss), pk, sizeof(pk))) {
    WiFi.begin(ss, pk);                 // 用桥接下发来的
  } else if (strncmp(WIFI_SSID, "YOUR_", 5) == 0) {
    // 既没下发过、编译期也是占位符：别傻连，明确告诉用户去配置
    static bool warned = false;
    if (!warned) { warned = true; showToast("未配置 WiFi：请用桥接下发"); }
    Serial.println("[wifi] 未配置：NVS 无凭据且编译期为占位符");
    g_wifiState = WIFI_FAIL;
    g_wifiRetryAt = millis() + 30000;
    return;
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASS);   // WPA2-PSK（路由器兼容）
  }
  g_wifiState = WIFI_CONNECTING;
  g_wifiT0 = millis();
}

static void wifiReport(const char* tag) {
  String ip = WiFi.localIP().toString();     // 别用临时对象的 c_str()（悬垂指针）
  char b[140];
  snprintf(b, sizeof(b),
           "{\"t\":\"wifi\",\"tag\":\"%s\",\"st\":%d,\"ip\":\"%s\",\"rssi\":%d,\"ch\":%d,\"g\":%lu}",
           tag, (int)WiFi.status(), ip.c_str(), (int)WiFi.RSSI(), (int)WiFi.channel(),
           (unsigned long)scanFreeGpios());
  sendJson(b);
}

/** 判断两个 IP 是否同一 /24 网段 */
static bool sameSubnet24(const char* a, const char* b) {
  if (!a || !b) return false;
  for (int i = 0; i < 3; i++) {
    const char* pa = strchr(a, '.');
    const char* pb = strchr(b, '.');
    if (!pa || !pb) return false;
    if ((pa - a) != (pb - b)) return false;
    if (strncmp(a, b, pa - a) != 0) return false;
    a = pa + 1; b = pb + 1;
  }
  return true;
}

/**
 * 无线链路策略：工牌当 TCP 服务器，PC 桥接主动连过来。
 * 原因：Windows 防火墙默认挡入站，而工牌在局域网里发广播是"出站"（不受限），
 *       PC 主动连工牌也是"出站"——两边都不需要开防火墙入站规则。
 * 流程：PC 广播 "FARM?" → 工牌回 "HERE:<ip>" → PC 连 <ip>:8897。
 */
/** 电源键探测：只读空闲 GPIO 电平，不改任何配置（零风险）
 *  bit0=GPIO11 bit1=GPIO12 ... bit6=GPIO17；被按下的读成 0 */
static uint32_t scanFreeGpios() {
  static const uint8_t pins[] = { 11, 12, 13, 14, 15, 16, 17 };
  uint32_t mask = 0;
  for (int i = 0; i < 7; i++) {
    if (digitalRead(pins[i]) == LOW) mask |= (1UL << i);
  }
  return mask;
}

/** 无线升级初始化。必须等 WiFi 连上、lwip 就绪之后再调，否则必崩
 *  （assert failed: tcpip_send_msg_wait_sem ... Invalid mbox） */
static void otaBegin() {
  if (g_otaReady) return;
  ArduinoOTA.setHostname("aipassport-farm");
  ArduinoOTA.onStart([]() {
    Serial.println("[ota] start");
    tft.fillScreen(TFT_BLACK);
    drawCNCenter(120, 140, "正在无线升级固件", C_WARN, TFT_BLACK);
    drawCNCenter(120, 164, "请勿断电", C_DIM, TFT_BLACK);
  });
  ArduinoOTA.onEnd([]() { Serial.println("[ota] done"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] error %u\n", e); });
  ArduinoOTA.begin();
  g_otaReady = true;
  Serial.println("[ota] ready");
}

static void wifiTick() {
  // 心跳：每 5 秒报一次状态，方便排错
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat > 5000) {
    lastBeat = millis();
    wifiReport("hb");
  }
  // ---- TCP 保活 ----
  // 对端（电脑）可能早就没了，但工牌这边还以为连着，写进去"成功"其实发不出去，
  // 表现就是"命令发出去石沉大海"。所以每 4 秒 ping 一次，等不到 pong 就强行释放，
  // 好让新的连接能进来。
  if (g_tcp.connected()) {
    static unsigned long lastPing = 0;
    if (millis() - lastPing > 5000) {
      lastPing = millis();
      g_pingSent++;
      char pb[40];
      snprintf(pb, sizeof(pb), "{\"t\":\"ping\",\"i\":%lu}", (unsigned long)(millis() / 1000));
      g_tcp.print(pb);
      g_tcp.print("\n");
      // 注意：这里**不**因为收不到 pong 就断开连接。
      // 实测 pong 经常丢（发 43 个只回 10 个），强行断开会让连接每隔十几秒
      // 重连一次，反而更不稳。判断"哪条连接是活的"交给桥接去做 —— 它看
      // 工牌心跳落在哪条连接上，那个信号准得多。
      if (g_pingSent - g_pongSeenAt >= 6) {
        Serial.printf("[tcp] 提示：连发 %lu 次 ping 没收到 pong（桥接可能没在跑）\n",
                      g_pingSent - g_pongSeenAt);
        g_pongSeenAt = g_pingSent;      // 只提示，不踢连接
      }
    }
  }

  switch (g_wifiState) {
    case WIFI_IDLE:
      if (millis() > 2000) wifiStart();
      break;
    case WIFI_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        g_wifiState = WIFI_OK;
        wifiReport("connected");
        g_tcpServer.begin();          // 工牌开 TCP 服务端
        g_udp.begin(BRIDGE_DISC_PORT); // 收 PC 的广播
        g_udpReady = true;
        otaBegin();                   // 无线升级：必须等 WiFi 拿到 IP 之后再起
        g_pongAt = millis();          // 新连接先当成活的，等 pong 超时再说
      } else if (millis() - g_wifiT0 > 25000) {
        g_wifiState = WIFI_FAIL;
        wifiReport("timeout");
        g_wifiRetryAt = millis() + 5000;
      }
      break;
    case WIFI_FAIL:
      if (millis() > g_wifiRetryAt) { g_wifiState = WIFI_IDLE; WiFi.disconnect(true); }
      break;
    default: break;
  }
  if (g_wifiState == WIFI_OK) {
    // 接受 PC 的 TCP 连接
    if (!g_tcp.connected()) {
      // backlog 里可能压着好几条（扫描探测留下的），其中不少早就断了。
      // 全部取出来丢掉、只留最后（最新）一条，能大幅降低接到"半死连接"的概率。
      WiFiClient last;
      int dropped = 0;
      for (;;) {
        WiFiClient c = g_tcpServer.available();
        if (!c) break;
        if (last) { last.stop(); dropped++; }
        last = c;
      }
      WiFiClient incoming = last;
      if (incoming) {
        if (dropped) Serial.printf("[tcp] 丢弃 %d 条积压连接，用最新这条\n", dropped);
        g_tcp = incoming;
        g_pongAt = millis();   // 新连接默认是活的
        // 先报个到，让桥接确认这就是工牌（它靠这个做网段扫描识别）
        char b[96];
        snprintf(b, sizeof(b),
                 "{\"t\":\"hello\",\"ip\":\"%s\",\"port\":%d,\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
                 WiFi.localIP().toString().c_str(), BRIDGE_TCP_PORT,
                 (unsigned char)WiFi.macAddress()[0], (unsigned char)WiFi.macAddress()[1],
                 (unsigned char)WiFi.macAddress()[2], (unsigned char)WiFi.macAddress()[3],
                 (unsigned char)WiFi.macAddress()[4], (unsigned char)WiFi.macAddress()[5]);
        sendJson(b);
      }
    }
    // 回应 PC 的广播：告诉它我在哪
    while (g_udp.parsePacket()) {
      char buf[32];
      int n = g_udp.read(buf, sizeof(buf) - 1);
      if (n > 0) {
        buf[n] = 0;
        if (strstr(buf, "FARM?")) {
          IPAddress peer = g_udp.remoteIP();
          g_udp.beginPacket(peer, g_udp.remotePort());
          g_udp.print("HERE:");
          g_udp.print(WiFi.localIP().toString());
          g_udp.endPacket();
        }
      }
    }
  }
}

// ================= 电池（CW2017 电量计，I2C0）=================
// AI Passport 板载 CW2017，7位地址 0x63，SDA=GPIO10 / SCL=GPIO7（与音频 codec 共总线）
// 寄存器：0x00 VERSION / 0x02 VCELL(16bit, 0.3125mV/LSB) / 0x04 SOC(1字节百分比)
//         0x08 MODE(0x30=休眠) / 0x0B SOC_ALERT(bit7=已写电池曲线) / 0x10 BATINFO(80字节曲线)
// ⚠ 电池相关代码必须放在结构体定义之后 —— Arduino 会在"第一个函数定义"之前插入函数原型，
//   放前面会让原型里的自定义类型找不到定义。

/** 收一个字节，攒够一行就解析。
 *  ★ 串口和 TCP 必须用各自的行缓冲：
 *    以前共用一个，USB 的短包（比如桥接回的 pong）会跟 TCP 的大包（状态 2KB）
 *    拼成一行，两边都解析失败 —— 表现为"心跳明明发了却收不到回应"。 */
static void feedChar(char c, int chan) {
  g_rxBytes++;
  String& L = (chan == 1) ? g_lineTcp : g_lineSer;
  if (c == '\n') {
    if (L.length() > 2) handleLine(L);
    L = "";
  } else if (c != '\r') {
    L += c;
    if (L.length() > 4096) { L = ""; g_rxFail++; }   // 兜底防溢出
  }
}

// ================= 入口 =================
void setup() {
  FARM_SERIAL.setRxBufferSize(4096);
  FARM_SERIAL.begin(115200);
  analogReadResolution(12);

  tft.init();
  // ST7789P3 厂商专属初始化序列
  tft.writecommand(0xB2); tft.writedata(0x0C); tft.writedata(0x0C); tft.writedata(0x00); tft.writedata(0x33); tft.writedata(0x33);
  tft.writecommand(0xB7); tft.writedata(0x35);
  tft.writecommand(0xBB); tft.writedata(0x28);
  tft.writecommand(0xC0); tft.writedata(0x0C);
  tft.writecommand(0xC2); tft.writedata(0x01); tft.writedata(0xFF);
  tft.writecommand(0xC3); tft.writedata(0x10);
  tft.writecommand(0xC4); tft.writedata(0x20);
  tft.writecommand(0xC6); tft.writedata(0x0F);
  tft.writecommand(0xD0); tft.writedata(0xA4); tft.writedata(0xA1);
  tft.writecommand(0xE0);
  const uint8_t pv[] = {0xD0,0x00,0x05,0x0E,0x15,0x0D,0x37,0x43,0x47,0x09,0x15,0x12,0x16,0x19};
  for (uint8_t v : pv) tft.writedata(v);
  tft.writecommand(0xE1);
  const uint8_t nv[] = {0xD0,0x00,0x05,0x0D,0x0C,0x06,0x2D,0x44,0x40,0x0E,0x1C,0x18,0x16,0x19};
  for (uint8_t v : nv) tft.writedata(v);
  tft.writecommand(0x21);
  tft.fillScreen(C_BG);

  ledcSetup(0, 5000, 10);
  ledcAttachPin(PIN_BL, 0);
  ledcWrite(0, 900);

  loadSleepPref();      // 读上次设的自动息屏时间
  loadAlertPref();      // 读声音告警开关
  batteryInit();
  batteryRead();

  drawCNCenter(120, 130, "AI Passport", C_ACCENT, C_BG);
  drawCNCenter(120, 156, "农场遥控器 启动中…", C_DIM, C_BG);
  drawCNCenter(120, 184, "版本 " BUILD_TAG, C_WARN, C_BG);
  sendJson("{\"t\":\"hello\"}");

  // OTA 不在这里 begin —— 此时 WiFi 还没连上，lwip 未就绪会直接崩
  // （assert failed: tcpip_send_msg_wait_sem ... Invalid mbox）
  // 真正的初始化放在 wifiTick() 里、WiFi 拿到 IP 之后。
}

void loop() {
  if (g_otaReady) ArduinoOTA.handle();   // 无线升级：以后刷固件不用插数据线
  wifiTick();

  // 两个通道喂进同一个行缓冲：USB 串口 + WiFi TCP
  g_rxChan = 0;
  while (FARM_SERIAL.available()) feedChar(FARM_SERIAL.read(), 0);
  g_rxChan = 1;
  while (g_tcp.connected() && g_tcp.available()) feedChar(g_tcp.read(), 1);
  // TCP 掉线要马上释放，不然新连接进不来
  // （症状：桥接日志显示"已连上工牌"，但工牌对任何命令都没反应 ——
  //   因为它还在读一条早就断了的老连接）
  {
    static bool tcpWasUp = false;
    bool tcpUp = g_tcp.connected();
    if (tcpWasUp && !tcpUp) {
      g_tcp.stop();
      sendJson("{\"t\":\"wifi\",\"tag\":\"tcp-lost\"}");
    }
    tcpWasUp = tcpUp;
  }

  // 无线升级中：只喂数据、不响应按键、不重画页面
  otaWatchdog();
  if (g_linkOta) return;

  int b = pollButton();
  if (b != B_NONE) {
    g_lastAct = millis();
    if (g_screenOff) { screenOn(); }        // 息屏时任意键先唤醒
    else {
      if (b == B_UP) actUp();
      else if (b == B_DOWN) actDown();
      else if (b == B_OK) actOk();
    }
  }
  // 长按 OK 1 秒 = 一路退回总览
  if (longPressed(B_OK, LONG_OK_MS)) {
    g_lastAct = millis();
    if (g_screenOff) screenOn();
    else if (g_page != P_HOME) { g_page = P_HOME; g_sel = 0; g_focus = 0; g_needRedraw = true; }
  }
  // 无操作自动息屏（时长在「设置」里可调）
  if (!g_screenOff && millis() - g_lastAct > idleSleepMs()) screenOff();

  // 按键电压上报（排错用：按下任意键时把实测 mV 发给电脑端）
  {
    static unsigned long lastKeyReport = 0;
    int rawMv = analogReadMilliVolts(PIN_BTN_ADC);
    if (rawMv < BTN_OK_MAX && millis() - lastKeyReport > 500) {
      lastKeyReport = millis();
      char kb[40];
      snprintf(kb, sizeof(kb), "{\"t\":\"key\",\"mv\":%d,\"as\":%d}", rawMv, b);
      sendJson(kb);
    }
  }

  static unsigned long lastReq = 0;
  if (millis() - lastReq > 15000) { lastReq = millis(); sendGetState(); }

  static unsigned long lastDiag = 0;
  if (!g_hasState && millis() - lastDiag > 1000) { lastDiag = millis(); g_needRedraw = true; }

  // 电量每 10 秒读一次（CW2017 自身也有更新周期，读太勤没意义）
  static unsigned long lastBatt = 0;
  if (millis() - lastBatt > 10000) {
    lastBatt = millis();
    int oldPct = g_battPct;
    batteryRead();
    if (g_battPct != oldPct) g_needRedraw = true;
  }

  if (g_needRedraw && !g_screenOff) drawPage();
}
