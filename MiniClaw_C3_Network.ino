/*
  ============================================================
  MINICLAW ESP32-C3 GATEWAY
  ============================================================

  ESP32-C3 + SIM800L + hidden Wi-Fi + ESP-NOW

  FEATURES
  --------
  1. SIM800L GPRS Internet
  2. Hidden Wi-Fi AP for ESP32-S3 only
  3. S3 MAC-address filtering
  4. ESP-NOW C3 <-> S3 communication
  5. HTTP proxy through SIM800L
  6. SMS sending from S3
  7. SMS reception forwarded to S3 through ESP-NOW
  8. Phone calls controlled from S3 through ESP-NOW
  9. Incoming call notifications forwarded to S3
  10. Hang-up from S3
  11. OTA over the hidden Wi-Fi
  12. Audio packets over ESP-NOW
  13. All configuration through Serial Monitor
  14. No recompilation required for APN, phone number,
      S3 MAC, Wi-Fi name/password, OTA settings, etc.

  IMPORTANT
  ---------
  The Wi-Fi network is HIDDEN.

  It is NOT possible to make an ESP32 Wi-Fi AP completely
  invisible at the radio level while still using it as an AP.
  However, this firmware:

      - hides the SSID
      - requires a password
      - only accepts the configured ESP32-S3 MAC
      - disconnects unauthorized Wi-Fi clients

  Therefore a normal phone Wi-Fi list will not show the
  network by its SSID, and unauthorized devices are rejected.

  The ESP32-S3 must connect using the configured SSID manually.

  IMPORTANT ABOUT "ONLINE S3"
  ---------------------------
  The C3 is implementing a CELLULAR PROXY/GATEWAY.

  The path is:

       Internet
          |
       SIM800L
          |
       GPRS
          |
       ESP32-C3
          |
       hidden Wi-Fi
          |
       ESP32-S3

  This is NOT a full IP NAT router.

  The S3 can use the C3 proxy endpoint for HTTP requests.

  ============================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>

#include <esp_now.h>
#include <esp_wifi.h>

#define TINY_GSM_MODEM_SIM800
#include <TinyGsmClient.h>


// ============================================================
// GENERAL SETTINGS
// ============================================================

#define RADIO_CHANNEL 1

#define DEFAULT_WIFI_SSID       "MINICLAW-C3"
#define DEFAULT_WIFI_PASSWORD   "miniclaw123"

#define DEFAULT_OTA_HOSTNAME    "miniclaw-c3"

#define AP_IP       IPAddress(192,168,4,1)
#define AP_GATEWAY  IPAddress(192,168,4,1)
#define AP_SUBNET   IPAddress(255,255,255,0)


// ============================================================
// SIM800L
// ============================================================

#define MODEM_TX_PIN 6
#define MODEM_RX_PIN 7

#define MODEM_BAUD 115200

HardwareSerial ModemSerial(1);

TinyGsm modem(ModemSerial);

TinyGsmClient cellularClient(modem);


// ============================================================
// HTTP
// ============================================================

WebServer server(80);


// ============================================================
// CONFIGURATION STORAGE
// ============================================================

Preferences preferences;

String wifiSSID;
String wifiPassword;

String cellularAPN;
String cellularUser;
String cellularPassword;
String cellularPIN;

String savedPhoneNumber;

String otaHostname;
String otaPassword;

bool cellularEnabled = true;
bool otaEnabled = true;

bool cellularReady = false;
bool cellularRegistered = false;
bool cellularDataReady = false;
bool otaReady = false;


// ============================================================
// ESP32-S3 MAC
// ============================================================

uint8_t s3MAC[6] =
{
  0,0,0,0,0,0
};


// ============================================================
// ESP-NOW PROTOCOL
// ============================================================

#define NOW_MAGIC 0xC3A5

#define NOW_CMD_CALL        1
#define NOW_CMD_HANGUP      2
#define NOW_CMD_SMS         3
#define NOW_CMD_SMS_READ    4
#define NOW_CMD_STATUS      5
#define NOW_CMD_HTTP        6

#define NOW_EVENT_SMS       20
#define NOW_EVENT_CALL     21
#define NOW_EVENT_CALL_END 22
#define NOW_EVENT_STATUS   23
#define NOW_EVENT_ERROR    24
#define NOW_EVENT_HTTP      25

#define NOW_MAX_DATA 220

struct __attribute__((packed))
NowPacket
{
  uint16_t magic;
  uint8_t type;
  uint16_t sequence;

  uint8_t data[NOW_MAX_DATA];
};


// ============================================================
// ESP-NOW SEQUENCE
// ============================================================

uint16_t nowSequence = 0;


// ============================================================
// AUDIO
// ============================================================

#define AUDIO_OUTPUT_PIN 10

#define AUDIO_PWM_FREQUENCY 312500
#define AUDIO_PWM_RESOLUTION 8

#define AUDIO_PAYLOAD_SIZE 240
#define AUDIO_QUEUE_SIZE 24

#define AUDIO_MAGIC 0xCAFE

#define AUDIO_TYPE_MIC 1
#define AUDIO_TYPE_TTS 2


struct __attribute__((packed))
AudioPacket
{
  uint16_t magic;
  uint16_t sequence;
  uint16_t sampleCount;
  uint8_t type;
  uint8_t reserved;

  uint8_t pcm[AUDIO_PAYLOAD_SIZE];
};


struct AudioBlock
{
  uint16_t length;
  uint8_t type;

  uint8_t data[AUDIO_PAYLOAD_SIZE];
};


QueueHandle_t audioQueue;


// ============================================================
// STATISTICS
// ============================================================

volatile uint32_t packetsReceived = 0;
volatile uint32_t packetsDropped = 0;
volatile uint32_t invalidPackets = 0;
volatile uint32_t bytesReceived = 0;


// ============================================================
// SERIAL INPUT
// ============================================================

String serialCommand;


// ============================================================
// UTILITY
// ============================================================

String macToString(const uint8_t *mac)
{
  char buffer[18];

  snprintf(
    buffer,
    sizeof(buffer),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0],
    mac[1],
    mac[2],
    mac[3],
    mac[4],
    mac[5]
  );

  return String(buffer);
}


bool parseMAC(String input, uint8_t *mac)
{
  input.trim();

  int values[6];

  int result = sscanf(
    input.c_str(),
    "%x:%x:%x:%x:%x:%x",
    &values[0],
    &values[1],
    &values[2],
    &values[3],
    &values[4],
    &values[5]
  );

  if (result != 6)
    return false;

  for (int i = 0; i < 6; i++)
  {
    if (values[i] < 0 || values[i] > 255)
      return false;

    mac[i] = (uint8_t)values[i];
  }

  return true;
}


bool hasS3MAC()
{
  for (int i = 0; i < 6; i++)
  {
    if (s3MAC[i] != 0)
      return true;
  }

  return false;
}


// ============================================================
// CONFIGURATION
// ============================================================

void loadConfiguration()
{
  preferences.begin(
    "miniclaw",
    false
  );


  String storedMAC =
    preferences.getString(
      "s3mac",
      ""
    );

  parseMAC(
    storedMAC,
    s3MAC
  );


  wifiSSID =
    preferences.getString(
      "ssid",
      DEFAULT_WIFI_SSID
    );


  wifiPassword =
    preferences.getString(
      "wpass",
      DEFAULT_WIFI_PASSWORD
    );


  cellularEnabled =
    preferences.getBool(
      "cell_enabled",
      true
    );


  cellularAPN =
    preferences.getString(
      "apn",
      ""
    );


  cellularUser =
    preferences.getString(
      "cell_user",
      ""
    );


  cellularPassword =
    preferences.getString(
      "cell_pass",
      ""
    );


  cellularPIN =
    preferences.getString(
      "cell_pin",
      ""
    );


  savedPhoneNumber =
    preferences.getString(
      "phone",
      ""
    );


  otaHostname =
    preferences.getString(
      "ota_host",
      DEFAULT_OTA_HOSTNAME
    );


  otaPassword =
    preferences.getString(
      "ota_pass",
      ""
    );


  otaEnabled =
    preferences.getBool(
      "ota_enabled",
      true
    );
}


void saveConfiguration()
{
  preferences.putString(
    "s3mac",
    macToString(s3MAC)
  );

  preferences.putString(
    "ssid",
    wifiSSID
  );

  preferences.putString(
    "wpass",
    wifiPassword
  );

  preferences.putBool(
    "cell_enabled",
    cellularEnabled
  );

  preferences.putString(
    "apn",
    cellularAPN
  );

  preferences.putString(
    "cell_user",
    cellularUser
  );

  preferences.putString(
    "cell_pass",
    cellularPassword
  );

  preferences.putString(
    "cell_pin",
    cellularPIN
  );

  preferences.putString(
    "phone",
    savedPhoneNumber
  );

  preferences.putString(
    "ota_host",
    otaHostname
  );

  preferences.putString(
    "ota_pass",
    otaPassword
  );

  preferences.putBool(
    "ota_enabled",
    otaEnabled
}


// ============================================================
// ESP-NOW SEND
// ============================================================

bool sendNow(
  uint8_t type,
  String message
)
{
  if (!hasS3MAC())
  {
    Serial.println(
      "ESP-NOW: S3 MAC NOT CONFIGURED"
    );

    return false;
  }


  NowPacket packet;

  memset(
    &packet,
    0,
    sizeof(packet)
  );


  packet.magic =
    NOW_MAGIC;

  packet.type =
    type;

  packet.sequence =
    ++nowSequence;


  if (message.length() >= NOW_MAX_DATA)
    message =
      message.substring(
        0,
        NOW_MAX_DATA - 1
      );


  memcpy(
    packet.data,
    message.c_str(),
    message.length()
  );


  esp_err_t result =
    esp_now_send(
      s3MAC,
      (uint8_t *)&packet,
      sizeof(packet)
    );


  if (result != ESP_OK)
  {
    Serial.print(
      "ESP-NOW SEND ERROR: "
    );

    Serial.println(
      result
    );

    return false;
  }


  return true;
}


// ============================================================
// CONFIGURE ESP-NOW PEER
// ============================================================

void configureS3Peer()
{
  if (!hasS3MAC())
  {
    Serial.println(
      "S3 MAC not configured."
    );

    return;
  }


  esp_now_peer_info_t peer;

  memset(
    &peer,
    0,
    sizeof(peer)
  );


  memcpy(
    peer.peer_addr,
    s3MAC,
    6
  );


  peer.channel =
    RADIO_CHANNEL;

  peer.encrypt =
    false;


  esp_now_del_peer(
    s3MAC
  );


  esp_err_t result =
    esp_now_add_peer(
      &peer
    );


  Serial.print(
    "ESP-NOW S3 peer: "
  );


  Serial.println(
    result == ESP_OK ?
    "READY" :
    "FAILED"
  );
}


// ============================================================
// AUDIO OUTPUT
// ============================================================

void initAudioOutput()
{
  ledcAttach(
    AUDIO_OUTPUT_PIN,
    AUDIO_PWM_FREQUENCY,
    AUDIO_PWM_RESOLUTION
  );

  ledcWrite(
    AUDIO_OUTPUT_PIN,
    128
  );
}


void audioTask(
  void *parameter
)
{
  AudioBlock block;


  while (true)
  {
    if (
      xQueueReceive(
        audioQueue,
        &block,
        portMAX_DELAY
      ) != pdTRUE
    )
      continue;


    for (
      uint16_t i = 0;
      i + 1 < block.length;
      i += 2
    )
    {
      int16_t sample =
        (int16_t)(
          block.data[i] |
          (
            block.data[i + 1]
            << 8
          )
        );


      int pwm =
        (sample >> 8) + 128;


      pwm =
        constrain(
          pwm,
          0,
          255
        );


      ledcWrite(
        AUDIO_OUTPUT_PIN,
        pwm
      );


      delayMicroseconds(62);
    }


    ledcWrite(
      AUDIO_OUTPUT_PIN,
      128
    );
  }
}


// ============================================================
// ESP-NOW RECEIVE
// ============================================================

void onESPNowReceive(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int length
)
{
  if (hasS3MAC())
  {
    if (
      memcmp(
        info->src_addr,
        s3MAC,
        6
      ) != 0
    )
    {
      invalidPackets++;
      return;
    }
  }


  /*
    First determine whether this is an audio packet
    or a command packet.
  */

  if (
    length ==
    sizeof(AudioPacket)
  )
  {
    AudioPacket audio;

    memcpy(
      &audio,
      data,
      sizeof(audio)
    );


    if (
      audio.magic ==
      AUDIO_MAGIC
    )
    {
      uint16_t bytes =
        audio.sampleCount * 2;


      if (
        bytes >
        AUDIO_PAYLOAD_SIZE
      )
      {
        invalidPackets++;
        return;
      }


      AudioBlock block;

      memset(
        &block,
        0,
        sizeof(block)
      );


      block.length =
        bytes;

      block.type =
        audio.type;


      memcpy(
        block.data,
        audio.pcm,
        bytes
      );


      if (
        xQueueSend(
          audioQueue,
          &block,
          0
        ) != pdTRUE
      )
      {
        packetsDropped++;
        return;
      }


      packetsReceived++;
      bytesReceived += bytes;

      return;
    }
  }


  /*
    Otherwise it is an ESP-NOW command packet.
  */

  if (
    length !=
    sizeof(NowPacket)
  )
  {
    invalidPackets++;
    return;
  }


  NowPacket packet;

  memcpy(
    &packet,
    data,
    sizeof(packet)
  );


  if (
    packet.magic !=
    NOW_MAGIC
  )
  {
    invalidPackets++;
    return;
  }


  String command =
    String(
      (char *)packet.data
    );


  command.trim();


  /*
    CALL
  */

  if (
    packet.type ==
    NOW_CMD_CALL
  )
  {
    Serial.print(
      "S3 REQUEST CALL: "
    );

    Serial.println(
      command
    );


    if (
      command.length() == 0
    )
      command =
        savedPhoneNumber;


    if (
      command.length() > 0
    )
    {
      ModemSerial.print(
        "ATD"
      );

      ModemSerial.print(
        command
      );

      ModemSerial.println(
        ";"
      );


      sendNow(
        NOW_EVENT_STATUS,
        "CALLING " + command
      );
    }

    return;
  }


  /*
    HANGUP
  */

  if (
    packet.type ==
    NOW_CMD_HANGUP
  )
  {
    Serial.println(
      "S3 REQUEST HANGUP"
    );


    ModemSerial.println(
      "ATH"
    );


    sendNow(
      NOW_EVENT_CALL_END,
      "HANGUP"
    );

    return;
  }


  /*
    SMS SEND

    Format:

      NUMBER|MESSAGE
  */

  if (
    packet.type ==
    NOW_CMD_SMS
  )
  {
    int separator =
      command.indexOf('|');


    if (separator <= 0)
    {
      sendNow(
        NOW_EVENT_ERROR,
        "SMS FORMAT: NUMBER|MESSAGE"
      );

      return;
    }


    String number =
      command.substring(
        0,
        separator
      );


    String message =
      command.substring(
        separator + 1
      );


    number.trim();


    bool result =
      modem.sendSMS(
        number.c_str(),
        message.c_str()
      );


    sendNow(
      result ?
      NOW_EVENT_STATUS :
      NOW_EVENT_ERROR,
      result ?
      "SMS SENT" :
      "SMS FAILED"
    );


    return;
  }


  /*
    SMS READ

    S3 requests the newest SMS.
  */

  if (
    packet.type ==
    NOW_CMD_SMS_READ
  )
  {
    readSMSFromModem();

    return;
  }


  /*
    STATUS
  */

  if (
    packet.type ==
    NOW_CMD_STATUS
  )
  {
    sendGatewayStatusToS3();

    return;
  }
}


