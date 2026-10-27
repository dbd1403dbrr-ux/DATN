#include <WiFi.h>
#include <time.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>


// ============================================================
//                    CAU HINH MQTT
// ============================================================
const char* MQTT_HOST =
    "21ad0a033055473782c625f0ccdea89b.s1.eu.hivemq.cloud";
const int MQTT_PORT = 8883;
const char* MQTT_USERNAME = "Duong";
const char* MQTT_PASSWORD = "12345678";

// APP -> ESP32
const char* TOPIC_CONTROL = "gate/esp32/control";
// ESP32 -> APP
const char* TOPIC_STATUS = "gate/esp32/app";

WiFiClientSecure espClient;
PubSubClient client(espClient);

// ============================================================
//                    WIFI
// ============================================================
String WIFI_SSID = "tata";
String WIFI_PASSWORD = "11111111";

unsigned long lastWiFiAttempt = 0;
bool wifiWasConnected = false;

// ============================================================
//                    THOI GIAN
// ============================================================
bool timeIsValid = false;

// ============================================================
//                    MQTT / HEARTBEAT
// ============================================================
unsigned long lastMQTTAttempt = 0;
const unsigned long MQTT_RETRY_INTERVAL = 5000;

bool app_ok = false;
unsigned long lastAppHeartbeat = 0;
unsigned long lastEspHeartbeatSend = 0;
const unsigned long HEARTBEAT_INTERVAL = 5000;
const unsigned long APP_TIMEOUT = 10000;

// ============================================================
//                    HANG DOI MQTT
// ============================================================
#define MAX_QUEUE 20
String mqttQueue[MAX_QUEUE];
int queueFront = 0;
int queueRear = 0;
int queueCount = 0;
unsigned long lastRetry = 0;
const unsigned long RETRY_TIME = 1000;

// ============================================================
//                    LCD
// ============================================================
#define I2C_SDA 21
#define I2C_SCL 22
LiquidCrystal_I2C lcd(0x27, 16, 2);

unsigned long messageUntil = 0;
unsigned long lastLCDUpdate = 0;

// ============================================================
//                    RFID RC522
// ============================================================
#define RFID_SS_PIN 27
#define RFID_RST_PIN 33
#define RFID_SCK_PIN 18
#define RFID_MISO_PIN 19
#define RFID_MOSI_PIN 23

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);

#define RFID_COOLDOWN 3000
String lastUID = "";
unsigned long lastRFIDTime = 0;

// ============================================================
//                    SERVO
// ============================================================
#define SERVO_IN_PIN 13
#define SERVO_OUT_PIN 14

// Neu can dao huong mot servo, doi rieng 2 cap goc duoi day.
#define GATE_IN_CLOSE_ANGLE 0
#define GATE_IN_OPEN_ANGLE 90
#define GATE_OUT_CLOSE_ANGLE 0
#define GATE_OUT_OPEN_ANGLE 90

#define GATE_OPEN_TIME 4000

Servo servoIn;
Servo servoOut;

bool gateInOpen = false;
bool gateOutOpen = false;
unsigned long gateInTimer = 0;
unsigned long gateOutTimer = 0;

// ============================================================
//                    BUZZER
// ============================================================
#define BUZZER_PIN 26

// ============================================================
//       CHI GIU 2 CAM BIEN O CONG VAO / CONG RA
//       DA BO 2 CAM BIEN O DO XE
// ============================================================
#define IR_ENTRANCE_PIN 15
#define IR_EXIT_PIN 17
#define IR_ACTIVE_LEVEL LOW

bool entranceDetected = false;
bool exitDetected = false;
bool previousEntranceState=false;
bool previousExitState =false;
unsigned long lastSensorRead = 0;

// ============================================================
//                    THE RFID
// ============================================================
#define MAX_CARDS 50

struct Card
{
    String uid;
    String name;
    bool month;
};

Card cards[MAX_CARDS];
int cardCount = 0;

bool addCardMode = false;
bool deleteCardMode = false;
String newCardName = "";
bool newCardMonthly = false;
unsigned long managementModeStart = 0;
const unsigned long MANAGEMENT_TIMEOUT = 10000;

