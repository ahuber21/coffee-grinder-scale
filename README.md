# coffee-grinder-scale

ESP32 project to control a coffee grinder by weight.

<img src="https://github.com/ahuber21/coffee-grinder-scale/blob/main/.doc/shot.png" width=300>

## Features

* Control your grinder from an ESP32.
  * State-machine approach with rich functionality and error handling.
* Control the ESP32 from your browser.
  * Websocket-based implementation of a logger, settings, and a weight graph.
  * Using [esphome](https://github.com/esphome/) libraries: ESPAsyncWebServer, AsyncTCP.
  * Settings are automatically persisted in EEPROM.

<div style="flex: 0 0 300px;">
    <img src="https://github.com/ahuber21/coffee-grinder-scale/blob/main/.doc/console.png" width="300" alt="Console">
</div>
<div style="flex: 0 0 300px;">
    <img src="https://github.com/ahuber21/coffee-grinder-scale/blob/main/.doc/settings.png" width="300" alt="Settings">
</div>
<div style="flex: 0 0 300px;">
    <img src="https://github.com/ahuber21/coffee-grinder-scale/blob/main/.doc/graph.png" width="300" alt="Graph">
</div>

* OTA updates for wire-free development (awesome in combination with the websocket logger!).
* GUI using a small TFT and Adafruit libraries.
  * Introduced the concept of FPS to reduce flickering.
* Configure two buttons for two pre-configured doses (grams).
  * Including some logic to filter sporadic glitches and self-triggering
* Fast, precise, responsive readout of ADS1232.
* Confirm button press to avoid accidental starts.
* Top-up logic to gradually converge to desired dose.


## Hardware

This project is using the hardware from jousis' espresso-scale (https://gitlab.com/jousis/espresso-scale) just because I had it lying around. You can port it to other ESP32-based systems, as long as you also use the ADS1232. Feel free to open an issue if you have questions.

My grinder is the Eureka Mignon. The hardware mod is based on this [Tech Dregs YouTube video](https://www.youtube.com/watch?v=ksemL5_kvDw). Note that the power supply of the 220 V version of the grinder is different and it doesn't seem to be capable of running the ESP32. I'm therefore using an external wall plug.

The case is very basic, but works for me. I'm using [these buttons](https://www.amazon.de/gp/product/B0BF51N8CK/ref=ppx_yo_dt_b_search_asin_image?ie=UTF8&th=1&language=en_GB), but ever since I added them I'm experiencing glitches that I had to correct in software (see `loopButtonFilter`). If you want to build this, maybe try different buttons, and play around with adding other pull-ups/pull-downs and filter caps.

