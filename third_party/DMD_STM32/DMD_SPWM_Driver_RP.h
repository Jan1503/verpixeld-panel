#pragma once
#ifndef DMD_RGB_SPWM_DRIVER_RP_H
#define DMD_RGB_SPWM_DRIVER_RP_H
/*--------------------------------------------------------------------------------------
 This file is a part of the DMD_STM32 library.

 DMD_STM32  - STM32 & RP2040 port of DMD.h library

 This module is an attempt to provide an experimental support
 of the new generation HUB75 LED panel drivers - so-called PWM/SPWM type.

 The file includes a one base class DMD_RGB_SPWM_DRIVER_BASE and a five 
 children class for specific LED drivers.

 The chips supported:

	* DMD_RGB_DP3264 class
		+ DP3264
	
	* DMD_RGB_ICN2055 class
		+ ICND2055	

	* DMD_RGB_FM6373 class
		+ FM6373

 	* DMD_RGB_FM6353 class
		+ FM6353
		+ ICN2153

	* DMD_RGB_FM6363 class
		+ FM6363

	* DMD_RGB_ICN1065 class
		+ ICND1065

 https://github.com/board707/DMD_STM32
 Dmitry Dmitriev (c) 2019-2026

 (for RP2040 boards )
/--------------------------------------------------------------------------------------*/

#if (defined(ARDUINO_ARCH_RP2040))
#include "DMD_RGB.h"
#include "dmd_spwm.pio.h"
#if defined(DMD_PIO_MUX) && DMD_PIO_MUX
#include "hardware/pio_instructions.h"
#include "mux595.pio.h"
#if !PICO_RP2350
#error DMD_PIO_MUX needs RP2350 (pio2 + WAIT IRQ NEXT)
#endif
#endif


#define ADD_CONFIG_REGS(arr) this->add_config_regs((arr), sizeof(arr) / sizeof((arr)[0]))

/*--------------------------------------------------------------------------------------*/
/*
 * Base class for all SPWM drivers  ( specialized as DP3264 type)
 *
 * Key features: The clock source for the internal driver PWM is the DCLK (CLK) signal.
 * Row switching is synchronized based on GCLK (OE) pulses after fixed number of CLK pulses.
 */
/*--------------------------------------------------------------------------------------*/


template <int... Pars>
class DMD_RGB_SPWM_DRIVER_BASE : public DMD_RGB<Pars...>
{
public:
	DMD_RGB_SPWM_DRIVER_BASE(uint8_t *mux_list, byte _pin_nOE, byte _pin_SCLK, uint8_t *pinlist,
							 byte panelsWide, byte panelsHigh, bool d_buf = false) : 
							 DMD_RGB<Pars...>
							 (mux_list, _pin_nOE, _pin_SCLK, pinlist, panelsWide, panelsHigh, false)
	{
	}

	~DMD_RGB_SPWM_DRIVER_BASE()
	{
		if (this->config_registers != NULL)
		{
			free(this->config_registers);
		}
		free(dma_buffer);
	}

    virtual void init(uint16_t scan_interval = 200) override
	{
		
		DMD_RGB_BASE::init(scan_interval);
		// Two buffer for async DMA transfer, with one row length each
		dma_buffer = (uint8_t*)malloc(this->x_len * 2);
		dma_buffs[0] = dma_buffer;
		dma_buffs[1] = dma_buffer + this->x_len;
	}

 	
	// load user data buffer to matrix
	void swapBuffers(bool copy) override
	{
		this->refresh_greyscale_data();
	}

	uint8_t shiftColorBrightnessDown(uint8_t shift) {
		if (shift > this->gclk_bits - min_gclk) {
			dimming_factor = this->gclk_bits - min_gclk;
		}
		else dimming_factor = shift;
		return dimming_factor;
	}

	// Set the max panel DCLK in MHz. MUST be called BEFORE init() because the PIO
	// clock dividers are computed there. Higher = faster data upload = shorter blank
	// window per field = higher field rate = less flicker (FM6363 tolerates up to 30 MHz).
	void setMaxClkFreq(uint8_t mhz) { if (mhz) max_clk_freq = mhz; }

	// Diagnostic knob: override the greyscale bit-width the chip clocks (default set per chip in init()).
	// Call AFTER init()/applyPanelRegs(). Higher = longer GCLK train => longer LSB pulse (same brightness
	// ratio) but lower field rate. Only useful if the driver chip actually accepts that width; otherwise
	// levels look corrupted -> revert. Range clamped to [min_gclk .. 14].
	void setGclkBits(uint8_t bits) { if (bits >= min_gclk && bits <= 14) gclk_bits = bits; }

	// Set the pixel-data shift clock (DCLK) to an EXACT MHz via the PIO's fractional divider, bypassing
	// the coarse integer steps of setMaxClkFreq (at 133 MHz sysclk those are only ~26.6/13.3/8.9 MHz).
	// Call AFTER init(). Safe to call at runtime (brief glitch) for live sweeping. Returns the achieved MHz.
	float setDataClkMHz(float mhz)
	{
		if (mhz <= 0.0f) return 0.0f;
		float div = (float)CYCLES_PER_MICROSECOND / (mhz * 5.0f);
		if (div < 1.0f) div = 1.0f;
		if (div > 65535.0f) div = 65535.0f;
		// data_transfer() re-inits the data SM from pio_config every row, so the divider MUST live in
		// pio_config or it gets clobbered. Update it there, then apply immediately for good measure.
		sm_config_set_clkdiv(&this->pio_config, div);
		pio_sm_set_clkdiv(this->pio, this->sm_data, div);
		pio_sm_clkdiv_restart(this->pio, this->sm_data);
		return (float)CYCLES_PER_MICROSECOND / (div * 5.0f);
	}
protected:
    volatile bool oe_scan_flag = false;
	volatile bool oe_scan_res = false;
	uint8_t max_clk_freq = 15;  // 15 MHz (most HUB75 panels has max CLK about 20-25MHz )

    // Most driver chips only receives the lower 14 bits of 16 bits transmitted data.
	// Some drivers needs 13 or 12 bits.
	uint8_t gclk_bits = 14;
	uint8_t dimming_factor = 0;
	const uint8_t min_gclk = 8;

	// Clk_Lat SM
    uint8_t sm_clk_lat = 0;
	uint16_t clk_lat_prog_offs = 0;
	pio_sm_config clk_lat_pio_config;
	bool clk_lat_out_pin_is_lat = true;
	
    // Clk counter SM
	uint8_t sm_clk_cnt;
	uint16_t clk_cnt_prog_offs = 0;
	pio_sm_config clk_cnt_pio_config;

	dma_channel_config dma_c;
	volatile uint8_t mux_counter = 0;
	uint8_t* dma_buffer;
	uint8_t* dma_buffs[2];
	

	uint16_t *config_registers;
	uint8_t conf_reg_cnt;
    bool clk_after_upload = true;

	// Hooks for multi-field drivers. Default = single field: upload ALL nRows
	// scanlines starting at scanline 0 (unchanged behaviour for every stock driver).
	// A 2-field driver (e.g. FM6363 128x128) overrides these to upload only the
	// current field's block of scanlines.
	virtual uint8_t  tx_row_count() { return this->nRows; }
	virtual uint16_t tx_row_base()  { return 0; }

    virtual void load_config_regs(uint16_t *conf_reg) {}

	// Copy chip config registers values to the class data
    void add_config_regs(uint16_t *cfg_regs, uint16_t regs_cnt)
	{

		if (this->config_registers != NULL)
		{
			free(this->config_registers);
		}
		this->conf_reg_cnt = regs_cnt;
		this->config_registers = (uint16_t *)malloc(this->conf_reg_cnt * sizeof(uint16_t));
		memcpy(this->config_registers, cfg_regs, (this->conf_reg_cnt * sizeof(uint16_t)));
	}

	virtual void spwm_chip_init() 
	{
		 this->clearScreen(true);
         for (uint8_t i = 0; i < this->conf_reg_cnt; i++) 
		 { 
			this->refresh_greyscale_data();
			delay(30);
		}
	}
	
	// Hold LAT line HIGH while generating given number of CLK pulses
    void send_latches(uint16_t latches)
	{
		// reinit pio program to use LAT as OUT pin
		if (! this->clk_lat_out_pin_is_lat) {
			sm_config_any_pins(this->pio, this->sm_clk_lat, &(this->clk_lat_pio_config), OUT_PINS, this->pin_DMD_SCLK, 1);
			dmd_out_program_reinit(this->pio, this->sm_clk_lat, this->clk_lat_prog_offs, &(this->clk_lat_pio_config));
			this->clk_lat_out_pin_is_lat = true;
		}
		send_clocks(latches, 1);
       
	}

    // The same as previous, but for OE line
	void send_oe(uint16_t latches)
	{
		// reinit pio program to use OE as OUT pin
		if ( this->clk_lat_out_pin_is_lat) {
			sm_config_any_pins(this->pio, this->sm_clk_lat, &(this->clk_lat_pio_config), OUT_PINS, this->pin_DMD_nOE, 1);
			dmd_out_program_reinit(this->pio, this->sm_clk_lat, this->clk_lat_prog_offs, &(this->clk_lat_pio_config));
			this->clk_lat_out_pin_is_lat = false;
		}
		send_clocks(latches, 1);
       
	}

	// Just send a given number of CLK pulses
	void send_clocks(uint16_t clocks, uint8_t out_state = 0)
	{
		if (!clocks) return;
		
		while (clocks)
		{
			pio_sm_put_blocking(this->pio, this->sm_clk_lat, out_state); 
			// cleat TX_STALL flag
			pio_sm_tx_fifo_stall_clear(this->pio, this->sm_clk_lat);
			clocks--;
			
		}
      
        // wait SM stall again after processing last pulse
        while(! pio_sm_is_tx_fifo_stall(this->pio, this->sm_clk_lat));
		

        // make LAT/OE line low after finishing
		if (out_state) {
			pio_sm_exec(this->pio, this->sm_clk_lat,  0xa003); // LAT - LOW
			
		}
		
	}

    // VSYNC = 3 clocks LAT pulse
	virtual void send_vsync()
	{
		if (! this->clk_lat_out_pin_is_lat) {
			sm_config_any_pins(this->pio, this->sm_clk_lat, &(this->clk_lat_pio_config), OUT_PINS, this->pin_DMD_SCLK, 1);
			dmd_out_program_reinit(this->pio, this->sm_clk_lat, this->clk_lat_prog_offs, &(this->clk_lat_pio_config));
			this->clk_lat_out_pin_is_lat = true;
		}
		pio_sm_put_blocking(this->pio, this->sm_clk_lat, 0);
		this->send_latches(3);
	}

