#include <Arduino.h>
#include <OSCBoards.h>
#include <OSCBundle.h>
#include <OSCData.h>
#include <OSCMatch.h>
#include <OSCMessage.h>
#include <OSCTiming.h>
#ifdef BOARD_HAS_USB_SERIAL
#include <SLIPEncodedUSBSerial.h>
SLIPEncodedUSBSerial SLIPSerial(thisBoardsSerialUSB);
#else
#include <SLIPEncodedSerial.h>
SLIPEncodedSerial SLIPSerial(Serial);
#endif
#include <LiquidCrystal.h>
#include <string.h>

/*******************************************************************************
   Macros and Constants
 ******************************************************************************/
#define LCD_CHARS           16
#define LCD_LINES           2   // Currently assume at least 2 lines

/** Button pins */
#define NEXT_BTN            8
#define LAST_BTN            9
#define SHIFT_BTN           10

#define SUBSCRIBE           ((int32_t)1)
#define UNSUBSCRIBE         ((int32_t)0)

#define EDGE_DOWN           ((int32_t)1)
#define EDGE_UP             ((int32_t)0)

#define FORWARD             0
#define REVERSE             1

// Change these values to switch which direction increase/decrease pan/tilt
#define PAN_DIR             FORWARD
#define TILT_DIR            FORWARD

// Use these values to make the encoder more coarse or fine.
// This controls the number of wheel "ticks" the device sends to the console
// for each tick of the encoder. 1 is the default and the most fine setting.
// Must be an integer.
#define PAN_SCALE           1
#define TILT_SCALE          1

#define SIG_DIGITS          3   // Number of significant digits displayed

#define OSC_BUF_MAX_SIZE    512

const String HANDSHAKE_QUERY = "ETCOSC?";
const String HANDSHAKE_REPLY = "OK";

//See displayScreen() below - limited to 10 chars (after 6 prefix chars)
#define VERSION_STRING      "2.0.0.1"
#define BOX_NAME_STRING     "box1"

// Change these values to alter how long we wait before sending an OSC ping
// to see if Eos is still there, and then finally how long before we
// disconnect and show the splash screen
// Values are in milliseconds
#define PING_AFTER_IDLE_INTERVAL    2500
#define TIMEOUT_AFTER_IDLE_INTERVAL 5000

/**
 * Custom Types
 */
enum WHEEL_TYPE { TILT, PAN };
enum WHEELE_MODE { COARSE, FINE };

typedef struct Encoder {
  uint8_t pinA;
  uint8_t pinB;
  int pinAPrevious;
  int pinBPrevious;
  float pos;
  uint8_t direction;
} Encoder;

Encoder panWheel;
Encoder tiltWheel;

enum ConsoleType {
  ConsoleNone,
  ConsoleEos
};

/* initialize lcd with interface pins */
LiquidCrystal lcd(7, 6, 5, 4, 3, 2);

bool updateDisplay = false;
ConsoleType connectedToConsole = ConsoleNone;
unsigned long lastMessageRxTime = 0;
bool timeoutPingSent = false;

void issueEosSubscribes() {
  OSCMessage filter("/eos/filter/add");
  filter.add("/eos/out/param/*");
  filter.add("/eos/out/ping");
  filter.send(SLIPSerial);
  SLIPSerial.endPacket();

  /* Subscribe to EOS pan and tilt updates */
  OSCMessage subPan("/eos/subscribe/param/pan");
  subPan.add(SUBSCRIBE);
  SLIPSerial.beginPacket();
  subPan.send(SLIPSerial);
  SLIPSerial.endPacket();

  OSCMessage subTilt("/eos/subscribe/param/tilt");
  subTilt.add(SUBSCRIBE);
  SLIPSerial.beginPacket();
  subTilt.send(SLIPSerial);
  SLIPSerial.endPacket();
}

void parseFloatPanUpdate(OSCMessage& msg, int addressOffset) {
  panWheel.pos = msg.getOSCData(0)->getFloat();
  updateDisplay = true;
}

void parseFloatTiltUpdate(OSCMessage& msg, int addressOffset) {
  tiltWheel.pos = msg.getOSCData(0)->getFloat();
  updateDisplay = true;
}

