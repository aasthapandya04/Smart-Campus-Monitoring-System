/*
  Smart Campus Monitoring System (Team Mavericks) - SLAVE ESP32
  --------------------------------------------
  Measures distance with an ultrasonic sensor and sends it to the master by ESP-NOW.
  Blinks its LED when the master asks (button press on the master).
  WIFI_NAME / WIFI_PASS must be the SAME as in the master, so both boards use the same channel.
*/

#include <WiFi.h>
#include <esp_now.h>

const char* WIFI_NAME = "XAMPP_SERVER";
const char* WIFI_PASS = "123456789";

#define TRIG_PIN 5
#define ECHO_PIN 18      
#define LED_PIN  4      

#define LED_ON   HIGH
#define LED_OFF  LOW

const float MAX_DISTANCE_CM = 30.0;           // readings are locked to 30 cm at most
const unsigned long SEND_EVERY     = 500;     // send distance every 0.5 s
const unsigned long BLINK_EVERY    = 300;     // LED blink speed
const unsigned long MASTER_TIMEOUT = 3000;    // LED turns off if the master goes silent

#define NOW_MAGIC 0xA7

typedef struct { uint8_t magic; float distance; } SlavePacket;   
typedef struct { uint8_t magic; uint8_t blink; }  MasterPacket; 

const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

float distanceCm = MAX_DISTANCE_CM;
bool hasReading = false;

volatile bool blinkRequested = false;
volatile unsigned long masterLastSeen = 0;

unsigned long tSend = 0, tBlink = 0;
bool blinkPhase = false;

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
void onReceive(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len != (int)sizeof(MasterPacket)) return;

  MasterPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.magic != NOW_MAGIC) return;          

  blinkRequested = (pkt.blink == 1);
  masterLastSeen = millis();
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

  if (!hasReading) {
    distanceCm = d;
    hasReading = true;
  } else {
    distanceCm = 0.6 * d + 0.4 * distanceCm;
  }
}


void sendDistance() {
  SlavePacket pkt;
  pkt.magic = NOW_MAGIC;
  pkt.distance = distanceCm;
  esp_now_send(BROADCAST_ADDR, (uint8_t *)&pkt, sizeof(pkt));
}

void updateLED() {
  bool masterOnline = (masterLastSeen != 0 && millis() - masterLastSeen < MASTER_TIMEOUT);

  if (masterOnline && blinkRequested) {
    if (millis() - tBlink >= BLINK_EVERY) {
      tBlink = millis();
      blinkPhase = !blinkPhase;
    }
    digitalWrite(LED_PIN, blinkPhase ? LED_ON : LED_OFF);
  } else {
    blinkPhase = false;
    digitalWrite(LED_PIN, LED_OFF);
  }
}

void keepWiFiAlive() {
  static unsigned long lastTry = 0;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastTry < 10000) return;
  lastTry = millis();
  WiFi.reconnect();
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(LED_PIN, LED_OFF);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_NAME, WIFI_PASS);

  Serial.print("Joining WiFi for the channel");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Slave MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("WiFi channel: ");
  Serial.println(WiFi.channel());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW could not start");
    return;
  }
  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = 0;                          
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.println("Slave ready.");
}

void loop() {
  keepWiFiAlive();

  if (millis() - tSend >= SEND_EVERY) {
    tSend = millis();
    readDistance();
    sendDistance();
    Serial.printf("Distance %.1f cm | Blink asked by master: %d\n",
                  distanceCm, blinkRequested);
  }

  updateLED();
}
