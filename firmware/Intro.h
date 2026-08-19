// Intro.h — SRAM 5x7 boot splash (not Adafruit/GFX fonts).
//
// GFX fonts in the same translation unit as data_transfer() used to shift XIP layout
// and jitter the field upload. This intro:
//   1. Renders glyphs via drawPixelLevelRGB (full bit-depth).
//   2. Keeps the 5x7 font in SRAM so glyph fetches don't thrash the XIP cache.
//   3. Uses clearFront() (not clear16) so core1's fill buffer is left alone.
//   4. Redraws the splash at ~25 fps but calls swapBuffers every loop so both
//      ICND fields stay pumped. data_transfer() is also RAM-resident in the driver.
#pragma once

// 5x7, column-major, bit0 = top. ASCII 32..90 (space through 'Z'). Lives in SRAM.
static const uint8_t FONT5x7[][5] __attribute__((section(".data.font5x7"))) = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 space
    {0x00,0x00,0x5F,0x00,0x00}, // 33 !
    {0x00,0x07,0x00,0x07,0x00}, // 34 "
    {0x14,0x7F,0x14,0x7F,0x14}, // 35 #
    {0x24,0x2A,0x7F,0x2A,0x12}, // 36 $
    {0x23,0x13,0x08,0x64,0x62}, // 37 %
    {0x36,0x49,0x55,0x22,0x50}, // 38 &
    {0x00,0x05,0x03,0x00,0x00}, // 39 '
    {0x00,0x1C,0x22,0x41,0x00}, // 40 (
    {0x00,0x41,0x22,0x1C,0x00}, // 41 )
    {0x14,0x08,0x3E,0x08,0x14}, // 42 *
    {0x08,0x08,0x3E,0x08,0x08}, // 43 +
    {0x00,0x00,0x50,0x30,0x00}, // 44 ,
    {0x08,0x08,0x08,0x08,0x08}, // 45 -
    {0x00,0x60,0x60,0x00,0x00}, // 46 .
    {0x20,0x10,0x08,0x04,0x02}, // 47 /
    {0x3E,0x51,0x49,0x45,0x3E}, // 48 0
    {0x00,0x42,0x7F,0x40,0x00}, // 49 1
    {0x42,0x61,0x51,0x49,0x46}, // 50 2
    {0x21,0x41,0x45,0x4B,0x31}, // 51 3
    {0x18,0x14,0x12,0x7F,0x10}, // 52 4
    {0x27,0x45,0x45,0x45,0x39}, // 53 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 54 6
    {0x01,0x71,0x09,0x05,0x03}, // 55 7
    {0x36,0x49,0x49,0x49,0x36}, // 56 8
    {0x06,0x49,0x49,0x29,0x1E}, // 57 9
    {0x00,0x36,0x36,0x00,0x00}, // 58 :
    {0x00,0x56,0x36,0x00,0x00}, // 59 ;
    {0x08,0x14,0x22,0x41,0x00}, // 60 <
    {0x14,0x14,0x14,0x14,0x14}, // 61 =
    {0x00,0x41,0x22,0x14,0x08}, // 62 >
    {0x02,0x01,0x51,0x09,0x06}, // 63 ?
    {0x32,0x49,0x79,0x41,0x3E}, // 64 @
    {0x7E,0x11,0x11,0x11,0x7E}, // 65 A
    {0x7F,0x49,0x49,0x49,0x36}, // 66 B
    {0x3E,0x41,0x41,0x41,0x22}, // 67 C
    {0x7F,0x41,0x41,0x22,0x1C}, // 68 D
    {0x7F,0x49,0x49,0x49,0x41}, // 69 E
    {0x7F,0x09,0x09,0x09,0x01}, // 70 F
    {0x3E,0x41,0x49,0x49,0x7A}, // 71 G
    {0x7F,0x08,0x08,0x08,0x7F}, // 72 H
    {0x00,0x41,0x7F,0x41,0x00}, // 73 I
    {0x20,0x40,0x41,0x3F,0x01}, // 74 J
    {0x7F,0x08,0x14,0x22,0x41}, // 75 K
    {0x7F,0x40,0x40,0x40,0x40}, // 76 L
    {0x7F,0x02,0x0C,0x02,0x7F}, // 77 M
    {0x7F,0x04,0x08,0x10,0x7F}, // 78 N
    {0x3E,0x41,0x41,0x41,0x3E}, // 79 O
    {0x7F,0x09,0x09,0x09,0x06}, // 80 P
    {0x3E,0x41,0x51,0x21,0x5E}, // 81 Q
    {0x7F,0x09,0x19,0x29,0x46}, // 82 R
    {0x46,0x49,0x49,0x49,0x31}, // 83 S
    {0x01,0x01,0x7F,0x01,0x01}, // 84 T
    {0x3F,0x40,0x40,0x40,0x3F}, // 85 U
    {0x1F,0x20,0x40,0x20,0x1F}, // 86 V
    {0x3F,0x40,0x38,0x40,0x3F}, // 87 W
    {0x63,0x14,0x08,0x14,0x63}, // 88 X
    {0x07,0x08,0x70,0x08,0x07}, // 89 Y
    {0x61,0x51,0x49,0x45,0x43}, // 90 Z
};

