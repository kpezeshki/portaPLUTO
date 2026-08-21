# constraints
# ad9361 (SWAP == 0x1)

set_property  -dict {PACKAGE_PIN  U18  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_clk_in_p]        
set_property  -dict {PACKAGE_PIN  U19  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_clk_in_n]        
set_property  -dict {PACKAGE_PIN  Y16  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_frame_in_p]      
set_property  -dict {PACKAGE_PIN  Y17  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_frame_in_n]      
set_property  -dict {PACKAGE_PIN  Y18  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_data_in_p[0]]   
set_property  -dict {PACKAGE_PIN  Y19  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_data_in_n[0]]   
set_property  -dict {PACKAGE_PIN  T16  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_data_in_p[1]]   
set_property  -dict {PACKAGE_PIN  U17  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_data_in_n[1]]   
set_property  -dict {PACKAGE_PIN  V20  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_data_in_p[2]]   
set_property  -dict {PACKAGE_PIN  W20  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_data_in_n[2]]   
set_property  -dict {PACKAGE_PIN  T17  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_data_in_p[3]]   
set_property  -dict {PACKAGE_PIN  R18  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_data_in_n[3]]   
set_property  -dict {PACKAGE_PIN  T20  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_data_in_p[4]]   
set_property  -dict {PACKAGE_PIN  U20  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_data_in_n[4]]   
set_property  -dict {PACKAGE_PIN  W18  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_data_in_p[5]]   
set_property  -dict {PACKAGE_PIN  W19  IOSTANDARD LVDS_25 DIFF_TERM TRUE} [get_ports rx_data_in_n[5]]   
set_property  -dict {PACKAGE_PIN  U14  IOSTANDARD LVDS_25} [get_ports tx_clk_out_p]                     
set_property  -dict {PACKAGE_PIN  U15  IOSTANDARD LVDS_25} [get_ports tx_clk_out_n]                     
set_property  -dict {PACKAGE_PIN  V16  IOSTANDARD LVDS_25} [get_ports tx_frame_out_p]                   
set_property  -dict {PACKAGE_PIN  W16  IOSTANDARD LVDS_25} [get_ports tx_frame_out_n]                   
set_property  -dict {PACKAGE_PIN  V15  IOSTANDARD LVDS_25} [get_ports tx_data_out_p[0]]                 
set_property  -dict {PACKAGE_PIN  W15  IOSTANDARD LVDS_25} [get_ports tx_data_out_n[0]]                 
set_property  -dict {PACKAGE_PIN  V12  IOSTANDARD LVDS_25} [get_ports tx_data_out_p[1]]                 
set_property  -dict {PACKAGE_PIN  W13  IOSTANDARD LVDS_25} [get_ports tx_data_out_n[1]]                 
set_property  -dict {PACKAGE_PIN  W14  IOSTANDARD LVDS_25} [get_ports tx_data_out_p[2]]                 
set_property  -dict {PACKAGE_PIN  Y14  IOSTANDARD LVDS_25} [get_ports tx_data_out_n[2]]                 
set_property  -dict {PACKAGE_PIN  T12  IOSTANDARD LVDS_25} [get_ports tx_data_out_p[3]]                 
set_property  -dict {PACKAGE_PIN  U12  IOSTANDARD LVDS_25} [get_ports tx_data_out_n[3]]                 
set_property  -dict {PACKAGE_PIN  T11  IOSTANDARD LVDS_25} [get_ports tx_data_out_p[4]]                 
set_property  -dict {PACKAGE_PIN  T10  IOSTANDARD LVDS_25} [get_ports tx_data_out_n[4]]                 
set_property  -dict {PACKAGE_PIN  U13  IOSTANDARD LVDS_25} [get_ports tx_data_out_p[5]]                 
set_property  -dict {PACKAGE_PIN  V13  IOSTANDARD LVDS_25} [get_ports tx_data_out_n[5]]                  

