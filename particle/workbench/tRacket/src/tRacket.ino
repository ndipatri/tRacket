#include <Adafruit_IO_Client.h>
#include "adafruit-ina219.h"
#include "LIS3DH.h"
#include "google-maps-device-locator.h"
#include <TinyGPS++/TinyGPS++.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_SPARK.h"

// GPS
SYSTEM_THREAD(ENABLED);
TinyGPSPlus gps;
// We only find one GPS fix during startup and then we shut down the GPS module.
bool foundAndReportedGPSFix = false;

// see https://github.com/particle-iot/google-maps-device-locator#firmware-library-api
GoogleMapsDeviceLocator googleMapsLocator;

// The current meter we will use to determine when battery needs to be
// recharged.
Adafruit_INA219 powerMeter;

// We use this to detect if the device is being tampered
LIS3DHI2C tamperMeter(Wire, 0, WKP);

// This can be retrieved from https://io.adafruit.com/ndipatri/profile
// (or if you aren't ndipatri, you can create an account for free)

// If you check in this code WITH this KEY defined, it will be detected by IO.Adafruit
// and the WILL DISABLE THIS KEY!!!  So please delete value below before checking in!
// ***************** !!!!!!!!!!!!!! **********
#define AIO_KEY         "aio_veFI12ZLFiXpgbvoZOBeHUw4dJRu" // Adafruit IO AIO Key
#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883                   // use 8883 for SSL
#define AIO_USERNAME    "ndipatri" 
// ***************** !!!!!!!!!!!!!! **********

TCPClient client; // TCP Client used by Adafruit IO library

// Create the AIO client object.. This knows how to push data to the
// Adafruit IO remote endpoint.. 
Adafruit_IO_Client aioClient = Adafruit_IO_Client(client, AIO_KEY);

Adafruit_MQTT_SPARK mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);


// These will automatically generate new feeds on the account defined by AIO_KEY
// https://io.adafruit.com/ndipatri/feeds/coopertownOccupiedT1
Adafruit_IO_Feed coopertownOccupiedFeed = aioClient.getFeed("coopertownOccupiedT1");
Adafruit_IO_Feed coopertownTamperFeed = aioClient.getFeed("coopertownTamperT1");
Adafruit_IO_Feed coopertownRechargeFeed = aioClient.getFeed("coopertownRechargeT1");

// Used by Google Location sevice which, given our GPS module, is redudant.
Adafruit_IO_Feed coopertownLocationFeed = aioClient.getFeed("coopertownLocationT1");

// We need to use MQTT protocol to transfer lat/lng information to Adafruit
// The '/csv' postfix is magic that treat the incoming data as a single entry
// So pushing data with format 'dataValue, lat, long, elevation' will result in a single
// data point (which is ignored), but it will have lat/long meta data attached.
Adafruit_MQTT_Publish coopertownLatLongMQTTPublish = Adafruit_MQTT_Publish(&mqtt,  AIO_USERNAME "/feeds/coopertownLocation/csv");

// Chip enabl for GPS module.
int GPS_MODULE_ENABLE_OUTPUT_PIN = D6; 

// The sensor is configured NC (normally closed)
int MOTION_SENSOR_DETECTED_INPUT_PIN = D8; 

// We want to indicate when we are actively listening to sensor input
int MOTION_LISTENING_LED_OUTPUT_PIN = D7; 

// At this voltage, the battery should be recharged.
float BATTERY_CHARGE_THRESHOLD_VOLTS = 12.2;
bool batteryNeedsCharging = false;
double batteryVoltageLevel = 0.0;

// If a monitored value is triggered we latch it for this time period so
// external monitors have time to capture it
long TRIGGER_SUSTAIN_INTERVAL_MILLIS = 10000;

long lastTamperTimeMillis = -1L;
long lastMotionTimeMillis = -1L;
int lastTamperSample = 0;

