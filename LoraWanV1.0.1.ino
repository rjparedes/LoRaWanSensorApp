/*
 * CÓDIGO FINAL LORAWAN - HELTEC V3.2 + SHT45
 * Librería LoRa: RadioLib
 * (VERSIÓN DEFINITIVA: Fix UPLINK_MIC para LoRaWAN 1.0.3 + std::array)
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

#define DISPLAY_ON_TIME_MS 10000
#define DEEP_SLEEP_DURATION_SEC 900 
#define CONTINUOUS_LORA_INTERVAL_SEC 300 
#define CONTINUOUS_SCREEN_INTERVAL_SEC 10

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

// --- CONFIGURACIÓN DE RADIOLIB ---
SX1262 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_NRST, RADIO_BUSY);
LoRaWANNode node(&radio, &US915, 2); 

// --- VARIABLES DE SESIÓN RTC (Persisten durante el Deep Sleep) ---
// Usamos std::array para evitar corchetes y mantener la memoria segura
RTC_DATA_ATTR std::array<uint8_t, 16> rtc_app_key;
RTC_DATA_ATTR bool rtc_keys_valid = false;
RTC_DATA_ATTR bool g_isJoined = false; 

// --- VARIABLES GLOBALES DE TRABAJO ---
String devEUI_str, appEUI_str, appKey_str;
bool g_isCharging = false;
bool g_continuousMode = false;
float g_tempOffset = 0.0;
float g_humOffset = 0.0;
bool g_offlineMode = false;
bool g_displayEnabled = true; 
bool g_inConfigMode = false;

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
uint64_t hexStringToUint64(String hexString) {
  return strtoull(hexString.c_str(), NULL, 16);
}

void hexStringToBytes(String hexString, uint8_t* byteArr, int byteArrLen) {
  for(int i = 0; i < byteArrLen; i++) {
    String byteString = hexString.substring(i*2, i*2+2);
    *(byteArr + i) = (uint8_t) strtol(byteString.c_str(), NULL, 16);
  }
}

// =================================================================
//                 FUNCIONES DE HARDWARE Y PANTALLA
// =================================================================
void VextON(void) { pinMode(Vext, OUTPUT); digitalWrite(Vext, LOW); }
void VextOFF(void) { pinMode(Vext, OUTPUT); digitalWrite(Vext, HIGH); }

void displayReset(void) {
  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, HIGH); delay(1);
  digitalWrite(RST_OLED, LOW); delay(1);
  digitalWrite(RST_OLED, HIGH); delay(1);
}

float getBatteryPercentage() {
  pinMode(BAT_ADC_CTRL, OUTPUT);
  digitalWrite(BAT_ADC_CTRL, HIGH); 
  delay(50); 

  float totalMv = 0;
  int numReadings = 20;
  for(int i=0; i < numReadings; i++) { 
    totalMv += analogReadMilliVolts(BAT_ADC); 
    delay(2); 
  }
  
  digitalWrite(BAT_ADC_CTRL, LOW);
  pinMode(BAT_ADC_CTRL, INPUT);

  float avgMv = totalMv / (float)numReadings;
  float pinVoltage = avgMv / 1000.0;
  float battVoltage = pinVoltage * 4.9; 
  
  Serial.printf("Milivoltios leidos: %.1f | Voltaje Real de Bateria: %.2fV\n", avgMv, battVoltage);

  long percentage = map((long)(battVoltage * 100), 340, 420, 0, 100);
  if (percentage > 100) percentage = 100; 
  else if (percentage < 0) percentage = 0;
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
  if (!g_displayEnabled && !g_inConfigMode) {
    display.displayOff();
    return;
  }
  
  display.displayOn();
  display.clear();
  
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 0, statusLine1);

  if (g_sensorReadOk) {
    display.setFont(ArialMT_Plain_16); 
    display.drawString(0, 16, "T: " + String(g_lastValidTemp, 1) + " C");
    display.drawString(0, 32, "H: " + String(g_lastValidHum, 1) + " %");
  } else {
    display.setFont(ArialMT_Plain_16); 
    display.drawString(0, 16, "T: --.- C");
    display.drawString(0, 32, "H: --.- %");
  }

  drawBatteryBar(0, 52, g_lastValidBatt); 
  display.setFont(ArialMT_Plain_10);
  if (g_isCharging) display.drawString(70, 52, "Cargando");
  else if (g_offlineMode) display.drawString(70, 52, "Offline");
  
  display.display();
}

// =================================================================
//                 LECTURA DEL SENSOR SHT45
// =================================================================
void readSensors() {
  sensors_event_t humidity, temp;
  sht4.getEvent(&humidity, &temp);

  float currentTemp = temp.temperature + g_tempOffset;
  float currentHum = humidity.relative_humidity + g_humOffset;

  if (currentHum > 100.0) currentHum = 100.0;
  if (currentHum < 0.0) currentHum = 0.0;

  g_lastValidBatt = getBatteryPercentage();

  if (isnan(currentTemp) || isnan(currentHum)) {
    g_sensorReadOk = false;
    Serial.println("Error leyendo SHT45");
  } else {
    g_lastValidTemp = currentTemp;
    g_lastValidHum = currentHum;
    g_sensorReadOk = true;
  }
}

// =================================================================
//                 CONEXIÓN Y TRANSMISIÓN LORAWAN
// =================================================================
bool connectLoRaWAN() {
  if (g_isJoined) return true; 

  if (devEUI_str.length() != 16 || appEUI_str.length() != 16 || appKey_str.length() != 32) {
    Serial.println("Error: Credenciales LoRaWAN incompletas o invalidas.");
    updateDisplay("Faltan Creds");
    return false;
  }

  int state = radio.begin();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Fallo en radio.begin(), codigo: %d\n", state);
    updateDisplay("Error Radio");
    return false;
  }

  updateDisplay("Conectando...");
  Serial.println("Configurando llave unica OTAA para protocolo 1.0.3...");

  uint64_t devEUI = hexStringToUint64(devEUI_str);
  uint64_t joinEUI = hexStringToUint64(appEUI_str);
  
  if (!rtc_keys_valid) {
      hexStringToBytes(appKey_str, rtc_app_key.data(), 16);
      rtc_keys_valid = true;
  }

  // SOLUCION DEFINITIVA: 
  // Tercer parámetro: AppKey real. Cuarto parámetro: NULL. Esto obliga a versión 1.0.3
  state = node.beginOTAA(joinEUI, devEUI, rtc_app_key.data(), NULL);

  if (state == RADIOLIB_ERR_NONE) {
      Serial.println("Configuracion OK. Lanzando Join Request por antena...");
      
      state = node.activateOTAA();

      if (state == RADIOLIB_ERR_NONE) {
        Serial.println("¡Join Exitoso! Red conectada.");
        g_isJoined = true;
        return true;
      } else {
        Serial.printf("Error en Join (Timeout o Rechazo), codigo: %d\n", state);
        updateDisplay("Error Join");
        return false;
      }
  } else {
    Serial.printf("Error al configurar OTAA, codigo: %d\n", state);
    updateDisplay("Error Conf");
    return false;
  }
}

void sendLoRaWANData() {
  if (g_offlineMode) return;
  if (!g_sensorReadOk) return;

  if (!connectLoRaWAN()) {
    delay(3000);
    return;
  }

  updateDisplay("Enviando...");
  Serial.println("Enviando datos de sensores...");

  int16_t tempPayload = (int16_t)(g_lastValidTemp * 10); 
  uint16_t humPayload = (uint16_t)(g_lastValidHum * 10);
  uint8_t currentBatt = (uint8_t)g_lastValidBatt;
  
  // Usamos std::array para evitar usar corchetes y mantener la memoria segura
  std::array<uint8_t, 5> txBuffer;
  uint8_t* ptr = txBuffer.data();
  *(ptr + 0) = tempPayload >> 8;
  *(ptr + 1) = tempPayload & 0xFF;
  *(ptr + 2) = humPayload >> 8;
  *(ptr + 3) = humPayload & 0xFF;
  *(ptr + 4) = currentBatt;

  int state = node.sendReceive(txBuffer.data(), 5, 1);

  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("¡Transmision exitosa!");
    updateDisplay("Enviado OK");
  } else {
    Serial.printf("Error al Enviar, codigo: %d\n", state);
    updateDisplay("Error Enviar");
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
      preferences.putString("devEUI", stringValue);
      devEUI_str = stringValue;
      updateDisplay("DevEUI Guardado");
    } else if (uuid.equals(CHAR_APPEUI_UUID)) {
      preferences.putString("appEUI", stringValue);
      appEUI_str = stringValue;
      updateDisplay("AppEUI Guardado");
    } else if (uuid.equals(CHAR_APPKEY_UUID)) {
      preferences.putString("appKey", stringValue);
      appKey_str = stringValue;
      rtc_keys_valid = false; // Forzar regeneracion al cambiar la llave
      updateDisplay("AppKey Guardado");
    } else if (uuid.equals(CHAR_CONTINUOUS_MODE_UUID)) {
      preferences.putBool("cont_mode", stringValue.equals("1"));
      updateDisplay("Modo Cont. Cambiado");
    } else if (uuid.equals(CHAR_TEMP_OFFSET_UUID)) {
      float offset = round(stringValue.toFloat() * 10.0) / 10.0;
      preferences.putFloat("temp_offset", offset);
      updateDisplay("Offset T Guardado");
    } else if (uuid.equals(CHAR_HUM_OFFSET_UUID)) {
      float offset = round(stringValue.toFloat() * 10.0) / 10.0;
      preferences.putFloat("hum_offset", offset);
      updateDisplay("Offset H Guardado");
    } else if (uuid.equals(CHAR_OFFLINE_MODE_UUID)) {
      preferences.putBool("offline_mode", stringValue.equals("1"));
      updateDisplay("Modo Offline Cambiado");
    } else if (uuid.equals(CHAR_DISPLAY_EN_UUID)) {
      g_displayEnabled = stringValue.equals("1");
      preferences.putBool("display_en", g_displayEnabled);
      updateDisplay("Pantalla: " + String(g_displayEnabled ? "ON" : "OFF"));
    } else if (uuid.equals(CHAR_REINICIAR_UUID)) {
      updateDisplay("Reiniciando...");
      delay(1000); ESP.restart();
    }
    preferences.end();
  }
};

void runConfigMode() {
  Serial.println("MODO BLE ACTIVADO");
  
  g_inConfigMode = true; 
  updateDisplay("MODO CONFIG BLE");

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
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  while (true) {
    g_lastValidBatt = getBatteryPercentage();
    updateDisplay("Esperando App BLE...");
    delay(5000); 
  }
}

// =================================================================
//                 SETUP Y FLUJO PRINCIPAL
// =================================================================
void setup() {
  Serial.begin(115200); delay(1000);
  
  VextON();
  displayReset();
  display.init(); 
  display.flipScreenVertically();

  preferences.begin("lorawan", true);
  devEUI_str = preferences.getString("devEUI", "");
  appEUI_str = preferences.getString("appEUI", "");
  appKey_str = preferences.getString("appKey", "");
  g_continuousMode = preferences.getBool("cont_mode", false);
  g_tempOffset = preferences.getFloat("temp_offset", 0.0);
  g_humOffset = preferences.getFloat("hum_offset", 0.0);
  g_offlineMode = preferences.getBool("offline_mode", false);
  g_displayEnabled = preferences.getBool("display_en", true);
  preferences.end();

  I2C_SHT.begin(SHT_SDA_PIN, SHT_SCL_PIN);
  if (!sht4.begin(&I2C_SHT)) {
    Serial.println("SHT45 NO ENCONTRADO");
    g_sensorReadOk = false;
  } else {
    Serial.println("SHT45 INICIALIZADO CORRECTAMENTE");
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sht4.setHeater(SHT4X_NO_HEATER);
    g_sensorReadOk = true;
  }

  pinMode(BAT_ADC_CTRL, OUTPUT);
  digitalWrite(BAT_ADC_CTRL, HIGH);
  delay(50);
  float pinVoltageSetup = analogReadMilliVolts(BAT_ADC) / 1000.0;
  digitalWrite(BAT_ADC_CTRL, LOW);
  pinMode(BAT_ADC_CTRL, INPUT);
  
  float battVoltageSetup = pinVoltageSetup * 4.9;
  if (battVoltageSetup >= 4.15) g_isCharging = true;

  g_lastValidBatt = getBatteryPercentage();

  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  updateDisplay("Presione PRG p/ BLE");
  unsigned long startTime = millis();
  bool enterConfig = false;
  while (millis() - startTime < 3000) {
    if (digitalRead(CONFIG_BUTTON_PIN) == LOW) { enterConfig = true; break; }
    delay(50);
  }

  if (enterConfig) runConfigMode();

  if (g_isCharging || g_continuousMode || g_offlineMode) {
    Serial.println("Entrando a Modo Continuo/Carga/Offline");
    readSensors();
    if(!g_offlineMode) sendLoRaWANData();
    updateDisplay("Iniciado...");
  } else {
    Serial.println("Modo Bateria: Leyendo, enviando y durmiendo.");
    readSensors();
    sendLoRaWANData();
    
    if (g_displayEnabled) {
      delay(DISPLAY_ON_TIME_MS);
    }
    
    display.clear(); display.displayOff(); VextOFF();
    
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_DURATION_SEC * 1000000);
    esp_deep_sleep_start();
  }
}

void loop() {
  unsigned long currentTime = millis();

  if (currentTime - g_lastScreenUpdateTime > (CONTINUOUS_SCREEN_INTERVAL_SEC * 1000)) {
    readSensors();
    String title = g_offlineMode ? "Offline" : (g_isCharging ? "Modo Carga" : "Modo Continuo");
    updateDisplay(title);
    g_lastScreenUpdateTime = currentTime;
  }

  if (!g_offlineMode && (currentTime - g_lastLoraPublishTime > (CONTINUOUS_LORA_INTERVAL_SEC * 1000))) {
    readSensors();
    sendLoRaWANData();
    g_lastLoraPublishTime = currentTime;
  }
}