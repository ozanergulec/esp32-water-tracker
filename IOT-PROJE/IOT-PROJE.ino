#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HX711_ADC.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Adafruit_MQTT.h>
#include <Adafruit_MQTT_Client.h>

#include "secrets.h"  // WIFI_SSID, WIFI_PASSWORD, AIO_USERNAME, AIO_KEY (gitignored)

// ============ ADAFRUIT IO SUNUCU ============
#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  8883

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
// Kalibrasyon: 500g referansla 50514 ham deger okundu -> 50514/500 = 101.03
const float KALIBRASYON_FAKTORU = 101.03;

// ============ ADAFRUIT IO ============
WiFiClientSecure wifiClient;
Adafruit_MQTT_Client mqtt(&wifiClient, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);

Adafruit_MQTT_Publish waterFeed     = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/water-intake");
Adafruit_MQTT_Publish lastDrinkFeed = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/last-drink-min");

Adafruit_MQTT_Subscribe goalFeed    = Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME "/feeds/daily-goal");
Adafruit_MQTT_Subscribe buzzerFeed  = Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME "/feeds/buzzer-enable");

// ============ DURUMLAR ============
float toplamIcilen = 0;
int gunlukHedef = 500;
unsigned long sonIcmeZamani = 0;
unsigned long hatirlatmaAraligi = 60UL * 60UL * 1000UL;
bool hatirlatmaAktif = true;

bool sisedikti = true;
float sonStabilAgirlik = 0;
unsigned long egilmeBaslangic = 0;
const int MIN_EGIK_SURE_MS = 800;
const float MIN_ICME_ML = 15.0;

// Sise dik durdugu sirada arka plan agirlik takibi
const unsigned long DIK_TAKIP_ARALIGI_MS = 500;
const float DIK_STABIL_TOLERANS = 3.0;
float dikSonAnlikAgirlik = 0;
unsigned long sonDikOkuma = 0;

// Stabil agirlik okuma parametreleri (dik gelme sonrasi)
const int STABIL_ORNEK_SAYISI = 10;
const float STABIL_TOLERANS = 2.0;
const unsigned long STABIL_MAX_BEKLEME = 4000;

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

// ============ TAZE VERI BEKLEYIP OKU ============
// HX711 ~10Hz uretiyor, update() her zaman taze veri donmez.
// Bu fonksiyon yeni bir okuma gelene kadar bekler.
bool tazeVeriOku(float &deger, unsigned long timeoutMs = 200) {
  unsigned long baslangic = millis();
  while (millis() - baslangic < timeoutMs) {
    if (LoadCell.update()) {
      deger = LoadCell.getData();
      return true;
    }
    delay(1);
  }
  return false;
}

// ============ STABIL AGIRLIK OKUMA ============
// Sise yerlestikten sonra titresim/sallanma bitene kadar bekler
float stabilAgirlikOku() {
  unsigned long baslangic = millis();
  float oncekiOrt = 0;
  bool ilkOlcum = true;

  while (millis() - baslangic < STABIL_MAX_BEKLEME) {
    float toplam = 0;
    int sayac = 0;
    unsigned long ornekBaslangic = millis();

    // 10 taze ornek topla (HX711 ~10Hz, ~1 sn surer)
    while (sayac < STABIL_ORNEK_SAYISI && millis() - ornekBaslangic < 2000) {
      float anlik;
      if (tazeVeriOku(anlik, 150)) {
        toplam += anlik;
        sayac++;
      }
    }

    if (sayac == 0) {
      Serial.println("Uyari: HX711'den veri gelmiyor!");
      continue;
    }
    float ortalama = toplam / sayac;
    Serial.print("Ortalama (n="); Serial.print(sayac);
    Serial.print("): "); Serial.println(ortalama, 1);

    if (!ilkOlcum && abs(ortalama - oncekiOrt) < STABIL_TOLERANS) {
      Serial.print(">>> Stabil: "); Serial.println(ortalama, 1);
      return ortalama;
    }

    oncekiOrt = ortalama;
    ilkOlcum = false;
  }

  Serial.print("Timeout, son deger: "); Serial.println(oncekiOrt, 1);
  return oncekiOrt;
}

