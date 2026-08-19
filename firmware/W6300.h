// W6300.h
// Part of the dmd_icnd1065l_w6300_pioq sketch. It is #included (in dependency order) from the
// main .ino, so this is still ONE translation unit - no forward declarations needed.
// W6300 QUAD QSPI-PIO transport + socket register access helpers.
#pragma once

static PIO w6pio = pio1;
static int w6sm, w6dma_out, w6dma_in;
static uint w6off;

static void w6_reset() { pinMode(W6_RST, OUTPUT); digitalWrite(W6_RST, LOW); delay(2); digitalWrite(W6_RST, HIGH); delay(50); }

static void w6_pio_init()
{
    gpio_init(W6_CS); gpio_set_dir(W6_CS, GPIO_OUT); gpio_put(W6_CS, 1);

    w6off = pio_add_program(w6pio, &w6qspi_program);
    w6sm = pio_claim_unused_sm(w6pio, true);

    pio_sm_config c = w6qspi_program_get_default_config(w6off);
    sm_config_set_out_pins(&c, W6_IO0, 4);
    sm_config_set_in_pins(&c, W6_IO0);
    sm_config_set_set_pins(&c, W6_IO0, 4);
    sm_config_set_sideset(&c, 1, false, false);
    sm_config_set_sideset_pins(&c, W6_SCLK);
    sm_config_set_in_shift(&c, false, true, 8);
    sm_config_set_out_shift(&c, false, true, 8);
    sm_config_set_clkdiv(&c, W6_CLKDIV);

    hw_set_bits(&w6pio->input_sync_bypass, IO_MASK);
    pio_sm_set_config(w6pio, w6sm, &c);
    pio_sm_set_consecutive_pindirs(w6pio, w6sm, W6_SCLK, 1, true);

    for (uint p = W6_IO0; p <= W6_IO3; p++) {
        gpio_set_function(p, GPIO_FUNC_PIO1);
        gpio_set_pulls(p, false, true);
        gpio_set_input_hysteresis_enabled(p, true);
    }

    pio_sm_exec(w6pio, w6sm, pio_encode_set(pio_pins, 1));

    w6dma_out = dma_claim_unused_channel(true);
    w6dma_in = dma_claim_unused_channel(true);
}

static inline void w6_frame_start()
{
    gpio_set_function(W6_IO0, GPIO_FUNC_PIO1);
    gpio_set_function(W6_IO1, GPIO_FUNC_PIO1);
    gpio_set_function(W6_IO2, GPIO_FUNC_PIO1);
    gpio_set_function(W6_IO3, GPIO_FUNC_PIO1);
    gpio_set_function(W6_SCLK, GPIO_FUNC_PIO1);
    gpio_pull_down(W6_SCLK);
    gpio_put(W6_CS, 0);
}
static inline void w6_frame_end() { gpio_put(W6_CS, 1); }

// Quad command buffer: opcode spread 1-bit-per-nibble (instruction stays 1-bit-serial on IO0),
// then address (4-bit wide) + 1 dummy. 7 bytes total. (WIZnet mk_cmd_buf quad.)
static inline uint16_t mk_cmd(uint8_t *d, uint8_t op, uint16_t addr)
{
    d[0] = (uint8_t)((((op >> 7) & 1) << 4) | (((op >> 6) & 1) << 0));
    d[1] = (uint8_t)((((op >> 5) & 1) << 4) | (((op >> 4) & 1) << 0));
    d[2] = (uint8_t)((((op >> 3) & 1) << 4) | (((op >> 2) & 1) << 0));
    d[3] = (uint8_t)((((op >> 1) & 1) << 4) | (((op >> 0) & 1) << 0));
    d[4] = (uint8_t)(addr >> 8);
    d[5] = (uint8_t)addr;
    d[6] = 0;
    return 7;
}

static void w6_pio_readb(uint8_t op, uint16_t addr, uint8_t *rx, uint16_t rxlen)
{
    uint8_t cmd[8] = { 0 };
    uint16_t clen = mk_cmd(cmd, op, addr);

    pio_sm_set_enabled(w6pio, w6sm, false);
    pio_sm_set_wrap(w6pio, w6sm, w6off, w6off + w6qspi_offset_read_bits_end - 1);
    pio_sm_clear_fifos(w6pio, w6sm);
    pio_sm_set_pindirs_with_mask(w6pio, w6sm, IO_MASK, IO_MASK);
    pio_sm_restart(w6pio, w6sm);
    pio_sm_clkdiv_restart(w6pio, w6sm);
    pio_sm_put(w6pio, w6sm, (uint32_t)clen * 2 - 1);   // quad: loop_cnt = 2 nibbles/byte
    pio_sm_exec(w6pio, w6sm, pio_encode_out(pio_x, 32));
    pio_sm_put(w6pio, w6sm, (uint32_t)rxlen - 1);
    pio_sm_exec(w6pio, w6sm, pio_encode_out(pio_y, 32));
    pio_sm_exec(w6pio, w6sm, pio_encode_jmp(w6off));

    dma_channel_abort(w6dma_out);
    dma_channel_abort(w6dma_in);
    dma_channel_config oc = dma_channel_get_default_config(w6dma_out);
    channel_config_set_transfer_data_size(&oc, DMA_SIZE_8);
    channel_config_set_bswap(&oc, true);
    channel_config_set_dreq(&oc, pio_get_dreq(w6pio, w6sm, true));
    dma_channel_configure(w6dma_out, &oc, &w6pio->txf[w6sm], cmd, clen, true);

    dma_channel_config ic = dma_channel_get_default_config(w6dma_in);
    channel_config_set_transfer_data_size(&ic, DMA_SIZE_8);
    channel_config_set_bswap(&ic, true);
    channel_config_set_dreq(&ic, pio_get_dreq(w6pio, w6sm, false));
    channel_config_set_write_increment(&ic, true);
    channel_config_set_read_increment(&ic, false);
    dma_channel_configure(w6dma_in, &ic, rx, &w6pio->rxf[w6sm], rxlen, true);

    pio_sm_set_enabled(w6pio, w6sm, true);
    dma_channel_wait_for_finish_blocking(w6dma_out);
    dma_channel_wait_for_finish_blocking(w6dma_in);
    pio_sm_set_enabled(w6pio, w6sm, false);
    pio_sm_exec(w6pio, w6sm, pio_encode_mov(pio_pins, pio_null));
}

