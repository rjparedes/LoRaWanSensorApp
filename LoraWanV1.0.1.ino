/*
 * CÓDIGO FINAL LORAWAN - HELTEC V3.2 + SHT45
 * Librería LoRa: RadioLib
 * (ENFOQUE DEFINITIVO: Formato Universal Cayenne LPP)
 */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_SHT4x.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include "SSD1306Wire.h"
#include "pins_arduino.h"
#include <RadioLib.h>
#include <array>

// --- MACROS DE BATERÍA ---
#ifndef BAT_ADC
#define BAT_ADC 1
#endif
#ifndef BAT_ADC_CTRL
#define BAT_ADC_CTRL 37
#endif

// --- PINES Y CONSTANTES ---
#define SHT_SDA_PIN 41
#define SHT_SCL_PIN 42
#define CONFIG_BUTTON_PIN 0

#define DISPLAY_ON_TIME_MS 5000
#define DEEP_SLEEP_DURATION_SEC 300 
#define CONTINUOUS_LORA_INTERVAL_SEC 300 
#define CONTINUOUS_SCREEN_INTERVAL_SEC 5

// --- PINES DE RADIO PARA HELTEC V3 (SX1262) ---
#define RADIO_NSS    8
#define RADIO_DIO1   14
#define RADIO_NRST   12
#define RADIO_BUSY   13

// --- OBJETOS GLOBALES ---
TwoWire I2C_SHT = TwoWire(1);
Adafruit_SHT4x sht4 = Adafruit_SHT4x();
Preferences preferences;
SSD1306Wire display(0x3c, SDA_OLED, SCL_OLED);

SX1262 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_NRST, RADIO_BUSY);
LoRaWANNode node(&radio, &US915, 2); 

// --- VARIABLES GLOBALES DE TRABAJO ---
String devEUI_str, appEUI_str, appKey_str; 
String temp_devEUI_str = "";
String temp_appEUI_str = "";
String temp_appKey_str = "";

bool g_isCharging = false;
bool g_continuousMode = false;
float g_tempOffset = 0.0;
float g_humOffset = 0.0;
bool g_offlineMode = false;
bool g_displayEnabled = true; 
bool g_inConfigMode = false;

std::array<uint8_t, 16> g_nwkSKey;
std::array<uint8_t, 16> g_appSKey;

float g_lastValidTemp = 0.0;
float g_lastValidHum = 0.0;
float g_lastValidBatt = 0.0;
bool g_sensorReadOk = false;

unsigned long g_lastLoraPublishTime = 0;
unsigned long g_lastScreenUpdateTime = 0;

// --- UUIDs BLE ---
#define SERVICE_UUID              "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_DEVEUI_UUID          "beb5483e-36e1-4688-b7f5-ea07361b26a8" 
#define CHAR_APPEUI_UUID          "c269226e-41d3-4903-a151-c116b412b6a8" 
#define CHAR_APPKEY_UUID          "a1e3b54b-611c-43f6-86d1-c6d9c6c4c8e7" 
#define CHAR_CONTINUOUS_MODE_UUID "c5d804a0-5c6a-4b53-a15b-b72d110b3a31"
#define CHAR_TEMP_OFFSET_UUID     "e5d8a1b0-6c6a-4b53-a15b-b72d110b3a32"
#define CHAR_HUM_OFFSET_UUID      "f5d8a1c0-7c6a-4b53-a15b-b72d110b3a33"
#define CHAR_OFFLINE_MODE_UUID    "a5d8a1c0-7c6a-4b53-a15b-b72d110b3a34"
#define CHAR_DISPLAY_EN_UUID      "d6d8a1c0-7c6a-4b53-a15b-b72d110b3a35" 
#define CHAR_REINICIAR_UUID       "a8a9e40c-232a-43a0-9854-d88686a5127f"

// =================================================================
//                 FUNCIONES AUXILIARES
// =================================================================
uint32_t hexStringToUint32(String hexString) { return strtoul(hexString.c_str(), NULL, 16); }

void hexStringToBytes(String hexString, uint8_t* byteArr, int byteArrLen) {
  for(int i = 0; i < byteArrLen; i++) {
    String byteString = hexString.substring(i*2, i*2+2);
    *(byteArr + i) = (uint8_t) strtol(byteString.c_str(), NULL, 16);
  }
}

void VextON(void) { pinMode(Vext, OUTPUT); digitalWrite(Vext, LOW); }
void VextOFF(void) { pinMode(Vext, OUTPUT); digitalWrite(Vext, HIGH); }