set_property  -dict {PACKAGE_PIN  L20 IOSTANDARD LVCMOS25} [get_ports gpio_status[0]]                  
set_property  -dict {PACKAGE_PIN  L19 IOSTANDARD LVCMOS25} [get_ports gpio_status[1]]                  
set_property  -dict {PACKAGE_PIN  K19 IOSTANDARD LVCMOS25} [get_ports gpio_status[2]]                  
set_property  -dict {PACKAGE_PIN  T14 IOSTANDARD LVCMOS25} [get_ports gpio_status[3]]                  
set_property  -dict {PACKAGE_PIN  P15 IOSTANDARD LVCMOS25} [get_ports gpio_status[4]]                  
set_property  -dict {PACKAGE_PIN  M20 IOSTANDARD LVCMOS25} [get_ports gpio_status[5]]                  
set_property  -dict {PACKAGE_PIN  M19 IOSTANDARD LVCMOS25} [get_ports gpio_status[6]]                  
set_property  -dict {PACKAGE_PIN  N20 IOSTANDARD LVCMOS25} [get_ports gpio_status[7]]
                 
set_property  -dict {PACKAGE_PIN  J19 IOSTANDARD LVCMOS25} [get_ports gpio_ctl[0]]                     
set_property  -dict {PACKAGE_PIN  K14 IOSTANDARD LVCMOS25} [get_ports gpio_ctl[1]]                     
#set_property  -dict {PACKAGE_PIN  L17 IOSTANDARD LVCMOS25} [get_ports gpio_ctl[2]]                     
set_property  -dict {PACKAGE_PIN  R14 IOSTANDARD LVCMOS25} [get_ports gpio_ctl[2]]                     
set_property  -dict {PACKAGE_PIN  J20 IOSTANDARD LVCMOS25} [get_ports gpio_ctl[3]] 
set_property  -dict {PACKAGE_PIN  P20  IOSTANDARD LVCMOS25} [get_ports gpio_en_agc]
set_property  -dict {PACKAGE_PIN  R19  IOSTANDARD LVCMOS25} [get_ports gpio_resetb]

set_property  -dict {PACKAGE_PIN  T15  IOSTANDARD LVCMOS25} [get_ports enable]
set_property  -dict {PACKAGE_PIN  P18  IOSTANDARD LVCMOS25} [get_ports txnrx]

set_property  -dict {PACKAGE_PIN  M14  IOSTANDARD LVCMOS25 PULLTYPE PULLUP} [get_ports iic_scl]
set_property  -dict {PACKAGE_PIN  M15  IOSTANDARD LVCMOS25 PULLTYPE PULLUP} [get_ports iic_sda]

set_property  -dict {PACKAGE_PIN  R17  IOSTANDARD LVCMOS25  PULLTYPE PULLUP} [get_ports spi_csn]
set_property  -dict {PACKAGE_PIN  V18  IOSTANDARD LVCMOS25} [get_ports spi_clk]
set_property  -dict {PACKAGE_PIN  P16  IOSTANDARD LVCMOS25} [get_ports spi_mosi]
set_property  -dict {PACKAGE_PIN  V17  IOSTANDARD LVCMOS25} [get_ports spi_miso]

# pl_spi unused on this board; pl_spi_clk_o moved off L14 (now MCU CS) to a free bank-13 pin.
set_property  -dict {PACKAGE_PIN  Y13  IOSTANDARD LVCMOS33} [get_ports pl_spi_clk_o]   ;# relocated from L14 (IO_L20N_T3_13, freed when SDO moved to V10)
set_property  -dict {PACKAGE_PIN  N15  IOSTANDARD LVCMOS25} [get_ports pl_spi_miso]
set_property  -dict {PACKAGE_PIN  N16  IOSTANDARD LVCMOS25} [get_ports pl_spi_mosi]

