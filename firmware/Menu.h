// Menu.h
// Part of the dmd_icnd1065l_w6300_pioq sketch. It is #included (in dependency order) from the
// main .ino, so this is still ONE translation unit - no forward declarations needed.
// Command parser (serial + web bridge), help text, and the core0 executors.
#pragma once

static void printHelp()
{
    Serial.println(F("\n=== W6300 LED panel - serial console ==="));
    Serial.println(F("Type a command + Enter. Runs on the panel core, so it never disturbs streaming."));
    Serial.println(F("\n-- DIAGNOSTICS (stop verpixeld before test patterns) --"));
    Serial.println(F("  s              print received/dropped frame counters"));
    Serial.println(F("  l              list the panel config registers (index, d0/G/d2 hex word)"));
    Serial.println(F("  t f <0..8191>  solid grey fill at a 13-bit level (best for tuning columns)"));
    Serial.println(F("  t v <col>      single vertical line at logical column 0..255"));
    Serial.println(F("  t h <row>      single horizontal line at logical row 0..127"));
    Serial.println(F("  t g            light only the 4 boundary columns 63/127/191/255"));
    Serial.println(F("  t n            leave test mode, resume the network stream"));
    Serial.println(F("\n-- PANEL TUNING --"));
    Serial.println(F("  k <mhz>        data clock MHz (higher = faster upload/refresh + brighter; <=19)"));
    Serial.println(F("  g <bits>       greyscale bit width (8..14; must match verpixeld ColorBits)"));
    Serial.println(F("  mode 8 | 14    8=8-bit/triple-buffer (min UDP drops, max fps), 14=14-bit/double"));
    Serial.println(F("                 (max colour). Set verpixeld ColorBits to match. save + reboot to apply."));
    Serial.println(F("  applymode 8|14 colour mode + save + reboot (web UI)"));
    Serial.println(F("  r <c> <i> <hx> set config register [i] on data line c (0/1/2) to hex word"));
    Serial.println(F("  a <i> <hx>     set config register [i] on ALL three data lines at once"));
    Serial.println(F("\n  (Boundary/seam column correction now runs host-side in the RgbPanel streaming"));
    Serial.println(F("   library - keeps the panel refresh fast. Tune it in verpixeld / seam_correction.json.)"));
    Serial.println(F("\n-- NETWORK (takes effect after 'save' + reboot) --"));
    Serial.println(F("  net                          show mode + current IP / mask / gateway"));
    Serial.println(F("  net dhcp                     obtain the address via DHCP (default)"));
    Serial.println(F("  net static <ip> <mask> <gw>  use a fixed address"));
    Serial.println(F("  applynet dhcp | applynet static <ip> <mask> <gw>  save + reboot (web UI)"));
    Serial.println(F("  name [text|-]                show / set friendly name (saved immediately; - clears)"));
    Serial.println(F("\n-- CONFIG PERSISTENCE (flash) --"));
    Serial.println(F("  save           store current settings so they survive reboot"));
    Serial.println(F("  load           re-read stored settings"));
    Serial.println(F("  reset          restore code defaults (then 'save' to persist)"));
    Serial.println(F("  reboot         restart the board (e.g. to apply a new network config)"));
    Serial.println(F("  help / ?       show this menu"));
    Serial.printf("\nConfig web UI: http://%u.%u.%u.%u:%d/  (same commands, no serial needed)\n",
                  g_ip[0], g_ip[1], g_ip[2], g_ip[3], WEB_PORT);
    Serial.printf("\nstate: DCLK=%.2fMHz gclk=%d mode=%d-bit/%s  net=%s IP=%u.%u.%u.%u\n\n",
        g_dclkMHz, g_gclkBits, COLORMODE_PLANES(g_colorMode), g_colorMode ? "triple" : "double",
        g_netMode == 0 ? "DHCP" : "STATIC", g_ip[0], g_ip[1], g_ip[2], g_ip[3]);
}