	void initialize_timers(voidFuncPtr handler) override
	{

		// Adjust pio clk divider to not overflow panel CLK > 15 MHz
		// will be overwritten below, leave for compatibility
		if (CYCLES_PER_MICROSECOND / (4 * this->pio_clkdiv) > MAX_PANEL_CLK)
		{
			this->pio_clkdiv = 1 + CYCLES_PER_MICROSECOND / (4 * MAX_PANEL_CLK);
		}

		// PIO and DMA config
		pio_dma_init();

		// configure IRQ handler for mux change
		irq_set_exclusive_handler(PIO0_IRQ_0, handler);
		irq_set_priority(PIO0_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY); // scan timing must not be preempted
		irq_set_enabled(PIO0_IRQ_0, true);
	}

	// CLK_CNT SM configuration differs in specific drivers, so we need virtual method
	virtual void add_pio_prg3()
	{
		this->pio_clkdiv = 1;
		this->sm_clk_cnt = pio_claim_unused_sm(this->pio, true);
		this->clk_cnt_prog_offs = pio_add_program(this->pio, &clock_cnt2_program);
		this->clk_cnt_pio_config = dmd_clk_cnt_program_init(this->pio, this->sm_clk_cnt, this->clk_cnt_prog_offs, this->pio_clkdiv,
															this->pin_DMD_CLK, this->pin_DMD_nOE);
	}

	virtual void pio_dma_init() override
	{
		// pio configs

		// Data out SM config
		// a PIO machine to output data to RGB pins
        this->pio_clkdiv = 1 + CYCLES_PER_MICROSECOND/(max_clk_freq * 5);
		this->sm_data = pio_claim_unused_sm(this->pio, true);
		this->data_prog_offs = pio_add_program(this->pio, &dmd_out_spwm_program);
		this->pio_config = dmd_out_spwm_program_init(this->pio, this->sm_data, this->data_prog_offs, this->pio_clkdiv,
							 this->data_pins[0], this->data_pins_cnt, this->pin_DMD_SCLK, this->pin_DMD_CLK);

        // CLK_LAT SM config
		// a PIO machine to generate CLK, LAT & OE pulses
		this->pio_clkdiv = 1 + CYCLES_PER_MICROSECOND/(max_clk_freq * 2);
		this->sm_clk_lat = pio_claim_unused_sm(this->pio, true);
		this->clk_lat_prog_offs = pio_add_program(this->pio, &clock_latches_program);
		this->clk_lat_pio_config = clock_latches_prg_init(this->pio, this->sm_clk_lat, this->clk_lat_prog_offs, this->pio_clkdiv,
                  this->pin_DMD_SCLK, this->pin_DMD_CLK);
		this->clk_lat_out_pin_is_lat = true;

        // CLK_CNT SM config
		// a PIO machine to generate DCLK & GCLK pulse trains
		this->add_pio_prg3();		  

	    pio_set_irq0_source_enabled(this->pio, pis_interrupt0, true);

		// DMA config
		this->dma_chan = dma_claim_unused_channel(true);
		this->dma_c = dma_channel_get_default_config(this->dma_chan);
		channel_config_set_transfer_data_size(&dma_c, DMA_SIZE_8); // read by one byte
		channel_config_set_read_increment(&dma_c, true);  		 
		channel_config_set_dreq(&dma_c, this->sm_data + DREQ_PIO0_TX0); // requested by PIO
		channel_config_set_ring(&dma_c, false, 4);	// (1 << 4) = wrap data by 16 bytes to write a config registers repeatedly

		dma_channel_configure(
			this->dma_chan,
			&dma_c,
			&pio0_hw->txf[this->sm_data], // Write address (only need to set this once)
			NULL,						  // Don't provide a read address yet
			this->x_len,				  // Write x_len bytes than stop
			false						  // Don't start yet
		);

	}						 
   
   // stop GCLK generation before loading new data
   virtual void stop_GCLK()
	{
		// if GCLK in progress
		if (this->oe_scan_flag)
		{
			noInterrupts();
			// set stopping GCLK (OE) flags
			this->oe_scan_flag = false;
			this->oe_scan_res = true;
			interrupts();

			// wait for GCLK stop flag change
			while (this->oe_scan_res){};
			
		}

        // stop CLK_LAT and CLK_CNT SMs
		pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);
		//  move program counter to first line to set CLK low     
		pio_sm_exec(this->pio, this->sm_clk_lat,  this->clk_lat_prog_offs);  
		pio_sm_set_enabled(this->pio, this->sm_clk_cnt, false);
		// restart CLK_LAT to use it in send_latches() 
		pio_sm_init(this->pio, this->sm_clk_lat, this->clk_lat_prog_offs, &(this->clk_lat_pio_config));
		pio_sm_set_enabled(this->pio, this->sm_clk_lat, true);
		delayMicroseconds(10);
		
	}
   
   // main method for refresh greyscale data
	virtual void refresh_greyscale_data() 
	{

		// wait for 0-th row and stop the GCLK generation
		this->stop_GCLK();
		
        
		// load config registers
		channel_config_set_ring(&(this->dma_c), false, 4);	// (1 << 4) = wrap data by 16 bytes to write a config registers repeatedly
		dma_channel_set_config(this->dma_chan, &(this->dma_c), false); // reload DMA config after wrap change
		this->load_config_regs(this->config_registers);

		
		// load new grayscale data
        this->data_transfer();
		
		// wait for finishing upload and stop data SM
		while (!pio_interrupt_get(this->pio, 7));
		pio_interrupt_clear(this->pio, 7);
		pio_sm_set_enabled(this->pio, this->sm_data, false);

		// start CLK generation if needed
		if (this->clk_after_upload ) {
			pio_sm_init(this->pio, this->sm_clk_lat, this->clk_lat_prog_offs +clock_latches_wrap + 1, &(this->clk_lat_pio_config));
  	        pio_sm_set_enabled(this->pio, this->sm_clk_lat, true);
		}
		
	}
	
	// extract 4bit color data
	virtual uint32_t expand_planes(volatile uint8_t *ptr3) 
	{
		uint8_t b = 0;
		uint32_t res = 0;
		for (byte i = 0; i < 4; i++)
		{
			if (i < this->nPlanes)
			{
				b = *ptr3;
				// b = *ptr3;
				ptr3 += this->displ_len;
			}
			res = (res << 8) | b ;
		}
		return res;
	}


	virtual void start_DCLK()
	{
		// Start clock_counter SM
		dmd_out_program_reinit(this->pio, this->sm_clk_cnt, this->clk_cnt_prog_offs, &this->clk_cnt_pio_config);
        
		// GCLK/DCLK generation by PIO machine controlled by parameter below.
		// Format of `control_par' parameter:
		// clock loop:
		// MUX IRQ | ==== d1 ==== | OE pulse = d2 | ==== d3 ==== | MUX IRQ |
		// bits of control par: (8 MS bits - duration by CLKs, 1 LS bit - is OE switched ON (1) or OFF (0))
		// 26:19 - d3 , 18 - 0
		// 17:10 - d2, 9 - 1
		// 8:1 - d1, 0 - 0
		uint32_t control_par = 0;
		uint8_t d1 = 28, d2 = 4, d3 = 96;
		
		// TYPE_595 mux needs more time for switching
		if (this->Mux->mux_type == DMD_MUX_TYPE_SHIFTREG) {
			d1 = 38; d2 = 4; d3 = 86;
		}
		
		control_par = d3-1;
		control_par = (control_par << 9) | (1 << 8) | (d2-1);
		control_par = (control_par << 9) | (d1-1);
		pio_sm_put_blocking(this->pio, this->sm_clk_cnt, control_par);
		this->oe_scan_flag = true;
	}

    // upload greyscale to the panel
	virtual void data_transfer() 
	{
		this->buffptr = this->matrixbuff[1 - this->backindex];
		volatile uint8_t *ptr = this->buffptr;
		volatile uint8_t *ptr2 = this->buffptr;
		const uint8_t num_sect = this->x_len / 16;
		
	    uint8_t buff_select = 0;
		uint8_t* forward_ptr = dma_buffs[buff_select];
	    uint8_t* backward_ptr = dma_buffs[1 - buff_select];
	   
		bool first_run = true;

		// RP2040 code ================
		// RGB data transfer by PIO machine controlled by two parameters below
		// The first is a number of transfers (x_len - 1)
		// The second is a number of x_len - Latches
		uint32_t control_par1 = this->x_len - 1;
		uint32_t control_par2 = this->x_len - 2;
		
		// The postion of greyscale MSB can varied from 14 to 12th depending of chip
		// You can additionally shift it down as low as 8th bit by <dimming_factor>
		// to adjust color and brightness
		uint8_t gclk_msb = this->gclk_bits - this->dimming_factor;

		// generate 12 clocks OE pulse to start the data group (see the datasheet)
		//this->send_oe(12);  moved to init_mux()
		
		//  iterate each scan line (multi-field drivers upload only their block)
		const uint8_t  tx_cnt  = this->tx_row_count();
		const uint16_t tx_base = this->tx_row_base();
		for (uint8_t y = 0; y < tx_cnt; y++)
		{
			// data pointer to next scan line data
			ptr = this->buffptr + (tx_base + y) * (this->x_len);

			// =========================
			// copy xlen bytes for DMA

			// each driver controls 16 leds
			// so the data should be loaded by 16 pixels
           
			for (uint8_t x = 0; x < 16; x++)
			{

				ptr2 = ptr + x;
				
 				uint16_t i = 0;
				for (uint8_t sect = 0; sect < num_sect; sect++)
				{
					// get 4 color bits
					// and put them to dma_buffer
					uint32_t greyscale = this->expand_planes(ptr2);  
					// Zerofill upper bits above greyscale MSB (14-12th ) 
					memset(backward_ptr + i,(uint8_t)0,16-gclk_msb); 
					memcpy(backward_ptr + i + 16-gclk_msb, (uint8_t*)&greyscale, sizeof(greyscale));  
					
					// zerofill the remaining bits to make a total of 16
					uint8_t lsb = 0;
					memset(backward_ptr + i + 20-gclk_msb, lsb, gclk_msb -4);
					ptr2+=16;
					i+=16;
				}
			

			// swap DMA buffers
			buff_select = 1 - buff_select;
			forward_ptr = dma_buffs[buff_select];
	    	backward_ptr = dma_buffs[1 - buff_select];
			

			if ( first_run) {
				first_run = false;
				// reconfigure DMA for continuous transfer of x_len bytes
				channel_config_set_ring(&(this->dma_c), false, 0);	// cancel wrapping data for 16 bytes
		        dma_channel_set_config(this->dma_chan, &(this->dma_c), false); // reload DMA config after wrap change
				
				// start CLK_CNT SM (for compatibility with FM6363 code, do nothing for other drivers)
				pio_sm_set_enabled(this->pio, this->sm_clk_cnt, true);
				
			}
			
			else
			{

				// Wait for finishing previous transfer and uploads
				dma_channel_wait_for_finish_blocking(this->dma_chan);
				while (!pio_interrupt_get(this->pio, 7));
				pio_interrupt_clear(this->pio, 7);
			}

			// Restart DATA SM
			pio_sm_set_enabled(this->pio, this->sm_data, false);
			dmd_out_program_reinit(this->pio, this->sm_data, this->data_prog_offs, &this->pio_config);
			
			// Put a `control_par' parameters
			pio_sm_put_blocking(this->pio, this->sm_data, (control_par2<<16)|control_par1);
			
			// Start DMA transfer from `buffptr` buffer
			dma_channel_set_read_addr(this->dma_chan, forward_ptr, true);
			
			}
		}

	// Wait for finishing last transfer 
	dma_channel_wait_for_finish_blocking(this->dma_chan);
	
	}

	// send config register data to RGB lines
	void send_to_allRGB(uint16_t data, uint16_t latches) override
	{

		// to use DMA ring mode we need to align data to 16 bytes
		static uint8_t reg[16] __attribute__((aligned(16))) = {0};

		// Convert 16bit config value to byte array
		// high bits to 0xff, low bits to 0
		for (int i = 0; i < 16; i++)
		{
			if (data & 0x8000)
			{
				reg[i] = 0xff;
			}
			else
			{
				reg[i] = 0x00;
			}
			data <<= 1;
		}

		// Restart DATA SM
		dmd_out_program_reinit(this->pio, this->sm_data, this->data_prog_offs, &this->pio_config);

		// RGB data transfer by PIO machine controlled by two parameters below
		// The first is a number of transfers (x_len - 1)
		// The second is a number of x_len - Latches

		// Put a `control_par' parameters
		uint32_t control_par1 = this->x_len - 1;
		uint32_t control_par2 = this->x_len - (latches + 1);
		pio_sm_put_blocking(this->pio, this->sm_data, (control_par2 << 16) | control_par1);

		// Start DMA transfer of `reg` buffer
		dma_channel_set_read_addr(this->dma_chan, reg, true);

		// Wait for finishing DMA transfer and uploads
		dma_channel_wait_for_finish_blocking(this->dma_chan);
		while (!pio_interrupt_get(this->pio, 7))
			;
		pio_interrupt_clear(this->pio, 7);
	}

	virtual void init_mux()
	{
		this->Mux->set_mux(0);
		this->row = 1;
		this->send_oe(12);
		this->send_clocks(88);
	}

	// PIO IRQ0 handler
	// Normal mode - Switch to next row
	// in case <oe_scan_flag == false> - waits for row =0 and stops GCLK/DCLK pulses
	// set <oe_scan_res = false> after stopping GCLK
	// RAM-resident (__not_in_flash_func): this is the latency-critical per-row scan handler; keeping it
	// out of flash prevents a second core's XIP traffic from jittering the row timing (= column walk).
	void __not_in_flash_func(scan_dmd)() override
	{
		pio_interrupt_clear(this->pio, 0);
		// switch the row
		this->Mux->set_mux(this->row);

		if (this->row == 0)
		{
			if (this->oe_scan_flag == false)
			{
				// disable GCLK generation
				pio_sm_set_enabled(this->pio, this->sm_clk_cnt, false);
				pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);
				this->oe_scan_res = false;
				return;
			}
		}

		this->row++;
		if (this->row >= this->nRows)
			this->row = 0;
	}
};

