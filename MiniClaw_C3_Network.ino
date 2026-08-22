/*
  ============================================================
                 MINICLAW ESP32-C3 AUDIO
              REAL PWM AUDIO OUTPUT TEST
  ============================================================

  ESP32-C3
       |
       | GPIO 10
       v
  Homemade transistor amplifier
       |
       v
     Speaker

  RESERVED:
      GPIO 3
      GPIO 4
      GPIO 5
      GPIO 6
      GPIO 7

  AUDIO:
      GPIO 10 = PWM audio output

  PWM carrier:
      312.5 kHz

  Audio sample rate:
      16 kHz

  Audio:
      8-bit unsigned PCM

  ============================================================
*/

#include <Arduino.h>
#include "esp_timer.h"

// ============================================================
// PIN
// ============================================================

#define AUDIO_PIN 10

// ============================================================
// PWM
// ============================================================

#define PWM_CHANNEL 0
#define PWM_FREQUENCY 312500
#define PWM_RESOLUTION 8

// ============================================================
// AUDIO
// ============================================================

#define SAMPLE_RATE 16000

volatile uint8_t audioSample = 128;

volatile bool audioRunning = false;

esp_timer_handle_t audioTimer;


// ============================================================
// AUDIO SAMPLE TIMER
// ============================================================

void audioSampleTimer(
  void* arg
) {

  if (!audioRunning) {

    ledcWrite(
      PWM_CHANNEL,
      128
    );

    return;
  }

  /*
     Send current PCM sample
     to PWM duty cycle.
  */

  ledcWrite(
    PWM_CHANNEL,
    audioSample
  );
}


// ============================================================
// SET AUDIO SAMPLE
// ============================================================

void setSample(
  uint8_t sample
) {

  audioSample =
    sample;
}


// ============================================================
// START AUDIO TIMER
// ============================================================

void startAudioTimer() {

  const esp_timer_create_args_t timerArgs = {

    .callback =
      &audioSampleTimer,

    .arg =
      NULL,

    .dispatch_method =
      ESP_TIMER_TASK,

    .name =
      "audio_sample"

  };


  esp_timer_create(
    &timerArgs,
    &audioTimer
  );


  /*
     1,000,000 microseconds / 16,000
     = 62.5 microseconds/sample
  */

  esp_timer_start_periodic(
    audioTimer,
    62
  );
}


// ============================================================
// SILENCE
// ============================================================

void silence() {

  audioRunning =
    false;

  audioSample =
    128;

  ledcWrite(
    PWM_CHANNEL,
    128
  );
}


// ============================================================
// SINE WAVE
// ============================================================

void playTone(
  float frequency,
  uint32_t duration
) {

  const uint32_t samplesPerCycle =
    SAMPLE_RATE / frequency;

  uint32_t totalSamples =
    ((uint64_t)duration *
     SAMPLE_RATE) /
    1000;


  audioRunning =
    true;


  for (
    uint32_t i = 0;
    i < totalSamples;
    i++
  ) {

    float phase =
      (float)(i % samplesPerCycle) /
      samplesPerCycle;

    float value =
      sin(
        phase *
        2.0 *
        PI
      );


    /*
       Convert -1...+1
       into 0...255
    */

    uint8_t sample =
      (uint8_t)(
        128 +
        value * 110
      );


    setSample(
      sample
    );


    delayMicroseconds(
      1000000 /
      SAMPLE_RATE
    );
  }


  silence();
}


// ============================================================
// FREQUENCY SWEEP
// ============================================================

void frequencySweep() {

  Serial.println(
    "Frequency sweep..."
  );


  audioRunning =
    true;


  for (
    float frequency = 100;
    frequency <= 5000;
    frequency += 20
  ) {

    uint32_t samples =
      SAMPLE_RATE / frequency;


    if (samples < 2)
      samples = 2;


    for (
      uint32_t i = 0;
      i < samples;
      i++
    ) {

      float phase =
        (float)i /
        samples;


      float value =
        sin(
          phase *
          2.0 *
          PI
        );


      audioSample =
        128 +
        value * 110;


      delayMicroseconds(
        1000000 /
        SAMPLE_RATE
      );
    }
  }


  silence();
}


