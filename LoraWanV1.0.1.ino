/*
 * CÓDIGO FINAL LORAWAN - HELTEC V3.2 + SHT45
 * ULTRA LOW POWER: Flujo rápido en Deep Sleep, Underclock 80MHz, Radio Sleep.
 */

#include <Arduino.h>
#include <WiFi.h> // Necesario para apagar el módem WiFi y ahorrar batería
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

#ifndef BAT_ADC
#define BAT_ADC 1
#endif
#ifndef BAT_ADC_CTRL
#define BAT_ADC_CTRL 37
#endif

#define SHT_SDA_PIN 41
#define SHT_SCL_PIN 42
#define CONFIG_BUTTON_PIN 0 // Botón PRG en la placa

#define DISPLAY_ON_TIME_MS 5000
#define DEEP_SLEEP_DURATION_SEC 300 
#define CONTINUOUS_LORA_INTERVAL_SEC 300 
#define CONTINUOUS_SCREEN_INTERVAL_SEC 5

#define RADIO_NSS    8
#define RADIO_DIO1   14
#define RADIO_NRST   12
#define RADIO_BUSY   13

TwoWire I2C_SHT = TwoWire(1);
Adafruit_SHT4x sht4 = Adafruit_SHT4x();
Preferences preferences;
SSD1306Wire display(0x3c, SDA_OLED, SCL_OLED);

SX1262 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_NRST, RADIO_BUSY);
LoRaWANNode node(&radio, &US915, 2); 

String devEUI_str, appEUI_str, appKey_str; 
String temp_devEUI_str = "";
String temp_appEUI_str = "";
String temp_appKey_str = "";

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

uint32_t hexStringToUint32(String hexString) { return strtoul(hexString.c_str(), NULL, 16); }

void hexStringToBytes(String hexString, uint8_t* byteArr, int byteArrLen) {
  for(int i = 0; i < byteArrLen; i++) {
    String byteString = hexString.substring(i*2, i*2+2);
    *(byteArr + i) = (uint8_t) strtol(byteString.c_str(), NULL, 16);
  }
}

void VextON(void) { pinMode(Vext, OUTPUT); digitalWrite(Vext, LOW); delay(50); }
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
  for(int i=0; i < 20; i++) { totalMv += analogReadMilliVolts(BAT_ADC); delay(2); }
  
  digitalWrite(BAT_ADC_CTRL, LOW); 
  pinMode(BAT_ADC_CTRL, INPUT); // Pasa a alta impedancia (Ahorro extra de batería en sleep)

  float battVoltage = ((totalMv / 20.0) / 1000.0) * 4.9; 
  long percentage = map((long)(battVoltage * 100), 320, 420, 0, 100);
  
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
  if (g_offlineMode) display.drawString(70, 52, "Tx PAUSADO");
  display.display();
}

void readSensors() {
  if (!sht4.begin(&I2C_SHT)) { g_sensorReadOk = false; return; }
  // SHT45 necesita solo ~8.3ms. 50ms ahorra casi medio segundo de CPU activa en cada ciclo.
  delay(50); 
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

  radio.setOutputPower(14); // Previene reinicios y reduce pico eléctrico

  uint32_t devAddr = hexStringToUint32(devEUI_str);
  hexStringToBytes(appEUI_str, g_nwkSKey.data(), 16);
  hexStringToBytes(appKey_str, g_appSKey.data(), 16);

  node.clearSession();
  state = node.beginABP(devAddr, g_nwkSKey.data(), g_nwkSKey.data(), g_nwkSKey.data(), g_appSKey.data());
  if (state == RADIOLIB_ERR_NONE) {
      state = node.activateABP();
      if (state == RADIOLIB_ERR_NONE || state == RADIOLIB_LORAWAN_NEW_SESSION || state == RADIOLIB_LORAWAN_SESSION_RESTORED) {
        return true;
      }
  }
  return false;
}

