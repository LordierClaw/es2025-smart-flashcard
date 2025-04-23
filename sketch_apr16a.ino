#include <WiFi.h>
#include <HTTPClient.h>
#include <LCDI2C_Multilingual.h>
#include <ArduinoJson.h>
#include "driver/i2s.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>

#define SERIAL_SPEED 115200

#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2
const bool LCD_IS_SHOW_BACKLIGHT = true;

#define BTN_LEFT_PIN   17
#define BTN_SELECT_PIN 5
#define BTN_RIGHT_PIN  18

#define AUDIO_LRC    27
#define AUDIO_BCLK   14
#define AUDIO_DIN    12
#define AUDIO_VOLUME 21

const unsigned long HOLD_TIME = 1000;

const char* API_BASE        = "http://192.168.0.104:3000";
const char* API_GET_COLL    = "/api/iot/word-groups";
const char* API_GET_WORDS   = "/api/iot/word-groups/";
const char* API_POST_DIFF   = "/api/iot/words/";

#define SERVICE_UUID                 "19b10000-e8f2-537e-4f6c-d104768a1214"
#define SSID_CHARACTERISTIC_UUID     "041675c7-d7e3-4b75-90f9-0c690823f847"
#define PASSWORD_CHARACTERISTIC_UUID "7589f9d3-eb44-423b-ba32-664e40da9ac2"
#define TOKEN_CHARACTERISTIC_UUID    "e6d7c837-879f-4139-8834-ceb5f7e3bafe"

enum State {
  SETUP_WIFI,
  STARTUP,
  COLLECTION_SELECTION,
  WORD_SELECTION,
  TIMEOUT_MENU
};
State currentState = STARTUP;

struct Collection { String id; String name; };
struct Word       { String id; String word; String meaning; String audioUrl; };
std::vector<Collection> collections;
std::vector<Word>       words;
int idxColl = 0;
int idxWord = 0;
int lastIdxColl = -1;
int lastIdxWord = -1;

// Audio output
String currentAudioUrl = "";
URLStream url;
I2SStream out;  // final output of decoded stream
EncodedAudioStream dec(&out, new MP3DecoderHelix()); // Decoding stream
StreamCopy copier(dec, url);

// LCD
typedef LCDI2C_Vietnamese LCD;
LCD lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

struct ButtonStatus {
  bool leftClick;
  bool selectClick;
  bool rightClick;
  bool leftHold;
  bool selectHold;
  bool rightHold;
};
int holdingButton = 0;
bool changeState = false;

Preferences preferences;

BLEServer* pServer = NULL;
BLECharacteristic* pSSIDCharacteristic = NULL;
BLECharacteristic* pPasswordCharacteristic = NULL;
BLECharacteristic* pTokenCharacteristic = NULL;
bool deviceConnected = false;

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Đã kết nối");
    lcd.setCursor(0,1);
    lcd.print("bluetooth");
  };

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Dừng kết nối");
    lcd.setCursor(0,1);
    lcd.print("bluetooth");
  }
};

class MySSIDCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pSSIDCharacteristic) {
    String value = pSSIDCharacteristic->getValue();
    if (value.length() > 0) {
      Serial.print("Received SSID: ");
      Serial.println(value); 
      preferences.putString("ssid", value);
    }
  }
};

class MyPasswordCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pPasswordCharacteristic) {
    String value = pPasswordCharacteristic->getValue();
    if (value.length() > 0) {
      Serial.print("Received Password: ");
      Serial.println(value); 
      preferences.putString("password", value);
    }
  }
};


class MyTokenCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pTokenCharacteristic) {
    String value = pTokenCharacteristic->getValue();
    if (value.length() > 0) {
      Serial.print("Received SSID: ");
      Serial.println(value); 
      preferences.putString("token", value);
    }
  }
};

void setup() {
  Serial.begin(SERIAL_SPEED);
  preferences.begin("smart_flashcard", false);
  // Setup button pin
  pinMode(BTN_LEFT_PIN,   INPUT_PULLUP);
  pinMode(BTN_SELECT_PIN, INPUT_PULLUP);
  pinMode(BTN_RIGHT_PIN,  INPUT_PULLUP);
  // Setup lcd
  lcd.init();
  if (LCD_IS_SHOW_BACKLIGHT) lcd.backlight();
  else lcd.noBacklight();
  // Setup audio output
  installSpeaker();

  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  String token = preferences.getString("token", "");
  if (ssid == "" || password == "" || token == "") {
    currentState = SETUP_WIFI;
    stateSetupWifi();
    return;
  }
}