/*--------------------------------------------------------------------------------------*/
// DP3264 driver class
/*--------------------------------------------------------------------------------------*/
template <int... Pars>
class DMD_RGB_DP3264 : public DMD_RGB_SPWM_DRIVER_BASE<Pars...>
{

public:
	DMD_RGB_DP3264(uint8_t *mux_list, byte _pin_nOE, byte _pin_SCLK, uint8_t *pinlist,
				   byte panelsWide, byte panelsHigh, bool d_buf = false)
		: DMD_RGB_SPWM_DRIVER_BASE<Pars...>
		(mux_list, _pin_nOE, _pin_SCLK, pinlist, panelsWide, panelsHigh, d_buf)
	{
	}

	void init(uint16_t scan_interval = 200) override
	{
		// MSB greyscale position for color bits
		this->gclk_bits = 13;
		
		DMD_RGB_SPWM_DRIVER_BASE<Pars...>::init(scan_interval);

		uint16_t conf_3264[] = {0x1100, 0x020f, 0x033f, 0x043f, 0x0504, 0x0642, 0x0700, 0x08BF, 0x0960, 0x0ABE, 0x0B8B, 0x0C88, 0x0D12};

		conf_3264[1] = 0x0200 | (this->nRows - 1); /// panel scan
		ADD_CONFIG_REGS(conf_3264);
		this->spwm_chip_init();
	}

protected:
	void load_config_regs(uint16_t *conf_reg) override
	{
		// send next config register in each call
		static uint8_t r = this->conf_reg_cnt;
		r++;
		if (r >= this->conf_reg_cnt)
		{

			r = 0;
		}
      
		this->send_vsync(); 
		this->send_clocks(16);
		this->send_latches(14); // pre-active command
		this->init_mux();
		pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);
		
		// config and start clk_cnt SM
		this->start_DCLK();

		// send one config register 
		this->send_to_allRGB(conf_reg[r], 5); 
		
	}


};

/*--------------------------------------------------------------------------------------*/
// ICN2055 driver class
/*--------------------------------------------------------------------------------------*/
template <int... Pars>
class DMD_RGB_ICN2055 : public DMD_RGB_SPWM_DRIVER_BASE<Pars...>
{

public:
	DMD_RGB_ICN2055(uint8_t *mux_list, byte _pin_nOE, byte _pin_SCLK, uint8_t *pinlist,
					byte panelsWide, byte panelsHigh, bool d_buf = false) : 
					DMD_RGB_SPWM_DRIVER_BASE<Pars...>
					(mux_list, _pin_nOE, _pin_SCLK, pinlist, panelsWide, panelsHigh, d_buf)
	{
	}

	void init(uint16_t scan_interval = 200) override
	{

	
		DMD_RGB_SPWM_DRIVER_BASE<Pars...>::init(scan_interval);
		
		// MSB greyscale position for color bits
		this->gclk_bits = 13;
		
		uint16_t icn2055_conf[] = {
			0x021f, 0x033f, 0x0400, 0x0507, 0x0603, 0x0720, 0x0820, 0x0908, 0x0a08, 0x0b00,
			0x0c08, 0x0d01, 0x0e04, 0x0f01, 0x1082, 0x1121, 0x1201, 0x17f0, 0x181f, 0x1950,
			0x1a1f, 0x1b10, 0x1ccf, 0x1d0a, 0x1e4c, 0x1f20, 0x2008, 0x2101, 0x221c};

		icn2055_conf[0] = 0x200 | (this->nRows - 1); /// panel scan

		ADD_CONFIG_REGS(icn2055_conf);
		this->spwm_chip_init();
	}

protected:
	void load_config_regs(uint16_t *conf_reg) override
	{
		// send next config register in each call
		static uint8_t r = this->conf_reg_cnt;
		r++;
		if (r >= this->conf_reg_cnt)
		{

			r = 0;
		}

		this->send_vsync(); // vsync
		this->send_clocks(8);
		this->send_latches(11); // pre-active command
		this->send_clocks(8);
		this->send_latches(14); // pre-active command
		this->send_clocks(8);
		this->init_mux();
		pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);
		
		// config and start clk_cnt SM
		this->start_DCLK();
		
		// in order to send config data we need 0x00AA 0x01AA before
		// and 0x0055,0x0155 after config value
		this->send_to_allRGB(0x00aa, 5);
		this->send_to_allRGB(0x01aa, 5);
		this->send_to_allRGB(conf_reg[r], 5); // send config register
		this->send_to_allRGB(0x0055, 5);
		this->send_to_allRGB(0x0155, 5);
		
	}
};
/*--------------------------------------------------------------------------------------*/
// FM6373 driver class
/*--------------------------------------------------------------------------------------*/
template <int... Pars>
class DMD_RGB_FM6373 : public DMD_RGB_SPWM_DRIVER_BASE<Pars...>
{

public:
	DMD_RGB_FM6373(uint8_t *mux_list, byte _pin_nOE, byte _pin_SCLK, uint8_t *pinlist,
				   byte panelsWide, byte panelsHigh, bool d_buf = false) : 
				  DMD_RGB_SPWM_DRIVER_BASE<Pars...>
				   (mux_list, _pin_nOE, _pin_SCLK, pinlist, panelsWide, panelsHigh, d_buf)
	{
	}

	void init(uint16_t scan_interval = 200) override
	{

		DMD_RGB_SPWM_DRIVER_BASE<Pars...>::init(scan_interval);

		uint16_t fm6373_conf[] = {
			0x021f, 0x033f, 0x0402, 0x0507, 0x0603, 0x0720, 0x0820, 0x0900, 0x0a00, 0x0b00,
			0x0c01, 0x0d01, 0x0e04, 0x0f01, 0x10c2, 0x1121, 0x1201, 0x17f0, 0x181f, 0x1900,
			0x1a1f, 0x1b10, 0x1cc1, 0x1d0a, 0x1e42, 0x1f04, 0x2008, 0x2101, 0x221c};

		fm6373_conf[0] = 0x200 | (this->nRows - 1); /// panel scan
		ADD_CONFIG_REGS(fm6373_conf);
		this->spwm_chip_init();
	}

protected:
	void load_config_regs(uint16_t *conf_reg) override
	{
		// send next config register in each call
		static uint8_t r = this->conf_reg_cnt;
		r++;
		if (r >= this->conf_reg_cnt)
		{

			r = 0;
		}

		this->send_vsync(); // vsync
		this->send_clocks(8);
		this->send_latches(11); // pre-active command
		this->send_clocks(8);
		this->send_latches(14); // pre-active command
		this->send_clocks(8);
		this->init_mux();
		pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);
		
		// config and start clk_cnt SM
		this->start_DCLK();
		
		// in order to send config data we need 0x00AA 0x01AA before
		// and 0x0055,0x0155 after config value
		this->send_to_allRGB(0x00aa, 5);
		this->send_to_allRGB(0x01aa, 5);
		this->send_to_allRGB(conf_reg[r], 5); // send config register
		this->send_to_allRGB(0x0055, 5);
		this->send_to_allRGB(0x0155, 5);
	}
};

/*--------------------------------------------------------------------------------------*/
// SM16380SH driver class
/*--------------------------------------------------------------------------------------*/
template <int... Pars>
class DMD_RGB_SM16380SH : public DMD_RGB_SPWM_DRIVER_BASE<Pars...>
{

public:
	DMD_RGB_SM16380SH(uint8_t *mux_list, byte _pin_nOE, byte _pin_SCLK, uint8_t *pinlist,
				   byte panelsWide, byte panelsHigh, bool d_buf = false) : 
				  DMD_RGB_SPWM_DRIVER_BASE<Pars...>
				   (mux_list, _pin_nOE, _pin_SCLK, pinlist, panelsWide, panelsHigh, d_buf)
	{
	}

