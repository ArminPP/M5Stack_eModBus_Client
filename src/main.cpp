
/*
Test eModBus Client based on the RTU04example file
Copyright 2020 by Michael Harwerth, Bert Melis and the contributors to ModbusClient

customized by Armin Pressler 2022

The client handles 6 servers (1x Arduino + RS485, 3x M5Atom + RS 485, 2x XY-MD02)

IMPORTANT:
  - If one server on the bus is down and still electrically connected
    the whole bus is affected (similar to ProfiBus)
  - If one sever disturbes the bus all the communication is affected


SERVER ISSUES:
  - Atom-Base RS485 has no 120 Ohm burden resistor! (R4 n/c ??)
  - M5Stack W5500 burden resistor unknown?!
  - XY-MD02 needs >5 VDC, USB from M5Stack delivers only ~ 4.6V but is still OK!
  - USB2RS485 Adapter no external VCC! -> kills/resets USB from Notebook!

XY-MD02 ISSUE:
  - After a few days (+10!) I figured out, that this device is not working well,
    if there are not done some fixes in the software.
    This device needs some 'resting' time *before* and *after* the request command!
    Maybe there is an issue with the timing of the RS485 adapter, I don't know,
    but this was very annoying!
    If there is a delay of 1000ms before/after the request, then there is no error anymore,
    if the requests are fired immediately, then there will be 50% errors with timeout.
    And the whole bus is also disturbed from time to time - with some errors on all devices ...

    The solution is a deleyed state machine, wich ensures that the timing is OK.


CONCLUSION:
                    ID      25          26          27          1             3               42
                DEVICE    M5Atom1     M5Atom2     M5Atom3    XY-MD02-1     XY-MD02-2     Arduino-Nano
DELAY_AFTER_STATE [ms]      50          50          1000       1000          1000             50              No errors!
DELAY_AFTER_STATE [ms]      50          50           50         50            50              50              50% timeout errors (ID1+3)!
DELAY_AFTER_STATE [ms]      1            1            1          x             x               1              No errors (ID1+3 are excluded)!

if the number of received Words was increased (eg 3x12 Words --> 3x80 Words at ID25-27):
DELAY_AFTER_STATE [ms]      50          50          1000       1000          1000             50              some errors, most ID1+3, some ID42



MODBUS Basics:
**************

https://ipc2u.de/artikel/wissenswertes/modbus-rtu-einfach-gemacht-mit-detaillierten-beschreibungen-und-beispielen/

REGISTERNUMMER	REGISTERADRESSE HEX	    TYP	                  NAME	                   TYP
1-9999	        0000 to 270E	      lesen-schreiben	  Discrete Output Coils	            DO
10001-19999	    0000 to 270E	      lesen	            Discrete Input Contacts	          DI
30001-39999	    0000 to 270E	      lesen	            Analog Input Registers	          AI
40001-49999	    0000 to 270E	      lesen-schreiben	  Analog Output Holding Registers	  AO

FUNKTIONSKODE	                FUNKTION   	                                                    WERTTYP	  ZUGRIFFSTYP
01 (0x01)	      Liest DO	                  Read Discrete Output Coil	                        Diskret	    Lesen
02 (0x02)	      Liest DI	                  Read Discrete Input Contact	                      Diskret	    Lesen
03 (0x03)	      Liest AO	                  Read Analog Output Holding Register	              16 Bit	    Lesen
04 (0x04)	      Liest AI	                  Read Analog Input Register	                      16 Bit	    Lesen
05 (0x05)	      Schreibt ein DO	            Setzen einer Discrete Output Coil	                Diskret	    Schreiben
06 (0x06)	      Schreibt ein AO	            Setzen eines Analog Output Holding Registers	    16 Bit	    Schreiben
15 (0x0F)	      Aufzeichnung mehrerer DOs	  Setzen mehrerer Discrete Output Coil	            Diskret	    Schreiben
16 (0x10)	      Aufzeichnung mehrerer AOs	  Setzen mehrerer Analog Output Holding Registers	  16 Bit	    Schreiben

####################################################################
#   The maximum packet sizes are:                                  #
#        Read registers (function codes 03 & 04)   = 125 registers #
#        Write registers (function code 16)        = 123 registers #
#        Read Booleans (function codes 01 & 02)    = 2000 bits     #
#        Write Booleans (function code 15)         = 1968 bits     #
####################################################################

Modbus Client == Master
Modbus Server == Slave

*/
#include <Arduino.h>
#include <M5Stack.h>