// ============================================================
// ESP-NOW INITIALIZATION
// ============================================================

void initESPNow()
{
  WiFi.mode(
    WIFI_AP_STA
  );


  esp_wifi_set_channel(
    RADIO_CHANNEL,
    WIFI_SECOND_CHAN_NONE
  );


  if (
    esp_now_init() != ESP_OK
  )
  {
    Serial.println(
      "ESP-NOW INIT FAILED"
    );

    return;
  }


  esp_now_register_recv_cb(
    onESPNowReceive
  );


  configureS3Peer();


  Serial.print(
    "ESP-NOW channel: "
  );

  Serial.println(
    RADIO_CHANNEL
  );
}


// ============================================================
// WIFI CLIENT FILTER
// ============================================================

void onWiFiEvent(
  WiFiEvent_t event,
  WiFiEventInfo_t info
)
{
  if (
    event ==
    ARDUINO_EVENT_WIFI_AP_STACONNECTED
  )
  {
    uint8_t *mac =
      info.wifi_ap_staconnected.mac;


    Serial.print(
      "Wi-Fi client connected: "
    );

    Serial.println(
      macToString(mac)
    );


    /*
      Only the configured S3 is allowed.
    */

    if (
      hasS3MAC() &&
      memcmp(
        mac,
        s3MAC,
        6
      ) != 0
    )
    {
      Serial.println(
        "UNAUTHORIZED CLIENT - DISCONNECTING"
      );


      /*
        Deauthenticate station.

        This prevents phones or other devices
        from actually using the gateway.
      */

      esp_wifi_deauth_sta(
        info.wifi_ap_staconnected.aid
      );
    }
    else
    {
      Serial.println(
        "AUTHORIZED S3 CONNECTED"
      );
    }
  }
}