// ============================================================
//                    PARKING SESSION
// ============================================================
// MAX_SESSIONS la gioi han bo nho cua chuong trinh.
// parkingCapacity moi la suc chua thuc te, co the doi tu APP.
#define MAX_SESSIONS 50

struct ParkingSession
{
    String uid;
    String name;
    time_t timeIn;
    bool active;
};

ParkingSession sessions[MAX_SESSIONS];
int parkingCapacity = 2;

// ============================================================
//                    GIA GUI XE
// ============================================================
#define PRICE_PER_HOUR 5000

// ============================================================
//                    TRANG THAI HE THONG
// ============================================================
bool systemPaused = false;
Preferences preferences;

// ============================================================
//                    KHAI BAO HAM
// ============================================================
void connectWiFi();
void handleWiFi();
bool syncTime();

void ketNoiMQTT();
void callback(char* topic, byte* payload, unsigned int length);
void handleHeartbeat();
bool pushMQTTQueue(const String& data);
bool sendMQTT(const String& data);
void handleMQTTQueue();
void publishNow(const String& data);

void readGateSensors();
void handleRFID();
String getUID();
void processCard(const String& uid);

int findCard(const String& uid);
int findActiveSession(const String& uid);
int getActiveSessionCount();
bool createSession(const String& uid, const String& name);
void clearSession(int index);

bool addNewCard(const String& uid, const String& name, bool month);
bool deleteCardByUID(const String& uid);
void saveCards();
void loadCards();
void saveSessions();
void loadSessions();
void saveCapacity();
void loadCapacity();
void checkEntranceSensor();
void checkExitSensor();

void openEntranceGate();
void openExitGate();
void updateGate();

void beep(int count);
void showLCD(String line1, String line2, unsigned long duration);
void showExitInfo(const String& name, long minutes, unsigned long fee);
void updateLCD();

void sendParkingStatus();

