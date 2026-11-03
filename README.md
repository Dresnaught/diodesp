# DiodESP
Multipurpose hardware device. (voice changer, Soundboard, and display things whats on your PC).

for now im foccusing on linux only, sorry :(

But Worry Not! i will plan to add other OS support later.

## features
the features im planning is quite a lot but, here goes whats fixed tba:
- voice changer(configurable)
- Music player (display the image, and whats currently playing on your PC)
- display Temps, CPU, RAM, GPU usage(configurable)
- Soundboard to pass an mp3(or any audio files) to the eq(vc)/direct to the sound input from the esp

## devices/part
- ESP32-S3 N16R8
- ILI9488 3.5 Inch SPI display TFT
- few buttons to navigate through the menu

## to build
requirements:
- esp-idf, ( i recomend using newest one: 6.1.0)
- python ( to ran the script for now).

step;
1. git clone or fork this repo.
2. go to the directory of this repo.
3. don't forget to run the `export.sh` from esp-idf
4. ran 
   ```bash
   idf.py build
   idf.py flash
   ```
5. done!

or you can specify what port by:
`idf.py -p <to/port/USB> flash`

- oh yeah, one thing i didnt include the sdkconfig, i will just not now

## tba
to be added as time goes.

