#include <Adafruit_IO_Client.h>
#include "adafruit-ina219.h"
#include <TinyGPS++/TinyGPS++.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_SPARK.h"

// GPS
SYSTEM_THREAD(ENABLED);
TinyGPSPlus gps;

// We only find one GPS fix during startup and then we shut down the GPS module.
bool foundAndReportedGPSFix = false;

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

// Create the AIO client object.. This knows how to push data to the
// Adafruit IO remote endpoint.. 
Adafruit_IO_Client aioClient = Adafruit_IO_Client(client, AIO_KEY);

Adafruit_MQTT_SPARK mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);

// These will automatically generate new feeds on the account defined by AIO_KEY
// https://io.adafruit.com/ndipatri/feeds/coopertownOccupiedT1

// NJD TODO - For now push de-normalized data to one feed so it's easier for client..
// eventually should push to separate occupancy feed per device
String occupiedFeedName = "Occupancy"; 
Adafruit_IO_Feed occupancyFeed = aioClient.getFeed(occupiedFeedName);

String rechargeFeedName = System.deviceID() + "Recharge"; 
Adafruit_IO_Feed rechargeFeed = aioClient.getFeed(rechargeFeedName);

// We need to use MQTT protocol to transfer lat/lng information to Adafruit
// The '/csv' postfix is magic that treat the incoming data as a single entry
// So pushing data with format 'dataValue, lat, long, elevation' will result in a single
// data point (which is ignored), but it will have lat/long meta data attached.
String mqttFeedName = AIO_USERNAME + "/feeds/Location/csv"; 
Adafruit_MQTT_Publish locationMQTTTopic = Adafruit_MQTT_Publish(&mqtt,  mqttFeedName);


// Chip enabl for GPS module.
int GPS_MODULE_ENABLE_OUTPUT_PIN = D6; 

// The sensor is configured NC (normally closed)
int MOTION_SENSOR_DETECTED_INPUT_PIN = D8; 
int MOTION_SENSOR_LED_ENABLE_OUTPUT_PIN = D4; 

// Keep track of when this device was powered up
long startTimeMillis;
// How long we keep the device in test mode after startup
long STARTUP_TEST_MODE_PERIOD_MINUTES = 15;

// We always enter test mode upon startup and we want to expire
// that particular test session
bool inStartupTestPeriod = true;

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
long TRIGGER_SUSTAIN_INTERVAL_MINUTES = 6;

long lastMotionTimeMillis = -1L;

long MOTION_POLL_DURATION_MINUTES = 3;

bool firstPass = true;

// Will remain empty until we get a callback from the cloud shortly
// after startup
String deviceName = "";

void setup() {
    
    Serial.begin(115200);

    // The Adafruit GPS module is connected to Serial1 and D6
    Serial1.begin(9600);

    // Settings D6 LOW powers up the GPS module
    pinMode(GPS_MODULE_ENABLE_OUTPUT_PIN, OUTPUT);
    digitalWrite(GPS_MODULE_ENABLE_OUTPUT_PIN, LOW);

    // By default, LED on OFF when motion is triggered
    pinMode(MOTION_SENSOR_LED_ENABLE_OUTPUT_PIN, OUTPUT);
    digitalWrite(MOTION_SENSOR_LED_ENABLE_OUTPUT_PIN, LOW);

    // For accelerometer
	Wire.setSpeed(CLOCK_SPEED_100KHZ);
	Wire.begin();

    powerMeter.begin();
    powerMeter.setCalibration_16V_400mA();

    Particle.variable("minutesSinceLastMotion", minutesSinceLastMotion);
    Particle.variable("batteryVoltageLevel", batteryVoltageLevel);
    Particle.variable("isInTestMode", isInTestMode);

    Particle.function("turnOnTestMode", turnOnTestMode);
    Particle.function("turnOffTestMode", turnOffTestMode);

    pinMode(MOTION_SENSOR_DETECTED_INPUT_PIN, INPUT_PULLUP);
    pinMode(MOTION_LISTENING_LED_OUTPUT_PIN, OUTPUT);

    resetMotion();

    // We always enter test mode upon startup and we want to expire
    // that particular test session
    turnOnTestMode("");
    startTimeMillis = millis();
    inStartupTestPeriod = true;

}

