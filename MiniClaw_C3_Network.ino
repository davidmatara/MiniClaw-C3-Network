// ============================================================
// MINICLAW ESP32-C3 NETWORK
// CONTINUATION — ESP-NOW + WIFI + GSM + MIC
// ============================================================

#include <WiFi.h>
#include <esp_now.h>

// ------------------------------------------------------------
// ESP-NOW
// ------------------------------------------------------------

#define ESPNOW_CHANNEL 6

// Replace with ESP32-S3 MAC later.
// Broadcast is useful during initial testing.
uint8_t S3_MAC[] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

uint32_t packetSequence = 0;

// ------------------------------------------------------------
// PACKET TYPES
// ------------------------------------------------------------

#define PACKET_MAGIC       0x4D434C57
#define MAX_PACKET_DATA    180

#define CMD_PING            1
#define CMD_PONG            2
#define CMD_C3_STATUS       3

#define CMD_WIFI_SCAN      10
#define CMD_WIFI_SCAN_RESULT 11
#define CMD_WIFI_CONNECT   12
#define CMD_WIFI_RESULT    13
#define CMD_WIFI_STATUS    14
#define CMD_WIFI_DISCONNECT 15

#define CMD_GSM_STATUS     20
#define CMD_MIC_STATUS     21
#define CMD_NETWORK_STATUS 22

#define CMD_TEXT            30

// ------------------------------------------------------------
// PACKET STRUCTURE
// ------------------------------------------------------------

typedef struct
{
  uint32_t magic;
  uint16_t type;
  uint16_t source;
  uint32_t sequence;
  uint16_t length;
  char data[MAX_PACKET_DATA];

} MiniClawPacket;

#define DEVICE_C3  3
#define DEVICE_S3  1

// ------------------------------------------------------------
// DEVICE STATUS
// ------------------------------------------------------------

bool espNowOK = false;

bool wifiConnected = false;
bool gsmAvailable = false;
bool microphoneAvailable = false;

String wifiSSID = "";
String wifiIP = "";

// ------------------------------------------------------------
// ESP-NOW SEND
// ------------------------------------------------------------

bool sendPacket(
  uint16_t type,
  const String &message
)
{
  MiniClawPacket packet;

  memset(
    &packet,
    0,
    sizeof(packet)
  );

  packet.magic = PACKET_MAGIC;
  packet.type = type;
  packet.source = DEVICE_C3;
  packet.sequence = packetSequence++;

  packet.length =
    min(
      (int)message.length(),
      MAX_PACKET_DATA - 1
    );

  memcpy(
    packet.data,
    message.c_str(),
    packet.length
  );

  packet.data[
    packet.length
  ] = '\0';

  esp_err_t result =
    esp_now_send(
      S3_MAC,
      (uint8_t *)&packet,
      sizeof(packet)
    );

  return result == ESP_OK;
}

// ------------------------------------------------------------
// ESP-NOW RECEIVE
// ------------------------------------------------------------

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
    Serial.println(
      "[ESP-NOW] Invalid packet size"
    );

    return;
  }

  MiniClawPacket packet;

  memcpy(
    &packet,
    data,
    sizeof(packet)
  );

  if (
    packet.magic != PACKET_MAGIC
  )
  {
    Serial.println(
      "[ESP-NOW] Invalid packet"
    );

    return;
  }

  String message =
    String(packet.data);

  Serial.print(
    "[ESP-NOW RX] CMD="
  );

  Serial.print(
    packet.type
  );

  Serial.print(
    " DATA="
  );

  Serial.println(
    message
  );

  // ----------------------------------------------------------
  // PING
  // ----------------------------------------------------------

  if (
    packet.type == CMD_PING
  )
  {
    sendPacket(
      CMD_PONG,
      "C3:PONG"
    );

    return;
  }

  // ----------------------------------------------------------
  // WIFI SCAN
  // ----------------------------------------------------------

  if (
    packet.type == CMD_WIFI_SCAN
  )
  {
    scanWiFi();

    return;
  }

  // ----------------------------------------------------------
  // WIFI CONNECT
  // ----------------------------------------------------------

  if (
    packet.type == CMD_WIFI_CONNECT
  )
  {
    int separator =
      message.indexOf('|');

    if (
      separator > 0
    )
    {
      String ssid =
        message.substring(
          0,
          separator
        );

      String password =
        message.substring(
          separator + 1
        );

      connectWiFi(
        ssid,
        password
      );
    }

    return;
  }

  // ----------------------------------------------------------
  // WIFI STATUS
  // ----------------------------------------------------------

  if (
    packet.type == CMD_WIFI_STATUS
  )
  {
    sendWiFiStatus();

    return;
  }

  // ----------------------------------------------------------
  // WIFI DISCONNECT
  // ----------------------------------------------------------

  if (
    packet.type == CMD_WIFI_DISCONNECT
  )
  {
    WiFi.disconnect();

    wifiConnected = false;

    sendPacket(
      CMD_WIFI_RESULT,
      "DISCONNECTED"
    );

    return;
  }

  // ----------------------------------------------------------
  // GSM STATUS
  // ----------------------------------------------------------

  if (
    packet.type == CMD_GSM_STATUS
  )
  {
    sendGSMStatus();

    return;
  }

  // ----------------------------------------------------------
  // MIC STATUS
  // ----------------------------------------------------------

  if (
    packet.type == CMD_MIC_STATUS
  )
  {
    sendMicStatus();

    return;
  }
}

