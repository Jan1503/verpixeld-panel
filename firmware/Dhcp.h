// Dhcp.h
// Part of the dmd_icnd1065l_w6300_pioq sketch. It is #included (in dependency order) from the
// main .ino, so this is still ONE translation unit - no forward declarations needed.
// DHCP client (DISCOVER/OFFER/REQUEST/ACK) on socket 0.
#pragma once

static uint16_t dhcp_build(uint8_t *b, uint32_t xid, uint8_t type, const uint8_t *reqIp, const uint8_t *srvId)
{
    memset(b, 0, 300);
    b[0] = 1; b[1] = 1; b[2] = 6;                       // op=BOOTREQUEST, htype=eth, hlen=6
    b[4] = (uint8_t)(xid >> 24); b[5] = (uint8_t)(xid >> 16); b[6] = (uint8_t)(xid >> 8); b[7] = (uint8_t)xid;
    b[10] = 0x80;                                       // flags: request broadcast reply
    memcpy(b + 28, W6_MAC, 6);                          // chaddr
    b[236] = 0x63; b[237] = 0x82; b[238] = 0x53; b[239] = 0x63; // magic cookie
    uint16_t o = 240;
    b[o++] = 53; b[o++] = 1; b[o++] = type;             // 53: DHCP message type
    b[o++] = 61; b[o++] = 7; b[o++] = 1; memcpy(b + o, W6_MAC, 6); o += 6; // 61: client id
    { uint8_t nlen = (uint8_t)strlen(g_hostLabel);      // 12: hostname so the DHCP list shows panel-xxxxxx
      if (nlen) { b[o++] = 12; b[o++] = nlen; memcpy(b + o, g_hostLabel, nlen); o += nlen; } }
    if (reqIp) { b[o++] = 50; b[o++] = 4; memcpy(b + o, reqIp, 4); o += 4; } // 50: requested IP
    if (srvId) { b[o++] = 54; b[o++] = 4; memcpy(b + o, srvId, 4); o += 4; } // 54: server id
    b[o++] = 55; b[o++] = 3; b[o++] = 1; b[o++] = 3; b[o++] = 6; // 55: param request (mask, router, dns)
    b[o++] = 255;                                       // end
    return o < 300 ? 300 : o;                           // BOOTP minimum length
}

static bool dhcp_send(const uint8_t *data, uint16_t len)
{
    uint8_t bcast[4] = { 255, 255, 255, 255 };
    uint8_t bmac[6]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    w6_write(BSB_S0_REG, 0x0118, bmac, 6);              // Sn_DHAR = broadcast MAC
    w6_write(BSB_S0_REG, 0x0120, bcast, 4);             // Sn_DIPR = 255.255.255.255
    w6_wr16(BSB_S0_REG, 0x0140, 67);                    // Sn_DPORTR = 67 (DHCP server)
    uint16_t wr = w6_rd16(BSB_S0_REG, 0x020C);          // Sn_TX_WR
    w6_write(BSB_S0_TX, wr, data, len);                 // write into the TX buffer block
    w6_wr16(BSB_S0_REG, 0x020C, (uint16_t)(wr + len));  // advance write pointer
    w6_wr8(BSB_S0_REG, 0x0010, 0x20);                   // SEND
    uint32_t t = millis(); uint8_t ir = 0;
    while (!((ir = w6_rd8(BSB_S0_REG, 0x0020)) & 0x18) && millis() - t < 400) {} // SENDOK(0x10)|TIMEOUT(0x08)
    w6_wr8(BSB_S0_REG, 0x0028, 0x18);                   // clear via Sn_IRCLR
    if (!(ir & 0x10)) Serial.printf("[dhcp]   send ir=0x%02X (%s)\n", ir, (ir & 0x08) ? "TIMEOUT" : "no-irq");
    return (ir & 0x10) != 0;
}

