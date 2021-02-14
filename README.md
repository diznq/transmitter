# K64F AM Radio Transmitter

Purely software radio transmitter implementation in C++.

To begin transmitting, connect antennae to ADC0 and microphone input or other audio input between DAC0 and GND.

Transmitter automatically tries to choose best frequency from array of predefined frequencies and writes chosen frequency on standard output after measurements are done.