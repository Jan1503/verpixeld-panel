// WebServer.h
// Part of the dmd_icnd1065l_w6300_pioq sketch. It is #included (in dependency order) from the
// main .ino, so this is still ONE translation unit - no forward declarations needed.
// Minimal config web server (socket 1, TCP, served on core1).
#pragma once

// ==================== minimal config web server (socket 1, core1 only) ====================
// A single self-contained page (no external assets) + tiny JSON status + a /cmd bridge that reuses the
// serial command parser on core0. Runs on core1 so it never races core0 for the W6300 QSPI.
// GET replies are paced (~1 KB per poll) so a page load cannot stall the UDP drain. OTA is the
// exception: g_otaActive already holds the panel, so that path may block.

static const char WEB_PAGE[] = R"HTML(<!doctype html><html lang=en><head>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>verpixeld</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' rx='7' fill='%23080d14'/%3E%3Crect x='5' y='7' width='5' height='18' rx='1' fill='%233ee0ff'/%3E%3Crect x='13.5' y='7' width='5' height='18' rx='1' fill='%23e8eef6'/%3E%3Crect x='22' y='7' width='5' height='18' rx='1' fill='%237c5cff'/%3E%3C/svg%3E">
<style>
:root{--bg:#07090d;--card:#10151c;--line:#1d2733;--tx:#e8eef6;--mut:#8b97a8;--ac:#3ee0ff;--ac2:#7c5cff;--ok:#3ee0a0;--bad:#ff5b6e;--warn:#ffc14a}
*{box-sizing:border-box}body{margin:0;font:14px/1.45 system-ui,Segoe UI,sans-serif;color:var(--tx);background:radial-gradient(900px 420px at 8% -8%,rgba(62,224,255,.14),transparent 55%),radial-gradient(800px 380px at 110% 0,rgba(124,92,255,.12),transparent 50%),var(--bg)}
header{display:flex;justify-content:space-between;align-items:center;gap:16px;padding:18px 22px;border-bottom:1px solid var(--line);background:linear-gradient(90deg,#0c1520,#07090d);position:relative;overflow:hidden}
header:before{content:"";position:absolute;inset:0;background-image:repeating-linear-gradient(90deg,transparent,transparent 9px,rgba(62,224,255,.045) 9px,rgba(62,224,255,.045) 10px),repeating-linear-gradient(0deg,transparent,transparent 9px,rgba(62,224,255,.045) 9px,rgba(62,224,255,.045) 10px);pointer-events:none}
header:after{content:"";position:absolute;left:0;right:0;height:1px;background:linear-gradient(90deg,transparent,var(--ac),transparent);animation:scan 5s linear infinite;opacity:.45;pointer-events:none}
@keyframes scan{from{top:0}to{top:100%}}@keyframes pulse{50%{opacity:.35}}@keyframes spin{to{transform:rotate(360deg)}}
@media(prefers-reduced-motion:reduce){header:after,.pill.on i,.spin{animation:none}}
.brand{display:flex;gap:12px;align-items:center;position:relative}
.logo{display:flex;gap:3px}.logo i{width:6px;height:20px;border-radius:2px;background:var(--ac)}.logo i:nth-child(2){background:#e8eef6;height:14px;align-self:center}.logo i:nth-child(3){background:var(--ac2)}
h1{margin:0;font-size:18px;letter-spacing:.08em;text-transform:lowercase}.sub{color:var(--mut);font-size:12px;margin-top:2px}
.hdr{display:flex;gap:8px;position:relative;flex-wrap:wrap;justify-content:flex-end}
.pill{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;border-radius:999px;background:#0d131b;border:1px solid var(--line);color:var(--mut);font-size:11px;letter-spacing:.08em;text-transform:uppercase}
.pill i{width:7px;height:7px;border-radius:50%;background:#4a5563}
.pill.on{color:#fff;border-color:rgba(255,91,110,.45)}.pill.on i{background:var(--bad);animation:pulse 1.3s ease-in-out infinite}
.wrap{max-width:920px;margin:0 auto;padding:16px;display:grid;gap:14px;grid-template-columns:1fr 1fr}
.metrics{grid-column:1/-1;display:grid;grid-template-columns:repeat(4,1fr);gap:12px}
.metric{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px 16px}
.metric b{display:block;font-size:22px;font-variant-numeric:tabular-nums;letter-spacing:-.02em}
.metric span{color:var(--mut);font-size:11px;text-transform:uppercase;letter-spacing:.08em}
.metric.warn b{color:var(--warn)}.metric.bad b{color:var(--bad)}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px}
.card h2{margin:0 0 12px;font-size:11px;text-transform:uppercase;letter-spacing:.1em;color:var(--mut)}
.stat{display:flex;justify-content:space-between;gap:12px;padding:6px 0;border-bottom:1px solid #1a222c;font-size:13px}
.stat b{color:#fff;font-variant-numeric:tabular-nums;font-weight:600;text-align:right}
label{display:block;margin:10px 0 4px;color:var(--mut);font-size:12px}
input,select{width:100%;padding:8px 10px;background:#0a0e13;border:1px solid #2a3340;border-radius:8px;color:var(--tx)}
.row{display:flex;gap:8px;align-items:end}.row>*{flex:1}
button{cursor:pointer;padding:8px 12px;border:0;border-radius:8px;background:linear-gradient(180deg,var(--ac),#2bb8d4);color:#04121f;font-weight:700;margin-top:8px}
button.sec{background:#1c2530;color:var(--tx);font-weight:600}
.btns{display:flex;flex-wrap:wrap;gap:8px}.btns button{flex:1;margin:0}
.full{grid-column:1/-1}small{color:var(--mut)}
.drop{border:1px dashed #2a3340;border-radius:12px;padding:20px;text-align:center;cursor:pointer;color:var(--mut)}
.drop.hot{border-color:var(--ac);color:var(--ac)}
.bar{display:none;height:4px;background:#1b2430;border-radius:4px;overflow:hidden;margin-top:10px}
.bar.on{display:block}#pbar{height:100%;width:0;background:linear-gradient(90deg,var(--ac),var(--ac2));transition:width .15s}
#ov{display:none;position:fixed;inset:0;background:rgba(4,7,10,.72);z-index:9;place-items:center}#ov.on{display:grid}
.ovc{display:flex;flex-direction:column;align-items:center;gap:12px;font-weight:700}
.spin{width:28px;height:28px;border:3px solid #243044;border-top-color:var(--ac);border-radius:50%;animation:spin .8s linear infinite}
@media(max-width:720px){.wrap,.metrics{grid-template-columns:1fr 1fr}header{flex-direction:column;align-items:flex-start}}
</style></head><body>
<div id=ov><div class=ovc><div class=spin></div><div id=ovt>Working…</div></div></div>
<header>
  <div class=brand><div class=logo><i></i><i></i><i></i></div><div><h1>verpixeld</h1><div class=sub id=sub>connecting…</div></div></div>
  <div class=hdr><span class=pill id=live><i></i>idle</span><span class=pill id=fw>—</span></div>
</header>
<div class=wrap>
  <div class=metrics>
    <div class=metric><b id=mFps>—</b><span>FPS</span></div>
    <div class=metric id=cDrop><b id=mDrop>—</b><span>dropped</span></div>
    <div class=metric id=cLink><b id=mLink>—</b><span>link</span></div>
    <div class=metric><b id=mMode>—</b><span>buffer</span></div>
  </div>
  <div class=card><h2>Status</h2><div id=st>…</div></div>
  <div class=card><h2>Tuning</h2>
    <label>Data clock (MHz) <small>live, no reboot</small></label>
    <div class=row><input id=dclk type=number step=0.1 min=8 max=20><button onclick="setDclk()">Set</button></div>
    <label>Colour / buffer <small>saves + reboots; set host ColorBits to match</small></label>
    <div class=btns>
      <button class=sec onclick="applyMode(14)">14-bit / double</button>
      <button class=sec onclick="applyMode(8)">8-bit / triple</button>
    </div>
    <label>Test pattern <small>stops the stream until you hit Stream</small></label>
    <div class=btns>
      <button class=sec onclick="go('t g')">Grid</button>
      <button class=sec onclick="go('t f 2000')">Fill</button>
      <button class=sec onclick="go('t n')">Stream</button>
    </div>
    <label>Test line</label>
    <div class=row>
      <input id=tv type=number min=0 max=255 placeholder=col>
      <button class=sec onclick="go('t v '+v('tv'))">V-line</button>
      <input id=th type=number min=0 max=127 placeholder=row>
      <button class=sec onclick="go('t h '+v('th'))">H-line</button>
    </div>
  </div>
  <div class="card full"><h2>Identity</h2>
    <label>Friendly name <small>A–Z a–z 0–9 _ - space, max 15; saved immediately</small></label>
    <div class=row><input id=pname maxlength=15 placeholder="Living room"><button onclick="setName()">Set name</button></div>
  </div>
  <div class="card full"><h2>Network <small>saves + reboots</small></h2>
    <div class=row><label><input type=radio name=nm value=dhcp checked> DHCP</label><label><input type=radio name=nm value=static> Static</label></div>
    <div class=row><div><label>IP</label><input id=ip></div><div><label>Mask</label><input id=mask></div><div><label>Gateway</label><input id=gw></div></div>
    <button onclick="applyNet()">Apply network</button>
  </div>
  <div class="card full"><h2>Firmware update</h2>
    <div id=drop class=drop>Drop the firmware <b>.ino.bin</b> here, or click to choose<input id=fw type=file accept=".bin" style="display:none"></div>
    <div class=bar id=bar><div id=pbar></div></div>
    <div id=fwmsg style="margin-top:8px"></div>
  </div>
  <div class="card full"><h2>Config</h2>
    <div class=btns>
      <button onclick="go('save')">Save to flash</button>
      <button class=sec onclick="go('load')">Load</button>
      <button class=sec onclick="identify()">Identify</button>
      <button class=sec onclick="go('zero')">Reset counters</button>
      <button class=sec onclick="go('reset')">Reset defaults</button>
      <button class=sec onclick="if(confirm('Reboot the panel?')){overlay(true,'Rebooting…');go('reboot')}">Reboot</button>
    </div>
  </div>
</div>
<script>
const $=id=>document.getElementById(id),v=id=>$(id).value;
const cmd=c=>fetch('/cmd?c='+encodeURIComponent(c));
const go=c=>cmd(c).then(()=>setTimeout(refresh,180));
function overlay(on,t){$('ov').className=on?'on':'';if(t)$('ovt').textContent=t}
function row(k,val){return `<div class=stat><span>${k}</span><b>${val}</b></div>`}
let pg=null,pt=0,filled=false,uploading=false,timer=null;
function arm(){if(timer||document.hidden||uploading)return;timer=setInterval(refresh,2000)}
function disarm(){if(timer){clearInterval(timer);timer=null}}
document.addEventListener('visibilitychange',()=>{if(document.hidden)disarm();else{refresh();arm()}});
function refresh(){if(uploading||document.hidden)return;fetch('/status').then(r=>r.json()).then(paint).catch(()=>{})}
function paint(s){
  const now=Date.now();let fps=0;
  if(pg!==null&&now>pt)fps=(s.good-pg)*1000/(now-pt);
  pg=s.good;pt=now;
  const tot=s.good+s.drop,dp=tot>0?s.drop*100/tot:0,live=fps>0.15;
  $('live').className='pill'+(live?' on':'');
  $('live').innerHTML='<i></i>'+(live?'live':'idle');
  $('fw').textContent='fw '+s.ver;
  $('sub').textContent=(s.name||s.host||'panel')+' · '+(s.host||'')+'.local · '+(s.ip||'');
  document.title=(s.name||s.host||'verpixeld')+' · verpixeld';
  $('mFps').textContent=fps?fps.toFixed(1):'0.0';
  $('mDrop').textContent=dp.toFixed(1)+'%';
  $('cDrop').className='metric'+(dp>=5?' bad':dp>=1?' warn':'');
  $('mLink').textContent=s.link?(s.spd+'M'):'down';
  $('cLink').className='metric'+(s.link?'':' bad');
  $('mMode').textContent=s.mode?'8-bit':'14-bit';
  const link=s.link?('up · '+s.spd+' Mbit/s · '+(s.dpx?'full':'half')+' duplex'):'down';
  $('st').innerHTML=row('Name',s.name||'—')+row('Host',(s.host||'')+'.local')+row('MAC',s.mac||'—')+row('Link',link)+row('IP',s.ip)+row('Mask',s.mask)+row('Gateway',s.gw)+row('Addressing',s.dhcp?'DHCP':'Static')+row('Data clock',s.dclk+' MHz')+row('GCLK','14-bit train')+row('Buffer',s.mode?'8-bit / triple':'14-bit / double')+row('Frames OK',s.good)+row('Dropped',s.drop+' ('+dp.toFixed(1)+'%)')+row('Uptime',s.up+' s')+row('Panel',s.w+'×'+s.h);
  if(!filled){
    $('dclk').value=s.dclk;$('ip').value=s.ip;$('mask').value=s.mask;$('gw').value=s.gw;
    if(s.name)$('pname').value=s.name;
    document.querySelector('input[name=nm][value="'+(s.dhcp?'dhcp':'static')+'"]').checked=true;
    filled=true;
  }else if(document.activeElement!==$('pname')&&s.name)$('pname').value=s.name;
}
function applyMode(m){
  if(!confirm('Switch to '+m+'-bit, save and reboot?\nSet host ColorBits to '+m+' afterwards.'))return;
  overlay(true,'Saving and rebooting…');
  cmd('applymode '+m).catch(()=>{}).finally(()=>setTimeout(()=>location.reload(),8000));
}
function applyNet(){
  const dhcp=document.querySelector('input[name=nm]:checked').value==='dhcp';
  if(!confirm('Save network settings and reboot?'))return;
  overlay(true,'Saving and rebooting…');
  cmd(dhcp?'applynet dhcp':'applynet static '+v('ip')+' '+v('mask')+' '+v('gw')).catch(()=>{}).finally(()=>setTimeout(()=>location.reload(),8000));
}
function setName(){const n=v('pname').trim();cmd(n?'name '+n:'name -').then(()=>setTimeout(refresh,180))}
function setDclk(){cmd('k '+v('dclk')).then(()=>setTimeout(refresh,180))}
function identify(){cmd('t f 8191');setTimeout(()=>cmd('t n'),800)}
function fwUpload(file){
  if(!file.name.toLowerCase().endsWith('.bin')){$('fwmsg').textContent='Choose the .ino.bin (raw firmware), not the .uf2.';return}
  uploading=true;disarm();$('bar').classList.add('on');
  $('fwmsg').textContent='Uploading '+file.name+'… the panel holds the stream. Do not power off.';
  const x=new XMLHttpRequest();
  x.upload.onprogress=e=>{if(e.lengthComputable){$('pbar').style.width=(e.loaded/e.total*100)+'%';$('fwmsg').textContent='Uploading '+Math.round(e.loaded/e.total*100)+'% — stream paused for flash.';}};
  x.onload=()=>{$('fwmsg').textContent='Panel: '+x.responseText+' — reconnecting…';setTimeout(()=>location.reload(),9000)};
  x.onerror=()=>{$('fwmsg').textContent='Upload sent; panel rebooting — reconnecting…';setTimeout(()=>location.reload(),9000)};
  x.open('POST','/update');x.send(file);
}
const drop=$('drop'),fw=$('fw');
drop.onclick=()=>fw.click();
drop.ondragover=e=>{e.preventDefault();drop.classList.add('hot')};
drop.ondragleave=()=>drop.classList.remove('hot');
drop.ondrop=e=>{e.preventDefault();drop.classList.remove('hot');if(e.dataTransfer.files[0])fwUpload(e.dataTransfer.files[0])};
fw.onchange=()=>{if(fw.files[0])fwUpload(fw.files[0])};
refresh();arm();
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
// Used by OTA, where the panel is already held via g_otaActive.
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

// Cooperative GET reply: ~1 KB per poll so loop1 can drain UDP between chunks. No FSR spin.
static char     g_webHdr[168];
static uint16_t g_webHdrLen = 0, g_webHdrOff = 0;
static const uint8_t *g_webBody = nullptr;
static uint32_t g_webBodyLen = 0, g_webBodyOff = 0;
static uint8_t  g_webPhase = 0;   // 0 idle, 1 header, 2 body

static void web_queue(const char *status, const char *ctype, const char *body, uint32_t blen)
{
    g_webHdrLen = (uint16_t)snprintf(g_webHdr, sizeof(g_webHdr),
        "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
        status, ctype, (unsigned long)blen);
    g_webHdrOff = 0;
    g_webBody = (const uint8_t *)body;
    g_webBodyLen = blen;
    g_webBodyOff = 0;
    g_webPhase = 1;
}

static void web_abort_send() { g_webPhase = 0; g_webBody = nullptr; }

// Returns true when the queued reply is fully on the wire (caller should DISCON).
static bool web_send_tick()
{
    const uint32_t budget = 1024;
    uint32_t used = 0;
    while (g_webPhase && used < budget) {
        const uint8_t *src;
        uint32_t left;
        if (g_webPhase == 1) {
            src = (const uint8_t *)g_webHdr + g_webHdrOff;
            left = (uint32_t)g_webHdrLen - g_webHdrOff;
        } else {
            src = g_webBody + g_webBodyOff;
            left = g_webBodyLen - g_webBodyOff;
        }
        if (left == 0) {
            g_webPhase = (g_webPhase == 1 && g_webBodyLen) ? 2 : 0;
            continue;
        }
        uint16_t fsr = w6_rd16(BSB_S1_REG, 0x0204);              // Sn_TX_FSR
        if (fsr == 0) return false;                              // TX full -> drain UDP, retry next poll
        uint32_t chunk = left;
        if (chunk > fsr) chunk = fsr;
        if (chunk > budget - used) chunk = budget - used;
        if (chunk == 0) return false;
        uint16_t wr = w6_rd16(BSB_S1_REG, 0x020C);               // Sn_TX_WR
        w6_write(BSB_S1_TX, wr, src, (int)chunk);
        w6_wr16(BSB_S1_REG, 0x020C, (uint16_t)(wr + chunk));
        w6_wr8(BSB_S1_REG, 0x0010, 0x20);                        // SEND
        uint32_t t = millis(); uint8_t ir = 0;
        while (!((ir = w6_rd8(BSB_S1_REG, 0x0020)) & 0x18) && millis() - t < 50) {}
        w6_wr8(BSB_S1_REG, 0x0028, 0x18);
        if (!(ir & 0x10)) { web_abort_send(); return true; }
        if (g_webPhase == 1) g_webHdrOff = (uint16_t)(g_webHdrOff + chunk);
        else g_webBodyOff += chunk;
        used += chunk;
    }
    return g_webPhase == 0;
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
    if (!sp) { web_queue("400 Bad Request", "text/plain", "", 0); return; }
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
        web_queue("200 OK", "application/json", js, (uint32_t)n);
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
        static const char OK[] = "{\"ok\":1}";
        web_queue("200 OK", "application/json", OK, (uint32_t)(sizeof(OK) - 1));
    } else if (path[0] == '/' && (path[1] == 0 || !strncmp(path, "/index", 6))) {
        web_queue("200 OK", "text/html", WEB_PAGE, (uint32_t)(sizeof(WEB_PAGE) - 1));
    } else if (!strncmp(path, "/favicon.ico", 12) || !strncmp(path, "/favicon.svg", 12)) {
        static const char FAV[] =
            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"
            "<rect width='32' height='32' rx='7' fill='#080d14'/>"
            "<rect x='5' y='7' width='5' height='18' rx='1' fill='#3ee0ff'/>"
            "<rect x='13.5' y='7' width='5' height='18' rx='1' fill='#e8eef6'/>"
            "<rect x='22' y='7' width='5' height='18' rx='1' fill='#7c5cff'/>"
            "</svg>";
        web_queue("200 OK", "image/svg+xml", FAV, (uint32_t)(sizeof(FAV) - 1));
    } else {
        web_queue("404 Not Found", "text/plain", "not found", 9);
    }
}

// Non-blocking service of socket 1. Called once per core1 loop after the UDP drain. Advances the TCP
// state machine over successive calls; only does real work (read request + send reply) on user action.
static void web_poll()
{
    uint8_t sr = w6_rd8(BSB_S1_REG, 0x0030);        // Sn_SR
    if (sr != 0x17 && g_webPhase) web_abort_send(); // client gone / not ESTABLISHED -> drop queued reply
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
        if (g_webPhase) {
            if (web_send_tick()) w6_wr8(BSB_S1_REG, 0x0010, 0x08);  // DISCON when the reply is out
            break;
        }
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
            w6_wr8(BSB_S1_REG, 0x0010, 0x08);       // DISCON (HTTP/1.0 close)
        } else {
            uint16_t extra = (uint16_t)(rsr - n);   // flush any leftover of this (small) GET request
            if (extra) { uint16_t rd2 = w6_rd16(BSB_S1_REG, 0x0228); w6_wr16(BSB_S1_REG, 0x0228, (uint16_t)(rd2 + extra)); w6_wr8(BSB_S1_REG, 0x0010, 0x40); }
            web_handle((char *)req);
            if (web_send_tick()) w6_wr8(BSB_S1_REG, 0x0010, 0x08);
        }
        break;
    }
    case 0x1C:                                      // CLOSE_WAIT -> disconnect
        w6_wr8(BSB_S1_REG, 0x0010, 0x08);           // DISCON
        break;
    default:
        break;                                      // transient states -> next poll
    }
}