// ============================================================
// START HIDDEN WIFI
// ============================================================

void startWiFiGateway()
{
  WiFi.mode(
    WIFI_AP_STA
  );


  WiFi.onEvent(
    onWiFiEvent
  );


  WiFi.softAPConfig(
    AP_IP,
    AP_GATEWAY,
    AP_SUBNET
  );


  /*
    The fourth parameter is HIDDEN.

       false = visible
       true  = hidden
  */

  bool result =
    WiFi.softAP(
      wifiSSID.c_str(),
      wifiPassword.c_str(),
      RADIO_CHANNEL,
      true,
      1
    );


  Serial.println();


  Serial.print(
    "Hidden Wi-Fi AP: "
  );

  Serial.println(
    result ?
    "STARTED" :
    "FAILED"
  );


  Serial.print(
    "SSID: "
  );

  Serial.println(
    wifiSSID
  );


  Serial.println(
    "SSID broadcast: DISABLED"
  );


  Serial.print(
    "Gateway IP: "
  );

  Serial.println(
    WiFi.softAPIP()
  );


  Serial.print(
    "Allowed S3 MAC: "
  );

  Serial.println(
    macToString(s3MAC)
  );
}


// ============================================================
// CELLULAR
// ============================================================

bool startCellularData();


bool initCellularModem()
{
  if (!cellularEnabled)
  {
    Serial.println(
      "Cellular disabled."
    );

    return false;
  }


  Serial.println();
  Serial.println(
    "Starting SIM800L..."
  );


  ModemSerial.begin(
    MODEM_BAUD,
    SERIAL_8N1,
    MODEM_RX_PIN,
    MODEM_TX_PIN
  );


  delay(1000);


  if (
    !modem.testAT(5000)
  )
  {
    Serial.println(
      "SIM800L NOT RESPONDING"
    );

    cellularReady =
      false;

    return false;
  }


  Serial.println(
    "SIM800L DETECTED"
  );


  Serial.print(
    "Modem: "
  );

  Serial.println(
    modem.getModemInfo()
  );


  if (
    cellularPIN.length()
  )
  {
    modem.simUnlock(
      cellularPIN.c_str()
    );
  }


  Serial.println(
    "Waiting for cellular network..."
  );


  if (
    !modem.waitForNetwork(
      60000L
    )
  )
  {
    Serial.println(
      "CELLULAR NETWORK FAILED"
    );

    cellularRegistered =
      false;

    cellularReady =
      false;

    return false;
  }


  cellularRegistered =
    true;

  cellularReady =
    true;


  Serial.println(
    "CELLULAR NETWORK REGISTERED"
  );


  Serial.print(
    "Operator: "
  );

  Serial.println(
    modem.getOperator()
  );


  Serial.print(
    "Signal: "
  );

  Serial.println(
    modem.getSignalQuality()
  );


  /*
    Configure SMS notifications.

    CNMI tells SIM800L to notify the C3
    when an SMS arrives.
  */

  ModemSerial.println(
    "AT+CMGF=1"
  );

  delay(300);

  ModemSerial.println(
    "AT+CNMI=2,1,0,0,0"
  );

  delay(300);


  if (
    cellularAPN.length()
  )
  {
    return startCellularData();
  }


  Serial.println(
    "APN not configured."
  );


  return true;
}


