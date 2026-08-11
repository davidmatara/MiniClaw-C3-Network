/*
============================================================
              MINICLAW ESP32-C3 NETWORK
             FINAL COMPANION FIRMWARE
============================================================

FUNCTIONS
---------
ESP-NOW
Wi-Fi
SIM800L / GSM / GPRS
Wi-Fi -> GSM fallback
INMP441 microphone
Audio RMS monitoring
Serial Monitor configuration

============================================================
PIN MAP
============================================================

SIM800L
TX -> GPIO 7
RX -> GPIO 6

INMP441
SCK/BCLK -> GPIO 3
WS/LRCLK -> GPIO 4
SD/DOUT  -> GPIO 5
L/R      -> GND

============================================================
SERIAL
============================================================

115200 baud

COMMANDS
--------
HELP
STATUS
WIFI SCAN
WIFI CONNECT
GSM STATUS
GSM APN <apn>
GSM CONNECT
NETWORK
MIC
SAVE
RESET

============================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <math.h>

// ============================================================
// SIM800L
// ============================================================

#define SIM800_TX 7
#define SIM800_RX 6
#define SIM800_BAUD 9600

HardwareSerial SIM800(1);

// ============================================================
// INMP441
// ============================================================

#define I2S_PORT I2S_NUM_0

#define MIC_SCK 3
#define MIC_WS 4
#define MIC_SD 5

#define MIC_SAMPLE_RATE 16000
#define MIC_BUFFER_SAMPLES 512

int32_t micBuffer[MIC_BUFFER_SAMPLES];

size_t micBytesRead = 0;

bool microphoneOK = false;

// ============================================================
// STORAGE
// ============================================================

Preferences prefs;

// ============================================================
// NETWORK
// ============================================================

enum NetworkType
{
  NET_NONE,
  NET_WIFI,
  NET_GSM
};

NetworkType activeNetwork = NET_NONE;

bool wifiOK = false;
bool gsmOK = false;

String wifiSSID = "";
String wifiPassword = "";
String gsmAPN = "";

unsigned long lastNetworkCheck = 0;
unsigned long lastStatusSend = 0;
unsigned long lastMicPrint = 0;

// ============================================================
// ESP-NOW
// ============================================================

typedef struct
{
  char command[160];
} EspNowMessage;

EspNowMessage outgoing;
EspNowMessage incoming;

uint8_t broadcastAddress[] =
{
  0xFF,
  0xFF,
  0xFF,
  0xFF,
  0xFF,
  0xFF
};

// Forward declaration
void processCommand(String command);

// ============================================================
// ESP-NOW SEND
// ============================================================

void sendESPNow(
  String message
)
{
  memset(
    &outgoing,
    0,
    sizeof(outgoing)
  );

  message.toCharArray(
    outgoing.command,
    sizeof(outgoing.command)
  );

  esp_err_t result =
    esp_now_send(
      broadcastAddress,
      (uint8_t *)&outgoing,
      sizeof(outgoing)
    );

  if (
    result != ESP_OK
  )
  {
    Serial.print(
      "[ESP-NOW] Send error: "
    );

    Serial.println(
      result
    );
  }
}

// ============================================================
// ESP-NOW RECEIVE
// ============================================================

void onDataReceive(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int len
)
{
  if (
    len <= 0
  )
    return;

  memset(
    &incoming,
    0,
    sizeof(incoming)
  );

  int copyLength =
    min(
      len,
      (int)sizeof(incoming)
    );

  memcpy(
    &incoming,
    data,
    copyLength
  );

  String command =
    String(
      incoming.command
    );

  command.trim();

  Serial.print(
    "[ESP-NOW] S3 -> C3: "
  );

  Serial.println(
    command
  );

  processCommand(
    command
  );
}

// ============================================================
// ESP-NOW INIT
// ============================================================

bool initESPNow()
{
  Serial.println(
    "[ESP-NOW] Starting..."
  );

  WiFi.mode(
    WIFI_STA
  );

  if (
    esp_now_init() != ESP_OK
  )
  {
    Serial.println(
      "[ESP-NOW] FAILED"
    );

    return false;
  }

  esp_now_register_recv_cb(
    onDataReceive
  );

  esp_now_peer_info_t peerInfo = {};

  memcpy(
    peerInfo.peer_addr,
    broadcastAddress,
    6
  );

  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (
    esp_now_add_peer(
      &peerInfo
    ) != ESP_OK
  )
  {
    Serial.println(
      "[ESP-NOW] Peer setup failed"
    );

    return false;
  }

  Serial.println(
    "[ESP-NOW] READY"
  );

  sendESPNow(
    "C3_READY"
  );

  return true;
}

// ============================================================
// MICROPHONE INIT
// ============================================================

bool initMicrophone()
{
  Serial.println(
    "[MIC] Starting INMP441..."
  );

  i2s_config_t config;

  memset(
    &config,
    0,
    sizeof(config)
  );

  config.mode =
    (i2s_mode_t)(
      I2S_MODE_MASTER |
      I2S_MODE_RX
    );

  config.sample_rate =
    MIC_SAMPLE_RATE;

  config.bits_per_sample =
    I2S_BITS_PER_SAMPLE_32BIT;

  config.channel_format =
    I2S_CHANNEL_FMT_ONLY_LEFT;

  config.communication_format =
    I2S_COMM_FORMAT_I2S;

  config.intr_alloc_flags =
    ESP_INTR_FLAG_LEVEL1;

  config.dma_buf_count = 8;

  config.dma_buf_len =
    MIC_BUFFER_SAMPLES;

  config.use_apll = false;

  config.tx_desc_auto_clear =
    false;

  config.fixed_mclk = 0;

  esp_err_t result =
    i2s_driver_install(
      I2S_PORT,
      &config,
      0,
      NULL
    );

  if (
    result != ESP_OK
  )
  {
    Serial.print(
      "[MIC] Driver error: "
    );

    Serial.println(
      esp_err_to_name(result)
    );

    return false;
  }

  i2s_pin_config_t pins;

  pins.bck_io_num =
    MIC_SCK;

  pins.ws_io_num =
    MIC_WS;

  pins.data_out_num =
    I2S_PIN_NO_CHANGE;

  pins.data_in_num =
    MIC_SD;

  result =
    i2s_set_pin(
      I2S_PORT,
      &pins
    );

  if (
    result != ESP_OK
  )
  {
    Serial.print(
      "[MIC] Pin error: "
    );

    Serial.println(
      esp_err_to_name(result)
    );

    i2s_driver_uninstall(
      I2S_PORT
    );

    return false;
  }

  i2s_zero_dma_buffer(
    I2S_PORT
  );

  Serial.println(
    "[MIC] INMP441 READY"
  );

  return true;
}

// ============================================================
// MICROPHONE RMS
// ============================================================

float readMicrophone()
{
  if (
    !microphoneOK
  )
    return 0;

  micBytesRead = 0;

  esp_err_t result =
    i2s_read(
      I2S_PORT,
      micBuffer,
      sizeof(micBuffer),
      &micBytesRead,
      pdMS_TO_TICKS(100)
    );

  if (
    result != ESP_OK ||
    micBytesRead == 0
  )
  {
    return 0;
  }

  int count =
    micBytesRead /
    sizeof(int32_t);

  if (
    count <= 0
  )
    return 0;

  double sum = 0;

  for (
    int i = 0;
    i < count;
    i++
  )
  {
    int32_t sample =
      micBuffer[i] >> 8;

    double value =
      (double)sample;

    sum +=
      value * value;
  }

  return sqrt(
    sum / count
  );
}

// ============================================================
// MICROPHONE STATUS
// ============================================================

void printMicrophone()
{
  if (
    !microphoneOK
  )
  {
    Serial.println(
      "[MIC] NOT AVAILABLE"
    );

    return;
  }

  float rms =
    readMicrophone();

  Serial.print(
    "[MIC] RMS = "
  );

  Serial.println(
    rms,
    0
  );

  String message =
    "MIC RMS=" +
    String(
      rms,
      0
    );

  sendESPNow(
    message
  );
}

// ============================================================
// CONFIGURATION
// ============================================================

void loadConfiguration()
{
  prefs.begin(
    "miniclaw",
    false
  );

  wifiSSID =
    prefs.getString(
      "ssid",
      ""
    );

  wifiPassword =
    prefs.getString(
      "password",
      ""
    );

  gsmAPN =
    prefs.getString(
      "apn",
      ""
    );
}

void saveConfiguration()
{
  prefs.putString(
    "ssid",
    wifiSSID
  );

  prefs.putString(
    "password",
    wifiPassword
  );

  prefs.putString(
    "apn",
    gsmAPN
  );

  Serial.println(
    "[CONFIG] Saved"
  );
}

// ============================================================
// WIFI SCAN
// ============================================================

void scanWiFi()
{
  Serial.println();
  Serial.println(
    "========== WIFI SCAN =========="
  );

  WiFi.mode(
    WIFI_STA
  );

  int count =
    WiFi.scanNetworks();

  if (
    count <= 0
  )
  {
    Serial.println(
      "No networks found."
    );

    return;
  }

  for (
    int i = 0;
    i < count;
    i++
  )
  {
    Serial.print(
      i
    );

    Serial.print(
      ": "
    );

    Serial.print(
      WiFi.SSID(i)
    );

    Serial.print(
      " RSSI="
    );

    Serial.println(
      WiFi.RSSI(i)
    );
  }

  Serial.println(
    "=============================="
  );
}

// ============================================================
// WIFI CONNECT
// ============================================================

bool connectWiFi()
{
  if (
    wifiSSID.length() == 0
  )
  {
    Serial.println(
      "[WIFI] No SSID configured"
    );

    return false;
  }

  Serial.print(
    "[WIFI] Connecting to "
  );

  Serial.println(
    wifiSSID
  );

  WiFi.mode(
    WIFI_STA
  );

  WiFi.begin(
    wifiSSID.c_str(),
    wifiPassword.c_str()
  );

  unsigned long start =
    millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 15000
  )
  {
    delay(500);

    Serial.print(
      "."
    );
  }

  Serial.println();

  if (
    WiFi.status() ==
    WL_CONNECTED
  )
  {
    wifiOK = true;

    activeNetwork =
      NET_WIFI;

    Serial.println(
      "[WIFI] CONNECTED"
    );

    Serial.print(
      "[WIFI] IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    sendESPNow(
      "WIFI_CONNECTED"
    );

    return true;
  }

  wifiOK = false;

  Serial.println(
    "[WIFI] FAILED"
  );

  sendESPNow(
    "WIFI_FAILED"
  );

  return false;
}

// ============================================================
// SIM800L COMMAND
// ============================================================

bool simCommand(
  const String &command,
  const String &expected,
  unsigned long timeout
)
{
  while (
    SIM800.available()
  )
  {
    SIM800.read();
  }

  SIM800.println(
    command
  );

  String response = "";

  unsigned long start =
    millis();

  while (
    millis() - start <
    timeout
  )
  {
    while (
      SIM800.available()
    )
    {
      char c =
        SIM800.read();

      response += c;

      Serial.write(
        c
      );

      if (
        response.indexOf(
          expected
        ) >= 0
      )
      {
        return true;
      }
    }
  }

  return false;
}

// ============================================================
// GSM STATUS
// ============================================================

void gsmStatus()
{
  Serial.println();
  Serial.println(
    "========== GSM STATUS =========="
  );

  simCommand(
    "AT",
    "OK",
    2000
  );

  Serial.println();

  simCommand(
    "AT+CPIN?",
    "OK",
    3000
  );

  Serial.println();

  simCommand(
    "AT+CSQ",
    "OK",
    3000
  );

  Serial.println();

  simCommand(
    "AT+CREG?",
    "OK",
    3000
  );

  Serial.println(
    "================================"
  );
}

// ============================================================
// GSM CONNECT
// ============================================================

bool connectGSM()
{
  if (
    gsmAPN.length() == 0
  )
  {
    Serial.println(
      "[GSM] APN not configured"
    );

    return false;
  }

  Serial.println(
    "[GSM] Starting GPRS..."
  );

  if (
    !simCommand(
      "AT",
      "OK",
      2000
    )
  )
  {
    Serial.println(
      "[GSM] No SIM800L response"
    );

    return false;
  }

  simCommand(
    "ATE0",
    "OK",
    2000
  );

  simCommand(
    "AT+CFUN=1",
    "OK",
    5000
  );

  String apnCommand =
    "AT+CSTT=\"" +
    gsmAPN +
    "\",\"\",\"\"";

  if (
    !simCommand(
      apnCommand,
      "OK",
      5000
    )
  )
  {
    Serial.println(
      "[GSM] APN failed"
    );

    return false;
  }

  if (
    !simCommand(
      "AT+CIICR",
      "OK",
      20000
    )
  )
  {
    Serial.println(
      "[GSM] GPRS failed"
    );

    return false;
  }

  gsmOK = true;

  activeNetwork =
    NET_GSM;

  Serial.println(
    "[GSM] GPRS CONNECTED"
  );

  sendESPNow(
    "GSM_CONNECTED"
  );

  return true;
}

// ============================================================
// NETWORK MANAGER
// ============================================================

void networkManager()
{
  if (
    WiFi.status() ==
    WL_CONNECTED
  )
  {
    wifiOK = true;

    activeNetwork =
      NET_WIFI;

    return;
  }

  wifiOK = false;

  if (
    gsmOK
  )
  {
    activeNetwork =
      NET_GSM;

    return;
  }

  if (
    wifiSSID.length() > 0
  )
  {
    if (
      connectWiFi()
    )
    {
      return;
    }
  }

  if (
    gsmAPN.length() > 0
  )
  {
    connectGSM();
  }
}

// ============================================================
// STATUS
// ============================================================

void printStatus()
{
  Serial.println();
  Serial.println(
    "========== MINICLAW C3 =========="
  );

  Serial.print(
    "Wi-Fi: "
  );

  Serial.println(
    wifiOK
      ? "CONNECTED"
      : "OFFLINE"
  );

  Serial.print(
    "GSM: "
  );

  Serial.println(
    gsmOK
      ? "CONNECTED"
      : "OFFLINE"
  );

  Serial.print(
    "MIC: "
  );

  Serial.println(
    microphoneOK
      ? "READY"
      : "ERROR"
  );

  Serial.print(
    "APN: "
  );

  Serial.println(
    gsmAPN.length()
      ? gsmAPN
      : "NOT SET"
  );

  Serial.print(
    "ACTIVE NETWORK: "
  );

  if (
    activeNetwork ==
    NET_WIFI
  )
  {
    Serial.println(
      "WIFI"
    );
  }
  else if (
    activeNetwork ==
    NET_GSM
  )
  {
    Serial.println(
      "GSM"
    );
  }
  else
  {
    Serial.println(
      "NONE"
    );
  }

  Serial.println(
    "================================="
  );

  sendESPNow(
    "C3_STATUS"
  );
}

// ============================================================
// COMMAND PROCESSOR
// ============================================================

void processCommand(
  String command
)
{
  command.trim();

  String upper =
    command;

  upper.toUpperCase();

  if (
    upper == "HELP"
  )
  {
    Serial.println();
    Serial.println(
      "MINICLAW C3 COMMANDS"
    );

    Serial.println(
      "STATUS"
    );

    Serial.println(
      "WIFI SCAN"
    );

    Serial.println(
      "WIFI CONNECT"
    );

    Serial.println(
      "GSM STATUS"
    );

    Serial.println(
      "GSM APN <apn>"
    );

    Serial.println(
      "GSM CONNECT"
    );

    Serial.println(
      "NETWORK"
    );

    Serial.println(
      "MIC"
    );

    Serial.println(
      "SAVE"
    );

    Serial.println(
      "RESET"
    );

    return;
  }

  if (
    upper == "STATUS"
  )
  {
    printStatus();

    return;
  }

  if (
    upper == "WIFI SCAN"
  )
  {
    scanWiFi();

    return;
  }

  if (
    upper == "WIFI CONNECT"
  )
  {
    connectWiFi();

    return;
  }

  if (
    upper == "GSM STATUS"
  )
  {
    gsmStatus();

    return;
  }

  if (
    upper.startsWith(
      "GSM APN "
    )
  )
  {
    gsmAPN =
      command.substring(
        8
      );

    gsmAPN.trim();

    Serial.print(
      "[GSM] APN = "
    );

    Serial.println(
      gsmAPN
    );

    return;
  }

  if (
    upper == "GSM CONNECT"
  )
  {
    connectGSM();

    return;
  }

  if (
    upper == "NETWORK"
  )
  {
    networkManager();

    printStatus();

    return;
  }

  if (
    upper == "MIC"
  )
  {
    printMicrophone();

    return;
  }

  if (
    upper == "SAVE"
  )
  {
    saveConfiguration();

    return;
  }

  if (
    upper == "RESET"
  )
  {
    Serial.println(
      "[SYSTEM] Restarting..."
    );

    delay(500);

    ESP.restart();

    return;
  }

  Serial.println(
    "Unknown command. Type HELP."
  );
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(
    115200
  );

  delay(1500);

  Serial.println();
  Serial.println(
    "=========================================="
  );

  Serial.println(
    "       MINICLAW ESP32-C3 NETWORK"
  );

  Serial.println(
    "       ESP-NOW + WIFI + GSM + MIC"
  );

  Serial.println(
    "=========================================="
  );

  // Configuration

  loadConfiguration();

  // SIM800L

  SIM800.begin(
    SIM800_BAUD,
    SERIAL_8N1,
    SIM800_RX,
    SIM800_TX
  );

  delay(1000);

  // Microphone

  microphoneOK =
    initMicrophone();

  // ESP-NOW

  initESPNow();

  // Wi-Fi

  WiFi.mode(
    WIFI_STA
  );

  // Automatic Wi-Fi

  if (
    wifiSSID.length() > 0
  )
  {
    connectWiFi();
  }

  // GSM fallback

  if (
    !wifiOK &&
    gsmAPN.length() > 0
  )
  {
    connectGSM();
  }

  printStatus();

  Serial.println();
  Serial.println(
    "C3 NETWORK + MICROPHONE READY"
  );

  Serial.println(
    "Type HELP"
  );
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
  // Serial Monitor

  if (
    Serial.available()
  )
  {
    String command =
      Serial.readStringUntil(
        '\n'
      );

    processCommand(
      command
    );
  }

  // SIM800L output

  while (
    SIM800.available()
  )
  {
    Serial.write(
      SIM800.read()
    );
  }

  // Network manager

  if (
    millis() -
    lastNetworkCheck >=
    10000
  )
  {
    lastNetworkCheck =
      millis();

    networkManager();
  }

  // Microphone monitoring

  if (
    millis() -
    lastMicPrint >=
    3000
  )
  {
    lastMicPrint =
      millis();

    if (
      microphoneOK
    )
    {
      float rms =
        readMicrophone();

      Serial.print(
        "[MIC] RMS = "
      );

      Serial.println(
        rms,
        0
      );
    }
  }

  // Periodic C3 status

  if (
    millis() -
    lastStatusSend >=
    15000
  )
  {
    lastStatusSend =
      millis();

    String status =
      "C3 WIFI=" +
      String(
        wifiOK ? "1" : "0"
      ) +
      " GSM=" +
      String(
        gsmOK ? "1" : "0"
      ) +
      " MIC=" +
      String(
        microphoneOK ? "1" : "0"
      );

    sendESPNow(
      status
    );
  }

  delay(5);
}
