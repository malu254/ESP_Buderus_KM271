
// Library Settings
#define SSE_MAX_QUEUED_MESSAGES 256      // ESPAsyncWebServer: max number of queued SSE messages
#define CONFIG_ASYNC_TCP_QUEUE 256       // AsyncTCP: max number of queued messages
#define CONFIG_ASYNC_TCP_STACK_SIZE 6144 // AsyncTCP: stack size
#define ESP_DRD_USE_LITTLEFS true        // DRD: use LittleFS
#define DOUBLERESETDETECTOR_DEBUG true   // DRD: debug serial outputs
#define DRD_TIMEOUT 3                    // DRD: timeout for double reset detection
#define DRD_ADDRESS 0                    // DRD: FLASH offset not used > LittleFS

// includes
#include <ArduinoOTA.h>
#include <ESP_DoubleResetDetector.h>
#include <basics.h>
#include <config.h>
#include <km271.h>
#include <message.h>
#include <mqtt.h>
#include <oilmeter.h>
#include <sensor.h>
#include <simulation.h>
#include <telnet.h>
#include <webUI.h>

/* D E C L A R A T I O N S ****************************************************/
muTimer mainTimer = muTimer();      // timer for cyclic info
muTimer heartbeat = muTimer();      // timer for heartbeat signal
muTimer setupModeTimer = muTimer(); // timer for heartbeat signal
muTimer dstTimer = muTimer();       // timer to check daylight saving time change
muTimer ntpTimer = muTimer();       // timer to check ntp sync

DoubleResetDetector *drd; // Double-Reset-Detector
bool main_reboot = true;  // reboot flag
int dst_old;              // reminder for change of daylight saving time
bool dst_ref;             // init flag fpr dst reference
bool ntpSynced;           // ntp sync flag
bool ntpInit = false;     // init flag for ntp sync

/**
 * *******************************************************************
 * @brief   function layer to store data before update or reboot
 * @param   none
 * @return  none
 * *******************************************************************/
void storeData() {
  if (config.oilmeter.use_hardware_meter) {
    cmdStoreOilmeter();
  }
}

/**
 * *******************************************************************
 * @brief   Main Setup routine
 * @param   none
 * @return  none
 * *******************************************************************/
void setup() {
  // Enable serial port
  Serial.begin(115200);

  // check for double reset
  drd = new DoubleResetDetector(DRD_TIMEOUT, DRD_ADDRESS);
  if (drd->detectDoubleReset()) {
    setupMode = true;
  }

  // initial configuration
  configSetup();

  // basic setup functions
  basicSetup();

  // Setup OTA
  ArduinoOTA.onStart([]() {
    // actions to do when OTA starts
    storeData(); // store Data before update
    delay(500);
  });

  ArduinoOTA.setHostname(config.wifi.hostname);
  ArduinoOTA.begin();

  // MQTT
  if (config.mqtt.enable && !setupMode) {
    mqttSetup();
  }

  // send initial WiFi infos
  if (!setupMode) {
    sendWiFiInfo();
  }

  // setup for km271
  if (!setupMode) {
    km271ProtInit(config.gpio.km271_RX, config.gpio.km271_TX);
  }

  // setup Oilmeter
  if (config.oilmeter.use_hardware_meter && !setupMode) {
    setupOilmeter();
  }

  // webUI Setup
  if (config.webUI.enable) {
    webUISetup();
  }

  // Sensor Setup
  setupSensor();

  // Message Service Setup
  messageSetup();

  // telnet Setup
  setupTelnet();
}

/**
 * *******************************************************************
 * @brief   Main Loop
 * @param   none
 * @return  none
 * *******************************************************************/
void loop() {
  // OTA Update
  ArduinoOTA.handle();

  // double reset detector
  drd->loop();

  // check WiFi - automatic reconnect
  if (!setupMode) {
    checkWiFi();
  }

  // check MQTT - automatic reconnect
  if (config.mqtt.enable && !setupMode) {
    checkMqtt();
  }

  if (setupMode) {
    // LED to Signal Setup-Mode
    digitalWrite(LED_BUILTIN, setupModeTimer.cycleOnOff(100, 500));
    digitalWrite(21, setupModeTimer.cycleOnOff(500, 100));
  } else {
    // LED for WiFi connected
    if (config.gpio.led_wifi != -1)
      digitalWrite(config.gpio.led_wifi, WiFi.status() != WL_CONNECTED); // (true=LED off)
    // LED for KM271 LogMode active
    if (config.gpio.led_logmode != -1)
      digitalWrite(config.gpio.led_logmode, !km271GetLogMode()); // (true=LED off)
    // LED for heartbeat
    if (config.gpio.led_heartbeat != -1)
      digitalWrite(config.gpio.led_heartbeat, heartbeat.cycleOnOff(1000, 1000));
  }

  // cyclic call for KM271
  if (!setupMode && !config.sim.enable) {
    cyclicKM271();
  }

  // cyclic Oilmeter
  if (config.oilmeter.use_hardware_meter && !setupMode) {
    cyclicOilmeter();
  }

  // send cyclic infos
  if (mainTimer.cycleTrigger(10000) && !setupMode) {
    sendWiFiInfo();
    sendKM271Info();
    sendKM271Debug();
  }

  if (config.webUI.enable) {
    webUICylic(); // call webUI
  }

  if (config.ntp.enable) {
    // check every hour if DST has changed
    if (dstTimer.cycleTrigger(3600000)) {
      time_t now;
      tm dti;
      time(&now);              // read the current time
      localtime_r(&now, &dti); // update the structure tm with the current time

      if (!dst_ref) { // save actual DST as reference after bootup
        dst_ref = true;
        dst_old = dti.tm_isdst;
      }

      if (dst_old != dti.tm_isdst && !main_reboot) { // check if DST has changed
        km271SetDateTimeNTP();                       // change date and time on buderus
      }
      dst_old = dti.tm_isdst;
    }
    // set ntp time on logamatic after bootup and successful ntp sync
    if (ntpTimer.cycleTrigger(10000)) {
      struct tm timeInfo;
      ntpSynced = getLocalTime(&timeInfo, 5);
      if (ntpSynced && !ntpInit && config.ntp.auto_sync) {
        km271SetDateTimeNTP();
        ntpInit = true;
      }
    }
  }

  // Sensor Cyclic
  cyclicSensor();

  // Message Service
  messageCyclic();

  // get simulation telegrams of KM271
  simDataCyclic();

  // telnet communication
  cyclicTelnet();

  // check if config has changed
  configCyclic();

  main_reboot = false; // reset reboot flag
}