void parseEos(OSCMessage& msg, int addressOffset) {
  // If we don't think we're connected, reconnect and subscribe
  if (connectedToConsole != ConsoleEos) {
    issueEosSubscribes();
    connectedToConsole = ConsoleEos;
    updateDisplay = true;
  }

  if (!msg.route("/out/param/pan", parseFloatPanUpdate, addressOffset)) {
    msg.route("/out/param/tilt", parseFloatTiltUpdate, addressOffset);
  }
}

void parseOSCMessage(String& msg) {
  // check to see if this is the handshake string
  if (msg.indexOf(HANDSHAKE_QUERY) != -1) {
    // handshake string found!
    SLIPSerial.beginPacket();
    SLIPSerial.write((const uint8_t*)HANDSHAKE_REPLY.c_str(), (size_t)HANDSHAKE_REPLY.length());
    SLIPSerial.endPacket();

    // An Eos would do nothing until subscribed
    // Let Eos know we want updates on some things
    issueEosSubscribes();

    updateDisplay = true;
    digitalWrite(LED_BUILTIN, HIGH);
  } else {
    // prepare the message for routing by filling an OSCMessage object with our message string
    OSCMessage oscmsg;
    oscmsg.fill((uint8_t*)msg.c_str(), (int)msg.length());
    // route pan/tilt messages to the relevant update function

    oscmsg.route("/eos", parseEos);
  }
}

void displayStatus() {
  lcd.clear();

  switch (connectedToConsole) {
    case ConsoleNone:
      {
        // display a splash message before the Eos connection is open
        lcd.setCursor(0, 0);
        lcd.print(BOX_NAME_STRING " v" VERSION_STRING);
        lcd.setCursor(0, 1);
        lcd.print("Waiting...");
      } break;

    case ConsoleEos:
      {
        // put the cursor at the begining of the first line
        lcd.setCursor(0, 0);
        lcd.print("Pan:  ");
        lcd.print(panWheel.pos, SIG_DIGITS);

        // put the cursor at the begining of the second line
        lcd.setCursor(0, 1);
        lcd.print("Tilt: ");
        lcd.print(tiltWheel.pos, SIG_DIGITS);
      } break;
  }

  updateDisplay = false;
}

void initEncoder(struct Encoder* encoder, uint8_t pinA, uint8_t pinB, uint8_t direction) {
  encoder->pinA = pinA;
  encoder->pinB = pinB;
  encoder->pos = 0;
  encoder->direction = direction;

  pinMode(pinA, INPUT_PULLUP);
  pinMode(pinB, INPUT_PULLUP);

  encoder->pinAPrevious = digitalRead(pinA);
  encoder->pinBPrevious = digitalRead(pinB);
}

int8_t updateEncoder(struct Encoder* encoder) {
  int8_t encoderMotion = 0;
  int pinACurrent = digitalRead(encoder->pinA);
  int pinBCurrent = digitalRead(encoder->pinB);

  // has the encoder moved at all?
  if (encoder->pinAPrevious != pinACurrent) {
    // Since it has moved, we must determine if the encoder has moved forwards or backwards
    encoderMotion = (encoder->pinAPrevious == encoder->pinBPrevious) ? -1 : 1;

    // If we are in reverse mode, flip the direction of the encoder motion
    if (encoder->direction == REVERSE) {
      encoderMotion = -encoderMotion;
    }
  }
  encoder->pinAPrevious = pinACurrent;
  encoder->pinBPrevious = pinBCurrent;

  return encoderMotion;
}

void sendOscMessage(const String &address, float value) {
  OSCMessage msg(address.c_str());
  msg.add(value);
  SLIPSerial.beginPacket();
  msg.send(SLIPSerial);
  SLIPSerial.endPacket();
}

void sendEosWheelMove(WHEEL_TYPE type, float ticks) {
  String wheelMsg("/eos/wheel");

  if (digitalRead(SHIFT_BTN) == LOW) {
    wheelMsg.concat("/fine");
  } else {
    wheelMsg.concat("/coarse");
  }
  if (type == PAN) {
    wheelMsg.concat("/pan");
  } else if (type == TILT) {
    wheelMsg.concat("/tilt");
  } else {
    // something has gone very wrong
    return;
  }

  sendOscMessage(wheelMsg, ticks);
}

void sendWheelMove(WHEEL_TYPE type, float ticks) {
  switch (connectedToConsole) {
    default:
    case ConsoleEos:
      sendEosWheelMove(type, ticks);
      break;
  }
}

