# ESP32-WROOM-32-Bluetooth-AM-broadcast-processor

WWARNING!!! Do not use the “loud 1”preset, recent updates got more UI controls working so it’s badly distorted, this notification will disappear when rectified on next update.

This is a basic Broadcast processor as a starter for Car Bluetooth processor projects that lacks this feature, Current FM Transmitters suffer from Sibilance and distortion, this is a new build to address this issue, This is the AM version which will progress to becoming a C-QUAM transmitter when the digital Oscillator and class D module arrives, an FM version will be built next.

The UI file is the GUI to adjust the settings of the processor

Uses the PCM5102 chip, so now has potentially better modulation control

Still has a few bugs to investigate, it looks like the asymmetric clippers dont do anything when using the calibration oscillators,
They are obviously working but only when handling program, so for now to adjust, watch the modulation waveform on the CRO.