long MOTION_POLL_DURATION_MINUTES = 3;
long TAMPER_POLL_DURATION_MINUTES = 1;

void setup() {

    Serial.begin(115200);

    // The Adafruit GPS module is connected to Serial1 and D6
    Serial1.begin(9600);

    // Settings D6 LOW powers up the GPS module
    pinMode(GPS_MODULE_ENABLE_OUTPUT_PIN, OUTPUT);
    digitalWrite(GPS_MODULE_ENABLE_OUTPUT_PIN, LOW);

    // This device is stationary.
    googleMapsLocator.withSubscribe(locationCallback).withLocateOnce();

    // For accelerometer
	Wire.setSpeed(CLOCK_SPEED_100KHZ);
	Wire.begin();
	LIS3DHConfig config;
	config.setAccelMode(LIS3DH::RATE_100_HZ);
	bool setupSuccess = tamperMeter.setup(config);

    powerMeter.begin();
    powerMeter.setCalibration_16V_400mA();

    Particle.variable("minutesSinceLastMotion", minutesSinceLastMotion);
    Particle.variable("batteryVoltageLevel", batteryVoltageLevel);

    pinMode(MOTION_SENSOR_DETECTED_INPUT_PIN, INPUT_PULLUP);
    pinMode(MOTION_LISTENING_LED_OUTPUT_PIN, OUTPUT);

    resetTamper();
    resetMotion();
    checkBattery(true);
}

void locationCallback(float lat, float lon, float accuracy) {
    coopertownLocationFeed.send("Location acquired: " + String(lat) + ", " + String(lon));
}