void sendLoRaWANData() {
  if (g_offlineMode || !g_sensorReadOk) return; 

  if (!connectLoRaWAN()) { delay(3000); return; }
  
  // Solo actualiza la pantalla si NO está durmiendo por flujo rápido
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
     updateDisplay("Enviando LPP...");
  }

  int16_t tempLPP = (int16_t)(g_lastValidTemp * 10); 
  uint8_t humLPP = (uint8_t)(g_lastValidHum * 2); 
  int16_t battLPP = (int16_t)(g_lastValidBatt * 100); 

  std::array<uint8_t, 11> txBuffer = {
    1, 103, (uint8_t)(tempLPP >> 8), (uint8_t)(tempLPP & 0xFF), 
    2, 104, humLPP,                                             
    3, 2,   (uint8_t)(battLPP >> 8), (uint8_t)(battLPP & 0xFF)  
  };

  int state = node.sendReceive(txBuffer.data(), 11, 1);
  
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
      if (state == RADIOLIB_ERR_NONE || state == RADIOLIB_ERR_RX_TIMEOUT) { updateDisplay("LPP Enviado OK"); } 
      else { updateDisplay("Error LPP"); }
  }
}

class MyCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    // ... (El bloque de guardado BLE queda idéntico para no alterar tu app) ...
    String stringValue = String(pCharacteristic->getValue().c_str());
    String uuid = String(pCharacteristic->getUUID().toString().c_str());
    preferences.begin("lorawan", false);
    
    if (uuid.equals(CHAR_DEVEUI_UUID)) {
      temp_devEUI_str += stringValue;
      if (temp_devEUI_str.length() >= 8) {
          preferences.putString("devEUI", temp_devEUI_str.substring(0, 8));
          devEUI_str = temp_devEUI_str.substring(0, 8); temp_devEUI_str = ""; updateDisplay("DevAddr OK");
      }
    } else if (uuid.equals(CHAR_APPEUI_UUID)) {
      temp_appEUI_str += stringValue;
      if (temp_appEUI_str.length() >= 32) {
          preferences.putString("appEUI", temp_appEUI_str.substring(0, 32));
          appEUI_str = temp_appEUI_str.substring(0, 32); temp_appEUI_str = ""; updateDisplay("NwkSKey OK");
      }
    } else if (uuid.equals(CHAR_APPKEY_UUID)) {
      temp_appKey_str += stringValue;
      if (temp_appKey_str.length() >= 32) {
          preferences.putString("appKey", temp_appKey_str.substring(0, 32));
          appKey_str = temp_appKey_str.substring(0, 32); temp_appKey_str = ""; updateDisplay("AppSKey OK");
      }
    } else if (uuid.equals(CHAR_CONTINUOUS_MODE_UUID)) {
      preferences.putBool("cont_mode", stringValue.equals("1")); updateDisplay("Modo Cont.");
    } else if (uuid.equals(CHAR_TEMP_OFFSET_UUID)) {
      preferences.putFloat("temp_offset", round(stringValue.toFloat() * 10.0) / 10.0); updateDisplay("Offset T");
    } else if (uuid.equals(CHAR_HUM_OFFSET_UUID)) {
      preferences.putFloat("hum_offset", round(stringValue.toFloat() * 10.0) / 10.0); updateDisplay("Offset H");
    } else if (uuid.equals(CHAR_OFFLINE_MODE_UUID)) {
      g_offlineMode = stringValue.equals("1");
      preferences.putBool("offline_mode", g_offlineMode); updateDisplay("Offline Mode");
    } else if (uuid.equals(CHAR_DISPLAY_EN_UUID)) {
      g_displayEnabled = stringValue.equals("1"); preferences.putBool("display_en", g_displayEnabled);
    } else if (uuid.equals(CHAR_REINICIAR_UUID)) {
      delay(1000); ESP.restart();
    }
    preferences.end();
  }
};

