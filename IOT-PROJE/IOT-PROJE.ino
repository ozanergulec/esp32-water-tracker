#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HX711_ADC.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Adafruit_MQTT.h>
#include <Adafruit_MQTT_Client.h>

// ============ WIFI & ADAFRUIT IO ============
#define WIFI_SSID       "Ozan"           // <<< DEGISTIR
#define WIFI_PASSWORD   "12345678"         // <<< DEGISTIR
#define AIO_USERNAME    "anbo511"  // <<< DEGISTIR
#define AIO_KEY         "YOUR_ADAFRUIT_IO_KEY"     // <<< DEGISTIR
#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  8883  // SSL portu

// ============ PIN TANIMLARI ============
const int HX711_DOUT = 4;
const int HX711_SCK  = 5;
const int TILT_PIN   = 18;
const int BUZZER_PIN = 19;
const int SDA_PIN    = 21;
const int SCL_PIN    = 22;

// ============ OLED ============
#define OLED_GENISLIK 128
#define OLED_YUKSEKLIK 64
#define OLED_RESET    -1
#define OLED_ADRES    0x3C
Adafruit_SSD1306 ekran(OLED_GENISLIK, OLED_YUKSEKLIK, &Wire, OLED_RESET);

// ============ LOAD CELL ============
HX711_ADC LoadCell(HX711_DOUT, HX711_SCK);
const float KALIBRASYON_FAKTORU = 1.0;

// ============ ADAFRUIT IO ============
WiFiClientSecure wifiClient;
Adafruit_MQTT_Client mqtt(&wifiClient, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);

// Yayinlanan feeds (ESP32 -> Cloud)
Adafruit_MQTT_Publish waterFeed     = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/water-intake");
Adafruit_MQTT_Publish lastDrinkFeed = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/last-drink-min");

// Abone feeds (Cloud -> ESP32)
Adafruit_MQTT_Subscribe goalFeed    = Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME "/feeds/daily-goal");
Adafruit_MQTT_Subscribe buzzerFeed  = Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME "/feeds/buzzer-enable");

// ============ DURUMLAR ============
float toplamIcilen = 0;
int gunlukHedef = 2000;
unsigned long sonIcmeZamani = 0;
unsigned long hatirlatmaAraligi = 60UL * 60UL * 1000UL;
bool hatirlatmaAktif = true;

bool sisedikti = true;
float dikkenAgirlik = 0;
unsigned long egilmeBaslangic = 0;
const int MIN_EGIK_SURE_MS = 800;
const float MIN_ICME_ML = 10.0;

bool wifiBagli = false;

// ============ BUZZER ============
void calSes(int frekans, int sureMs) {
  ledcWriteTone(BUZZER_PIN, frekans);
  delay(sureMs);
  ledcWriteTone(BUZZER_PIN, 0);
}

void onaySesi() {
  calSes(2000, 80); delay(50);
  calSes(2500, 150);
}

void hatirlatmaSesi() {
  calSes(1000, 200); delay(150);
  calSes(1000, 200);
}

void basariSesi() {
  calSes(523, 150); delay(50);
  calSes(659, 150); delay(50);
  calSes(784, 300);
}

// ============ EKRAN ============
void ekranGuncelle() {
  ekran.clearDisplay();
  ekran.setTextSize(1);
  ekran.setTextColor(SSD1306_WHITE);
  
  ekran.setCursor(0, 0);          // Water -> sari bolge (degismedi)
  ekran.print("Water: ");
  ekran.print((int)toplamIcilen);
  ekran.println(" ml");
  
  ekran.setCursor(0, 18);         // Goal -> 12'den 18'e
  ekran.print("Goal:  ");
  ekran.print(gunlukHedef);
  ekran.println(" ml");
  
  ekran.setCursor(0, 30);         // Last -> 24'ten 30'a
  ekran.print("Last:  ");
  unsigned long gecenDk = (millis() - sonIcmeZamani) / 60000;
  ekran.print(gecenDk);
  ekran.println(" min");
  
  ekran.setCursor(0, 42);         // Status -> 36'dan 42'ye
  ekran.print("Status: ");
  if (gecenDk >= 60) {
    ekran.println("DRINK NOW!");
  } else if (toplamIcilen >= gunlukHedef) {
    ekran.println("GOAL DONE!");
  } else {
    ekran.println("NORMAL");
  }
  
  ekran.setCursor(0, 56);         // Cloud -> degismedi
  ekran.print("Cloud: ");
  ekran.println(wifiBagli ? "ON" : "OFF");
  
  ekran.display();
}

// ============ WIFI ============
void wifiBaglan() {
  Serial.print("WiFi'ye baglanniyor: ");
  Serial.println(WIFI_SSID);
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int deneme = 0;
  while (WiFi.status() != WL_CONNECTED && deneme < 20) {
    delay(500);
    Serial.print(".");
    deneme++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi baglandi. IP: ");
    Serial.println(WiFi.localIP());
    wifiBagli = true;
  } else {
    Serial.println();
    Serial.println("WiFi baglantisi basarisiz, offline calisacak.");
    wifiBagli = false;
  }
}

void mqttBaglan() {
  if (!wifiBagli) return;
  if (mqtt.connected()) return;
  
  Serial.print("MQTT'ye baglanniyor...");
  int8_t ret;
  uint8_t deneme = 3;
  while ((ret = mqtt.connect()) != 0 && deneme > 0) {
    Serial.println(mqtt.connectErrorString(ret));
    Serial.println("5 sn sonra tekrar...");
    mqtt.disconnect();
    delay(5000);
    deneme--;
  }
  
  if (mqtt.connected()) {
    Serial.println("MQTT baglandi!");
  }
}