// ============================================================
//                    MQTT CALLBACK - KHONG AES
// ============================================================
void callback(char* topic, byte* payload, unsigned int length)
{
    if (String(topic) != TOPIC_CONTROL)
        return;

    String data = "";
    for (unsigned int i = 0; i < length; i++)
        data += (char)payload[i];

    data.trim();

    Serial.print("MQTT NHAN: ");
    Serial.println(data);

    // APP heartbeat
    if (data == "APPOK")
    {
        bool wasOffline = !app_ok;
        app_ok = true;
        lastAppHeartbeat = millis();

        if (wasOffline)
        {
            Serial.println("APP ONLINE");
            sendParkingStatus();
        }
        return;
    }

    if (data == "GET_STATUS")
    {
        publishNow("STATUS|ONLINE");
        sendParkingStatus();
        return;
    }

    // Them the: ADD|Ten|0 hoac ADD|Ten|1
    if (data.startsWith("ADD|"))
    {
        int p1 = data.indexOf('|');
        int p2 = data.indexOf('|', p1 + 1);

        if (p1 < 0 || p2 < 0)
        {
            sendMQTT("ADD_FAIL|FORMAT");
            return;
        }

        String name = data.substring(p1 + 1, p2);
        String monthly = data.substring(p2 + 1);
        name.trim();
        monthly.trim();

        if (name.length() == 0)
        {
            sendMQTT("ADD_FAIL|TEN_TRONG");
            return;
        }

        newCardName = name;
        newCardMonthly = (monthly == "1");
        addCardMode = true;
        deleteCardMode = false;
        managementModeStart = millis();

        showLCD("DE THE LEN", "RFID", 2000);
        publishNow("ADD_WAIT_SCAN");
        return;
    }

    // Xoa truc tiep theo UID
    if (data.startsWith("DELETE_UID|"))
    {
        String uid = data.substring(String("DELETE_UID|").length());
        uid.trim();
        uid.toUpperCase();

        if (uid.length() == 0)
        {
            sendMQTT("DELETE_FAIL|UID_TRONG");
            return;
        }

        // if (findActiveSession(uid) != -1)
        // {
        //     sendMQTT("DELETE_FAIL|XE_DANG_TRONG_BAI");
        //     showLCD("KHONG THE XOA", "XE DANG TRONG BAI", 2500);
        //     return;
        // }

        if (deleteCardByUID(uid))
            
            
           {
            sendMQTT("DELETE_OK|" + uid);
            int y=findActiveSession( uid);
            clearSession(y);
           }
        else
            sendMQTT("DELETE_FAIL|KHONG_TIM_THAY");

        return;
    }

    // Xoa bang cach quet the
    if (data == "DELETE_QUET" || data == "DELETE_SCANNED")
    {
        deleteCardMode = true;
        addCardMode = false;
        managementModeStart = millis();
        showLCD("DE THE LEN", "RFID", 2000);
        publishNow("DELETE_WAIT_SCAN");
        return;
    }

    // Dieu chinh suc chua: SET_CAPACITY|5
    if (data.startsWith("SET_CAPACITY|"))
    {
        String value = data.substring(String("SET_CAPACITY|").length());
        value.trim();
        int newCapacity = value.toInt();
        int current = getActiveSessionCount();

        if (newCapacity < 1 || newCapacity > MAX_SESSIONS)
        {
            sendMQTT("CAPACITY_FAIL|GIA_TRI_KHONG_HOP_LE");
            return;
        }

        // Khong cho giam nho hon so xe dang co trong bai
        if (newCapacity < current)
        {
            sendMQTT(
                "CAPACITY_FAIL|NHO_HON_SO_XE_HIEN_TAI|" +
                String(current)
            );
            return;
        }

        parkingCapacity = newCapacity;
        saveCapacity();

        sendMQTT(
            "CAPACITY_OK|" +
            String(parkingCapacity) + "|" +
            String(current)
        );

        sendParkingStatus();
        showLCD("SUC CHUA MOI", String(parkingCapacity), 2000);
        return;
    }

    // Dieu khien thu cong
    if (data == "OPEN_IN")
    {
        openEntranceGate();
        showLCD("MO CONG VAO", "TU APP", 2000);
        return;
    }

    if (data == "OPEN_OUT" || data == "B")
    {
        openExitGate();
        showLCD("MO CONG RA", "TU APP", 2000);
        return;
    }

    if (data == "PAUSE")
    {
        systemPaused = true;
        sendMQTT("PAUSED");
        showLCD("HE THONG", "TAM DUNG", 2000);
        return;
    }

    if (data == "RESUME")
    {
        systemPaused = false;
        sendMQTT("RESUMED");
        showLCD("HE THONG", "TIEP TUC", 2000);
        return;
    }

    sendMQTT("ERROR|LENH_KHONG_HOP_LE");
}

// ============================================================
//                    SETUP
// ============================================================
void setup()
{
    Serial.begin(115200);
    esp_task_wdt_config_t config = {
        .timeout_ms = 10000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };

    esp_task_wdt_init(&config);

    // Theo dõi task loop hiện tại
    esp_task_wdt_add(NULL);

    Serial.setTimeout(50);
    delay(300);

    preferences.begin("parking", false);
    loadCards();
    loadSessions();
    loadCapacity();

    Wire.begin(I2C_SDA, I2C_SCL);
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("BAI DO XE");
    lcd.setCursor(0, 1);
    lcd.print("Khoi dong...");

    SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
    rfid.PCD_Init();
    delay(100);

    pinMode(IR_ENTRANCE_PIN, INPUT);
    pinMode(IR_EXIT_PIN, INPUT);

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    servoIn.setPeriodHertz(50);
    servoOut.setPeriodHertz(50);

    servoIn.attach(SERVO_IN_PIN, 500, 2400);
    servoOut.attach(SERVO_OUT_PIN, 500, 2400);

    servoIn.write(GATE_IN_CLOSE_ANGLE);
    servoOut.write(GATE_OUT_CLOSE_ANGLE);

    readGateSensors();

    connectWiFi();

    // Van dung TLS cua HiveMQ Cloud
    espClient.setInsecure();
    client.setServer(MQTT_HOST, MQTT_PORT);
    client.setCallback(callback);
    client.setBufferSize(512);

    delay(700);
    lcd.clear();
    esp_task_wdt_reset();
}