void runConfigMode() {
  g_inConfigMode = true; 
  setCpuFrequencyMhz(240); // Restaurar CPU a 240MHz para que el BLE funcione fluido
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
  pAdvertising->addServiceUUID(SERVICE_UUID); pAdvertising->setScanResponse(true); BLEDevice::startAdvertising();

  while (true) { g_lastValidBatt = getBatteryPercentage(); updateDisplay("App BLE..."); delay(5000); }
}

void setup() {
  // --- OPTIMIZACIONES GLOBALES DE ENERGÍA ---
  setCpuFrequencyMhz(80); // Reducir velocidad de CPU a 80MHz (ahorra ~50% batería activa)
  WiFi.mode(WIFI_OFF);    // Apagar radio WiFi interna por completo
  btStop();               // Apagar hardware Bluetooth por completo

  Serial.begin(115200); delay(100); 

  // Determinar por qué despertó la placa
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool isTimerWakeup = (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER);

  VextON(); // Enciende corriente de sensores (y pantalla, aunque no la usemos a veces)

  preferences.begin("lorawan", true);
  devEUI_str = preferences.getString("devEUI", ""); appEUI_str = preferences.getString("appEUI", ""); appKey_str = preferences.getString("appKey", "");
  g_continuousMode = preferences.getBool("cont_mode", false); g_tempOffset = preferences.getFloat("temp_offset", 0.0);
  g_humOffset = preferences.getFloat("hum_offset", 0.0); g_offlineMode = preferences.getBool("offline_mode", false);
  g_displayEnabled = preferences.getBool("display_en", true); preferences.end();

  I2C_SHT.begin(SHT_SDA_PIN, SHT_SCL_PIN);
  if (!sht4.begin(&I2C_SHT)) { g_sensorReadOk = false; } 
  else { sht4.setPrecision(SHT4X_HIGH_PRECISION); sht4.setHeater(SHT4X_NO_HEATER); g_sensorReadOk = true; }

  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP); 

  // ==============================================================================
  // FLUJO RÁPIDO (FAST PATH): Ocurre si despertó solo por Temporizador (Deep Sleep)
  // ==============================================================================
  if (isTimerWakeup && !g_continuousMode) {
      Serial.println("[LOW POWER] Despertar por Timer. Bypass pantalla activo.");
      
      readSensors(); 
      sendLoRaWANData();
      
      radio.sleep(); // ¡CRÍTICO! Manda a dormir en seco el chip LoRa
      VextOFF();     // Apaga corriente de SHT y Pantalla OLED
      
      // Armar interrupciones y a dormir directo
      esp_sleep_enable_ext0_wakeup((gpio_num_t)CONFIG_BUTTON_PIN, 0); 
      esp_sleep_enable_timer_wakeup(DEEP_SLEEP_DURATION_SEC * 1000000); 
      esp_deep_sleep_start();
      return; // Detiene el Setup() aquí, ahorrando todos los segundos siguientes.
  }

  // ==============================================================================
  // FLUJO NORMAL: Ocurre si despertó por Botón de Reset o Botón PRG (EXT0)
  // ==============================================================================
  Serial.println("[UI] Despertar Manual/Reset. Iniciando Pantalla...");
  
  displayReset(); display.init(); display.flipScreenVertically();
  readSensors(); 

  unsigned long startTime = millis(); 
  bool btnPressed = false;
  unsigned long pressStart = 0;
  int action = 0; 
  int scrollX = 128; 
  String scrollMsg = "Btn(3s): Corto->App BLE | Largo->Pausa Tx";
  display.setFont(ArialMT_Plain_10);
  int textWidth = display.getStringWidth(scrollMsg);

  // Pantalla interactiva por 4 segundos
  while (millis() - startTime < 4000) { 
    if (digitalRead(CONFIG_BUTTON_PIN) == LOW) { 
      if (!btnPressed) { btnPressed = true; pressStart = millis(); }
    } else {
      if (btnPressed) { 
        unsigned long dur = millis() - pressStart;
        if (dur > 1500) action = 2;       
        else if (dur > 50) action = 1;    
        break; 
      }
    }

    display.clear();
    
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(scrollX, 0, scrollMsg);

    if (g_sensorReadOk) {
      display.setFont(ArialMT_Plain_16); 
      display.drawString(0, 16, "T: " + String(g_lastValidTemp, 1) + " C");
      display.drawString(0, 32, "H: " + String(g_lastValidHum, 1) + " %");
    } else {
      display.setFont(ArialMT_Plain_16); 
      display.drawString(0, 16, "T: --.- C"); display.drawString(0, 32, "H: --.- %");
    }

    drawBatteryBar(0, 52, g_lastValidBatt); 

    int timeLeft = map(millis() - startTime, 0, 4000, 128, 0);
    display.fillRect(0, 63, timeLeft, 1);

    display.display();

    scrollX -= 2; 
    if (scrollX < -textWidth) { scrollX = 128; } 
    delay(20); 
  }
  
  if (btnPressed && digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    if (millis() - pressStart > 1500) action = 2;
  }

  // Acciones del botón
  if (action == 2) {
    g_offlineMode = !g_offlineMode;
    preferences.begin("lorawan", false);
    preferences.putBool("offline_mode", g_offlineMode);
    preferences.end();
    updateDisplay(g_offlineMode ? "Tx PAUSADO" : "Tx REANUDADO");
    delay(2000);
  } else if (action == 1) {
    runConfigMode();
  }

  // Flujo post-botón
  if (g_continuousMode) {
    readSensors(); 
    sendLoRaWANData(); 
    updateDisplay("Iniciado...");
  } else {
    readSensors(); 
    sendLoRaWANData();
    if (g_displayEnabled) delay(DISPLAY_ON_TIME_MS);
    
    display.clear(); display.displayOff(); VextOFF();
    radio.sleep(); // Manda a dormir chip LoRa
    
    esp_sleep_enable_ext0_wakeup((gpio_num_t)CONFIG_BUTTON_PIN, 0); 
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_DURATION_SEC * 1000000); 
    esp_deep_sleep_start();
  }
}