// ============================================================
// WIFI SCANNER
// ============================================================

void scanWiFi()
{
  Serial.println();
  Serial.println(
    "[WIFI] Scanning..."
  );

  int count =
    WiFi.scanNetworks();

  if (
    count <= 0
  )
  {
    Serial.println(
      "[WIFI] No networks found"
    );

    sendPacket(
      CMD_WIFI_SCAN_RESULT,
      "NO_NETWORKS"
    );

    return;
  }

  Serial.print(
    "[WIFI] Found "
  );

  Serial.print(
    count
  );

  Serial.println(
    " networks"
  );

  for (
    int i = 0;
    i < count;
    i++
  )
  {
    String result =
      String(i + 1) +
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
      CMD_WIFI_SCAN_RESULT,
      result
    );

    delay(30);
  }

  WiFi.scanDelete();

  sendPacket(
    CMD_WIFI_SCAN_RESULT,
    "SCAN_COMPLETE"
  );
}

// ============================================================
// WIFI CONNECT
// ============================================================

void connectWiFi(
  String ssid,
  String password
)
{
  Serial.println();
  Serial.print(
    "[WIFI] Connecting to "
  );

  Serial.println(
    ssid
  );

  WiFi.disconnect();

  delay(200);

  WiFi.mode(
    WIFI_STA
  );

  WiFi.begin(
    ssid.c_str(),
    password.c_str()
  );

  unsigned long start =
    millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 20000
  )
  {
    delay(500);

    Serial.print(".");
  }

  Serial.println();

  if (
    WiFi.status() == WL_CONNECTED
  )
  {
    wifiConnected = true;

    wifiSSID =
      WiFi.SSID();

    wifiIP =
      WiFi.localIP().toString();

    Serial.println(
      "[WIFI] CONNECTED"
    );

    Serial.print(
      "[WIFI] IP: "
    );

    Serial.println(
      wifiIP
    );

    sendPacket(
      CMD_WIFI_RESULT,
      "CONNECTED|" +
      wifiSSID +
      "|" +
      wifiIP
    );
  }
  else
  {
    wifiConnected = false;

    Serial.println(
      "[WIFI] CONNECTION FAILED"
    );

    sendPacket(
      CMD_WIFI_RESULT,
      "FAILED"
    );
  }
}

// ============================================================
// WIFI STATUS
// ============================================================

void sendWiFiStatus()
{
  if (
    WiFi.status() ==
    WL_CONNECTED
  )
  {
    wifiConnected = true;

    String result =
      "CONNECTED|" +
      WiFi.SSID() +
      "|" +
      WiFi.localIP().toString();

    sendPacket(
      CMD_WIFI_STATUS,
      result
    );
  }
  else
  {
    wifiConnected = false;

    sendPacket(
      CMD_WIFI_STATUS,
      "DISCONNECTED"
    );
  }
}

// ============================================================
// GSM STATUS
// ============================================================

void sendGSMStatus()
{
  if (
    gsmAvailable
  )
  {
    sendPacket(
      CMD_GSM_STATUS,
      "AVAILABLE"
    );
  }
  else
  {
    sendPacket(
      CMD_GSM_STATUS,
      "NOT_AVAILABLE"
    );
  }
}

// ============================================================
// MICROPHONE STATUS
// ============================================================