// ============================================================

void loop()
{
    handleWiFi();

    if (millis() - lastSensorRead >= 100)
    {
        lastSensorRead = millis();
        readGateSensors();
        checkEntranceSensor();
        checkExitSensor();
    }
    

    handleRFID();
    esp_task_wdt_reset();
    updateGate();
    esp_task_wdt_reset();
    updateLCD();
    esp_task_wdt_reset();

    if ((addCardMode || deleteCardMode) &&
        millis() - managementModeStart >= MANAGEMENT_TIMEOUT)
    {
        addCardMode = false;
        deleteCardMode = false;
        showLCD("HET THOI GIAN", "THAO TAC", 1500);
    }

    ketNoiMQTT();
    esp_task_wdt_reset();

    if (client.connected())
        client.loop();

    handleHeartbeat();

    if (app_ok)
        handleMQTTQueue();
   esp_task_wdt_reset();
}

// ============================================================
//                    WIFI
// ============================================================
void connectWiFi()
{
    if (WIFI_SSID.length() == 0)
        return;

    if (WiFi.status() == WL_CONNECTED)
        return;

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID.c_str(), WIFI_PASSWORD.c_str());

    Serial.println("Bat dau ket noi WiFi...");
}

void handleWiFi()
{
    bool connected = (WiFi.status() == WL_CONNECTED);

    if (connected)
    {
        if (!wifiWasConnected)
        {
            wifiWasConnected = true;
            Serial.println("WIFI DA KET NOI");
            Serial.print("IP: ");
            Serial.println(WiFi.localIP());
            syncTime();
        }
        return;
    }

    if (wifiWasConnected)
    {
        wifiWasConnected = false;
        app_ok = false;
        Serial.println("WIFI BI MAT");
    }

    if (millis() - lastWiFiAttempt < 5000)
        return;

    lastWiFiAttempt = millis();
    WiFi.begin(WIFI_SSID.c_str(), WIFI_PASSWORD.c_str());
}

bool syncTime()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        timeIsValid = false;
        return false;
    }

    configTzTime(
        "ICT-7",
        "pool.ntp.org",
        "time.nist.gov",
        "time.google.com"
    );

    struct tm timeinfo;

    // Cho toi da khoang 5 giay
    for (int i = 0; i < 10; i++)
    {
        if (getLocalTime(&timeinfo, 500))
        {
            if (timeinfo.tm_year >= (2020 - 1900))
            {
                timeIsValid = true;
                Serial.println("NTP OK");
                return true;
            }
        }
        delay(200);
    }

    timeIsValid = false;
    Serial.println("NTP THAT BAI");
    return false;
}

// ============================================================
//                    MQTT
// ============================================================
void ketNoiMQTT()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    if (client.connected())
        return;

    if (millis() - lastMQTTAttempt < MQTT_RETRY_INTERVAL)
        return;

    lastMQTTAttempt = millis();

    String clientId = "ESP32_Gate_" + String(random(0xffff), HEX);

    Serial.println("Dang ket noi MQTT...");

    if (client.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD))
    {
        Serial.println("MQTT CONNECTED");
        client.subscribe(TOPIC_CONTROL);
        app_ok = false;
        publishNow("STATUS|ONLINE");
    }
    else
    {
        Serial.print("MQTT failed, state = ");
        Serial.println(client.state());
    }
}

void publishNow(const String& data)
{
    if (!client.connected())
        return;

    client.publish(TOPIC_STATUS, data.c_str());
}

bool pushMQTTQueue(const String& data)
{
    if (queueCount >= MAX_QUEUE)
    {
        Serial.println("MQTT QUEUE DA DAY");
        return false;
    }

    mqttQueue[queueRear] = data;
    queueRear = (queueRear + 1) % MAX_QUEUE;
    queueCount++;

    Serial.print("QUEUE +: ");
    Serial.println(data);
    return true;
}

