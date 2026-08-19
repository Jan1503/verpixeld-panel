#pragma once
// 74HC595 (TC7559) walking-one-hot row mux, driven by a dedicated PIO SM.
//
// Pin map (SET base = mux_list[0] = 26, count = 3):
//   bit0 = A = CLK  (GPIO 26)
//   bit1 = B = LAT  (GPIO 27)
//   bit2 = C = SDIN (GPIO 28)
//
// X = remaining zero-shifts before the next inject-1; Y = nRows-1 (reload).
// After init_mux seeds the 595 with a 1 at row 0, X and Y are both nRows-1 so
// the next nRows-1 IRQs shift zeros (rows 1..nRows-1) and then inject a 1 (row 0).
//
// Lives on pio2 — pio0 instruction memory is full, pio1 is the W6300 QSPI.
// clock_cnt2_mux raises irq 0 (CPU scan, unchanged) and irq 4 (this SM).
// Sync is `wait 1 irq next 4` (RP2350: PIO2 NEXT == PIO0). WAIT 1 IRQ clears
// irq 4 only, so the CPU keeps PIO0 irq 0 / PIO0_IRQ_0 as in production.
//
// Instruction encodings are origin-0; pio_add_program relocates JMPs.

#define mux595_wrap_target 0
#define mux595_wrap        9

#define mux595_instr_lat      0xe702u // set pins, 0b010 [7]  LAT, SDIN=0
#define mux595_instr_clk      0xef03u // set pins, 0b011 [15] LAT+CLK
#define mux595_instr_idle     0xe000u // set pins, 0
#define mux595_instr_lat_sdin 0xe706u // set pins, 0b110 [7]  LAT+SDIN
#define mux595_instr_clk_sdin 0xef07u // set pins, 0b111 [15] LAT+SDIN+CLK

static const uint16_t mux595_program_instructions[] = {
    //     .wrap_target
    0x20dc, //  0: wait  1 irq next 4     ; PIO0 irq 4 (clock_cnt2_mux), clears it
    0x0026, //  1: jmp   !x, 6            ; X==0 -> inject 1
    mux595_instr_lat,      //  2: shift 0
    mux595_instr_clk,      //  3
    mux595_instr_idle,     //  4
    0x0040, //  5: jmp   x--, 0
    mux595_instr_lat_sdin, //  6: inject 1
    mux595_instr_clk_sdin, //  7
    mux595_instr_idle,     //  8
    0xa022, //  9: mov   x, y
            //     .wrap
};

#if !PICO_NO_HARDWARE
static const struct pio_program mux595_program = {
    .instructions = mux595_program_instructions,
    .length = 10,
    .origin = -1,
    .pio_version = 1,
};

static inline pio_sm_config mux595_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + mux595_wrap_target, offset + mux595_wrap);
    return c;
}

static inline pio_sm_config mux595_program_init(PIO pio, uint sm, uint offset, uint pin_clk)
{
    pio_sm_config c = mux595_program_get_default_config(offset);
    sm_config_set_clkdiv(&c, 1.0f);
    sm_config_any_pins(pio, sm, &c, SET_PINS, pin_clk, 3);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, false);
    return c;
}

// One 595 shift while the SM is halted (used to flush/seed in init_mux).
static inline void mux595_exec_shift(PIO pio, uint sm, bool sdin)
{
    pio_sm_exec(pio, sm, sdin ? mux595_instr_lat_sdin : mux595_instr_lat);
    pio_sm_exec(pio, sm, sdin ? mux595_instr_clk_sdin : mux595_instr_clk);
    pio_sm_exec(pio, sm, mux595_instr_idle);
}
#endif

// clock_cnt2 + extra `irq nowait 4` so the mux SM can wait without stealing irq 0.
#define clock_cnt2_mux_wrap_target 2
#define clock_cnt2_mux_wrap        11

static const uint16_t clock_cnt2_mux_program_instructions[] = {
    0x80a0, //  0: pull   block
    0xa0c7, //  1: mov    isr, osr
            //     .wrap_target
    0xc000, //  2: irq    nowait 0        ; CPU scan ISR (PIO0_IRQ_0)
    0xc004, //  3: irq    nowait 4        ; mux SM on pio2
    0xe042, //  4: set    y, 2
    0x6028, //  5: out    x, 8
    0x6001, //  6: out    pins, 1
    0x2020, //  7: wait   0 pin, 0
    0x20a0, //  8: wait   1 pin, 0
    0x0047, //  9: jmp    x--, 7
    0x0085, // 10: jmp    y--, 5
    0xa0e6, // 11: mov    osr, isr
            //     .wrap
};

#if !PICO_NO_HARDWARE
static const struct pio_program clock_cnt2_mux_program = {
    .instructions = clock_cnt2_mux_program_instructions,
    .length = 12,
    .origin = -1,
    .pio_version = 0,
};

static inline pio_sm_config clock_cnt2_mux_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + clock_cnt2_mux_wrap_target, offset + clock_cnt2_mux_wrap);
    return c;
}

static inline pio_sm_config clock_cnt2_mux_program_init(PIO pio, uint sm, uint offset,
    int clk_div, int in_pins_base, int out_pins_base)
{
    pio_sm_config c = clock_cnt2_mux_program_get_default_config(offset);
    sm_config_set_clkdiv(&c, clk_div);
    sm_config_set_in_pins(&c, in_pins_base);
    sm_config_any_pins(pio, sm, &c, OUT_PINS, out_pins_base, 1);
    sm_config_set_out_shift(&c, true, false, 24);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_exec(pio, sm, offset);
    return c;
}
#endif
