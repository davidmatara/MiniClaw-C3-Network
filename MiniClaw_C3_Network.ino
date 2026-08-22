/*
  ============================================================
                 MINICLAW ESP32-C3 AUDIO RECEIVER
  ============================================================

  S3 -> ESP-NOW -> C3 -> GPIO10 -> HOMEMADE AMPLIFIER

  AUDIO:
    16-bit PCM
    16 kHz
    mono

  OUTPUT:
    GPIO 10 PWM

  RESERVED:
    GPIO 3,4,5,6,7 are NOT USED
  ============================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ============================================================
// SETTINGS
// ============================================================

#define ESPNOW_CHANNEL 1

#define AUDIO_PIN 10

#define PWM_FREQUENCY 312500
#define PWM_RESOLUTION 8

#define AUDIO_RATE 16000

#define PACKET_MAGIC 0xCAFE

// 240 bytes + 8-byte header = 248 bytes
#define AUDIO_BYTES 240


// ============================================================
// PACKET
// ============================================================

struct __attribute__((packed)) AudioPacket {

  uint16_t magic;

  uint16_t sequence;

  uint16_t sampleCount;

  uint8_t reserved;

  uint8_t reserved2;

  uint8_t pcm[AUDIO_BYTES];
};


// ============================================================
// AUDIO BUFFER
// ============================================================

#define AUDIO_QUEUE_SIZE 24

struct AudioBlock {

  uint16_t length;

  uint8_t data[AUDIO_BYTES];
};


QueueHandle_t audioQueue;


// ============================================================
// STATISTICS
// ============================================================

volatile uint32_t packetsReceived = 0;
volatile uint32_t packetsDropped = 0;
volatile uint32_t bytesReceived = 0;


// ============================================================
// PWM
// ============================================================

void setupAudioPWM() {

  if (
    !ledcAttach(
      AUDIO_PIN,
      PWM_FREQUENCY,
      PWM_RESOLUTION
    )
  ) {

    Serial.println(
      "ERROR: PWM setup failed!"
    );

    while (true)
      delay(1000);
  }


  ledcWrite(
    AUDIO_PIN,
    128
  );


  Serial.println(
    "GPIO 10 PWM audio ready."
  );
}


// ============================================================
// ESP-NOW RECEIVE
// ============================================================

void onDataReceive(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int length
) {

  if (
    length !=
    sizeof(AudioPacket)
  ) {

    return;
  }


  AudioPacket packet;

  memcpy(
    &packet,
    data,
    sizeof(packet)
  );


  if (
    packet.magic !=
    PACKET_MAGIC
  ) {

    return;
  }


  AudioBlock block;

  block.length =
    AUDIO_BYTES;


  memcpy(
    block.data,
    packet.pcm,
    AUDIO_BYTES
  );


  if (
    xQueueSend(
      audioQueue,
      &block,
      0
    ) != pdTRUE
  ) {

    packetsDropped++;

    return;
  }


  packetsReceived++;

  bytesReceived +=
    AUDIO_BYTES;
}


// ============================================================
// AUDIO OUTPUT TASK
// ============================================================

void audioTask(
  void *parameter
) {

  AudioBlock block;


  while (true) {

    if (
      xQueueReceive(
        audioQueue,
        &block,
        portMAX_DELAY
      ) != pdTRUE
    ) {

      continue;
    }


    /*
      Each sample is 16-bit signed PCM.

      Convert:

        -32768 ... +32767

      to:

        0 ... 255

      for the PWM output.
    */

    for (
      int i = 0;
      i < block.length;
      i += 2
    ) {

      int16_t sample =
        (int16_t)(
          block.data[i] |
          (block.data[i + 1] << 8)
        );


      uint8_t pwm =
        (uint8_t)(
          (sample >> 8) +
          128
        );


      ledcWrite(
        AUDIO_PIN,
        pwm
      );


      /*
        16 kHz sample period:

          1,000,000 / 16,000
          = 62.5 us
      */

      delayMicroseconds(
        62
      );
    }
  }
}


// ============================================================
// ESP-NOW SETUP
// ============================================================

void setupESPNow() {

  WiFi.mode(
    WIFI_STA
  );


  /*
    Both S3 and C3 must use the same
    Wi-Fi channel for ESP-NOW.
  */

  esp_wifi_set_channel(
    ESPNOW_CHANNEL,
    WIFI_SECOND_CHAN_NONE
  );


  Serial.print(
    "C3 MAC: "
  );

  Serial.println(
    WiFi.macAddress()
  );


  Serial.print(
    "Channel: "
  );

  Serial.println(
    ESPNOW_CHANNEL
  );


  if (
    esp_now_init() !=
    ESP_OK
  ) {

    Serial.println(
      "ESP-NOW FAILED"
    );

    while (true)
      delay(1000);
  }


  esp_now_register_recv_cb(
    onDataReceive
  );


  Serial.println(
    "ESP-NOW receiver ready."
  );
}


// ============================================================
// STATUS
// ============================================================

void printStatus() {

  Serial.println();
  Serial.println(
    "========= MINICLAW C3 ========="
  );

  Serial.print(
    "Packets: "
  );

  Serial.println(
    packetsReceived
  );

  Serial.print(
    "Dropped: "
  );

  Serial.println(
    packetsDropped
  );

  Serial.print(
    "Bytes: "
  );

  Serial.println(
    bytesReceived
  );

  Serial.print(
    "Queue: "
  );

  Serial.println(
    uxQueueMessagesWaiting(
      audioQueue
    )
  );

  Serial.print(
    "MAC: "
  );

  Serial.println(
    WiFi.macAddress()
  );

  Serial.println(
    "==============================="
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
    "================================"
  );

  Serial.println(
    "       MINICLAW C3 AUDIO"
  );

  Serial.println(
    "================================"
  );


  audioQueue =
    xQueueCreate(
      AUDIO_QUEUE_SIZE,
      sizeof(AudioBlock)
    );


  if (
    audioQueue == NULL
  ) {

    Serial.println(
      "Audio queue FAILED"
    );

    while (true)
      delay(1000);
  }


  setupAudioPWM();

  setupESPNow();


  xTaskCreate(
    audioTask,
    "AudioOutput",
    4096,
    NULL,
    4,
    NULL
  );


  Serial.println();

  Serial.println(
    "READY."
  );

  Serial.println(
    "Waiting for S3 audio..."
  );

  Serial.println(
    "Type STATUS for statistics."
  );
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

    command.trim();

    command.toUpperCase();


    if (
      command ==
      "STATUS"
    ) {

      printStatus();
    }
  }


  delay(10);
}