static void w6_pio_writeb(uint8_t op, uint16_t addr, const uint8_t *tx, uint16_t txlen)
{
    uint8_t cmd[8] = { 0 };
    uint16_t clen = mk_cmd(cmd, op, addr);
    uint32_t total = (uint32_t)txlen + clen;

    pio_sm_set_enabled(w6pio, w6sm, false);
    pio_sm_set_wrap(w6pio, w6sm, w6off, w6off + w6qspi_offset_write_bits_end - 1);
    pio_sm_clear_fifos(w6pio, w6sm);
    pio_sm_set_pindirs_with_mask(w6pio, w6sm, IO_MASK, IO_MASK);
    pio_sm_restart(w6pio, w6sm);
    pio_sm_clkdiv_restart(w6pio, w6sm);
    pio_sm_put(w6pio, w6sm, total * 2 - 1);
    pio_sm_exec(w6pio, w6sm, pio_encode_out(pio_x, 32));
    pio_sm_put(w6pio, w6sm, 0);
    pio_sm_exec(w6pio, w6sm, pio_encode_out(pio_y, 32));
    pio_sm_exec(w6pio, w6sm, pio_encode_jmp(w6off));

    dma_channel_abort(w6dma_out);
    dma_channel_config oc = dma_channel_get_default_config(w6dma_out);
    channel_config_set_transfer_data_size(&oc, DMA_SIZE_8);
    channel_config_set_bswap(&oc, true);
    channel_config_set_dreq(&oc, pio_get_dreq(w6pio, w6sm, true));

    pio_sm_set_enabled(w6pio, w6sm, true);
    dma_channel_configure(w6dma_out, &oc, &w6pio->txf[w6sm], cmd, clen, true);
    dma_channel_wait_for_finish_blocking(w6dma_out);
    dma_channel_configure(w6dma_out, &oc, &w6pio->txf[w6sm], tx, txlen, true);
    dma_channel_wait_for_finish_blocking(w6dma_out);

    uint32_t txstall = 1u << (PIO_FDEBUG_TXSTALL_LSB + w6sm);
    w6pio->fdebug = txstall;
    while (!(w6pio->fdebug & txstall)) tight_loop_contents();

    pio_sm_set_pindirs_with_mask(w6pio, w6sm, 0, IO_MASK);
    pio_sm_exec(w6pio, w6sm, pio_encode_mov(pio_pins, pio_null));
    pio_sm_set_enabled(w6pio, w6sm, false);
}

// Opcode: MOD[1:0]=10 (quad) in bits [7:6], RWB in bit5, BSB in [4:0].
static void w6_read(uint8_t bsb, uint16_t addr, uint8_t *buf, int n)
{
    w6_frame_start();
    w6_pio_readb((uint8_t)(QSPI_MODE_QUAD | (bsb & 0x1F)), addr, buf, (uint16_t)n);
    w6_frame_end();
}
static void w6_write(uint8_t bsb, uint16_t addr, const uint8_t *buf, int n)
{
    w6_frame_start();
    w6_pio_writeb((uint8_t)(QSPI_MODE_QUAD | (1 << 5) | (bsb & 0x1F)), addr, buf, (uint16_t)n);
    w6_frame_end();
}
static uint8_t w6_rd8(uint8_t bsb, uint16_t a) { uint8_t v; w6_read(bsb, a, &v, 1); return v; }
static void    w6_wr8(uint8_t bsb, uint16_t a, uint8_t v) { w6_write(bsb, a, &v, 1); }
static uint16_t w6_rd16(uint8_t bsb, uint16_t a) { uint8_t b[2]; w6_read(bsb, a, b, 2); return (uint16_t)((b[0] << 8) | b[1]); }
static void    w6_wr16(uint8_t bsb, uint16_t a, uint16_t v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; w6_write(bsb, a, b, 2); }
static inline void w6_read_rx(uint16_t off, uint8_t *buf, int n) { w6_read(BSB_S0_RX, off, buf, n); }
