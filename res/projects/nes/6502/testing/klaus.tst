target CPU

// Virtual RAM allocation
// Usage: ram <size> <addr_pin> <data_in_pin> <data_out_pin> <we_pin> <we_active_high>
ram 65536 addr data_in data_out rw 0

load_rom 6502_functional_test.bin

// Define the clock pin
clock phi0

// Set default states for interrupt pins and bus
set n_nmi 1
set n_irq 1
set rdy 1
set so 0

// Trigger the Power-Up Reset Sequence
set n_res 0
tick 7
set n_res 1
tick 2

// Run the test suite
tick 10000000

// Success verification. The test loops infinitely at $3469 on success.
print addr
assert addr 13417