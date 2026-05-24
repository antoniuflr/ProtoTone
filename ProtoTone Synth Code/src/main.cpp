#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <avr/interrupt.h>
#include <util/atomic.h>
#include <math.h>

// Screen constants
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

// Arduino pin for audio
#define AUDIO_PIN 9

// Arduino pins for analog entries
#define NOTES_PIN A0
#define PITCH_PIN A1
#define VIBRATO_PIN A2
#define VOLUME_PIN A3

// Arduino pins for GPIO
#define BTN_OCT_DOWN 2
#define BTN_OCT_UP 3
#define BTN_WAVEFORM 4
#define BTN_HOLD 5
#define LED_STATUS 6

// Rate at which the code samples the audio
#define SAMPLE_RATE 16000.0f

// Safe thresholds for speaker
#define MIN_SAFE_FREQ 150.0f
#define MAX_SAFE_FREQ 2000.0f

// Safe space around the center of the joystick
#define PITCH_DEADZONE 25

// Display constructor
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// All the notes
const char* noteNames[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};


// Notes frequencies
const float noteFreqs[12] = {
    261.63, 277.18, 293.66, 311.13,
    329.63, 349.23, 369.99, 392.00,
    415.30, 440.00, 466.16, 493.88
};

// Each note converted (measured on board)
const int noteAdcExpected[12] = {
    861, 737, 636, 553, 478, 413,
    354, 298, 244, 189, 133, 70
};


// Current position in waveform
volatile uint32_t phaseAcc = 0;

// Rate of navigation thorugh waveform
volatile uint32_t phaseInc = 0;

// Volume, scaling factor for waveform
volatile uint8_t audioVolume = 0;

// Waveform type
volatile uint8_t waveform = 0; // 0 square, 1 saw, 2 triangle

// Note active boolean
volatile bool noteActive = false;

// Current note
int currentNote = -1;

// Current frequency
float currentFreq = 0.0f;

// Current octave
int8_t octaveOffset = 0;

// Hold status boolean
bool holdEnabled = false;

// What note should be played while hold is enabled
int heldNote = -1;

// Values after conversion for notes, pitch, vibrato and volume
uint16_t notesAdc = 0;
uint16_t pitchAdc = 0;
uint16_t vibratoAdc = 0;
uint16_t volumeAdc = 0;

// Transform a frequency from Hz to a step in the waveform
uint32_t freqToPhaseInc(float freq) {
    // Clamps to not damage the speaker
    if (freq < MIN_SAFE_FREQ) freq = MIN_SAFE_FREQ;
    if (freq > MAX_SAFE_FREQ) freq = MAX_SAFE_FREQ;

    // phaseInc = frequency * 2^32 / sample_rate
    return (uint32_t)((freq * 4294967296.0f) / SAMPLE_RATE);
}

// Get median of reads
uint16_t smoothAnalogRead(uint8_t pin) {
    uint32_t sum = 0;

    // Add up 12 consecutive reads
    for (uint8_t i = 0; i < 12; i++) {
        sum += analogRead(pin);
        delayMicroseconds(120);
    }

    // Get median
    return sum / 12;
}

// Clamp read adc value to nearest note
int detectNearestNote(uint16_t adc) {
    int bestIndex = -1;

    // Maximum difference
    int bestDiff = 1024;

    for (int i = 0; i < 12; i++) {
        int diff = abs((int)adc - noteAdcExpected[i]);

        if (diff < bestDiff) {
            bestDiff = diff;
            bestIndex = i;
        }
    }

    if (bestDiff > 90) {
        return -1;
    }

    return bestIndex;
}