// ============================================================
// GPRS
// ============================================================

bool startCellularData()
{
  if (!cellularReady)
  {
    Serial.println(
      "CELLULAR MODEM NOT READY"
    );

    return false;
  }


  if (!cellularAPN.length())
  {
    Serial.println(
      "APN EMPTY"
    );

    return false;
  }


  Serial.print(
    "Starting GPRS: "
  );

  Serial.println(
    cellularAPN
  );


  bool result =
    modem.gprsConnect(
      cellularAPN.c_str(),
      cellularUser.c_str(),
      cellularPassword.c_str()
    );


  if (!result)
  {
    cellularDataReady =
      false;

    Serial.println(
      "GPRS CONNECTION FAILED"
    );

    return false;
  }


  cellularDataReady =
    true;


  Serial.println(
    "GPRS INTERNET CONNECTED"
  );


  Serial.print(
    "Cellular IP: "
  );

  Serial.println(
    modem.localIP()
  );


  return true;
}


void stopCellularData()
{
  if (cellularDataReady)
    modem.gprsDisconnect();


  cellularDataReady =
    false;


  Serial.println(
    "GPRS DISCONNECTED"
  );
}


// ============================================================
// SMS READING
// ============================================================

void readSMSFromModem()
{
  if (!cellularReady)
  {
    sendNow(
      NOW_EVENT_ERROR,
      "MODEM OFFLINE"
    );

    return;
  }


  /*
    List unread SMS.

    SIM800L returns something like:

    +CMGL: 1,"REC UNREAD","+254....",... 
    Hello
  */

  ModemSerial.println(
    "AT+CMGF=1"
  );

  delay(300);


  while (
    ModemSerial.available()
  )
    ModemSerial.read();


  ModemSerial.println(
    "AT+CMGL=\"REC UNREAD\""
  );


  unsigned long start =
    millis();


  String response;


  while (
    millis() - start < 5000
  )
  {
    while (
      ModemSerial.available()
    )
    {
      char c =
        ModemSerial.read();

      response += c;
    }
  }


  response.trim();


  if (!response.length())
  {
    sendNow(
      NOW_EVENT_STATUS,
      "NO UNREAD SMS"
    );

    return;
  }


  /*
    Forward the modem response to the S3.

    The S3 can parse +CMGL records.
  */

  sendNow(
    NOW_EVENT_SMS,
    response
  );


  /*
    Mark messages as read by reading
    their indexes.

    For simplicity the S3 receives the
    complete modem response.
  */

  Serial.println(
    "SMS LIST FORWARDED TO S3"
  );
}


// ============================================================
// SMS UNSOLICITED PROCESSING
// ============================================================

