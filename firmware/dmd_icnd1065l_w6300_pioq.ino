/*--------------------------------------------------------------------------------------
  Production firmware for the ICND1065L HUB320 panel on a W6300-EVB-Pico2.

  PIO-driven 74HC595 row mux + SRAM 5x7 font boot splash. Stream over W6300 quad QSPI,
  OTA via the panel web UI. The steady brightness offset of the seam columns is a
  hardware artifact; this build targets scan timing (micro-flashes), not that DC offset.

  W6300-EVB-Pico2 - MAX performance: QUAD-mode QSPI-PIO + DMA, 13-bit -> panel.

  Quad mode = 4 data bits per SCLK. At ~33 MHz SCLK that's ~132 Mbit/s (vs ~33 in single mode), so a
  13-bit frame (208 KB) transfers in ~13 ms -> plenty of headroom for >=20 fps (theoretically ~70).
  Instruction phase is sent 1-bit-serial (per datasheet), address+data go 4-bit-wide - WIZnet's
  mk_cmd_buf quad packing handles that. Transport = WIZnet wizchip_qspi_pio quad branch, transcribed
  for pio1 + our pins. Everything else (panel, UDP receive/reassembly) is unchanged.

  Pins: CS=16 SCLK=17 IO0=18 IO1=19 IO2=20 IO3=21 RST=22. verpixeld: ColorBits=13, high TargetMbps.
--------------------------------------------------------------------------------------*/
#if !defined(ARDUINO_ARCH_RP2040)
#error "RP2040/RP2350 only."
#endif

// Flicker fix experiment: TRIPLE-buffer so the RX core never stalls in the buffer handoff -> constant
// bus load -> constant panel refresh -> no scan-home (row 0) flicker under streaming. 14-bit x2 = 448KB
// fits the RP2350's 520KB; triple would not. NPLANES = the greyscale bit depth per channel and MUST match
// gclk_bits (see CFG_DEFAULT) and the host's ColorBits. 14 = the ICND1065L's maximum.
#define HUB_NPLANES 14
#define HUB_DOUBLE_BUFFER 1
#define HUB_NUM_BUFFERS 2
#define DMD_PIO_MUX 1
#include "DMD_SPWM_Driver_RP.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "w6qspi.pio.h"
#include <EEPROM.h>   // flash-backed config persistence (save/load/reset)
#include <LittleFS.h> // OTA firmware staging - REQUIRES an FS partition: build with fqbn ...:flash=4194304_1048576
#include <Updater.h>  // the 'Update' object: receive a .bin over the web server and self-flash (OTA)
#include "pico/unique_id.h"

#define RGB128x128plainS64 33, 128, 128, 64, 0
uint8_t custom_rgbpins[] = { 12, 0, 1, 2, 3, 4, 5 };
#define DMD_PIN_nOE 14
#define DMD_PIN_SCLK 13
uint8_t mux_list[] = { 26, 27, 28 };

#define REG03 0x3F   // NovaLCT default (was 0x40); part of the ghost-reduction on scan row 0
static uint16_t regRed[] = {
    0x00AA, 0x01AA, 0x027F, (0x0300 | REG03), 0x0410, 0x0500, 0x0601, 0x0720,
    0x0C18, 0x0D01, 0x0E88, 0x0F01, 0x1040, 0x1127, 0x1800, 0x1906,
    0x1C60, 0x1DEA, 0x1E71, 0x2040, 0x2100, 0x2340, 0x74A0
};
static uint16_t regGreen[] = {
    0x00AA, 0x01AA, 0x027F, (0x0300 | REG03), 0x0410, 0x0500, 0x0601, 0x0720,
    0x0C1E, 0x0D01, 0x0E88, 0x0F01, 0x1040, 0x1127, 0x1800, 0x1908,
    0x1C60, 0x1DEA, 0x1E75, 0x2060, 0x2100, 0x2340, 0x74A0
};
static uint16_t regBlue[] = {
    0x00AA, 0x01AA, 0x027F, (0x0300 | REG03), 0x0410, 0x0500, 0x0601, 0x0720,
    0x0C1E, 0x0D01, 0x0E88, 0x0F01, 0x1040, 0x1127, 0x1800, 0x190A,
    0x1C60, 0x1DEA, 0x1EB5, 0x2060, 0x2100, 0x2340, 0x74A0
};
#define REG_CNT (sizeof(regGreen) / sizeof(regGreen[0]))
#define CFG_BGR 1
#if CFG_BGR
  #define SLOT0 regBlue
  #define SLOT2 regRed