void loop() {
    checkForDeviceName();

    checkForAndPublishGPSFix();

    if (PLATFORM_ID != PLATFORM_ARGON && PLATFORM_ID != PLATFORM_BORON) {
        Particle.publish("TERMINAL", "Must be run on Argon or Boron", 60, PUBLIC);

        delay(60000);
        return;
    }

    publishParticleLog("activityReport", "awake");

    digitalWrite(MOTION_LISTENING_LED_OUTPUT_PIN, HIGH);

    long pollIntervalStartTimeMillis = millis();

    // stay awake until we hear motion or we reach MOTION_POLL_DURATION_MINUTES
    bool isMotionDetectedNow = false;
    while(!isMotionDetectedNow && 
          ((millis() - pollIntervalStartTimeMillis) < (MOTION_POLL_DURATION_MINUTES*60000))) {

        isMotionDetectedNow = checkForMotion();

        if (isMotionDetectedNow) {
            publishParticleLog("activityReport", "motionDetected");

            if (!currentlyInMotionPeriod()) {

                // Now starting new motion period
                if (deviceName.equals("")) {
                    publishParticleLog("activityReport", "startMotionPeriod(noDeviceNameYet)");
                } else {
                    publishParticleLog("activityReport", "startMotionPeriod(" + String(deviceName) + ")");
                    occupancyFeed.send(String(deviceName) + ":1");
                }
            }

            // Detecting new motion extends our current motion period
            lastMotionTimeMillis = millis();
        }

        // when triggered, motion sensor only indicates for 3 seconds...
        // so our poll interval has to be at least twice as fast (Nyquist Interval!)
        delay(1000);
    }
    
    if (currentlyInMotionPeriod() && 
        !isMotionDetectedNow &&
        (millis() - lastMotionTimeMillis) > (TRIGGER_SUSTAIN_INTERVAL_MINUTES * 60000)) {

        // We need to have a 'minimum motion period' so it can be picked up by
        // external monitoring    
        resetMotion();

        if (deviceName.equals("")) {
            publishParticleLog("activityReport", "endMotionPeriod(noDeviceNameYet");
        } else {
            publishParticleLog("activityReport", "endMotionPeriod(" + String(deviceName) + ")");
            occupancyFeed.send(String(deviceName) + ":0");
        }
    }

    checkBattery(firstPass);
    firstPass = false;

    expireStartupTestModeIfNecessary();

    digitalWrite(MOTION_LISTENING_LED_OUTPUT_PIN, LOW);

    if (PLATFORM_ID == PLATFORM_ARGON || isInTestMode()) {
        delay(1000);
    } else {
        // assume this is Boron.. PLATFORM_BORON didn't seem to work properly
        // see https://docs.particle.io/reference/device-os/firmware/boron/#sleep
        SystemSleepConfiguration config;
        config
            // processor goes to sleep to save energy
            .mode(SystemSleepMode::STOP)

            // keeps cellular running when sleeping processor, and also lets
            // network be a wake source
            .network(NETWORK_INTERFACE_CELLULAR)

            // make sure all cloud events have been acknowledged before sleeping
            .flag(SystemSleepFlag::WAIT_CLOUD)
            .duration(1min);
        System.sleep(config);
    }
}

void checkForDeviceName() {
    if (deviceName == "") {
        Particle.subscribe("particle/device/name", nameHandler);
        Particle.publish("particle/device/name");
    }
}

void nameHandler(const char *topic, const char *data) {
    publishParticleLog("activityReport", "deviceNameReceived:" + String(data));
    deviceName = String(data);    
}

void expireStartupTestModeIfNecessary() {
    if (inStartupTestPeriod &&
        millis() - startTimeMillis > (STARTUP_TEST_MODE_PERIOD_MINUTES * 60000)) {

        publishParticleLog("activityReport", "endStartupTestMode");

        inStartupTestPeriod = false;
        turnOffTestMode("");
    }
}

void checkForAndPublishGPSFix() {
    if (!foundAndReportedGPSFix) {

        // check for available data from GPS module
        while (Serial1.available() > 0) {
            if (gps.encode(Serial1.read())) {

                // Valid data received from GPS module ...    
                if (gps.location.isValid()) {

                    Particle.publish("gpsUpdate", "lat:" + String(gps.location.lat()) + ", lon:" + String(gps.location.lng()), 60, PUBLIC);
                    foundAndReportedGPSFix = true;

                    sendLatLongToCloud(gps.location.lat(), gps.location.lng());

                    // Turn off GPS module
                    digitalWrite(GPS_MODULE_ENABLE_OUTPUT_PIN, HIGH);
                }
            }
        }
    }
}

void sendLatLongToCloud(double lat, double lng) {

    Particle.publish("gpsUpdate", "Connecting to MQTT Server", 60, PUBLIC);
    MQTTconnect();

    String deviceIdString = System.deviceID();
    locationMQTTTopic.publish(deviceIdString + "," + String(lat) + "," + String(lng) + ",10");

    mqtt.disconnect();
}

void MQTTconnect() {
  int8_t ret;

  // Stop if already connected.
  if (mqtt.connected()) {
    return;
  }

  Particle.publish("gpsUpdate", "Connecting to Adafruit IO MQTT Server", 60, PUBLIC);

  while ((ret = mqtt.connect()) != 0) { // connect will return 0 for connected
       Particle.publish("gpsUpdate", "Retrying MQTT connect from error: " + String(mqtt.connectErrorString(ret)), 60, PUBLIC);
       mqtt.disconnect();
       delay(5000);  // wait 5 seconds
  }
  Particle.publish("gpsUpdate", "MQTT Connected!", 60, PUBLIC);
}

bool checkForMotion() {
    return digitalRead(MOTION_SENSOR_DETECTED_INPUT_PIN) == LOW;
}

void resetMotion() {
    lastMotionTimeMillis = -1L;
}

bool currentlyInMotionPeriod() {
    return lastMotionTimeMillis > 0;
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

        sendBatteryStatus(batteryNeedsCharging, batteryVoltageLevel);
    }
}

void sendBatteryStatus(bool batteryNeedsCharging, float batteryVoltageLevel) {
    publishParticleLog("checkBattery", "batteryVoltageLevel=" + String(batteryVoltageLevel));

    rechargeFeed.send((batteryNeedsCharging ? "RECHARGE" : "GOOD") + String("(") + String(batteryVoltageLevel) + String(")"));
}

int turnOnTestMode(String _na) {

    Particle.publish("config", "testMode turned ON", 60, PUBLIC);
    digitalWrite(MOTION_SENSOR_LED_ENABLE_OUTPUT_PIN, HIGH);

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