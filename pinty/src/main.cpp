#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>

// --- Configuration OLED ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- ICONES BITMAP (8x8 pixels) ---
const unsigned char icon_wifi_ok[] PROGMEM = {
  0x00, 0x3C, 0x7E, 0xDB, 0x99, 0x18, 0x18, 0x00
};
const unsigned char icon_wifi_no[] PROGMEM = {
  0x00, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x00
};
const unsigned char icon_ap[] PROGMEM = {
  0x18, 0x24, 0x42, 0x99, 0x24, 0x42, 0x81, 0x00
};
const unsigned char icon_mqtt[] PROGMEM = {
  0x18, 0x3C, 0x7E, 0xFF, 0x18, 0x18, 0x18, 0x18
};

// --- Configuration DÃ©bitmÃ¨tre ---
const int flowSensorPin = 18;
volatile long pulseCount = 0;
float flowRate = 0.0;
float totalLitres = 0.0;
unsigned long oldTime = 0;
float calibrationFactor = 5.0;

// --- Configuration Buffer ---
#define BUFFER_SIZE 120
struct DataPoint {
  float debit;
  float total;
  unsigned long timestamp;
};
DataPoint dataBuffer[BUFFER_SIZE];
int bufferHead = 0;
int bufferTail = 0;
int bufferCount = 0;

// --- Configuration Wi-Fi ---
Preferences preferences;
WebServer server(80);
String apSSID = "Pinty";
bool wifiConnected = false;
bool isConnecting = false;
unsigned long wifiStartTime = 0;
bool apMode = false;
const unsigned long reconnectInterval = 30000;
unsigned long lastReconnectAttempt = 0;

// --- Configuration MQTT ---
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
String mqttServer = "";
int mqttPort = 8883;
String mqttUser = "";
String mqttPass = "";
String mqttTopic = "pinty/dev01";
bool mqttConnected = false;
unsigned long lastMQTTAttempt = 0;
const unsigned long mqttRetryInterval = 5000;
String mqttStatus = "INIT";

// Interruption
void IRAM_ATTR pulseCounter() {
  pulseCount++;
}

// --- Callback MQTT : Reception commandes ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("MQTT reÃ§u [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  String topicStr = String(topic);
  if (topicStr == mqttTopic + "/cmd") {
    if (message == "RESET") {
      totalLitres = 0.0;
      bufferCount = 0;
      bufferHead = 0;
      bufferTail = 0;
      mqttClient.publish((mqttTopic + "/status").c_str(), "RESET_OK");
      Serial.println(">>> RESET EXECUTÃ‰ <<<");
    }
  }
}

// --- Gestion Web (Interface Mobile) ---
void handleRoot() {
  float currentKFactor = preferences.getFloat("kfactor", 5.0);
  String currentMQTTServer = preferences.getString("mqttserver", "");
  String currentMQTTUser = preferences.getString("mqttuser", "");

  String html = "<!DOCTYPE html><html lang='fr'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', Helvetica, Arial, sans-serif; background-color: #f4f4f9; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; }";
  html += ".card { background: white; padding: 2rem; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.1); width: 90%; max-width: 400px; }";
  html += "h1 { color: #333; margin-bottom: 1rem; font-size: 1.5rem; text-align: center; }";
  html += "h2 { color: #666; margin-top: 1.5rem; margin-bottom: 0.5rem; font-size: 1.1rem; border-bottom: 2px solid #007bff; padding-bottom: 5px; }";
  html += "input { width: 100%; padding: 12px; margin: 10px 0; border: 1px solid #ddd; border-radius: 8px; box-sizing: border-box; font-size: 16px; }";
  html += "button { width: 100%; padding: 12px; margin-top: 10px; border: none; border-radius: 8px; font-size: 16px; font-weight: bold; cursor: pointer; transition: background 0.3s; }";
  html += ".btn-save { background-color: #007bff; color: white; }";
  html += ".btn-save:hover { background-color: #0056b3; }";
  html += ".btn-warn { background-color: #fff; color: #dc3545; border: 2px solid #dc3545; margin-top: 2rem; }";
  html += ".btn-warn:hover { background-color: #dc3545; color: white; }";
  html += "label { display: block; text-align: left; font-weight: 500; margin-top: 10px; color: #555; font-size: 14px; }";
  html += ".hint { font-size: 12px; color: #888; margin-top: -5px; margin-bottom: 10px; }";
  html += "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h1>Configuration Pinty ðŸº</h1>";
  html += "<form action='/save' method='POST'>";
  
  html += "<h2>Wi-Fi</h2>";
  html += "<label>R&eacute;seau (SSID)</label>";
  html += "<input type='text' name='ssid' placeholder='Nom de la box' required>";
  
  html += "<label>Mot de passe</label>";
  html += "<input type='password' name='pass' placeholder='Cl&eacute; Wi-Fi'>";
  
  html += "<h2>MQTT Broker</h2>";
  html += "<label>Serveur HiveMQ</label>";
  html += "<input type='text' name='mqttserver' value='" + currentMQTTServer + "' placeholder='xxxxx.s1.eu.hivemq.cloud' required>";
  html += "<p class='hint'>URL sans http:// ni port</p>";
  
  html += "<label>Utilisateur MQTT</label>";
  html += "<input type='text' name='mqttuser' value='" + currentMQTTUser + "' placeholder='pinty_esp32' required>";
  
  html += "<label>Mot de passe MQTT</label>";
  html += "<input type='password' name='mqttpass' placeholder='Mot de passe broker'>";
  
  html += "<label>Topic de base</label>";
  html += "<input type='text' name='mqtttopic' value='" + mqttTopic + "' placeholder='pinty/dev01' required>";
  
  html += "<h2>Calibration</h2>";
  html += "<label>K-Factor</label>";
  html += "<input type='number' step='0.1' name='kfactor' value='" + String(currentKFactor, 1) + "' required>";
  
  html += "<button type='submit' class='btn-save'>Enregistrer et Red&eacute;marrer</button>";
  html += "</form>";
  
  html += "<form action='/reset' method='POST'><button class='btn-warn' type='submit'>RAZ Compteur (0L)</button></form>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleSave() {
  preferences.putString("ssid", server.arg("ssid"));
  preferences.putString("pass", server.arg("pass"));
  preferences.putString("mqttserver", server.arg("mqttserver"));
  preferences.putString("mqttuser", server.arg("mqttuser"));
  if (server.arg("mqttpass") != "") {
    preferences.putString("mqttpass", server.arg("mqttpass"));
  }
  preferences.putString("mqtttopic", server.arg("mqtttopic"));
  preferences.putFloat("kfactor", server.arg("kfactor").toFloat());
  
  server.send(200, "text/html", "<!DOCTYPE html><html lang='fr'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:sans-serif;text-align:center;padding:50px;}</style></head><body><h1>Sauvegard&eacute; !</h1><p>Red&eacute;marrage en cours...</p></body></html>");
  delay(500);
  ESP.restart();
}