static uint16_t introFull() { return (uint16_t)((1u << g_gclkBits) - 1u); }

static void hue6(int h, uint16_t &r, uint16_t &g, uint16_t &b)
{
    const uint16_t F = introFull(), O = 0;
    switch (((h % 6) + 6) % 6) {
    case 0:  r = F; g = O; b = O; break;
    case 1:  r = F; g = F; b = O; break;
    case 2:  r = O; g = F; b = O; break;
    case 3:  r = O; g = F; b = F; break;
    case 4:  r = O; g = O; b = F; break;
    default: r = F; g = O; b = F; break;
    }
}

static void hsv(uint8_t h, uint16_t v, uint16_t &r, uint16_t &g, uint16_t &b)
{
    uint8_t region = h / 43;
    uint16_t f = (uint16_t)((h - region * 43) * 6);
    uint16_t p = 0;
    uint16_t q = (uint16_t)((v * (255 - f)) >> 8);
    uint16_t t = (uint16_t)((v * f) >> 8);
    switch (region) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

static char upch(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static int fontW(int scale) { return 6 * scale; } // 5 px glyph + 1 px gap
static int fontH(int scale) { return 7 * scale; }

static void drawChar5x7(int x, int y, char ch, int scale, uint16_t r, uint16_t g, uint16_t b)
{
    ch = upch(ch);
    if (ch < 32 || ch > 90) return;
    const uint8_t *col = FONT5x7[ch - 32];
    for (int cx = 0; cx < 5; cx++) {
        uint8_t bits = col[cx];
        for (int cy = 0; cy < 7; cy++) {
            if (bits & (1u << cy)) {
                int px = x + cx * scale, py = y + cy * scale;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        dmd.drawPixelLevelRGB(px + dx, py + dy, r, g, b);
            }
        }
    }
}

static int strW(const char *s, int scale)
{
    int n = 0; while (s[n]) n++;
    return n ? n * fontW(scale) - scale : 0;
}

static void drawStr(int x, int y, const char *s, int scale, uint16_t r, uint16_t g, uint16_t b)
{
    for (const char *p = s; *p; ++p) {
        drawChar5x7(x, y, *p, scale, r, g, b);
        x += fontW(scale);
    }
}

static void drawStrCentered(int y, const char *s, int scale, uint16_t r, uint16_t g, uint16_t b)
{
    int w = strW(s, scale);
    int x = (dmd.width() - w) / 2;
    if (x < 0) x = 0;
    drawStr(x, y, s, scale, r, g, b);
}

static void drawStrRainbow(int y, const char *s, int scale, int hueOff)
{
    int w = strW(s, scale);
    int x = (dmd.width() - w) / 2;
    if (x < 0) x = 0;
    int i = 0;
    const uint16_t F = introFull();
    for (const char *p = s; *p; ++p, ++i) {
        uint16_t r, g, b;
        hsv((uint8_t)(hueOff + i * 18), F, r, g, b);
        drawChar5x7(x, y, *p, scale, r, g, b);
        x += fontW(scale);
    }
}

static void drawBorder(int hueT)
{
    const int W = dmd.width(), H = dmd.height();
    const uint16_t F = introFull();
    for (int x = 0; x < W; x++) {
        uint16_t r, g, b;
        hsv((uint8_t)(x + hueT), F, r, g, b);
        dmd.drawPixelLevelRGB(x, 0, r, g, b);
        dmd.drawPixelLevelRGB(x, H - 1, r, g, b);
    }
    for (int y = 1; y < H - 1; y++) {
        uint16_t r, g, b;
        hsv((uint8_t)(y + hueT), F, r, g, b);
        dmd.drawPixelLevelRGB(0, y, r, g, b);
        dmd.drawPixelLevelRGB(W - 1, y, r, g, b);
    }
}

static void drawBar(int y, int fillOrPos, int span, bool determinate, int hueT)
{
    const int W = dmd.width();
    const int bx = 10, bw = W - 20, bh = 4;
    const uint16_t F = introFull(), dim = F / 18;
    for (int i = 0; i < bw; i++)
        for (int j = 0; j < bh; j++)
            dmd.drawPixelLevelRGB(bx + i, y + j, dim, dim, dim);
    if (determinate) {
        int fill = fillOrPos; if (fill < 0) fill = 0; if (fill > bw) fill = bw;
        for (int i = 0; i < fill; i++) {
            uint16_t r, g, b; hsv((uint8_t)(i * 2 - hueT), F, r, g, b);
            for (int j = 0; j < bh; j++) dmd.drawPixelLevelRGB(bx + i, y + j, r, g, b);
        }
    } else {
        int travel = bw - span; if (travel < 1) travel = 1;
        int p = fillOrPos % (2 * travel);
        if (p > travel) p = 2 * travel - p;
        for (int i = 0; i < span; i++) {
            uint16_t r, g, b; hsv((uint8_t)(i * 4 - hueT), F, r, g, b);
            for (int j = 0; j < bh; j++) dmd.drawPixelLevelRGB(bx + p + i, y + j, r, g, b);
        }
    }
}

static const char *bootMsg(uint8_t phase)
{
    switch (phase) {
    case BOOT_ETH:       return "INIT ETHERNET";
    case BOOT_LINK:      return "WAITING FOR LINK";
    case BOOT_DHCP:      return "ACQUIRING IP VIA DHCP";
    case BOOT_DHCP_FAIL: return "DHCP FAILED - FALLBACK";
    case BOOT_STATIC:    return "STATIC IP CONFIG";
    case BOOT_READY:     return "NETWORK READY";
    default:             return "STARTING UP";
    }
}

static void drawBootScreen()
{
    const uint16_t F = introFull();
    const uint32_t now = millis();
    const int hueT = (int)(now / 12);
    const int H = dmd.height();

    drawBorder(hueT);
    drawStrRainbow(8, "RGB PANEL", 2, hueT);

    const uint8_t phase = g_bootPhase;
    const bool busy = (phase == BOOT_INIT || phase == BOOT_ETH || phase == BOOT_LINK || phase == BOOT_DHCP);
    char line[40];
    if (busy) snprintf(line, sizeof(line), "%s%.*s", bootMsg(phase), (int)((now / 400) % 4), "...");
    else      snprintf(line, sizeof(line), "%s", bootMsg(phase));

    if (phase == BOOT_DHCP_FAIL) drawStrCentered(H / 2 - 6, line, 1, F, F / 6, F / 6);
    else                         drawStrCentered(H / 2 - 6, line, 1, F, F, F);

    char host[24];
    snprintf(host, sizeof(host), "%s.local", g_hostLabel);
    drawStrCentered(H / 2 + 10, host, 1, F / 2, F / 2, F / 2);

    drawBar(H - 18, (int)(now / 6), 48, false, hueT);

    char fw[24]; snprintf(fw, sizeof(fw), "FW %s", FW_VERSION);
    drawStrCentered(H - 12, fw, 1, F / 2, F / 2, F / 2);
}

#define INTRO_HOLD_MS  5000u
#define INTRO_FADE_MS   800u
#define INTRO_TOTAL_MS (INTRO_HOLD_MS + INTRO_FADE_MS)

static void drawIntro(uint32_t el)
{
    const uint16_t F = introFull();
    const int hueT = (int)(el / 12);
    const int H = dmd.height();

    drawBorder(hueT);
    drawStrRainbow(8, "RGB PANEL", 2, hueT);

    char ip[24];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", g_ip[0], g_ip[1], g_ip[2], g_ip[3]);
    int ipScale = (strW(ip, 3) <= dmd.width() - 16) ? 3 : 2;
    drawStrRainbow(H / 2 - fontH(ipScale) / 2, ip, ipScale, -2 * hueT);

    char st[40];
    snprintf(st, sizeof(st), "%s  FW %s", g_dhcpOk ? "DHCP" : "STATIC", FW_VERSION);
    drawStrCentered(H / 2 + fontH(ipScale) / 2 + 6, st, 1, F, F, F);

    int fill = (int)((dmd.width() - 20) * (el > INTRO_TOTAL_MS ? INTRO_TOTAL_MS : el) / INTRO_TOTAL_MS);
    drawBar(H - 18, fill, 0, true, hueT);
}

static bool introPump(bool bootPhase, uint32_t introElapsed)
{
    static uint32_t lastDraw = 0;
    uint32_t now = millis();
    if (now - lastDraw >= 40) {
        lastDraw = now;
        dmd.clearFront();
        if (bootPhase) drawBootScreen();
        else           drawIntro(introElapsed);
    }
    dmd.swapBuffers(true);
    return true;
}
