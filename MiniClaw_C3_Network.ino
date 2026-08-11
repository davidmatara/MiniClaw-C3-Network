/*
============================================================
              MINICLAW ESP32-C3
          FINAL NETWORK + AUDIO NODE
============================================================

RESPONSIBILITIES
----------------
- Wi-Fi scanning
- Wi-Fi connection
- Saved Wi-Fi credentials
- GSM / SIM800L
- Wi-Fi -> GSM fallback
- ESP-NOW communication
- INMP441 microphone
- Network status
- Microphone level
- Commands from S3

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
SERIAL MONITOR
============================================================

115200 baud

COMMANDS
--------
HELP
STATUS
WIFI SCAN
WIFI SSID <name>
WIFI PASSWORD <password>
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
#define MIC_BUFFER_SAMPLES 256

int32_t micBuffer[MIC_BUFFER_SAMPLES];

size_t micBytesRead = 0;

bool microphoneOK = false;

// ============================================================
// STORAGE
// ============================================================

Preferences prefs;

String wifiSSID;
String wifiPassword;
String gsmAPN;

// ============================================================
// NETWORK STATE
// ============================================================

enum NetworkType
{
  NETWORK_NONE,
  NETWORK_WIFI,
  NETWORK_GSM
};

NetworkType activeNetwork = NETWORK_NONE;

bool wifiOK = false;
bool gsmOK = false;

// ============================================================
// TIMERS
// ============================================================

unsigned long lastNetworkCheck = 0;
unsigned long lastMicCheck = 0;
unsigned long lastStatus = 0;

// ============================================================
// ESP-NOW PROTOCOL
// ============================================================

#define ESPNOW_MAGIC 0x4D43
#define ESPNOW_VERSION 1

enum MessageType
{
  MSG_PING = 1,
  MSG_PONG,

  MSG_STATUS_REQUEST,
  MSG_STATUS_RESPONSE,

  MSG_WIFI_SCAN,
  MSG_WIFI_SCAN_RESULT,

  MSG_WIFI_CONNECT,
  MSG_WIFI_CONNECTED,
  MSG_WIFI_FAILED,

  MSG_GSM_STATUS,
  MSG_GSM_CONNECT,
  MSG_GSM_CONNECTED,
  MSG_GSM_FAILED,

  MSG_NETWORK_STATUS,

  MSG_MIC_STATUS,
  MSG_MIC_LEVEL,

  MSG_AUDIO_REQUEST,
  MSG_AUDIO_DATA,

  MSG_TEXT
};

struct MiniClawPacket
{
  uint16_t magic;
  uint8_t version;
  uint8_t type;
  uint16_t sequence;
  uint16_t length;
  char data[220];
};

MiniClawPacket txPacket;
MiniClawPacket rxPacket;

uint16_t packetSequence = 0;

uint8_t broadcastAddress[] =
{
  0xFF,
  0xFF,
  0xFF,
  0xFF,
  0xFF,
  0xFF
};

// ============================================================
// ESP-NOW SEND
// ============================================================

void sendPacket(
  uint8_t type,
  const String &data
)
{
  memset(
    &txPacket,
    0,
    sizeof(txPacket)
  );

  txPacket.magic =
    ESPNOW_MAGIC;

  txPacket.version =
    ESPNOW_VERSION;

  txPacket.type =
    type;

  txPacket.sequence =
    packetSequence++;

  String limitedData =
    data;

  if (
    limitedData.length() >
    sizeof(txPacket.data) - 1
  )
  {
    limitedData =
      limitedData.substring(
        0,
        sizeof(txPacket.data) - 1
      );
  }

  txPacket.length =
    limitedData.length();

  limitedData.toCharArray(
    txPacket.data,
    sizeof(txPacket.data)
  );

  esp_err_t result =
    esp_now_send(
      broadcastAddress,
      (uint8_t *)&txPacket,
      sizeof(txPacket)
    );

  if (
    result != ESP_OK
  )
  {
    Serial.print(
      "[ESP-NOW] Send error: "
    );

    Serial.println(
      esp_err_to_name(result)
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
    len != sizeof(MiniClawPacket)
  )
  {
    return;
  }

  memcpy(
    &rxPacket,
    data,
    sizeof(rxPacket)
  );

  if (
    rxPacket.magic !=
    ESPNOW_MAGIC
  )
  {
    return;
  }

  if (
    rxPacket.version !=
    ESPNOW_VERSION
  )
  {
    return;
  }

  String command =
    String(
      rxPacket.data
    );

  command.trim();

  Serial.print(
    "[ESP-NOW] RX type="
  );

  Serial.print(
    rxPacket.type
  );

  Serial.print(
    " data="
  );

  Serial.println(
    command
  );

  switch (
    rxPacket.type
  )
  {
    case MSG_PING:

      sendPacket(
        MSG_PONG,
        "C3_PONG"
      );

      break;

    case MSG_STATUS_REQUEST:

      sendStatusPacket();

      break;

    case MSG_WIFI_SCAN:

      scanWiFi();

      break;

    case MSG_WIFI_CONNECT:

      connectWiFi();

      break;

    case MSG_GSM_STATUS:

      gsmStatus();

      break;

    case MSG_GSM_CONNECT:

      connectGSM();

      break;

    case MSG_NETWORK_STATUS:

      sendNetworkStatus();

      break;

    case MSG_MIC_STATUS:

      sendMicStatus();

      break;

    case MSG_AUDIO_REQUEST:

      sendMicLevel();

      break;

    case MSG_TEXT:

      Serial.print(
        "[S3] "
      );

      Serial.println(
        command
      );

      break;

    default:

      Serial.println(
        "[ESP-NOW] Unknown message"
      );

      break;
  }
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
      "[ESP-NOW] INIT FAILED"
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
      "[ESP-NOW] PEER FAILED"
    );

    return false;
  }

  Serial.println(
    "[ESP-NOW] READY"
  );

  sendPacket(
    MSG_TEXT,
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
// MICROPHONE READ
// ============================================================

float readMicrophone()
{
  if (
    !microphoneOK
  )
  {
    return 0;
  }

  micBytesRead = 0;

  esp_err_t result =
    i2s_read(
      I2S_PORT,
      micBuffer,
      sizeof(micBuffer),
      &micBytesRead,
      pdMS_TO_TICKS(50)
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
  {
    return 0;
  }

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

void sendMicStatus()
{
  if (
    microphoneOK
  )
  {
    sendPacket(
      MSG_MIC_STATUS,
      "MIC_READY"
    );
  }
  else
  {
    sendPacket(
      MSG_MIC_STATUS,
      "MIC_ERROR"
    );
  }
}

void sendMicLevel()
{
  float rms =
    readMicrophone();

  String data =
    "RMS=" +
    String(
      rms,
      0
    );

  sendPacket(
    MSG_MIC_LEVEL,
    data
  );

  Serial.print(
    "[MIC] "
  );

  Serial.println(
    data
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

  Serial.println(
    "[CONFIG] Loaded"
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
    WiFi.scanNetworks(
      false,
      true
    );

  if (
    count <= 0
  )
  {
    Serial.println(
      "No networks found."
    );

    sendPacket(
      MSG_WIFI_SCAN_RESULT,
      "COUNT=0"
    );

    return;
  }

  for (
    int i = 0;
    i < count;
    i++
  )
  {
    String result =
      String(i) +
      "|" +
      WiFi.SSID(i) +
      "|" +
      String(
        WiFi.RSSI(i)
      );

    Serial.println(
      result
    );

    sendPacket(
      MSG_WIFI_SCAN_RESULT,
      result
    );

    delay(20);
  }

  Serial.println(
    "=============================="
  );

  WiFi.scanDelete();
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
      "[WIFI] SSID not configured"
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
    WiFi.status() !=
      WL_CONNECTED &&
    millis() - start <
      15000
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
      NETWORK_WIFI;

    Serial.println(
      "[WIFI] CONNECTED"
    );

    Serial.print(
      "[WIFI] IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    sendPacket(
      MSG_WIFI_CONNECTED,
      WiFi.localIP().toString()
    );

    return true;
  }

  wifiOK = false;

  Serial.println(
    "[WIFI] FAILED"
  );

  sendPacket(
    MSG_WIFI_FAILED,
    "WIFI_FAILED"
  );

  return false;
}

// ============================================================
// SIM800 COMMAND
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

  String response;

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
  Serial.println(
    "[GSM] STATUS"
  );

  bool modem =
    simCommand(
      "AT",
      "OK",
      2000
    );

  if (
    !modem
  )
  {
    gsmOK = false;

    sendPacket(
      MSG_GSM_STATUS,
      "MODEM_ERROR"
    );

    return;
  }

  simCommand(
    "AT+CPIN?",
    "OK",
    3000
  );

  simCommand(
    "AT+CSQ",
    "OK",
    3000
  );

  simCommand(
    "AT+CREG?",
    "OK",
    3000
  );

  sendPacket(
    MSG_GSM_STATUS,
    "MODEM_OK"
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

    sendPacket(
      MSG_GSM_FAILED,
      "APN_NOT_SET"
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
    gsmOK = false;

    sendPacket(
      MSG_GSM_FAILED,
      "MODEM_ERROR"
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
    gsmOK = false;

    sendPacket(
      MSG_GSM_FAILED,
      "APN_FAILED"
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
    gsmOK = false;

    sendPacket(
      MSG_GSM_FAILED,
      "GPRS_FAILED"
    );

    return false;
  }

  gsmOK = true;

  activeNetwork =
    NETWORK_GSM;

  Serial.println(
    "[GSM] GPRS CONNECTED"
  );

  sendPacket(
    MSG_GSM_CONNECTED,
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
      NETWORK_WIFI;

    return;
  }

  wifiOK = false;

  if (
    gsmOK
  )
  {
    activeNetwork =
      NETWORK_GSM;

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
// STATUS PACKET
// ============================================================

String networkName()
{
  if (
    activeNetwork ==
    NETWORK_WIFI
  )
  {
    return "WIFI";
  }

  if (
    activeNetwork ==
    NETWORK_GSM
  )
  {
    return "GSM";
  }

  return "NONE";
}

void sendNetworkStatus()
{
  String data =
    "WIFI=" +
    String(
      wifiOK ? "1" : "0"
    ) +
    ",GSM=" +
    String(
      gsmOK ? "1" : "0"
    ) +
    ",ACTIVE=" +
    networkName();

  sendPacket(
    MSG_NETWORK_STATUS,
    data
  );
}

void sendStatusPacket()
{
  String data =
    "WIFI=" +
    String(
      wifiOK ? "1" : "0"
    ) +
    ",GSM=" +
    String(
      gsmOK ? "1" : "0"
    ) +
    ",MIC=" +
    String(
      microphoneOK ? "1" : "0"
    ) +
    ",NET=" +
    networkName();

  sendPacket(
    MSG_STATUS_RESPONSE,
    data
  );
}

// ============================================================
// SERIAL STATUS
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
    "Microphone: "
  );

  Serial.println(
    microphoneOK
      ? "READY"
      : "ERROR"
  );

  Serial.print(
    "Active network: "
  );

  Serial.println(
    networkName()
  );

  Serial.print(
    "Wi-Fi SSID: "
  );

  Serial.println(
    wifiSSID.length()
      ? wifiSSID
      : "NOT SET"
  );

  Serial.print(
    "GSM APN: "
  );

  Serial.println(
    gsmAPN.length()
      ? gsmAPN
      : "NOT SET"
  );

  Serial.println(
    "================================="
  );
}

// ============================================================
// SERIAL COMMANDS
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
      "MINICLAW C3 COMMANDS:"
    );

    Serial.println(
      "STATUS"
    );

    Serial.println(
      "WIFI SCAN"
    );

    Serial.println(
      "WIFI SSID <name>"
    );

    Serial.println(
      "WIFI PASSWORD <password>"
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
    upper.startsWith(
      "WIFI SSID "
    )
  )
  {
    wifiSSID =
      command.substring(
        10
      );

    wifiSSID.trim();

    Serial.print(
      "[WIFI] SSID = "
    );

    Serial.println(
      wifiSSID
    );

    return;
  }

  if (
    upper.startsWith(
      "WIFI PASSWORD "
    )
  )
  {
    wifiPassword =
      command.substring(
        14
      );

    wifiPassword.trim();

    Serial.println(
      "[WIFI] Password stored in memory."
    );

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

    re