bool sendMQTT(const String& data)
{
    if (!client.connected() || !app_ok)
    {
        pushMQTTQueue(data);
        return false;
    }

    bool ok = client.publish(TOPIC_STATUS, data.c_str());

    if (ok)
    {
        Serial.print("MQTT GUI: ");
        Serial.println(data);
        return true;
    }

    pushMQTTQueue(data);
    return false;
}

void handleMQTTQueue()
{
    if (queueCount == 0 || !client.connected() || !app_ok)
        return;

    if (millis() - lastRetry < RETRY_TIME)
        return;

    lastRetry = millis();

    String data = mqttQueue[queueFront];
    bool ok = client.publish(TOPIC_STATUS, data.c_str());

    if (ok)
    {
        mqttQueue[queueFront] = "";
        queueFront = (queueFront + 1) % MAX_QUEUE;
        queueCount--;

        Serial.print("QUEUE GUI LAI OK, CON: ");
        Serial.println(queueCount);
    }
}

void handleHeartbeat()
{
    if (app_ok && millis() - lastAppHeartbeat > APP_TIMEOUT)
    {
        app_ok = false;
        Serial.println("APP OFFLINE");
    }

    if (client.connected() &&
        millis() - lastEspHeartbeatSend >= HEARTBEAT_INTERVAL)
    {
        lastEspHeartbeatSend = millis();
        publishNow("ESPOK");
    }
}

// ============================================================
//              CAM BIEN CONG VAO / CONG RA
// ============================================================
void readGateSensors()
{
    entranceDetected =
        (digitalRead(IR_ENTRANCE_PIN) == IR_ACTIVE_LEVEL);

    exitDetected =
        (digitalRead(IR_EXIT_PIN) == IR_ACTIVE_LEVEL);
}

// ============================================================
//                    RFID
// ============================================================
void handleRFID()
{
    if (!rfid.PICC_IsNewCardPresent())
        return;

    if (!rfid.PICC_ReadCardSerial())
        return;

    String uid = getUID();

    if (uid == lastUID &&
        millis() - lastRFIDTime < RFID_COOLDOWN)
    {
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
        return;
    }

    lastUID = uid;
    lastRFIDTime = millis();

    // Chi thong bao scan tuc thoi, khong dua vao queue.
    if (client.connected() && app_ok)
        publishNow("SCAN|" + uid);

    processCard(uid);

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}

String getUID()
{
    String uid = "";

    for (byte i = 0; i < rfid.uid.size; i++)
    {
        if (rfid.uid.uidByte[i] < 0x10)
            uid += "0";

        uid += String(rfid.uid.uidByte[i], HEX);
    }

    uid.toUpperCase();
    return uid;
}

