// Tests Max.hack on the CPU emulator. Reads inputs from RAM[0] and RAM[1]
// and asserts the maximum lands in RAM[2].

load Max.hack,
compare-to MaxRam.cmp,
output-list RAM[0]%D2.6.2 RAM[1]%D2.6.2 RAM[2]%D2.6.2;

set PC 0,
set RAM[0] 3,
set RAM[1] 5,
set RAM[2] 0;
repeat 14 {
  ticktock;
}
output;

set PC 0,
set RAM[0] 23456,
set RAM[1] 12345,
set RAM[2] 0;
repeat 14 {
  ticktock;
}
output;