void handleReset() {
  totalLitres = 0.0;
  bufferCount = 0;
  bufferHead = 0;
  bufferTail = 0;
  server.send(200, "text/html", "<!DOCTYPE html><html lang='fr'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:sans-serif;text-align:center;padding:50px;}</style></head><body><h1>Compteur &agrave; Z&eacute;ro !</h1><p><a href='/'>Retour</a></p></body></html>");
}

void startAP() {
  if (!apMode) {
    WiFi.softAP(apSSID.c_str());
    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/reset", HTTP_POST, handleReset);
    server.begin();
    apMode = true;
    Serial.println("Mode AP activÃ© pour configuration");
  }
}

void startWiFiConnection() {
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("pass", "");
  
  if (ssid != "") {
    WiFi.begin(ssid.c_str(), pass.c_str());
    isConnecting = true;
    wifiStartTime = millis();
    mqttStatus = "WIFI...";
    Serial.println("Tentative connexion Wi-Fi...");
  } else {
    startAP();
  }
}

void connectMQTT() {
  if (mqttServer == "" || !wifiConnected) return;
  
  if (millis() - lastMQTTAttempt < mqttRetryInterval) return;
  lastMQTTAttempt = millis();
  
  Serial.print("Connexion MQTT...");
  String clientId = "Pinty-" + String(random(0xffff), HEX);
  
  if (mqttClient.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str())) {
    mqttConnected = true;
    mqttStatus = "MQTT OK";
    Serial.println(" OK!");
    
    // Subscribe aux commandes
    mqttClient.subscribe((mqttTopic + "/cmd").c_str());
    
    // Envoi status connexion
    mqttClient.publish((mqttTopic + "/status").c_str(), "ONLINE");
  } else {
    mqttConnected = false;
    mqttStatus = "MQTT ERR";
    Serial.print(" Ã‰chec, rc=");
    Serial.println(mqttClient.state());
  }
}

void addToBuffer(float debit, float total) {
  dataBuffer[bufferHead] = {debit, total, millis()};
  bufferHead = (bufferHead + 1) % BUFFER_SIZE;
  
  if (bufferCount < BUFFER_SIZE) {
    bufferCount++;
  } else {
    bufferTail = (bufferTail + 1) % BUFFER_SIZE;
  }
}

void publishBatch() {
  if (bufferCount == 0 || !mqttConnected) return;

  int batchSize = (bufferCount > 10) ? 10 : bufferCount;
  String json = "{\"batch\":[\"";
  
  for (int i = 0; i < batchSize; i++) {
    int index = (bufferTail + i) % BUFFER_SIZE;
    json += "{\"d\":" + String(dataBuffer[index].debit, 2) + 
            ",\"t\":" + String(dataBuffer[index].total, 2) + 
            ",\"ts\":" + String(dataBuffer[index].timestamp) + "}";
    if (i < batchSize - 1) json += ",";
  }
  json += "]}";

  if (mqttClient.publish((mqttTopic + "/batch").c_str(), json.c_str())) {
    bufferTail = (bufferTail + batchSize) % BUFFER_SIZE;
    bufferCount -= batchSize;
    mqttStatus = "SYNC";
    Serial.printf("PubliÃ© %d items, reste %d\n", batchSize, bufferCount);
  } else {
    mqttStatus = "PUB ERR";
  }
}

