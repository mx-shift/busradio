# busradio: AM-radio audio over an IBM PC AT's bus emissions

Turn an unmodified IBM PC AT (80286) into a crude AM transmitter by
deliberately leaking RF off the system bus — no DAC, no Sound Blaster
for output, no PWM speaker tricks, just software driving the bus hard
enough to be heard on a nearby AM radio.

Tested on an 8 MHz 80286. No special hardware: an AT and a cheap AM
radio placed within ~30 cm of the case. The carrier lands around
1500 kHz; sweep the dial for it.

Released under the Apache License 2.0 — see `LICENSE`.