// Parse one complete command line. Multi-char keywords first, then single-letter commands.
static void processCmd(char *b)
{
    if (b[0] == '?' || !strcmp(b, "help")) { printHelp(); return; }
    if (!strcmp(b, "save"))  { saveCfg();  Serial.println(F("[cfg] saved to flash")); return; }
    if (!strcmp(b, "load"))  { loadCfg();  g_testDirty = true; Serial.println(F("[cfg] loaded from flash")); return; }
    if (!strcmp(b, "reset")) { resetCfg(); g_testDirty = true; Serial.println(F("[cfg] defaults restored (use 'save' to persist)")); return; }
    if (!strcmp(b, "reboot")) { Serial.println(F("[cfg] rebooting...")); Serial.flush(); delay(80); rp2040.reboot(); return; }
    if (!strcmp(b, "zero"))   { g_good = 0; g_drop = 0; Serial.println(F("[stat] counters cleared")); return; }
    if (!strncmp(b, "applymode", 9)) {
        int m = atoi(b + 9);
        if (m == 8) g_colorMode = 1;
        else if (m == 14) g_colorMode = 0;
        else { Serial.println(F("usage: applymode 8 | 14")); return; }
        saveCfg();
        Serial.printf("[cfg] mode=%d-bit saved, rebooting...\n", m);
        Serial.flush(); delay(80); rp2040.reboot();
        return;
    }
    if (!strncmp(b, "applynet", 8)) {
        char *a = b + 8; while (*a == ' ') a++;
        if (!strncmp(a, "dhcp", 4)) g_netMode = 0;
        else if (!strncmp(a, "static", 6)) {
            char ip[20], mk[20], gw[20];
            if (sscanf(a + 6, "%19s %19s %19s", ip, mk, gw) != 3 ||
                !parseIp(ip, g_cfgIp) || !parseIp(mk, g_cfgMask) || !parseIp(gw, g_cfgGw)) {
                Serial.println(F("usage: applynet static <ip> <mask> <gw>"));
                return;
            }
            g_netMode = 1;
        } else { Serial.println(F("usage: applynet dhcp | applynet static <ip> <mask> <gw>")); return; }
        saveCfg();
        Serial.println(F("[net] saved, rebooting..."));
        Serial.flush(); delay(80); rp2040.reboot();
        return;
    }
    if (!strncmp(b, "net", 3)) { handleNetCmd(b + 3); return; }
    if (!strncmp(b, "name", 4)) {
        char *a = b + 4; while (*a == ' ') a++;
        if (*a == 0) {
            Serial.printf("[net] name='%s'  host=%s.local  mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                          panel_display_name(), g_hostLabel,
                          W6_MAC[0], W6_MAC[1], W6_MAC[2], W6_MAC[3], W6_MAC[4], W6_MAC[5]);
            return;
        }
        if (a[0] == '-' && a[1] == 0) g_panelName[0] = 0;
        else {
            strncpy(g_panelName, a, sizeof(g_panelName) - 1);
            g_panelName[sizeof(g_panelName) - 1] = 0;
            sanitize_name(g_panelName, sizeof(g_panelName));
        }
        Serial.printf("[net] name='%s' (saved)\n", panel_display_name());
        saveCfg();
        return;
    }
    if (!strncmp(b, "mode", 4)) {
        int m = atoi(b + 4);
        if (m == 8)       { g_colorMode = 1; Serial.println(F("[cfg] mode=8-bit/triple (min drops, max fps, full brightness) - run 'save' then reboot")); }
        else if (m == 14) { g_colorMode = 0; Serial.println(F("[cfg] mode=14-bit/double (max colour depth) - run 'save' then reboot")); }
        else Serial.printf("[cfg] mode=%d-bit/%s (gclk=14, full brightness). usage: mode 8 | mode 14  (set verpixeld ColorBits to match; save + reboot)\n",
                           COLORMODE_PLANES(g_colorMode), g_colorMode ? "triple" : "double");
        return;
    }
    switch (b[0]) {
    case 'k': { g_dclkMHz = dmd.setDataClkMHz((float)atof(b + 1)); Serial.printf("[tune] DCLK=%.2f MHz\n", g_dclkMHz); } break;
    case 'g': { int bits = atoi(b + 1); dmd.setGclkBits((uint8_t)bits); g_gclkBits = (uint8_t)bits; Serial.printf("[tune] gclk_bits=%d\n", bits); } break;
    case 'r': { int c, idx; unsigned v;
                if (sscanf(b + 1, "%d %d %x", &c, &idx, &v) == 3 && c >= 0 && c <= 2 && idx >= 0 && idx < (int)REG_CNT) {
                    uint16_t *arr = (c == 0) ? SLOT0 : (c == 2) ? SLOT2 : regGreen;
                    arr[idx] = (uint16_t)v; applyRegs(); g_testDirty = true;
                    Serial.printf("[reg] c%d[%d]=0x%04X applied\n", c, idx, (uint16_t)v);
                } else Serial.println(F("usage: r <0|1|2> <idx> <hexword>")); } break;
    case 'a': { int idx; unsigned v;
                if (sscanf(b + 1, "%d %x", &idx, &v) == 2 && idx >= 0 && idx < (int)REG_CNT) {
                    regRed[idx] = regGreen[idx] = regBlue[idx] = (uint16_t)v; applyRegs(); g_testDirty = true;
                    Serial.printf("[reg] all[%d]=0x%04X applied\n", idx, (uint16_t)v);
                } else Serial.println(F("usage: a <idx> <hexword>")); } break;
    case 'l': for (int i = 0; i < (int)REG_CNT; i++)
                  Serial.printf("  [%2d] d0=%04X G=%04X d2=%04X\n", i, SLOT0[i], regGreen[i], SLOT2[i]);
              break;
    case 's': Serial.printf("[stat] good=%lu drop=%lu\n", (unsigned long)g_good, (unsigned long)g_drop); break;
    case 't': { char sub = 0; int arg = 0; int nf = sscanf(b + 1, " %c %i", &sub, &arg);
                if (nf >= 1 && sub == 'v') { g_testMode = 1; g_testArg = arg; g_testDirty = true; Serial.printf("[test] vline col=%d (stop verpixeld!)\n", arg); }
                else if (nf >= 1 && sub == 'h') { g_testMode = 2; g_testArg = arg; g_testDirty = true; Serial.printf("[test] hline row=%d\n", arg); }
                else if (nf >= 1 && sub == 'f') { g_testMode = 3; g_testArg = arg; g_testDirty = true; Serial.printf("[test] fill level=%d/8191\n", arg); }
                else if (nf >= 1 && sub == 'g') { g_testMode = 4; g_testDirty = true; Serial.println(F("[test] grid at 63/127/191/255")); }
                else if (nf >= 1 && sub == 'n') { g_testMode = 0; Serial.println(F("[test] off -> stream")); }
                else Serial.println(F("usage: t v<col> | t h<row> | t f<0..8191> | t g | t n")); } break;
    default:  Serial.println(F("unknown command - type 'help'")); break;
    }
}

// Non-blocking serial reader. Runs ONLY on core0 (owns the panel + Serial after g_netReady), so it
// never touches core1's W6300 drain. Acts only on a complete '\n'/'\r'-terminated line.
static void handleSerial()
{
    static char b[80];
    static uint8_t n = 0;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') {
            if (n == 0) continue;
            b[n] = 0; n = 0;
            processCmd(b);
        } else if (n < sizeof(b) - 1) {
            b[n++] = ch;
        }
    }
}

// Run a command queued by the web server (core1). Same executor as the serial menu, on core0 (owns panel).
static void handleWebCmd()
{
    if (!g_webCmdPending) return;
    __sync_synchronize();                 // see the fully-written command before acting on the flag
    char cmd[sizeof(g_webCmd)];
    strncpy(cmd, (const char *)g_webCmd, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = 0;
    g_webCmdPending = false;              // release the slot for the next request
    Serial.printf("[web] %s\n", cmd);
    processCmd(cmd);
}

// ---------------- boot intro: bright, colourful 7-segment IP + firmware version ----------------