void loop() {

    checkForAndPublishGPSFix();

    googleMapsLocator.loop();

    if (PLATFORM_ID != PLATFORM_ARGON && PLATFORM_ID != PLATFORM_BORON) {
        Particle.publish("TERMINAL", "Must be run on Argon or Boron", 60, PUBLIC);

        delay(60000);
        return;
    }

    if (PLATFORM_ID == PLATFORM_ARGON) {
        Particle.publish("activityReport", "awake", 60, PUBLIC);
    }

    digitalWrite(MOTION_LISTENING_LED_OUTPUT_PIN, HIGH);

    long startTime = millis();

    // stay awake until we hear motion/tamper or we reach MOTION_POLL_DURATION_MINUTES
    bool isMotionDetectedNow = false;
    bool isTamperDetectedNow = false;
    while(!isTamperDetectedNow &&
          !isMotionDetectedNow && 
          ((millis() - startTime) < (MOTION_POLL_DURATION_MINUTES*60000))) {

        isTamperDetectedNow = checkForTamper();

        isMotionDetectedNow = checkForMotion();

        // when triggered, motion sensor only indicates for 3 seconds...
        // so our poll interval has to be at least twice as fast (Nyquist Interval!)
        delay(500);
    }
    
    if (isMotionDetectedNow) {

        // We need to make sure our motion wasn't triggered due to a tamper.
        startTime = millis();
        bool isTamperDetectedNow = false;
        while(!isTamperDetectedNow &&
              ((millis() - startTime) < (TAMPER_POLL_DURATION_MINUTES*60000))) {

            isTamperDetectedNow = checkForTamper();
            delay(500);
        }
    }

    // See if we need to clear our latches...
    if (isTamperDetected() && 
        (millis() - lastTamperTimeMillis) > TRIGGER_SUSTAIN_INTERVAL_MILLIS) {

        // It's been long enough, we can clear out this trigger    
        resetTamper();
    }

    if (isMotionDetected() && 
        (millis() - lastMotionTimeMillis) > TRIGGER_SUSTAIN_INTERVAL_MILLIS) {

        // It's been long enough, we can clear out this trigger    
        resetMotion();
    }

    digitalWrite(MOTION_LISTENING_LED_OUTPUT_PIN, LOW);

    checkBattery(false);

    if (PLATFORM_ID == PLATFORM_ARGON) {
        delay(20000);
    } else {
        // assume this is Boron.. PLATFORM_BORON didn't seem to work properly
        // see https://docs.particle.io/reference/device-os/firmware/boron/#sleep
        SystemSleepConfiguration config;
        config
            .mode(SystemSleepMode::STOP)
            .network(NETWORK_INTERFACE_CELLULAR)
            .flag(SystemSleepFlag::WAIT_CLOUD)
            .duration(5min);
        System.sleep(config);
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

    coopertownLatLongMQTTPublish.publish("0," + String(lat) + "," + String(lng) + ",10");

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

bool checkForTamper() {
    bool isTamperDetectedNow = measureForTamper();
    if (isTamperDetectedNow) {
        if (PLATFORM_ID == PLATFORM_ARGON) {
            Particle.publish("activityReport", "tamperDetected", 60, PUBLIC);
        }

        if (!isTamperDetected()) {
            // We only want to send positive edge to feed. 
            coopertownTamperFeed.send(true); 
        }

        lastTamperTimeMillis = millis();
    }

    return isTamperDetectedNow;
}

bool checkForMotion() {

    bool isMotionDetectedNow = digitalRead(MOTION_SENSOR_DETECTED_INPUT_PIN) == LOW;
    if (isMotionDetectedNow) {
        if (PLATFORM_ID == PLATFORM_ARGON) {
            Particle.publish("activityReport", "motionDetected", 60, PUBLIC);
        }

        if (!isMotionDetected()) {
            // We only want to send positive edge to feed. 
            coopertownOccupiedFeed.send(true); 
        }

        lastMotionTimeMillis = millis();
    }

    return isMotionDetectedNow;
}

void resetTamper() {
    lastTamperTimeMillis = -1L;
    coopertownTamperFeed.send(false); 
    if (PLATFORM_ID == PLATFORM_ARGON) {
        Particle.publish("activityReport", "tamperReset", 60, PUBLIC);
    }
}

void resetMotion() {
    lastMotionTimeMillis = -1L;
    coopertownOccupiedFeed.send(false); 
    if (PLATFORM_ID == PLATFORM_ARGON) {
        Particle.publish("activityReport", "motionReset", 60, PUBLIC);
    }
}

bool isTamperDetected() {
    return lastTamperTimeMillis > 0;
}

bool isMotionDetected() {
    return lastMotionTimeMillis > 0;
}

int minutesSinceLastMotion() {
    return (int)((millis() - lastMotionTimeMillis)/60000L);
}

void checkBattery(bool force) {
    float shuntvoltage = powerMeter.getShuntVoltage_mV();
    float busvoltage = powerMeter.getBusVoltage_V();
    batteryVoltageLevel = busvoltage + (shuntvoltage / 1000);

    bool batteryNeedsChargingNow = false;
    if (batteryVoltageLevel <= BATTERY_CHARGE_THRESHOLD_VOLTS) {
        batteryNeedsChargingNow = true;
    }

    if (force || (batteryNeedsChargingNow != batteryNeedsCharging)) {
        batteryNeedsCharging = batteryNeedsChargingNow;

        coopertownRechargeFeed.send(batteryNeedsCharging ? "RECHARGE" : "GOOD"); 
    }
}

bool measureForTamper() {

    bool tampered = false;

    LIS3DHSample sample;
    if (tamperMeter.getSample(sample)) {

        int tamperSample = abs(sample.x + sample.y + sample.z);

        if (lastTamperSample > 0) {
            int tamperChange = (abs(tamperSample - lastTamperSample)/(float)lastTamperSample)*100.0;

            if (PLATFORM_ID == PLATFORM_ARGON) {
                //Particle.publish("tamperSample", "tamperChange=" + String(tamperChange), 60, PUBLIC);
            }

            tampered = tamperChange > 30; // change percent
        }

        lastTamperSample = tamperSample;
    } else {
        Serial.println("no sample");
    }

    return tampered;
}