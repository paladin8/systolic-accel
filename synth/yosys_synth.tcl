# Quick manual synthesis of systolic array with default parameters (iCE40).
# Usage: yosys -s synth/yosys_synth.tcl
#
# For parameter sweeps, run_sweep.py constructs inline Yosys commands with
# chparam directly — it does not use this script.

read_verilog -sv rtl/mac_unit.sv rtl/systolic_array.sv
synth_ice40 -top systolic_array
stat