#include <Ethernet.h>
#define SCK 18
#define MISO 19
#define MOSI 23
#define CS 26
#include <aWOT.h>

EthernetServer server(80);
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xE1}; // TODO use internal MAC !!!
Application app;

// DEBUG                                                                               .
// ERROR  If using chrono then there is a 'false' definition of min() max() in M5STack.h
//        to fix this, #undef MUST be written after M5Stack.h include !!!!!!!!!!!!!!!!!
//        https://stackoverflow.com/questions/41093090/esp8266-error-macro-min-passed-3-arguments-but-takes-just-2
//        https://community.platformio.org/t/project-build-fails-after-latest-updates/8731/2
#undef max // chrono:225:6: error: macro "max" requires 2 arguments, but only 1 given
#undef min // chrono:225:6: error: macro "min" requires 2 arguments, but only 1 given

// Include the header for the ModbusClient RTU style
#include "ModbusClientRTU.h"

#define LOG_TERM_NOCOLOR // coloring terminal output doesn't work with ArduinoIDE and PlatformIO
#include "Logging.h"

const uint32_t STATE_MACHINE_INTERVAL = 5000; // must be equal or larger than the sum of all delays!

const uint32_t DELAY_AFTER_STATE_1 = 50;   // Arduino Nano and 5V RS485 Shield
const uint32_t DELAY_AFTER_STATE_2 = 1000; // M5Atom with RS485 Module
const uint32_t DELAY_AFTER_STATE_3 = 1000; // XY-MD02 cheap chinese temperature sensor
const uint32_t DELAY_AFTER_STATE_4 = 1000; // XY-MD02 cheap chinese temperature sensor
const uint32_t DELAY_AFTER_STATE_5 = 50;   // M5Atom with RS485 Module
const uint32_t DELAY_AFTER_STATE_6 = 50;   // M5Atom with RS485 Module

enum STATES // different tasks in the state machine
{
  READ_SENSOR_1,
  READ_SENSOR_2,
  READ_SENSOR_3,
  READ_SENSOR_4,
  READ_SENSOR_5,
  READ_SENSOR_6,
  LAST_TASK
};

byte STATE_NR;                       // state tracking
unsigned long STATE_START_DELAY = 0; // global delay for each state
unsigned long STATE_WAIT_DELAY = 0;  // global delay for each state

unsigned long STATE_MACHINE_START = 0; // measurement of runtime of the whole state machine
unsigned long STATE_MACHINE_END = 0;   // to calculate the difference to STATE_MACHINE_INTERVAL

#define BAUDRATE 9600
// #define READ_INTERVAL 5000
// #define POLL_DELAY 500 // important! XY-MD02 need a delay between requests!

// Server Arduino Nano and 5V RS485 Shield
#define SERVER1_ID 42
#define SERVER1_TOKEN 42              // only for Test same # as ID
#define SERVER1_INPUT_REGISTER 0x0001 // statt 0x0000
#define SERVER1_NUM_VALUES 8

// Server M5Atom with RS485 Module
#define SERVER2_ID 27
#define SERVER2_TOKEN 27             // only for Test same # as ID
#define SERVER2_HOLD_REGISTER 0x012C // =300d
#define SERVER2_NUM_VALUES 80