void sendKeyPress(bool down, const String &key) {
  String keyAddress;
  switch (connectedToConsole) {
    default:
    case ConsoleEos:
      keyAddress = "/eos/key/" + key;
      break;
  }
  OSCMessage keyMsg(keyAddress.c_str());

  if (down) {
    keyMsg.add(EDGE_DOWN);
  } else {
    keyMsg.add(EDGE_UP);
  }
  SLIPSerial.beginPacket();
  keyMsg.send(SLIPSerial);
  SLIPSerial.endPacket();
}

void checkButtons() {
  // OSC configuration
  const int keyCount = 2;
  const int keyPins[2] = {NEXT_BTN, LAST_BTN};
  const String keyNames[2] = { "NEXT", "LAST", };

  static int keyStates[2] = {HIGH, HIGH};

  // Loop over the buttons
  for (int keyNum = 0; keyNum < keyCount; ++keyNum) {
    // Has the button state changed
    if (digitalRead(keyPins[keyNum]) != keyStates[keyNum]) {
      // Notify console of this key press
      if (keyStates[keyNum] == LOW) {
        sendKeyPress(false, keyNames[keyNum]);
        keyStates[keyNum] = HIGH;
      } else {
        sendKeyPress(true, keyNames[keyNum]);
        keyStates[keyNum] = LOW;
      }
    }
  }
}

void setup() {
  SLIPSerial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

#ifdef BOARD_HAS_USB_SERIAL
  while (!SerialUSB);
#else
  while (!Serial);
#endif

  SLIPSerial.beginPacket();
  SLIPSerial.write((const uint8_t*)HANDSHAKE_REPLY.c_str(), (size_t)HANDSHAKE_REPLY.length());
  SLIPSerial.endPacket();

  issueEosSubscribes();

  /* encoder pins */
  initEncoder(&panWheel, A0, A1, PAN_DIR);
  initEncoder(&tiltWheel, A3, A4, TILT_DIR);

  lcd.begin(LCD_CHARS, LCD_LINES);
  lcd.clear();

  pinMode(NEXT_BTN, INPUT_PULLUP);
  pinMode(LAST_BTN, INPUT_PULLUP);
  pinMode(SHIFT_BTN, INPUT_PULLUP);

  displayStatus();
}

void loop() {
  static String curMsg;
  int size;
  // get the updated state of each encoder
  int32_t panMotion = updateEncoder(&panWheel);
  int32_t tiltMotion = updateEncoder(&tiltWheel);

  // Scale the result by a scaling factor
  panMotion *= PAN_SCALE;
  tiltMotion *= TILT_SCALE;

  // check for next/last updates
  checkButtons();

  // now update our wheels
  if (tiltMotion != 0) {
    sendWheelMove(TILT, tiltMotion);
  }

  if (panMotion != 0) {
    sendWheelMove(PAN, panMotion);
  }

  // Then we check to see if any OSC commands have come from Eos
  // and update the display accordingly.
  size = SLIPSerial.available();
  if (size > 0) {
    // Fill the msg with all of the available bytes
    while (size--)
      curMsg += (char)(SLIPSerial.read());
  }
  if (SLIPSerial.endofPacket()) {
    parseOSCMessage(curMsg);
    lastMessageRxTime = millis();
    // We only care about the ping if we haven't heard recently
    // Clear flag when we get any traffic
    timeoutPingSent = false;
    curMsg = String();
  }

  if (lastMessageRxTime > 0) {
    unsigned long diff = millis() - lastMessageRxTime;
    // We first check if it's been too long and we need to time out
    if (diff > TIMEOUT_AFTER_IDLE_INTERVAL) {
      connectedToConsole = ConsoleNone;
      lastMessageRxTime = 0;
      updateDisplay = true;
      timeoutPingSent = false;
      digitalWrite(LED_BUILTIN, LOW);
    }

    // It could be the console is sitting idle. Send a ping once to
    // double check that it's still there, but only once after 2.5s have passed
    if (!timeoutPingSent && diff > PING_AFTER_IDLE_INTERVAL) {
      OSCMessage ping("/eos/ping");
      ping.add(BOX_NAME_STRING "_hello"); // This way we know who is sending the ping
      SLIPSerial.beginPacket();
      ping.send(SLIPSerial);
      SLIPSerial.endPacket();
      timeoutPingSent = true;
    }
  }

  if (updateDisplay) {
    displayStatus();
  }
}
