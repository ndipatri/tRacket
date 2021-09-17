#include <Adafruit_IO_Client.h>
#include "adafruit-ina219.h"
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_SPARK.h"
#include "DeviceNameHelperRK.h"

// The current meter we will use to determine when battery needs to be
// recharged.
Adafruit_INA219 powerMeter;

// This can be retrieved from https://io.adafruit.com/ndipatri/profile
// (or if you aren't ndipatri, you can create an account for free)

// If you check in this code WITH this KEY defined, it will be detected by IO.Adafruit
// and the WILL DISABLE THIS KEY!!!  So please delete value below before checking in!
// ***************** !!!!!!!!!!!!!! **********
#define AIO_KEY         "aio_XmSy92vYjddoZhQ4f19ztFc8Qm6r" // Adafruit IO AIO Key
#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883                   // use 8883 for SSL
String AIO_USERNAME     = "ndipatri";
// ***************** !!!!!!!!!!!!!! **********

TCPClient client; // TCP Client used by Adafruit IO library

Adafruit_MQTT_SPARK mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);

String mqttOccupancyTopicName = AIO_USERNAME + "/feeds/occupancy"; 
Adafruit_MQTT_Publish occupancyMQTTTopic = Adafruit_MQTT_Publish(&mqtt,  mqttOccupancyTopicName);

String mqttRechargeTopicName = AIO_USERNAME + "/feeds/recharge"; 
Adafruit_MQTT_Publish rechargeMQTTTopic = Adafruit_MQTT_Publish(&mqtt,  mqttRechargeTopicName);

Adafruit_MQTT_Subscribe errors = Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME + "/errors");
Adafruit_MQTT_Subscribe throttle = Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME + "/throttle");

// The sensor is configured NC (normally closed)
int MOTION_SENSOR_DETECTED_INPUT_PIN = D8; 
int MOTION_SENSOR_LED_ENABLE_OUTPUT_PIN = D4; 

// How long we keep the device in test mode after startup
long STARTUP_TEST_MODE_PERIOD_MINUTES = 15;
long testModeStartTimeMillis = -1;

// We want to indicate when we are actively listening to sensor input
int MOTION_LISTENING_LED_OUTPUT_PIN = D7; 

// We need a HIGH and LOW value to implement a 'Schmitt Trigger' for
// voltage measurement.  Otherwise, we get oscilation of value around
// a single threshold.
float BATTERY_CHARGE_THRESHOLD_VOLTS_HIGH = 12.4;
float BATTERY_CHARGE_THRESHOLD_VOLTS_LOW = 11.9;

bool batteryNeedsCharging = false;
double batteryVoltageLevel = 0.0;

// If a monitored value is triggered we latch it for this time period so
// external monitors have time to capture it
//
// This also has the effect of maintaining the 'occupied' state for the entire
// duration a court is in use... it's a sort of low-pass filter
long MOTION_PERIOD_INTERVAL_MINUTES = 6;

long lastMotionTimeMillis = -1L;

long MOTION_POLL_DURATION_MINUTES = 3;

bool firstPass = true;

// For deviceName retrieval and storage
SYSTEM_THREAD(ENABLED);
retained DeviceNameHelperData deviceNameHelperRetained;

int SLEEP_START_HOUR = 21; // 9pm
int SLEEP_STOP_HOUR = 6; // 6am duh.

// Keeps track of the last time we synchronized our wall clock with
// the server
int lastClockSyncMillis = 0;

void setup() {
    
    Serial.begin(115200);

    // This helps with retrieving Device name
    DeviceNameHelperRetained::instance().setup(&deviceNameHelperRetained);
    DeviceNameHelperRetained::instance().withCheckPeriod(24h);

    // By default, LED on OFF when motion is triggered
    pinMode(MOTION_SENSOR_LED_ENABLE_OUTPUT_PIN, OUTPUT);
    digitalWrite(MOTION_SENSOR_LED_ENABLE_OUTPUT_PIN, LOW);

    powerMeter.begin();
    powerMeter.setCalibration_16V_400mA();

    Particle.variable("minutesSinceLastMotion", minutesSinceLastMotion);
    Particle.variable("batteryVoltageLevel", batteryVoltageLevel);
    Particle.variable("isInTestMode", isInTestMode);

    Particle.function("turnOnTestMode", turnOnTestMode);
    Particle.function("turnOffTestMode", turnOffTestMode);

    pinMode(MOTION_SENSOR_DETECTED_INPUT_PIN, INPUT_PULLUP);
    pinMode(MOTION_LISTENING_LED_OUTPUT_PIN, OUTPUT);

    stopMotionPeriod();

    // NJD TODO - If this were to move elsewhere, we could support something more
    // dynamic.. but this is ok for now.
    // Set time zone to Eastern USA daylight saving time
    Time.zone(-4);

    // We always enter test mode upon startup and we want to expire
    // that particular test session
    turnOnTestMode("");
}


