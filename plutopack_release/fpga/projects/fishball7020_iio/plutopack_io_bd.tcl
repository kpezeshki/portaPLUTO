# Extend the upstream Pluto block design for the PlutoPack V1.1 front end.
# PS GPIO EMIO 0..17 remain the original radio controls; 18..25 drive
# LCD D/C and the seven Pico control lines.
set_property -dict [list \
    CONFIG.PCW_GPIO_EMIO_GPIO_IO {26} \
    CONFIG.PCW_SPI1_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_SPI1_SPI1_IO {EMIO}] [get_bd_cells sys_ps7]

foreach {name dir} {
    spi1_csn_0_o O spi1_csn_i I
    spi1_clk_i I spi1_clk_o O
    spi1_sdo_i I spi1_sdo_o O
    spi1_sdi_i I
} {
    create_bd_port -dir $dir $name
}
ad_connect spi1_csn_0_o sys_ps7/SPI1_SS_O
ad_connect spi1_csn_i sys_ps7/SPI1_SS_I
ad_connect spi1_clk_i sys_ps7/SPI1_SCLK_I
ad_connect spi1_clk_o sys_ps7/SPI1_SCLK_O
ad_connect spi1_sdo_i sys_ps7/SPI1_MOSI_I
ad_connect spi1_sdo_o sys_ps7/SPI1_MOSI_O
ad_connect spi1_sdi_i sys_ps7/SPI1_MISO_I

foreach signal {i o t} {
    set direction [expr {$signal eq "i" ? "I" : "O"}]
    create_bd_port -dir $direction -from 7 -to 0 disp_mcu_gpio_$signal
}

# Concatenate inputs; split the PS tri-state output into original and new bits.
delete_bd_objs [get_bd_nets -of_objects [get_bd_pins sys_ps7/GPIO_I]]
set cell [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 gpio_i_plutopack]
set_property -dict [list CONFIG.NUM_PORTS {2} CONFIG.IN0_WIDTH {18} CONFIG.IN1_WIDTH {8}] $cell
connect_bd_net [get_bd_ports gpio_i] [get_bd_pins $cell/In0]
connect_bd_net [get_bd_ports disp_mcu_gpio_i] [get_bd_pins $cell/In1]
connect_bd_net [get_bd_pins $cell/dout] [get_bd_pins sys_ps7/GPIO_I]

delete_bd_objs [get_bd_nets -of_objects [get_bd_pins sys_ps7/GPIO_T]]
foreach {name low high target} {
    gpio_t_original 0 17 gpio_t
    gpio_t_plutopack 18 25 disp_mcu_gpio_t
} {
    set cell [create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice:1.0 $name]
    set_property -dict [list CONFIG.DIN_WIDTH {26} CONFIG.DIN_FROM $high CONFIG.DIN_TO $low CONFIG.DOUT_WIDTH [expr {$high-$low+1}]] $cell
    connect_bd_net [get_bd_pins sys_ps7/GPIO_T] [get_bd_pins $cell/Din]
    connect_bd_net [get_bd_pins $cell/Dout] [get_bd_ports $target]
}

set cell [create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice:1.0 gpio_o_plutopack]
set_property -dict [list CONFIG.DIN_WIDTH {26} CONFIG.DIN_FROM {25} CONFIG.DIN_TO {18} CONFIG.DOUT_WIDTH {8}] $cell
connect_bd_net [get_bd_pins sys_ps7/GPIO_O] [get_bd_pins $cell/Din]
connect_bd_net [get_bd_pins $cell/Dout] [get_bd_ports disp_mcu_gpio_o]