#else
  #define SLOT0 regRed
  #define SLOT2 regBlue
#endif

DMD_RGB_ICND1065L_HUB320<RGB128x128plainS64, COLOR_4BITS>
    dmd(mux_list, DMD_PIN_nOE, DMD_PIN_SCLK, custom_rgbpins, 1, 1, false);

// ---------------- W6300 QUAD QSPI-PIO ----------------
#define W6_CS   16
#define W6_SCLK 17
#define W6_IO0  18
#define W6_IO1  19
#define W6_IO2  20
#define W6_IO3  21
#define W6_RST  22
#define IO_MASK ((1u << W6_IO0) | (1u << W6_IO1) | (1u << W6_IO2) | (1u << W6_IO3))
#define BSB_COMMON 0x00
#define BSB_S0_REG 0x01
#define BSB_S0_TX  0x02
#define BSB_S0_RX  0x03
// Socket 1 = config web server (TCP). W6300 block-select for socket n: reg=(n<<2)|1, tx=|2, rx=|3.
#define BSB_S1_REG 0x05
#define BSB_S1_TX  0x06
#define BSB_S1_RX  0x07
#define UDP_PORT 7777
#define WEB_PORT 5000   // config web server (unobtrusive, non-default port)
#define DISC_PORT 7778  // UDP probe / announce (not the frame socket)
#define CHUNK 1440
// W6300 socket RX/TX buffer sizes MUST be powers of two (1/2/4/8/16 KB); the sum per direction must fit
// the pool. Socket 0 (stream) = 16 KB, socket 1 (web) = 2 KB. 16 KB still buffers ~6.7 ms at 19 Mbit/s,
// well above the sub-millisecond core1 stalls, so the stream is unaffected.
#define RXBUF_KB 16
#define WEB_RXBUF_KB 2
#define WEB_TXBUF_KB 4
#define W6_CLKDIV 2.0f   // 33 MHz SCLK * 4 bits = ~132 Mbit/s. Tune up (3/4) only if unstable.
#define QSPI_MODE_QUAD 0x80  // MOD[1:0]=10 in opcode bits [7:6]

#define FW_VERSION "1.3"

static uint8_t W6_MAC[6];
static uint8_t g_uid[8];
static char    g_hostLabel[16];   // "panel-aabbcc" from MAC, used for mDNS + DHCP hostname
static char    g_panelName[16];   // optional friendly name (flash)

volatile bool g_dmdReady = false;
volatile bool g_netReady = false;      // core1 -> core0: net init done, core0 now owns Serial
volatile uint32_t g_good = 0, g_drop = 0;
static volatile int g_testMode = 0;    // 0=stream, 1=vline, 2=hline, 3=fill, 4=grid  (core0)
static int g_testArg = 0;
static volatile bool g_testDirty = false;

// Resolved network config (set by core1 after DHCP/static), read by core0 for the boot intro.
static volatile uint8_t g_ip[4]   = { 0, 0, 0, 0 };
static volatile uint8_t g_mask[4] = { 0, 0, 0, 0 };
static volatile uint8_t g_gw[4]   = { 0, 0, 0, 0 };
static volatile bool    g_dhcpOk  = false;   // true if the address came from DHCP

// Boot progress (core1 sets, core0 paints the font splash).
enum { BOOT_INIT, BOOT_ETH, BOOT_LINK, BOOT_DHCP, BOOT_DHCP_FAIL, BOOT_STATIC, BOOT_READY };
static volatile uint8_t g_bootPhase = BOOT_INIT;