void processModemLine(
  String line
)
{
  line.trim();


  if (!line.length())
    return;


  Serial.print(
    "[SIM800L] "
  );

  Serial.println(
    line
  );


  /*
    Incoming SMS notification:

      +CMTI: "SM",3
  */

  if (
    line.startsWith(
      "+CMTI:"
    )
  )
  {
    int comma =
      line.lastIndexOf(',');


    if (comma > 0)
    {
      String index =
        line.substring(
          comma + 1
        );

      index.trim();


      ModemSerial.print(
        "AT+CMGR="
      );

      ModemSerial.println(
        index
      );


      delay(1000);


      String sms;


      unsigned long start =
        millis();


      while (
        millis() - start < 3000
      )
      {
        while (
          ModemSerial.available()
        )
        {
          char c =
            ModemSerial.read();

          sms += c;
        }
      }


      sms.trim();


      sendNow(
        NOW_EVENT_SMS,
        sms
      );


      Serial.println(
        "NEW SMS FORWARDED TO S3"
      );
    }


    return;
  }


  /*
    Incoming call.

    SIM800L normally reports:

      RING
  */

  if (
    line == "RING"
  )
  {
    sendNow(
      NOW_EVENT_CALL,
      "RING"
    );

    Serial.println(
      "INCOMING CALL -> S3"
    );

    return;
  }


  /*
    Caller ID.

    With CLIP enabled:

      +CLIP: "+254...",...
  */

  if (
    line.startsWith(
      "+CLIP:"
    )
  )
  {
    sendNow(
      NOW_EVENT_CALL,
      line
    );

    return;
  }


  /*
    Call ended.
  */

  if (
    line == "NO CARRIER" ||
    line == "BUSY" ||
    line == "NO ANSWER"
  )
  {
    sendNow(
      NOW_EVENT_CALL_END,
      line
    );

    return;
  }
}


// ============================================================
// STATUS TO S3
// ============================================================

void sendGatewayStatusToS3()
{
  String status;

  status +=
    "C3=";

  status +=
    cellularReady ?
    "MODEM_OK;" :
    "MODEM_OFFLINE;";


  status +=
    "GPRS=";

  status +=
    cellularDataReady ?
    "ONLINE;" :
    "OFFLINE;";


  status +=
    "WIFI=HIDDEN;";


  status +=
    "CHANNEL=";

  status +=
    String(
      RADIO_CHANNEL
    );

  status +=
    ";";


  status +=
    "S3MAC=";

  status +=
    macToString(
      s3MAC
    );


  sendNow(
    NOW_EVENT_STATUS,
    status
  );
}


// ============================================================
// HTTP PROXY
// ============================================================

void handleProxy()
{
  if (!cellularDataReady)
  {
    server.send(
      503,
      "text/plain",
      "CELLULAR DATA OFFLINE"
    );

    return;
  }


  if (
    !server.hasArg("host") ||
    !server.hasArg("path")
  )
  {
    server.send(
      400,
      "text/plain",
      "USE /proxy?host=example.com&port=80&path=/"
    );

    return;
  }


  String host =
    server.arg("host");


  String path =
    server.arg("path");


  uint16_t port =
    80;


  if (
    server.hasArg("port")
  )
  {
    port =
      server.arg("port").toInt();
  }


  if (!path.length())
    path = "/";


  Serial.print(
    "PROXY -> "
  );

  Serial.print(
    host
  );

  Serial.print(
    ":"
  );

  Serial.println(
    port
  );


  if (
    !cellularClient.connect(
      host.c_str(),
      port
    )
  )
  {
    server.send(
      502,
      "text/plain",
      "CELLULAR TCP CONNECTION FAILED"
    );

    return;
  }


  cellularClient.print(
    "GET "
  );

  cellularClient.print(
    path
  );

  cellularClient.println(
    " HTTP/1.1"
  );


  cellularClient.print(
    "Host: "
  );

  cellularClient.println(
    host
  );


  cellularClient.println(
    "Connection: close"
  );


  cellularClient.println();


  unsigned long timeout =
    millis();


  String response;


  while (
    cellularClient.connected() &&
    millis() - timeout < 15000
  )
  {
    while (
      cellularClient.available()
    )
    {
      char c =
        cellularClient.read();

      response += c;

      timeout =
        millis();


      if (
        response.length() >= 12000
      )
      {
        cellularClient.stop();

        server.send(
          200,
          "text/plain",
          response
        );

        return;
      }
    }
  }


  cellularClient.stop();


  server.send(
    200,
    "text/plain",
    response
  );
}


// ============================================================
// HTTP STATUS
// ============================================================

void handleGatewayStatus()
{
  String response;

  response +=
    "MINICLAW C3 GATEWAY\n";

  response +=
    "WIFI: HIDDEN\n";

  response +=
    "GATEWAY: 192.168.4.1\n";

  response +=
    "CHANNEL: ";

  response +=
    String(
      RADIO_CHANNEL
    );

  response +=
    "\n";

  response +=
    "S3 MAC: ";

  response +=
    macToString(
      s3MAC
    );

  response +=
    "\n";

  response +=
    "MODEM: ";

  response +=
    cellularReady ?
    "READY\n" :
    "OFFLINE\n";

  response +=
    "GPRS: ";

  response +=
    cellularDataReady ?
    "CONNECTED\n" :
    "OFFLINE\n";


  if (cellularDataReady)
  {
    response +=
      "CELL IP: ";

    response +=
      modem.localIP().toString();

    response +=
      "\n";
  }


  response +=
    "WIFI CLIENTS: ";

  response +=
    String(
      WiFi.softAPgetStationNum()
    );

  response +=
    "\n";


  server.send(
    200,
    "text/plain",
    response
  );
}


// ============================================================
// HTTP SERVER
// ============================================================

void startGatewayServer()
{
  server.on(
    "/",
    HTTP_GET,
    []()
    {
      server.send(
        200,
        "text/plain",
        "MINICLAW C3 CELLULAR GATEWAY"
      );
    }
  );


  server.on(
    "/status",
    HTTP_GET,
    handleGatewayStatus
  );


  server.on(
    "/proxy",
    HTTP_GET,
    handleProxy
  );


  server.begin();


  Serial.println(
    "HTTP GATEWAY STARTED"
  );
}


// ============================================================
// OTA
// ============================================================

