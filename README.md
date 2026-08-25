# ESP32-WROOM-32-Bluetooth-AM-broadcast-processor

This is a basic Broadcast processor as a starter for Car Bluetooth processor projects that lacks this feature, Current FM Transmitters suffer from Sibilance and distortion, this is a new build to address this issue, This is the AM version which will progress to becoming a C-QUAM transmitter when the digital Oscillator and class D module arrives, an FM version will be built next.

Make sure you use an oscilloscope while doing callibration, make sure you have the output turned all the way to nil before applying any audio!

The UI preset Loud 1 is a good starting point

The UI file is the GUI to adjust the settings of the processor

Uses the PCM5102 chip, so now has potentially better modulation control

Latest update has improved the matrix modulation control, listening through a high energy stereophonic percussive moment at 2 mins 6-7 seconds of the 1985 hit "Howard Jones - Like to Get to know you well" no longer causes fussy mono DSP radios from muting (hopefully at this point, but test shows massive improvement), the Digitech AR-1780 tends to be bad for that panic "muting" that it normally does when shortwave stations fade, perceiving the Q shift getting too close to exceeding 90 degrees as a loss of carrier. The receiver now stays at full volume through that percussive event, at worst a slightly gritty boost of treble which is now perfectly acceptable.

There was also a bug that was causing bass distortion, now fixed, now the gain can be cranked right up and it just sounds dense instead of distorted.

Also the calibration oscillators are now functioning properly.