void processCard(const String& uid)
{
    // ---------------- XOA BANG QUET ----------------
    if (deleteCardMode)
    {
        deleteCardMode = false;
        
  
    // if (findActiveSession(uid) != -1)
    //     return false;
        if (findActiveSession(uid) != -1)
        {
            beep(3);
            showLCD("KHONG THE XOA", "XE DANG TRONG BAI", 2500);
            sendMQTT("DELETE_FAIL|XE_DANG_TRONG_BAI");
            return;
        }
        int y=findActiveSession( uid);
        sessions[y].active=false;

        if (deleteCardByUID(uid))
            sendMQTT("DELETE_OK|" + uid);
        else
            sendMQTT("DELETE_FAIL|KHONG_TIM_THAY");

        return;
    }

    // ---------------- THEM THE ----------------
    if (addCardMode)
    {
        addCardMode = false;

        if (addNewCard(uid, newCardName, newCardMonthly))
        {
            sendMQTT(
                "ADD_OK|" + uid + "|" + newCardName + "|" +
                String(newCardMonthly ? 1 : 0)
            );
        }
        else
        {
            sendMQTT("ADD_FAIL|THE_DA_TON_TAI_HOAC_DANH_SACH_DAY");
        }

        newCardName = "";
        newCardMonthly = false;
        return;
    }

    if (systemPaused)
    {
        beep(2);
        showLCD("HE THONG", "DANG TAM DUNG", 2000);
        return;
    }

    int cardIndex = findCard(uid);

    if (cardIndex == -1)
    {
        beep(3);
        showLCD("THE CHUA DANG KY", "KHONG TRUY CAP", 2500);
        sendMQTT("CARD_INVALID|" + uid);
        return;
    }

    String name = cards[cardIndex].name;
    int sessionIndex = findActiveSession(uid);

    // ========================================================
    // XE VAO: UID CHUA CO SESSION ACTIVE
    // ========================================================
    if (sessionIndex == -1)
    {
        if (!entranceDetected)
        {
            beep(2);
            showLCD("VUI LONG DO", "DUNG VACH VAO", 1500);
            return;
        }

        int currentCars = getActiveSessionCount();

        if (currentCars >= parkingCapacity)
        {
            beep(3);
            showLCD("BAI XE DA DAY", "KHONG THE VAO", 2500);
            sendParkingStatus();
            return;
        }

        if (!timeIsValid)
        {
            beep(3);
            showLCD("LOI THOI GIAN", "KIEM TRA WIFI", 2000);
            return;
        }

        if (!createSession(uid, name))
        {
            beep(3);
            showLCD("LOI SESSION", "KHONG THE VAO", 2000);
            return;
        }

        sendMQTT(
            "IN|" + uid + "|" + name + "|" +
            String(cards[cardIndex].month ? 1 : 0)
        );

        openEntranceGate();
        beep(2);
        showLCD("WELCOME", name, 2500);
        sendParkingStatus();
        return;
    }

    // ========================================================
    // XE RA: UID DA CO SESSION ACTIVE
    // ========================================================
    if (!exitDetected)
    {
        beep(2);
        showLCD("VUI LONG DO", "DUNG VACH RA", 1500);
        return;
    }

    if (!timeIsValid)
    {
        beep(3);
        showLCD("LOI THOI GIAN", "KIEM TRA WIFI", 2000);
        return;
    }

    time_t timeOut = time(nullptr);
    long seconds = difftime(timeOut, sessions[sessionIndex].timeIn);
    long minutes = seconds / 60;
    if (minutes < 0)
        minutes = 0;

    unsigned long hours = (minutes + 59) / 60;
    if (hours < 1)
        hours = 1;

    unsigned long fee =
        cards[cardIndex].month ? 0 : hours * PRICE_PER_HOUR;

    sendMQTT(
        "OUT|" + uid + "|" + name + "|" +
        String(cards[cardIndex].month ? 1 : 0) + "|" +
        String(minutes) + "|" + String(fee)
    );

    clearSession(sessionIndex);

    openExitGate();
    beep(2);
    showExitInfo(name, minutes, fee);
    sendParkingStatus();
}

// ============================================================
//                    CARD / SESSION
// ============================================================
int findCard(const String& uid)
{
    for (int i = 0; i < cardCount; i++)
    {
        if (cards[i].uid == uid)
            return i;
    }
    return -1;
}

int findActiveSession(const String& uid)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (sessions[i].active && sessions[i].uid == uid)
            return i;
    }
    return -1;
}

int getActiveSessionCount()
{
    int count = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (sessions[i].active)
            count++;
    }
    return count;
}

bool createSession(const String& uid, const String& name)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (!sessions[i].active)
        {
            sessions[i].uid = uid;
            sessions[i].name = name;
            sessions[i].timeIn = time(nullptr);
            sessions[i].active = true;
            saveSessions();
            return true;
        }
    }
    return false;
}

void clearSession(int index)
{
    if (index < 0 || index >= MAX_SESSIONS)
        return;

    sessions[index].uid = "";
    sessions[index].name = "";
    sessions[index].timeIn = 0;
    sessions[index].active = false;
    saveSessions();
}

