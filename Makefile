.PHONY: sim_mac clean

sim_mac:
	verilator --cc --exe --build --trace \
		-Wall -Wno-fatal \
		rtl/mac_unit.sv tb/tb_mac_unit.cpp \
		-o mac_sim
	./obj_dir/mac_sim

clean:
	rm -rf obj_dir waves/*.vcd