void cloudGonder(float waterMl, unsigned long lastDrinkMin) {
  if (!mqtt.connected()) return;
  
  if (waterFeed.publish(waterMl)) {
    Serial.print("Water gonderildi: "); Serial.println(waterMl);
  }
  if (lastDrinkFeed.publish((uint32_t)lastDrinkMin)) {
    Serial.print("LastDrink gonderildi: "); Serial.println(lastDrinkMin);
  }
}

void cloudKomutOku() {
  if (!mqtt.connected()) return;
  
  Adafruit_MQTT_Subscribe *subscription;
  while ((subscription = mqtt.readSubscription(50))) {
    if (subscription == &goalFeed) {
      int yeniHedef = atoi((char *)goalFeed.lastread);
      if (yeniHedef > 0) {
        gunlukHedef = yeniHedef;
        Serial.print("Yeni hedef: "); Serial.println(gunlukHedef);
      }
    }
    else if (subscription == &buzzerFeed) {
      String deger = String((char *)buzzerFeed.lastread);
      hatirlatmaAktif = (deger == "ON" || deger == "1" || deger == "true");
      Serial.print("Hatirlatma: "); Serial.println(hatirlatmaAktif ? "ACIK" : "KAPALI");
    }
  }
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Su Takip Sistemi ===");
  
  // Buzzer
  ledcAttach(BUZZER_PIN, 2000, 8);
  
  // Tilt
  pinMode(TILT_PIN, INPUT);
  
  // I2C + OLED
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!ekran.begin(SSD1306_SWITCHCAPVCC, OLED_ADRES)) {
    Serial.println("OLED HATA!");
  }
  ekran.clearDisplay();
  ekran.setTextSize(2);
  ekran.setTextColor(SSD1306_WHITE);
  ekran.setCursor(10, 20);
  ekran.println("HELLO!");
  ekran.display();
  delay(1500);
  
  // WiFi
  ekran.clearDisplay();
  ekran.setTextSize(1);
  ekran.setCursor(0, 0);
  ekran.println("WiFi baglaniyor...");
  ekran.display();
  
  wifiBaglan();
  
  // SSL sertifika dogrulamayi atla (kolayca calismasi icin)
  wifiClient.setInsecure();
  
  // MQTT abone
  mqtt.subscribe(&goalFeed);
  mqtt.subscribe(&buzzerFeed);
  
  if (wifiBagli) {
    mqttBaglan();
  }
  
  // Load Cell
  ekran.clearDisplay();
  ekran.setCursor(0, 0);
  ekran.println("Load cell");
  ekran.println("kalibre ediliyor...");
  ekran.display();
  
  LoadCell.begin();
  LoadCell.start(2000, true);
  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("HX711 baglantisi yok!");
    while (1) delay(100);
  }
  LoadCell.setCalFactor(KALIBRASYON_FAKTORU);
  
  delay(500);
  LoadCell.update();
  dikkenAgirlik = LoadCell.getData();
  sonIcmeZamani = millis();
  
  onaySesi();
  Serial.println("Sistem hazir!");
  ekranGuncelle();
}

// ============ LOOP ============
void loop() {
  // MQTT baglantisini canli tut
  if (wifiBagli) {
    mqttBaglan();
    cloudKomutOku();
  }
  
  LoadCell.update();
  
  int tiltDurum = digitalRead(TILT_PIN);
  bool sisedik = (tiltDurum == LOW);
  
  if (sisedikti && !sisedik) {
    egilmeBaslangic = millis();
    dikkenAgirlik = LoadCell.getData();
    Serial.print("Egildi. Onceki agirlik: ");
    Serial.println(dikkenAgirlik, 1);
  }
  else if (!sisedikti && sisedik) {
    unsigned long egikSure = millis() - egilmeBaslangic;
    
    if (egikSure >= MIN_EGIK_SURE_MS) {
      delay(500);
      LoadCell.update();
      float yeniAgirlik = LoadCell.getData();
      float fark = dikkenAgirlik - yeniAgirlik;
      
      Serial.print("Dik geldi. Fark: ");
      Serial.println(fark, 1);
      
      if (fark >= MIN_ICME_ML) {
        toplamIcilen += fark;
        sonIcmeZamani = millis();
        Serial.print(">>> ICME: ");
        Serial.println(fark, 1);
        
        onaySesi();
        
        // Cloud'a gonder
        cloudGonder(toplamIcilen, 0);
        
        if (toplamIcilen >= gunlukHedef) {
          basariSesi();
        }
      }
    }
  }
  
  sisedikti = sisedik;
  
  // Hatirlatma
  if (hatirlatmaAktif && toplamIcilen < gunlukHedef && (millis() - sonIcmeZamani > hatirlatmaAraligi)) {
    Serial.println("Hatirlatma!");
    hatirlatmaSesi();
  }
  
  // Periyodik gorevler
  static unsigned long sonEkran = 0;
  if (millis() - sonEkran > 1000) {
    ekranGuncelle();
    sonEkran = millis();
  }
  
  // Cloud'a periyodik son icme zamani gonder (her 30 sn)
  static unsigned long sonCloudGonder = 0;
  if (wifiBagli && millis() - sonCloudGonder > 30000) {
    unsigned long gecenDk = (millis() - sonIcmeZamani) / 60000;
    lastDrinkFeed.publish((uint32_t)gecenDk);
    sonCloudGonder = millis();
  }
}