bool addNewCard(const String& uid, const String& name, bool month)
{
    if (findCard(uid) != -1)
    {
        beep(3);
        showLCD("THE DA TON TAI", "", 2000);
        return false;
    }

    if (cardCount >= MAX_CARDS)
    {
        beep(3);
        showLCD("DANH SACH DAY", "", 2000);
        return false;
    }

    cards[cardCount].uid = uid;
    cards[cardCount].name = name;
    cards[cardCount].month = month;
    cardCount++;

    saveCards();
    beep(2);
    showLCD("THEM THE OK", name, 2000);
    return true;
}

bool deleteCardByUID(const String& uid)
{
    int index = findCard(uid);

    if (index == -1)
    {
        beep(3);
        showLCD("THE KHONG TON TAI", "THU LAI", 2000);
        return false;
    }

    // if (findActiveSession(uid) != -1)
    //     return false;

    String deletedName = cards[index].name;

    for (int i = index; i < cardCount - 1; i++)
        cards[i] = cards[i + 1];

    cardCount--;
    cards[cardCount].uid = "";
    cards[cardCount].name = "";
    cards[cardCount].month = false;

    saveCards();
    beep(2);
    showLCD("XOA THE OK", deletedName, 2000);
    return true;
}

// ============================================================
//                    PREFERENCES
// ============================================================
void saveCards()
{
    preferences.putInt("cardCount", cardCount);

    for (int i = 0; i < MAX_CARDS; i++)
    {
        String keyUID = "u" + String(i);
        String keyName = "n" + String(i);
        String keyMonth = "m" + String(i);

        if (i < cardCount)
        {
            preferences.putString(keyUID.c_str(), cards[i].uid);
            preferences.putString(keyName.c_str(), cards[i].name);
            preferences.putBool(keyMonth.c_str(), cards[i].month);
        }
        else
        {
            preferences.remove(keyUID.c_str());
            preferences.remove(keyName.c_str());
            preferences.remove(keyMonth.c_str());
        }
    }
}

void loadCards()
{
    cardCount = preferences.getInt("cardCount", 0);

    if (cardCount < 0 || cardCount > MAX_CARDS)
        cardCount = 0;

    for (int i = 0; i < cardCount; i++)
    {
        String keyUID = "u" + String(i);
        String keyName = "n" + String(i);
        String keyMonth = "m" + String(i);

        // Tuong thich du lieu cu neu key moi chua ton tai
        cards[i].uid = preferences.getString(keyUID.c_str(), "");
        cards[i].name = preferences.getString(keyName.c_str(), "");
        cards[i].month = preferences.getBool(keyMonth.c_str(), false);

        if (cards[i].uid.length() == 0)
        {
            String oldUID = "uid" + String(i);
            String oldName = "name" + String(i);
            String oldMonth = "month" + String(i);

            cards[i].uid = preferences.getString(oldUID.c_str(), "");
            cards[i].name = preferences.getString(oldName.c_str(), "");
            cards[i].month = preferences.getBool(oldMonth.c_str(), false);
        }
    }

    Serial.print("So the da luu: ");
    Serial.println(cardCount);
}

void saveSessions()
{
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        String base = "s" + String(i);
        preferences.putString((base + "u").c_str(), sessions[i].uid);
        preferences.putString((base + "n").c_str(), sessions[i].name);
        preferences.putLong((base + "t").c_str(), (long)sessions[i].timeIn);
        preferences.putBool((base + "a").c_str(), sessions[i].active);
    }
}

void loadSessions()
{
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        String base = "s" + String(i);

        sessions[i].uid = preferences.getString((base + "u").c_str(), "");
        sessions[i].name = preferences.getString((base + "n").c_str(), "");
        sessions[i].timeIn = (time_t)preferences.getLong((base + "t").c_str(), 0);
        sessions[i].active = preferences.getBool((base + "a").c_str(), false);

        // Tuong thich du lieu session cu
        if (!sessions[i].active && sessions[i].uid.length() == 0)
        {
            sessions[i].uid = preferences.getString((base + "uid").c_str(), "");
            sessions[i].name = preferences.getString((base + "name").c_str(), "");
            sessions[i].timeIn = (time_t)preferences.getLong((base + "time").c_str(), 0);
            sessions[i].active = preferences.getBool((base + "active").c_str(), false);
        }
    }

    Serial.print("So xe dang trong bai: ");
    Serial.println(getActiveSessionCount());
}

