# ESP32-WROOM-32-Bluetooth-AM-broadcast-processor

****************************************
WARNING pay attention to this before you proceed to use this firmware!
****************************************
WARNING: This processor is under Beta development, I cannot guarantee the modulation is 100 percent tightly controlled. Do not use this processor on any high power transmitters until this warning is removed from this readme!

Over-modulation of AM transmitters have serious consequences causing splatter and interference, and possibly can lead to transmitter damage!

Hobby low power use only until extensive tests and debugging has been done
*************************************€****


********************************
Issues to rectify in performance optimisation! 
********************************

****The solution has been found, doing further testing, the attack/release times can only be so fast before causing CPU overload, However turns out this extreme test tests has driven the optimisation of the code to result in what seems to be a very well performing processor, The Loud 1`preset has been updated to a more conservative setting that works well, If you set the attack and release too fast on the multiband section, the processor has to work harder. So far listening tests has been glitch free. this section will be removed when this fix has been confirmed***



Also I do hear some clipping artefacts on piano solo's, which I would like to address, even though mostly its not audible on dense material because the artefact gets masked. this is listening on the legendary vintage Sony WM-F16 walkman with multi AM Stereo compatibility.

I will keep you updated in this development as always. cheers.
********************

ABOUT THIS PROCESSOR:
This is a basic Broadcast processor as a starter for Car Bluetooth Transmitter projects that lacks this feature, Current FM Transmitters suffer from Sibilance and distortion, this is a new build to address this issue, This is the AM version which is C-QUAM compatible.

This project will spin off in two paths (or more), the FM Bluetooth transmitter, and the AM CQUAM Bluetooth transmitter which would be in their own repository in future.

Make sure you use an oscilloscope while doing callibration, make sure you have the output turned all the way to nil before applying any audio or turning on any calibration oscillator

The UI preset Loud 1 is a good starting point otherwise what is pre loaded when you turn it on.

The UI file is the GUI to adjust the settings of the processor which uses a serial protocol over USB, you will need Python installed to use the UI


Uses the PCM5102 chip, so now has potentially better modulation control.
during the R&D, there is specific configurations you have to do on this chip, optimal settings will be published once trialed and approved for field use, the wrong settings can cause spikes that are out of the softwares control (spikes are observed on square and sawtooth waveforms which is currently being investigated, a solution will be found)

Latest update has improved the matrix modulation control, listening through a high energy stereophonic percussive moment at 2 mins 6-7 seconds of the 1985 hit "Howard Jones - Like to Get to know you well" no longer causes fussy mono DSP radios from muting (hopefully at this point, but test shows massive improvement), the Digitech AR-1780 tends to be bad for that panic "muting" that it normally does when shortwave stations fade, perceiving the Q shift getting too close to exceeding 90 degrees as a loss of carrier. The receiver now stays at full volume through that percussive event.

There was also a bug that was causing bass distortion, now fixed, now the gain can be cranked right up and it just sounds dense instead of distorted.

Also the calibration oscillators are now functioning properly.