void setup() {
  Serial.begin(115200);

  // 1. Capteur en prioritÃ©
  pinMode(flowSensorPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(flowSensorPin), pulseCounter, FALLING);

  // 2. OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for(;;);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(40, 30);
  display.println("PINTY");
  display.display();
  delay(500);

  // 3. Chargement Config
  preferences.begin("wifi-config", false);
  calibrationFactor = preferences.getFloat("kfactor", 5.0);
  mqttServer = preferences.getString("mqttserver", "");
  mqttUser = preferences.getString("mqttuser", "");
  mqttPass = preferences.getString("mqttpass", "");
  mqttTopic = preferences.getString("mqtttopic", "pinty/dev01");

  // 4. Configuration MQTT
  espClient.setInsecure(); // Solution 1 : pas de vÃ©rification certificat
  mqttClient.setServer(mqttServer.c_str(), mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);

  // 5. Wi-Fi
  startWiFiConnection();
}

void loop() {
  // --- Gestion Wi-Fi ---
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    mqttConnected = false;
    isConnecting = false;
    startAP();
    lastReconnectAttempt = millis();
    Serial.println("Signal Wi-Fi perdu !");
  }

  if (isConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      isConnecting = false;
      wifiConnected = true;
      Serial.println("Wi-Fi OK !");
      if (apMode) {
        WiFi.softAPdisconnect(true);
        apMode = false;
      }
      connectMQTT(); // Connexion MQTT immÃ©diate
    } else if (millis() - wifiStartTime > 10000) {
      isConnecting = false;
      startAP();
    }
  }

  if (!wifiConnected && !isConnecting && (millis() - lastReconnectAttempt > reconnectInterval)) {
    startWiFiConnection();
    lastReconnectAttempt = millis();
  }

  // --- Gestion MQTT ---
  if (wifiConnected && !mqttConnected) {
    connectMQTT();
  }

  if (mqttConnected) {
    if (!mqttClient.connected()) {
      mqttConnected = false;
      mqttStatus = "MQTT DOWN";
    } else {
      mqttClient.loop(); // Important : Ã©coute des messages
    }
  }

  if (apMode) server.handleClient();

  // --- Logique MÃ©tier ---
  if ((millis() - oldTime) > 1000) {
    detachInterrupt(digitalPinToInterrupt(flowSensorPin));
    flowRate = (float)pulseCount / calibrationFactor; 
    float addedLitres = (flowRate / 60.0);
    totalLitres += addedLitres;
    oldTime = millis();
    pulseCount = 0;
    attachInterrupt(digitalPinToInterrupt(flowSensorPin), pulseCounter, FALLING);

    // Publish ou buffer
    if (mqttConnected && bufferCount == 0 && flowRate > 0) {
      char msg_flow[10], msg_total[10];
      dtostrf(flowRate, 4, 2, msg_flow);
      dtostrf(totalLitres, 4, 3, msg_total);
      
      mqttClient.publish((mqttTopic + "/flow").c_str(), msg_flow);
      mqttClient.publish((mqttTopic + "/total").c_str(), msg_total);
      mqttStatus = "PUB OK";
    } else if (!mqttConnected || bufferCount > 0) {
      addToBuffer(flowRate, totalLitres);
      mqttStatus = "BUFFER";
    }

    // Envoi buffer si prÃ©sent
    if (mqttConnected && bufferCount > 0) {
      publishBatch();
    }

    // --- OLED ---
    display.clearDisplay();
    
    // Icones
    if (isConnecting) {
      if ((millis() / 500) % 2 == 0) display.drawBitmap(0, 0, icon_wifi_ok, 8, 8, WHITE);
    } else if (wifiConnected) {
      display.drawBitmap(0, 0, icon_wifi_ok, 8, 8, WHITE);
    } else {
      display.drawBitmap(0, 0, icon_wifi_no, 8, 8, WHITE);
    }

    if (apMode) {
      display.drawBitmap(120, 0, icon_ap, 8, 8, WHITE);
    }

    if (mqttConnected) {
      display.drawBitmap(110, 0, icon_mqtt, 8, 8, WHITE);
    }

    // Status
    display.setFont(NULL);
    display.setCursor(10, 0);
    display.print(mqttStatus);
    
    if (bufferCount > 0) {
      display.setCursor(60, 0);
      display.print("B:");
      display.print(bufferCount);
    }

    display.drawFastHLine(0, 10, 128, WHITE);

    // DonnÃ©es
    display.setFont(&FreeSans9pt7b);
    display.setCursor(0, 30);
    display.print("Flux: ");
    display.print(flowRate, 1);
    display.println(" L/m");
    
    display.setCursor(0, 55);
    display.print("Tot: ");
    display.print(totalLitres, 2);
    display.print(" L");

    display.display();
  }
}
