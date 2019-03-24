  /*

 J1772 Hydra for Arduino
 Copyright 2013 Nicholas W. Sayer
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License along
 with this program; if not, write to the Free Software Foundation, Inc.,
 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <avr/wdt.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <PWM.h>
#include <EEPROM.h>

#define LCD_I2C_ADDR 0x27 // for adafruit shield or backpack

// By historical accident, car B is actually
// the lower pin number in most cases.
//
// The reason it matters is that in the UI, car "A"
// is on the left. Therefore, car "A" is on the *right*
// side of the component side of the board, since the LCD
// is now permanently mounted. All things being equal, it's
// more user-friendly to have the physical cable layout of
// your chassis correspond to the display.
//
// If, for some reason, your wiring layout differs, then...
// #define SWAP_CARS 1

// If your Hydra lacks the ground test functionality, comment this out
//#define GROUND_TEST

// If your Hydra lacks the Relay test functionality, comment this out
//#define RELAY_TEST

// If the relay test is a combined relay test and GCM, then uncomment
// this.
//#define RELAY_TESTS_GROUND

#ifdef RELAY_TESTS_GROUND
// This implies RELAY_TEST
#define RELAY_TEST
// ... and implies an alternative to GROUND_TEST
#undef GROUND_TEST
#endif

// After the relay changes state, don't bomb on relay errors for this long.
#define RELAY_TEST_GRACE_TIME 500

#ifdef GROUND_TEST

// This must be high at all times while charging either car, or else it's a ground failure.
#define GROUND_TEST_PIN 6

#endif

// ---------- DIGITAL PINS ----------
#define INCOMING_PILOT_PIN      2
#define INCOMING_PILOT_INT      0

#define INCOMING_PROXIMITY_PIN  3
#define INCOMING_PROXIMITY_INT  1

#define OUTGOING_PROXIMITY_PIN  4

#ifndef SWAP_CARS
#define CAR_A_PILOT_OUT_PIN     10
#define CAR_B_PILOT_OUT_PIN     9

#define CAR_A_RELAY             8
#define CAR_B_RELAY             7

#ifdef RELAY_TEST
#define CAR_A_RELAY_TEST        A3
#define CAR_B_RELAY_TEST        A2
#endif

// ---------- ANALOG PINS ----------
#define CAR_A_PILOT_SENSE_PIN   1
#define CAR_B_PILOT_SENSE_PIN   0
#ifdef RELAY_TEST
// When the relay test functionality was added, the current read pins moved.
#define CAR_A_CURRENT_PIN       7
#define CAR_B_CURRENT_PIN       6
#else
#define CAR_A_CURRENT_PIN       3
#define CAR_B_CURRENT_PIN       2
#endif
#else
#define CAR_A_PILOT_OUT_PIN     9
#define CAR_B_PILOT_OUT_PIN     10

#define CAR_A_RELAY             7
#define CAR_B_RELAY             8

#ifdef RELAY_TEST
#define CAR_A_RELAY_TEST        A2
#define CAR_B_RELAY_TEST        A3
#endif

// ---------- ANALOG PINS ----------
#define CAR_A_PILOT_SENSE_PIN   0
#define CAR_B_PILOT_SENSE_PIN   1
#ifdef RELAY_TEST
#define CAR_A_CURRENT_PIN       6
#define CAR_B_CURRENT_PIN       7
#else
#define CAR_A_CURRENT_PIN       2
#define CAR_B_CURRENT_PIN       3
#endif
#endif

#if 0
// This is for hardware version 0.5 only, which is now obsolete.
#define CAR_A_PILOT_SENSE_PIN   0
#define CAR_A_CURRENT_PIN       1
#define CAR_B_PILOT_SENSE_PIN   2
#define CAR_B_CURRENT_PIN       3
#endif

// for things like erroring out a car
#define BOTH                    0
#define CAR_A                   1
#define CAR_B                   2

// Don't use 0 or 1 because that's the value of LOW and HIGH.
#define HALF                    3
#define FULL                    4
#define DCOM                    5

#define STATE_A                 1
#define STATE_B                 2
#define STATE_C                 3
#define STATE_D                 4
#define STATE_E                 5
#define DUNNO                   0

// These are the expected analogRead() ranges for pilot read-back from the cars.
// These are calculated from the expected voltages seen through the dividor network,
// then scaling those voltages for 0-1024.

// 11 volts
#define STATE_A_MIN      870
// 10 volts
#define STATE_B_MAX      869
// 8 volts
#define STATE_B_MIN      775
// 7 volts
#define STATE_C_MAX      774
// 5 volts
#define STATE_C_MIN      682
// 4 volts
#define STATE_D_MAX      681
// 2 volts
#define STATE_D_MIN      610
// This represents 0 volts. No, it's not 512. Deal.
#define PILOT_0V         556
// -10 volts. We're fairly generous.
#define PILOT_DIODE_MAX  250

// This is the amount the incoming pilot needs to change for us to react (in milliamps).
#define PILOT_FUZZ 500

// This is how long we allow a car to draw more current than it is allowed before we
// error it out (in milliseconds). The spec says that a car is supposed to have 5000
// msec to respond to a pilot reduction, but it also says that we must respond to a
// state C transition within 5000 ms, so something has to give.
#define OVERDRAW_GRACE_PERIOD 4000

// This is how much "slop" we allow a car to have in terms of keeping under its current allowance.
// That is, this value (in milliamps) plus the calculated current limit is what we enforce.
#define OVERDRAW_GRACE_AMPS 1000

// The time between withdrawing the pilot from a car and disconnecting its relay (in milliseconds).
// The spec says that this must be no shorter than 3000 ms.
#define ERROR_DELAY 3000

// When a car requests state C while the other car is already in state C, we delay them for
// this long while the other car transitions to half power. THIS INTERVAL MUST BE LONGER
// THAN THE OVERDRAW_GRACE_PERIOD! (in milliseconds) The spec says it must be shorter than
// 5000 ms.
#define TRANSITION_DELAY 4500

// This is the current limit (in milliamps) of all of the components on the inlet side of the hydra -
// the inlet itself, any fuses, and the wiring to the common sides of the relays.
#define MAXIMUM_INLET_CURRENT 30000

// This is the minimum of the ampacity (in milliamps) of all of the components from the relay to the plug -
// The relay itself, the J1772 cable and plug.
#define MAXIMUM_OUTLET_CURRENT 24000

// This can not be lower than 12, because the J1772 spec bottoms out at 6A.
// The hydra won't operate properly if it can't divide the incoming power in half. (in milliamps)
#define MINIMUM_INLET_CURRENT 12000

// This is the amount of current (in milliamps) we subtract from the inlet before apportioning it to the cars.
#define INLET_CURRENT_DERATE 0

// Amount of time, in milliseconds, we will analyse the duty cycle of the incoming pilot
// Default is 200 cycles. We're doing a digitalRead(), so this will be thousands of samples.
#define PILOT_POLL_INTERVAL 25

// Amount of time, in milliseconds, we will look for positive and negative peaks on the car
// pilot pins. It takes around .1 ms to do one read, so we should get a bit less than 200 chances
// this way.
#define STATE_CHECK_INTERVAL 20

// How often (in milliseconds) is the state of both cars logged?
#define STATE_LOG_INTERVAL 60000

// This is the number of duty cycle or ammeter samples we keep to make a rolling average to stabilize
// the display. The balance here is between stability and responsiveness,
#define ROLLING_AVERAGE_SIZE 10

// The maximum number of milliseconds to sample an ammeter pin in order to find three zero-crossings.
// one and a half cycles at 50 Hz is 30 ms.
#define CURRENT_SAMPLE_INTERVAL 35

// Once we detect a zero-crossing, we should not look for one for another quarter cycle or so. 1/4
// cycle at 50 Hz is 5 ms.
#define CURRENT_ZERO_DEBOUNCE_INTERVAL 5

// How often (in milliseconds) is the current draw by a car logged?
#define CURRENT_LOG_INTERVAL 1000

// This multiplier is the number of milliamps per A/d converter unit.

// First, you need to select the burden resistor for the CT. You choose the largest value possible such that
// the maximum peak-to-peak voltage for the current range is 5 volts. To obtain this value, divide the maximum
// outlet current by the Te. That value is the maximum CT current RMS. You must convert that to P-P, so multiply
// by 2*sqrt(2). Divide 5 by that value and select the next lower standard resistor value. For the reference
// design, Te is 1018 and the outlet maximum is 30. 5/((30/1018)*2*sqrt(2)) = 59.995. Some cars, however,
// have non-sinusoidal current draws for an equivalent RMS value, but we don't want to clip those peaks,
// so a 47 ohm resistor is called for. Call this value Rb (burden resistor).

// Next, one must use Te and Rb to determine the volts-per-amp value. Note that the readCurrent()
// method calculates the RMS value before the scaling factor, so RMS need not be taken into account.
// (1 / Te) * Rb = Rb / Te = Volts per Amp. For the reference design, that's 46.169 mV.

// Each count of the A/d converter is 4.882 mV (5/1024). V/A divided by V/unit is unit/A. For the reference
// design, that's 9.46. But we want milliamps per unit, so divide that into 1000. Round up to get...
// RB = 56: Original reference design
#define CURRENT_SCALE_FACTOR 89
// RB = 47: Current reference design
//#define CURRENT_SCALE_FACTOR 106

#define LOG_NONE 0
#define LOG_INFO 1
#define LOG_DEBUG 2
#define LOG_TRACE 3

// Hardware versions 1.0 and beyond have a 6 pin FTDI compatible port laid out on the board.
// We're going to use this sort of "log4j" style. The log level is 0 for no logging at all
// (and if it's 0, the Serial won't be initialized), 1 for info level logging (which will
// simply include state transitions only), or 2 for debugging.
#define SERIAL_LOG_LEVEL LOG_INFO
#define SERIAL_BAUD_RATE 9600

// in shared mode, two cars connected simultaneously will get 50% of the incoming pilot
#define MODE_SHARED 0
// in sequential mode, the first car to enter state B gets the pilot until it transitions
// from C/D to B again. When it does, if the other car is in state B1, then it will be given
// a pilot.
#define MODE_SEQUENTIAL 1

// In sequential mode, if both cars are sitting in state B, flip the pilot back and forth between
// both cars every so often in case one of them changes their mind.
#define SEQ_MODE_OFFER_TIMEOUT (5 * 60 * 1000L) // 5 minutes

// If we add more modes, set this to the highest numbered one.
#define LAST_MODE MODE_SEQUENTIAL

// Set this to the desired startup mode
#define DEFAULT_MODE MODE_SHARED

// Which button do we use?
#define BUTTON BUTTON_SELECT
// If we see any state change on the button, we ignore all changes for this long
#define BUTTON_DEBOUNCE_INTERVAL 50
// How long does the button have to stay down before we call it a LONG push?
#define BUTTON_LONG_START 250

#define EVENT_NONE 0
#define EVENT_SHORT_PUSH 1
#define EVENT_LONG_PUSH 2

// The location in EEPROM to save the operating mode
#define EEPROM_LOC_MODE 0
// The location in EEPROM to save the (sequential mode) starting car
#define EEPROM_LOC_CAR 1

// Thanks to Gareth Evans at http://todbot.com/blog/2008/06/19/how-to-do-big-strings-in-arduino/
// Note that you must be careful not to use this macro more than once per "statement", lest you
// risk overwriting the buffer before it is used. So no using it inside methods that return
// strings that are then used in snprintf statements that themselves use this macro.
char p_buffer[96];
#define P(str) (strcpy_P(p_buffer, PSTR(str)), p_buffer)

#define VERSION "2.3 (Splitter)"

LiquidCrystal_I2C display(LCD_I2C_ADDR, 2, 1, 0, 4, 5, 6, 7, 3, POSITIVE);

unsigned long incoming_pilot_samples[ROLLING_AVERAGE_SIZE];
unsigned long car_a_current_samples[ROLLING_AVERAGE_SIZE], car_b_current_samples[ROLLING_AVERAGE_SIZE];
unsigned long incomingPilotMilliamps, lastIncomingPilot;
unsigned int last_car_a_state, last_car_b_state;
unsigned long car_a_overdraw_begin, car_b_overdraw_begin;
unsigned long car_a_request_time, car_b_request_time;
unsigned long car_a_error_time, car_b_error_time;
unsigned long last_current_log_car_a, last_current_log_car_b;
unsigned long last_state_log;
unsigned long relay_change_time;
unsigned long sequential_pilot_timeout;
unsigned int relay_state_a, relay_state_b, pilot_state_a, pilot_state_b;
unsigned int lastProximity, operatingMode, sequential_mode_tiebreak;
unsigned long button_press_time, button_debounce_time;
boolean paused = false;
#ifdef GROUND_TEST
unsigned char current_ground_status;
#endif

void log(unsigned int level, const char * fmt_str, ...) {
#if SERIAL_LOG_LEVEL > 0
  if (level > SERIAL_LOG_LEVEL) return;
  char buf[96]; // Danger, Will Robinson!
  va_list argptr;
  va_start(argptr, fmt_str);
  vsnprintf(buf, sizeof(buf), fmt_str, argptr);
  va_end(argptr);

  switch(level) {
  case LOG_INFO: 
    Serial.print(P("INFO: ")); 
    break;
  case LOG_DEBUG: 
    Serial.print(P("DEBUG: ")); 
    break;
  case LOG_TRACE:
    Serial.print(millis());
    Serial.print(P(" TRACE: "));
    break;
  default: 
    Serial.print(P("UNKNOWN: ")); 
    break;
  }
  Serial.println(buf);
#endif
}

// Delay, but pet the watchdog while doing it.
static void Delay(unsigned long ms) {
  while(ms > 100) {
    delay(100);
    wdt_reset();
    ms -= 100;
  }
  delay(ms);
  wdt_reset();
}

static void die() {
  // set both pilots to -12
  setPilot(CAR_A, LOW);
  setPilot(CAR_B, LOW);
  // make sure both relays are off
  setRelay(CAR_A, LOW);
  setRelay(CAR_B, LOW);
  // and goodnight
  do {
    wdt_reset(); // keep petting the dog, but do nothing else.
  } while(1);
}

static inline const char *car_str(unsigned int car) {
  switch(car) {
    case CAR_A: return "car A";
    case CAR_B: return "car B";
    case BOTH: return "both car";
    default: return "UNKNOWN";
  }
}

static inline const char *logic_str(unsigned int state) {
  switch(state) {
    case LOW: return "LOW";
    case HIGH: return "HIGH";
    case HALF: return "HALF";
    case FULL: return "FULL";
    case DCOM: return "DCOM";
    default: return "UNKNOWN";
  }
}

static inline const char* state_str(unsigned int state) {
  switch(state) {
  case STATE_A: 
    return "A";
  case STATE_B: 
    return "B";
  case STATE_C: 
    return "C";
  case STATE_D: 
    return "D";
  case STATE_E: 
    return "E";
  default: 
    return "UNKNOWN";
  }
}

// Deal in milliamps so that we don't have to use floating point.
// Convert the microsecond state timings from the incoming pilot into
// an instantaneous current allowance value. With polling, it's
// the two sample counts, but the math winds up being exactly the same.
static inline unsigned long timeToMA(unsigned long samplesHigh, unsigned long samplesLow) {
  // Calculate the duty cycle in mils (tenths of a percent)
  unsigned int duty = (samplesHigh * 1000) / (samplesHigh + samplesLow);
  if (duty < 80) { // < 8% is an error (digital comm not supported)
    return 0;
  } else if (duty <= 100) { // 8-10% is 6A - tolerance grace
    return 6000L;
  } else if (duty <= 850) { // 10-85% uses the "low" function
    return duty * 60L;
  } else if (duty <= 960) { // 85-96% uses the "high" function
    return (duty - 640) * 250L;
  } else if (duty <= 980) { // 96-98% is 80A - tolerance grace
    return 80000L;
  } else { // > 98% is an error
    return 0;
  }
}

// Convert a milliamp allowance into an outgoing pilot duty cycle.
// In lieu of floating point, this is duty in mils (tenths of a percent)
static inline unsigned int MAToDuty(unsigned long milliamps) {
  if (milliamps < 6000) {
    return 9999; // illegal - set pilot to "high"
  } 
  else if (milliamps < 51000) {
    return milliamps/60;
  } 
  else if (milliamps <= 80000) {
    return (milliamps / 250) + 640;
  } 
  else {
    return 9999; // illegal - set pilot to "high"
  }
}

// Convert a milliamp allowance into a value suitable for
// pwmWrite - a scale from 0 to 255.
static inline unsigned int MAtoPwm(unsigned long milliamps) {
  unsigned int out = MAToDuty(milliamps);

  if (out >= 1000) return 255; // full on

  out = (unsigned int)((out * 256L) / 1000);  

  return out;
}

// Turn a millamp value into nn.n as amps, with the tenth rounded near.
char *formatMilliamps(unsigned long milliamps) {
  static char out[6];

  if (milliamps < 1000) {
    // truncate the units digit - there's no way we're that accurate.
    milliamps /= 10;
    milliamps *= 10;

    sprintf(out, P("%3lumA"), milliamps);
  } 
  else {
    int hundredths = (milliamps / 10) % 100;
    int tenths = hundredths / 10 + (((hundredths % 10) >= 5)?1:0);
    int units = milliamps / 1000;
    if (tenths >= 10) {
      tenths -= 10;
      units++;
    }

    sprintf(out, P("%2d.%01dA"), units, tenths);
  }

  return out;
}

void error(unsigned int car, char err) {
  unsigned long now = millis(); // so both cars get the same time.
  // Set the pilot to constant 12: indicates an EVSE error.
  // We can't use -12, because then we'd never detect a return
  // to state A. But the spec says we're allowed to return to B1
  // (that is, turn off the oscillator) whenever we want.
  
  // Stop flipping, one way or another
  sequential_pilot_timeout = 0;
  if (car == BOTH || car == CAR_A) {
    setPilot(CAR_A, HIGH);
    last_car_a_state = STATE_E;
    car_a_error_time = now;
    car_a_request_time = 0;
  }
  if (car == BOTH || car == CAR_B) {
    setPilot(CAR_B, HIGH);
    last_car_b_state = STATE_E;
    car_b_error_time = now;
    car_b_request_time = 0;
  }
  
  //display.setBacklight(RED);
  if (car == BOTH || car == CAR_A) {
    display.setCursor(0, 1);
    display.print(P("A:ERR "));
    display.print(err);
    display.print(' ');
  }
  if (car == BOTH || car == CAR_B) {
    display.setCursor(8, 1);
    display.print(P("B:ERR "));
    display.print(err);
    display.print(' ');
  }

  log(LOG_INFO, P("Error %c on %s"), err, car_str(car));
}

void setRelay(unsigned int car, unsigned int state) {
  log(LOG_DEBUG, P("Setting %s relay to %s"), car_str(car), logic_str(state));
  switch(car) {
  case CAR_A:
    if (relay_state_a == state) return; // Nothing changed
    digitalWrite(CAR_A_RELAY, state);
    relay_state_a = state;
    break;
  case CAR_B:
    if (relay_state_b == state) return; // Nothing changed
    digitalWrite(CAR_B_RELAY, state);
    relay_state_b = state;
    break;
  }
  relay_change_time = millis();
}

// If it's in an error state, it's not charging (the relay may still be on during error delay).
// If it's in a transition delay, then it's "charging" (the relay is off during transition delay).
// Otherwise, check the state of the relay.
static inline boolean isCarCharging(unsigned int car) {
  if (paused) return false;
  switch(car) {
  case CAR_A:
    if (last_car_a_state == STATE_E) return LOW;
    if (car_a_request_time != 0) return HIGH;
    return relay_state_a;
    break;
  case CAR_B:
    if (last_car_b_state == STATE_E) return LOW;
    if (car_b_request_time != 0) return HIGH;
    return relay_state_b;
    break;
  default:
    return LOW; // This should not be possible
  }
}

// Set the pilot for the car as appropriate. 'which' is either HALF, FULL, LOW or HIGH.
// HIGH sets a constant +12v, which is the spec for state A, but we also use it for
// state E. HALF means that the other car is charging, so we only can have half power.

void setPilot(unsigned int car, unsigned int which) {
  log(LOG_DEBUG, P("Setting %s pilot to %s"), car_str(car), logic_str(which));
  // set the outgoing pilot for the given car to either HALF state, FULL state, or HIGH.
  int pin;
  switch(car) {
    case CAR_A:
      pin = CAR_A_PILOT_OUT_PIN;
      pilot_state_a = which;
      break;
    case CAR_B:
      pin = CAR_B_PILOT_OUT_PIN;
      pilot_state_b = which;
      break;
    default: return;
  }
  if (which == LOW || which == HIGH) {
    // This is what the pwm library does anyway.
    log(LOG_TRACE, P("Pin %d to digital %d"), pin, which);
    digitalWrite(pin, which);
  }
  else if (1){//FIXME (which == DCOM){
    digitalWrite(pin,12);   
  }
  else {
    unsigned long ma = incomingPilotMilliamps;
    if (which == HALF) ma /= 2;
    if (ma > MAXIMUM_OUTLET_CURRENT) ma = MAXIMUM_OUTLET_CURRENT;
    unsigned int val = MAtoPwm(ma);
    log(LOG_TRACE, P("Pin %d to PWM %d"), pin, val);
    pwmWrite(pin, val);
  }
}

static inline unsigned int pilotState(unsigned int car) {
  return (car == CAR_A)?pilot_state_a:pilot_state_b;
}

int checkState(unsigned int car) {
  // poll the pilot state pin for 10 ms (should be 10 pilot cycles), looking for the low and high.
  unsigned int low = 9999, high = 0;
  unsigned int car_pin = (car == CAR_A) ? CAR_A_PILOT_SENSE_PIN : CAR_B_PILOT_SENSE_PIN;
  unsigned long count = 0;
  for(unsigned long start = millis(); millis() - start < STATE_CHECK_INTERVAL; ) {
    unsigned int val = analogRead(car_pin);
    if (val > high) high = val;
    if (val < low) low = val;
    count++;
  }

  log(LOG_TRACE, P("%s high %u low %u count %lu"), car_str(car), high, low, count);
  
  // If the pilot low was below zero, then that means we must have
  // been oscillating. If we were, then perform the diode check.
  if (low < PILOT_0V && low > PILOT_DIODE_MAX) {
    return STATE_E; // diode check fail
  }
  if (high >= STATE_A_MIN) {
    return STATE_A;
  } 
  else if (high >= STATE_B_MIN && high <= STATE_B_MAX) {
    return STATE_B;
  } 
  else if (high >= STATE_C_MIN && high <= STATE_C_MAX) {
    return STATE_C;
  } 
  else if (high >= STATE_D_MIN && high <= STATE_D_MAX) {
    return STATE_D;
  }
  // I dunno how we got here, but we fail it.
  return STATE_E;
}

static inline unsigned long ulong_sqrt(unsigned long in) {
  unsigned long out;
  // find the last int whose square is not too big
  // Yes, it's wasteful, but we only theoretically ever have to go to 512.
  // Removing floating point saves us almost 1K of flash.
  for(out = 1; out*out <= in; out++) ;
  return out - 1;
}

unsigned long readCurrent(unsigned int car) {
  unsigned int car_pin = (car == CAR_A) ? CAR_A_CURRENT_PIN : CAR_B_CURRENT_PIN;
  unsigned long sum = 0;
  unsigned int zero_crossings = 0;
  unsigned long last_zero_crossing_time = 0, now_ms;
  long last_sample = -1; // should be impossible - the A/d is 0 to 1023.
  unsigned int sample_count = 0;
  for(unsigned long start = millis(); (now_ms = millis()) - start < CURRENT_SAMPLE_INTERVAL; ) {
    long sample = analogRead(car_pin);
    // If this isn't the first sample, and if the sign of the value differs from the
    // sign of the previous value, then count that as a zero crossing.
    if (last_sample != -1 && ((last_sample > 512) != (sample > 512))) {
      // Once we've seen a zero crossing, don't look for one for a little bit.
      // It's possible that a little noise near zero could cause a two-sample
      // inversion.
      if (now_ms - last_zero_crossing_time > CURRENT_ZERO_DEBOUNCE_INTERVAL) {
        zero_crossings++;
        last_zero_crossing_time = now_ms;
      }
    }
    last_sample = sample;
    switch(zero_crossings) {
    case 0: 
      continue; // Still waiting to start sampling
    case 1:
    case 2:
      // Gather the sum-of-the-squares and count how many samples we've collected.
      sum += (unsigned long)((sample - 512) * (sample - 512));
      sample_count++;
      continue;
    case 3:
      // The answer is the square root of the mean of the squares.
      // But additionally, that value must be scaled to a real current value.
      return ulong_sqrt(sum / sample_count) * CURRENT_SCALE_FACTOR;
    }
  }
  // ran out of time. Assume that it's simply not oscillating any. 
  return 0;
}

unsigned long rollRollingAverage(unsigned long array[], unsigned long new_value) {
#if ROLLING_AVERAGE_SIZE == 0
  return new_value;
#else
  unsigned long sum = new_value;
  for(int i = ROLLING_AVERAGE_SIZE - 1; i >= 1; i--) {
    array[i] = array[i - 1];
    sum += array[i];
  }
  array[0] = new_value;
  return (sum / ROLLING_AVERAGE_SIZE);
#endif
}

static inline void reportIncomingPilot(unsigned long milliamps) {

  milliamps = rollRollingAverage(incoming_pilot_samples, milliamps);
  // Clamp to the maximum allowable current
  if (milliamps > MAXIMUM_INLET_CURRENT) milliamps = MAXIMUM_INLET_CURRENT;

  milliamps -= INLET_CURRENT_DERATE;

  incomingPilotMilliamps = milliamps;
}

void pollIncomingPilot() {
  unsigned long transitions = -1; // ignore the first "transition"
  int last_state = 99; // neither high nor low
  unsigned long high_count = 0, low_count = 0;
  for(unsigned long start = millis(); millis() - start < PILOT_POLL_INTERVAL; ) {
    int state = digitalRead(INCOMING_PILOT_PIN);
    if (state != last_state) transitions++;
    last_state = state;
    if (state == HIGH)
      high_count++;
    else
      low_count++;
  }
  
  // We assume that the PILOT_POLL_INTERVAL is less than a second, so multiply by the
  // reciprocal of the "seconds" of polling
  unsigned long hz = (transitions / 2) * (1000 / PILOT_POLL_INTERVAL);
  
  // The spec allows 20% grace for frequency precision.
  if (hz < 800 || hz > 1200) {
    reportIncomingPilot(0);
    return;
  }

  unsigned long milliamps = timeToMA(high_count, low_count);

  reportIncomingPilot(milliamps);

}

void z   unsigned int us, unsigned int car_state) {
  unsigned int them = (us == CAR_A)?CAR_B:CAR_A;
  unsigned int *last_car_state = (us == CAR_A)?&last_car_a_state:&last_car_b_state;
  unsigned int their_state = (us == CAR_A)?last_car_b_state:last_car_a_state;
  
  switch(car_state) {
    case STATE_A:
      // No matter what, insure that the pilot and relay are off.
      setRelay(us, LOW);
      setPilot(us, HIGH);
      // We're not both in state B anymore.
      sequential_pilot_timeout = 0;
      // We don't exist. If they're waiting, they can have it.
      if (their_state == STATE_B)
      {
          setPilot(them, FULL);
          EEPROM.write(EEPROM_LOC_CAR, them);
          display.setCursor((them == CAR_A)?0:8, 1);
          display.print((them == CAR_A)?"A":"B");
          display.print(P(": off  "));
      }
      display.setCursor((us == CAR_A)?0:8, 1);
      display.print((us == CAR_A)?"A":"B");
      display.print(P(": ---  "));
      break;
    case STATE_B:
      // No matter what, insure that the relay is off.
      setRelay(us, LOW);
      if (*last_car_state == STATE_C || *last_car_state == STATE_D) {
        // We transitioned from C/D to B. That means we're passing the batton
        // to the other car, if they want it.
        if (their_state == STATE_B) {
          setPilot(them, FULL);
          setPilot(us, HIGH);
          EEPROM.write(EEPROM_LOC_CAR, them);
          display.setCursor((them == CAR_A)?0:8, 1);
          display.print((them == CAR_A)?"A":"B");
          display.print(P(": off  "));
          display.setCursor((us == CAR_A)?0:8, 1);
          display.print((us == CAR_A)?"A":"B");
          display.print(P(": done ")); // differentiated from "wait" because a C/D->B transition has occurred.
          sequential_pilot_timeout = millis(); // We're both now in B. Start flipping.
        } else {
          display.setCursor((us == CAR_A)?0:8, 1);
          display.print((us == CAR_A)?"A":"B");
          display.print(P(": off  "));
          // their state is not B, so we're not "flipping"
          sequential_pilot_timeout = 0;
        }
      } else {
        if (their_state == STATE_A) {
          // We can only grab the batton if they're not plugged in at all.
          setPilot(us, FULL);
          sequential_pilot_timeout = 0;
          EEPROM.write(EEPROM_LOC_CAR, us);
          display.setCursor((us == CAR_A)?0:8, 1);
          display.print((us == CAR_A)?"A":"B");
          display.print(P(": off  "));
          break;
        } else if (their_state == STATE_B || their_state == DUNNO) {
          // BUT if we're *both* in state b, then that's a tie. We break the tie with our saved tiebreak value.
          // If it's not us, then we simply ignore this transition entirely. The other car will wind up in this same place,
          // we'll turn their pilot on, and then clear the tiebreak. Next time we roll through, we'll go into the other
          // half of this if/else and we'll get the "wait" display
          if (sequential_mode_tiebreak != us && sequential_mode_tiebreak != DUNNO) {
            return;
          }
          // But if it IS us, then clear the tiebreak.
          if (sequential_mode_tiebreak == us) {
            sequential_mode_tiebreak = DUNNO;
            setPilot(us, FULL);
            sequential_pilot_timeout = millis();
            EEPROM.write(EEPROM_LOC_CAR, us);
            display.setCursor((us == CAR_A)?0:8, 1);
            display.print((us == CAR_A)?"A":"B");
            display.print(P(": off  "));
            break;
          }
        }
        // Either they are in state C/D or they're in state B and we lost the tiebreak.
        display.setCursor((us == CAR_A)?0:8, 1);
        display.print((us == CAR_A)?"A":"B");
        display.print(P(": wait "));
      }
      break;
    case STATE_C:
    case STATE_D:
      if (isCarCharging(us)) {
        // we're already charging. This might be a flip from C to D. Ignore it.
        break;
      }
      if (pilotState(us) != FULL) {
        error(us, 'T'); // illegal transition: no state C without a pilot
        return;
      }
      // We're not both in state B anymore
      sequential_pilot_timeout = 0;
      setRelay(us, HIGH); // turn on the juice
      display.setCursor((us == CAR_A)?0:8, 1);
      display.print((us == CAR_A)?"A":"B");
      display.print(P(": ON   "));
      break;
    case STATE_E:
      error(us, 'E');
      return;
  }
  *last_car_state = car_state;
}

void shared_mode_transition(unsigned int us, unsigned int car_state) {
  unsigned int them = (us == CAR_A)?CAR_B:CAR_A;
  unsigned int *last_car_state = (us == CAR_A)?&last_car_a_state:&last_car_b_state;
  unsigned long *car_request_time = (us == CAR_A)?&car_a_request_time:&car_b_request_time;
    
  *last_car_state = car_state;    
  switch(car_state) {
    case STATE_A:
    case STATE_B:
      // We're in an "off" state of one sort or other.
      // In either case, clear any connection delay timer,
      // make sure the relay is off, and set the diplay
      // appropriately. For state A, set our pilot high,
      // and fot state B, set it to half if the other car
      // is charging, full otherwise.
      setRelay(us, LOW);
      setPilot(us, car_state == STATE_A ? HIGH : (isCarCharging(them)?HALF:FULL));
      display.setCursor((us == CAR_A)?0:8, 1);
      display.print((us == CAR_A)?"A":"B");
      display.print(car_state == STATE_A ? ": ---  " : ": off  ");
      *car_request_time = 0;
      if (pilotState(them) == HALF)
        setPilot(them, FULL);
      break;
    case STATE_C:
    case STATE_D:
      if (isCarCharging(us)) {
        // we're already charging. This might be a flip from C to D. Ignore it.
        break;
      }
      if (isCarCharging(them)) {
        // if they are charging, we must transition them.
        *car_request_time = millis();
        // Drop car A down to 50%
        setPilot(them, HALF);
        setPilot(us, HALF); // this is redundant unless we are transitioning from A directly to C
        display.setCursor((us == CAR_A)?0:8, 1);
        display.print((us == CAR_A)?"A":"B");
        display.print(": wait ");
      } else {
        // if they're not charging, then we can just go. If they have a full pilot, they get downshifted.
        if (pilotState(them) == FULL)
          setPilot(them, HALF);
        setPilot(us, FULL); // this is redundant unless we are transitioning from A directly to C
        setRelay(us, HIGH);
        *car_request_time = 0;
        display.setCursor((us == CAR_A)?0:8, 1);
        display.print((us == CAR_A)?"A":"B");
        display.print(P(": ON   "));
      }
      break;
    case STATE_E:
      error(us, 'E');
      break;
  }
}

unsigned int checkEvent() {
  return EVENT_NONE;

#if 0
  log(LOG_TRACE, P("Checking for button event"));
  if (button_debounce_time != 0 && millis() - button_debounce_time < BUTTON_DEBOUNCE_INTERVAL) {
    // debounce is in progress
    return EVENT_NONE;
  } else {
    // debounce is over
    button_debounce_time = 0;
  }
  unsigned int buttons = display.readButtons();
  log(LOG_TRACE, P("Buttons %d"), buttons);
  if ((buttons & BUTTON) != 0) {
    log(LOG_TRACE, P("Button is down"));
    // Button is down
    if (button_press_time == 0) { // this is the start of a press.
      button_debounce_time = button_press_time = millis();
    }
    return EVENT_NONE; // We don't know what this button-push is going to be yet
  } else {
    log(LOG_TRACE, P("Button is up"));
    // Button released
    if (button_press_time == 0) return EVENT_NONE; // It wasn't down anyway.
    // We are now ending a button-push. First, start debuncing.
    button_debounce_time = millis();
    unsigned long button_pushed_time = button_debounce_time - button_press_time;
    button_press_time = 0;
    if (button_pushed_time > BUTTON_LONG_START) {
      log(LOG_DEBUG, P("Button long-push event"));
      return EVENT_LONG_PUSH;
    } else {
      log(LOG_DEBUG, P("Button short-push event"));
      return EVENT_SHORT_PUSH;
    }
  }
  #endif
}

void setup() {
  // This must be done as early as possible to prevent the watchdog from biting during reset.
  MCUSR = 0;
  wdt_enable(WDTO_1S);

  InitTimersSafe();
//  display.setMCPType(LTI_TYPE_MCP23017);
  display.begin(16, 2); 

#if SERIAL_LOG_LEVEL > 0
  Serial.begin(SERIAL_BAUD_RATE);
#endif

  log(LOG_DEBUG, P("Starting v%s"), VERSION);
  
  pinMode(INCOMING_PILOT_PIN, INPUT_PULLUP);
  pinMode(INCOMING_PROXIMITY_PIN, INPUT_PULLUP);
  pinMode(OUTGOING_PROXIMITY_PIN, OUTPUT);
  pinMode(CAR_A_PILOT_OUT_PIN, OUTPUT);
  pinMode(CAR_B_PILOT_OUT_PIN, OUTPUT);
  pinMode(CAR_A_RELAY, OUTPUT);
  pinMode(CAR_B_RELAY, OUTPUT);
#ifdef GROUND_TEST
  pinMode(GROUND_TEST_PIN, INPUT);
#endif
#ifdef RELAY_TEST
  pinMode(CAR_A_RELAY_TEST, INPUT);
  pinMode(CAR_B_RELAY_TEST, INPUT);
#endif

  digitalWrite(OUTGOING_PROXIMITY_PIN, LOW);

  // Enter state A on both cars
  setPilot(CAR_A, HIGH);
  setPilot(CAR_B, HIGH);
  // And make sure the power is off.
  setRelay(CAR_A, LOW);
  setRelay(CAR_B, LOW);

  memset(car_a_current_samples, 0, sizeof(car_a_current_samples));
  memset(car_b_current_samples, 0, sizeof(car_b_current_samples));
  last_car_a_state = DUNNO;
  last_car_b_state = DUNNO;
  car_a_request_time = 0;
  car_b_request_time = 0;
  car_a_overdraw_begin = 0;
  car_b_overdraw_begin = 0;
  last_current_log_car_a = 0;
  last_current_log_car_b = 0;
  lastProximity = HIGH;
  button_debounce_time = 0;
  button_press_time = 0;
  sequential_pilot_timeout = 0;
  relay_change_time = 0;
#ifdef GROUND_TEST
  current_ground_status = 0;
#endif
  operatingMode = EEPROM.read(EEPROM_LOC_MODE);
  if (operatingMode > LAST_MODE) {
    operatingMode = DEFAULT_MODE;
    EEPROM.write(EEPROM_LOC_MODE, operatingMode);
  }
  if (operatingMode == MODE_SEQUENTIAL) {
    sequential_mode_tiebreak = EEPROM.read(EEPROM_LOC_CAR);
    if (sequential_mode_tiebreak != CAR_A && sequential_mode_tiebreak != CAR_B)
      sequential_mode_tiebreak = DUNNO;
  }

//  display.setBacklight(WHITE);
  display.clear();
  display.setCursor(0, 0);
  display.print(P("J1772 Hydra"));
  display.setCursor(0, 1);
  display.print(P(VERSION));

  boolean success = SetPinFrequencySafe(CAR_A_PILOT_OUT_PIN, 1000);
  if (!success) {
    log(LOG_INFO, P("SetPinFrequency for car A failed!"));
    //display.setBacklight(YELLOW);
  }
  success = SetPinFrequencySafe(CAR_B_PILOT_OUT_PIN, 1000);
  if (!success) {
    log(LOG_INFO, P("SetPinFrequency for car B failed!"));
    ////display.setBacklight(BLUE);
  }
  // In principle, neither of the above two !success conditions should ever
  // happen.

#ifdef RELAY_TEST
  {
    boolean test_a = digitalRead(CAR_A_RELAY_TEST) == HIGH;
    boolean test_b = digitalRead(CAR_B_RELAY_TEST) == HIGH;
    if (test_a || test_b) {
      //display.setBacklight(RED);
      display.clear();
      display.print(P("Relay Test Failure: "));
      if (test_a) display.print('A');
      if (test_b) display.print('B');
      die(); // and goodnight
    }
  }
#endif

  // meanwhile, the fill in the incoming pilot rolling average...
  for(int i = 0; i < ROLLING_AVERAGE_SIZE; i++) {
    pollIncomingPilot();
  }
  lastIncomingPilot = incomingPilotMilliamps;

  // Display the splash screen for 2 seconds total. We spent some time above sampling the
  // pilot, so don't include that.
  Delay(2000 - (ROLLING_AVERAGE_SIZE * PILOT_POLL_INTERVAL)); // let the splash screen show
  display.clear();
}

void loop() {

  // Pet the dog
  wdt_reset();
  
#ifdef GROUND_TEST
  if ((relay_state_a == HIGH || relay_state_b == HIGH) && relay_change_time == 0) {
    unsigned char ground = digitalRead(GROUND_TEST_PIN) == HIGH;
    if (ground != current_ground_status) {
      current_ground_status = ground;
      if (!ground) {
        // we've just noticed a ground failure.
        log(LOG_INFO, P("Ground failure detected"));
        error(BOTH, 'F');
      }
    }
  } else {
    current_ground_status = 2; // Force the check to take place
  }
#endif

#ifdef RELAY_TEST
  if (relay_change_time == 0) {
    // The relay is off, but the relay test shows a voltage, that's a stuck relay
    if ((digitalRead(CAR_A_RELAY_TEST) == HIGH) && (relay_state_a == LOW)) {
      log(LOG_INFO, P("Relay fault detected on car A"));
      error(CAR_A, 'R');    
    }
    if ((digitalRead(CAR_B_RELAY_TEST) == HIGH) && (relay_state_b == LOW)) {
      log(LOG_INFO, P("Relay fault detected on car B"));
      error(CAR_B, 'R');
    }
#ifdef RELAY_TESTS_GROUND
    // If the relay is on, but the relay test does not show a voltage, that's a ground impedance failure
    if ((digitalRead(CAR_A_RELAY_TEST) == LOW) && (relay_state_a == HIGH)) {
      log(LOG_INFO, P("Ground failure detected on car A"));
      error(CAR_A, 'F');    
    }
    if ((digitalRead(CAR_B_RELAY_TEST) == LOW) && (relay_state_b == HIGH)) {
      log(LOG_INFO, P("Ground failure detected on car B"));
      error(CAR_B, 'F);
    }
#endif
  }
#endif
  if (millis() > relay_change_time + RELAY_TEST_GRACE_TIME)
    relay_change_time = 0;

  // cut down on how frequently we call millis()
  boolean proximityOrPilotError = false;
  
  // Update the display

  if (last_car_a_state == STATE_E || last_car_b_state == STATE_E) {
    // One or both cars in error state
    //display.setBacklight(RED);
  } 
  else {
    boolean a = isCarCharging(CAR_A);
    boolean b = isCarCharging(CAR_B);

    // Neither car
//    if (!a && !b) display.setBacklight(paused?YELLOW:GREEN);
    // Both cars
//    else if (a && b) display.setBacklight(VIOLET);
    // One car or the other
//    else if (a ^ b) display.setBacklight(TEAL);
  }

  // Check proximity
  unsigned int proximity = digitalRead(INCOMING_PROXIMITY_PIN);
  if (proximity != lastProximity) {
    if (proximity != HIGH) {

      log(LOG_INFO, P("Incoming proximity disconnect"));
      
      // EVs are supposed to react to a proximity transition much faster than
      // an error transition.
      digitalWrite(OUTGOING_PROXIMITY_PIN, HIGH);

      display.setCursor(0, 0);
      display.print(P("DISCONNECTING..."));
      error(BOTH, 'P');
    } 
    else {
      log(LOG_INFO, P("Incoming proximity restore"));
      // Clear out "Disconnecting..."
      display.setCursor(0, 0);
      display.print(P("                "));
      
      // In case someone pushed the button and changed their mind
      digitalWrite(OUTGOING_PROXIMITY_PIN, LOW);
    }
  }
  lastProximity = proximity;
  if (proximity != HIGH) proximityOrPilotError = true;

  pollIncomingPilot();
  if (!proximityOrPilotError && incomingPilotMilliamps < MINIMUM_INLET_CURRENT) {
    if (!paused) {
      if (operatingMode == MODE_SEQUENTIAL) {
        // remember which car was active
        if (pilot_state_a == FULL) sequential_mode_tiebreak = CAR_A;
        else if (pilot_state_b == FULL) sequential_mode_tiebreak = CAR_B;
        else sequential_mode_tiebreak = DUNNO;
      }
      // Turn off both pilots
      setPilot(CAR_A, HIGH);
      setPilot(CAR_B, HIGH);
      unsigned long now = millis();
      car_a_error_time = now;
      car_b_error_time = now;
      last_car_a_state = DUNNO;
      last_car_b_state = DUNNO;
      car_a_request_time = 0;
      car_b_request_time = 0;      
      log(LOG_INFO, P("Incoming pilot invalid. Pausing."));
      display.setCursor(0, 0);
      display.print(P("I:PAUSE "));
    }
    paused = true;
    // Forget it. Nothing else is worth doing as long as the input pilot continues to be gone.
    proximityOrPilotError = true;
  } else {
    paused = false;
  }

  if(paused || !proximityOrPilotError) {
    if (!paused) {
      display.setCursor(0, 0);
      display.print(P("I:"));
      display.print(formatMilliamps(incomingPilotMilliamps));
    }
    display.setCursor(8, 0);
    display.print(P("M:"));
    switch(operatingMode) {
      case MODE_SHARED:
        display.print(P("shared")); break;
      case MODE_SEQUENTIAL:
        display.print(P("seqntl")); break;
      default:
        display.print(P("UNK")); break;
    }
  }

  // Adjust the pilot levels to follow any changes in the incoming pilot
  unsigned long fuzz = labs(incomingPilotMilliamps - lastIncomingPilot);
  if (fuzz > PILOT_FUZZ) {
    log(LOG_INFO, P("Detected incoming pilot fuzz of %lu mA"), fuzz);
    switch(pilot_state_a) {
      case HALF: setPilot(CAR_A, HALF); break;
      case FULL: setPilot(CAR_A, FULL); break;
    }
    switch(pilot_state_b) {
      case HALF: setPilot(CAR_B, HALF); break;
      case FULL: setPilot(CAR_B, FULL); break;
    }
    lastIncomingPilot = incomingPilotMilliamps;
  }

  // Check the pilot sense on each car.
  unsigned int car_a_state = checkState(CAR_A);

  if (paused || last_car_a_state == STATE_E) {
    switch(car_a_state) {
    case STATE_A:
      // we were in error, but the car's been disconnected.
      // If we still have a pilot or proximity error, then
      // we can't clear the error.
      if (!proximityOrPilotError) {
      // If not, clear the error state. The next time through
      // will take us back to state A.
        last_car_a_state = DUNNO;
        log(LOG_INFO, P("Car A disconnected, clearing error"));
      }
      if (paused) {
          display.setCursor(0, 1);
          display.print("A: ---  ");
      }
      // fall through...
    case STATE_B:
      // If we see a transition to state B, the error is still in effect, but complete (and
      // cancel) any pending relay opening.
      if (car_a_error_time != 0) {
        car_a_error_time = 0;
        setRelay(CAR_A, LOW);
        if (isCarCharging(CAR_B) || last_car_b_state == STATE_B)
          setPilot(CAR_B, FULL);
      }
      if (paused && car_a_state == STATE_B) {
        display.setCursor(0, 1);
        display.print("A: off  ");
      }
      break;
    }
  } else if (car_a_state != last_car_a_state) {
    if (last_car_a_state != DUNNO)
      log(LOG_INFO, P("Car A state transition: %s->%s."), state_str(last_car_a_state), state_str(car_a_state));
    switch(operatingMode) {
      case MODE_SHARED:
        shared_mode_transition(CAR_A, car_a_state);
        break;
      case MODE_SEQUENTIAL:
        sequential_mode_transition(CAR_A, car_a_state);
        break;
    }
  }

  unsigned int car_b_state = checkState(CAR_B);
  if (paused || last_car_b_state == STATE_E) {
    switch(car_b_state) {
    case STATE_A:
      // we were in error, but the car's been disconnected.
      // If we still have a pilot or proximity error, then
      // we can't clear the error.
      if (!proximityOrPilotError) {
      // If not, clear the error state. The next time through
      // will take us back to state A.
        last_car_b_state = DUNNO;
        log(LOG_INFO, P("Car B disconnected, clearing error"));
      }
      if (paused) {
          display.setCursor(8, 1);
          display.print("B: ---  ");
      }
      // fall through...
    case STATE_B:
      // If we see a transition to state B, the error is still in effect, but complete (and
      // cancel) any pending relay opening.
      if (car_b_error_time != 0) {
        car_b_error_time = 0;
        setRelay(CAR_B, LOW);
        if (isCarCharging(CAR_A) || last_car_a_state == STATE_B)
          setPilot(CAR_A, FULL);
      }
      if (paused && car_b_state == STATE_B) {
        display.setCursor(8, 1);
        display.print("B: off  ");
      }
      break;
    }
  } else if (car_b_state != last_car_b_state) {
    if (last_car_b_state != DUNNO)
      log(LOG_INFO, P("Car B state transition: %s->%s."), state_str(last_car_b_state), state_str(car_b_state));
    switch(operatingMode) {
      case MODE_SHARED:
        shared_mode_transition(CAR_B, car_b_state);
        break;
      case MODE_SEQUENTIAL:
        sequential_mode_transition(CAR_B, car_b_state);
        break;
    }
  }
  if (sequential_pilot_timeout != 0) {
    unsigned long now = millis();
    if (now - sequential_pilot_timeout > SEQ_MODE_OFFER_TIMEOUT) {
      if (pilot_state_a == FULL) {
        log(LOG_INFO, P("Sequential mode offer timeout, moving offer to %s"), car_str(CAR_B));
        setPilot(CAR_A, HIGH);
        setPilot(CAR_B, FULL);
        sequential_pilot_timeout = now;
        display.setCursor(0, 1);
        display.print("A: wait B: off  ");
      } else if (pilot_state_b == FULL) {
        log(LOG_INFO, P("Sequential mode offer timeout, moving offer to %s"), car_str(CAR_A));
        setPilot(CAR_B, HIGH);
        setPilot(CAR_A, FULL);
        sequential_pilot_timeout = now;
        display.setCursor(0, 1);
        display.print("A: off  B: wait ");
      }
    }
  }
  
  {
    unsigned long now = millis();
    if (now - last_state_log > STATE_LOG_INTERVAL) {
      last_state_log = now;
      log(LOG_INFO, P("States: Car A, %s; Car B, %s"), state_str(last_car_a_state), state_str(last_car_b_state));
      log(LOG_INFO, P("Incoming pilot %s"), formatMilliamps(incomingPilotMilliamps));
    }
  }
   
  // Update the ammeter display and check for overdraw conditions.
  // We allow a 5 second grace because the J1772 spec requires allowing
  // the car 5 seconds to respond to incoming pilot changes.
  // If the overdraw condition is acute enough, we'll be blowing fuses
  // in hardware, so this isn't as dire a condition as it sounds.
  // More likely what it means is that the car hasn't reacted to an
  // attempt to reduce it to half power (and the other car has not yet
  // been turned on), so we must error them out before letting the other
  // car start.
  if (relay_state_a == HIGH && last_car_a_state != STATE_E) { // Only check the ammeter if the power is actually on and we're not errored
    unsigned long car_a_draw = readCurrent(CAR_A);

    {
      unsigned long now = millis();
      if (now - last_current_log_car_a > CURRENT_LOG_INTERVAL) {
        last_current_log_car_a = now;
        log(LOG_INFO, P("Car A current draw %lu mA"), car_a_draw);
      }
    }
    
    // If the other car is charging, then we can only have half power
    unsigned long car_a_limit = incomingPilotMilliamps / ((pilotState(CAR_A) == HALF)? 2 : 1);

    if (car_a_draw > car_a_limit + OVERDRAW_GRACE_AMPS) {
      // car A has begun an over-draw condition. They have 5 seconds of grace before we pull the plug.
      if (car_a_overdraw_begin == 0) {
        car_a_overdraw_begin = millis();
      } 
      else {
        if (millis() - car_a_overdraw_begin > OVERDRAW_GRACE_PERIOD) {
          error(CAR_A, 'O');
          return;
        }
      }
    }
    else {
      // car A is under its limit. Cancel any overdraw in progress
      car_a_overdraw_begin = 0;
    }
    display.setCursor(0, 1);
    display.print("A:");
    display.print(formatMilliamps(car_a_draw));
  } 
  else {
    // Car A is not charging
    car_a_overdraw_begin = 0;
    memset(car_b_current_samples, 0, sizeof(car_b_current_samples));
  }

  if (relay_state_b == HIGH && last_car_b_state != STATE_E) { // Only check the ammeter if the power is actually on and we're not errored
    unsigned long car_b_draw = readCurrent(CAR_B);

    {
      unsigned long now = millis();
      if (now - last_current_log_car_b > CURRENT_LOG_INTERVAL) {
        last_current_log_car_b = now;
        log(LOG_INFO, P("Car B current draw %lu mA"), car_b_draw);
      }
    }
    
    // If the other car is charging, then we can only have half power
    unsigned long car_b_limit = incomingPilotMilliamps / ((pilotState(CAR_B) == HALF) ? 2 : 1);

    if (car_b_draw > car_b_limit + OVERDRAW_GRACE_AMPS) {
      // car B has begun an over-draw condition. They have 5 seconds of grace before we pull the plug.
      if (car_b_overdraw_begin == 0) {
        car_b_overdraw_begin = millis();
      } 
      else {
        if (millis() - car_b_overdraw_begin > OVERDRAW_GRACE_PERIOD) {
          error(CAR_B, 'O');
          return;
        }
      }
    }
    else {
      car_b_overdraw_begin = 0;
    }
    display.setCursor(8, 1);
    display.print("B:");
    display.print(formatMilliamps(car_b_draw));
  } 
  else {
    // Car B is not charging
    car_b_overdraw_begin = 0;
    memset(car_b_current_samples, 0, sizeof(car_b_current_samples));
  }

  // We need to use labs() here because we cached now early on, so it may actually be
  // *before* the time in question
  if (car_a_request_time != 0 && (millis() - car_a_request_time) > TRANSITION_DELAY) {
    // We've waited long enough.
    log(LOG_INFO, P("Delayed transition completed on car A"));
    car_a_request_time = 0;
    setRelay(CAR_A, HIGH);
    display.setCursor(0, 1);
    display.print("A: ON   ");
  }
  if (car_a_error_time != 0 && (millis() - car_a_error_time) > ERROR_DELAY) {
    car_a_error_time = 0;
    setRelay(CAR_A, LOW);
    if (paused) {
      display.setCursor(0, 1);
      display.print(P("A: off  "));
      log(LOG_INFO, P("Power withdrawn after pause delay on car A"));
    } else {
      log(LOG_INFO, P("Power withdrawn after error delay on car A"));
    }
    if (isCarCharging(CAR_B) || last_car_b_state == STATE_B)
        setPilot(CAR_B, FULL);
  }
  if (car_b_request_time != 0 && (millis() - car_b_request_time) > TRANSITION_DELAY) {
    log(LOG_INFO, P("Delayed transition completed on car B"));
    // We've waited long enough.
    car_b_request_time = 0;
    setRelay(CAR_B, HIGH);
    display.setCursor(8, 1);
    display.print("B: ON   ");
  }
  if (car_b_error_time != 0 && (millis() - car_b_error_time) > ERROR_DELAY) {
    car_b_error_time = 0;
    setRelay(CAR_B, LOW);
    if (paused) {
      display.setCursor(8, 1);
      display.print(P("B: off  "));
      log(LOG_INFO, P("Power withdrawn after pause delay on car B"));
    } else {
      log(LOG_INFO, P("Power withdrawn after error delay on car B"));
    }
    if (isCarCharging(CAR_A) || last_car_a_state == STATE_B)
        setPilot(CAR_A, FULL);
  }
  
  if (car_a_state == STATE_A && car_b_state == STATE_A) {
    // Allow playing with the menu button only when both plugs are out
    unsigned int event = checkEvent();
    if (event == EVENT_SHORT_PUSH || event == EVENT_LONG_PUSH) {
      operatingMode++;
      if (operatingMode > LAST_MODE) operatingMode = 0;
      EEPROM.write(EEPROM_LOC_MODE, operatingMode);
      const char *modeStr;
      switch(operatingMode) {
        case MODE_SEQUENTIAL: modeStr = "sequential"; break;
        case MODE_SHARED: modeStr = "shared"; break;
        default: modeStr = "UNKNOWN";
      }
      log(LOG_INFO, P("Changing operating mode to %s"), modeStr);
    }
  }
  
}
