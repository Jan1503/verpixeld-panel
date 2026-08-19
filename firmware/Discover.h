// Discover.h
// UDP 7778 probe/announce + a tiny mDNS A-record responder for panel-xxxxxx.local.
// Runs on core1, socket 2 (discovery) and socket 3 (mDNS). Never touches socket 0 (frame drain).
#pragma once

#define BSB_S2_REG 0x09
#define BSB_S2_TX  0x0A
#define BSB_S2_RX  0x0B
#define BSB_S3_REG 0x0D
#define BSB_S3_TX  0x0E
#define BSB_S3_RX  0x0F
#define MDNS_PORT  5353

#define VPXD_MAGIC0 'V'
#define VPXD_MAGIC1 'P'
#define VPXD_MAGIC2 'X'
#define VPXD_MAGIC3 'D'
#define VPXD_PROBE    0x01
#define VPXD_ANNOUNCE 0x02

static uint32_t g_discLastAnnounce = 0;

static void disc_open_udp(uint8_t bsb_reg, uint16_t port, bool multicast)
{
    w6_wr8(bsb_reg, 0x0000, multicast ? 0x82 : 0x02);   // Sn_MR = UDP (+ MULTI)
    w6_wr16(bsb_reg, 0x0114, port);                     // Sn_PORT
    if (multicast) {
        uint8_t gip[4] = { 224, 0, 0, 251 };
        uint8_t gmac[6] = { 0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB };
        w6_write(bsb_reg, 0x0120, gip, 4);              // Sn_DIPR
        w6_write(bsb_reg, 0x0118, gmac, 6);             // Sn_DHAR
        w6_wr16(bsb_reg, 0x0140, MDNS_PORT);            // Sn_DPORTR
    }
    w6_wr8(bsb_reg, 0x0010, 0x01);                      // OPEN
    uint32_t t = millis();
    while (w6_rd8(bsb_reg, 0x0010) && millis() - t < 200) {}
}

static void disc_udp_send(uint8_t bsb_reg, uint8_t bsb_tx,
                          const uint8_t *dip, uint16_t dport,
                          const uint8_t *dmac,
                          const uint8_t *data, uint16_t len)
{
    if (dmac) w6_write(bsb_reg, 0x0118, dmac, 6);
    w6_write(bsb_reg, 0x0120, dip, 4);
    w6_wr16(bsb_reg, 0x0140, dport);
    uint16_t wr = w6_rd16(bsb_reg, 0x020C);
    w6_write(bsb_tx, wr, data, len);
    w6_wr16(bsb_reg, 0x020C, (uint16_t)(wr + len));
    w6_wr8(bsb_reg, 0x0010, 0x20);                      // SEND
    uint32_t t = millis(); uint8_t ir = 0;
    while (!((ir = w6_rd8(bsb_reg, 0x0020)) & 0x18) && millis() - t < 80) {}
    w6_wr8(bsb_reg, 0x0028, 0x18);
}

static uint16_t disc_build_announce(uint8_t *b)
{
    // VPXD | type | mac[6] | ip[4] | udpBE | webBE | wBE | hBE | bits | name[16] | ver[8] | uid[8]
    uint16_t o = 0;
    b[o++] = VPXD_MAGIC0; b[o++] = VPXD_MAGIC1; b[o++] = VPXD_MAGIC2; b[o++] = VPXD_MAGIC3;
    b[o++] = VPXD_ANNOUNCE;
    memcpy(b + o, W6_MAC, 6); o += 6;
    b[o++] = g_ip[0]; b[o++] = g_ip[1]; b[o++] = g_ip[2]; b[o++] = g_ip[3];
    b[o++] = (uint8_t)(UDP_PORT >> 8); b[o++] = (uint8_t)UDP_PORT;
    b[o++] = (uint8_t)(WEB_PORT >> 8); b[o++] = (uint8_t)WEB_PORT;
    uint16_t w = (uint16_t)dmd.width(), h = (uint16_t)dmd.height();
    b[o++] = (uint8_t)(w >> 8); b[o++] = (uint8_t)w;
    b[o++] = (uint8_t)(h >> 8); b[o++] = (uint8_t)h;
    b[o++] = (uint8_t)COLORMODE_PLANES(g_colorMode);
    memset(b + o, 0, 16);
    strncpy((char *)b + o, panel_display_name(), 15); o += 16;
    memset(b + o, 0, 8);
    strncpy((char *)b + o, FW_VERSION, 7); o += 8;
    memcpy(b + o, g_uid, 8); o += 8;
    return o;
}

static void disc_announce_to(const uint8_t *dip, uint16_t dport, const uint8_t *dmac)
{
    uint8_t pkt[80];
    uint16_t n = disc_build_announce(pkt);
    disc_udp_send(BSB_S2_REG, BSB_S2_TX, dip, dport, dmac, pkt, n);
}