void displayReset(void) {
  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, HIGH); delay(1);
  digitalWrite(RST_OLED, LOW); delay(1);
  digitalWrite(RST_OLED, HIGH); delay(1);
}

float getBatteryPercentage() {
  pinMode(BAT_ADC_CTRL, OUTPUT); digitalWrite(BAT_ADC_CTRL, HIGH); delay(50); 
  float totalMv = 0;
  for(int i=0; i < 20; i++) { totalMv += analogReadMilliVolts(BAT_ADC); delay(2); }
  digitalWrite(BAT_ADC_CTRL, LOW); pinMode(BAT_ADC_CTRL, INPUT);

  float battVoltage = ((totalMv / 20.0) / 1000.0) * 4.9; 
  long percentage = map((long)(battVoltage * 100), 340, 420, 0, 100);
  if (percentage > 100) percentage = 100; else if (percentage < 0) percentage = 0;
  return (float)percentage;
}

void drawBatteryBar(int x, int y, float percentage) {
  int barWidth = 60; int barHeight = 10;
  display.drawRect(x, y, barWidth, barHeight);
  display.fillRect(x + barWidth + 1, y + 2, 2, barHeight - 4);
  int fillWidth = (int)((percentage / 100.0) * (barWidth - 4));
  if (fillWidth < 0) fillWidth = 0; if (fillWidth > barWidth - 4) fillWidth = barWidth - 4;
  display.fillRect(x + 2, y + 2, fillWidth, barHeight - 4);
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(x + barWidth + 6, y, String((int)percentage) + "%");
}

void updateDisplay(String statusLine1) {
  if (!g_displayEnabled && !g_inConfigMode) { display.displayOff(); return; }
  display.displayOn(); display.clear();
  display.setFont(ArialMT_Plain_10); display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 0, statusLine1);

  if (g_sensorReadOk) {
    display.setFont(ArialMT_Plain_16); 
    display.drawString(0, 16, "T: " + String(g_lastValidTemp, 1) + " C");
    display.drawString(0, 32, "H: " + String(g_lastValidHum, 1) + " %");
  } else {
    display.setFont(ArialMT_Plain_16); 
    display.drawString(0, 16, "T: --.- C"); display.drawString(0, 32, "H: --.- %");
  }

  drawBatteryBar(0, 52, g_lastValidBatt); 
  display.setFont(ArialMT_Plain_10);
  if (g_isCharging) display.drawString(70, 52, "Cargando");
  else if (g_offlineMode) display.drawString(70, 52, "Offline");
  display.display();
}

void readSensors() {
  if (!sht4.begin(&I2C_SHT)) { g_sensorReadOk = false; return; }
  delay(500); 
  sensors_event_t humidity, temp;
  sht4.getEvent(&humidity, &temp);

  float currentTemp = temp.temperature + g_tempOffset;
  float currentHum = humidity.relative_humidity + g_humOffset;
  if (currentHum > 100.0) currentHum = 100.0; if (currentHum < 0.0) currentHum = 0.0;

  g_lastValidBatt = getBatteryPercentage();

  if (isnan(currentTemp) || isnan(currentHum) || (currentTemp == 0.0 && currentHum == 0.0)) {
    g_sensorReadOk = false;
  } else {
    g_lastValidTemp = currentTemp; g_lastValidHum = currentHum; g_sensorReadOk = true;
  }
}

bool connectLoRaWAN() {
  if (devEUI_str.length() < 8 || appEUI_str.length() != 32 || appKey_str.length() != 32) return false;
  int state = radio.begin();
  if (state != RADIOLIB_ERR_NONE) return false;

  uint32_t devAddr = hexStringToUint32(devEUI_str);
  hexStringToBytes(appEUI_str, g_nwkSKey.data(), 16);
  hexStringToBytes(appKey_str, g_appSKey.data(), 16);

  node.clearSession();
  state = node.beginABP(devAddr, g_nwkSKey.data(), g_nwkSKey.data(), g_nwkSKey.data(), g_appSKey.data());
  if (state == RADIOLIB_ERR_NONE) {
      state = node.activateABP();
      if (state == RADIOLIB_ERR_NONE || state == RADIOLIB_LORAWAN_NEW_SESSION || state == RADIOLIB_LORAWAN_SESSION_RESTORED) return true;
  }
  return false;
}