void loop() {
  unsigned long currentTime = millis();

  // Detección de Botón en tiempo real (Solo Modo Continuo)
  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    unsigned long pressStart = millis();
    while(digitalRead(CONFIG_BUTTON_PIN) == LOW) { delay(50); } 
    if (millis() - pressStart > 1500) {
      g_offlineMode = !g_offlineMode;
      preferences.begin("lorawan", false);
      preferences.putBool("offline_mode", g_offlineMode);
      preferences.end();
      updateDisplay(g_offlineMode ? "Tx PAUSADO" : "Tx REANUDADO");
      delay(2000);
      g_lastScreenUpdateTime = millis(); 
    }
  }
  
  if (currentTime - g_lastScreenUpdateTime > (CONTINUOUS_SCREEN_INTERVAL_SEC * 1000)) {
    readSensors(); 
    updateDisplay(g_offlineMode ? "Tx PAUSADO" : "Modo Continuo"); 
    g_lastScreenUpdateTime = currentTime;

    if (!g_continuousMode) {
        display.clear(); display.displayOff(); VextOFF();
        radio.sleep(); 
        
        esp_sleep_enable_ext0_wakeup((gpio_num_t)CONFIG_BUTTON_PIN, 0);
        esp_sleep_enable_timer_wakeup(DEEP_SLEEP_DURATION_SEC * 1000000);
        esp_deep_sleep_start();
    }
  }

  if (!g_offlineMode && (currentTime - g_lastLoraPublishTime > (CONTINUOUS_LORA_INTERVAL_SEC * 1000))) {
    readSensors(); 
    sendLoRaWANData(); 
    g_lastLoraPublishTime = currentTime;
  }
}