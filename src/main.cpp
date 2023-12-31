/*
 *  Spresense LTE with DFRobot Environmental Sensor
 *
 *  send env sensor, gnss and voltage data to MQTT broker via LTE network.
 *
 *  Under GPL v3
 */

// libraries
#include <Arduino.h>
#include <RTC.h>
#include <SDHCI.h>
#include <GNSS.h>
#include <LTE.h>
#include <ArduinoMqttClient.h>
#include "gnss_nmea.h"
#include <Wire.h>
#include "DFRobot_EnvironmentalSensor.h"
#include <SoftwareSerial.h>
#include <LowPower.h>

// APN name
#define APP_LTE_APN "soracom.io" // replace your APN

/* APN authentication settings
 * Ignore these parameters when setting LTE_NET_AUTHTYPE_NONE.
 */
#define APP_LTE_USER_NAME "sora" // replace with your username
#define APP_LTE_PASSWORD  "sora" // replace with your password

// APN IP type
#define APP_LTE_IP_TYPE (LTE_NET_IPTYPE_V4V6) // IP : IPv4v6
// #define APP_LTE_IP_TYPE (LTE_NET_IPTYPE_V4) // IP : IPv4
// #define APP_LTE_IP_TYPE (LTE_NET_IPTYPE_V6) // IP : IPv6

// APN authentication type
#define APP_LTE_AUTH_TYPE (LTE_NET_AUTHTYPE_CHAP) // Authentication : CHAP
// #define APP_LTE_AUTH_TYPE (LTE_NET_AUTHTYPE_PAP) // Authentication : PAP
// #define APP_LTE_AUTH_TYPE (LTE_NET_AUTHTYPE_NONE) // Authentication : NONE

/* RAT to use
 * Refer to the cellular carriers information
 * to find out which RAT your SIM supports.
 * The RAT set on the modem can be checked with LTEModemVerification::getRAT().
 */

#define APP_LTE_RAT (LTE_NET_RAT_CATM) // RAT : LTE-M (LTE Cat-M1)
// #define APP_LTE_RAT (LTE_NET_RAT_NBIOT) // RAT : NB-IoT

// MQTT broker
#define BROKER_NAME "iot.env.cs.i.nagoya-u.ac.jp" // MQTT broker hostname

#define BROKER_PORT 8883               // port 8883 is the default for MQTT over TLS.

#define ROOTCA_FILE "certs/chain.pem"   // Define the path to a file containing CA
                                       // certificates that are trusted.
#define CERT_FILE   "certs/cert.pem" // Define the path to a file containing certificate
                                       // for this client, if required by the server.
#define KEY_FILE    "certs/privkey.pem"  // Define the path to a file containing private key
                                       // for this client, if required by the server.

// MQTT topic
#define MQTT_TOPIC         "spresense/gnss_tracker"
#define MQTT_TOPIC2        "spresense/env_data"
#define MQTT_TOPIC3        "spresense/voltage"

// MQTT publish interval settings
#define PUBLISH_INTERVAL_SEC   1   // MQTT publish interval in sec
#define MAX_NUMBER_OF_PUBLISH  60  // Maximum number of publish
#define RET_SUCCESS            1   // Return value meaning success
#define RET_FAILURE            0   // Return value meaning failure
#define CONNECT_RETRY          5   // Number of retries when connection to the broker fails

// MQTT client username and password
const char clientID[40] = "spresense_lte";
const char *mqttUsername = "********";
const char *mqttPassword = "********";

// MQTT settings
unsigned long lastPubSec = 0;
char broker[] = BROKER_NAME;
int port = BROKER_PORT;
char topic[]  = MQTT_TOPIC;
char topic2[] = MQTT_TOPIC2;
char topic3[] = MQTT_TOPIC3;

// network related objects
LTE lteAccess;
LTETLSClient client;
MqttClient mqttClient(client);
LTEModemStatus modemStatus;

SDClass theSD;
SpGnss Gnss;

// battery voltage (mV)
int mvolt = 0;

//DFRobot_EnvironmentalSensor environment(/*addr = */SEN050X_DEFAULT_DEVICE_ADDRESS, /*pWire = */&Wire);
DFRobot_EnvironmentalSensor environment(/*addr = */SEN050X_DEFAULT_DEVICE_ADDRESS, /*pWire = */&Wire1);