// ============================================================
// STARTUP SOUND
// ============================================================

void startupSound() {

  playTone(
    440,
    300
  );

  delay(80);

  playTone(
    660,
    300
  );

  delay(80);

  playTone(
    880,
    500
  );
}


// ============================================================
// AMPLIFIER TEST
// ============================================================

void amplifierTest() {

  Serial.println();

  Serial.println(
    "=============================="
  );

  Serial.println(
    "AMPLIFIER TEST"
  );

  Serial.println(
    "440 Hz"
  );

  Serial.println(
    "=============================="
  );


  playTone(
    440,
    2000
  );


  delay(500);


  Serial.println(
    "880 Hz"
  );


  playTone(
    880,
    2000
  );


  delay(500);


  Serial.println(
    "Test finished."
  );
}


// ============================================================
// SERIAL COMMANDS
// ============================================================

void processCommand(
  String command
) {

  command.trim();

  command.toUpperCase();


  if (
    command ==
    "TEST"
  ) {

    amplifierTest();

    return;
  }


  if (
    command ==
    "440"
  ) {

    playTone(
      440,
      2000
    );

    return;
  }


  if (
    command ==
    "880"
  ) {

    playTone(
      880,
      2000
    );

    return;
  }


  if (
    command ==
    "SWEEP"
  ) {

    frequencySweep();

    return;
  }


  if (
    command ==
    "STOP"
  ) {

    silence();

    Serial.println(
      "Audio stopped."
    );

    return;
  }


  if (
    command ==
    "STATUS"
  ) {

    Serial.println();

    Serial.println(
      "MiniClaw C3 Audio"
    );

    Serial.println(
      "Output GPIO: 10"
    );

    Serial.println(
      "PWM carrier: 312.5 kHz"
    );

    Serial.println(
      "Audio rate: 16 kHz"
    );

    Serial.println(
      "Resolution: 8-bit"
    );

    return;
  }


  if (
    command ==
    "HELP"
  ) {

    Serial.println();

    Serial.println(
      "MiniClaw C3 commands:"
    );

    Serial.println(
      "TEST   - amplifier test"
    );

    Serial.println(
      "440    - 440 Hz tone"
    );

    Serial.println(
      "880    - 880 Hz tone"
    );

    Serial.println(
      "SWEEP  - frequency sweep"
    );

    Serial.println(
      "STOP   - stop audio"
    );

    Serial.println(
      "STATUS - show audio settings"
    );

    Serial.println(
      "HELP   - show commands"
    );

    return;
  }


  Serial.println(
    "Unknown command."
  );
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(
    115200
  );


  delay(1000);


  Serial.println();

  Serial.println(
    "======================================"
  );

  Serial.println(
    "       MINICLAW ESP32-C3"
  );

  Serial.println(
    "       PWM AUDIO CONTROLLER"
  );

  Serial.println(
    "======================================"
  );


  /*
     Configure high-frequency PWM.
  */

  ledcSetup(
    PWM_CHANNEL,
    PWM_FREQUENCY,
    PWM_RESOLUTION
  );


  ledcAttachPin(
    AUDIO_PIN,
    PWM_CHANNEL
  );


  /*
     Start at the centre point.
     This represents zero amplitude
     for unsigned 8-bit PCM.
  */

  ledcWrite(
    PWM_CHANNEL,
    128
  );


  /*
     Start sample timer.
  */

  startAudioTimer();


  Serial.println();

  Serial.println(
    "GPIO 10 = PWM AUDIO"
  );

  Serial.println(
    "GPIO 3-7 untouched."
  );

  Serial.println();

  Serial.println(
    "MiniClaw C3 AUDIO READY."
  );

  Serial.println(
    "Type HELP."
  );


  delay(500);


  /*
     Startup sound.
  */

  startupSound();
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  if (
    Serial.available()
  ) {

    String command =
      Serial.readStringUntil(
        '\n'
      );


    processCommand(
      command
    );
  }


  delay(5);
}