	void init(uint16_t scan_interval = 200) override
	{

		DMD_RGB_SPWM_DRIVER_BASE<Pars...>::init(scan_interval);

		uint16_t sm16380sh_conf[] = {
			0x021f, 0x0300, 0x0400, 0x0500, 0x0600, 0x0750, 0x0800, 0x0900, 0x0a02, 0x0b0c,
			0x0c08, 0x0d00, 0x0e05, 0x0f00, 0x1000, 0x1100, 0x1200, 0x1300, 0x1414, 0x1500,
			0x1630, 0x1700, 0x1801, 0x1904,
			0x1a03, 0x1b14, 0x1c12, 0x1d00, 0x1e00, 0x1f0c};

		sm16380sh_conf[0] = 0x200 | (this->nRows - 1); /// panel scan
		ADD_CONFIG_REGS(sm16380sh_conf);
		this->spwm_chip_init();
	}

protected:
	void load_config_regs(uint16_t *conf_reg) override
	{
		// send next config register in each call
		static uint8_t r = this->conf_reg_cnt;
		r++;
		if (r >= this->conf_reg_cnt)
		{

			r = 0;
		}

		this->send_vsync(); // vsync
		this->send_clocks(8);
		this->send_latches(11); // pre-active command
		this->send_clocks(8);
		this->send_latches(14); // pre-active command
		this->send_clocks(8);
		this->init_mux();
		pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);
		
		// config and start clk_cnt SM
		this->start_DCLK();
		
		// in order to send config data we need 0x00AA 0x01AA before
		// and 0x0055,0x0155 after config value
		this->send_to_allRGB(0x00aa, 5);
		this->send_to_allRGB(0x01aa, 5);
		this->send_to_allRGB(conf_reg[r], 5); // send config register
		this->send_to_allRGB(0x0055, 5);
		this->send_to_allRGB(0x0155, 5);
	}
};

/*--------------------------------------------------------------------------------------*/
// FM6353 driver class
/*--------------------------------------------------------------------------------------*/
template<int... Pars> 
class DMD_RGB_FM6353 : public DMD_RGB_SPWM_DRIVER_BASE<Pars...>
{

public:
	DMD_RGB_FM6353(uint8_t *mux_list, byte _pin_nOE, byte _pin_SCLK, uint8_t *pinlist,
				   byte panelsWide, byte panelsHigh, bool d_buf = false) : 
				  DMD_RGB_SPWM_DRIVER_BASE<Pars...>
				   (mux_list, _pin_nOE, _pin_SCLK, pinlist, panelsWide, panelsHigh, d_buf)
	{
	}

	void init(uint16_t scan_interval = 200) override
	{

		DMD_RGB_SPWM_DRIVER_BASE<Pars...>::init(scan_interval);
		
		this->clk_after_upload  = false;
		
		uint16_t conf_6353[] = {0x0008, 0x1f70, 0x6707, 0x40f7, 0x0040};

		// Config value for 4 latches depends on number of scans
		conf_6353[1] = ((this->nRows - 1) << 8) | (conf_6353[1] & 0xFF);
		ADD_CONFIG_REGS(conf_6353);
	}

protected:
	void load_config_regs(uint16_t *conf_reg) override
	{
		// send next config register in each call
		static uint8_t r = this->conf_reg_cnt;
		r++;
		if (r >= this->conf_reg_cnt)
		{

			r = 0;
		}

        this->send_latches(14); // pre-active command, 14 clks LAT pulses
		delayMicroseconds(1);
		this->send_latches(12); // enable all output, 12 clks LAT
		delayMicroseconds(1);
		this->send_vsync();		// vsync, 3 clks LAT
		delayMicroseconds(1);
		this->start_DCLK();
		this->send_latches(14);	
		delayMicroseconds(1);		
		pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);			 
		
		// send one config register 
		this->send_to_allRGB(conf_reg[r], r * 2 + 2); 
}


    // load GCLK SM program instead of CLK_CNT prog
	void add_pio_prg3() override {
		this->pio_clkdiv = 1 + CYCLES_PER_MICROSECOND/(this->max_clk_freq * 2);
		this->sm_clk_cnt = pio_claim_unused_sm(this->pio, true);
		this->clk_cnt_prog_offs = pio_add_program(this->pio, &gclk_program);
		this->clk_cnt_pio_config = gclk_program_init(this->pio, this->sm_clk_cnt, this->clk_cnt_prog_offs, this->pio_clkdiv,
        this->pin_DMD_nOE);
	}
   
   
	void start_DCLK() override
	{
		// Start clock_counter SM
		dmd_out_program_reinit(this->pio, this->sm_clk_cnt, this->clk_cnt_prog_offs, &this->clk_cnt_pio_config);
        
		// Put a `control_par' parameter
		// Format of `control_par' parameter:
		// clock loop:
		// d1 OE pulses | IRQ | d2 idle pulses
		this->row = 1;
		uint32_t control_par = 0;
		uint16_t d1 = 138, d2 = 30;
		control_par = d2-1;
		control_par = (control_par << 16) | (d1-1);
		pio_sm_put_blocking(this->pio, this->sm_clk_cnt, control_par);
		this->oe_scan_flag = true;
	}
};


/*--------------------------------------------------------------------------------------*/
// FM6353 driver class
/*--------------------------------------------------------------------------------------*/
template <int ...Pars>
class DMD_RGB_FM6363 : public DMD_RGB_SPWM_DRIVER_BASE<Pars...>
{

public:
	DMD_RGB_FM6363(uint8_t *mux_list, byte _pin_nOE, byte _pin_SCLK, uint8_t *pinlist,
				   byte panelsWide, byte panelsHigh, bool d_buf = false) : 
				  DMD_RGB_SPWM_DRIVER_BASE<Pars...>
				   (mux_list, _pin_nOE, _pin_SCLK, pinlist, panelsWide, panelsHigh, d_buf)
	{
	}

	void init(uint16_t scan_interval = 200) override
	{

		DMD_RGB_SPWM_DRIVER_BASE<Pars...>::init(scan_interval);
		
		this->clk_after_upload  = true;
		// MSB greyscale position for color bits
		this->gclk_bits = 13;
		
		uint16_t conf_6363[] = {0x7e08, 0x0fb0, 0xe6fc, 0x60b6,  0x5a70};

		// Config value for 4 latches depends on number of scans
		conf_6363[1] = ((this->nRows - 1) << 8) | (conf_6363[1] & 0xFF);
		ADD_CONFIG_REGS(conf_6363);
	}

protected:
	void load_config_regs(uint16_t *conf_reg) override
	{
		
        this->send_latches(14); // pre-active command, 14 clks LAT pulses
		delayMicroseconds(1);
		this->send_latches(12); // enable all output, 12 clks LAT
		delayMicroseconds(1);
		this->send_vsync();		// vsync, 3 clks LAT
		delayMicroseconds(1);
		
		// load all config registers 
		for (uint8_t r = 0; r < this->conf_reg_cnt; r++)
		{
			pio_sm_set_enabled(this->pio, this->sm_clk_lat, true);	
			this->send_latches(14);						  // pre-active command
			pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);	
			this->send_to_allRGB(conf_reg[r], r * 2 + 2); // send config registers
			pio_sm_set_enabled(this->pio, this->sm_data, false);
		}
		this->start_DCLK();
		
}

 
    // load GCLK SM program instead of CLK_CNT prog
	void add_pio_prg3() override {
		this->pio_clkdiv = 1 + CYCLES_PER_MICROSECOND/(this->max_clk_freq * 2);
		this->sm_clk_cnt = pio_claim_unused_sm(this->pio, true);
		this->clk_cnt_prog_offs = pio_add_program(this->pio, &gclk_program);
		this->clk_cnt_pio_config = gclk_program_init(this->pio, this->sm_clk_cnt, this->clk_cnt_prog_offs, this->pio_clkdiv,
        this->pin_DMD_nOE);
	}
   
   
	void start_DCLK() override
	{
		// Start GCLK SM
		
		// Reinit CLK_CNT SM, but not enable it.
		// It will be enabled in data_transfer method 
		// to starts of DCLK and GCLK generation synchronicly
		 pio_sm_init(this->pio, this->sm_clk_cnt, this->clk_cnt_prog_offs, &this->clk_cnt_pio_config);
         pio_sm_exec(this->pio, this->sm_clk_cnt, this->clk_cnt_prog_offs);
		// Put a `control_par' parameter
		// Format of `control_par' parameter:
		// clock loop:
		// d1 OE pulses | IRQ | d2 idle pulses
		this->row = 1;
		uint32_t control_par = 0;
		uint16_t d1 = 74, d2 = 20;
		control_par = d2-1;
		control_par = (control_par << 16) | (d1-1);
		pio_sm_put_blocking(this->pio, this->sm_clk_cnt, control_par);
		this->oe_scan_flag = true;
	}
};

/*--------------------------------------------------------------------------------------*/
// FM6363 128x128 driver class  (2-FIELD time-multiplexed)
//
// WHY a dedicated class:
//   This particular 128x128 panel uses an FM6363 whose greyscale RAM only holds
//   32 scanlines, while the 74HC595/ICND2018 row chain addresses 64 rows
//   (each 595 output P drives physical rows P and P+64 = the R0/R1 upper/lower lanes).
//   A naive single-field upload of 64 scanlines WRAPS in the chip RAM (the classic
//   "+32 doubling"). The panel therefore MUST be driven as TWO time-multiplexed
//   fields per frame (identical mechanism to the working Raspberry-Pi driver):
//     * Field 0: image rows [0,32) (upper lane) + [64,96)  (lower lane), 595 pos 0..31
//     * Field 1: image rows [32,64)(upper lane) + [96,128) (lower lane), 595 pos 32..63
//
//   Geometry: use a 1/64-scan, multiplex=1 template  ->  RGB128x128_FM6363 = 33,128,128,64,0
//   so nRows=64, x_len=128, and the internal frame buffer is a clean 64-scanline layout
//   where scanline s already holds image row s (upper) + s+64 (lower). We then upload it
//   in two 32-scanline halves and walk the 595 one-hot to the matching field offset.
//
//   The stock DMD_Mux595 walks the one-hot INCREMENTALLY (injects a 1 only when row==0
//   and shifts 0s afterwards) so it cannot jump to offset 32. We therefore drive the 3
//   mux GPIOs (A=DCLK, B=RCK, C=SDIN) directly and (re)load the one-hot at the correct
//   field offset at the start of every field sweep.
/*--------------------------------------------------------------------------------------*/
template <int ...Pars>
class DMD_RGB_FM6363_128 : public DMD_RGB_FM6363<Pars...>
{
public:
	DMD_RGB_FM6363_128(uint8_t *mux_list, byte _pin_nOE, byte _pin_SCLK, uint8_t *pinlist,
				   byte panelsWide, byte panelsHigh, bool d_buf = false) :
				  DMD_RGB_FM6363<Pars...>
				   (mux_list, _pin_nOE, _pin_SCLK, pinlist, panelsWide, panelsHigh, d_buf)
	{
		// A=DCLK(clock), B=RCK(latch), C=SDIN(data) - same order as DMD_Mux595
		mux_A = mux_list[0];
		mux_B = mux_list[1];
		mux_C = mux_list[2];
	}