// XY-MD02 cheap chinese temperature sensor (https://www.aliexpress.com/i/1005001475675808.html)
#define SERVER3_ID 1
#define SERVER3_TOKEN 1               // only for Test same # as ID
#define SERVER3_INPUT_REGISTER 0x0001 //
#define SERVER3_NUM_VALUES 2

// XY-MD02 cheap chinese temperature sensor (https://www.aliexpress.com/i/1005001475675808.html)
#define SERVER4_ID 3
#define SERVER4_TOKEN 3               // only for Test same # as ID
#define SERVER4_INPUT_REGISTER 0x0001 //
#define SERVER4_NUM_VALUES 2

// Server M5Atom with RS485 Module
#define SERVER5_ID 26
#define SERVER5_TOKEN 26             // only for Test same # as ID
#define SERVER5_HOLD_REGISTER 0x012C // =300d
#define SERVER5_NUM_VALUES 80

// Server M5Atom with RS485 Module
#define SERVER6_ID 25
#define SERVER6_TOKEN 25             // only for Test same # as ID
#define SERVER6_HOLD_REGISTER 0x012C // =300d
#define SERVER6_NUM_VALUES 80

// received data from the servers
uint16_t Server1_values[SERVER1_NUM_VALUES];
uint16_t Server2_values[SERVER2_NUM_VALUES];
uint16_t Server3_values[SERVER3_NUM_VALUES];
uint16_t Server4_values[SERVER4_NUM_VALUES];
uint16_t Server5_values[SERVER5_NUM_VALUES];
uint16_t Server6_values[SERVER6_NUM_VALUES];

// Create a ModbusRTU client instance
// The RS485 module has halfduplex, so the second parameter with the DE/RE pin is not required!
ModbusClientRTU MB(Serial2);

Error MB_ERROR1;
Error MB_ERROR2;
Error MB_ERROR3;
Error MB_ERROR4;
Error MB_ERROR5;
Error MB_ERROR6;

// test variables
uint32_t MB_Errors = 0;
uint32_t MB_Requests = 0;