// =================================================================
//        NUEVA TRANSMISIÓN: FORMATO UNIVERSAL CAYENNE LPP
// =================================================================
void sendLoRaWANData() {
  if (g_offlineMode || !g_sensorReadOk) return;
  if (!connectLoRaWAN()) { delay(3000); return; }

  updateDisplay("Enviando LPP...");

  // Matemáticas estrictas del estándar Cayenne LPP
  int16_t tempLPP = (int16_t)(g_lastValidTemp * 10); 
  uint8_t humLPP = (uint8_t)(g_lastValidHum * 2); 
  int16_t battLPP = (int16_t)(g_lastValidBatt * 100); 

  // ¡Definimos todo el arreglo de un solo golpe sin usar corchetes!
  std::array<uint8_t, 11> txBuffer = {
    1, 103, (uint8_t)(tempLPP >> 8), (uint8_t)(tempLPP & 0xFF), // Sensor 1: Temp
    2, 104, humLPP,                                             // Sensor 2: Hum
    3, 2,   (uint8_t)(battLPP >> 8), (uint8_t)(battLPP & 0xFF)  // Sensor 3: Bat
  };

  int state = node.sendReceive(txBuffer.data(), 11, 1);
  
  if (state == RADIOLIB_ERR_NONE || state == RADIOLIB_ERR_RX_TIMEOUT) {
    updateDisplay("LPP Enviado OK");
  } else {
    updateDisplay("Error LPP");
  }
}

// =================================================================
//                 CALLBACKS BLE
// =================================================================
class MyCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String stringValue = String(pCharacteristic->getValue().c_str());
    String uuid = String(pCharacteristic->getUUID().toString().c_str());
    preferences.begin("lorawan", false);
    
    if (uuid.equals(CHAR_DEVEUI_UUID)) {
      temp_devEUI_str += stringValue;
      if (temp_devEUI_str.length() >= 8) {
          preferences.putString("devEUI", temp_devEUI_str.substring(0, 8));
          devEUI_str = temp_devEUI_str.substring(0, 8); temp_devEUI_str = ""; updateDisplay("DevAddr Guardado");
      }
    } else if (uuid.equals(CHAR_APPEUI_UUID)) {
      temp_appEUI_str += stringValue;
      if (temp_appEUI_str.length() >= 32) {
          preferences.putString("appEUI", temp_appEUI_str.substring(0, 32));
          appEUI_str = temp_appEUI_str.substring(0, 32); temp_appEUI_str = ""; updateDisplay("NwkSKey Guardada");
      }
    } else if (uuid.equals(CHAR_APPKEY_UUID)) {
      temp_appKey_str += stringValue;
      if (temp_appKey_str.length() >= 32) {
          preferences.putString("appKey", temp_appKey_str.substring(0, 32));
          appKey_str = temp_appKey_str.substring(0, 32); temp_appKey_str = ""; updateDisplay("AppSKey Guardada");
      }
    } else if (uuid.equals(CHAR_CONTINUOUS_MODE_UUID)) {
      preferences.putBool("cont_mode", stringValue.equals("1")); updateDisplay("Modo Cont.");
    } else if (uuid.equals(CHAR_TEMP_OFFSET_UUID)) {
      preferences.putFloat("temp_offset", round(stringValue.toFloat() * 10.0) / 10.0); updateDisplay("Offset T");
    } else if (uuid.equals(CHAR_HUM_OFFSET_UUID)) {
      preferences.putFloat("hum_offset", round(stringValue.toFloat() * 10.0) / 10.0); updateDisplay("Offset H");
    } else if (uuid.equals(CHAR_OFFLINE_MODE_UUID)) {
      preferences.putBool("offline_mode", stringValue.equals("1")); updateDisplay("Offline Mode");
    } else if (uuid.equals(CHAR_DISPLAY_EN_UUID)) {
      g_displayEnabled = stringValue.equals("1"); preferences.putBool("display_en", g_displayEnabled);
    } else if (uuid.equals(CHAR_REINICIAR_UUID)) {
      delay(1000); ESP.restart();
    }
    preferences.end();
  }
};

