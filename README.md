# DiodESP
Multipurpose hardware device. (voice changer, Soundboard, and display things whats on your PC).

for now im foccusing on linux only, sorry.
But i will plan to add other OS support later.

## features
the features:
- voice changer(configurable)
- Music player (display the image, and whats currently playing on your PC)
- display Temps, CPU, RAM, GPU usage(configurable)
- Soundboard to pass an mp3(or any audio files) to the eq(vc)/direct to the sound input from the esp
- maybe more

## devices/part
- ESP32-S3 N16R8
- ILI9488 3.5 Inch SPI display TFT
- 3 buttons to navigate through the menu

## to build and flash
requirements:
- esp-idf, ( i recomend using newest one: 6.1.0)
- python ( to ran the script for now).
- mpris

step;
1. git clone or fork this repo.
2. go to the directory of this repo.
3. don't forget to run the `export.sh` from esp-idf
4. plug the mcu to flash, (i recommend using the uart one or the right port as the first port is used to the script.)
5. ran 
   ```bash
   idf.py build
   idf.py flash
   ```
6. done!

or you can specify what port by:
`idf.py -p <to/port/USB> flash`
and if you want to use jtag method: `idf.py openocd flash` or if you want to specify the port ` idf.py openocd -p <to/port/USB> flash`

## whats done
- the usb to mpris script is done and working fine
- display tft using lvgl and lovyanGFX is working fine
- able to display whats playing and control media using buttons working fine
- able to show the image art from the music playing, via mpris

## whats not done yet
- the voice changer, (it only has the base page display, usb code configuration for that is hasnt done yet)
- soundboard, hasnt do any code for that feature yet
- page to display ram usage, cpu more into your pc is also not done yet
- Schematic and PCB design, is also not done yet, with much probability will use I2S IC (like TLV320AIC3254) for later