// Detect note with ADC unwanted fluctuation
int detectNoteStable(uint16_t adc) {
    // Static variables to stay the same during function calls
    // Current stabilized note
    static int lockedNote = -1;

    // How many consecutive reads are in the air
    static uint8_t airCounter = 0;

    // Potential new note
    static int candidateNote = -1;

    // How many times in a row we-ve seen the candidate
    static uint8_t candidateCounter = 0;

    // If ADC is below this value, we consider the stylus being in the air
    const int AIR_THRESHOLD = 35;

    // We change the note only if the new note is this close to other note
    const int SWITCH_MARGIN = 55;

    // How many consecutive air reads are necessary to stop the current nore
    const uint8_t AIR_RELEASE_COUNT = 4;

    // How many consecutive candidate reads to switch to it
    const uint8_t NOTE_CONFIRM_COUNT = 3;

    // ADC below threshold = air
    // If stylus doesn't make good contact for 1-2 reads the sound doesn't stop
    if (adc < AIR_THRESHOLD) {
        if (airCounter < AIR_RELEASE_COUNT) {
            airCounter++;
            return lockedNote;
        }

        // Reset
        lockedNote = -1;
        candidateNote = -1;
        candidateCounter = 0;
        return -1;
    }

    // No air
    airCounter = 0;

    // Get nearest note
    int nearestNote = detectNearestNote(adc);

    // If no note is found, return
    if (nearestNote < 0) {
        return lockedNote;
    }

    // If we don't have a locked note (no sound playing)
    if (lockedNote < 0) {
        // If we've seen this candidate we increment the counter
        if (nearestNote == candidateNote) {
            candidateCounter++;
        // If not, this is the candidate now
        } else {
            candidateNote = nearestNote;
            candidateCounter = 1;
        }

        // If we've seen the candidate enough times in a row, it becomes our locked note
        if (candidateCounter >= NOTE_CONFIRM_COUNT) {
            lockedNote = candidateNote;
            candidateNote = -1;
            candidateCounter = 0;
        }

        return lockedNote;
    }

    // If we already have a note and there is another
    // How close is adc value to the new note
    int nearestDiff = abs((int)adc - noteAdcExpected[nearestNote]);

    // How close is adc value to the current note 
    int currentDiff = abs((int)adc - noteAdcExpected[lockedNote]);

    // Hysteresis: we switch only if the new note is SWITCH_MARGIN closer to the adc value to prevent false switches
    if (nearestNote != lockedNote && nearestDiff + SWITCH_MARGIN < currentDiff) {
        if (nearestNote == candidateNote) {
            candidateCounter++;
        } else {
            candidateNote = nearestNote;
            candidateCounter = 1;
        }

        if (candidateCounter >= NOTE_CONFIRM_COUNT) {
            lockedNote = candidateNote;
            candidateNote = -1;
            candidateCounter = 0;
        }
    } else {
        // No clear switch, destroy candidate
        candidateNote = -1;
        candidateCounter = 0;
    }

    // Return satabilized note
    return lockedNote;
}

// Debounce button press
bool pressedOnce(uint8_t pin) {
    // Last time button was pressed
    static uint32_t lastTime[20] = {0};

    // Last state of pin
    static uint8_t lastState[20];

    // Initialize lastState
    static bool initialized[20] = {false};

    if (!initialized[pin]) {
        lastState[pin] = HIGH;
        initialized[pin] = true;
    }

    // Get current satate
    uint8_t state = digitalRead(pin);

    // If enough time has passed and the input changed from high to low we have a press
    if (lastState[pin] == HIGH && state == LOW && millis() - lastTime[pin] > 200) {
        lastTime[pin] = millis();
        lastState[pin] = state;
        return true;
    }

    // No change
    lastState[pin] = state;
    return false;
}

// Setup Timer1 to generate audio on AUDIO_PIN
void setupAudioPWM() {
    // Switch pin to output
    pinMode(AUDIO_PIN, OUTPUT);

    // Reset Timer1
    TCCR1A = 0;
    TCCR1B = 0;
    TCNT1 = 0;

    // Set Timer1 non-inverting PWM on OC1A
    TCCR1A |= (1 << COM1A1);
    
    // Set Fast PWM 8-bit
    TCCR1A |= (1 << WGM10);
    TCCR1B |= (1 << WGM12);

    // Select prescaler 1 => timer clock = cpu frequency = 16MHz
    TCCR1B |= (1 << CS10);

    // Set a default duty cycle
    OCR1A = 128;
}

