#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>

// --- Configuration OLED ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- ICONES BITMAP (8x8 pixels) ---
const unsigned char icon_wifi_ok[] PROGMEM = {
  0x00, 0x3C, 0x7E, 0xDB, 0x99, 0x18, 0x18, 0x00 // Symbole Wi-Fi classique
};
const unsigned char icon_wifi_no[] PROGMEM = {
  0x00, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x00 // Croix (X)
};
const unsigned char icon_ap[] PROGMEM = {
  0x18, 0x24, 0x42, 0x99, 0x24, 0x42, 0x81, 0x00 // Symbole émission/point d'accès
};

// --- Configuration Débitmètre ---
const int flowSensorPin = 18; 
volatile long pulseCount = 0;
float flowRate = 0.0;
float totalLitres = 0.0;
unsigned long oldTime = 0;
float calibrationFactor = 5.0; 

// --- Configuration Buffer (Robustesse) ---
// 120 slots * 10s = 20 minutes d'autonomie sans Wi-Fi
#define BUFFER_SIZE 120 
struct DataPoint {
  float debit;
  float total;
};
DataPoint dataBuffer[BUFFER_SIZE];
int bufferHead = 0; // Tête d'écriture
int bufferTail = 0; // Queue de lecture
int bufferCount = 0; // Nombre d'éléments en attente

// --- Configuration Wi-Fi ---
Preferences preferences;
WebServer server(80);
String apSSID = "Pinty";
bool wifiConnected = false;
bool isConnecting = false;       // Nouvelle variable pour état transitoire
unsigned long wifiStartTime = 0; // Pour timeout connexion
bool apMode = false;
const unsigned long reconnectInterval = 30000;
unsigned long lastReconnectAttempt = 0;

// --- Configuration Webhook ---
// Mise à jour URL (sans l'étoile à la fin qui est souvent pour le routage serveur)
const char* webhookURL = "https://app.hooklistener.com/w/my-first-endpoint-k9dt";
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 10000;
String sendStatus = "INIT";

// Interruption : DOIT être rapide
void IRAM_ATTR pulseCounter() {
  pulseCount++;
}

// --- Gestion Web (Interface Mobile Style) ---
void handleRoot() {
  float currentKFactor = preferences.getFloat("kfactor", 5.0);
  
  String html = "<!DOCTYPE html><html lang='fr'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', Helvetica, Arial, sans-serif; background-color: #f4f4f9; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; }";
  html += ".card { background: white; padding: 2rem; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.1); width: 90%; max-width: 400px; text-align: center; }";
  html += "h1 { color: #333; margin-bottom: 1.5rem; font-size: 1.5rem; }";
  html += "input { width: 100%; padding: 12px; margin: 10px 0; border: 1px solid #ddd; border-radius: 8px; box-sizing: border-box; font-size: 16px; }";
  html += "button { width: 100%; padding: 12px; margin-top: 10px; border: none; border-radius: 8px; font-size: 16px; font-weight: bold; cursor: pointer; transition: background 0.3s; }";
  html += ".btn-save { background-color: #007bff; color: white; }";
  html += ".btn-save:hover { background-color: #0056b3; }";
  html += ".btn-warn { background-color: #fff; color: #dc3545; border: 2px solid #dc3545; margin-top: 2rem; }";
  html += ".btn-warn:hover { background-color: #dc3545; color: white; }";
  html += "label { display: block; text-align: left; font-weight: 500; margin-top: 10px; color: #555; }";
  html += "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h1>Configuration Pinty 🍺</h1>";
  html += "<form action='/save' method='POST'>";
  
  html += "<label>R&eacute;seau Wi-Fi (SSID)</label>";
  html += "<input type='text' name='ssid' placeholder='Nom de la box' required>";
  
  html += "<label>Mot de passe</label>";
  html += "<input type='password' name='pass' placeholder='Cl&eacute; Wi-Fi'>";
  
  html += "<label>Calibration (K-Factor)</label>";
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
  preferences.putFloat("kfactor", server.arg("kfactor").toFloat());
  server.send(200, "text/html", "<!DOCTYPE html><html lang='fr'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:sans-serif;text-align:center;padding:50px;}</style></head><body><h1>Sauvegard&eacute; !</h1><p>Red&eacute;marrage en cours...</p></body></html>");
  delay(500);
  ESP.restart();
}

void handleReset() {
  totalLitres = 0.0;
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
    Serial.println("Mode AP activé pour configuration");
  }
}

// Nouvelle fonction de connexion non-bloquante
void startWiFiConnection() {
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("pass", "");
  
  if (ssid != "") {
    WiFi.begin(ssid.c_str(), pass.c_str());
    isConnecting = true;
    wifiStartTime = millis();
    sendStatus = "WIFI...";
    Serial.println("Tentative connexion Wi-Fi (non bloquant)...");
  } else {
    startAP();
  }
}

// Gestion Buffer Circulaire
void addToBuffer(float debit, float total) {
  dataBuffer[bufferHead] = {debit, total};
  bufferHead = (bufferHead + 1) % BUFFER_SIZE; // Avance et boucle si fin
  
  if (bufferCount < BUFFER_SIZE) {
    bufferCount++;
  } else {
    // Si plein, on écrase le plus vieux, donc la queue doit avancer aussi
    bufferTail = (bufferTail + 1) % BUFFER_SIZE;
    Serial.println("Buffer plein: ancienne donnée écrasée !");
  }
}

