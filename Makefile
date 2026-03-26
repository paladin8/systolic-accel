.PHONY: sim_mac sim_array clean

sim_mac:
	verilator --cc --exe --build --trace \
		-Wall -Wno-fatal \
		rtl/mac_unit.sv tb/tb_mac_unit.cpp \
		-o mac_sim
	./obj_dir/mac_sim

sim_array:
	uv run python tb/ref_matmul.py 4
	verilator --cc --exe --build --trace \
		-Wall -Wno-fatal \
		--top-module systolic_array \
		rtl/mac_unit.sv rtl/systolic_array.sv tb/tb_systolic_array.cpp \
		-o array_sim
	./obj_dir/array_sim

clean:
	rm -rf obj_dir waves/*.vcd