void loop() {

    if (handleIncorrectDevice()) {
        return;
    }
    
    // so everyone knows we're awake!
    digitalWrite(MOTION_LISTENING_LED_OUTPUT_PIN, HIGH);

    // Check to see if we know ou device name yet...
    DeviceNameHelperRetained::instance().loop();

    // We need an accurate clock for when we go to sleep
    // NJD Disabling thing temporarily as I try to track down what's 
    // causing device to have issues...
    //syncWallClock();

    if (!deviceNameFound()) {
        sleepForSeconds(5);

        return;
    }

    // this can take minutes to execute
    if (checkForMotion()) {
        publishParticleLog("activityReport", "motionDetected");

        if (!currentlyInMotionPeriod()) {

            // Starting new motion period
            publishParticleLog("activityReport", "startMotionPeriod(" + String(getDeviceName()) + ")");
            sendOccupiedToCloud();
        }

        // Detecting motion either starts or extends our current motion period
        startMotionPeriod();
    } else {

        // no current motion.. see if our motion period has expired yet...
        if (hasMotionPeriodExpired()) {

            stopMotionPeriod();

            publishParticleLog("activityReport", "endMotionPeriod(" + String(getDeviceName()) + ")");
            sendUnOccupiedToCloud();
        }
    }

    checkBattery(firstPass);
    firstPass = false;

    expireStartupTestModeIfNecessary();

    // so everyone knows we're going to sleep!
    digitalWrite(MOTION_LISTENING_LED_OUTPUT_PIN, LOW);

    sleepForSeconds(2);
}




// this can take minutes to execute
bool checkForMotion() {
    long pollIntervalStartTimeMillis = millis();

    // check for a bit of time
    while((millis() - pollIntervalStartTimeMillis) < (MOTION_POLL_DURATION_MINUTES*60000)) {

        if (digitalRead(MOTION_SENSOR_DETECTED_INPUT_PIN) == LOW) {
            return true;
        }

        // when triggered, motion sensor only indicates for 3 seconds...
        // so our poll interval has to be at least twice as fast (Nyquist Interval!)
        delay(1000);
    }

    return false;
}

bool handleIncorrectDevice() {
    if (PLATFORM_ID != PLATFORM_ARGON && PLATFORM_ID != PLATFORM_BORON) {
        Particle.publish("TERMINAL", "Must be run on Argon or Boron", 60, PUBLIC);

        sleepForSeconds(60);

        return true;
    }

    return false;
}

void sleepForSeconds(int seconds) {
    publishParticleLog("activityReport", "sleep");
    delay(seconds * 1000);
    publishParticleLog("activityReport", "awake");
}

bool shouldBeAsleepForNight() {
    int currentHour = Time.hour();

    return ((currentHour >= SLEEP_START_HOUR) ||
            (currentHour < SLEEP_STOP_HOUR));
}

void syncWallClock() {
    // manage wall clock.. since this device runs constantly, its clock can
    // drift.
    if (millis() - lastClockSyncMillis > 1000 * 60 * 60 * 24) { // once a day
        // Request time synchronization from the Particle Device Cloud
        publishParticleLog("syncWallClock", "syncTime!");
        Particle.syncTime();
        lastClockSyncMillis = millis();
    }
}

bool deviceNameFound() {
    return DeviceNameHelperRetained::instance().hasName();
}

char const* getDeviceName() {
    return DeviceNameHelperRetained::instance().getName();
}

void expireStartupTestModeIfNecessary() {
    if (isInTestMode() &&
        millis() - testModeStartTimeMillis > (STARTUP_TEST_MODE_PERIOD_MINUTES * 60000)) {

        publishParticleLog("activityReport", "endStartupTestMode");

        turnOffTestMode("");
    }
}

void sendOccupiedToCloud() {
    sendMessageToCloud(String(getDeviceName()) + ":1", &occupancyMQTTTopic);
}