void indexCmd(Request &req, Response &res)
{

  // P macro for printing strings from program memory
  P(index) =
      "<html>\n"
      "<head>\n"
      "<title>Hello World!</title>\n"
      "</head>\n"
      "<body>\n"
      "<h1>Greetings middle earth!</h1>\n"
      "</body>\n"
      "</html>";

  res.set("Content-Type", "text/html");
  res.printP(index);

  Serial.println("Mem after settings:");
  Serial.printf("MinFreeHeap %d, MaxAllocHeap %d\n", ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
  Serial.printf("Internal Total heap %d, internal Free Heap %d\n", ESP.getHeapSize(), ESP.getFreeHeap());
  Serial.printf("HiWaterMark: %d bytes | Idle: %d bytes\n",
                uxTaskGetStackHighWaterMark(0),
                uxTaskGetStackHighWaterMark(xTaskGetIdleTaskHandle()));
}

void getValues(ModbusMessage response, uint16_t values[], uint16_t numVal)
{
  // First value is on pos 3, after server ID, function code and length byte
  uint16_t offs = 3;
  // Read the requested in a loop
  for (uint8_t i = 0; i < numVal; ++i)
  {
    offs = response.get(offs, values[i]);
  }
}

// Define an onData handler function to receive the regular responses
// Arguments are received response message and the request's token
void handleData(ModbusMessage response, uint32_t token)
{
  // received ID from active server
  uint8_t ID;
  // get server ID from active server
  ID = response.getServerID();
  // according to the length of the server data, change the number of values directly
  if (ID == SERVER1_ID)
  {
    getValues(response, Server1_values, SERVER1_NUM_VALUES);
  }
  if (ID == SERVER2_ID)
  {
    getValues(response, Server2_values, SERVER2_NUM_VALUES);
  }
  if (ID == SERVER3_ID) // XY-MD02 sensor
  {
    getValues(response, Server3_values, SERVER3_NUM_VALUES);
  }
  if (ID == SERVER4_ID) // XY-MD02 sensor
  {
    getValues(response, Server4_values, SERVER4_NUM_VALUES);
  }
  if (ID == SERVER5_ID) // XY-MD02 sensor
  {
    getValues(response, Server5_values, SERVER5_NUM_VALUES);
  }
  if (ID == SERVER6_ID) // XY-MD02 sensor
  {
    getValues(response, Server6_values, SERVER6_NUM_VALUES);
  }
}

// Define an onError handler function to receive error responses
// Arguments are the error code returned and a user-supplied token to identify the causing request
void handleError(Error error, uint32_t token)
{
  // ModbusError wraps the error code and provides a readable error message for it
  ModbusError me(error);
  // LOG_E("Error: %02X - %s ServerID:n/a Time: %8.3fs\n", (int)me, (const char *)me, (millis() - token) / 1000.0);
  LOG_E("Error: %02X - %s ServerID:%i \n", (int)me, (const char *)me, token);
  MB_Errors++;
}

// Setup() - initialization happens here
void setup()
{
  // init M5Stack
  M5.begin();
  Serial.begin(115200);

  SPI.begin(SCK, MISO, MOSI, -1);
  Ethernet.init(CS);

  // Set up Serial2 connected to Modbus RTU
  Serial2.begin(BAUDRATE, SERIAL_8N1);

  Serial.println("\nPress some serial key or M5 Button B to start program"); // DEBUG
  M5.Lcd.println("Press some serial key or M5 Button B to start program");
  while (Serial.available() == 0)
  {
    M5.update();
    if (M5.BtnB.wasPressed())
    { // if M5 Buttons B was pressed, then also start...
      break;
    }
  }
  Serial.println("OK"); // DEBUG
  M5.Lcd.println("OK");

  // Set up ModbusRTU client.
  // - provide onData handler function
  MB.onDataHandler(&handleData);
  // - provide onError handler function
  MB.onErrorHandler(&handleError);
  // Set message timeout to 2000ms
  MB.setTimeout(2500);
  // Start ModbusRTU background task
  MB.begin();

  M5.Lcd.setTextSize(2);

  if (Ethernet.begin(mac))
  {
    Serial.println(Ethernet.localIP());
  }
  else
  {
    Serial.println("Ethernet failed ");
  }

  // mount the handler to the default router
  app.get("/", &indexCmd);

  Serial.println("Mem after settings:");
  Serial.printf("MinFreeHeap %d, MaxAllocHeap %d\n", ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
  Serial.printf("Internal Total heap %d, internal Free Heap %d\n", ESP.getHeapSize(), ESP.getFreeHeap());
  Serial.printf("HiWaterMark: %d bytes | Idle: %d bytes\n",
                uxTaskGetStackHighWaterMark(0),
                uxTaskGetStackHighWaterMark(xTaskGetIdleTaskHandle()));
}

void printRequests()
{
  // if data is ready
  Serial.printf("                 Requests %i / Errors %i\n", MB_Requests, MB_Errors);
  M5.Lcd.setCursor(10, 30);
  M5.Lcd.printf("Requests %i / Errors %i\n", MB_Requests, MB_Errors);

  Serial.printf("\nRequested from Server1 @ID %2i\n", SERVER1_ID);
  M5.Lcd.setCursor(1, 60);
  M5.Lcd.printf("Server1 @ID %2i\n", SERVER1_ID);
  // Serial.printf("\nRequested from Server1 @ID%2i  Time: %8.3fs\n", SERVER1_ID, request_time1 / 1000.0);
  // M5.Lcd.setCursor(1, 80);
  // M5.Lcd.printf("Server1@ID%2i  Time:%8.3fs\n", SERVER1_ID, request_time1 / 1000.0);
  for (uint8_t i = 0; i < SERVER1_NUM_VALUES; ++i) // DEBUG SHOW ONLY 8 of 50!
  {
    if (((i + 1) % 4) == 0) // format print output to 4 collumns @ xx rows
      Serial.printf("    %04X: %8i\n", i, Server1_values[i]);
    else
      Serial.printf("    %04X: %8i", i, Server1_values[i]);
  }

  Serial.printf("\nRequested from Server2 @ID %2i\n", SERVER2_ID);
  M5.Lcd.setCursor(1, 90);
  M5.Lcd.printf("Server2 @ID %2i\n", SERVER2_ID);
  for (uint8_t i = 0; i < SERVER2_NUM_VALUES; ++i)
  {
    if (((i + 1) % 4) == 0) // format print output to 4 collumns @ xx rows
      Serial.printf("    %04X: %8i\n", i, Server2_values[i]);
    else
      Serial.printf("    %04X: %8i", i, Server2_values[i]);
  }

  Serial.printf("\nRequested from Server3 @ID %2i\n", SERVER3_ID);
  M5.Lcd.setCursor(1, 120);
  M5.Lcd.printf("Server3@ ID %2i\n", SERVER3_ID);
  for (uint8_t i = 0; i < SERVER3_NUM_VALUES; ++i)
  {
    Serial.printf("    %04X: %5.1f\n", i, Server3_values[i] / 10.0);
  }

  Serial.printf("\nRequested from Server4 @ID %2i\n", SERVER4_ID);
  M5.Lcd.setCursor(1, 150);
  M5.Lcd.printf("Server4 @ID %2i\n", SERVER4_ID);
  for (uint8_t i = 0; i < SERVER4_NUM_VALUES; ++i) // DEBUG SHOW ONLY 8 of 50!
  {
    Serial.printf("    %04X: %5.1f\n", i, Server4_values[i] / 10.0);
  }

  Serial.printf("\nRequested from Server5 @ID %2i\n", SERVER5_ID);
  M5.Lcd.setCursor(1, 180);
  M5.Lcd.printf("Server5 @ID %2i\n", SERVER5_ID);
  for (uint8_t i = 0; i < SERVER5_NUM_VALUES; ++i) // DEBUG SHOW ONLY 8 of 50!
  {
    if (((i + 1) % 4) == 0) // format print output to 4 collumns @ xx rows
      Serial.printf("    %04X: %8i\n", i, Server5_values[i]);
    else
      Serial.printf("    %04X: %8i", i, Server5_values[i]);
  }
  Serial.printf("\nRequested from Server6 @ID %2i\n", SERVER6_ID);
  M5.Lcd.setCursor(1, 210);
  M5.Lcd.printf("Server6 @ID %2i\n", SERVER6_ID);
  for (uint8_t i = 0; i < SERVER6_NUM_VALUES; ++i) // DEBUG SHOW ONLY 8 of 50!
  {
    if (((i + 1) % 4) == 0) // format print output to 4 collumns @ xx rows
      Serial.printf("    %04X: %8i\n", i, Server6_values[i]);
    else
      Serial.printf("    %04X: %8i", i, Server6_values[i]);
  }

  Serial.printf("-------------------------------------------\n");
}

void NonBlockingStateMachine()
{
  /*
 non blocking delayed state machine with constant loop time
 ##########################################################

 based on the code from 'GoForSmoke' in the arduino forum
 https://forum.arduino.cc/t/fading-led-up-and-down-with-predetermined-intervals-non-blocking/559601/4


                                           STATE MACHINE DIAGRAM

                  Task 1           Task 2             Task 3           Task n
                 +-----+       +-----------+        +--------+       +-------+
                 |     |       |           |        |        |       |       |
                 |     |       |           |        |        |       |       |
                 |     |       |           |        |        |       |       |
                 |     |       |           |        |        |       |       |
                 +     +-------+           +--------+        +-------+       +-----/ /------+

                 |             |                    |                |                      |
                 |             |                    |                |                      |
                 |<----------->|<------------------>|<-------------->|<-------------------->|
                 |                                                                          |
                 |   State 1          State 2             State 3       State n + last Task |
                 |                                                                          |
                 |<------------------------------------------------------------------------>|
                                        state machine interval time


              every state includes some blocking code (Task) and a nonblocking delay
              the last task fills the gap between runtime and interval time if necessary

 */
  // start of one-shot timer
  if (STATE_WAIT_DELAY > 0) // one-shot timer only runs when set
  {
    if (millis() - STATE_START_DELAY < STATE_WAIT_DELAY)
    {
      return; // instead of blocking, the undelayed function returns
    }
    else
    {
      STATE_WAIT_DELAY = 0; // time's up! turn off the timer and run the STATE_NR case
    }
  }

  static unsigned long timer = millis();
  if (STATE_NR == 0)
  {
    Serial.printf("----- state machine interval: %lu ms\n", millis() - timer);
    timer = millis();
  }
  // end of one-shot timer

  switch (STATE_NR) // runs the case numbered in STATE_NR
  {
  case READ_SENSOR_1:
    STATE_MACHINE_START = millis(); // start measuring loop time of state machine

    Serial.printf("StateNo:%2i delay: %lu\n", READ_SENSOR_1, millis() - STATE_START_DELAY);
    STATE_START_DELAY = millis();

    // put some blocking code always AFTER this line
    /*
    MB_Requests++; // TEST DEBUG
    MB_ERROR1 = MB.addRequest(SERVER1_TOKEN, SERVER1_ID, READ_INPUT_REGISTER, SERVER1_INPUT_REGISTER, SERVER1_NUM_VALUES);
    if (MB_ERROR1 != SUCCESS)
    {
      ModbusError e(MB_ERROR1);
      LOG_E("Error creating request: %02X - %s\n", (int)e, (const char *)e);
    }
    */
    // end of blocking code

    STATE_WAIT_DELAY = DELAY_AFTER_STATE_1; // for the next DELAY_AFTER_STATE_1, this function will return on entry.
    STATE_NR++;                             // when the switch-case runs again it will be the next case that runs
    break;                                  // exit switch-case

  case READ_SENSOR_2:
    Serial.printf("StateNo:%2i delay: %lu\n", READ_SENSOR_2, millis() - STATE_START_DELAY);
    STATE_START_DELAY = millis();

    MB_Requests++; // TEST DEBUG
    MB_ERROR2 = MB.addRequest(SERVER2_TOKEN, SERVER2_ID, READ_HOLD_REGISTER, SERVER2_HOLD_REGISTER, SERVER2_NUM_VALUES);
    if (MB_ERROR2 != SUCCESS)
    {
      ModbusError e(MB_ERROR2);
      LOG_E("Error creating request2: %02X - %s\n", (int)e, (const char *)e);
    }

    STATE_WAIT_DELAY = DELAY_AFTER_STATE_2;
    STATE_NR++;
    break;

  case READ_SENSOR_3:
    Serial.printf("StateNo:%2i delay: %lu\n", READ_SENSOR_3, millis() - STATE_START_DELAY);
    STATE_START_DELAY = millis();

    MB_Requests++; // TEST DEBUG
    MB_ERROR3 = MB.addRequest(SERVER3_TOKEN, SERVER3_ID, READ_INPUT_REGISTER, SERVER3_INPUT_REGISTER, SERVER3_NUM_VALUES);
    if (MB_ERROR3 != SUCCESS)
    {
      ModbusError e(MB_ERROR3);
      LOG_E("Error creating request3: %02X - %s\n", (int)e, (const char *)e);
    }

    STATE_WAIT_DELAY = DELAY_AFTER_STATE_3;
    STATE_NR++;
    break;

  case READ_SENSOR_4:
    Serial.printf("StateNo:%2i delay: %lu\n", READ_SENSOR_4, millis() - STATE_START_DELAY);
    STATE_START_DELAY = millis();
    /*
        MB_Requests++; // TEST DEBUG
        MB_ERROR4 = MB.addRequest(SERVER4_TOKEN, SERVER4_ID, READ_INPUT_REGISTER, SERVER4_INPUT_REGISTER, SERVER4_NUM_VALUES);
        if (MB_ERROR4 != SUCCESS)
        {
          ModbusError e(MB_ERROR4);
          LOG_E("Error creating request3: %02X - %s\n", (int)e, (const char *)e);
        }
    */
    STATE_WAIT_DELAY = DELAY_AFTER_STATE_4;
    STATE_NR++;
    break;

  case READ_SENSOR_5:
    Serial.printf("StateNo:%2i delay: %lu\n", READ_SENSOR_5, millis() - STATE_START_DELAY);
    STATE_START_DELAY = millis();
    /*
        MB_Requests++; // TEST DEBUG
        MB_ERROR5 = MB.addRequest(SERVER5_TOKEN, SERVER5_ID, READ_HOLD_REGISTER, SERVER5_HOLD_REGISTER, SERVER5_NUM_VALUES);
        if (MB_ERROR5 != SUCCESS)
        {
          ModbusError e(MB_ERROR5);
          LOG_E("Error creating request2: %02X - %s\n", (int)e, (const char *)e);
        }
    */
    STATE_WAIT_DELAY = DELAY_AFTER_STATE_5;
    STATE_NR++;
    break;

  case READ_SENSOR_6:
    Serial.printf("StateNo:%2i delay: %lu\n", READ_SENSOR_6, millis() - STATE_START_DELAY);
    STATE_START_DELAY = millis();
    /*
        MB_Requests++; // TEST DEBUG
        MB_ERROR6 = MB.addRequest(SERVER6_TOKEN, SERVER6_ID, READ_HOLD_REGISTER, SERVER6_HOLD_REGISTER, SERVER6_NUM_VALUES);
        if (MB_ERROR6 != SUCCESS)
        {
          ModbusError e(MB_ERROR6);
          LOG_E("Error creating request2: %02X - %s\n", (int)e, (const char *)e);
        }
    */
    STATE_WAIT_DELAY = DELAY_AFTER_STATE_6;
    STATE_NR++;
    break;

  case LAST_TASK:
    // End of state machine loop --> do some other stuff!
    // E.g. send all the collected values via MQTT or something similar to an upper layer/server
    // ---------------------------------------------------
    printRequests();

    // fill up the gap between runntime and looptime if necessary
    STATE_MACHINE_END = millis() - STATE_MACHINE_START; // calculate running time of state machine
    // be careful: Serial.println() takes about 3ms (at least with wokwi.com online sim) - so timing is not very accurate
    //      Serial.printf("[%s] Last task %2i delay: %i  time left:%i\n",
    //                     runtime(), LAST_TASK, millis() - STATE_START_DELAY, STATE_MACHINE_INTERVAL - STATE_MACHINE_END);
    STATE_START_DELAY = millis();

    Serial.printf("Looptime state machine: %lu ms\n", STATE_MACHINE_END);
    if (STATE_MACHINE_END <= STATE_MACHINE_INTERVAL) // if runtime smaller than looptime of state machine
    {
      STATE_WAIT_DELAY = STATE_MACHINE_INTERVAL - STATE_MACHINE_END;
      // be careful: Serial.println() takes about 3ms (at least with wokwi.com online sim) - so timing is not very accurate
      //        Serial.printf("compensate time: %i ms\n", STATE_WAIT_DELAY);
    }
    else
    {
      STATE_WAIT_DELAY = 0;
      Serial.printf("WARNING: blocking code took longer (%lu ms) than overall interval (%i ms) !\n",
                    STATE_MACHINE_END - STATE_MACHINE_INTERVAL, STATE_MACHINE_INTERVAL);
    }
    STATE_NR = 0; // finish, start again
    break;
  }
}

// loop() - cyclically request the data
void loop()
{
  M5.update();
  NonBlockingStateMachine();
  EthernetClient client = server.available();
  if (client.connected())
  {
    app.process(&client);
    client.stop();
  }
}