// Set Timer2 to generate an interruption at 16kHz
void setupSampleTimer() {
    // Reset Timer2
    TCCR2A = 0;
    TCCR2B = 0;
    TCNT2 = 0;

    // Value at which we compare match
    OCR2A = 124;

    // Set timer to CTC
    TCCR2A |= (1 << WGM21);

    // Set prescaler at 8 so ISR runs at 16kHz
    TCCR2B |= (1 << CS21);

    // Activate interruption on compare match
    TIMSK2 |= (1 << OCIE2A);
}

// Interrupt routine that generates the sample
ISR(TIMER2_COMPA_vect) {
    // If no note is active or volume is 0 we set duty cycle to constant
    if (!noteActive || audioVolume == 0) {
        OCR1A = 128;
        return;
    }

    // Go further in waveform by phaseInc determined by current note
    phaseAcc += phaseInc;

    // phaseAcc is on 32 bits for accuracy, while our waveform is on 8, so we take the 8 most significant bits
    uint8_t p = phaseAcc >> 24;

    // Raw value of the wave
    uint8_t raw;

    switch (waveform) {
        // Square wave: first half is up, second is down
        case 0:
            raw = (p < 128) ? 255 : 0;
            break;

        // Saw wave: goes up linearly then jumps to 0
        case 1:
            raw = p;
            break;

        // Triangle wave: goes up linearly to max then descends linearly to 0
        case 2:
            raw = (p < 128) ? (p * 2) : (255 - ((p - 128) * 2));
            break;

        default:
            raw = 128;
            break;
    }

    // Transpose signal from 0:255 ro -128:127 for volume to scale the amplitude around the center, not to drag everything towards 0
    int16_t centered = (int16_t)raw - 128;
    int16_t scaled = (centered * audioVolume) / 255;

    // Set signal back to original interval
    OCR1A = (uint8_t)(scaled + 128);
}

// Display stuff
void updateDisplay() {
    static uint32_t lastDisplay = 0;

    // Update only after 150 ms
    if (millis() - lastDisplay < 150) {
        return;
    }

    lastDisplay = millis();

    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);

    display.print("Note: ");
    if (currentNote >= 0) display.println(noteNames[currentNote]);
    else display.println("-");

    display.print("Freq: ");
    if (currentNote >= 0) {
        display.print((int)currentFreq);
        display.println(" Hz");
    } else {
        display.println("-");
    }

    display.print("Oct: ");
    display.println(octaveOffset);

    display.print("Wave: ");
    if (waveform == 0) display.println("Square");
    else if (waveform == 1) display.println("Saw");
    else display.println("Triangle");

    display.print("Hold: ");
    display.println(holdEnabled ? "ON" : "OFF");

    display.print("Vol: ");
    display.println(volumeAdc);

    display.print("Pitch: ");
    display.println(pitchAdc);

    display.print("Vib: ");
    display.println(vibratoAdc);

    display.print("ADC: ");
    display.println(notesAdc);

    display.display();
}

// Setup some stuff
void setup() {
    // Setup serial
  Serial.begin(9600);
  delay(200);

  Serial.println(F("ProtoTone booting..."));

  // Set pins
  pinMode(BTN_OCT_DOWN, INPUT_PULLUP);
  pinMode(BTN_OCT_UP, INPUT_PULLUP);
  pinMode(BTN_WAVEFORM, INPUT_PULLUP);
  pinMode(BTN_HOLD, INPUT_PULLUP);
  pinMode(LED_STATUS, OUTPUT);

  digitalWrite(LED_STATUS, LOW);
  Serial.println(F("Controls OK."));

  Wire.begin();

  // Setup display
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED init failed."));
    while (1);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("ProtoTone"));
  display.println(F("Init OK"));
  display.display();

  Serial.println(F("OLED OK."));

  // Setup timers
  setupAudioPWM();
  Serial.println(F("Audio PWM OK."));

  setupSampleTimer();
  Serial.println(F("Sample timer OK."));

  Serial.println(F("ADC OK."));
  Serial.println(F("Init complete."));

  delay(1000);
}