#define SENSING_RATE 1 * 60 // sec

void printData();

void printClock(RtcTime &rtc)
{
  printf("%04d/%02d/%02d %02d:%02d:%02d\n",
         rtc.year(), rtc.month(), rtc.day(),
         rtc.hour(), rtc.minute(), rtc.second());
}

String readFromSerial() {
  /* Read String from serial monitor */
  String str;
  int  read_byte = 0;
  while (true) {
    if (Serial.available() > 0) {
      read_byte = Serial.read();
      if (read_byte == '\n' || read_byte == '\r') {
        Serial.println("");
        break;
      }
      Serial.print((char)read_byte);
      str += (char)read_byte;
    }
  }
  return str;
}

void readApnInformation(char apn[], LTENetworkAuthType *authtype,
                       char user_name[], char password[]) {
  /* Set APN parameter to arguments from readFromSerial() */

  String read_buf;

  while (strlen(apn) == 0) {
    Serial.print("Enter Access Point Name:");
    readFromSerial().toCharArray(apn, LTE_NET_APN_MAXLEN);
  }

  while (true) {
    Serial.print("Enter APN authentication type(CHAP, PAP, NONE):");
    read_buf = readFromSerial();
    if (read_buf.equals("NONE") == true) {
      *authtype = LTE_NET_AUTHTYPE_NONE;
    } else if (read_buf.equals("PAP") == true) {
      *authtype = LTE_NET_AUTHTYPE_PAP;
    } else if (read_buf.equals("CHAP") == true) {
      *authtype = LTE_NET_AUTHTYPE_CHAP;
    } else {
      /* No match authtype */
      Serial.println("No match authtype. type at CHAP, PAP, NONE.");
      continue;
    }
    break;
  }

  if (*authtype != LTE_NET_AUTHTYPE_NONE) {
    while (strlen(user_name)== 0) {
      Serial.print("Enter username:");
      readFromSerial().toCharArray(user_name, LTE_NET_USER_MAXLEN);
    }
    while (strlen(password) == 0) {
      Serial.print("Enter password:");
      readFromSerial().toCharArray(password, LTE_NET_PASSWORD_MAXLEN);
    }
  }

  return;
}

/* Attach to the LTE network */

void doAttach()
{
  char apn[LTE_NET_APN_MAXLEN] = APP_LTE_APN;
  LTENetworkAuthType authtype = APP_LTE_AUTH_TYPE;
  char user_name[LTE_NET_USER_MAXLEN] = APP_LTE_USER_NAME;
  char password[LTE_NET_PASSWORD_MAXLEN] = APP_LTE_PASSWORD;

  /* Set if Access Point Name is empty */
  if (strlen(APP_LTE_APN) == 0) {
    Serial.println("This sketch doesn't have a APN information.");
    readApnInformation(apn, &authtype, user_name, password);
  }
  Serial.println("=========== APN information ===========");
  Serial.print("Access Point Name  : ");
  Serial.println(apn);
  Serial.print("Authentication Type: ");
  Serial.println(authtype == LTE_NET_AUTHTYPE_CHAP ? "CHAP" :
                 authtype == LTE_NET_AUTHTYPE_NONE ? "NONE" : "PAP");
  if (authtype != LTE_NET_AUTHTYPE_NONE) {
    Serial.print("User Name          : ");
    Serial.println(user_name);
    Serial.print("Password           : ");
    Serial.println(password);
  }

  while (true) {

    /* Power on the modem and Enable the radio function. */

    if (lteAccess.begin() != LTE_SEARCHING) {
      Serial.println("Could not transition to LTE_SEARCHING.");
      Serial.println("Please check the status of the LTE board.");
      for (;;) {
        sleep(1);
      }
    }

    /* The connection process to the APN will start.
     * If the synchronous parameter is false,
     * the return value will be returned when the connection process is started.
     */
    if (lteAccess.attach(APP_LTE_RAT,
                         apn,
                         user_name,
                         password,
                         authtype,
                         APP_LTE_IP_TYPE,
                         false) == LTE_CONNECTING) {
      Serial.println("Attempting to connect to network.");
      break;
    }

    /* If the following logs occur frequently, one of the following might be a cause:
     * - APN settings are incorrect
     * - SIM is not inserted correctly
     * - If you have specified LTE_NET_RAT_NBIOT for APP_LTE_RAT,
     *   your LTE board may not support it.
     */
    Serial.println("An error has occurred. Shutdown and retry the network attach preparation process after 1 second.");
    lteAccess.shutdown();
    sleep(1);
  }
}