static void disc_announce_broadcast()
{
    uint8_t bcast[4] = { 255, 255, 255, 255 };
    uint8_t bmac[6]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    disc_announce_to(bcast, DISC_PORT, bmac);
}

// Unsolicited mDNS A-record for <g_hostLabel>.local so `ping panel-xxxxxx.local` works.
static void mdns_send_a(uint16_t txn, const uint8_t *dip, uint16_t dport, const uint8_t *dmac)
{
    uint8_t p[96];
    uint16_t o = 0;
    p[o++] = (uint8_t)(txn >> 8); p[o++] = (uint8_t)txn;
    p[o++] = 0x84; p[o++] = 0x00;                       // response, authoritative
    p[o++] = 0; p[o++] = 0;                             // questions
    p[o++] = 0; p[o++] = 1;                             // answers
    p[o++] = 0; p[o++] = 0; p[o++] = 0; p[o++] = 0;     // ns / ar
    uint8_t nlen = (uint8_t)strlen(g_hostLabel);
    p[o++] = nlen;
    memcpy(p + o, g_hostLabel, nlen); o += nlen;
    p[o++] = 5; memcpy(p + o, "local", 5); o += 5;
    p[o++] = 0;
    p[o++] = 0x00; p[o++] = 0x01;                       // A
    p[o++] = 0x80; p[o++] = 0x01;                       // IN + cache flush
    p[o++] = 0; p[o++] = 0; p[o++] = 0; p[o++] = 120;   // TTL 120s
    p[o++] = 0; p[o++] = 4;
    p[o++] = g_ip[0]; p[o++] = g_ip[1]; p[o++] = g_ip[2]; p[o++] = g_ip[3];
    disc_udp_send(BSB_S3_REG, BSB_S3_TX, dip, dport, dmac, p, o);
}

static void mdns_announce()
{
    uint8_t gip[4] = { 224, 0, 0, 251 };
    uint8_t gmac[6] = { 0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB };
    mdns_send_a(0, gip, MDNS_PORT, gmac);
}

static void disc_drain_sock(uint8_t bsb_reg, uint8_t bsb_rx, bool discovery)
{
    uint16_t rsr = w6_rd16(bsb_reg, 0x0224);
    if (rsr < 8) return;
    uint16_t rd = w6_rd16(bsb_reg, 0x0228);
    uint8_t hdr[8];
    w6_read(bsb_rx, rd, hdr, 8);
    uint16_t dsize = (uint16_t)(((hdr[0] & 0x07) << 8) | hdr[1]);
    uint8_t sip[4] = { hdr[2], hdr[3], hdr[4], hdr[5] };
    uint16_t sport = (uint16_t)((hdr[6] << 8) | hdr[7]);
    uint8_t payload[256];
    uint16_t n = dsize > sizeof(payload) ? sizeof(payload) : dsize;
    if (n && (uint32_t)8 + dsize <= rsr)
        w6_read(bsb_rx, (uint16_t)(rd + 8), payload, n);
    rd = (uint16_t)(rd + 8 + dsize);
    w6_wr16(bsb_reg, 0x0228, rd);
    w6_wr8(bsb_reg, 0x0010, 0x40);                      // RECV

    if (discovery) {
        if (n >= 5 && payload[0] == VPXD_MAGIC0 && payload[1] == VPXD_MAGIC1 &&
            payload[2] == VPXD_MAGIC2 && payload[3] == VPXD_MAGIC3 &&
            payload[4] == VPXD_PROBE) {
            disc_announce_to(sip, sport, nullptr);
        }
        return;
    }

    // mDNS: reply to queries that mention our hostname (QR bit clear).
    if (n >= 12 && (payload[2] & 0x80) == 0) {
        uint8_t nlen = (uint8_t)strlen(g_hostLabel);
        for (uint16_t i = 12; i + 1 + nlen < n; i++) {
            if (payload[i] == nlen && memcmp(payload + i + 1, g_hostLabel, nlen) == 0) {
                uint16_t txn = (uint16_t)((payload[0] << 8) | payload[1]);
                uint8_t gip[4] = { 224, 0, 0, 251 };
                uint8_t gmac[6] = { 0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB };
                mdns_send_a(txn, gip, MDNS_PORT, gmac);
                break;
            }
        }
    }
}

static void disc_init()
{
    disc_open_udp(BSB_S2_REG, DISC_PORT, false);
    disc_open_udp(BSB_S3_REG, MDNS_PORT, true);
    disc_announce_broadcast();
    mdns_announce();
    g_discLastAnnounce = millis();
}

static void disc_poll()
{
    disc_drain_sock(BSB_S2_REG, BSB_S2_RX, true);
    disc_drain_sock(BSB_S3_REG, BSB_S3_RX, false);
    if (millis() - g_discLastAnnounce >= 3000) {
        g_discLastAnnounce = millis();
        disc_announce_broadcast();
        mdns_announce();
    }
}
