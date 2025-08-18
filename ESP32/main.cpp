// 📦 Bibliotheken
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <U8g2lib.h>
#include <ESP32Servo.h>

// 📌 Pinconfiguratie (AANGEPAST)
#define SDA_PIN 21
#define SCL_PIN 22
#define RED_PIN D2
#define GREEN_PIN D3
#define BUZZER_PIN 13
#define SERVO_PIN 2
#define DOOR_LED_PIN 12 // <-- CORRECTIE: Veranderd van D13 om conflict met buzzer te voorkomen

// 🌐 WiFi + Firebase instellingen
const char* ssid = "***********";
const char* password = "***********";
const char* firebaseApiKey = "***********"; 
const char* projectId = "*****************";                         
const char* collPersoneel = "****************";
const char* collLog = "****************";

// ⏰ Tijd
const long gmtOffset = 3600;      // 1 uur (voor CET)
const int daylightOffset = 3600;  // 1 uur voor zomertijd (CEST)

// 📟 OLED-scherm
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);

// 🔐 RFID & Admin
const char* MY_ADMIN_UID = "*********"; // <-- CORRECTIE: Naam veranderd om conflict te voorkomen 
Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);

// ⚙️ Servo
Servo servo;

// 🧠 Globale variabelen
String laatstGescandeUID = "";      // Voor RFID anti-dubbelscan

// --- Timers voor non-blocking operaties ---
unsigned long lastRfidScanTime = 0;
const long rfidScanInterval = 500; // Hoe vaak we de RFID-lezer checken (ms)
unsigned long lastDisplayClearTime = 0;
const long displayMessageDuration = 1500; // Hoe lang een boodschap op het scherm blijft staan (ms)
unsigned long doorOpenStartTime = 0;
const long doorOpenDuration = 5000; // Hoe lang de deur open blijft (ms)

// --- RFID State Machine ---
enum RfidState {
  RFID_IDLE,
  RFID_PROCESSING,
  RFID_ACCESS_GRANTED,
  RFID_ACCESS_DENIED
};
RfidState currentRfidState = RFID_IDLE;

// --- FUNCTIE PROTOTYPES ---
void showOLED(String regel1, String regel2);

// --- Verbinding maken met WiFi ---
void connectWiFi() {
  Serial.print("Verbinden met WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 30000) { // Max 30 sec wachten
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Verbonden met WiFi!");
    Serial.print("IP Adres: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n❌ WiFi verbinding MISLUKT!");
    showOLED("WiFi Fout!", "Admin werkt"); // Aangepaste boodschap
  }
}

// 🕒 Tijd synchroniseren
void setupTime() {
  if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⏰ Tijd synchronisatie overgeslagen (geen WiFi)");
      return;
  }
  Serial.println("Synchroniseren van tijd...");
  configTime(gmtOffset, daylightOffset, "pool.ntp.org", "time.nist.gov");
  unsigned long startTime = millis();
  while (time(nullptr) < 100000 && millis() - startTime < 10000) { // Max 10 seconden wachten
    delay(500);
    Serial.print("*");
  }
  if (time(nullptr) < 100000) {
    Serial.println("\n⏰ Tijd synchronisatie MISLUKT!");
    showOLED("Tijd Fout!", "Geen NTP");
  } else {
    Serial.println("\n⏰ Tijd gesynchroniseerd!");
  }
}

String getTijd() {
  if (WiFi.status() != WL_CONNECTED) {
    return "N/A (offline)"; // Geef een offline indicatie terug
  }
  time_t now = time(nullptr);
  struct tm* tijd = localtime(&now);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tijd);
  return String(buffer);
}

// ⚙️ Servo bedienen: deur openen
void openDeur() {
  Serial.println("Deur wordt geopend...");
  servo.write(180); // Open de deur (aanpassen afhankelijk van servo instelling)
  doorOpenStartTime = millis(); // Start timer voor hoe lang de deur open blijft
  digitalWrite(GREEN_PIN, LOW);     // Groene LED aan (Actief Laag)
  digitalWrite(BUZZER_PIN, HIGH);   // Buzzer aan (Actief Hoog)
  digitalWrite(DOOR_LED_PIN, HIGH); // Deur LED aan wanneer deur opent
  delay(100); // Korte delay voor buzzergeluid
  digitalWrite(BUZZER_PIN, LOW);
  delay(100);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
}