void saveCapacity()
{
    preferences.putInt("capacity", parkingCapacity);
}

void loadCapacity()
{
    parkingCapacity = preferences.getInt("capacity", 2);

    if (parkingCapacity < 1 || parkingCapacity > MAX_SESSIONS)
        parkingCapacity = 2;

    int current = getActiveSessionCount();
    if (parkingCapacity < current)
        parkingCapacity = current;

    Serial.print("Suc chua: ");
    Serial.println(parkingCapacity);
}

// ============================================================
//                    GATE
// ============================================================
void openEntranceGate()
{
    servoIn.write(GATE_IN_OPEN_ANGLE);
    gateInOpen = true;
    gateInTimer = millis();
}

void openExitGate()
{
    servoOut.write(GATE_OUT_OPEN_ANGLE);
    gateOutOpen = true;
    gateOutTimer = millis();
}

void updateGate()
{
    if (gateInOpen)
    {
        if (entranceDetected)
        {
            gateInTimer = millis();
        }
        else if (millis() - gateInTimer >= GATE_OPEN_TIME)
        {
            servoIn.write(GATE_IN_CLOSE_ANGLE);
            gateInOpen = false;
        }
    }

    if (gateOutOpen)
    {
        if (exitDetected)
        {
            gateOutTimer = millis();
        }
        else if (millis() - gateOutTimer >= GATE_OPEN_TIME)
        {
            servoOut.write(GATE_OUT_CLOSE_ANGLE);
            gateOutOpen = false;
        }
    }
}

// ============================================================
//                    LCD / BUZZER
// ============================================================
void beep(int count)
{
    for (int i = 0; i < count; i++)
    {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(80);
        digitalWrite(BUZZER_PIN, LOW);
        delay(80);
    }
}

void showLCD(String line1, String line2, unsigned long duration)
{
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1.substring(0, 16));
    lcd.setCursor(0, 1);
    lcd.print(line2.substring(0, 16));
    messageUntil = millis() + duration;
}

void showExitInfo(const String& name, long minutes, unsigned long fee)
{
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(name.substring(0, 8));
    lcd.print(" ");
    lcd.print(minutes);
    lcd.print("p");

    lcd.setCursor(0, 1);
    lcd.print(fee);
    lcd.print(" VND");

    messageUntil = millis() + 4000;
}

void updateLCD()
{
    if (millis() < messageUntil)
        return;

    if (millis() - lastLCDUpdate < 1000)
        return;

    lastLCDUpdate = millis();

    int current = getActiveSessionCount();

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("BAI:");
    lcd.print(current);
    lcd.print("/");
    lcd.print(parkingCapacity);

    lcd.setCursor(10, 0);
    lcd.print(WiFi.status() == WL_CONNECTED ? "WIFI" : "OFF ");

    lcd.setCursor(0, 1);

    if (systemPaused)
    {
        lcd.print("TAM DUNG");
    }
    else if (current >= parkingCapacity)
    {
        lcd.print("BAI XE DA DAY");
    }
    else
    {
        lcd.print("CON TRONG:");
        lcd.print(parkingCapacity - current);
    }
}

void sendParkingStatus()
{
    String data =
        "PARKING|" +
        String(getActiveSessionCount()) + "|" +
        String(parkingCapacity);

    
    if (client.connected() && app_ok)
        client.publish(TOPIC_STATUS, data.c_str());
}
void checkEntranceSensor()
{
  if (
    entranceDetected &&
    !previousEntranceState
  )
 {     
    beep(1);
      showLCD(
        "CO XE DEN",
        "MOI QUET THE",
        3000
      );
    }
  previousEntranceState =
    entranceDetected;
}
void checkExitSensor()
{
   if (
    exitDetected &&
    !previousExitState
  )
  {
    beep(1);
    showLCD("XE RA","",3000);
    }
     previousExitState =
    exitDetected;
}