// ============ EKRAN ============
void ekranGuncelle() {
  ekran.clearDisplay();
  ekran.setTextSize(1);
  ekran.setTextColor(SSD1306_WHITE);

  ekran.setCursor(0, 0);
  ekran.print("Water: ");
  ekran.print((int)toplamIcilen);
  ekran.println(" ml");

  ekran.setCursor(0, 18);
  ekran.print("Goal:  ");
  ekran.print(gunlukHedef);
  ekran.println(" ml");

  ekran.setCursor(0, 30);
  ekran.print("Last:  ");
  unsigned long gecenDk = (millis() - sonIcmeZamani) / 60000;
  ekran.print(gecenDk);
  ekran.println(" min");

  ekran.setCursor(0, 42);
  ekran.print("Status: ");
  if (gecenDk >= 60) {
    ekran.println("DRINK NOW!");
  } else if (toplamIcilen >= gunlukHedef) {
    ekran.println("GOAL DONE!");
  } else {
    ekran.println("NORMAL");
  }

  ekran.setCursor(0, 56);
  ekran.print("Cloud: ");
  ekran.println(wifiBagli ? "ON" : "OFF");

  ekran.display();
}

void olcumEkrani(const char* satir1, const char* satir2) {
  ekran.clearDisplay();
  ekran.setTextSize(1);
  ekran.setTextColor(SSD1306_WHITE);
  ekran.setCursor(0, 20);
  ekran.println(satir1);
  ekran.setCursor(0, 35);
  ekran.println(satir2);
  ekran.display();
}

// ============ WIFI ============
void wifiBaglan() {
  Serial.print("WiFi'ye baglaniyor: ");
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

  Serial.print("MQTT'ye baglaniyor...");
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

// ============ DIK DURURKEN AGIRLIK TAKIBI ============
// Sise sensorun ustunde dik dururken sonStabilAgirlik'i gunceller
void dikAgirlikTakip() {
  if (millis() - sonDikOkuma < DIK_TAKIP_ARALIGI_MS) return;

  float anlik;
  if (!tazeVeriOku(anlik, 150)) return;  // taze veri yoksa atla

  // Onceki anlik okumayla yakinsa stabil say -> sonStabilAgirlik'i guncelle
  if (abs(anlik - dikSonAnlikAgirlik) < DIK_STABIL_TOLERANS) {
    sonStabilAgirlik = anlik;
  }

  dikSonAnlikAgirlik = anlik;
  sonDikOkuma = millis();
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Su Takip Sistemi ===");

  ledcAttach(BUZZER_PIN, 2000, 8);
  pinMode(TILT_PIN, INPUT);

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

  ekran.clearDisplay();
  ekran.setTextSize(1);
  ekran.setCursor(0, 0);
  ekran.println("WiFi baglaniyor...");
  ekran.display();

  wifiBaglan();
  wifiClient.setInsecure();

  mqtt.subscribe(&goalFeed);
  mqtt.subscribe(&buzzerFeed);

  if (wifiBagli) {
    mqttBaglan();
  }

  ekran.clearDisplay();
  ekran.setCursor(0, 0);
  ekran.println("Load cell");
  ekran.println("kalibre ediliyor...");
  ekran.display();

  // Load Cell baslat
  // ONEMLI: Bu sirada sise sensorde dik durmali!
  // Tare sirasinda zero offset olarak kullanacak.
  LoadCell.begin();
  LoadCell.start(2000, true);
  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("HX711 baglantisi yok!");
    while (1) delay(100);
  }
  LoadCell.setCalFactor(KALIBRASYON_FAKTORU);

  // Tare sonrasi sise zaten ustte oldugu icin "0" okuyacak.
  // Ama biz sisenin gercek agirligini referans almaliyiz.
  // Bu yuzden start'tan SONRA kalibrasyon faktorunu uygulayip,
  // su anki agirligi (sise + icindeki su) referans olarak alacagiz.
  
  // Not: Eger sensorde sise yokken baslattiysan, sise koyduktan
  // sonra agirlik artar ve dogru farki yine de hesaplar.

  delay(1000);  // bir saniye stabilizasyon
  ekran.clearDisplay();
  ekran.setCursor(0, 0);
  ekran.println("Stabil okuma...");
  ekran.display();

  sonStabilAgirlik = stabilAgirlikOku();
  dikSonAnlikAgirlik = sonStabilAgirlik;
  sonIcmeZamani = millis();

  Serial.print("Baslangic referans agirlik: ");
  Serial.print(sonStabilAgirlik, 1);
  Serial.println(" g");

  onaySesi();
  Serial.println("Sistem hazir!");
  ekranGuncelle();
}