void setupOTA()
{
  if (!otaEnabled)
    return;


  ArduinoOTA.setHostname(
    otaHostname.c_str()
  );


  if (otaPassword.length())
  {
    ArduinoOTA.setPassword(
      otaPassword.c_str()
    );
  }


  ArduinoOTA.onStart(
    []()
    {
      Serial.println(
        "OTA START"
      );
    }
  );


  ArduinoOTA.onEnd(
    []()
    {
      Serial.println(
        "OTA COMPLETE"
      );
    }
  );


  ArduinoOTA.onError(
    [](ota_error_t error)
    {
      Serial.print(
        "OTA ERROR: "
      );

      Serial.println(
        error
      );
    }
  );


  ArduinoOTA.begin();


  otaReady =
    true;
}


// ============================================================
// STATUS
// ============================================================

void printStatus()
{
  Serial.println();
  Serial.println(
    "=========================================="
  );

  Serial.println(
    "       MINICLAW ESP32-C3 GATEWAY"
  );

  Serial.println(
    "=========================================="
  );


  Serial.print(
    "C3 MAC: "
  );

  Serial.println(
    WiFi.macAddress()
  );


  Serial.print(
    "S3 MAC: "
  );

  Serial.println(
    macToString(
      s3MAC
    )
  );


  Serial.print(
    "WiFi SSID: "
  );

  Serial.println(
    wifiSSID
  );


  Serial.println(
    "WiFi visibility: HIDDEN"
  );


  Serial.print(
    "Gateway: "
  );

  Serial.println(
    WiFi.softAPIP()
  );


  Serial.print(
    "Channel: "
  );

  Serial.println(
    RADIO_CHANNEL
  );


  Serial.print(
    "WiFi clients: "
  );

  Serial.println(
    WiFi.softAPgetStationNum()
  );


  Serial.print(
    "SIM800L: "
  );

  Serial.println(
    cellularReady ?
    "READY" :
    "OFFLINE"
  );


  Serial.print(
    "Network: "
  );

  Serial.println(
    cellularRegistered ?
    "REGISTERED" :
    "NOT REGISTERED"
  );


  Serial.print(
    "GPRS: "
  );

  Serial.println(
    cellularDataReady ?
    "CONNECTED" :
    "OFFLINE"
  );


  if (cellularDataReady)
  {
    Serial.print(
      "Cellular IP: "
    );

    Serial.println(
      modem.localIP()
    );
  }


  Serial.print(
    "APN: "
  );

  Serial.println(
    cellularAPN
  );


  Serial.print(
    "Phone: "
  );

  Serial.println(
    savedPhoneNumber
  );


  Serial.print(
    "OTA: "
  );

  Serial.println(
    otaEnabled ?
    "ENABLED" :
    "DISABLED"
  );


  Serial.println(
    "=========================================="
  );
}


// ============================================================
// HELP
// ============================================================

void printHelp()
{
  Serial.println();
  Serial.println(
    "========== SERIAL COMMANDS =========="
  );

  Serial.println(
    "STATUS"
  );

  Serial.println(
    "WIFI STATUS"
  );

  Serial.println(
    "SET SSID <name>"
  );

  Serial.println(
    "SET WIFI PASSWORD <password>"
  );

  Serial.println(
    "SET S3MAC XX:XX:XX:XX:XX:XX"
  );

  Serial.println(
    "SET APN <apn>"
  );

  Serial.println(
    "SET CELL USER <user>"
  );

  Serial.println(
    "SET CELL PASSWORD <password>"
  );

  Serial.println(
    "SET CELL PIN <pin>"
  );

  Serial.println(
    "SET PHONE <number>"
  );

  Serial.println(
    "CELL INIT"
  );

  Serial.println(
    "CELL CONNECT"
  );

  Serial.println(
    "CELL DISCONNECT"
  );

  Serial.println(
    "CELL STATUS"
  );

  Serial.println(
    "CALL <number>"
  );

  Serial.println(
    "CALL"
  );

  Serial.println(
    "HANGUP"
  );

  Serial.println(
    "SMS <number> <message>"
  );

  Serial.println(
    "SMS READ"
  );

  Serial.println(
    "SET OTA HOST <hostname>"
  );

  Serial.println(
    "SET OTA PASSWORD <password>"
  );

  Serial.println(
    "OTA ON"
  );

  Serial.println(
    "OTA OFF"
  );

  Serial.println(
    "SAVE"
  );

  Serial.println(
    "REBOOT"
  );

  Serial.println(
    "FACTORY RESET"
  );

  Serial.println(
    "===================================="
  );

  Serial.println();
}


// ============================================================
// SERIAL COMMAND PROCESSING
// ============================================================