// ⚙️ Servo bedienen: deur sluiten
void closeDeur() {
  Serial.println("Deur wordt gesloten...");
  servo.write(0);  // Sluit de deur (terug naar beginpositie)
  digitalWrite(GREEN_PIN, HIGH);    // Groene LED uit (Actief Laag)
  digitalWrite(DOOR_LED_PIN, LOW);  // Deur LED uit wanneer deur sluit
}

// 📟 Tonen van tekst op een OLED-scherm
void showOLED(String regel1, String regel2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB14_tr); // Groter lettertype voor regel 1
  u8g2.drawStr(0, 20, regel1.c_str());
  u8g2.setFont(u8g2_font_ncenB10_tr); // Kleiner lettertype voor regel 2
  u8g2.drawStr(0, 50, regel2.c_str());
  u8g2.sendBuffer();
  lastDisplayClearTime = millis(); // Reset timer voor het wissen van het scherm
}

// 📡 Communiceren met Firestore voor check van UID en logboek van pogingen
void controleerUID(String uid) {
  currentRfidState = RFID_PROCESSING; // Zet de status naar processing

  // =========================================================================
  // --- EERST CONTROLEREN OP ADMIN UID ---
  // Dit gebeurt lokaal, zonder dat er een WiFi-verbinding nodig is.
  // =========================================================================
  if (uid.equalsIgnoreCase(MY_ADMIN_UID)) {
    Serial.println("✅ ADMIN UID gedetecteerd! Toegang lokaal verleend.");
    showOLED("Welkom", "Admin");
    openDeur();
    currentRfidState = RFID_ACCESS_GRANTED;
    
    // Optioneel: probeer de admin-toegang alsnog te loggen als er wel WiFi is.
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Logboek opslaan voor Admin...");
      HTTPClient post;
      String logUrl = "https://firestore.googleapis.com/v1/projects/" + String(projectId) + "/databases/(default)/documents/" + String(collLog) + "?key=" + firebaseApiKey;
      post.begin(logUrl);
      post.addHeader("Content-Type", "application/json");

      JsonDocument logDoc;
      logDoc["fields"]["Tijd"]["stringValue"] = getTijd();
      logDoc["fields"]["UID"]["stringValue"] = uid;
      logDoc["fields"]["Gebruiker"]["stringValue"] = "Admin (Lokaal)";
      logDoc["fields"]["Resultaat"]["stringValue"] = "Toegang toegestaan";
      logDoc["fields"]["Locatie"]["stringValue"] = "Kantoor";
      String logBody;
      serializeJson(logDoc, logBody);

      int postResponseCode = post.POST(logBody);
      if (postResponseCode == HTTP_CODE_OK) {
        Serial.println("✅ Admin-logboek succesvol opgeslagen.");
      } else {
        Serial.printf("❌ Fout bij admin-logboek opslaan (%d)\n", postResponseCode);
      }
      post.end();
    }
    
    return; // BELANGRIJK: Stop de functie hier.
  }

  // De rest van de code wordt alleen uitgevoerd als het NIET de Admin UID is.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ Geen WiFi verbinding voor UID controle.");
    showOLED("Fout!", "Geen WiFi");
    digitalWrite(RED_PIN, LOW); // Rode LED aan
    digitalWrite(BUZZER_PIN, HIGH); delay(500); digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(RED_PIN, HIGH); // Rode LED uit
    currentRfidState = RFID_IDLE; // Direct terug naar IDLE, probeer opnieuw
    return;
  }

  HTTPClient http;
  String url = "https://firestore.googleapis.com/v1/projects/" + String(projectId) + "/databases/(default)/documents/" + String(collPersoneel) + "/" + uid + "?key=" + firebaseApiKey;
  http.begin(url);

  Serial.print("Controleer UID: "); Serial.println(uid);
  Serial.print("Firestore GET URL: "); Serial.println(url);

  int code = http.GET();
  String naamGebruiker = "Onbekend"; // Standaardwaarde
  String resultaat = "Toegang geweigerd";

  Serial.print("HTTP GET Response Code: "); Serial.println(code);

  if (code == HTTP_CODE_OK) { // HTTP_CODE_OK is 200
    String res = http.getString();
    Serial.print("Firestore Response: "); Serial.println(res);
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, res);

    if (error) {
      Serial.print("deserializeJson() mislukt: ");
      Serial.println(error.f_str());
    } else {
      // <-- CORRECTIE: Haal 'voornaam' en 'achternaam' op en voeg ze samen.
      String voornaam = doc["fields"]["voornaam"]["stringValue"] | "";
      String achternaam = doc["fields"]["achternaam"]["stringValue"] | "";
      
      naamGebruiker = voornaam + " " + achternaam;
      naamGebruiker.trim(); // Verwijdert eventuele spaties aan begin/einde

      // Als de naam na het samenvoegen nog steeds leeg is, gebruik dan "Onbekend"
      if (naamGebruiker.length() == 0) {
        naamGebruiker = "Onbekend";
      }
      // <-- EINDE CORRECTIE
      
      resultaat = "Toegang toegestaan";
    }
    Serial.println("✅ Toegang toegestaan!");
    showOLED("Toegestaan!", ("Welkom " + naamGebruiker).c_str());
    openDeur();
    currentRfidState = RFID_ACCESS_GRANTED;
  } else {
    Serial.println("❌ Toegang geweigerd!");
    digitalWrite(RED_PIN, LOW);
    digitalWrite(BUZZER_PIN, HIGH); delay(700); digitalWrite(BUZZER_PIN, LOW);
    showOLED("Geweigerd", "Geen toegang");
    digitalWrite(RED_PIN, HIGH);
    currentRfidState = RFID_ACCESS_DENIED;
  }
  http.end();

  // Logboek opslaan
  Serial.println("Logboek opslaan...");
  static HTTPClient post;
  String logUrl = "https://firestore.googleapis.com/v1/projects/" + String(projectId) + "/databases/(default)/documents/" + String(collLog) + "?key=" + firebaseApiKey;
  post.begin(logUrl);
  post.addHeader("Content-Type", "application/json");

  JsonDocument logDoc;
  logDoc["fields"]["Tijd"]["stringValue"] = getTijd();
  logDoc["fields"]["UID"]["stringValue"] = uid;
  logDoc["fields"]["Gebruiker"]["stringValue"] = naamGebruiker;
  logDoc["fields"]["Resultaat"]["stringValue"] = resultaat;
  logDoc["fields"]["Locatie"]["stringValue"] = "Kantoor";
  String logBody;
  serializeJson(logDoc, logBody);

  Serial.print("Logboek payload: "); Serial.println(logBody);

  int postResponseCode = post.POST(logBody);
  Serial.print("HTTP POST Response code voor logboek: "); Serial.println(postResponseCode);
  if (postResponseCode == HTTP_CODE_OK) {
    Serial.println("✅ Logboek succesvol opgeslagen.");
  } else {
    Serial.printf("❌ Fout bij logboek opslaan (%d): %s\n", postResponseCode, http.errorToString(postResponseCode).c_str());
  }
  post.end();
}