// Web server (core1) -> command executor (core0) bridge. Single-slot, lock-free: core1 fills g_webCmd
// and raises g_webCmdPending; core0 runs it through processCmd() (the same path as the serial menu) and
// clears the flag. A memory barrier orders the payload write before the flag so core0 sees a complete cmd.
static volatile char     g_webCmd[96] = { 0 };
static volatile bool     g_webCmdPending = false;

// ---------------- persistent config (flash-backed EEPROM emulation) ----------------
// Live-tunable values that survive a reboot. Code defaults (CFG_DEFAULT) are the recovery baseline:
// 'reset' restores them, 'save' persists, 'load' re-reads flash. Boundary/seam correction runs
// HOST-SIDE (RgbPanel DLL). Network: mode 0 = DHCP (default), 1 = static ip/mask/gw below.
#define CFG_MAGIC 0x57365137u   // 'W6Q7' bump when the layout changes (also invalidates stale layouts)
// colorMode: 0 = 14-bit / DOUBLE-buffer (max colour depth; RX core stalls per frame -> more UDP drops
//            at high fps), 1 = 8-bit / TRIPLE-buffer (RX core never stalls -> minimal drops + max fps,
//            256 shades). BRIGHTNESS is identical in both modes: gclk_bits stays 14 (full GCLK train);
//            only the stored plane count (buffer RAM) changes. Applied at boot (save + reboot); the host
//            must send matching ColorBits (14 or 8).
#define COLORMODE_PLANES(m) ((m) ? 8 : 14)
#define COLORMODE_NBUF(m)   ((m) ? 3 : 2)
struct PersistCfg {
    uint32_t magic;
    float    dclkMHz;      // panel data clock
    uint8_t  gclkBits;     // greyscale bit width
    uint8_t  netMode;      // 0 = DHCP, 1 = static
    uint8_t  ip[4];        // static IP / DHCP fallback
    uint8_t  mask[4];
    uint8_t  gw[4];
    uint8_t  colorMode;    // 0 = 14-bit/double, 1 = 8-bit/triple (see above)
    char     name[16];     // optional friendly name; empty -> g_hostLabel
};
static const PersistCfg CFG_DEFAULT = {
    CFG_MAGIC, 15.6f, HUB_NPLANES, 0, { 192, 168, 10, 181 }, { 255, 255, 255, 0 }, { 192, 168, 10, 1 }, 0, { 0 }
};
static float   g_dclkMHz  = 15.6f;   // mirror of the live DCLK (setDataClkMHz doesn't expose a getter)
static uint8_t g_gclkBits = HUB_NPLANES;   // greyscale bits = data planes (keep in lock-step)
static uint8_t g_netMode  = 0;                          // configured mode (DHCP/static)
static uint8_t g_colorMode = 0;                         // 0 = 14-bit/double, 1 = 8-bit/triple (boot-applied)
static volatile bool g_otaActive = false;               // true while core1 is flashing an OTA upload -> core0 holds
static uint8_t g_cfgIp[4]   = { 192, 168, 10, 181 };    // configured static IP / DHCP fallback
static uint8_t g_cfgMask[4] = { 255, 255, 255, 0 };
static uint8_t g_cfgGw[4]   = { 192, 168, 10, 1 };

#include "W6300.h"

static void applyRegs() { dmd.applyPanelRegsRGB(SLOT0, regGreen, SLOT2, REG_CNT); }

static void sanitize_name(char *n, int cap)
{
    int i = 0;
    for (; i < cap - 1 && n[i]; i++) {
        char c = n[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ')) {
            n[0] = 0; return;
        }
    }
    n[i] = 0;
}

static const char *panel_display_name()
{
    return g_panelName[0] ? g_panelName : g_hostLabel;
}