void sendUnOccupiedToCloud() {
    sendMessageToCloud(String(getDeviceName()) + ":0", &occupancyMQTTTopic);
}

void sendBatteryStatusToCloud(bool batteryNeedsCharging, float batteryVoltageLevel) {

    String message = String(getDeviceName()) + ":" + (batteryNeedsCharging ? "RECHARGE" : "GOOD") + String("(") + String(batteryVoltageLevel) + String(")");

    sendMessageToCloud(message, &rechargeMQTTTopic);
}

void sendMessageToCloud(const char* message, Adafruit_MQTT_Publish* topic) {
    MQTTConnect();

    if (topic->publish(message)) {
        publishParticleLog("mqtt", "Message SUCCESS (" + String(message) + ").");
    } else {
        publishParticleLog("mqtt", "Message FAIL (" + String(message) + ").");
    }

    // this is our 'wait for incoming subscription packets' busy subloop
    // try to spend your time here
    Adafruit_MQTT_Subscribe *subscription;
    while ((subscription = mqtt.readSubscription(5000))) {
        if(subscription == &errors) {
            publishParticleLog("mqtt", "Error: (" + String((char *)errors.lastread) + ").");
        } else if(subscription == &throttle) {
            publishParticleLog("mqtt", "Throttle: (" + String((char *)throttle.lastread) + ").");
        }
    }

    MQTTDisconnect();
}


void MQTTConnect() {
    int8_t ret;

    // Stop if already connected.
    if (mqtt.connected()) {
        return;
    }

    while ((ret = mqtt.connect()) != 0) { // connect will return 0 for connected
        publishParticleLog("mqtt", "Retrying MQTT connect from error: " + String(mqtt.connectErrorString(ret)));
        mqtt.disconnect();
        delay(5000);  // wait 5 seconds
    }
}

void MQTTDisconnect() {
    mqtt.disconnect();
}

void startMotionPeriod() {
    lastMotionTimeMillis = millis();
}

void stopMotionPeriod() {
    lastMotionTimeMillis = -1L;
}

bool currentlyInMotionPeriod() {
    return lastMotionTimeMillis > 0;
}

bool hasMotionPeriodExpired() {
    return currentlyInMotionPeriod() &&
           ((millis() - lastMotionTimeMillis) > (MOTION_PERIOD_INTERVAL_MINUTES * 60000));
}

int minutesSinceLastMotion() {
    return (int)((millis() - lastMotionTimeMillis)/60000L);
}

void checkBattery(bool forceSendUpdate) {
    float shuntvoltage = powerMeter.getShuntVoltage_mV();
    float busvoltage = powerMeter.getBusVoltage_V();
    batteryVoltageLevel = busvoltage + (shuntvoltage / 1000);

    // We implement a Schmitt Trigger.  The two thresholds prevent 'oscillating'
    // that occurs with a single threshold.
    bool currentBatteryNeedsCharging = batteryNeedsCharging;

    if (batteryNeedsCharging && (batteryVoltageLevel > BATTERY_CHARGE_THRESHOLD_VOLTS_HIGH)) {
        currentBatteryNeedsCharging = false;
    }

    if (!batteryNeedsCharging && (batteryVoltageLevel < BATTERY_CHARGE_THRESHOLD_VOLTS_LOW)) {
        currentBatteryNeedsCharging = true;
    }

    if (forceSendUpdate || (currentBatteryNeedsCharging != batteryNeedsCharging)) {
        batteryNeedsCharging = currentBatteryNeedsCharging;

        sendBatteryStatusToCloud(batteryNeedsCharging, batteryVoltageLevel);
    }
}

int turnOnTestMode(String _na) {

    Particle.publish("config", "testMode turned ON", 60, PUBLIC);
    digitalWrite(MOTION_SENSOR_LED_ENABLE_OUTPUT_PIN, HIGH);

    testModeStartTimeMillis = millis();

    return 1;
}

int turnOffTestMode(String _na) {

    Particle.publish("config", "testMode turned OFF", 60, PUBLIC);
    digitalWrite(MOTION_SENSOR_LED_ENABLE_OUTPUT_PIN, LOW);

    return 1;
}

bool isInTestMode() {
    return digitalRead(MOTION_SENSOR_LED_ENABLE_OUTPUT_PIN) == HIGH;
}

void publishParticleLog(String group, String message) {
    if (PLATFORM_ID == PLATFORM_ARGON || isInTestMode()) {
        Particle.publish(group, message, 60, PUBLIC);
    }
}