void connectMqttBroker(char *_broker, int _port)
{
  int i;

  for (i = 0; i < CONNECT_RETRY; i++) {
    Serial.print("Attempting to connect to the MQTT broker: ");
    Serial.println(_broker);

    if (mqttClient.connect(_broker, _port)) {
      break;
    }
    Serial.print("MQTT connection failed! Error code = ");
    Serial.println(mqttClient.connectError());
    sleep(1);
  }

  if (i >= CONNECT_RETRY) {
    Serial.println("Exceeded maximum number of retries to connect to the broker, application terminated.");

    // do nothing forevermore:
    for (;;)
      sleep(1);
  }

  Serial.println("You're connected to the MQTT broker!");
}

int mqttPublish(char *_topic, const String &_data)
{
  // Publish to broker
  Serial.print("Sending message to topic: ");
  Serial.println(_topic);
  Serial.print("Publish: ");
  Serial.println(_data);

  if (mqttClient.beginMessage(_topic) == 0) {
    Serial.println("mqttClient.beginMessage failed!");
    mqttClient.stop();
    return RET_FAILURE;
  }

  if (mqttClient.print(_data) == 0) {
    Serial.println("mqttClient.print failed!");
    mqttClient.stop();
    return RET_FAILURE;
  }

  if (mqttClient.endMessage() == 0) {
    Serial.println("mqttClient.endMessage failed!");
    mqttClient.stop();
    return RET_FAILURE;
  }

  Serial.println("MQTT Publish succeeded!");

  return RET_SUCCESS;
}

void setup()
{
  LowPower.begin();

  // Open serial communications and wait for port to open
  Serial.begin(115200);
  while (!Serial) {
      ; // wait for serial port to connect. Needed for native USB port only
  }

  Serial.println("Starting GNSS tracker via LTE.");

  /* Initialize SD */
  while (!theSD.begin()) {
    ; /* wait until SD card is mounted. */
  }

  /* Initialize environmental sensor */
  Wire1.begin();

  // start environment sensor test
  while(environment.begin() != 0){
    Serial.println(" Sensor initialize failed!!");
    delay(1000);
  }
  Serial.println(" Sensor  initialize success!!");

  /* Connect LTE network */
  doAttach();

  int result;

  /* Activate GNSS device */
  result = Gnss.begin();
  assert(result == 0);

  /* Start positioning */
  result = Gnss.start();
  assert(result == 0);
  Serial.println("Gnss setup OK");

  // Wait for the modem to connect to the LTE network.
  Serial.println("Waiting for successful attach.");
  modemStatus = lteAccess.getStatus();

  while(LTE_READY != modemStatus) {
    if (LTE_ERROR == modemStatus) {

      /* If the following logs occur frequently, one of the following might be a cause:
       * - Reject from LTE network
       */
      Serial.println("An error has occurred. Shutdown and retry the network attach process after 1 second.");
      lteAccess.shutdown();
      sleep(1);
      doAttach();
    }
    sleep(1);
    modemStatus = lteAccess.getStatus();
  }

  Serial.println("attach succeeded.");

  // Set local time (not UTC) obtained from the network to RTC.
  RTC.begin();
  unsigned long currentTime;
  while(0 == (currentTime = lteAccess.getTime())) {
    sleep(1);
  }
  RtcTime rtc(currentTime);
  printClock(rtc);
  RTC.setTime(rtc);

  // Set certifications via a file on the SD card before connecting to the MQTT broker
  File rootCertsFile = theSD.open(ROOTCA_FILE, FILE_READ);
  client.setCACert(rootCertsFile, rootCertsFile.available());
  rootCertsFile.close();

  File certsFile = theSD.open(CERT_FILE, FILE_READ);
  client.setCertificate(certsFile, certsFile.available());
  certsFile.close();

  File priKeyFile = theSD.open(KEY_FILE, FILE_READ);
  client.setPrivateKey(priKeyFile, priKeyFile.available());
  priKeyFile.close();

  // connect to the MQTT broker
  connectMqttBroker(broker, port);
}