	void init(uint16_t scan_interval = 200) override
	{
		DMD_RGB_SPWM_DRIVER_BASE<Pars...>::init(scan_interval);

		this->clk_after_upload = true;
		this->gclk_bits = 13;

		// Tell the chip it has ROWS_PER_FIELD (=32) scanlines, NOT nRows (=64).
		uint16_t conf_6363[] = {0x7e08, 0x0fb0, 0xe6fc, 0x60b6, 0x5a70};
		conf_6363[1] = ((ROWS_PER_FIELD - 1) << 8) | (conf_6363[1] & 0xFF);
		this->add_config_regs(conf_6363, sizeof(conf_6363) / sizeof(conf_6363[0]));

		// We drive the 595 mux pins directly (bypassing DMD_Mux595::set_mux).
		gpio_init(mux_A); gpio_set_dir(mux_A, GPIO_OUT); gpio_put(mux_A, 0);
		gpio_init(mux_B); gpio_set_dir(mux_B, GPIO_OUT); gpio_put(mux_B, 0);
		gpio_init(mux_C); gpio_set_dir(mux_C, GPIO_OUT); gpio_put(mux_C, 0);
	}

	// Each call uploads ONE field and flips to the other for the next call.
	// The panel self-refreshes the just-uploaded field (via GCLK) until the next
	// call, so the sketch loop must call this continuously (both fields alternate).
	void swapBuffers(bool copy) override
	{
		// FLICKER LEVER: the chip config registers are static, so we only re-send
		// them every <config_period> uploads. Every other upload skips the 5-register
		// blast -> much shorter "blank" window while GCLK is stopped -> less flicker.
		if (config_ctr == 0) send_config_now = true;
		else                 send_config_now = false;
		if (++config_ctr >= config_period) config_ctr = 0;

		this->cur_field ^= 1;
		this->refresh_greyscale_data();
	}

	// tune the flicker/robustness trade-off from the sketch:
	//   period = 1  -> resend config every field (most robust, most flicker)
	//   period big  -> resend rarely (least flicker) - verify the panel stays stable
	void setConfigPeriod(uint16_t period) { config_period = (period < 1) ? 1 : period; }

protected:
	static const uint8_t FIELDS = 2;
	static const uint8_t ROWS_PER_FIELD = 32;
	volatile uint8_t cur_field = 1;   // ^=1 on first swap -> starts at field 0
	uint8_t mux_A = 0, mux_B = 0, mux_C = 0;

	uint16_t config_period = 1;
	uint16_t config_ctr = 0;
	volatile bool send_config_now = true;

	// Lean per-field arming sequence. Sends the 5 config registers only when due
	// (see swapBuffers); otherwise just does the pre-active/vsync + restarts GCLK.
	void load_config_regs(uint16_t *conf_reg) override
	{
		this->send_latches(14); // pre-active command
		delayMicroseconds(1);
		this->send_latches(12); // enable all output
		delayMicroseconds(1);
		this->send_vsync();     // vsync
		delayMicroseconds(1);

		if (send_config_now)
		{
			for (uint8_t r = 0; r < this->conf_reg_cnt; r++)
			{
				pio_sm_set_enabled(this->pio, this->sm_clk_lat, true);
				this->send_latches(14);
				pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);
				this->send_to_allRGB(conf_reg[r], r * 2 + 2);
				pio_sm_set_enabled(this->pio, this->sm_data, false);
			}
		}
		this->start_DCLK();
	}

	// upload only the current field's 32 scanlines
	uint8_t  tx_row_count() override { return ROWS_PER_FIELD; }
	uint16_t tx_row_base()  override { return (uint16_t)cur_field * ROWS_PER_FIELD; }

	static inline void mux_delay()
	{
		for (volatile uint8_t i = 0; i < 4; i++) { __asm volatile("nop"); }
	}

	// one 595 shift: put `bit` on SDIN, pulse DCLK, latch with RCK (mirrors DMD_Mux595)
	inline void mux_shift(bool bit)
	{
		gpio_put(mux_B, 1);              // RCK high
		gpio_put(mux_C, bit ? 1 : 0);    // SDIN
		mux_delay();
		gpio_put(mux_A, 1);              // DCLK high
		mux_delay();
		gpio_put(mux_A, 0);
		gpio_put(mux_B, 0);              // RCK low
	}

	// place a single one-hot at absolute 595 position `pos` (0..63)
	inline void mux_load_onehot(uint8_t pos)
	{
		mux_shift(1);
		for (uint8_t k = 0; k < pos; k++) mux_shift(0);
	}

	// same as FM6363 but start scanning at row 0 (so the one-hot injects cleanly)
	void start_DCLK() override
	{
		pio_sm_init(this->pio, this->sm_clk_cnt, this->clk_cnt_prog_offs, &this->clk_cnt_pio_config);
		pio_sm_exec(this->pio, this->sm_clk_cnt, this->clk_cnt_prog_offs);
		this->row = 0;
		uint32_t control_par = 0;
		uint16_t d1 = 74, d2 = 20;
		control_par = d2 - 1;
		control_par = (control_par << 16) | (d1 - 1);
		pio_sm_put_blocking(this->pio, this->sm_clk_cnt, control_par);
		this->oe_scan_flag = true;
	}

	// Row scan (PIO IRQ): walk the 595 one-hot inside the current field's window.
	void scan_dmd() override
	{
		pio_interrupt_clear(this->pio, 0);

		if (this->row == 0)
			mux_load_onehot(cur_field * ROWS_PER_FIELD); // (re)seat one-hot at field offset
		else
			mux_shift(0);                                // walk to next physical row

		if (this->row == 0)
		{
			if (this->oe_scan_flag == false)
			{
				pio_sm_set_enabled(this->pio, this->sm_clk_cnt, false);
				pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);
				this->oe_scan_res = false;
				return;
			}
		}

		this->row++;
		if (this->row >= ROWS_PER_FIELD) this->row = 0;
	}
};

/*--------------------------------------------------------------------------------------*/
// ICN1065 driver class
/*--------------------------------------------------------------------------------------*/
template <int ...Pars>
class DMD_RGB_ICN1065 : public DMD_RGB_SPWM_DRIVER_BASE<Pars...>
{

public:
	DMD_RGB_ICN1065(uint8_t *mux_list, byte _pin_nOE, byte _pin_SCLK, uint8_t *pinlist,
					byte panelsWide, byte panelsHigh, bool d_buf = false) : 
					DMD_RGB_SPWM_DRIVER_BASE<Pars...>
					(mux_list, _pin_nOE, _pin_SCLK, pinlist, panelsWide, panelsHigh, d_buf)
	{
	}
	  // Fast text shift is disabled for complex patterns, so we don't need the method
  	void disableFastTextShift(bool shift) override {}

	void init(uint16_t scan_interval = 200) override
	{
		DMD_RGB_SPWM_DRIVER_BASE<Pars...>::init(scan_interval);
		this->fast_Hbyte = false;
    	this->use_shift = false;
		// MSB greyscale position for color bits
		this->gclk_bits = 12;
		
		uint16_t icn1065_conf[] = {
			0x00aa, 0x01aa, 0x022a, 0x0335, 0x0412, 0x0500, 0x0601, 0x0720, 0x0c18, 0x0d01, 0x0e86, 0x0f01, //00-12
			0x1040, 0x1127, 0x1200, 0x1300, 0x1400, 0x1500, 0x1600, 0x1800, 0x1906, 0x1c60, 0x1dca, 0x1e73, //13-24
			0x1f00, 0x2000, 0x2100, 0x2200, 0x2300, 0x2400, 0x2500, 0x2600, 0x2700, 0x7000, 0x7100, 0x7200, 0x7300, 0x74A0 //25-38
			};
		icn1065_conf[2] = 0x200 | (this->nRows - 1); //Special register location is 2
		ADD_CONFIG_REGS(icn1065_conf);
		this->spwm_chip_init();
	}

protected:
  
	void load_config_regs(uint16_t *conf_reg) override
	{
		// send next config register in each call
		static uint8_t r = this->conf_reg_cnt;
		r++;
		if (r >= this->conf_reg_cnt)
		{
			r = 0;
		}

		this->send_vsync(); // vsync
		this->send_clocks(8);
		this->send_latches(11); // pre-active command
		this->send_clocks(8);
		this->send_latches(14); // pre-active command
		this->send_clocks(8);
		this->init_mux();
		pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);
		
		// config and start clk_cnt SM
		this->start_DCLK();
		
		// in order to send config data we need 0x00AA 0x01AA before
		// and 0x0055,0x0155 after config value
		this->send_to_allRGB(0x00aa, 5);
		this->send_to_allRGB(0x01aa, 5);
		this->send_to_allRGB(conf_reg[r], 5); // send config register
		this->send_to_allRGB(0x0055, 5);
		this->send_to_allRGB(0x0155, 5);
		
	}
};

/*--------------------------------------------------------------------------------------
 * HUB320 / 4-data-group variant of the ICND1065L driver (native 256x128 panel = two
 * internal 128x128 modules driven in parallel over 12 data lines).
 *
 * Only the DATA PATH is widened 6 -> 12 bits vs. the stock ICN1065 driver:
 *   - dmd_out_spwm12 PIO program (out pins,12), 16-bit autopull
 *   - DMA_SIZE_16, uint16 frame buffer (buf16) and DMA scratch (dma16_buffs)
 *   - send_to_allRGB / data_transfer / expand widened to uint16
 * Register upload, TC7559 shift-reg row mux and all timing are inherited unchanged.
 *
 * Buffer cell (uint16) bit -> group -> screen region mapping:
 *   bits 0-2  = group 1 (data pins 0-2)   = RIGHT module, upper half  (x 128-255, y 0-63)
 *   bits 3-5  = group 2 (data pins 3-5)   = RIGHT module, lower half  (x 128-255, y 64-127)
 *   bits 6-8  = group 3 (data pins 6-8)   = LEFT  module, upper half  (x 0-127,  y 0-63)
 *   bits 9-11 = group 4 (data pins 9-11)  = LEFT  module, lower half  (x 0-127,  y 64-127)
 * (RIGHT=group1/2 because the panel wired that way in the Stage-A first-light test.)
 * Wire 12 CONSECUTIVE data GPIOs; pinlist = { CLK, D0..D11 }.
 *
 * Colour depth: HUB_NPLANES greyscale bit-planes per channel (4 = default, 8 = true colour). More
 * planes just fill previously-zero slots of the fixed 16-entry greyscale window -> NO extra upload
 * time. Define HUB_NPLANES before including this header (the bridge sketch uses 8). buf16 grows to
 * nPlanes*displ_len*2 bytes (8 planes = 128 KB, fits the RP2040's 264 KB).
 *------------------------------------------------------------------------------------*/