void processCommand(
  String command
)
{
  command.trim();


  if (!command.length())
    return;


  String upper =
    command;

  upper.toUpperCase();


  // ----------------------------------------------------------
  // HELP
  // ----------------------------------------------------------

  if (
    upper ==
    "HELP"
  )
  {
    printHelp();
    return;
  }


  // ----------------------------------------------------------
  // STATUS
  // ----------------------------------------------------------

  if (
    upper ==
    "STATUS"
  )
  {
    printStatus();
    return;
  }


  // ----------------------------------------------------------
  // WIFI STATUS
  // ----------------------------------------------------------

  if (
    upper ==
    "WIFI STATUS"
  )
  {
    Serial.print(
      "SSID: "
    );

    Serial.println(
      wifiSSID
    );


    Serial.println(
      "SSID broadcast: HIDDEN"
    );


    Serial.print(
      "Gateway: "
    );

    Serial.println(
      WiFi.softAPIP()
    );


    Serial.print(
      "Clients: "
    );

    Serial.println(
      WiFi.softAPgetStationNum()
    );


    return;
  }


  // ----------------------------------------------------------
  // SET SSID
  // ----------------------------------------------------------

  if (
    upper.startsWith(
      "SET SSID "
    )
  )
  {
    wifiSSID =
      command.substring(
        9
      );

    wifiSSID.trim();


    saveConfiguration();


    Serial.println(
      "SSID SAVED - REBOOT REQUIRED"
    );

    return;
  }


  // ----------------------------------------------------------
  // WIFI PASSWORD
  // ----------------------------------------------------------

  if (
    upper.startsWith(
      "SET WIFI PASSWORD "
    )
  )
  {
    wifiPassword =
      command.substring(
        19
      );

    wifiPassword.trim();


    if (
      wifiPassword.length() < 8
    )
    {
      Serial.println(
        "ERROR: WIFI PASSWORD MUST BE 8+ CHARACTERS"
      );

      return;
    }


    saveConfiguration();


    Serial.println(
      "WIFI PASSWORD SAVED - REBOOT REQUIRED"
    );

    return;
  }


  // ----------------------------------------------------------
  // S3 MAC
  // ----------------------------------------------------------

  if (
    upper.startsWith(
      "SET S3MAC "
    )
  )
  {
    uint8_t newMAC[6];


    if (
      parseMAC(
        command.substring(
          10
        ),
        newMAC
      )
    )
    {
      memcpy(
        s3MAC,
        newMAC,
        6
      );


      saveConfiguration();


      configureS3Peer();


      Serial.println(
        "S3 MAC SAVED"
      );
    }
    else
    {
      Serial.println(
        "INVALID MAC"
      );
    }


    return;
  }


  // ----------------------------------------------------------
  // APN
  // ----------------------------------------------------------

  if (
    upper.startsWith(
      "SET APN "
    )
  )
  {
    cellularAPN =
      command.substring(
        8
      );

    cellularAPN.trim();


    saveConfiguration();


    Serial.println(
      "APN SAVED"
    );

    return;
  }


  // ----------------------------------------------------------
  // CELL USER
  // ----------------------------------------------------------

  if (
    upper.startsWith(
      "SET CELL USER "
    )
  )
  {
    cellularUser =
      command.substring(
        14
      );

    cellularUser.trim();


    saveConfiguration();


    Serial.println(
      "CELL USER SAVED"
    );

    return;
  }


  // ----------------------------------------------------------
  // CELL PASSWORD
  // ----------------------------------------------------------

  if (
    upper.startsWith(
      "SET CELL PASSWORD "
    )
  )
  {
    cellularPassword =
      command.substring(
        18
      );

    cellularPassword.trim();


    saveConfiguration();


    Serial.println(
      "CELL PASSWORD SAVED"
    );

    return;
  }


  // ----------------------------------------------------------
  // SIM PIN
  // ----------------------------------------------------------

  if (
    upper.startsWith(
      "SET CELL PIN "
    )
  )
  {
    cellularPIN =
      command.substring(
        13
      );

    cellularPIN.trim();


    saveConfiguration();


    Serial.println(
      "CELL PIN SAVED"
    );

    return;
  }


  // ----------------------------------------------------------
  // PHONE
  // ----------------------------------------------------------

  if (
    upper.startsWith(
      "SET PHONE "
    )
  )
  {
    savedPhoneNumber =
      command.substring(
        10
      );

    savedPhoneNumber.trim();


    saveConfiguration();


    Serial.println(
      "PHONE NUMBER SAVED"
    );

    return;
  }


  // ----------------------------------------------------------
  // CELL INIT
  // ----------------------------------------------------------

  if (
    upper ==
    "CELL INIT"
  )
  {
    initCellularModem();
    return;
  }


  // ----------------------------------------------------------
  // CELL CONNECT
  // ----------------------------------------------------------

  if (
    upper ==
    "CELL CONNECT"
  )
  {
    if (!cellularReady)
      initCellularModem();
    else
      startCellularData();

    return;
  }


  // ----------------------------------------------------------
  // CELL DISCONNECT
  // ----------------------------------------------------------

  if (
    upper ==
    "CELL DISCONNECT"
  )
  {
    stopCellularData();
    return;
  }


  // ----------------------------------------------------------
  // CELL STATUS
  // ----------------------------------------------------------

  if (
    upper ==
    "CELL STATUS"
  )
  {
    Serial.print(
      "MODEM: "
    );

    Serial.println(
      cellularReady ?
      "READY" :
      "OFFLINE"
    );


    Serial.print(
      "NETWORK: "
    );

    Serial.println(
      cellularRegistered ?
      "REGISTERED" :
      "NOT REGISTERED"
    );


    Serial.print(
      "GPRS: "
    );

    Serial.println(
      cellularDataReady ?
      "CONNECTED" :
      "OFFLINE"
    );


    return;
  }


  // ----------------------------------------------------------
  // CALL
  // ----------------------------------------------------------

  if (
    upper ==
    "CALL"
  )
  {
    if (
      savedPhoneNumber.length()
    )
    {
      ModemSerial.print(
        "ATD"
      );

      ModemSerial.print(
        savedPhoneNumber
      );

      ModemSerial.println(
        ";"
      );


      Serial.print(
        "CALLING "
      );

      Serial.println(
        savedPhoneNumber
      );
    }
    else
    {
      Serial.println(
        "NO SAVED PHONE NUMBER"
      );
    }


    return;
  }


  if (
    upper.startsWith(
      "CALL "
    )
  )
  {
    String number =
      command.substring(
        5
      );

    number.trim();


    ModemSerial.print(
      "ATD"
    );

    ModemSerial.print(
      number
    );

    ModemSerial.println(
      ";"
    );


    Serial.print(
      "CALLING "
    );

    Serial.println(
      number
    );


    return;
  }


  // ----------------------------------------------------------
  // HANGUP
  // ----------------------------------------------------------

  if (
    upper ==
    "HANGUP"
  )
  {
    ModemSerial.println(
      "ATH"
    );


    Serial.println(
      "HANGUP SENT"
    );


    return;
  }


  // ----------------------------------------------------------
  // SMS
  // ----------------------------------------------------------

  if (
    upper ==
    "SMS READ"
  )
  {
    readSMSFromModem();
    return;
  }


  if (
    upper.startsWith(
      "SMS "
    )
  )
  {
    String value =
      command.substring(
        4
      );


    int separator =
      value.indexOf(
        ' '
      );


    if (
      separator <= 0
    )
    {
      Serial.println(
        "USE: SMS <NUMBER> <MESSAGE>"
      );

      return;
    }


    String number =
      value.substring(
        0,
        separator
      );


    String message =
      value.substring(
        separator + 1
      );


    bool result =
      modem.sendSMS(
        number.c_str(),
        message.c_str()
      );


    Serial.println(
      result ?
      "SMS SENT" :
      "SMS FAILED"
    );


    return;
  }


  // ----------------------------------------------------------
  // OTA
  // ----------------------------------------------------------

  if (
    upper.startsWith(
      "SET OTA HOST "
    )
  )
  {
    otaHostname =
      command.substring(
        14
      );

    otaHostname.trim();


    saveConfiguration();


    Serial.println(
      "OTA HOST SAVED - REBOOT REQUIRED"
    );


    return;
  }


  if (
    upper.startsWith(
      "SET OTA PASSWORD "
    )
  )
  {
    otaPassword =
      command.substring(
        18
      );

    otaPassword.trim();


    saveConfiguration();


    Serial.println(
      "OTA PASSWORD SAVED - REBOOT REQUIRED"
    );


    return;
  }


  if (
    upper ==
    "OTA ON"
  )
  {
    otaEnabled =
      true;


    saveConfiguration();


    Serial.println(
      "OTA ENABLED - REBOOT REQUIRED"
    );


    return;
  }


  if (
    upper ==
    "OTA OFF"
  )
  {
    otaEnabled =
      false;


    saveConfiguration();


    Serial.println(
      "OTA DISABLED - REBOOT REQUIRED"
    );


    return;
  }


  // ----------------------------------------------------------
  // SAVE
  // ----------------------------------------------------------

  if (
    upper ==
    "SAVE"
  )
  {
    saveConfiguration();


    Serial.println(
      "CONFIGURATION SAVED"
    );


    return;
  }


  // ----------------------------------------------------------
  // REBOOT
  // ----------------------------------------------------------

  if (
    upper ==
    "REBOOT"
  )
  {
    Serial.println(
      "REBOOTING..."
    );


    delay(500);


    ESP.restart();


    return;
  }


  // ----------------------------------------------------------
  // FACTORY RESET
  // ----------------------------------------------------------

  if (
    upper ==
    "FACTORY RESET"
  )
  {
    preferences.clear();


    Serial.println(
      "NVS CLEARED"
    );


    Serial.println(
      "REBOOT TO LOAD DEFAULTS"
    );


    return;
  }


  Serial.println(
    "UNKNOWN COMMAND - TYPE HELP"
  );
}