void sendMicStatus()
{
  if (
    microphoneAvailable
  )
  {
    sendPacket(
      CMD_MIC_STATUS,
      "AVAILABLE"
    );
  }
  else
  {
    sendPacket(
      CMD_MIC_STATUS,
      "NOT_AVAILABLE"
    );
  }
}

// ============================================================
// NETWORK STATUS
// ============================================================

void sendNetworkStatus()
{
  String result;

  if (
    wifiConnected
  )
  {
    result =
      "WIFI|" +
      WiFi.SSID() +
      "|" +
      WiFi.localIP().toString();
  }
  else if (
    gsmAvailable
  )
  {
    result =
      "GSM";
  }
  else
  {
    result =
      "OFFLINE";
  }

  sendPacket(
    CMD_NETWORK_STATUS,
    result
  );
}

// ============================================================
// ESP-NOW INITIALIZATION
// ============================================================

bool initESPNow()
{
  Serial.println(
    "[ESP-NOW] Starting..."
  );

  WiFi.mode(
    WIFI_STA
  );

  WiFi.disconnect();

  if (
    esp_now_init() != ESP_OK
  )
  {
    Serial.println(
      "[ESP-NOW] Initialization failed"
    );

    return false;
  }

  esp_now_register_recv_cb(
    onDataReceive
  );

  esp_now_peer_info_t peer;

  memset(
    &peer,
    0,
    sizeof(peer)
  );

  memcpy(
    peer.peer_addr,
    S3_MAC,
    6
  );

  peer.channel =
    ESPNOW_CHANNEL;

  peer.encrypt =
    false;

  if (
    esp_now_is_peer_exist(
      S3_MAC
    )
  )
  {
    esp_now_del_peer(
      S3_MAC
    );
  }

  if (
    esp_now_add_peer(
      &peer
    ) != ESP_OK
  )
  {
    Serial.println(
      "[ESP-NOW] Peer failed"
    );

    return false;
  }

  espNowOK = true;

  Serial.println(
    "[ESP-NOW] READY"
  );

  return true;
}

// ============================================================
// SERIAL COMMANDS
// ============================================================

void printHelp()
{
  Serial.println();
  Serial.println(
    "========== MINICLAW C3 =========="
  );

  Serial.println(
    "PING"
  );

  Serial.println(
    "SCAN"
  );

  Serial.println(
    "CONNECT <ssid>|<password>"
  );

  Serial.println(
    "WIFI STATUS"
  );

  Serial.println(
    "WIFI OFF"
  );

  Serial.println(
    "GSM STATUS"
  );

  Serial.println(
    "MIC STATUS"
  );

  Serial.println(
    "NETWORK STATUS"
  );

  Serial.println(
    "STATUS"
  );

  Serial.println(
    "MAC"
  );

  Serial.println(
    "HELP"
  );

  Serial.println(
    "================================="
  );

  Serial.println();
}

// ============================================================
// SERIAL COMMAND PROCESSOR
// ============================================================