void loop()
{
  /* Get navData. */
  SpNavData navData;
  Gnss.getNavData(&navData);

  /* Get environment data. */
  String env_data = "T: ";
  env_data += environment.getTemperature(TEMP_C);
  env_data += "C";
  env_data += " H: ";
  env_data += environment.getHumidity();
  env_data += "%";
  env_data += " P: ";
  env_data += environment.getAtmospherePressure(HPA);
  env_data += " hPa";
  env_data += " UV: ";
  env_data += environment.getUltravioletIntensity();
  env_data += " mw/cm2";
  env_data += " L: ";
  env_data += environment.getLuminousIntensity();
  env_data += " lx";
  env_data += " A: ";
  env_data += environment.getElevation();
  env_data += " m";

  // get current time from LTE network
  unsigned long currentTime = lteAccess.getTime();
  if (currentTime >= lastPubSec + PUBLISH_INTERVAL_SEC) {
    if (!mqttClient.connected()) {
      // reconnect to the MQTT broker
      connectMqttBroker(broker, port);
    }

    /**
     * Publish each data to MQTT broker
     * 
     * gnss, env data, battery voltage
     */

    // publish position data
    bool posFix = ((navData.posDataExist) && (navData.posFixMode != FixInvalid));
    if (posFix) {
      Serial.println("Position is fixed.");
      String nmeaString = getNmeaGga(&navData);
      if (strlen(nmeaString.c_str()) != 0) {
        if (mqttPublish(topic, nmeaString) == RET_SUCCESS) {
          lastPubSec = currentTime;

          Serial.println("-------------------------------");
          Serial.print("Position: ");
          Serial.println(nmeaString);
          Serial.println("-------------------------------");

          delay(10);
        }
      }
    } else {
      Serial.println("ERROR: Position is not fixed.");
    }

    // publish environment data
    if (mqttPublish(topic2, env_data) == RET_SUCCESS) {
      lastPubSec = currentTime;

      printData();

      delay(10);
    }

    // publish battery voltage
    mvolt = LowPower.getVoltage();
    String currentVoltage = String(mvolt);
    if (mqttPublish(topic3, currentVoltage) == RET_SUCCESS) {
      lastPubSec = currentTime;

      Serial.println("-------------------------------");
      Serial.print("Battery voltage: ");
      Serial.print(currentVoltage);
      Serial.println(" mV");
      Serial.println("-------------------------------");

      delay(10);
    }

    delay(SENSING_RATE * 1000);
  }

  // 切断対策 ========================================

  // Wait for the modem to connect to the LTE network.
  Serial.println("Waiting for successful attach.");
  modemStatus = lteAccess.getStatus();

  while(LTE_READY != modemStatus) {
    if (LTE_ERROR == modemStatus) {

      /* If the following logs occur frequently, one of the following might be a cause:
       * - Reject from LTE network
       */
      Serial.println("An error has occurred. Shutdown and retry the network attach process after 1 second.");
      lteAccess.shutdown();
      sleep(1);
      doAttach();
    }
    sleep(1);
    modemStatus = lteAccess.getStatus();
  }
  Serial.println("attach succeeded.");
}

void printData() {
  //Print the data obtained from sensor
  Serial.println("-------------------------------");
  Serial.print("Temperature: ");
  Serial.print(environment.getTemperature(TEMP_C));
  Serial.println(" ℃");
  Serial.print("Humidity: ");
  Serial.print(environment.getHumidity());
  Serial.println(" %");
  Serial.print("Ultraviolet intensity: ");
  Serial.print(environment.getUltravioletIntensity());
  Serial.println(" mW/cm2");
  Serial.print("Luminous intensity: ");
  Serial.print(environment.getLuminousIntensity());
  Serial.println(" lx");
  Serial.print("Atmospheric pressure: ");
  Serial.print(environment.getAtmospherePressure(HPA));
  Serial.println(" hPa");
  Serial.print("Altitude: ");
  Serial.print(environment.getElevation());
  Serial.println(" m");
  Serial.println("-------------------------------");
}
