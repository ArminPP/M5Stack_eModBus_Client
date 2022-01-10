
/*
Test eModBus Client based on the RTU04example file
Copyright 2020 by Michael Harwerth, Bert Melis and the contributors to ModbusClient

Armin Pressler 2022

The client handles 3 servers (USB2RS485, M5Atom, XY-MD02)

IMPORTANT:
  - If one server on the bus is down and still electrically connected
    the whole bus is affected (similar to ProfiBus)
  - If one sever disturbes the bus all the communication is affected


SERVER ISSUES:
  - Atom-Base RS485 has no 120 Ohm burden resistor! (R4 n/c ??)
  - M5Stack W5500 burden resistor unknown?!
  - XY-MD02 needs >5 VDC USB from M5Stack delivers only ~ 4.6V
  - USB2RS485 Adapter no external VCC! -> kills/resets USB from Notebook!


*/
#include <Arduino.h>
#include <M5Stack.h>

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

#define BAUDRATE 9600
#define READ_INTERVAL 5000

// Test Server USB2RS485 with pyModSlave running in Windows (https://sourceforge.net/projects/pymodslave/)
#define SERVER1_ID 28
#define SERVER1_TOKEN 28              // only for Test same # as ID
#define SERVER1_INPUT_REGISTER 0x0000 //
#define SERVER1_NUM_VALUES 50

// Test Server M5Atom with RS485 Module
#define SERVER2_ID  27
#define SERVER2_TOKEN 27             // only for Test same # as ID
#define SERVER2_HOLD_REGISTER 0x012C // =300d
#define SERVER2_NUM_VALUES 8

// Test XY-MD02 cheap chinese temperature sensor (https://www.aliexpress.com/i/1005001475675808.html)
#define SERVER3_ID 1
#define SERVER3_TOKEN 1               // only for Test same # as ID
#define SERVER3_INPUT_REGISTER 0x0001 //
#define SERVER3_NUM_VALUES 2

// received data from the servers
uint16_t Server1_values[SERVER1_NUM_VALUES];
uint16_t Server2_values[SERVER2_NUM_VALUES];
uint16_t Server3_values[SERVER3_NUM_VALUES];

uint32_t MB_Errors = 0;
uint32_t MB_Requests = 0;

// Create a ModbusRTU client instance
// The RS485 module has halfduplex, so the second parameter with the DE/RE pin is not required!
ModbusClientRTU MB(Serial2);

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
    // getValues(response, Server1_values, SERVER1_NUM_VALUES);
  }
  else if (ID == SERVER2_ID)
  {
    // getValues(response, Server2_values, SERVER2_NUM_VALUES);
  }
  else if (ID == SERVER3_ID) // XY-MD02 sensor is broken, so skip this..
  {
    getValues(response, Server3_values, SERVER3_NUM_VALUES);
  }
}

// Define an onError handler function to receive error responses
// Arguments are the error code returned and a user-supplied token to identify the causing request
void handleError(Error error, uint32_t token)
{
  // ModbusError wraps the error code and provides a readable error message for it
  ModbusError me(error);
  LOG_E("Error response: %02X - %s  ServerID: %i\n", (int)me, (const char *)me, token);
  MB_Errors++;
}

// Setup() - initialization happens here
void setup()
{
  // init M5Stack
  M5.begin();
  Serial.begin(115200);

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
  MB.setTimeout(2000);
  // Start ModbusRTU background task
  MB.begin();
}

// loop() - cyclically request the data
void loop()
{
  static uint32_t next_request = millis();
  // Shall we do another request?
  if (millis() - next_request > READ_INTERVAL)
  {
    // Save current time to check for next cycle
    next_request = millis();

    MB_Requests++;

    // Issue the request
    // Error err = MB.addRequest(SERVER1_TOKEN, SERVER1_ID, READ_INPUT_REGISTER, SERVER1_INPUT_REGISTER, SERVER1_NUM_VALUES);
    // if (err != SUCCESS)
    // {
    //   ModbusError e(err);
    //   LOG_E("Error creating request: %02X - %s\n", (int)e, (const char *)e);
    // }

    // Error err2 = MB.addRequest(SERVER2_TOKEN, SERVER2_ID, READ_HOLD_REGISTER, SERVER2_HOLD_REGISTER, SERVER2_NUM_VALUES);
    // if (err2 != SUCCESS)
    // {
    //   ModbusError e(err2);
    //   LOG_E("Error creating request2: %02X - %s\n", (int)e, (const char *)e);
    // }

    Error err3 = MB.addRequest(SERVER3_TOKEN, SERVER3_ID, READ_INPUT_REGISTER, SERVER3_INPUT_REGISTER, SERVER3_NUM_VALUES);
    if (err3 != SUCCESS)
    {
      ModbusError e(err3);
      LOG_E("Error creating request3: %02X - %s\n", (int)e, (const char *)e);
    }
  }

  static uint32_t print_values = millis();
  if (millis() - print_values > 10000)
  {
    // Save current time to check for next cycle
    print_values = millis();
    {
      // if data is ready
      Serial.printf("                 Requests %i / Errors %i\n", MB_Requests, MB_Errors);

      Serial.printf("\nRequested from Server %2i\n", SERVER1_ID);
      for (uint8_t i = 0; i < SERVER1_NUM_VALUES; ++i)
      {
        if (((i + 1) % 4) == 0) // format print output to 4 collumns @ xx rows
          Serial.printf("    %04X: %8i\n", i, Server1_values[i]);
        else
          Serial.printf("    %04X: %8i", i, Server1_values[i]);
      }

      Serial.printf("\nRequested from Server %2i\n", SERVER2_ID);
      for (uint8_t i = 0; i < SERVER2_NUM_VALUES; ++i)
      {
        if (((i + 1) % 4) == 0) // format print output to 4 collumns @ xx rows
          Serial.printf("    %04X: %8i\n", i, Server2_values[i]);
        else
          Serial.printf("    %04X: %8i", i, Server2_values[i]);
      }
      Serial.printf("Requested from Server %2i\n", SERVER3_ID);
      for (uint8_t i = 0; i < SERVER3_NUM_VALUES; ++i)
      {
        Serial.printf("         %04X: %f\n", i, Server3_values[i]/10.0);
      }
      Serial.printf("-------------------------------------------\n\n");
    }
    // DEBUG
    delay(1000); // simulate some blocking code!
  }
}