void installSpeaker() {
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  url.setSSID(ssid.c_str());  // Set WiFi credentials
  url.setPassword(password.c_str());  // Set WiFi credentials
  // setup out
  auto config = out.defaultConfig(TX_MODE);
  config.pin_bck = AUDIO_BCLK;
  config.pin_ws = AUDIO_LRC;
  config.pin_data = AUDIO_DIN;
  out.begin(config);

  // setup out based on sampling rate provided by decoder
  dec.begin();
}

void connectWiFi() {
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  WiFi.begin(ssid.c_str(), password.c_str());
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

void loop() {
  handleState();
}

// --- STATE FUNCTIONS ---

void handleState() {
  switch (currentState) {
    case SETUP_WIFI:           break;
    case STARTUP:              stateStartup(); break;
    case COLLECTION_SELECTION: stateCollection(); break;
    case WORD_SELECTION:       stateWords(); break;
    case TIMEOUT_MENU:         stateTimeout(); break;
  }
}

void stateSetupWifi() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Hãy kết nối");
  lcd.setCursor(0,1);
  lcd.print("với bluetooth");

  // Create the BLE Device
  BLEDevice::init("Smart Flashcard ESP32");

   // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create the BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create the ON button Characteristic
  pSSIDCharacteristic = pService->createCharacteristic(SSID_CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);

  pPasswordCharacteristic = pService->createCharacteristic(PASSWORD_CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);

  pTokenCharacteristic = pService->createCharacteristic(TOKEN_CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);  

  // Register the callback for the ON button characteristic
  pSSIDCharacteristic->setCallbacks(new MySSIDCharacteristicCallbacks());
  pPasswordCharacteristic->setCallbacks(new MyPasswordCharacteristicCallbacks());
  pTokenCharacteristic->setCallbacks(new MyTokenCharacteristicCallbacks());

  // Create a BLE Descriptor
  pSSIDCharacteristic->addDescriptor(new BLE2902());
  pPasswordCharacteristic->addDescriptor(new BLE2902());
  pTokenCharacteristic->addDescriptor(new BLE2902());

  // Start the service
  pService->start();

  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0);  // set value to 0x00 to not advertise this parameter
  BLEDevice::startAdvertising();
  Serial.println("Waiting a client connection to notify...");
}

void stateStartup() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Smart Flashcard");
  lcd.setCursor(0,1); lcd.print("Loading...");
  ButtonStatus btn = checkButtons();
  if (btn.leftHold) {
    preferences.clear(); 
    preferences.end();
    ESP.restart();
  }
  connectWiFi();
  fetchCollections();
  currentState = COLLECTION_SELECTION;
  lastIdxColl = -1;
}

void stateCollection() {
  waitForButtonRelease();
  if (collections.size() == 0) {
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Không có nhóm từ");
    lcd.setCursor(0,1);
    lcd.print("Vui lòng thử lại");
    delay(2000);
    currentState = STARTUP;
    return;
  }
  ButtonStatus btn = checkButtons();
  if (btn.leftClick)  idxColl = (idxColl - 1 + collections.size()) % collections.size();
  if (btn.rightClick) idxColl = (idxColl + 1) % collections.size();
  if (btn.selectClick) {
    idxWord = 0;
    currentState = WORD_SELECTION;
    lastIdxColl = -1;
    lastIdxWord = -1;
    return;
  }
  if (idxColl != lastIdxColl) {
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Chọn nhóm từ:");
    lcd.setCursor(0,1);
    lcd.print("> " + collections[idxColl].name);
    lastIdxColl = idxColl;
    // Preload
    fetchWords(collections[idxColl].id);
  }
  if (btn.leftHold) {
    preferences.clear(); 
    preferences.end();
    ESP.restart();
  }
}