#ifndef HUB_NPLANES
#define HUB_NPLANES 4
#endif
#ifndef HUB_DOUBLE_BUFFER
#define HUB_DOUBLE_BUFFER 0
#endif
// Number of streaming buffers when HUB_DOUBLE_BUFFER is set: 3 = triple (default, fully decoupled),
// 2 = double (half the RAM - needed for 13-bit where 3x208 KB would overflow; the RX core waits for
// the panel core to adopt each frame, which is free at the lower frame rates of high colour depth).
#ifndef HUB_NUM_BUFFERS
#define HUB_NUM_BUFFERS 3
#endif

template <int... Pars>
class DMD_RGB_ICND1065L_HUB320 : public DMD_RGB_ICN1065<Pars...>
{
public:
	DMD_RGB_ICND1065L_HUB320(uint8_t *mux_list, byte _pin_nOE, byte _pin_SCLK, uint8_t *pinlist,
							 byte panelsWide, byte panelsHigh, bool d_buf = false) :
		DMD_RGB_ICN1065<Pars...>(mux_list, _pin_nOE, _pin_SCLK, pinlist, panelsWide, panelsHigh, false)
	{
#if defined(DMD_PIO_MUX) && DMD_PIO_MUX
		mux_clk_pin = mux_list[0];
#endif
		this->nPlanes = HUB_NPLANES; // colour depth (4 or 8 bit/channel); must be <= gclk_bits (12)
		// 12-bit data -> one uint16 per buffer cell. The stock (byte) matrixbuff is left in place but
		// unused; we keep our own uint16 buffer.
		size_t bytes = (size_t)this->nPlanes * this->displ_len * sizeof(uint16_t);
#if HUB_DOUBLE_BUFFER
		// TRIPLE buffering (needs 3x RAM; RP2350 has plenty). One core uploads the FRONT buffer
		// continuously (stable display), the other RECEIVES into a FREE buffer non-stop (never stalls
		// -> no TCP window stall), and the finished buffer is published lock-free. Enable with
		// -DHUB_DOUBLE_BUFFER=1. Default off so RP2040 single-buffer sketches still fit.
		for (int i = 0; i < HUB_NUM_BUFFERS; i++) { bufs[i] = (uint16_t *)malloc(bytes); memset(bufs[i], 0, bytes); }
		frontIdx = 0; fillIdx = 1; readyIdx = -1;
		buf16 = bufs[frontIdx];
		buf16Back = bufs[fillIdx];
#else
		buf16 = (uint16_t *)malloc(bytes);
		memset(buf16, 0, bytes);
		buf16Back = buf16;
#endif
		// two DMA scratch buffers of x_len uint16 each
		dma16 = (uint16_t *)malloc((size_t)2 * this->x_len * sizeof(uint16_t));
		dma16_buffs[0] = dma16;
		dma16_buffs[1] = dma16 + this->x_len;
	}

	void init(uint16_t scan_interval = 200) override
	{
		DMD_RGB_ICN1065<Pars...>::init(scan_interval); // base init -> our pio_dma_init (12-bit)
		// GFX logical canvas (physical shift width stays WIDTH=128). Set the mode BEFORE init().
		if (cfg_landscape) {
			this->_width  = 2 * this->WIDTH;      // 256 (landscape canvas, rotated onto the panel)
			this->_height = 2 * this->pol_displ;  // 128
		} else if (cfg_stacked) {
			this->_width  = this->WIDTH;          // 128
			this->_height = 4 * this->pol_displ;  // 256
		} else {
			this->_width  = 2 * this->WIDTH;      // 256
			this->_height = 2 * this->pol_displ;  // 128
		}
	}

	// Replace the inherited ICN1065 default registers with the panel's NovaLCT set and re-upload.
	void applyPanelRegs(uint16_t *regs, uint16_t cnt)
	{
		this->add_config_regs(regs, cnt);
		this->spwm_chip_init();
	}

	// Per-colour register upload. Some ICND1065L registers carry different values per colour
	// (white balance / per-colour current, e.g. 0x19, 0x1E, 0x20). applyPanelRegs() broadcasts one
	// value to all three colour data lines and loses that; this sends independent values on the
	// three sub-pixel lines of every group. The three arrays map to the group data bits in the
	// order drawPacked() uses: d0 -> bit gbase+0, d1 -> bit gbase+1, d2 -> bit gbase+2. Which
	// physical colour each bit drives depends on the panel wiring (BGR here), so the caller assigns
	// the arrays accordingly. All three arrays must have length cnt.
	void applyPanelRegsRGB(const uint16_t *d0, const uint16_t *d1, const uint16_t *d2, uint16_t cnt)
	{
		if (cfg0) free(cfg0);
		if (cfg1) free(cfg1);
		if (cfg2) free(cfg2);
		cfg0 = (uint16_t *)malloc(cnt * sizeof(uint16_t));
		cfg1 = (uint16_t *)malloc(cnt * sizeof(uint16_t));
		cfg2 = (uint16_t *)malloc(cnt * sizeof(uint16_t));
		memcpy(cfg0, d0, cnt * sizeof(uint16_t));
		memcpy(cfg1, d1, cnt * sizeof(uint16_t));
		memcpy(cfg2, d2, cnt * sizeof(uint16_t));
		this->add_config_regs(cfg0, cnt); // keeps config_registers + conf_reg_cnt valid
		perColor = true;
		this->spwm_chip_init();
	}

	// --- Mapping tuning knobs (set before drawing; used to find the right layout on hardware) ---
	bool cfg_swap_lr = false;        // swap which data-group pair drives the left vs right module
	bool cfg_swap_ud = false;        // swap which group is the upper vs lower half within a module
	bool cfg_bgr     = false;        // swap R and B within every group (colour order)
	bool cfg_row_interleave = false; // physical rows are interleaved (row 0,1 = rowaddr0 upper/lower)
	                                 //   false = block (upper = rows 0..63, lower = 64..127)
	bool cfg_stacked = false;        // true  = 128x256 portrait: modules stacked (top = groups 3/4,
	                                 //         bottom = groups 1/2). Set BEFORE init().
	                                 // false = 256x128 side-by-side (original wrong assumption).
	bool cfg_flip_x = false;         // reverse column order within a module (physical wiring)
	bool cfg_flip_y = false;         // reverse row order within a module (physical scan starts bottom)
	bool cfg_landscape = false;      // true = expose a 256x128 LANDSCAPE canvas, rotated 90 deg onto
	                                 //        the physically-stacked 128x256 panel. Set BEFORE init().
	bool cfg_rot_cw = false;         // landscape rotation direction (false = CCW, true = CW)

	// Map a logical (x,y) to a PACKED target: cellOff (bits 0-12) | ((gbase/3) << 13). 0xFFFF = off.
	// Precompute this once per pixel (streaming receivers) to avoid the transform in the hot loop.
	uint16_t mapPixel(int x, int y)
	{
		const int PW = (int)this->WIDTH;               // 128
		const int halfH = 2 * (int)this->pol_displ;    // 128 (one module's height)
		const int logW = cfg_stacked ? PW : 2 * PW;
		const int logH = cfg_stacked ? 4 * (int)this->pol_displ : halfH;
		if (x < 0 || x >= logW || y < 0 || y >= logH) return 0xFFFF;

		int px, py;
		if (cfg_landscape) {
			if (cfg_rot_cw) { px = (halfH - 1) - y; py = x; }
			else            { px = y;               py = (2 * PW - 1) - x; }
		} else if (cfg_stacked) {
			px = x; py = y;
		} else {
			const bool right = (x >= PW);
			px = right ? (x - PW) : x;
			py = right ? (halfH + y) : y;
		}
		uint16_t col = cfg_flip_x ? (uint16_t)(PW - 1 - px) : (uint16_t)px;
		const bool bottom = (py >= halfH);
		uint16_t withinY = bottom ? (uint16_t)(py - halfH) : (uint16_t)py;
		if (cfg_flip_y) withinY = (uint16_t)((halfH - 1) - withinY);
		bool useLowBits = bottom ^ cfg_swap_lr;

		bool lower;
		uint16_t rowaddr;
		if (cfg_row_interleave) { rowaddr = (uint16_t)(withinY >> 1); lower = (withinY & 1); }
		else { lower = (withinY >= (int)this->pol_displ); rowaddr = lower ? (uint16_t)(withinY - this->pol_displ) : withinY; }
		const bool useLowerHalf = lower ^ cfg_swap_ud;
		const uint8_t gbase = (uint8_t)((useLowBits ? 0 : 6) + (useLowerHalf ? 3 : 0)); // 0/3/6/9

		uint16_t cellOff = (uint16_t)((uint32_t)rowaddr * this->WIDTH + col); // <= 8191 (13 bits)
		return (uint16_t)(cellOff | ((uint16_t)(gbase / 3) << 13));
	}

	// Fast pixel write from a precomputed packed map value + an RGB565 colour.
	inline void drawPacked(uint16_t packed, uint16_t color)
	{
		if (packed == 0xFFFF) return;
		const uint16_t cellOff = packed & 0x1FFF;
		const uint8_t gbase = (uint8_t)(((packed >> 13) & 0x3) * 3);
		uint8_t r = (color >> 12) & 0x0F;
		uint8_t g = (color >> 7)  & 0x0F;
		uint8_t b = (color >> 1)  & 0x0F;
		if (cfg_bgr) { uint8_t t = r; r = b; b = t; }
		const uint16_t mask = (uint16_t)(0x7u << gbase);
		uint16_t *cell = buf16 + cellOff;
		for (uint8_t p = 0; p < this->nPlanes; p++) {
			uint16_t v = (uint16_t)((((r >> p) & 1) << gbase) |
			                        (((g >> p) & 1) << (gbase + 1)) |
			                        (((b >> p) & 1) << (gbase + 2)));
			*cell = (uint16_t)((*cell & ~mask) | v);
			cell += this->displ_len;
		}
	}

	void drawPixel(int16_t x, int16_t y, uint16_t color) override
	{
		if (this->graph_mode == GRAPHICS_NOR) {
			if (color == this->textcolor) color = this->textbgcolor; else return;
		}
		drawPacked(mapPixel(x, y), color);
	}

