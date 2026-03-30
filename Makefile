.PHONY: sim_mac sim_mac_d1 sim_mac_d3 sim_array sim_array_d1 sim_top synth clean \
       formal_sp formal_mac formal_array formal_ctrl formal_top formal

sim_mac:
	verilator --cc --exe --build --trace \
		-Wall -Wno-fatal \
		rtl/mac_unit.sv tb/tb_mac_unit.cpp \
		-o mac_sim
	./obj_dir/mac_sim

sim_mac_d1:
	verilator --cc --exe --build --trace \
		-Wall -Wno-fatal \
		-GPIPELINE_DEPTH=1 --Mdir obj_dir_mac_d1 \
		-CFLAGS "-DPIPELINE_DEPTH=1" \
		rtl/mac_unit.sv tb/tb_mac_unit.cpp \
		-o mac_sim_d1
	./obj_dir_mac_d1/mac_sim_d1

sim_mac_d3:
	verilator --cc --exe --build --trace \
		-Wall -Wno-fatal \
		-GPIPELINE_DEPTH=3 --Mdir obj_dir_mac_d3 \
		-CFLAGS "-DPIPELINE_DEPTH=3" \
		rtl/mac_unit.sv tb/tb_mac_unit.cpp \
		-o mac_sim_d3
	./obj_dir_mac_d3/mac_sim_d3

sim_array:
	uv run python tb/ref_matmul.py 4
	verilator --cc --exe --build --trace \
		-Wall -Wno-fatal \
		--top-module systolic_array \
		rtl/mac_unit.sv rtl/systolic_array.sv tb/tb_systolic_array.cpp \
		-o array_sim
	./obj_dir/array_sim

sim_array_d1:
	uv run python tb/ref_matmul.py 4
	verilator --cc --exe --build --trace \
		-Wall -Wno-fatal \
		--top-module systolic_array \
		-GPIPELINE_DEPTH=1 --Mdir obj_dir_array_d1 \
		-CFLAGS "-DPIPELINE_DEPTH=1" \
		rtl/mac_unit.sv rtl/systolic_array.sv tb/tb_systolic_array.cpp \
		-o array_sim_d1
	./obj_dir_array_d1/array_sim_d1

sim_top:
	verilator --cc --exe --build --trace \
		-Wall -Wno-fatal \
		--top-module top --Mdir obj_dir_top \
		rtl/mac_unit.sv rtl/systolic_array.sv rtl/scratchpad.sv \
		rtl/controller.sv rtl/top.sv tb/tb_top.cpp \
		-o top_sim
	./obj_dir_top/top_sim

synth:
	uv run python synth/run_sweep.py
	uv run python synth/analyze.py

formal_sp:
	sby -f formal/scratchpad.sby

formal_mac:
	sby -f formal/mac_unit.sby

formal_array:
	sby -f formal/systolic_array.sby

formal_ctrl:
	sby -f formal/controller.sby

formal_top:
	sby -f formal/top.sby

formal: formal_sp formal_mac formal_array formal_ctrl formal_top

clean:
	rm -rf obj_dir obj_dir_mac_d1 obj_dir_mac_d3 obj_dir_array_d1 obj_dir_top waves/*.vcd formal/*/