// ============================================================
// SERIAL MONITOR
// ============================================================

void handleSerial()
{
  while (
    Serial.available()
  )
  {
    char c =
      Serial.read();


    if (
      c == '\n' ||
      c == '\r'
    )
    {
      if (
        serialCommand.length()
      )
      {
        processCommand(
          serialCommand
        );

        serialCommand =
          "";
      }
    }
    else
    {
      serialCommand += c;


      if (
        serialCommand.length() > 250
      )
      {
        serialCommand =
          "";
      }
    }
  }
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(
    115200
  );


  delay(1000);


  Serial.println();
  Serial.println(
    "=========================================="
  );

  Serial.println(
    " MINICLAW ESP32-C3 CELLULAR GATEWAY"
  );

  Serial.println(
    " SIM800L + HIDDEN WIFI + ESP-NOW"
  );

  Serial.println(
    "=========================================="
  );


  // ----------------------------------------------------------
  // CONFIGURATION
  // ----------------------------------------------------------

  loadConfiguration();


  // ----------------------------------------------------------
  // AUDIO
  // ----------------------------------------------------------

  initAudioOutput();


  audioQueue =
    xQueueCreate(
      AUDIO_QUEUE_SIZE,
      sizeof(AudioBlock)
    );


  if (!audioQueue)
  {
    Serial.println(
      "AUDIO QUEUE FAILED"
    );

    while (true)
      delay(1000);
  }


  xTaskCreate(
    audioTask,
    "AudioTask",
    4096,
    NULL,
    2,
    NULL
  );


  // ----------------------------------------------------------
  // WIFI
  // ----------------------------------------------------------

  startWiFiGateway();


  // ----------------------------------------------------------
  // ESP-NOW
  // ----------------------------------------------------------

  initESPNow();


  // ----------------------------------------------------------
  // SIM800L
  // ----------------------------------------------------------

  initCellularModem();


  // ----------------------------------------------------------
  // HTTP
  // ----------------------------------------------------------

  startGatewayServer();


  // ----------------------------------------------------------
  // OTA
  // ----------------------------------------------------------

  setupOTA();


  // ----------------------------------------------------------
  // STATUS
  // ----------------------------------------------------------

  printStatus();


  Serial.println();
  Serial.println(
    "TYPE HELP FOR SERIAL COMMANDS"
  );

  Serial.println();


  Serial.println(
    "S3 CONNECTION:"
  );

  Serial.println(
    "SSID is HIDDEN."
  );

  Serial.println(
    "S3 must connect using the configured SSID."
  );

  Serial.println(
    "Gateway IP: 192.168.4.1"
  );
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
  handleSerial();


  server.handleClient();


  if (otaReady)
    ArduinoOTA.handle();


  /*
    Read SIM800L unsolicited messages.
  */

  while (
    ModemSerial.available()
  )
  {
    String line =
      ModemSerial.readStringUntil(
        '\n'
      );


    processModemLine(
      line
    );
  }


  /*
    Automatic GPRS reconnection.
  */

  static unsigned long lastCellCheck = 0;


  if (
    millis() - lastCellCheck >
    30000
  )
  {
    lastCellCheck =
      millis();


    if (
      cellularReady &&
      cellularAPN.length()
    )
    {
      if (
        !modem.isGprsConnected()
      )
      {
        cellularDataReady =
          false;


        Serial.println(
          "GPRS LOST - RECONNECTING..."
        );


        startCellularData();
      }
      else
      {
        cellularDataReady =
          true;
      }
    }
  }


  delay(2);
}
