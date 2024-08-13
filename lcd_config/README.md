## Instruction for the configuration of the LCD

The TFT_eSPI library requires the user to configure the LCD connection in a file located in *.pio/libdeps/esp32/TFT_eSPI/User_Setup.h*.

Note: it is first necessary to install the library via platform.io before modifying this file.

1. Install the TFT_eSPI library
2. Replace the file in *.pio/libdeps/esp32/TFT_eSPI/User_Setup.h* with the one in this folder
3. Change the configuration file to your liking (e.g., your controller, LCD resolutin, etc.)