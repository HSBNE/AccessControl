

/*
    ESP8266 Access Control Firmware for HSBNE's Sonoff TH10 based control hardware.
    Written by nog3 August 2018
    Contribs: pelrun (Sane rfid reading), jabeone (fix reset on some card reads bug)
*/

// Include all the libraries we need for this.
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <FS.h>
#include <WS2812FX.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <WebSocketsServer.h>
#include <WebSockets.h>

// Editable config values.
const char* ssid     = ""; // Wifi SSID
const char* password = ""; // Wifi Password
const char* host = ""; // Host URL
const char* secret = ""; // Secret to talk to the Host on.
const char* deviceName = "DOOR-TEST"; // Device name. DOOR-DoorName or INT-InterlockName
const char* devicePassword = ""; // Password for OTA on device.
const char* deviceType = "door"; // either interlock or door
int checkinRate = 60; // How many seconds between standard server checkins.
int sessionCheckinRate = 60; // How many seconds between interlock session checkins.
int contact = 0; // Set default switch state, 1 for doors that are permanantly powered/fail-open.
int rfidSquelchTime = 5000; // How long after checking a card with the server should we ignore reads.

// Configure our output pins.
const int switchPin = 12; // This is the pin the relay is on in the TH10 board.
const int ledPin = 13; // This is an onboard LED, just to show we're alive.
const int statePin = 14; // This is the pin exposed on the TRRS plug on the sonoff, used for LED on interlocks.

// Initialise our base state vars.
int triggerFlag = 0; //State trigger for heartbeats and other useful blocking things.
int lastReadSuccess = 5000; // Set last read success base state. Setting to 5 seconds to make sure on boot it's going to ignore initial reads.
uint32_t lastId = 0; // Set lastID to nothing.
String sessionID = ""; // Set sessionID as null.
char currentColor = 'b'; // Default interlock status led color is blue, let's start there.
String curCacheHash = ""; // Current cache hash, pulled from cache file.
int useLocal = 0; // Whether or not to use local cache, set by heartbeat failures or manually.
int tagsArray[200]; // Where the int array of tags is loaded to from cache on heartbeat fail.

//Configure our objects.
HTTPClient client;
WS2812FX ws2812fx = WS2812FX(1, statePin, NEO_RGB + NEO_KHZ800);
ESP8266WebServer http(80);
WebSocketsServer webSocket = WebSocketsServer(81);
Ticker heartbeat;
Ticker heartbeatSession;

// ISR and RAM cached functions go here. Stuff we want to fire fast and frequently.
void ICACHE_RAM_ATTR idleHeartBeatFlag() {
  triggerFlag = 1;
}

void ICACHE_RAM_ATTR activeHeartBeatFlag() {
  triggerFlag = 2;
}

void ICACHE_RAM_ATTR log(String entry) {
  Serial.println(entry);
  webSocket.broadcastTXT(String(millis()) + " " + entry);
  delay(10);
}

void ICACHE_RAM_ATTR checkIn() {
  // Serial.println("[CHECKIN] Standard checkin begin");
  // Delay to clear wifi buffer.
  delay(10);
  String url = String(host) + "/api/" + deviceType + "/checkin/?secret=" + String(secret);
  log("[CHECKIN] Get:" + String(url));
  client.begin(url);

  // Start http request.
  int httpCode = client.GET();
  // httpCode will be negative on error
  if (httpCode > 0) {
    // Serial.println("[CHECKIN] Code: " + String(httpCode));
    // Checkin succeeded.
    if (httpCode == HTTP_CODE_OK) {
      String payload = client.getString();
      log("[CHECKIN] Server response: " + payload);
      DynamicJsonBuffer jsonBuffer;
      JsonObject&root = jsonBuffer.parseObject(payload.substring(payload.indexOf('{'), payload.length()));
      String serverCacheHash = root["hashOfTags"].as<String>();
      if (serverCacheHash != curCacheHash) {
        log("[CACHE] Cache hashes don't match, flagging update.");
        triggerFlag = 3;
      }
      else {
        triggerFlag = 0;
      }
    }
  } else {
    log("[CHECKIN] Error: " + client.errorToString(httpCode));
    statusLight('y');
    triggerFlag = 0;
  }
  client.end();
  // log("[CHECKIN] Checkin done.");
  delay(10);
}