// 🟢 Setup
void setup() {
  Serial.begin(115200);
  Serial.println("Systeem opstarten...");

  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(DOOR_LED_PIN, OUTPUT); // <-- CORRECTIE: Deze pin moet nu ook ingesteld worden

  digitalWrite(RED_PIN, HIGH);
  digitalWrite(GREEN_PIN, HIGH);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(DOOR_LED_PIN, LOW);

  servo.attach(SERVO_PIN, 500, 2400);
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();
  nfc.begin();
  nfc.SAMConfig();

  connectWiFi();
  setupTime();

  showOLED("Klaar!", "Scan badge");
  Serial.println("Systeem gereed!");
}

// 🔁 Loop
void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastRfidScanTime >= rfidScanInterval && currentRfidState == RFID_IDLE) {
    lastRfidScanTime = currentMillis;

    uint8_t uid[7];
    uint8_t uidLength;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
      String uidStr = "";
      for (int i = 0; i < uidLength; i++) {
        if (uid[i] < 0x10) uidStr += "0";
        uidStr += String(uid[i], HEX);
      }
      uidStr.toUpperCase();

      if (uidStr != laatstGescandeUID) {
        laatstGescandeUID = uidStr;
        controleerUID(uidStr);
      }
    }
  }

  if (doorOpenStartTime > 0 && currentMillis - doorOpenStartTime >= doorOpenDuration) {
    closeDeur();
    doorOpenStartTime = 0;
  }

  if (currentRfidState != RFID_PROCESSING && currentMillis - lastDisplayClearTime >= displayMessageDuration) {
      showOLED("Klaar!", "Scan badge");
      currentRfidState = RFID_IDLE;
      laatstGescandeUID = "";
  }
  
  delay(10);
}