static void init_identity()
{
    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);
    memcpy(g_uid, uid.id, 8);
    // Locally-administered unicast MAC derived from the RP2350 unique ID so two
    // boards never share DE:AD:BE:EF:00:01 (DHCP/ARP would collapse).
    W6_MAC[0] = (uint8_t)((uid.id[0] & 0xFE) | 0x02);
    W6_MAC[1] = uid.id[1];
    W6_MAC[2] = uid.id[2];
    W6_MAC[3] = uid.id[3];
    W6_MAC[4] = uid.id[4];
    W6_MAC[5] = uid.id[5];
    snprintf(g_hostLabel, sizeof(g_hostLabel), "panel-%02x%02x%02x",
             W6_MAC[3], W6_MAC[4], W6_MAC[5]);
    g_panelName[0] = 0;
}

// Apply a config struct to the live panel state (DCLK, greyscale width, boundary correction).
static void applyCfg(const PersistCfg &c)
{
    g_gclkBits = c.gclkBits;
    dmd.setGclkBits(c.gclkBits);
    g_dclkMHz = dmd.setDataClkMHz(c.dclkMHz);
    g_netMode = c.netMode;
    g_colorMode = c.colorMode;   // buffer/depth mode is applied at BOOT (see setup) -> save + reboot
    memcpy(g_cfgIp, c.ip, 4); memcpy(g_cfgMask, c.mask, 4); memcpy(g_cfgGw, c.gw, 4);
    memcpy(g_panelName, c.name, sizeof(g_panelName));
    g_panelName[sizeof(g_panelName) - 1] = 0;
    sanitize_name(g_panelName, sizeof(g_panelName));
}
static void gatherCfg(PersistCfg &c)
{
    c.magic = CFG_MAGIC; c.dclkMHz = g_dclkMHz; c.gclkBits = g_gclkBits;
    c.netMode = g_netMode; c.colorMode = g_colorMode;
    memcpy(c.ip, g_cfgIp, 4); memcpy(c.mask, g_cfgMask, 4); memcpy(c.gw, g_cfgGw, 4);
    memset(c.name, 0, sizeof(c.name));
    strncpy(c.name, g_panelName, sizeof(c.name) - 1);
}
static void saveCfg()  { PersistCfg c; gatherCfg(c); EEPROM.put(0, c); EEPROM.commit(); }
static void loadCfg()  { PersistCfg c; EEPROM.get(0, c); applyCfg(c.magic == CFG_MAGIC ? c : CFG_DEFAULT); }
static void resetCfg() { applyCfg(CFG_DEFAULT); }

void setup()
{
    Serial.begin(115200);
    init_identity();
    EEPROM.begin(256);
    LittleFS.begin();   // OTA staging area; harmless if built without an FS partition (OTA just unavailable)
    dmd.cfg_landscape = true; dmd.cfg_rot_cw = true; dmd.cfg_flip_x = true; dmd.cfg_flip_y = true;
    dmd.setMaxClkFreq(16);
    dmd.init();
    dmd.applyPanelRegsRGB(SLOT0, regGreen, SLOT2, REG_CNT);
    loadCfg();   // persisted values, or CFG_DEFAULT (14-bit/double, DCLK 15.6)
    // Apply the persisted colour/buffer mode ONCE, here at boot (never live): 14-bit/double for max
    // colour, or 8-bit/triple for a non-stalling RX handoff (minimal UDP drops, max fps). Reallocates
    // the frame buffers to the matching depth+count before the panel scans or the RX core streams.
    {
        uint8_t planes = COLORMODE_PLANES(g_colorMode), nbuf = COLORMODE_NBUF(g_colorMode);
        dmd.reconfigure(planes, nbuf);   // sets nPlanes (buffer depth) + buffer count only
        dmd.setGclkBits(14); g_gclkBits = 14;   // brightness: full 14-bit GCLK train in BOTH modes
    }
    dmd.clear16();
    dmd.swapBuffers(true);
    g_dmdReady = true;
}

