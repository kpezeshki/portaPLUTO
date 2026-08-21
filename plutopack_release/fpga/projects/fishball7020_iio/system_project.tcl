source ../../adi-hdl/scripts/adi_env.tcl
source $ad_hdl_dir/projects/scripts/adi_project_xilinx.tcl
source $ad_hdl_dir/projects/scripts/adi_board.tcl

set p_device "xc7z020clg400-1"
# Vivado 2025.x fix: register the IP repos (incl. the generated maia_sdr IP in
# ../../ip) before adi_project opens the block design. Consumed by the guarded
# block in adi-hdl/projects/scripts/adi_project_xilinx.tcl. Paths mirror the
# set already done in ../pluto/system_bd.tcl.
set ::ad_project_ip_repo_paths {../../antsdr-hdl ../../ip ../../adi-hdl/library}
adi_project fishball

adi_project_files fishball [list \
  "../fishball_iio/system_top.v" \
  "../fishball_iio/system_constr.xdc" \
  "$ad_hdl_dir/library/common/ad_iobuf.v"]


# use improved implementation strategy for best timing results
set_property strategy Performance_ExplorePostRoutePhysOpt [get_runs impl_1]

set_property is_enabled false [get_files  *system_sys_ps7_0.xdc]
adi_project_run fishball
source $ad_hdl_dir/library/axi_ad9361/axi_ad9361_delay.tcl