void ICACHE_RAM_ATTR checkInSession(String sessionGUID, uint32_t cardNo) {
  log("[SESSION] Session Heartbeat Begin.");
  // Delay to clear wifi buffer.
  delay(10);
  String url;
  if (cardNo > 0) {
    url = String(host) + "/api/" + deviceType + "/session/" + sessionGUID + "/end/" + cardNo + "/?secret=" + String(secret);
  } else {
    url = String(host) + "/api/" + deviceType + "/session/" + sessionGUID + "/heartbeat/?secret=" + String(secret);
  }
  log("[SESSION] Get:" + String(url));
  client.begin(url);

  // Start http request.
  int httpCode = client.GET();
  // httpCode will be negative on error
  if (httpCode > 0) {
    // log("[SESSION] Code: " + String(httpCode));

    // Checkin succeeded.
    if (httpCode == HTTP_CODE_OK) {
      String payload = client.getString();
      log("[SESSION] Heartbeat response: " + payload);
    }
  } else {
    log("[SESSION] Heartbeat Error: " + client.errorToString(httpCode));
    statusLight('y');
  }
  client.end();
  log("[SESSION] Session Heartbeat Done.");
  triggerFlag = 0;
  delay(10);
}


void readTagInterlock() {
  char tagBytes[6];

  //  while (!Serial.available()) { delay(10); }

  if (Serial.readBytes(tagBytes, 5) == 5)
  {
    uint8_t checksum = 0;
    uint32_t cardId = 0;

    tagBytes[6] = 0;

    //    Serial.println("Raw Tag:");
    for (int i = 0; i < 4; i++)
    {
      checksum ^= tagBytes[i];
      cardId = cardId << 8 | tagBytes[i];
      Serial.println(tagBytes[i], HEX);
    }

    if (checksum == tagBytes[4])
    {
      log("[AUTH] Tag Number:" + String(cardId));
      flushSerial();
      if (cardId != lastId) {
        if (!contact) {
          log("[AUTH] Tag is new, checking with server.");
          statusLight('w');
          Serial.println(millis());
          authCard(cardId);
        } else {
          log("[AUTH] This is someone else disabling the interlock.");
          int state = contact;
          // Turn off contact, detach timer and heartbeat one last time.
          toggleContact();
          heartbeatSession.detach();
          checkInSession(sessionID, cardId);

          // Update the user that swipe timeout has begun.
          statusLight('w');
          lastId = 0;
          // Clear temp globals.
          sessionID = "";

        }
      } else {
        log("[AUTH] This is the last user disabling the interlock.");
        // Turn off contact, detach timer and heartbeat one last time.
        toggleContact();
        heartbeatSession.detach();
        checkInSession(sessionID, cardId);
        // Update the user that swipe timeout has begun.
        statusLight('w');
        lastId = 0;
        // Clear temp globals.
        sessionID = "";
      }

      lastReadSuccess = millis();
    } else {
      flushSerial();
      //log("incomplete or corrupted RFID read, sorry. ");
    }
  }
}

void readTagDoor() {
  char tagBytes[6];

  //  while (!Serial.available()) { delay(10); }

  if (Serial.readBytes(tagBytes, 5) == 5)
  {
    uint8_t checksum = 0;
    uint32_t cardId = 0;

    tagBytes[6] = 0;

    //    log("Raw Tag:");
    for (int i = 0; i < 4; i++)
    {
      checksum ^= tagBytes[i];
      cardId = cardId << 8 | tagBytes[i];
      //     Serial.println(tagBytes[i], HEX);
    }

    if (checksum == tagBytes[4])
    {
      log("[AUTH] Tag Number:" + String(cardId));
      flushSerial();
      authCard(cardId);
      lastReadSuccess = millis();
    } else {
      flushSerial();
      log("[AUTH] incomplete or corrupted RFID read, sorry. ");
    }
  }
}