// Firmware test patterns (no network needed) to characterise the bright-column artifact precisely.
// Redrawn every frame in test mode because acquireFront() may hand core0 a different buffer each frame.
static void drawTestPattern()
{
    dmd.clear16();
    const int W = dmd.width(), H = dmd.height();
    const uint16_t ON = 8191;   // full 13-bit white
    switch (g_testMode) {
    case 1: if (g_testArg >= 0 && g_testArg < W) for (int y = 0; y < H; y++) dmd.drawPixelLevel(g_testArg, y, ON); break; // vertical line at column
    case 2: if (g_testArg >= 0 && g_testArg < H) for (int x = 0; x < W; x++) dmd.drawPixelLevel(x, g_testArg, ON); break; // horizontal line at row
    case 3: { uint16_t lv = (uint16_t)g_testArg; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) dmd.drawPixelLevel(x, y, lv); } break; // solid fill (13-bit level 0..8191)
    case 4: for (int x = 0; x < W; x++) if ((x % 64) == 63) for (int y = 0; y < H; y++) dmd.drawPixelLevel(x, y, ON); break; // lines exactly at 63/127/191/255
    }
}

static bool parseIp(const char *s, uint8_t *out)
{
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) == 4 &&
        (unsigned)a < 256 && (unsigned)b < 256 && (unsigned)c < 256 && (unsigned)d < 256) {
        out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c; out[3] = (uint8_t)d; return true;
    }
    return false;
}

static void printNet()
{
    Serial.printf("[net] mode=%s  active: IP=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u (%s)\n",
        g_netMode == 0 ? "DHCP" : "STATIC",
        g_ip[0], g_ip[1], g_ip[2], g_ip[3], g_mask[0], g_mask[1], g_mask[2], g_mask[3],
        g_gw[0], g_gw[1], g_gw[2], g_gw[3], g_dhcpOk ? "leased by DHCP" : "static/fallback");
    Serial.printf("[net] configured static: %u.%u.%u.%u / %u.%u.%u.%u / %u.%u.%u.%u\n",
        g_cfgIp[0], g_cfgIp[1], g_cfgIp[2], g_cfgIp[3], g_cfgMask[0], g_cfgMask[1], g_cfgMask[2],
        g_cfgMask[3], g_cfgGw[0], g_cfgGw[1], g_cfgGw[2], g_cfgGw[3]);
}

// `net` (show) | `net dhcp` | `net static <ip> <mask> <gw>`. Changes take effect after save + reboot.
static void handleNetCmd(char *a)
{
    while (*a == ' ') a++;
    if (*a == 0) { printNet(); return; }
    if (!strncmp(a, "dhcp", 4)) { g_netMode = 0; Serial.println(F("[net] mode=DHCP (run 'save' then reboot)")); return; }
    if (!strncmp(a, "static", 6)) {
        char ip[20], mk[20], gw[20];
        if (sscanf(a + 6, "%19s %19s %19s", ip, mk, gw) == 3 &&
            parseIp(ip, g_cfgIp) && parseIp(mk, g_cfgMask) && parseIp(gw, g_cfgGw)) {
            g_netMode = 1;
            Serial.println(F("[net] mode=STATIC set (run 'save' then reboot)"));
            printNet();
        } else Serial.println(F("usage: net static <ip> <mask> <gw>"));
        return;
    }
    Serial.println(F("usage: net | net dhcp | net static <ip> <mask> <gw>"));
}

#include "Menu.h"
#include "Intro.h"
void loop()
{
    static uint32_t introStart = 0;

    if (g_otaActive) { delay(5); return; }   // firmware update in progress on core1 -> hold the panel

    if (g_testMode) {
        // Test mode: keep ADOPTING core1's frame buffer (acquireFront) so a racy submitFill on core1 can
        // never block indefinitely (core1 would otherwise stall, killing the web server). The static test
        // pattern is redrawn each frame over the adopted buffer - cheap, deterministic, no flicker.
        dmd.acquireFront();
        drawTestPattern();
        g_testDirty = false;
        dmd.swapBuffers(true);
        handleSerial();   // keep the serial menu alive so 't n' (and everything else) still works
        handleWebCmd();
        return;
    }

    if (!g_netReady) {
        // Network still coming up: font boot splash with live step text. swapBuffers every loop so
        // both ICND fields keep pumping; introPump throttles the (expensive) glyph redraw.
        introPump(true, 0);
        return;
    }

    if (introStart == 0) introStart = millis();
    uint32_t el = millis() - introStart;

    if (el < INTRO_TOTAL_MS && g_good == 0) {
        introPump(false, el);
    } else {
        dmd.acquireFront();
        dmd.swapBuffers(true);
    }

    handleSerial();                      // core0 owns Serial once g_netReady
    handleWebCmd();                      // execute any command queued by the web server (core1)
}