void runConfigMode() {
  g_inConfigMode = true; updateDisplay("MODO CONFIG BLE");
  BLEDevice::init("Heltec_LoRa_Node");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  MyCharacteristicCallbacks *callbacks = new MyCharacteristicCallbacks();

  (pService->createCharacteristic(CHAR_DEVEUI_UUID, BLECharacteristic::PROPERTY_WRITE))->setCallbacks(callbacks);
  (pService->createCharacteristic(CHAR_APPEUI_UUID, BLECharacteristic::PROPERTY_WRITE))->setCallbacks(callbacks);
  (pService->createCharacteristic(CHAR_APPKEY_UUID, BLECharacteristic::PROPERTY_WRITE))->setCallbacks(callbacks);
  (pService->createCharacteristic(CHAR_CONTINUOUS_MODE_UUID, BLECharacteristic::PROPERTY_WRITE))->setCallbacks(callbacks);
  (pService->createCharacteristic(CHAR_TEMP_OFFSET_UUID, BLECharacteristic::PROPERTY_WRITE))->setCallbacks(callbacks);
  (pService->createCharacteristic(CHAR_HUM_OFFSET_UUID, BLECharacteristic::PROPERTY_WRITE))->setCallbacks(callbacks);
  (pService->createCharacteristic(CHAR_OFFLINE_MODE_UUID, BLECharacteristic::PROPERTY_WRITE))->setCallbacks(callbacks);
  (pService->createCharacteristic(CHAR_DISPLAY_EN_UUID, BLECharacteristic::PROPERTY_WRITE))->setCallbacks(callbacks);
  (pService->createCharacteristic(CHAR_REINICIAR_UUID, BLECharacteristic::PROPERTY_WRITE))->setCallbacks(callbacks);

  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID); pAdvertising->setScanResponse(true); BLEDevice::startAdvertising();

  while (true) { g_lastValidBatt = getBatteryPercentage(); updateDisplay("App BLE..."); delay(5000); }
}

void setup() {
  Serial.begin(115200); delay(1000);
  VextON(); displayReset(); display.init(); display.flipScreenVertically();

  preferences.begin("lorawan", true);
  devEUI_str = preferences.getString("devEUI", ""); appEUI_str = preferences.getString("appEUI", ""); appKey_str = preferences.getString("appKey", "");
  g_continuousMode = preferences.getBool("cont_mode", false); g_tempOffset = preferences.getFloat("temp_offset", 0.0);
  g_humOffset = preferences.getFloat("hum_offset", 0.0); g_offlineMode = preferences.getBool("offline_mode", false);
  g_displayEnabled = preferences.getBool("display_en", true); preferences.end();

  I2C_SHT.begin(SHT_SDA_PIN, SHT_SCL_PIN);
  if (!sht4.begin(&I2C_SHT)) g_sensorReadOk = false; else { sht4.setPrecision(SHT4X_HIGH_PRECISION); sht4.setHeater(SHT4X_NO_HEATER); g_sensorReadOk = true; }

  pinMode(BAT_ADC_CTRL, OUTPUT); digitalWrite(BAT_ADC_CTRL, HIGH); delay(50);
  float pinVoltageSetup = analogReadMilliVolts(BAT_ADC) / 1000.0; digitalWrite(BAT_ADC_CTRL, LOW); pinMode(BAT_ADC_CTRL, INPUT);
  if ((pinVoltageSetup * 4.9) >= 4.15) g_isCharging = true; g_lastValidBatt = getBatteryPercentage();

  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP); updateDisplay("PRG p/ BLE");
  unsigned long startTime = millis(); bool enterConfig = false;
  while (millis() - startTime < 3000) { if (digitalRead(CONFIG_BUTTON_PIN) == LOW) { enterConfig = true; break; } delay(50); }
  if (enterConfig) runConfigMode();

  if (g_isCharging || g_continuousMode || g_offlineMode) {
    readSensors(); if(!g_offlineMode) sendLoRaWANData(); updateDisplay("Iniciado...");
  } else {
    readSensors(); sendLoRaWANData();
    if (g_displayEnabled) delay(DISPLAY_ON_TIME_MS);
    display.clear(); display.displayOff(); VextOFF();
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_DURATION_SEC * 1000000); esp_deep_sleep_start();
  }
}

void loop() {
  unsigned long currentTime = millis();
  if (currentTime - g_lastScreenUpdateTime > (CONTINUOUS_SCREEN_INTERVAL_SEC * 1000)) {
    readSensors(); updateDisplay(g_offlineMode ? "Offline" : (g_isCharging ? "Modo Carga" : "Modo Continuo")); g_lastScreenUpdateTime = currentTime;
  }
  if (!g_offlineMode && (currentTime - g_lastLoraPublishTime > (CONTINUOUS_LORA_INTERVAL_SEC * 1000))) {
    readSensors(); sendLoRaWANData(); g_lastLoraPublishTime = currentTime;
  }
}