	// Diagnostic/intro: set a pixel to FULL-DEPTH per-channel levels (0 .. (1<<nPlanes)-1). Unlike
	// drawPixel() (4-bit RGB565, dim on a 13-plane panel) this writes every bit-plane, so it is bright.
	// Channels map to the 3 data lines of the pixel's group (d0/d1/d2). Writes the FRONT buffer.
	void drawPixelLevelRGB(int x, int y, uint16_t r, uint16_t g, uint16_t b)
	{
		uint16_t packed = mapPixel(x, y);
		if (packed == 0xFFFF) return;
		const uint16_t cellOff = packed & 0x1FFF;
		const uint8_t  gbase   = (uint8_t)(((packed >> 13) & 0x3) * 3);
		const uint16_t mask    = (uint16_t)(0x7u << gbase);
		uint16_t *cell = buf16 + cellOff;
		for (uint8_t p = 0; p < this->nPlanes; p++) {
			uint16_t v = (uint16_t)((((r >> p) & 1) << gbase) |
			                        (((g >> p) & 1) << (gbase + 1)) |
			                        (((b >> p) & 1) << (gbase + 2)));
			*cell = (uint16_t)((*cell & ~mask) | v);
			cell += this->displ_len;
		}
	}
	void drawPixelLevel(int x, int y, uint16_t level) { drawPixelLevelRGB(x, y, level, level, level); }

	// Zero only the FRONT buffer currently being drawn into. Prefer this for boot/intro frames:
	// clear16() wipes every streaming buffer (including core1's fill target).
	void clearFront()
	{
		if (!buf16) return;
		memset(buf16, 0, (size_t)this->nPlanes * this->displ_len * sizeof(uint16_t));
	}

	// Zero the whole frame buffer(s). Call once per frame BEFORE a run of drawPackedFast (which ORs).
	void clear16()
	{
		size_t bytes = (size_t)this->nPlanes * this->displ_len * sizeof(uint16_t);
#if HUB_DOUBLE_BUFFER
		for (int i = 0; i < HUB_NUM_BUFFERS; i++) if (bufs[i]) memset(bufs[i], 0, bytes);
#else
		memset(buf16, 0, bytes);
#endif
	}

	// Raw access to the panel bit-plane buffer (Pi packs the exact buf16 layout; MCU copies it in).
	// Layout (LE): nPlanes planes, each displ_len uint16; plane p, cell -> index p*displ_len+cell.
	// With triple-buffering this returns the current FILL buffer (the RX core's write target).
	uint8_t *rawBuf16() {
#if HUB_DOUBLE_BUFFER
		return (uint8_t *)bufs[fillIdx];
#else
		return (uint8_t *)buf16Back;
#endif
	}
	uint32_t buf16Bytes() const { return (uint32_t)this->nPlanes * this->displ_len * 2u; }

	// --- Triple-buffer handoff (lock-free, single producer = RX core, single consumer = panel core) ---
	// RX core: fill rawBuf16(), then submitFill() to publish it. Panel core: acquireFront() each loop
	// before swapBuffers() to adopt any freshly published frame. data_transfer latches the front pointer
	// at its start, so adopting a new front never tears an in-flight upload. All no-ops if single-buffered.
	void submitFill()
	{
#if HUB_DOUBLE_BUFFER
		if (numBuffers <= 2) {
			// DOUBLE buffer: publish, then WAIT for the panel core to adopt (only 2 buffers, so there is
			// no free buffer to fill until it does). This stalls the RX core each frame.
			__sync_synchronize();
			readyIdx = fillIdx;
			while (readyIdx >= 0) { }
			fillIdx = 1 - frontIdx;
		} else {
			// TRIPLE buffer: publish and immediately take the remaining free buffer -> the RX core never
			// stalls (no socket-drain gap -> far fewer dropped frames). Usually no wait at all.
			while (readyIdx >= 0) { }
			int nf = 3 - frontIdx - fillIdx;      // the remaining free buffer
			__sync_synchronize();
			readyIdx = fillIdx;
			fillIdx = nf;
		}
#endif
	}

	// After a keyframe the fill target is the stale previous front. Call just before applying a
	// delta so patches land on a copy of the last published frame (not on every keyframe — that
	// 224 KB memcpy contended with the panel DMA and killed fps).
	void copyFrontToFill()
	{
#if HUB_DOUBLE_BUFFER
		if (numBuffers < 2 || fillIdx == frontIdx) return;
		if (!bufs[fillIdx] || !bufs[frontIdx]) return;
		memcpy(bufs[fillIdx], bufs[frontIdx], buf16Bytes());
#endif
	}

	// (Re)allocate the frame buffer(s) for a given colour depth + buffer count:
	// {14 planes, 2 buffers} = max colour depth (double-buffer, RX core stalls per frame), or
	// {8 planes, 3 buffers} = triple-buffer (RX core never stalls -> minimal UDP drops, max fps).
	// Safe live: stops GCLK so DMA is not reading the buffers being freed. The sketch must pause
	// the RX core (no memcpy / submitFill) across this call, then resume scan with swapBuffers().
	void reconfigure(uint8_t planes, uint8_t nbuf)
	{
#if HUB_DOUBLE_BUFFER
		this->stop_GCLK();
		if (planes < 1) planes = 1;
		if (planes > 14) planes = 14;
		if (nbuf < 2) nbuf = 2;
		if (nbuf > 3) nbuf = 3;
		for (int i = 0; i < 3; i++) { if (bufs[i]) { free(bufs[i]); bufs[i] = nullptr; } }
		// Only the STORED plane count (buffer RAM / colour shades) changes here. gclk_bits (the GCLK
		// train length = BRIGHTNESS) is left untouched: data_transfer maps the nPlanes planes into the
		// top bits of the gclk_bits-wide train, so 8 planes at gclk=14 = full brightness, 256 shades.
		this->nPlanes = planes;
		numBuffers = nbuf;
		size_t bytes = (size_t)this->nPlanes * this->displ_len * sizeof(uint16_t);
		for (int i = 0; i < nbuf; i++) { bufs[i] = (uint16_t *)malloc(bytes); memset(bufs[i], 0, bytes); }
		frontIdx = 0; fillIdx = 1; readyIdx = -1;
		buf16 = bufs[0];
		buf16Back = bufs[1];
#endif
	}
	void acquireFront()
	{
#if HUB_DOUBLE_BUFFER
		int r = readyIdx;
		if (r >= 0) { frontIdx = r; buf16 = bufs[r]; __sync_synchronize(); readyIdx = -1; } // old front becomes free
#endif
	}
	// legacy alias (single-buffer builds): commit == nothing to swap
	void commitFrame() { submitFill(); }

	// Fastest per-pixel path for streaming: `packed` from mapPixel, `q` = 4 precomputed plane 3-bit
	// group values (plane 0..3) at gbase 0 for this colour. Requires clear16() first (OR writes).
	// Assumes nPlanes == 4 (COLOR_4BITS).
	inline void drawPackedFast(uint16_t packed, const uint8_t *q)
	{
		if (packed == 0xFFFF) return;
		const uint16_t cellOff = packed & 0x1FFF;
		const uint8_t gbase = (uint8_t)(((packed >> 13) & 0x3) * 3);
		const uint32_t stride = this->displ_len;
		uint16_t *cell = buf16 + cellOff;
		cell[0]            |= (uint16_t)(q[0] << gbase);
		cell[stride]       |= (uint16_t)(q[1] << gbase);
		cell[stride * 2]   |= (uint16_t)(q[2] << gbase);
		cell[stride * 3]   |= (uint16_t)(q[3] << gbase);
	}

#if defined(DMD_PIO_MUX) && DMD_PIO_MUX
	// CPU scan ISR stays on PIO0_IRQ_0 (irq 0). The mux SM lives on pio2 and waits
	// on irq 4 so it never shares a PIO block with the W6300 QSPI (pio1).
	void add_pio_prg3() override
	{
		this->pio_clkdiv = 1;
		this->sm_clk_cnt = pio_claim_unused_sm(this->pio, true);
		this->clk_cnt_prog_offs = pio_add_program(this->pio, &clock_cnt2_mux_program);
		this->clk_cnt_pio_config = clock_cnt2_mux_program_init(this->pio, this->sm_clk_cnt,
			this->clk_cnt_prog_offs, this->pio_clkdiv, this->pin_DMD_CLK, this->pin_DMD_nOE);
	}

	void __not_in_flash_func(scan_dmd)() override
	{
		pio_interrupt_clear(this->pio, 0);
		if (this->row == 0) {
			if (this->oe_scan_flag == false) {
				pio_sm_set_enabled(this->pio, this->sm_clk_cnt, false);
				pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);
				pio_sm_set_enabled(mux_pio, sm_mux, false);
				this->oe_scan_res = false;
				return;
			}
		}
		this->row++;
		if (this->row >= this->nRows)
			this->row = 0;
	}

	void init_mux() override
	{
		mux_resync();
		this->row = 1;
		this->send_oe(12);
		this->send_clocks(88);
	}
#endif

protected:
	uint16_t *buf16 = nullptr;      // FRONT: uploaded by data_transfer (latched per transfer)
	uint16_t *buf16Back = nullptr;  // single-buffer write target (== buf16 unless HUB_DOUBLE_BUFFER)
#if HUB_DOUBLE_BUFFER
	uint16_t *bufs[3] = {nullptr, nullptr, nullptr};
	int frontIdx = 0, fillIdx = 1;
	uint8_t numBuffers = HUB_NUM_BUFFERS;   // runtime buffer count (2 = double, 3 = triple); see reconfigure()
	volatile int readyIdx = -1;     // >=0 = a filled buffer waiting to become front
#endif
	uint16_t *dma16 = nullptr;
	uint16_t *dma16_buffs[2] = {nullptr, nullptr};

	// Per-colour config register arrays (see applyPanelRegsRGB). Null / perColor=false = broadcast.
	uint16_t *cfg0 = nullptr, *cfg1 = nullptr, *cfg2 = nullptr;
	bool perColor = false;

#if defined(DMD_PIO_MUX) && DMD_PIO_MUX
	PIO mux_pio = pio2;
	uint sm_mux = 0;
	uint mux_off = 0;
	uint mux_clk_pin = 0;
	pio_sm_config mux_cfg;

	void mux_pio_init()
	{
		mux_off = pio_add_program(mux_pio, &mux595_program);
		sm_mux = (uint)pio_claim_unused_sm(mux_pio, true);
		mux_cfg = mux595_program_init(mux_pio, sm_mux, mux_off, mux_clk_pin);
	}

	// Force a clean one-hot at row 0. Stock set_mux(0) is a no-op when last_row
	// is already 0; a second inject would clock the existing 1 to row 1 and load
	// a new 1 at row 0 → two adjacent rows (looks like column N plus N-1 after
	// the panel rotation). Flush 64 zeros, then inject once.
	void mux_resync()
	{
		pio_sm_set_enabled(mux_pio, sm_mux, false);
		pio_sm_clear_fifos(mux_pio, sm_mux);
		pio_interrupt_clear(this->pio, 4);

		for (uint8_t i = 0; i < this->nRows; i++)
			mux595_exec_shift(mux_pio, sm_mux, false);
		mux595_exec_shift(mux_pio, sm_mux, true);

		pio_sm_put(mux_pio, sm_mux, (uint32_t)(this->nRows - 1u));
		pio_sm_exec(mux_pio, sm_mux, pio_encode_pull(false, false));
		pio_sm_exec(mux_pio, sm_mux, pio_encode_mov(pio_y, pio_osr));
		pio_sm_exec(mux_pio, sm_mux, pio_encode_mov(pio_x, pio_y));
		pio_sm_exec(mux_pio, sm_mux, pio_encode_jmp(mux_off));
		pio_sm_set_enabled(mux_pio, sm_mux, true);
	}