static uint8_t frag[1500];

// ---------------- DHCP client (runs on socket 0 before it becomes the streaming socket) ----------------
#include "Dhcp.h"
#include "WebServer.h"
#include "Discover.h"
void setup1()
{
    while (!g_dmdReady) delay(1);
    g_bootPhase = BOOT_ETH;
    w6_reset();
    w6_pio_init();

    uint32_t tr = millis(); uint8_t cid = 0;
    while (millis() - tr < 800) { cid = w6_rd8(BSB_COMMON, 0x0000); if (cid == 0x61) break; delay(2); }
    Serial.printf("[pioq] CIDR0=0x%02X %s\n", cid, cid == 0x61 ? "(ready)" : "(NO COMMS - raise W6_CLKDIV?)");

    w6_write(BSB_COMMON, 0x4120, W6_MAC, 6);            // MAC (SHAR)
    for (uint8_t s = 0; s < 8; s++) {                  // 0=stream, 1=web, 2=discovery, 3=mDNS
        uint8_t bsb = (uint8_t)((s << 2) | 1);
        uint8_t rx = 0, tx = 0;
        if (s == 0) { rx = RXBUF_KB; tx = 2; }
        else if (s == 1) { rx = WEB_RXBUF_KB; tx = WEB_TXBUF_KB; }
        else if (s == 2) { rx = 1; tx = 1; }           // UDP 7778 probe/announce
        else if (s == 3) { rx = 1; tx = 1; }           // mDNS 5353
        w6_wr8(bsb, 0x0220, rx);                       // Sn_RX_BSR
        w6_wr8(bsb, 0x0200, tx);                       // Sn_TX_BSR
    }

    // Resolve the network address: DHCP (default) with static fallback, or configured static.
    uint8_t ip[4], mask[4], gw[4];
    bool viaDhcp = false;
    if (g_netMode == 0) {
        g_bootPhase = BOOT_LINK;
        delay(2500);  // let the PHY link come up before DHCP (early DISCOVERs are lost pre-link -> no OFFER)
        Serial.println(F("[net] DHCP: requesting lease..."));
        g_bootPhase = BOOT_DHCP;
        viaDhcp = dhcp_run(ip, mask, gw);
        if (!viaDhcp) { g_bootPhase = BOOT_DHCP_FAIL; Serial.println(F("[net] DHCP failed -> using configured/fallback static IP")); }
    } else {
        g_bootPhase = BOOT_STATIC;
    }
    if (!viaDhcp) { memcpy(ip, g_cfgIp, 4); memcpy(mask, g_cfgMask, 4); memcpy(gw, g_cfgGw, 4); }

    w6_write(BSB_COMMON, 0x4130, gw, 4);               // GAR
    w6_write(BSB_COMMON, 0x4134, mask, 4);             // SUBR
    w6_write(BSB_COMMON, 0x4138, ip, 4);               // SIPR
    memcpy((void *)g_ip, ip, 4); memcpy((void *)g_mask, mask, 4); memcpy((void *)g_gw, gw, 4);
    g_dhcpOk = viaDhcp;

    // Open the streaming socket (socket 0, UDP 7777).
    w6_wr8(BSB_S0_REG, 0x0000, 0x02);
    w6_wr16(BSB_S0_REG, 0x0114, UDP_PORT);
    w6_wr8(BSB_S0_REG, 0x0010, 0x01);
    uint32_t t0 = millis();
    while (w6_rd8(BSB_S0_REG, 0x0010) && millis() - t0 < 200) {}
    Serial.printf("[net] %s  IP=%u.%u.%u.%u  mask=%u.%u.%u.%u  gw=%u.%u.%u.%u  Sn_SR=0x%02X\n",
                  viaDhcp ? "DHCP" : "STATIC", ip[0], ip[1], ip[2], ip[3],
                  mask[0], mask[1], mask[2], mask[3], gw[0], gw[1], gw[2], gw[3],
                  w6_rd8(BSB_S0_REG, 0x0030));

    Serial.printf("[net] config web UI at http://%u.%u.%u.%u:%d/\n", ip[0], ip[1], ip[2], ip[3], WEB_PORT);
    Serial.printf("[net] MAC=%02X:%02X:%02X:%02X:%02X:%02X  id=%02x%02x%02x%02x%02x%02x%02x%02x  host=%s.local\n",
                  W6_MAC[0], W6_MAC[1], W6_MAC[2], W6_MAC[3], W6_MAC[4], W6_MAC[5],
                  g_uid[0], g_uid[1], g_uid[2], g_uid[3], g_uid[4], g_uid[5], g_uid[6], g_uid[7],
                  g_hostLabel);

    disc_init();

    // Hand Serial ownership to core0: from here core1 is a pure W6300 drain (no Serial -> no USB-CDC
    // blocking stalls that would drop fragments). All prints/tuning happen on core0.
    g_bootPhase = BOOT_READY;
    g_netReady = true;
}