# --- PlutoSky display (CH320QV15A-T, PS SPI1/EMIO) + MCU EMIO GPIO ---
# Bank 13 @ 3.3V (LVCMOS33) - display SPI data bus, direct to the 3.3V panel.
# Ball assignments per board schematic (confirmed 2026-07-25 vs xc7z020clg400 device DB).
set_property  -dict {PACKAGE_PIN  U9   IOSTANDARD LVCMOS33} [get_ports disp_sdi]       ;# IO_L17P_T2_13      -> display SDI (MOSI)
set_property  -dict {PACKAGE_PIN  V10  IOSTANDARD LVCMOS33} [get_ports disp_sdo]       ;# IO_L21N_T3_DQS_13  <- display SDO (MISO)
set_property  -dict {PACKAGE_PIN  T9   IOSTANDARD LVCMOS33} [get_ports disp_scl]       ;# IO_L12P_T1_MRCC_13 -> display LCD_WR (SCLK)
set_property  -dict {PACKAGE_PIN  U10  IOSTANDARD LVCMOS33} [get_ports disp_csn]       ;# IO_L12N_T1_MRCC_13 -> display LCD_RD (CS)
# --- D/C + MCU bus — BANK 35, LVCMOS25 (matches the working AD9361 bank; ---
# --- level-shifted off-chip to the Pico/panel). Balls per board schematic. ---
set_property  -dict {PACKAGE_PIN  G19  IOSTANDARD LVCMOS25} [get_ports disp_rs]        ;# IO_L18P_T2_AD13P_35     -> display LCD_RS (D/C)
set_property  -dict {PACKAGE_PIN  G20  IOSTANDARD LVCMOS25} [get_ports {mcu_io[0]}]    ;# IO_L18N_T2_AD13N_35     <-> Pico GPIO22 (spare)
set_property  -dict {PACKAGE_PIN  J18  IOSTANDARD LVCMOS25} [get_ports {mcu_io[1]}]    ;# IO_L14P_T2_AD4P_SRCC_35 <-> Pico GPIO21 (spare)
set_property  -dict {PACKAGE_PIN  H18  IOSTANDARD LVCMOS25} [get_ports {mcu_io[2]}]    ;# IO_L14N_T2_AD4N_SRCC_35 <-> Pico GPIO20 (INT)
set_property  -dict {PACKAGE_PIN  H16  IOSTANDARD LVCMOS25} [get_ports {mcu_io[3]}]    ;# IO_L13P_T2_MRCC_35      <-> Pico GPIO19 (MISO)
set_property  -dict {PACKAGE_PIN  H17  IOSTANDARD LVCMOS25} [get_ports {mcu_io[4]}]    ;# IO_L13N_T2_MRCC_35      <-> Pico GPIO18 (SCK)
set_property  -dict {PACKAGE_PIN  L14  IOSTANDARD LVCMOS25} [get_ports {mcu_io[5]}]    ;# IO_L22P_T3_AD7P_35      <-> Pico GPIO17 (CS) [reclaimed from pl_spi_clk_o]
set_property  -dict {PACKAGE_PIN  L15  IOSTANDARD LVCMOS25} [get_ports {mcu_io[6]}]    ;# IO_L22N_T3_AD7N_35      <-> Pico GPIO16 (MOSI)


create_clock -period 8.000 -name rx_clk [get_ports rx_clk_in_p]

# probably gone in 2016.4

create_clock -name clk_fpga_0 -period 10 [get_pins "i_system_wrapper/system_i/sys_ps7/inst/PS7_i/FCLKCLK[0]"]
create_clock -name clk_fpga_1 -period  5 [get_pins "i_system_wrapper/system_i/sys_ps7/inst/PS7_i/FCLKCLK[1]"]

create_clock -name spi0_clk      -period 40   [get_pins -hier */EMIOSPI0SCLKO]

set_input_jitter clk_fpga_0 0.3
set_input_jitter clk_fpga_1 0.15

#set_false_path -to [get_pins i_system_wrapper/system_i/lvds_clck2/inst/clk_out_reg/CLR]

set_false_path -from [get_pins {i_system_wrapper/system_i/axi_ad9361/inst/i_rx/i_up_adc_common/up_adc_gpio_out_int_reg[0]/C}]
set_false_path -from [get_pins {i_system_wrapper/system_i/axi_ad9361/inst/i_tx/i_up_dac_common/up_dac_gpio_out_int_reg[0]/C}]

set_false_path -from [get_pins {i_system_wrapper/system_i/manual_decim/U0/gpio_core_1/Not_Dual.gpio_Data_Out_reg[0]/C}]