void startWifi () {
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.println();
  Serial.print("[WIFI] Connecting to ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.hostname(deviceName);

  // If we're setup for static IP assignment, apply it.
#ifdef USE_STATIC
  WiFi.config(ip, gateway, subnet);
#endif

  // Interlock Only: While we're not connected breathe the status light and output to serial that we're still connecting.

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(50);
    ws2812fx.service();
  }
  Serial.println(".");
  Serial.println("[WIFI] WiFi connected");
  Serial.print("[WIFI] IP address: ");
  Serial.println(WiFi.localIP());
  statusLight('w');
  delay(10);
}

void toggleContact() {
  switch (contact) {
    case 0:
      {
        contact = 1;
        digitalWrite(switchPin, HIGH);
        statusLight('e');
        break;
      }
    case 1:
      {
        contact = 0;
        digitalWrite(switchPin, LOW);
        statusLight('b');
        break;
      }
  }
}

void pulseContact() {
  switch (contact) {
    case 0:
      {
        digitalWrite(switchPin, HIGH);
        delay(5000);
        digitalWrite(switchPin, LOW);
        break;
      }
    case 1:
      {
        digitalWrite(switchPin, LOW);
        delay(5000);
        digitalWrite(switchPin, HIGH);
        break;
      }
  }
}

void statusLight(char color) {
  if (deviceType == "door") {
    return;
  }
  if (currentColor == color) {
    return;
  } else {
    switch (color) {
      case 'r':
        {
          ws2812fx.setSegment(0,  0,  0, FX_MODE_STATIC, 0xFF0000, 1000, false);
          break;
        }
      case 'g':
        {
          ws2812fx.setSegment(0,  0,  0, FX_MODE_STATIC, 0x00FF00, 1000, false);
          break;
        }
      case 'b':
        {
          ws2812fx.setSegment(0,  0,  0, FX_MODE_STATIC, 0x0000FF, 1000, false);
          break;
        }
      case 'y':
        {
          ws2812fx.setSegment(0,  0,  0, FX_MODE_STROBE, 0xFF6400, 250, false);
          break;
        }
      case 'p':
        {
          ws2812fx.setSegment(0,  0,  0, FX_MODE_BREATH, 0x800080, 250, false);
          break;
        }
      case 'w':
        {
          ws2812fx.setSegment(0,  0,  0, FX_MODE_BREATH, 0x0000FF, 250, false);
          break;
        }
      case 'e':
        {
          ws2812fx.setSegment(0,  0,  0, FX_MODE_BREATH, 0x00FF00, 250, false);
          break;
        }
    }
    currentColor = color;
    ws2812fx.service();
  }
}

void flushSerial () {
  int flushCount = 0;
  while (  Serial.available() ) {
    char t = Serial.read();  // flush any remaining bytes.
    flushCount++;
    // Serial.println("flushed a byte");
  }
  if (flushCount > 0) {
    log("[DEBUG] Flushed " + String(flushCount) + " bytes.");
    flushCount = 0;
  }

}

void httpRoot() {
  String message = "<html><head><script>var connection = new WebSocket('ws://'+location.hostname+':81/', ['arduino']);connection.onopen = function () {  connection.send('Connect ' + new Date()); }; connection.onerror = function (error) {    console.log('WebSocket Error ', error);};connection.onmessage = function (e) {  console.log('Server: ', e.data); var logObj = document.getElementById('logs'); logObj.insertAdjacentHTML('afterend', e.data + '</br>');;};</script><title>" + String(deviceName) + "</title></head>";
  message += "<h1>" + String(deviceName) + "</h1>";
  message += "Last swiped tag was " + String(lastId)  + "<br />";
  if (deviceType == "interlock" & contact == 1) {
    message += "Interlock is Active, Session ID is " + String(sessionID) + "<br />";
  }
  message += "Current cache hash is " + curCacheHash + " <br /> ";
  message += "<h2>Logs: </h2> <div id ='logs'> </div> ";
  http.send(200, "text/html", message);
}