void stateWords() {
  waitForButtonRelease();
  ButtonStatus btn = checkButtons();
  if (btn.leftClick)  idxWord = (idxWord - 1 + words.size()) % words.size();
  if (btn.rightClick) idxWord = (idxWord + 1) % words.size();

  if (idxWord != lastIdxWord) {
    lcd.clear();
    lcd.setCursor(0,0); lcd.print(words[idxWord].word);
    lcd.setCursor(0,1); lcd.print(words[idxWord].meaning);
    lastIdxWord = idxWord;
    currentAudioUrl = words[idxWord].audioUrl;
    preloadAudio(currentAudioUrl);
  }

  if (btn.selectClick) {
    if (currentAudioUrl != "") {
      playAudio();
    } else {
      Serial.println("Không tìm thấy audio");
    }
  }
  if (btn.leftHold) {
    currentState = COLLECTION_SELECTION;
    changeState = true;
  }
  if (btn.rightHold) {
    currentState = TIMEOUT_MENU;
    changeState = true;
  }
}

void stateTimeout() {
  waitForButtonRelease();
  const char* items[] = {"Easy","Medium","Hard"};
  const int items_interval[] = {1, 5, 10};
  int sel = 0;
  int lastSel = -1;
  while (true) {
    ButtonStatus btn = checkButtons();
    if (btn.leftClick)  sel = (sel - 1 + 3) % 3;
    if (btn.rightClick) sel = (sel + 1) % 3;
    if (sel != lastSel) {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Đánh giá độ khó");
      lcd.setCursor(0,1); lcd.print(items[sel]);
      lastSel = sel;
    }
    if (btn.selectClick) {
      postTimeout(words[idxWord].id, items_interval[sel]);
      break;
    }
  }
  currentState = WORD_SELECTION;
  changeState = true;
  lastIdxWord = -1;
}

ButtonStatus checkButtons() {
  ButtonStatus result = {false, false, false, false, false, false};

  struct { int pin; bool &click; bool &hold; } btns[] = {
    { BTN_LEFT_PIN,   result.leftClick,   result.leftHold },
    { BTN_SELECT_PIN, result.selectClick, result.selectHold },
    { BTN_RIGHT_PIN,  result.rightClick,  result.rightHold }
  };

  for (auto &btn : btns) {
    if (digitalRead(btn.pin) == LOW) {
      unsigned long t = millis();
      while (digitalRead(btn.pin) == LOW) {
        if (millis() - t >= HOLD_TIME) {
          btn.hold = true;
          holdingButton = btn.pin;
          break;
        }
      }
      if (!btn.hold) btn.click = true;
    }
  }
  return result;
}

void waitForButtonRelease() {
  if (holdingButton == 0) return;
  if (changeState == false) return;
  while (digitalRead(holdingButton) == LOW) {
    delay(10);
  }
  holdingButton = 0;
  changeState = false;
}

// --- AUDIO OUTPUT FUNCTIONS ---

void preloadAudio(const String& audioUrl) {
  Serial.println("Load file âm thanh: " + audioUrl);
  url.begin(audioUrl.c_str(), "audio/mp3");
}

void playAudio() {
  while (copier.available()) {
    copier.copy();
  }
  preloadAudio(currentAudioUrl);
}

void audio_info(const char *info) {
  Serial.print("audio_info: ");
  Serial.println(info);
}

// --- WEB API RELATED FUNCTIONS ---

void fetchCollections() {
  HTTPClient http; http.begin(String(API_BASE) + API_GET_COLL);
  String token = preferences.getString("token", "");
  http.addHeader("x-device-token", token.c_str());
  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, http.getString());
    collections.clear();
    JsonArray arr = doc.as<JsonArray>();
    for (JsonVariant item : arr) {
      collections.push_back({ item["id"].as<String>(), item["name"].as<String>() });
    }
  }
  http.end();
}

void fetchWords(const String& collId) {
  HTTPClient http; http.begin(String(API_BASE) + API_GET_WORDS + collId + "/words");
  String token = preferences.getString("token", "");
  http.addHeader("x-device-token", token.c_str());
  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, http.getString());
    words.clear();
    JsonArray arr = doc.as<JsonArray>();
    for (JsonVariant item : arr) {
      words.push_back({ item["id"].as<String>(), item["word"].as<String>(), item["meaning"].as<String>(), item["audioUrl"].as<String>() });
    }
  }
  http.end();
}

void postTimeout(const String& wordId, int interval) {
  HTTPClient http; http.begin(String(API_BASE) + API_POST_DIFF + wordId + "/timeout");
  http.addHeader("Content-Type", "application/json");
  String token = preferences.getString("token", "");
  http.addHeader("x-device-token", token.c_str());
  DynamicJsonDocument req(256); 
  req["timeoutMinutes"] = interval;
  String body; serializeJson(req, body);
  http.POST(body);
  http.end();
}
