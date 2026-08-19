// WebServer.h
// Part of the dmd_icnd1065l_w6300_pioq sketch. It is #included (in dependency order) from the
// main .ino, so this is still ONE translation unit - no forward declarations needed.
// Minimal config web server (socket 1, TCP, served on core1).
#pragma once

// ==================== minimal config web server (socket 1, core1 only) ====================
// A single self-contained page (no external assets) + tiny JSON status + a /cmd bridge that reuses the
// serial command parser on core0. Runs on core1 so it never races core0 for the W6300 QSPI. Handling is
// brief and only happens on user interaction, so the stream's 30 KB RX buffer covers the pause.

static const char WEB_PAGE[] = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>W6300 Panel</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' rx='7' fill='%2312233b'/%3E%3Crect x='5' y='7' width='5' height='18' rx='1' fill='%233ba1ff'/%3E%3Crect x='13.5' y='7' width='5' height='18' rx='1' fill='%23d6dde6'/%3E%3Crect x='22' y='7' width='5' height='18' rx='1' fill='%233ba1ff'/%3E%3C/svg%3E">
<style>
:root{--bg:#0e1116;--card:#171b22;--ac:#3ba1ff;--tx:#d6dde6;--mut:#7c8794}
*{box-sizing:border-box}body{margin:0;font:14px system-ui,Segoe UI,sans-serif;background:var(--bg);color:var(--tx)}
header{padding:16px 20px;background:linear-gradient(90deg,#12233b,#0e1116);border-bottom:1px solid #222}
h1{margin:0;font-size:18px}h1 span{color:var(--ac)}
.wrap{max-width:860px;margin:0 auto;padding:16px;display:grid;gap:16px;grid-template-columns:1fr 1fr}
.card{background:var(--card);border:1px solid #232a33;border-radius:12px;padding:16px}
.card h2{margin:0 0 12px;font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--mut)}
.stat{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #20262e}
.stat b{color:#fff;font-variant-numeric:tabular-nums}
label{display:block;margin:8px 0 4px;color:var(--mut)}
input,select{width:100%;padding:8px;background:#0d1117;border:1px solid #2a323c;border-radius:8px;color:var(--tx)}
.row{display:flex;gap:8px}.row>*{flex:1}
button{cursor:pointer;padding:8px 12px;border:0;border-radius:8px;background:var(--ac);color:#04121f;font-weight:600;margin-top:8px}
button.sec{background:#263140;color:var(--tx)}
.btns{display:flex;flex-wrap:wrap;gap:8px}.btns button{flex:1;margin:0}
.full{grid-column:1/-1}small{color:var(--mut)}
</style></head><body><header><h1>W6300 <span>Panel Control</span></h1></header><div class=wrap>
<div class=card><h2>Status</h2><div id=st>...</div></div>
<div class=card><h2>Tuning</h2>
<label>Data clock (MHz)</label><div class=row><input id=dclk type=number step=0.1 min=8 max=20><button onclick="cmd('k '+v('dclk'))">Set</button></div>
<label>Greyscale bits</label><div class=row><select id=gclk><option>8<option>10<option>12<option>13<option>14</select><button onclick="cmd('g '+v('gclk'))">Set</button></div>
<label>Colour / buffer mode <small>(set verpixeld ColorBits to match; save + reboot)</small></label><div class=btns><button class=sec onclick="if(confirm('Switch to 14-bit/double and reboot after save?'))cmd('mode 14')">14-bit / double</button><button class=sec onclick="if(confirm('Switch to 8-bit/triple and reboot after save?'))cmd('mode 8')">8-bit / triple</button></div>
<label>Test pattern</label><div class=btns><button class=sec onclick="cmd('t g')">Grid</button><button class=sec onclick="cmd('t f 2000')">Fill</button><button class=sec onclick="cmd('t n')">Stream</button></div>
<label>Test line</label><div class=row><input id=tv type=number min=0 max=255 placeholder=col><button class=sec onclick="cmd('t v '+v('tv'))">V-line</button><input id=th type=number min=0 max=127 placeholder=row><button class=sec onclick="cmd('t h '+v('th'))">H-line</button></div></div>
<div class=card full><h2>Identity</h2>
<label>Friendly name <small>(A–Z a–z 0–9 _ - space, max 15; saved immediately, no reboot)</small></label>
<div class=row><input id=pname maxlength=15 placeholder="Living room"><button onclick="setName()">Set name</button></div>
</div>
<div class=card full><h2>Network <small>(save + reboot to apply)</small></h2>
<div class=row><label><input type=radio name=nm value=dhcp checked> DHCP</label><label><input type=radio name=nm value=static> Static</label></div>
<div class=row><div><label>IP</label><input id=ip></div><div><label>Mask</label><input id=mask></div><div><label>Gateway</label><input id=gw></div></div>
<button onclick="applyNet()">Apply network</button></div>
<div class=card full><h2>Firmware update <small>(drop a .bin, updates over LAN + reboots)</small></h2>
<div id=drop style="border:2px dashed #2a323c;border-radius:10px;padding:18px;text-align:center;cursor:pointer;color:#9aa4b0">Drop the firmware <b>.ino.bin</b> here, or click to choose<input id=fw type=file accept=".bin" style="display:none"></div>
<div id=fwmsg style="margin-top:8px"></div></div>
<div class=card full><h2>Config</h2><div class=btns><button onclick="cmd('save')">Save to flash</button><button class=sec onclick="cmd('load')">Load</button><button class=sec onclick="identify()">Identify</button><button class=sec onclick="cmd('zero')">Reset counters</button><button class=sec onclick="cmd('reset')">Reset defaults</button><button class=sec onclick="if(confirm('Reboot the panel?'))cmd('reboot')">Reboot</button></div></div>
</div><script>
const v=id=>document.getElementById(id).value;
const row=(k,val)=>`<div class=stat><span>${k}</span><b>${val}</b></div>`;
function cmd(c){fetch('/cmd?c='+encodeURIComponent(c)).then(()=>setTimeout(refresh,150))}
function applyNet(){let m=document.querySelector('input[name=nm]:checked').value;cmd(m=='dhcp'?'net dhcp':'net static '+v('ip')+' '+v('mask')+' '+v('gw'))}
function setName(){let n=v('pname').trim();cmd(n?'name '+n:'name -')}
function identify(){cmd('t f 8191');setTimeout(()=>cmd('t n'),800)}
let _pg=null,_pt=0,_filled=false;
function refresh(){fetch('/status').then(r=>r.json()).then(s=>{
let now=Date.now(),fps='--';
if(_pg!==null&&now>_pt)fps=((s.good-_pg)*1000/(now-_pt)).toFixed(1);
_pg=s.good;_pt=now;
let tot=s.good+s.drop,dp=tot>0?(s.drop*100/tot).toFixed(1):'0';
let link=s.link?('up, '+s.spd+' Mbit/s, '+(s.dpx?'full':'half')+' duplex'):'down';
document.getElementById('st').innerHTML=row('Name',s.name||s.host||'—')+row('Host',(s.host||'')+'.local')+row('MAC',s.mac||'—')+row('Link',link)+row('IP',s.ip)+row('Mask',s.mask)+row('Gateway',s.gw)+row('Mode',s.dhcp?'DHCP':'Static')+row('Data clock',s.dclk+' MHz')+row('Greyscale',s.gclk+'-bit')+row('Buffer mode',s.mode?'8-bit / triple':'14-bit / double')+row('FPS',fps)+row('Frames OK',s.good)+row('Dropped',s.drop+' ('+dp+'%)')+row('Uptime',s.up+' s')+row('Firmware',s.ver);
if(!_filled){document.getElementById('dclk').value=s.dclk;document.getElementById('gclk').value=s.gclk;document.getElementById('ip').value=s.ip;document.getElementById('mask').value=s.mask;document.getElementById('gw').value=s.gw;if(s.name)document.getElementById('pname').value=s.name;_filled=true;}
else if(document.activeElement!==document.getElementById('pname')&&s.name)document.getElementById('pname').value=s.name;})}
refresh();setInterval(refresh,2000);
const drop=document.getElementById('drop'),fw=document.getElementById('fw'),fwmsg=document.getElementById('fwmsg');
drop.onclick=()=>fw.click();
drop.ondragover=e=>{e.preventDefault();drop.style.borderColor='#3ba1ff';};
drop.ondragleave=()=>{drop.style.borderColor='#2a323c';};
drop.ondrop=e=>{e.preventDefault();drop.style.borderColor='#2a323c';if(e.dataTransfer.files[0])fwUpload(e.dataTransfer.files[0]);};
fw.onchange=()=>{if(fw.files[0])fwUpload(fw.files[0]);};
function fwUpload(file){
  if(!file.name.toLowerCase().endsWith('.bin')){fwmsg.textContent='Choose the .ino.bin (raw firmware), not the .uf2.';return;}
  fwmsg.textContent='Uploading '+file.name+' ('+file.size+' bytes)… the panel reboots when done, do not power off.';
  fetch('/update',{method:'POST',body:file})
    .then(r=>r.text()).then(t=>{fwmsg.textContent='Panel: '+t+' — reconnecting…';setTimeout(()=>location.reload(),9000);})
    .catch(()=>{fwmsg.textContent='Upload sent; panel rebooting — reconnecting…';setTimeout(()=>location.reload(),9000);});
}
</script></body></html>)HTML";

static int web_hex(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

static void web_url_decode(char *dst, const char *src, int max)
{
    int i = 0;
    while (*src && i < max - 1) {
        char c = *src++;
        if (c == '&') break;                       // stop at next query param
        if (c == '+') c = ' ';
        else if (c == '%' && src[0] && src[1]) { c = (char)((web_hex(src[0]) << 4) | web_hex(src[1])); src += 2; }
        dst[i++] = c;
    }
    dst[i] = 0;
}

// Send raw bytes on socket 1, chunked to the TX buffer free size. Bounded by timeouts (never hangs core1).
static void web_send_raw(const uint8_t *data, uint32_t len)
{
    uint32_t sent = 0;
    while (sent < len) {
        uint16_t fsr = 0; uint32_t t = millis();
        while ((fsr = w6_rd16(BSB_S1_REG, 0x0204)) == 0 && millis() - t < 200) {}   // Sn_TX_FSR
        if (fsr == 0) return;
        uint32_t chunk = len - sent; if (chunk > fsr) chunk = fsr;
        uint16_t wr = w6_rd16(BSB_S1_REG, 0x020C);                                   // Sn_TX_WR
        w6_write(BSB_S1_TX, wr, data + sent, (int)chunk);
        w6_wr16(BSB_S1_REG, 0x020C, (uint16_t)(wr + chunk));
        w6_wr8(BSB_S1_REG, 0x0010, 0x20);                                            // SEND
        t = millis(); uint8_t ir = 0;
        while (!((ir = w6_rd8(BSB_S1_REG, 0x0020)) & 0x18) && millis() - t < 300) {} // SENDOK|TIMEOUT
        w6_wr8(BSB_S1_REG, 0x0028, 0x18);                                            // clear Sn_IR
        if (!(ir & 0x10)) return;
        sent += chunk;
    }
}

static void web_send(const char *status, const char *ctype, const char *body, uint32_t blen)
{
    char hdr[128];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
        status, ctype, (unsigned long)blen);
    web_send_raw((const uint8_t *)hdr, (uint32_t)hl);
    if (blen) web_send_raw((const uint8_t *)body, blen);
}

// Receive a raw firmware .bin POSTed to /update and self-flash it (OTA). The body is streamed straight
// from socket 1 into the Updater (which stages to LittleFS + programs PicoOTA at the correct address);
// on success we reboot so the OTA bootloader applies it. Runs on core1; g_otaActive holds core0's panel
// while the flash writes stall XIP. Requires the sketch to be built with an FS partition.
static void web_ota_receive(uint8_t *req, uint16_t firstN)
{
    req[firstN] = 0;
    char *cl = strstr((char *)req, "Content-Length:");
    if (!cl) cl = strstr((char *)req, "content-length:");
    long clen = cl ? atol(cl + 15) : -1;
    char *body = strstr((char *)req, "\r\n\r\n");
    if (clen <= 0 || clen > 4000000 || !body) { web_send("400 Bad Request", "text/plain", "bad OTA request", 15); return; }
    body += 4;
    uint16_t headerLen = (uint16_t)((uint8_t *)body - req);
    uint16_t bodyInBuf = (firstN > headerLen) ? (uint16_t)(firstN - headerLen) : 0;

    g_otaActive = true;                    // core0 stops touching the panel while we program flash
    __sync_synchronize();

    if (!Update.begin((size_t)clen, U_FLASH)) {
        g_otaActive = false;
        web_send("507 Insufficient Storage", "text/plain", "OTA begin failed - built with an FS partition?", 46);
        return;
    }

    uint32_t written = 0;
    bool werr = false;
    if (bodyInBuf) { werr = (Update.write((uint8_t *)body, bodyInBuf) != bodyInBuf); written += bodyInBuf; }

    static uint8_t chunk[2048];
    uint32_t lastData = millis();
    while (!werr && written < (uint32_t)clen) {
        uint16_t rsr = w6_rd16(BSB_S1_REG, 0x0224);            // Sn_RX_RSR
        if (rsr == 0) {
            if (millis() - lastData > 10000) break;            // sender stalled -> abort
            continue;
        }
        uint32_t remain = (uint32_t)clen - written;
        uint16_t want = (uint16_t)(rsr < remain ? rsr : remain);
        if (want > sizeof(chunk)) want = sizeof(chunk);
        uint16_t rd = w6_rd16(BSB_S1_REG, 0x0228);             // Sn_RX_RD
        w6_read(BSB_S1_RX, rd, chunk, want);
        w6_wr16(BSB_S1_REG, 0x0228, (uint16_t)(rd + want));
        w6_wr8(BSB_S1_REG, 0x0010, 0x40);                      // RECV
        if (Update.write(chunk, want) != want) { werr = true; break; }
        written += want;
        lastData = millis();
    }

    if (!werr && written == (uint32_t)clen && Update.end(true)) {
        web_send("200 OK", "text/plain", "OTA OK - rebooting", 18);
        delay(400);
        rp2040.reboot();                                       // OTA bootloader applies the image on boot
    } else {
        Update.end(false);
        g_otaActive = false;
        web_send("500 Internal Server Error", "text/plain", "OTA failed (size/write/verify)", 30);
    }
}

static void web_handle(char *req)
{
    char *sp = strchr(req, ' ');
    if (!sp) { web_send("400 Bad Request", "text/plain", "", 0); return; }
    char *path = sp + 1;
    char *end = strchr(path, ' '); if (end) *end = 0;

    if (!strncmp(path, "/status", 7)) {
        uint8_t phy = w6_rd8(BSB_COMMON, 0x3000);   // PHYSR: b0 LNK(1=up), b1 SPD(1=10M), b2 DPX(1=half)
        static char js[640];
        int n = snprintf(js, sizeof(js),
            "{\"kind\":\"verpixeld-panel\",\"ip\":\"%u.%u.%u.%u\",\"mask\":\"%u.%u.%u.%u\",\"gw\":\"%u.%u.%u.%u\","
            "\"dhcp\":%d,\"dclk\":%.2f,\"gclk\":%d,\"good\":%lu,\"drop\":%lu,\"up\":%lu,"
            "\"link\":%d,\"spd\":%d,\"dpx\":%d,\"mode\":%d,\"ver\":\"%s\","
            "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"id\":\"%02x%02x%02x%02x%02x%02x%02x%02x\","
            "\"name\":\"%s\",\"host\":\"%s\",\"w\":%d,\"h\":%d,\"bits\":%d,\"udp\":%d,\"web\":%d}",
            g_ip[0], g_ip[1], g_ip[2], g_ip[3], g_mask[0], g_mask[1], g_mask[2], g_mask[3],
            g_gw[0], g_gw[1], g_gw[2], g_gw[3], g_dhcpOk ? 1 : 0, (double)g_dclkMHz, g_gclkBits,
            (unsigned long)g_good, (unsigned long)g_drop, (unsigned long)(millis() / 1000),
            (phy & 1) ? 1 : 0, (phy & 2) ? 10 : 100, (phy & 4) ? 0 : 1, g_colorMode, FW_VERSION,
            W6_MAC[0], W6_MAC[1], W6_MAC[2], W6_MAC[3], W6_MAC[4], W6_MAC[5],
            g_uid[0], g_uid[1], g_uid[2], g_uid[3], g_uid[4], g_uid[5], g_uid[6], g_uid[7],
            panel_display_name(), g_hostLabel, dmd.width(), dmd.height(),
            COLORMODE_PLANES(g_colorMode), UDP_PORT, WEB_PORT);
        web_send("200 OK", "application/json", js, (uint32_t)n);
    } else if (!strncmp(path, "/cmd", 4)) {
        char *q = strchr(path, '?');
        char *cv = q ? strstr(q, "c=") : nullptr;
        if (cv && !g_webCmdPending) {
            static char dec[96];
            web_url_decode(dec, cv + 2, sizeof(dec));
            strncpy((char *)g_webCmd, dec, sizeof(g_webCmd) - 1);
            ((char *)g_webCmd)[sizeof(g_webCmd) - 1] = 0;
            __sync_synchronize();               // publish the command before the flag
            g_webCmdPending = true;
        }
        web_send("200 OK", "application/json", "{\"ok\":1}", 8);
    } else if (path[0] == '/' && (path[1] == 0 || !strncmp(path, "/index", 6))) {
        web_send("200 OK", "text/html", WEB_PAGE, (uint32_t)(sizeof(WEB_PAGE) - 1));
    } else if (!strncmp(path, "/favicon.ico", 12) || !strncmp(path, "/favicon.svg", 12)) {
        static const char FAV[] =
            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"
            "<rect width='32' height='32' rx='7' fill='#12233b'/>"
            "<rect x='5' y='7' width='5' height='18' rx='1' fill='#3ba1ff'/>"
            "<rect x='13.5' y='7' width='5' height='18' rx='1' fill='#d6dde6'/>"
            "<rect x='22' y='7' width='5' height='18' rx='1' fill='#3ba1ff'/>"
            "</svg>";
        web_send("200 OK", "image/svg+xml", FAV, (uint32_t)(sizeof(FAV) - 1));
    } else {
        web_send("404 Not Found", "text/plain", "not found", 9);
    }
}

// Non-blocking service of socket 1. Called once per core1 loop after the UDP drain. Advances the TCP
// state machine over successive calls; only does real work (read request + send reply) on user action.
static void web_poll()
{
    uint8_t sr = w6_rd8(BSB_S1_REG, 0x0030);        // Sn_SR
    switch (sr) {
    case 0x00:                                      // CLOSED -> open TCP
        w6_wr8(BSB_S1_REG, 0x0000, 0x01);           // Sn_MR = TCP
        w6_wr16(BSB_S1_REG, 0x0114, WEB_PORT);      // Sn_PORT
        w6_wr8(BSB_S1_REG, 0x0010, 0x01);           // OPEN
        break;
    case 0x13:                                      // INIT -> LISTEN
        w6_wr8(BSB_S1_REG, 0x0010, 0x02);           // LISTEN
        break;
    case 0x17: {                                    // ESTABLISHED
        uint16_t rsr = w6_rd16(BSB_S1_REG, 0x0224); // Sn_RX_RSR
        if (rsr == 0) break;
        static uint8_t req[1024];
        uint16_t n = rsr > (uint16_t)(sizeof(req) - 1) ? (uint16_t)(sizeof(req) - 1) : rsr;
        uint16_t rd = w6_rd16(BSB_S1_REG, 0x0228);  // Sn_RX_RD
        w6_read(BSB_S1_RX, rd, req, n);
        w6_wr16(BSB_S1_REG, 0x0228, (uint16_t)(rd + n)); // consume ONLY what we read (OTA body streams on)
        w6_wr8(BSB_S1_REG, 0x0010, 0x40);           // RECV
        req[n] = 0;

        if (!strncmp((char *)req, "POST /update", 12)) {
            web_ota_receive(req, n);                // streams the rest of the .bin, then reboots on success
        } else {
            uint16_t extra = (uint16_t)(rsr - n);   // flush any leftover of this (small) GET request
            if (extra) { uint16_t rd2 = w6_rd16(BSB_S1_REG, 0x0228); w6_wr16(BSB_S1_REG, 0x0228, (uint16_t)(rd2 + extra)); w6_wr8(BSB_S1_REG, 0x0010, 0x40); }
            web_handle((char *)req);
        }
        w6_wr8(BSB_S1_REG, 0x0010, 0x08);           // DISCON (HTTP/1.0 close)
        break;
    }
    case 0x1C:                                      // CLOSE_WAIT -> disconnect
        w6_wr8(BSB_S1_REG, 0x0010, 0x08);           // DISCON
        break;
    default:
        break;                                      // transient states -> next poll
    }
}