void authCard(long tagid) {

  log("[AUTH] Server auth check begin");
  String url = String(host) + "/api/" + deviceType + "/check/" + String(tagid) + "/?secret=" + String(secret);
  log("[AUTH] Get:" + String(url));
  client.begin(url);

  // Start http request.
  int httpCode = client.GET();
  // httpCode will be negative on error
  if (httpCode > 0) {
    log("[AUTH] Code: " + String(httpCode));

    // Checkin succeeded.
    if (httpCode == HTTP_CODE_OK) {
      String payload = client.getString();
      log("[AUTH] Server response: " + payload);
      DynamicJsonBuffer jsonBuffer;
      JsonObject&root = jsonBuffer.parseObject(payload.substring(payload.indexOf('{'), payload.length()));
      if ( root[String("access")] == true ) {
        log("[AUTH] Access granted.");
        if (deviceType == "interlock") {
          sessionID = root["session_id"].as<String>();
          toggleContact();
          lastId = tagid;
          heartbeatSession.attach(sessionCheckinRate, activeHeartBeatFlag);
        } else {
          lastId = tagid;
          pulseContact();
        }

      } else {
        log("[AUTH] Access not granted.");
        statusLight('r');
        delay(1000);
      }

    }
  } else {
    log("[AUTH] Error: " + client.errorToString(httpCode));
    statusLight('y');
  }
  client.end();
  log("[AUTH] Card Auth done.");
  delay(10);
}




void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {

  switch (type) {
    case WStype_DISCONNECTED:
      log(num + " Disconnected!");
      break;
    case WStype_CONNECTED: {
        IPAddress ip = webSocket.remoteIP(num);
        //   Serial.println(String(num) + " Connected from " + String(ip));
        log("[DEBUG] Client connected.");
      }
      break;
  }

}