// Main loop
void loop() {
    // Get note from stylus
    notesAdc = smoothAnalogRead(NOTES_PIN);

    // get pitch from joystick
    pitchAdc = smoothAnalogRead(PITCH_PIN);

    // Get vibrato from potentiometer
    vibratoAdc = smoothAnalogRead(VIBRATO_PIN);

    // Get volume from potentiometer
    volumeAdc = smoothAnalogRead(VOLUME_PIN);

    // Get stabilized note
    int detectedNote = detectNoteStable(notesAdc);

    // If we got a stable note, set current note
    if (detectedNote >= 0) {
        currentNote = detectedNote;

        // Set held note if hold is enabled
        if (holdEnabled) {
            heldNote = detectedNote;
        }
    } else {
        // If no note is detected and hold is enabled and we have a held note set, set current note
        if (holdEnabled && heldNote >= 0) {
            currentNote = heldNote;
        } else {
            // No note :(
            currentNote = -1;
        }
    }

    // Go down an octave
    if (pressedOnce(BTN_OCT_DOWN)) {
        if (octaveOffset > -1) octaveOffset--;
    }

    // Go up an octave
    if (pressedOnce(BTN_OCT_UP)) {
        if (octaveOffset < 1) octaveOffset++;
    }

    // Cycle through waveforms
    if (pressedOnce(BTN_WAVEFORM)) {
        waveform = (waveform + 1) % 3;
    }

    // Toggle hold
    if (pressedOnce(BTN_HOLD)) {
        holdEnabled = !holdEnabled;

        if (!holdEnabled) {
            heldNote = -1;
        }
    }

    // Boolean for current note set
    bool active = currentNote >= 0;

    // Final augmented signal
    float finalFreq = 0.0f;

    // If there's a note set
    if (active) {
        // Get target frequency for the current note
        float baseFreq = noteFreqs[currentNote];

        // Double or half the frequency based on the octave offset (-1, 0 or 1)
        baseFreq *= powf(2.0f, octaveOffset);

        // Center pitch in 0
        int pitchCentered = (int)pitchAdc - 512;
        float bendSemitones = 0.0f;

        if (abs(pitchCentered) > PITCH_DEADZONE) {
            bendSemitones = (pitchCentered / 512.0f) * 12.0f;
        }

        // Aplitude of vibrato from potentiometer
        float vibratoDepth = (vibratoAdc / 1023.0f) * 0.5f;

        // Oscilates 5 times a second
        float vibratoRate = 5.0f;

        // Get seconds from starting
        float t = millis() / 1000.0f;

        // Final vibrato value based on sin oscilation
        float vibrato = sinf(2.0f * PI * vibratoRate * t) * vibratoDepth;

        // Apply everything
        finalFreq = baseFreq * powf(2.0f, (bendSemitones + vibrato) / 12.0f);

        // Clamp frequency
        if (finalFreq < MIN_SAFE_FREQ) finalFreq = MIN_SAFE_FREQ;
        if (finalFreq > MAX_SAFE_FREQ) finalFreq = MAX_SAFE_FREQ;
    }

    // Update frequency
    currentFreq = finalFreq;

    // Map volume potentiometer input to 0:170 to not overwhelm the speaker
    uint8_t vol = map(volumeAdc, 0, 1023, 0, 170);

    // Atomic operations on variables used by Timer2 ISR
    // This stops interruptions temporarily while we update critical variables to not read while they are written
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        // Set if note is active
        noteActive = active;

        // Set increment in waveform
        phaseInc = active ? freqToPhaseInc(finalFreq) : 0;

        // Set volume
        audioVolume = active ? vol : 0;

        // If not active, set default
        if (!active) {
            OCR1A = 128;
        }
    }

    // Are we playing music or nah? Now visual
    digitalWrite(LED_STATUS, active ? HIGH : LOW);
    updateDisplay();

    // Delay to save on reads and writes
    // Audio is not affected due to it being generated in the Timer2 ISR
    delay(5);
}