void sendBatch() {
  if (bufferCount == 0 || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(webhookURL);
  http.addHeader("Content-Type", "application/json");

  // On envoie max 10 items par requête pour ne pas saturer la mémoire
  int batchSize = (bufferCount > 10) ? 10 : bufferCount;
  
  String json = "{\"batch\":[";
  for (int i = 0; i < batchSize; i++) {
    int index = (bufferTail + i) % BUFFER_SIZE;
    json += "{\"d\":" + String(dataBuffer[index].debit, 2) + ",\"t\":" + String(dataBuffer[index].total, 2) + "}";
    if (i < batchSize - 1) json += ",";
  }
  json += "]}";
  Serial.println("Envoi batch: " + json); // Debug

  int code = http.POST(json);
  if (code > 0) {
    // Succès : on libère les slot envoyés
    bufferTail = (bufferTail + batchSize) % BUFFER_SIZE;
    bufferCount -= batchSize;
    sendStatus = "SYNC OK";
    Serial.printf("Envoyé %d items. Reste: %d\n", batchSize, bufferCount);
  } else {
    sendStatus = "ERR HTTP";
  }
  http.end();
}

void setup() {
  Serial.begin(115200);

  // 1. DÉMARRAGE CAPTEUR (PRIORITÉ MAX)
  // On attache l'interruption AVANT tout le reste (Wi-Fi, OLED...)
  pinMode(flowSensorPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(flowSensorPin), pulseCounter, FALLING);
  Serial.println("Capteur actif immédiatement.");

  // 2. Init OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for(;;); // Bloque si pas d'écran (erreur matérielle grave)
  }
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1); // Petite police par défaut pour le boot
  
  // Petit logo de boot (texte temporaire)
  display.setCursor(40, 30);
  display.println("PINTY");
  display.display();
  delay(500);

  // 3. Chargement Config
  preferences.begin("wifi-config", false);
  calibrationFactor = preferences.getFloat("kfactor", 5.0);

  // 4. Lancement Wi-Fi (Non Bloquant)
  startWiFiConnection();
}

void loop() {
  // --- Gestion Wi-Fi (Correction BUG Déconnexion) ---
  
  // 1. Si on était connecté mais que le signal est perdu
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    isConnecting = false;
    startAP(); // Relancer l'AP immédiatement si on perd le réseau
    lastReconnectAttempt = millis(); // Préparer la reconnexion
    Serial.println("Signal Wi-Fi perdu ! Passage en mode secours.");
  }

  // 2. Gestion connexion initiale
  if (isConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      isConnecting = false;
      wifiConnected = true;
      Serial.println("Wi-Fi OK !");
      if (apMode) {
        WiFi.softAPdisconnect(true); // Coupe l'AP quand connecté
        apMode = false;
      }
    } else if (millis() - wifiStartTime > 10000) {
      isConnecting = false;
      wifiConnected = false;
      startAP(); 
      Serial.println("Timeout connexion -> Mode AP");
    }
  }

  // 3. Tentative reconnexion périodique
  if (!wifiConnected && !isConnecting && (millis() - lastReconnectAttempt > reconnectInterval)) {
    startWiFiConnection();
    lastReconnectAttempt = millis();
  }

  if (apMode) server.handleClient();

  // --- Logique Métier (Toute les secondes) ---
  if ((millis() - oldTime) > 1000) {
    detachInterrupt(digitalPinToInterrupt(flowSensorPin));
    flowRate = (float)pulseCount / calibrationFactor; 
    float addedLitres = (flowRate / 60.0);
    totalLitres += addedLitres;
    oldTime = millis();
    pulseCount = 0;
    attachInterrupt(digitalPinToInterrupt(flowSensorPin), pulseCounter, FALLING);

    // Ajouter au buffer si pas de Wi-Fi ou si buffer déjà rempli
    if (!wifiConnected || bufferCount > 0) {
      addToBuffer(flowRate, totalLitres);
    }

    // --- MISE À JOUR OLED (ICONES) ---
    display.clearDisplay();
    
    // Icone Wi-Fi (Coin haut gauche)
    if (isConnecting) {
      // Clignotement simple si connexion en cours
      if ((millis() / 500) % 2 == 0) display.drawBitmap(0, 0, icon_wifi_ok, 8, 8, WHITE);
    } else if (wifiConnected) {
      display.drawBitmap(0, 0, icon_wifi_ok, 8, 8, WHITE);
    } else {
      display.drawBitmap(0, 0, icon_wifi_no, 8, 8, WHITE);
    }

    // Icone Mode AP (Coin haut droit) - Seulement si AP actif
    if (apMode) {
      display.drawBitmap(120, 0, icon_ap, 8, 8, WHITE);
    }

    // Statut Buffer (Texte petit au milieu haut)
    display.setFont(NULL);
    display.setCursor(40, 0);
    if (bufferCount > 0) {
      display.print("BUF:");
      display.print(bufferCount);
    } 

    display.drawFastHLine(0, 10, 128, WHITE);

    // Données Principales
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

  // --- Envoi Données (Non Bloquant) ---
  if (wifiConnected && !isConnecting && (millis() - lastSendTime > 1000)) { 
    if (bufferCount > 0) {
      sendBatch();
      lastSendTime = millis(); 
    }
  }
}