void processSerialCommand(
  String command
)
{
  command.trim();

  String upper =
    command;

  upper.toUpperCase();

  // ----------------------------------------------------------
  // HELP
  // ----------------------------------------------------------

  if (
    upper == "HELP"
  )
  {
    printHelp();

    return;
  }

  // ----------------------------------------------------------
  // PING
  // ----------------------------------------------------------

  if (
    upper == "PING"
  )
  {
    if (
      sendPacket(
        CMD_PING,
        "C3:PING"
      )
    )
    {
      Serial.println(
        "[ESP-NOW] PING sent"
      );
    }
    else
    {
      Serial.println(
        "[ESP-NOW] PING failed"
      );
    }

    return;
  }

  // ----------------------------------------------------------
  // SCAN
  // ----------------------------------------------------------

  if (
    upper == "SCAN"
  )
  {
    scanWiFi();

    return;
  }

  // ----------------------------------------------------------
  // CONNECT
  // ----------------------------------------------------------

  if (
    upper.startsWith(
      "CONNECT "
    )
  )
  {
    String credentials =
      command.substring(8);

    int separator =
      credentials.indexOf('|');

    if (
      separator < 1
    )
    {
      Serial.println(
        "Use: CONNECT SSID|PASSWORD"
      );

      return;
    }

    String ssid =
      credentials.substring(
        0,
        separator
      );

    String password =
      credentials.substring(
        separator + 1
      );

    connectWiFi(
      ssid,
      password
    );

    return;
  }

  // ----------------------------------------------------------
  // WIFI STATUS
  // ----------------------------------------------------------

  if (
    upper == "WIFI STATUS"
  )
  {
    sendWiFiStatus();

    return;
  }

  // ----------------------------------------------------------
  // WIFI OFF
  // ----------------------------------------------------------

  if (
    upper == "WIFI OFF"
  )
  {
    WiFi.disconnect();

    wifiConnected = false;

    Serial.println(
      "[WIFI] Disconnected"
    );

    return;
  }

  // ----------------------------------------------------------
  // GSM STATUS
  // ----------------------------------------------------------

  if (
    upper == "GSM STATUS"
  )
  {
    sendGSMStatus();

    return;
  }

  // ----------------------------------------------------------
  // MIC STATUS
  // ----------------------------------------------------------

  if (
    upper == "MIC STATUS"
  )
  {
    sendMicStatus();

    return;
  }

  // ----------------------------------------------------------
  // NETWORK STATUS
  // ----------------------------------------------------------

  if (
    upper == "NETWORK STATUS"
  )
  {
    sendNetworkStatus();

    return;
  }

  // ----------------------------------------------------------
  // MAC ADDRESS
  // ----------------------------------------------------------

  if (
    upper == "MAC"
  )
  {
    Serial.print(
      "[C3 MAC] "
    );

    Serial.println(
      WiFi.macAddress()
    );

    return;
  }

  // ----------------------------------------------------------
  // STATUS
  // ----------------------------------------------------------

  if (
    upper == "STATUS"
  )
  {
    Serial.println();
    Serial.println(
      "========== C3 STATUS =========="
    );

    Serial.print(
      "ESP-NOW: "
    );

    Serial.println(
      espNowOK
        ? "OK"
        : "ERROR"
    );

    Serial.print(
      "WiFi: "
    );

    Serial.println(
      wifiConnected
        ? "CONNECTED"
        : "DISCONNECTED"
    );

    Serial.print(
      "GSM: "
    );

    Serial.println(
      gsmAvailable
        ? "AVAILABLE"
        : "NOT AVAILABLE"
    );

    Serial.print(
      "MIC: "
    );

    Serial.println(
      microphoneAvailable
        ? "AVAILABLE"
        : "NOT AVAILABLE"
    );

    Serial.println(
      "================================"
    );

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
    "======================================"
  );

  Serial.println(
    "       MINICLAW ESP32-C3"
  );

  Serial.println(
    "       NETWORK CONTROLLER"
  );

  Serial.println(
    "======================================"
  );

  // ----------------------------------------------------------
  // WIFI
  // ----------------------------------------------------------

  WiFi.mode(
    WIFI_STA
  );

  Serial.print(
    "[C3 MAC] "
  );

  Serial.println(
    WiFi.macAddress()
  );

  // ----------------------------------------------------------
  // ESP-NOW
  // ----------------------------------------------------------

  espNowOK =
    initESPNow();

  // ----------------------------------------------------------
  // CURRENT WIFI STATE
  // ----------------------------------------------------------

  if (
    WiFi.status() ==
    WL_CONNECTED
  )
  {
    wifiConnected = true;
  }

  // ----------------------------------------------------------
  // INITIAL STATUS
  // ----------------------------------------------------------

  Serial.println();

  Serial.println(
    "[C3] System ready."
  );

  Serial.println(
    "Type HELP for commands."
  );

  Serial.println();

  // Send initial status to S3

  sendPacket(
    CMD_C3_STATUS,
    "C3_READY"
  );

  sendNetworkStatus();
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
  // ----------------------------------------------------------
  // SERIAL MONITOR
  // ----------------------------------------------------------

  if (
    Serial.available()
  )
  {
    String command =
      Serial.readStringUntil(
        '\n'
      );

    processSerialCommand(
      command
    );
  }

  // ----------------------------------------------------------
  // WIFI STATE MONITOR
  // ----------------------------------------------------------

  bool currentWiFi =
    WiFi.status() ==
    WL_CONNECTED;

  if (
    currentWiFi !=
    wifiConnected
  )
  {
    wifiConnected =
      currentWiFi;

    if (
      wifiConnected
    )
    {
      Serial.println(
        "[WIFI] Connection detected"
      );
    }
    else
    {
      Serial.println(
        "[WIFI] Connection lost"
      );
    }

    sendNetworkStatus();
  }

  delay(10);
}