#endif

	// read one uint16 from each of the 4 planes (12 data bits each), MSB plane first in the low word
	uint64_t expand_planes16(uint16_t *p)
	{
		uint64_t res = 0;
		for (uint8_t i = 0; i < 4; i++) {
			uint16_t b = (i < this->nPlanes) ? *p : 0;
			p += this->displ_len;
			res = (res << 16) | b;
		}
		return res; // = p0<<48 | p1<<32 | p2<<16 | p3
	}

	// 12-bit PIO data path + 16-bit DMA (mirrors the stock pio_dma_init otherwise)
	void pio_dma_init() override
	{
		// Data SM (12-bit program)
		this->pio_clkdiv = 1 + CYCLES_PER_MICROSECOND / (this->max_clk_freq * 5);
		this->sm_data = pio_claim_unused_sm(this->pio, true);
		this->data_prog_offs = pio_add_program(this->pio, &dmd_out_spwm12_program);
		this->pio_config = dmd_out_spwm12_program_init(this->pio, this->sm_data, this->data_prog_offs,
			this->pio_clkdiv, this->data_pins[0], 12, this->pin_DMD_SCLK, this->pin_DMD_CLK);

		// CLK_LAT SM
		this->pio_clkdiv = 1 + CYCLES_PER_MICROSECOND / (this->max_clk_freq * 2);
		this->sm_clk_lat = pio_claim_unused_sm(this->pio, true);
		this->clk_lat_prog_offs = pio_add_program(this->pio, &clock_latches_program);
		this->clk_lat_pio_config = clock_latches_prg_init(this->pio, this->sm_clk_lat,
			this->clk_lat_prog_offs, this->pio_clkdiv, this->pin_DMD_SCLK, this->pin_DMD_CLK);
		this->clk_lat_out_pin_is_lat = true;

		// CLK_CNT SM
		this->add_pio_prg3();
#if defined(DMD_PIO_MUX) && DMD_PIO_MUX
		mux_pio_init();
#endif
		pio_set_irq0_source_enabled(this->pio, pis_interrupt0, true);

		// DMA (16-bit)
		this->dma_chan = dma_claim_unused_channel(true);
		this->dma_c = dma_channel_get_default_config(this->dma_chan);
		// The panel data DMA feeds the PIO FIFO while GCLK runs; if it underruns (e.g. a second core
		// hammering its own DMA/bus during streaming) the DCLK train stalls and the greyscale timing
		// jitters ("unruhig" display). Mark it high priority so the DMA arbiter services it first.
		channel_config_set_high_priority(&this->dma_c, true);
		channel_config_set_transfer_data_size(&this->dma_c, DMA_SIZE_16);
		channel_config_set_read_increment(&this->dma_c, true);
		channel_config_set_dreq(&this->dma_c, this->sm_data + DREQ_PIO0_TX0);
		channel_config_set_ring(&this->dma_c, false, 5); // 32-byte wrap = 16 uint16 (config regs)
		dma_channel_configure(this->dma_chan, &this->dma_c, &pio0_hw->txf[this->sm_data],
			NULL, this->x_len, false);
	}

	// broadcast a 16-bit config register value to all 12 data lines (16-bit version)
	void send_to_allRGB(uint16_t data, uint16_t latches) override
	{
		static uint16_t reg16[16] __attribute__((aligned(32)));
		for (int i = 0; i < 16; i++) {
			reg16[i] = (data & 0x8000) ? 0x0FFF : 0x0000;
			data <<= 1;
		}
		channel_config_set_ring(&(this->dma_c), false, 5); // 32-byte wrap so the 16 words repeat
		dma_channel_set_config(this->dma_chan, &(this->dma_c), false);

		dmd_out_program_reinit(this->pio, this->sm_data, this->data_prog_offs, &this->pio_config);
		uint32_t control_par1 = this->x_len - 1;
		uint32_t control_par2 = this->x_len - (latches + 1);
		pio_sm_put_blocking(this->pio, this->sm_data, (control_par2 << 16) | control_par1);
		dma_channel_set_read_addr(this->dma_chan, reg16, true);
		dma_channel_wait_for_finish_blocking(this->dma_chan);
		while (!pio_interrupt_get(this->pio, 7));
		pio_interrupt_clear(this->pio, 7);
	}

	// Send independent 16-bit config words on the three sub-pixel data lines of every group.
	// d0 -> bit gbase+0, d1 -> bit gbase+1, d2 -> bit gbase+2, replicated across all 4 groups.
	void send_to_allRGB_rgb(uint16_t d0, uint16_t d1, uint16_t d2, uint16_t latches)
	{
		static uint16_t reg16[16] __attribute__((aligned(32)));
		for (int i = 0; i < 16; i++) {
			uint16_t base3 = (uint16_t)(((d0 & 0x8000) ? 0x1 : 0) |
			                            ((d1 & 0x8000) ? 0x2 : 0) |
			                            ((d2 & 0x8000) ? 0x4 : 0));
			reg16[i] = (uint16_t)(base3 | (base3 << 3) | (base3 << 6) | (base3 << 9));
			d0 <<= 1; d1 <<= 1; d2 <<= 1;
		}
		channel_config_set_ring(&(this->dma_c), false, 5); // 32-byte wrap so the 16 words repeat
		dma_channel_set_config(this->dma_chan, &(this->dma_c), false);

		dmd_out_program_reinit(this->pio, this->sm_data, this->data_prog_offs, &this->pio_config);
		uint32_t control_par1 = this->x_len - 1;
		uint32_t control_par2 = this->x_len - (latches + 1);
		pio_sm_put_blocking(this->pio, this->sm_data, (control_par2 << 16) | control_par1);
		dma_channel_set_read_addr(this->dma_chan, reg16, true);
		dma_channel_wait_for_finish_blocking(this->dma_chan);
		while (!pio_interrupt_get(this->pio, 7));
		pio_interrupt_clear(this->pio, 7);
	}

	// Per-colour config refresh: mirrors ICN1065::load_config_regs but sends independent R/G/B for
	// the register value line. Called once per frame (cycles through one register). Falls back to
	// the broadcast path when perColor is off.
	void load_config_regs(uint16_t *conf_reg) override
	{
		if (!perColor) { DMD_RGB_ICN1065<Pars...>::load_config_regs(conf_reg); return; }

		static uint8_t r = 0xFF;
		r++;
		if (r >= this->conf_reg_cnt) r = 0;

		this->send_vsync();
		this->send_clocks(8);
		this->send_latches(11); // pre-active command
		this->send_clocks(8);
		this->send_latches(14); // pre-active command
		this->send_clocks(8);
		this->init_mux();
		pio_sm_set_enabled(this->pio, this->sm_clk_lat, false);
		this->start_DCLK();

		this->send_to_allRGB(0x00aa, 5);
		this->send_to_allRGB(0x01aa, 5);
		send_to_allRGB_rgb(cfg0[r], cfg1[r], cfg2[r], 5); // per-colour register value
		this->send_to_allRGB(0x0055, 5);
		this->send_to_allRGB(0x0155, 5);
	}

	// upload greyscale (16-bit / 12-line version of the stock data_transfer)
	// RAM-resident: this inner loop used to live in flash (XIP). Extra sketch data (GFX fonts)
	// shifted its cache-line layout and jittered the field upload -> panel flicker even after
	// the intro ended. Keeping the body in SRAM makes refresh timing independent of flash layout.
	void __not_in_flash_func(data_transfer)() override
	{
		const uint8_t num_sect = this->x_len / 16;
		uint8_t buff_select = 0;
		uint16_t *forward_ptr = dma16_buffs[buff_select];
		uint16_t *backward_ptr = dma16_buffs[1 - buff_select];
		bool first_run = true;

		uint32_t control_par1 = this->x_len - 1;
		uint32_t control_par2 = this->x_len - 2;
		uint8_t gclk_msb = this->gclk_bits - this->dimming_factor;

		const uint8_t tx_cnt = this->tx_row_count();
		const uint16_t tx_base = this->tx_row_base();
		uint16_t *const fb = buf16; // latch FRONT once: a commitFrame() swap mid-upload can't tear this frame
		for (uint8_t y = 0; y < tx_cnt; y++)
		{
			uint16_t *ptr = fb + (uint32_t)(tx_base + y) * this->x_len;
			for (uint8_t x = 0; x < 16; x++)
			{
				uint16_t *ptr2 = ptr + x;
				uint16_t i = 0;
				const uint8_t np = this->nPlanes;                    // 4 (default) or 8 (true colour)
				const uint32_t stride = this->displ_len;
				for (uint8_t sect = 0; sect < num_sect; sect++)
				{
					// 16-entry greyscale window: leading zeros, then nPlanes planes MSB-first, then
					// trailing zeros. More planes just fill previously-zero slots -> no extra upload.
					uint16_t base = (uint16_t)(i + (16 - gclk_msb));
					for (uint8_t z = 0; z < (16 - gclk_msb); z++) backward_ptr[i + z] = 0;
					for (uint8_t pl = 0; pl < np; pl++)
						backward_ptr[base + pl] = ptr2[(uint32_t)(np - 1 - pl) * stride]; // MSB plane first
					for (uint8_t z = 0; z < (gclk_msb - np); z++) backward_ptr[base + np + z] = 0;
					ptr2 += 16;
					i += 16;
				}

				buff_select = 1 - buff_select;
				forward_ptr = dma16_buffs[buff_select];
				backward_ptr = dma16_buffs[1 - buff_select];

				if (first_run) {
					first_run = false;
					channel_config_set_ring(&(this->dma_c), false, 0); // cancel config-reg wrap
					dma_channel_set_config(this->dma_chan, &(this->dma_c), false);
					pio_sm_set_enabled(this->pio, this->sm_clk_cnt, true);
				}
				else {
					dma_channel_wait_for_finish_blocking(this->dma_chan);
					while (!pio_interrupt_get(this->pio, 7));
					pio_interrupt_clear(this->pio, 7);
				}

				pio_sm_set_enabled(this->pio, this->sm_data, false);
				dmd_out_program_reinit(this->pio, this->sm_data, this->data_prog_offs, &this->pio_config);
				pio_sm_put_blocking(this->pio, this->sm_data, (control_par2 << 16) | control_par1);
				dma_channel_set_read_addr(this->dma_chan, forward_ptr, true);
			}
		}
		dma_channel_wait_for_finish_blocking(this->dma_chan);
	}
};

#endif  // ARCH_RP2040
#endif
