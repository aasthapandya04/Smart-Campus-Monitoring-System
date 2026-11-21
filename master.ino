/*
  Smart Campus Monitoring System (Team Mavericks) - MASTER ESP32
  ---------------------------------------------
  Reads DHT22 (temperature/humidity) + ultrasonic (water level), shows them on a 16x2 LCD,
  receives the slave's distance by ESP-NOW and uploads everything to the XAMPP dashboard
  (server/index.php) every 5 seconds.

  Change these before uploading:  WIFI_NAME, WIFI_PASS, SERVER_IP, TEAM, API_KEY
  Libraries (Library Manager): DHT sensor library (Adafruit), Adafruit Unified Sensor, LiquidCrystal I2C
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <esp_now.h>

const char* WIFI_NAME = "XAMPP_SERVER";
const char* WIFI_PASS = "123456789";
const char* API_KEY   = "Campus123";

const char* SERVER_IP = "192.168.137.1";   
const char* TEAM      = "team13";            

#define DHT_PIN    19
#define DHT_TYPE   DHT22
#define TRIG_PIN   5
#define ECHO_PIN   18   
#define GREEN_LED  25
#define RED_LED    26
#define BUZZER     27
#define BUTTON     4

#define BUZZER_ON  HIGH
#define BUZZER_OFF LOW
#define LED_ON     HIGH
#define LED_OFF    LOW

const float MAX_DISTANCE_CM = 30.0;  
const float ALERT_ON_CM     = 8.0;   // closer than this -> buzzer + red LED
const float ALERT_OFF_CM    = 10.0;  

const unsigned long DHT_EVERY    = 2500;
const unsigned long WATER_EVERY  = 500;
const unsigned long LCD_EVERY    = 500;
const unsigned long UPLOAD_EVERY = 5000;
const unsigned long LED_EVERY    = 2000;
const unsigned long RETRY_AFTER  = 10000;   

const int MODE_AUTO  = 0; 
const int MODE_ON    = 1;
const int MODE_BLINK = 2;
const int MODE_OFF   = 3;

#define NOW_MAGIC 0xA7                          
const unsigned long SLAVE_TIMEOUT    = 3000; 
const unsigned long SLAVE_TALK_EVERY = 500;

typedef struct { uint8_t magic; float distance; } SlavePacket;   
typedef struct { uint8_t magic; uint8_t blink; }  MasterPacket;  

const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

volatile float slaveDistance = 0;
volatile unsigned long slaveLastSeen = 0;
bool slaveBlink = false;                  
unsigned long tSlave = 0;

DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

float temperature = 0, humidity = 0;
float distanceCm = MAX_DISTANCE_CM;
float waterLevel = 0, waterPercent = 0;
bool dhtOK = false;
bool hasWater = false;
unsigned long lastDhtGood = 0;

bool alertActive = false;
bool muted = false;

int greenMode = MODE_AUTO;
int redMode   = MODE_AUTO;
bool redNeedsSync   = true; 
bool greenNeedsSync = true;

bool blinkPhase = false;
unsigned long tBlink = 0;

unsigned long tDht = 0, tWater = 0, tLcd = 0, tUpload = 0, tLed = 0;
unsigned long serverRetryAt = 0;

String baseURL;

volatile bool buttonPressed = false;
volatile unsigned long lastPressTime = 0;

void IRAM_ATTR onButton() {
  unsigned long now = millis();
  if (now - lastPressTime > 250) {  
    buttonPressed = true;
    lastPressTime = now;
  }
}

void printRow(int row, String text) {
  if (text.length() > 16) text = text.substring(0, 16);
  while (text.length() < 16) text += " ";
  lcd.setCursor(0, row);
  lcd.print(text);
}

bool serverAvailable() {
  if (WiFi.status() != WL_CONNECTED) return false;
  return (long)(millis() - serverRetryAt) >= 0;
}

void serverFailed() {
  serverRetryAt = millis() + RETRY_AFTER;
  Serial.println("Server not reachable. Will try again soon.");
}

void keepWiFiAlive() {
  static unsigned long lastTry = 0;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastTry < 10000) return;
  lastTry = millis();
  Serial.println("WiFi lost. Reconnecting...");
  WiFi.reconnect();
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
void onReceive(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len != (int)sizeof(SlavePacket)) return;

  SlavePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.magic != NOW_MAGIC) return;          

  slaveDistance = pkt.distance;
  slaveLastSeen = millis();
}

bool slaveOnline() {
  return slaveLastSeen != 0 && millis() - slaveLastSeen < SLAVE_TIMEOUT;
}

void sendToSlave() {
  MasterPacket pkt;
  pkt.magic = NOW_MAGIC;
  pkt.blink = slaveBlink ? 1 : 0;
  esp_now_send(BROADCAST_ADDR, (uint8_t *)&pkt, sizeof(pkt));
}

void setupEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW could not start");
    return;
  }
  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = 0;                            // 0 = use the current WiFi channel
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.print("ESP-NOW ready. Master MAC: ");
  Serial.println(WiFi.macAddress());
}

void readDHT() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t) && !isnan(h)) {
    temperature = t;
    humidity = h;
    lastDhtGood = millis();
  }

  dhtOK = (lastDhtGood != 0 && millis() - lastDhtGood < 15000);
}

//Distance 

float measureDistanceCm() {
  float samples[5];

  for (int i = 0; i < 5; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long echo = pulseIn(ECHO_PIN, HIGH, 6000);  

    float cm = MAX_DISTANCE_CM;           
    if (echo > 0) {
      cm = echo * 0.0343 / 2.0;
      cm = constrain(cm, 2.0, MAX_DISTANCE_CM);
    }
    samples[i] = cm;
    delay(15);
  }

  for (int i = 0; i < 4; i++) {            
    for (int j = i + 1; j < 5; j++) {
      if (samples[j] < samples[i]) {
        float tmp = samples[i];
        samples[i] = samples[j];
        samples[j] = tmp;
      }
    }
  }
  return samples[2];                            
}

void readDistance() {
  float d = measureDistanceCm();

  if (!hasWater) {
    distanceCm = d;            
    hasWater = true;
  } else {
    distanceCm = 0.6 * d + 0.4 * distanceCm;
  }

  waterLevel   = MAX_DISTANCE_CM - distanceCm;   // for the dashboard
  waterPercent = waterLevel / MAX_DISTANCE_CM * 100.0;
}

void updateAlert() {
  bool before = alertActive;

  if (!alertActive && distanceCm < ALERT_ON_CM)   alertActive = true;
  if (alertActive  && distanceCm >= ALERT_OFF_CM) alertActive = false;

  if (alertActive && !before) muted = false;
  if (!alertActive) muted = false;
}

void handleButton() {
  if (!buttonPressed) return;
  buttonPressed = false;

  slaveBlink = !slaveBlink;
  sendToSlave();
  Serial.println(slaveBlink ? "Slave LED: blink" : "Slave LED: off");

  if (alertActive) {
    muted = !muted;
    Serial.println(muted ? "Buzzer muted" : "Buzzer on again");
  } else {
    bool redIsOn = (redMode == MODE_ON || redMode == MODE_BLINK);
    redMode = redIsOn ? MODE_OFF : MODE_ON;
    redNeedsSync = true;
    Serial.println(redMode == MODE_ON ? "Red LED on (button)" : "Red LED off (button)");
  }
}

bool modeLevel(int mode, bool autoValue) {
  if (mode == MODE_ON)    return true;
  if (mode == MODE_OFF)   return false;
  if (mode == MODE_BLINK) return blinkPhase;
  return autoValue;
}

void updateOutputs() {
  if (millis() - tBlink >= 500) {
    tBlink = millis();
    blinkPhase = !blinkPhase;
  }

  bool green, red;

  if (alertActive) {
    red = true;
    green = false;
  } else {
    green = modeLevel(greenMode, true);    
    red   = modeLevel(redMode, false);     
  }

  digitalWrite(GREEN_LED, green ? LED_ON : LED_OFF);
  digitalWrite(RED_LED,   red   ? LED_ON : LED_OFF);
  digitalWrite(BUZZER, (alertActive && !muted) ? BUZZER_ON : BUZZER_OFF);
}

void updateLCD() {
  if (dhtOK) {
    printRow(0, "T:" + String(temperature, 1) + "C H:" + String(humidity, 0) + "%");
  } else {
    printRow(0, "T:--  H:--");
  }

  float sd = slaveDistance;
  String row = "M:" + String(distanceCm, 1) + "cm S:";
  row += slaveOnline() ? String(sd, 1) : String("--");
  if (alertActive) {
    while (row.length() < 15) row += " ";
    row += "!";
  }
  printRow(1, row);
}

void uploadData() {
  if (!serverAvailable()) return;
  if (!hasWater) return;                       // wait for the first distance reading

  String json = "{";
  if (dhtOK) {                                 // send temp/humidity only when the DHT22 is working
    json += "\"temperature\":"    + String(temperature, 1)  + ",";
    json += "\"humidity\":"       + String(humidity, 1)     + ",";
  }
  json += "\"distance\":"         + String(distanceCm, 1)   + ",";
  json += "\"water_level\":"      + String(waterLevel, 1)   + ",";
  json += "\"water_percentage\":" + String(waterPercent, 1) + ",";
  json += "\"alert_active\":"     + String(alertActive ? 1 : 0) + ",";
  json += "\"buzzer_muted\":"     + String(muted ? 1 : 0);
  if (slaveOnline()) {
    json += ",\"slave_distance\":" + String((float)slaveDistance, 1);
  }
  json += "}";

  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  http.begin(baseURL + "index.php?api=insert");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);

  int code = http.POST(json);
  Serial.print("Sent to dashboard, response: ");
  Serial.println(code);

  if (code <= 0) serverFailed();
  http.end();
}

int readNumber(String json, String key) {
  int pos = json.indexOf("\"" + key + "\":");
  if (pos < 0) return -1;
  pos += key.length() + 3;
  while (pos < (int)json.length() && json[pos] == ' ') pos++;
  return json.substring(pos).toInt();
}

bool pushLed(const char* led, int state) {
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  http.begin(baseURL + "index.php?api=setled&led=" + led + "&state=" + String(state));
  http.addHeader("X-API-Key", API_KEY);
  int code = http.GET();
  http.end();
  if (code <= 0) serverFailed();
  return code == 200;
}

void syncLeds() {
  if (!serverAvailable()) return;

  if (redNeedsSync) {
    if (pushLed("red", redMode)) redNeedsSync = false;
    return;
  }
  if (greenNeedsSync) {
    if (pushLed("green", greenMode)) greenNeedsSync = false;
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  http.begin(baseURL + "index.php?api=leds");
  int code = http.GET();

  if (code == 200) {
    String reply = http.getString();
    int g = readNumber(reply, "green_led");
    int r = readNumber(reply, "red_led");
    if (g >= 0 && g <= 3 && r >= 0 && r <= 3) {
      greenMode = g;
      redMode   = r;
    }
  } else if (code <= 0) {
    serverFailed();
  }
  http.end();
}

void startupTest() {
  printRow(0, "LED test");

  printRow(1, "RED LED...");
  digitalWrite(RED_LED, LED_ON);
  delay(800);
  digitalWrite(RED_LED, LED_OFF);

  printRow(1, "GREEN LED...");
  digitalWrite(GREEN_LED, LED_ON);
  delay(800);
  digitalWrite(GREEN_LED, LED_OFF);

  printRow(1, "BUZZER...");
  digitalWrite(BUZZER, BUZZER_ON);
  delay(150);
  digitalWrite(BUZZER, BUZZER_OFF);
  delay(300);
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);

  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(RED_LED, LED_OFF);
  digitalWrite(GREEN_LED, LED_OFF);
  digitalWrite(BUZZER, BUZZER_OFF);

  attachInterrupt(digitalPinToInterrupt(BUTTON), onButton, FALLING);

  baseURL = String("http://") + SERVER_IP + "/student-sites/" + TEAM + "/";

  dht.begin();
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  printRow(0, "SMART CAMPUS");
  printRow(1, "Starting...");
  delay(1000);

  startupTest();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);           
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_NAME, WIFI_PASS);
  setupEspNow();

  lcd.clear();

  tDht    = millis() - DHT_EVERY;
  tWater  = millis() - WATER_EVERY;
  tLcd    = millis() - LCD_EVERY;
  tLed    = millis() - LED_EVERY;
  tUpload = millis();

  Serial.println("Smart Campus started.");
}

void loop() {
  keepWiFiAlive();
  handleButton();

  if (millis() - tDht >= DHT_EVERY) {
    tDht = millis();
    readDHT();
  }

  if (millis() - tWater >= WATER_EVERY) {
    tWater = millis();
    readDistance();
    updateAlert();
    Serial.printf("Temp %.1f C | Hum %.0f %% | Distance %.1f cm | Alert %d\n",
                  temperature, humidity, distanceCm, alertActive);
  }

  updateOutputs();

  if (millis() - tLcd >= LCD_EVERY) {
    tLcd = millis();
    updateLCD();
  }

  if (millis() - tLed >= LED_EVERY) {
    tLed = millis();
    syncLeds();
  }

  if (millis() - tSlave >= SLAVE_TALK_EVERY) {
    tSlave = millis();
    sendToSlave();
  }

  if (millis() - tUpload >= UPLOAD_EVERY) {
    tUpload = millis();
    uploadData();
  }
}
