# ProtoTone

ProtoTone is a small stylophone-style synthesizer built around the ATmega328P Xplained Mini. It uses a stylus and a resistor ladder to select notes, then generates audio with PWM and a small PAM8403 amplifier.

## Features

- 12-note chromatic keyboard using a resistor ladder
- Stylus-based note selection
- OLED status display over I2C
- PWM audio output on OC1A / PB1
- Square, saw and triangle waveforms
- Pitch bend using a joystick
- Vibrato control using a potentiometer
- Volume control using a potentiometer
- Hold mode
- Octave up/down buttons
- Status LED
- Serial debug output

## Hardware

Main components:

- ATmega328P Xplained Mini
- SSD1306 128x64 OLED display
- PAM8403 3W audio amplifier
- 4Ω / 3W speaker
- 10k potentiometers for volume and vibrato
- Analog joystick for pitch bend
- Resistor ladder keyboard
- Stylus contact
- Buttons for octave, waveform and hold

## Pin usage

| Function | ATmega328P / Arduino pin |
|---|---|
| Audio PWM | PB1 / D9 / OC1A |
| Notes ADC | PC0 / A0 |
| Pitch bend | PC1 / A1 |
| Vibrato | PC2 / A2 |
| Volume | PC3 / A3 |
| OLED SDA | PC4 / A4 / SDA |
| OLED SCL | PC5 / A5 / SCL |
| Octave down | D2 |
| Octave up | D3 |
| Waveform select | D4 |
| Hold | D5 |
| Status LED | D6 |

## How it works

The note keyboard is built as a resistor ladder. Each key produces a different voltage, read through `A0`. The software compares the ADC value with calibrated values and locks onto the closest stable note.

Once a note is detected, the code calculates its final frequency by applying octave shift, joystick pitch bend and vibrato. The frequency is converted into a phase increment and used by a phase accumulator to generate waveforms.

Timer1 runs Fast PWM on `OC1A` at 62.5 kHz. Timer2 runs an interrupt at 16 kHz and updates the PWM duty cycle with the next audio sample. The PWM output is filtered with an RC filter, then sent to the PAM8403 amplifier and speaker.

## Software notes

The code uses:

- `Adafruit_SSD1306` and `Adafruit_GFX` for the OLED
- `Wire` for I2C
- Timer1 for PWM audio output
- Timer2 Compare Match interrupt for audio sample generation
- ADC reads for note, pitch, vibrato and volume
- software debounce for buttons
- note hysteresis to avoid unstable note switching

## Build

The project is built with PlatformIO.

Required libraries:

```ini
lib_deps =
    adafruit/Adafruit SSD1306
    adafruit/Adafruit GFX Library