static bool dhcp_recv(uint8_t *pkt, uint32_t xid, uint8_t wantType,
                      uint8_t *ip, uint8_t *srvId, uint8_t *mask, uint8_t *gw, uint32_t timeoutMs)
{
    uint32_t t = millis();
    while (millis() - t < timeoutMs) {
        uint16_t rsr = w6_rd16(BSB_S0_REG, 0x0224);
        if (rsr >= 8) {
            uint16_t rd = w6_rd16(BSB_S0_REG, 0x0228);
            uint8_t h[8]; w6_read_rx(rd, h, 8);
            uint16_t dsize = (uint16_t)(((h[0] & 0x07) << 8) | h[1]);
            if (dsize >= 240 && dsize <= 512 && (uint32_t)8 + dsize > rsr)
                continue;                               // full datagram not in the RX buffer yet -> wait
            if (dsize >= 240 && dsize <= 512) w6_read_rx((uint16_t)(rd + 8), pkt, dsize);
            rd = (uint16_t)(rd + 8 + dsize);
            w6_wr16(BSB_S0_REG, 0x0228, rd);
            w6_wr8(BSB_S0_REG, 0x0010, 0x40);           // RECV
            if (dsize >= 240 && dsize <= 512 && pkt[0] == 2 &&
                pkt[4] == (uint8_t)(xid >> 24) && pkt[5] == (uint8_t)(xid >> 16) &&
                pkt[6] == (uint8_t)(xid >> 8) && pkt[7] == (uint8_t)xid) {
                uint8_t type = 0;
                memcpy(ip, pkt + 16, 4);                // yiaddr
                uint16_t o = 240;
                while (o < dsize && pkt[o] != 255) {
                    uint8_t opt = pkt[o++];
                    if (opt == 0) continue;             // pad
                    uint8_t l = pkt[o++];
                    if (opt == 53 && l >= 1) type = pkt[o];
                    else if (opt == 54 && l >= 4) memcpy(srvId, pkt + o, 4);
                    else if (opt == 1 && l >= 4) memcpy(mask, pkt + o, 4);
                    else if (opt == 3 && l >= 4) memcpy(gw, pkt + o, 4);
                    o += l;
                }
                if (type == wantType) return true;
            }
        }
    }
    Serial.printf("[dhcp]   timeout: final RSR=%u Sn_IR=0x%02X Sn_SR=0x%02X\n",
                  w6_rd16(BSB_S0_REG, 0x0224), w6_rd8(BSB_S0_REG, 0x0020), w6_rd8(BSB_S0_REG, 0x0030));
    return false;
}

static bool dhcp_run(uint8_t *ip, uint8_t *mask, uint8_t *gw)
{
    static uint8_t pkt[512];
    uint32_t xid = 0x57365100u ^ micros();
    uint8_t zero4[4] = { 0, 0, 0, 0 };
    w6_write(BSB_COMMON, 0x4138, zero4, 4);             // SIPR = 0 (unconfigured for DHCP)
    w6_write(BSB_COMMON, 0x4130, zero4, 4);             // GAR = 0
    w6_write(BSB_COMMON, 0x4134, zero4, 4);             // SUBR = 0

    w6_wr8(BSB_S0_REG, 0x0000, 0x02);                   // Sn_MR = UDP4
    w6_wr16(BSB_S0_REG, 0x0114, 68);                    // Sn_PORT = 68
    w6_wr8(BSB_S0_REG, 0x0010, 0x01);                   // OPEN
    uint32_t t = millis();
    while (w6_rd8(BSB_S0_REG, 0x0030) != 0x22 && millis() - t < 200) {}
    delay(300);                                         // let the socket/RX settle before the first send
    Serial.printf("[dhcp] open, Sn_SR=0x%02X (0x22=UDP)\n", w6_rd8(BSB_S0_REG, 0x0030));

    uint8_t srvId[4] = { 0 }, m[4] = { 255, 255, 255, 0 }, g[4] = { 0 };
    for (int attempt = 0; attempt < 6; attempt++) {
        Serial.printf("[dhcp] attempt %d: DISCOVER\n", attempt + 1);
        dhcp_send(pkt, dhcp_build(pkt, xid, 1, nullptr, nullptr));   // DISCOVER
        uint8_t off[4];
        if (!dhcp_recv(pkt, xid, 2, off, srvId, m, g, 2000)) { Serial.println(F("[dhcp]   no OFFER")); continue; }
        Serial.printf("[dhcp]   OFFER ip=%u.%u.%u.%u srv=%u.%u.%u.%u\n",
                      off[0], off[1], off[2], off[3], srvId[0], srvId[1], srvId[2], srvId[3]);
        dhcp_send(pkt, dhcp_build(pkt, xid, 3, off, srvId));         // REQUEST
        uint8_t ack[4];
        if (!dhcp_recv(pkt, xid, 5, ack, srvId, m, g, 2000)) { Serial.println(F("[dhcp]   no ACK")); continue; }
        Serial.println(F("[dhcp]   ACK received"));
        memcpy(ip, ack, 4); memcpy(mask, m, 4); memcpy(gw, g, 4);
        w6_wr8(BSB_S0_REG, 0x0010, 0x10);               // CLOSE
        t = millis(); while (w6_rd8(BSB_S0_REG, 0x0030) && millis() - t < 100) {}
        return true;
    }
    w6_wr8(BSB_S0_REG, 0x0010, 0x10);                   // CLOSE
    return false;
}