void loop1()
{
    static uint16_t curId = 0xFFFF, curN = 0, haveCnt = 0;
    static uint8_t fragBits[32];

    uint16_t rsr = w6_rd16(BSB_S0_REG, 0x0224);
    uint16_t rd = w6_rd16(BSB_S0_REG, 0x0228);
    const uint16_t want = (uint16_t)(8 + CHUNK + 3);   // packet-info(8) + max payload

    while (rsr >= 8) {
        // One combined read: packet-info + payload (+ possibly start of next datagram, discarded).
        uint16_t rlen = (rsr < want) ? rsr : want;
        w6_read_rx(rd, frag, rlen);
        uint16_t dsize = (uint16_t)(((frag[0] & 0x07) << 8) | frag[1]);
        if (dsize < 3 || dsize > (CHUNK + 3)) {   // corrupt header -> flush socket, resync
            rd = (uint16_t)(rd + rsr);
            w6_wr16(BSB_S0_REG, 0x0228, rd); w6_wr8(BSB_S0_REG, 0x0010, 0x40);
            g_drop++; break;
        }
        if ((uint32_t)8 + dsize > rsr) break;   // datagram not fully arrived yet

        rd = (uint16_t)(rd + 8 + dsize);
        w6_wr16(BSB_S0_REG, 0x0228, rd);
        w6_wr8(BSB_S0_REG, 0x0010, 0x40);
        rsr = (uint16_t)(rsr - (8 + dsize));

        // In test mode core0 stops consuming frames (no acquireFront), so submitFill() would block on the
        // double-buffer handoff and stall this core (killing web_poll). Keep draining, but don't assemble
        // or submit while testing.
        if (g_testMode) continue;

        const uint8_t *pl = frag + 8;   // payload: [frameId, fragIdx, nFrags, data...]
        uint8_t frameId = pl[0], fragIdx = pl[1], nFrags = pl[2];
        uint32_t chunkLen = (uint32_t)(dsize - 3);
        if (frameId != curId) {
            if (curN && haveCnt != curN) g_drop++;
            curId = frameId; curN = nFrags; haveCnt = 0; memset(fragBits, 0, sizeof(fragBits));
        }
        uint32_t off = (uint32_t)fragIdx * CHUNK;
        if (off + chunkLen <= dmd.buf16Bytes()) {
            memcpy(dmd.rawBuf16() + off, pl + 3, chunkLen);
            uint8_t m = 1u << (fragIdx & 7);
            if (!(fragBits[fragIdx >> 3] & m)) { fragBits[fragIdx >> 3] |= m; haveCnt++; }
        }
        if (curN && haveCnt == curN) { g_good++; dmd.submitFill(); curId = 0xFFFF; }
    }

    // Stream is drained first; service the config web server after (brief, user-initiated only).
    web_poll();
    disc_poll();
}