void setup() {
  Serial.begin(9600);
  Serial.println("[SETUP] Serial Started");
  ws2812fx.init();
  ws2812fx.start();
  statusLight('p');
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  Serial.setTimeout(500);
  startWifi();
  // Set switch pin to output.
  pinMode(switchPin, OUTPUT);
  if (!contact) {
    digitalWrite(switchPin, LOW); // Set base switch state.
  } else {
    digitalWrite(switchPin, HIGH); // Set base switch state.
  }
  // Configure OTA settings.
  ArduinoOTA.setHostname(deviceName);
  ArduinoOTA.setPassword(devicePassword);


  ArduinoOTA.onStart([]() {
    log("[OTA] Start");
    statusLight('p');
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] End");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    ws2812fx.service();
    yield();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  //Setup Websocket debug logger
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  //Setup HTTP debug server.
  http.on("/", httpRoot);

  http.on("/reboot", []() {
    http.sendHeader("Location", "/");
    // Redirect back to root in case chrome refreshes.
    http.send(200, "text/plain", "[DEBUG] Rebooting.");
    log("[DEBUG] Rebooting");
    ESP.reset();
  });
  http.on("/bump", []() {
    if (deviceType == "door") {
      http.send(200, "text/plain", "Bumping door.");
      log("[DEBUG] Bumped lock.");
      pulseContact();
    }
  });
  http.on("/checkin", []() {
    idleHeartBeatFlag();
  });
  http.on("/getcache", []() {
    getCache();
  });
  http.on("/printcache", []() {
    printCache();
  });
  http.on("/loadtags", []() {
    loadTags();
  });
  http.on("/printtags", []() {
    printTags();
  });
  http.on("/end", []() {
    if (deviceType == "interlock") {
      contact = 0;
      digitalWrite(switchPin, LOW);
      statusLight('b');
    }
  });
  http.begin();
  log("[SETUP] HTTP server started");
  heartbeat.attach(checkinRate, idleHeartBeatFlag);
  delay(10);

  // Handle caching functions.
  if (!SPIFFS.begin()) {
    log("[STORAGE]Failed to mount file system");
    return;
  } else {
    File cacheFile = SPIFFS.open("/authorised.json", "r");
    if (!cacheFile) {
      log("[CACHE] Error opening authorised json file.");
      return;
    } else {
      String cacheBuf = cacheFile.readStringUntil('\n');
      cacheFile.close();
      DynamicJsonBuffer jsonBuffer;
      JsonObject&root = jsonBuffer.parseObject(cacheBuf.substring(cacheBuf.indexOf('{'), cacheBuf.length()));
      curCacheHash = root["authorised_tags_hash"].as<String>();
    }
  }
}

void loop()
{
  delay(10);
  // Check to see if any of our state flags have tripped.
  switch (triggerFlag) {
    case 1:
      {
        delay(10);
        checkIn();
        delay(10);
        log("[DEBUG] Free Heap Size: " + String(ESP.getFreeHeap()));
        break;
      }
    case 2:
      {
        delay(10);
        checkInSession(sessionID, 0);
        delay(10);
        log("[DEBUG] Free Heap Size: " + String(ESP.getFreeHeap()));
        break;
      }
    case 3:
      {
        delay(10);
        getCache();
        delay(10);
        triggerFlag = 0;
        break;
      }
  }

  // Yield for 10ms so we can then handle any wifi data.
  delay(10);
  ArduinoOTA.handle();
  http.handleClient();
  webSocket.loop();
  // And let's animate this shit, if we're an interlock.
  ws2812fx.service();
  delay(10);

  // If it's been more than rfidSquelchTime since we last read a card, then try to read a card.
  if (millis() > (lastReadSuccess + rfidSquelchTime)) {
    if (!contact) {
      statusLight('b');
    } else {
      statusLight('g');
    }
    if (Serial.available()) {
      if (deviceType == "interlock") {
        readTagInterlock();
        delay(10);
      } else {
        readTagDoor();
        delay(10);
      }

    }
  } else {
    flushSerial();
    delay(10);
  }
  delay(10);

}

void getCache () {
  log("[CACHE] Acquiring cache.");
  // Delay to clear wifi buffer.
  delay(10);
  String url = String(host) + "/api/" + deviceType + "/authorised/?secret=" + String(secret);
  log("[CACHE] Get:" + String(url));
  client.begin(url);

  // Start http request.
  int httpCode = client.GET();
  // httpCode will be negative on error
  if (httpCode > 0) {
    // log("[SESSION] Code: " + String(httpCode));

    // Checkin succeeded.
    if (httpCode == HTTP_CODE_OK) {
      String payload = client.getString();
      log("[CACHE] Server Response: " + payload);
      File cacheFile = SPIFFS.open("/authorised.json", "w");
      if (!cacheFile) {
        log("[CACHE] Error opening authorised json file.");
      } else {
        cacheFile.print(payload + '\n');
        cacheFile.close();
      }
      // Pull hash from the response, store in string in RAM.
      DynamicJsonBuffer jsonBuffer;
      JsonObject&root = jsonBuffer.parseObject(payload.substring(payload.indexOf('{'), payload.length()));
      curCacheHash = root["authorised_tags_hash"].as<String>();

    }
  } else {
    log("[CACHE] Error: " + client.errorToString(httpCode));
  }
  client.end();
  log("[CACHE] Cache acquisition done.");
  delay(10);
}

void printCache() {
  String cacheContent;
  File cacheFile = SPIFFS.open("/authorised.json", "r");
  if (!cacheFile) {
    cacheContent = "Error opening authorised json file.";
  } else {
    cacheContent = cacheFile.readStringUntil('\n');
  }
  cacheFile.close();

  String message = "<html><head><title>" + String(deviceName) + " Cache</title></head>";
  message += "<h2>Cache:</h2>";
  message += cacheContent;
  http.send(200, "text/html", message);
}

void loadTags() {
  String cacheContent;
  File cacheFile = SPIFFS.open("/authorised.json", "r");
  if (!cacheFile) {
    cacheContent = "Error opening authorised json file.";
  } else {
    cacheContent = cacheFile.readStringUntil('\n');
  }
  cacheFile.close();
  DynamicJsonBuffer jsonBuffer;
  JsonObject&root = jsonBuffer.parseObject(cacheContent.substring(cacheContent.indexOf('{'), cacheContent.length()));
  JsonArray& authorised_tags = root["authorised_tags"];
  authorised_tags.copyTo(tagsArray);
}

void printTags() {
  String message = "<html><head><title>" + String(deviceName) + " Tags</title></head>";
  message += "<h2>Tags:</h2>";
  if (tagsArray[0] > 0) {
    for (int i = 0; i < sizeof(tagsArray) / sizeof(int); i++) {
      if (tagsArray[i] > 0) {
        message += String(tagsArray[i]) + "<br />";

      }
    }
  } else {
    message+= "No tag loaded in slot 0, assuming none loaded.";
  }

  http.send(200, "text/html", message);
}

