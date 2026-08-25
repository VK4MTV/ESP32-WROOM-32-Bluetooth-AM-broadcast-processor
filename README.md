# ESP32-WROOM-32-Bluetooth-AM-broadcast-processor

This is a basic Broadcast processor as a starter for Car Bluetooth processor projects that lacks this feature, Current FM Transmitters suffer from Sibilance and distortion, this is a new build to address this issue, This is the AM version which will progress to becoming a C-QUAM transmitter when the digital Oscillator and class D module arrives, an FM version will be built next.

Make sure you use an oscilloscope while doing callibration, make sure you have the output turned all the way to nil before applying any audio!

Not recommended to use on commercial Transmitter Hundreds to thousands of watts, yet!!!! This processor is in beta testing and development and modulation control is tight but it still has suspected bugs in the look ahead limiting clippers, read on to find out how I am progressing on this. Its pretty close.

The UI preset Loud 1 is a good starting point

The UI file is the GUI to adjust the settings of the processor

Uses the PCM5102 chip, so now has potentially better modulation control

Latest update has improved the matrix modulation control, listening through a high energy stereophonic percussive moment at 2 mins 6-7 seconds of the 1985 hit "Howard Jones - Like to Get to know you well" no longer causes fussy mono DSP radios from muting (hopefully at this point, but test shows massive improvement), the Digitech AR-1780 tends to be bad for that panic "muting" that it normally does when shortwave stations fade, perceiving the Q shift getting too close to exceeding 90 degrees as a loss of carrier. The receiver now stays at full volume through that percussive event, at worst a slightly gritty boost of treble which is now perfectly acceptable.

Still has a few bugs to investigate and fix, it looks like the asymmetric clippers dont do anything when using the calibration oscillators,
They are obviously working but only when handling program which I have located a little bug in the firmware that sends it to the output after the clippers, so for now to adjust, watch the modulation waveform on the Oscilloscope while playing music, adjust in increments with the output and pos/neg sliders for maximum modulation while avoid the carrier from pinching, be aware that not all transmitters support asymetric modulation so best to keep the sliders in line for now for , on air loudness is already.


REASON TO NOT USE ON KILOWATT RIGS!!!
I will put forward that there is occasional transient carrier pinches from observation of prolonged Oscilloscope measurements particularly on some of the most brutally bass heavy tracks, so some more work needs to be done on the final limiters to eliminate this, AM modulation should never go to zero carrier, -95, or -98% is absolute limits! Some receivers particularly with envelope detectors begin to distort if you exceed 95% modulation, -98% many transmitters lose linearity and begin to splatter. The issue just fixed mentioned above was the L-R limiter in the wrong place in the look ahead chain causing it to be delayed, this may be the issue in the final clippers as well.

however the latest update does sound cleaner than before, watch this space for further updates.