// ============ LOOP ============
void loop() {
  if (wifiBagli) {
    mqttBaglan();
    cloudKomutOku();
  }

  // HX711'in surekli veri akisini saglamak icin update'i cagir
  LoadCell.update();

  int tiltDurum = digitalRead(TILT_PIN);
  bool sisedik = (tiltDurum == LOW);

  // Sise DIK duruyor -> arka planda agirligi takip et
  if (sisedik && sisedikti) {
    dikAgirlikTakip();
  }

  // Sise egildi (dik -> egik gecisi)
  // BURADA OLCUM YAPMIYORUZ! Sise artik elinde.
  if (sisedikti && !sisedik) {
    egilmeBaslangic = millis();
    Serial.print("Sise egildi. Onceki stabil agirlik: ");
    Serial.print(sonStabilAgirlik, 1);
    Serial.println(" g");
  }
  // Sise dike geldi (egik -> dik gecisi) — fark hesapla
  else if (!sisedikti && sisedik) {
    unsigned long egikSure = millis() - egilmeBaslangic;

    if (egikSure >= MIN_EGIK_SURE_MS) {
      Serial.println("Sise sensore geri kondu. Yerlesme bekleniyor...");
      olcumEkrani("Siseyi birakin", "Olcum yapiliyor...");
      delay(1500);

      float yeniAgirlik = stabilAgirlikOku();
      float fark = sonStabilAgirlik - yeniAgirlik;

      Serial.print("Onceki: "); Serial.print(sonStabilAgirlik, 1);
      Serial.print(" g | Yeni: "); Serial.print(yeniAgirlik, 1);
      Serial.print(" g | Fark: "); Serial.print(fark, 1);
      Serial.println(" g");

      if (fark >= MIN_ICME_ML) {
        toplamIcilen += fark;
        sonIcmeZamani = millis();

        Serial.print(">>> ICME ALGILANDI: ");
        Serial.print(fark, 1);
        Serial.print(" ml | Toplam: ");
        Serial.println(toplamIcilen, 1);

        onaySesi();
        cloudGonder(toplamIcilen, 0);

        if (toplamIcilen >= gunlukHedef) {
          basariSesi();
        }
      } else {
        Serial.println("Fark yetersiz, icme sayilmadi.");
      }

      // Yeni baslangic noktasi
      sonStabilAgirlik = yeniAgirlik;
      dikSonAnlikAgirlik = yeniAgirlik;
    } else {
      Serial.println("Cok kisa egim, sayilmadi.");
    }
  }

  sisedikti = sisedik;

  // Su icme hatirlatmasi
  if (hatirlatmaAktif &&
      toplamIcilen < gunlukHedef &&
      (millis() - sonIcmeZamani > hatirlatmaAraligi)) {
    Serial.println("Hatirlatma: Su ic!");
    hatirlatmaSesi();
    sonIcmeZamani = millis();
  }

  // Ekran guncelle (her 1 sn)
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
