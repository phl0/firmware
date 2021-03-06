/*
This file is part of bt-trx

bt-trx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

bt-trx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

Copyright (C) 2019 Christian Obersteiner (DL1COM), Andreas Müller (DC1MIL)
Contact: bt-trx.com, mail@bt-trx.com
*/

#include "bttrx_fsm.h"
#include "resulttype.h"
#include "splitstring.h"

BTTRX_FSM::BTTRX_FSM()
    : bttrx_control_(&serial_, &wt32i_), current_state_(STATE_INIT),
      led_connected_(PIN_LED_BLUE), led_busy_(PIN_LED_GREEN),
      helper_button_(PIN_BTN_0), ptt_button_(PIN_PTT_IN),
      ptt_output_(PIN_PTT_OUT, PIN_PTT_LED) {}

BTTRX_FSM::BTTRX_FSM(Stream *serial_bt, Stream *serial_dbg) : BTTRX_FSM() {
  setSerial(serial_bt, serial_dbg);
}

void BTTRX_FSM::setSerial(Stream *serial_bt, Stream *serial_dbg) {
  serial_.setSerialStreams(serial_bt, serial_dbg);
  wt32i_.setSerialWrapper(&serial_);
}

/**
 * @brief Run the State Machine, has to be called in the main loop
 *
 */
void BTTRX_FSM::run() {
  // Read button states
  ptt_button_.update();
  helper_button_.update();
  ble_button_.update();

  // Run State Machine
  switch (current_state_) {
  case STATE_INIT:
    handleStateInit();
    break;
  case STATE_CONFIGURE:
    handleStateConfigure();
    break;
  case STATE_INQUIRY:
    handleStateInquiry();
    break;
  case STATE_CONNECTING:
    handleStateConnecting();
    break;
  case STATE_CONNECTED:
    handleStateConnected();
    break;
  case STATE_CALL_RUNNING:
    handleStateCallRunning();
    break;
  default:
    serial_.dbg_println("ERROR: TRYING TO REACH UNKOWN STATE");
    while (true)
      ;
    break;
  }
}

void BTTRX_FSM::updateStatusmessage() {
  string message = "";
  string device_name = remote_device_info_.bd_address;
  if (!remote_device_info_.bd_friendly_name.empty()) {
    device_name = remote_device_info_.bd_friendly_name;
  }

  switch (current_state_) {
  case STATE_INIT:
    message = "Initialization...";
    break;
  case STATE_CONFIGURE:
    message = "Configuring...";
    break;
  case STATE_INQUIRY:
    message = "Looking for Bluetooth devices...";
    break;
  case STATE_CONNECTING:
    message = "Connecting to " + remote_device_info_.bd_address;
    break;
  case STATE_CONNECTED:
    message = "Connected to " + device_name;
    break;
  case STATE_CALL_RUNNING:
    message = "Call running";
    break;
  default:
    break;
  }
  bttrx_control_.storeSetting(kStatusmessage, message);
#ifdef ARDUINO
  bttrx_display_.setStatusMessage(message);
#endif // ARDUINO
}

/**
 * @brief StateInit: Try to reach the Bluetooth module
 * If the Bluetooth module is available, advance to next state
 */
void BTTRX_FSM::handleStateInit() {
  led_busy_.off();
  led_connected_.off();
  ptt_output_.off();

  // Try to reach Bt Module
  if (wt32i_.available() == ResultType::kSuccess) {
    setState(STATE_CONFIGURE);
  } else {
    serial_.dbg_println("ERROR: can't reach WT32i module");
  }
}

/**
 * @brief StateConfigure: Check and correct the configuration of the
 * Bluetooth module
 */
void BTTRX_FSM::handleStateConfigure() {
  led_busy_.on();
  led_connected_.off();
  ptt_output_.off();

  handleIncomingMessage();
  if (current_state_ != STATE_CONFIGURE) {
    return;
  }

  // Enforce configuration
  string friendly_name = "bt-trx_";
  string callsign = bttrx_control_.getCallsign();
  if (!callsign.empty()) {
    wt32i_.set("BT", "NAME", friendly_name + callsign);
  } else {
    wt32i_.set("BT", "NAME", friendly_name + wt32i_.getBDAddressSuffix());
  }

  wt32i_.set("PROFILE", "HFP-AG", "ON");
  wt32i_.set("BT", "CLASS", "400204"); // HFP-AG
  wt32i_.set("BT", "SSP",
             "1 0"); // Display yes/no button, MITM not mandatory

  // Service Class: Audio, Major Device Class: Audio/Video
  wt32i_.set("BT", "FILTER", "200400 200400");
  // Set PIN to 0000 as fallback if no SSP is available
  wt32i_.set("BT", "AUTH *", "0000");

  // Configuration: KLUDGE REMOVE_PAIR NO_AUTO_DATAMODE HFP_ERROR_BYPASS
  // MITM_DISCARD_L4_KEY
  wt32i_.set("CONTROL", "CONFIG", "0001 0000 00A0 1100");
  wt32i_.set("CONTROL", "ECHO", "5"); // Do not echo issued commands

  // Future (needs iWrap 6.2)
  // wt32i_.set("CONTROL", "HFPINIT", "SERVICE 1 SIGNAL 5");

  // Read wt32i configuration to get current values of ADC/DAC Gain and PIN
  wt32i_.set();
  wt32i_.list();

  setState(STATE_INQUIRY);
}

/**
 * @brief StateInquiry: Wait for connections (automatically
 * reestablished by already known partners). If no active connections appear,
 * make an inquiry for nearby devices regularly
 */
void BTTRX_FSM::handleStateInquiry() {
  led_busy_.off();
  led_connected_.blink(500);
  ptt_output_.off();

  handleIncomingMessage();
  if (current_state_ != STATE_INQUIRY) {
    return;
  }

  ulong now = millis();

  static ulong last_start_inquiry = -10000;
  if (last_start_inquiry + 10000 < now && !wt32i_.inquiryRunning()) {
    last_start_inquiry = now;
    wt32i_.startInquiry();
  }
}

/**
 * @brief StateConnecting: Waiting for the result of the connection request
 */
void BTTRX_FSM::handleStateConnecting() {
  led_busy_.off();
  led_connected_.blink(250);
  ptt_output_.off();

  handleIncomingMessage();
  if (current_state_ != STATE_CONNECTING) {
    return;
  }
}

/**
 * @brief STATE_CONNECTED: HFP-Connection established, waiting for calls
 */
void BTTRX_FSM::handleStateConnected() {
  led_connected_.on();
  led_busy_.off();
  ptt_output_.off();

  handleIncomingMessage();
  if (current_state_ != STATE_CONNECTED) {
    return;
  }

  // If either the PTT button or the helper button is pressed, start a
  // phone call
  if (ptt_button_.isPressedEdge() || ble_button_.isPressedEdge() ||
      helper_button_.isPressedEdge()) {
    wt32i_.dial();
  }

  // Experimental: Workaround for iWrap 6.1.0, as AT+COPS message does not
  // get exposed to us we have to send +COPS message on our own
  ulong now = millis();
  ulong COPS_INTERVAL = 10000;
  static ulong last_cops_sent = -COPS_INTERVAL;
  if (last_cops_sent + COPS_INTERVAL < now) {
    last_cops_sent = now;
    serial_.println("+COPS: 0,0,\"BTTRX\"");
    serial_.println("OK");
  }

  // If not known yet, request the friendly name of the remote device
  if (!remote_device_info_.bd_address.empty() &&
      remote_device_info_.bd_friendly_name.empty()) {
    ulong now = millis();
    static ulong last_start_inquiry = -10000;
    if (last_start_inquiry + 10000 < now) {
      last_start_inquiry = now;
      wt32i_.name(remote_device_info_.bd_address);
    }
  }
}

/**
 * @brief STATE_CALL_RUNNING: Phone call running
 */
void BTTRX_FSM::handleStateCallRunning() {
  led_connected_.on();
  led_busy_.blink(1000);

  handleIncomingMessage();
  if (current_state_ != STATE_CALL_RUNNING) {
    return;
  }

  handlePTTDuringCall();

  // If the button is pressed, send the "HANGUP" message.
  // State change back to STATE_CONNECTED happens when HFP device indicates
  // end of call
  if (helper_button_.isPressedEdge()) {
    wt32i_.hangup();
  }
}

/**
 * @brief Reads serial messages from the bluetooth module and handles them
 */
void BTTRX_FSM::handleIncomingMessage() {
  iWrapMessage msg;
  wt32i_.getIncomingMessage(&msg);

  switch (msg.msg_type) {
  case kSETTING_CONTROL_GAIN:
    bttrx_control_.storeSetting(kADCGain, splitString(msg.msg)[3]);
    bttrx_control_.storeSetting(kDACGain, splitString(msg.msg)[4]);
    break;
  case kSETTING_PIN_CODE:
    bttrx_control_.storeSetting(kPinCode, splitString(msg.msg)[4]);
    break;
  case kLIST_RESULT:
    if (current_state_ != STATE_CONNECTED) {
      // Kill existing connections
      wt32i_.close(splitString(msg.msg)[1]);
    } else if (current_state_ == STATE_CONNECTED) {
      remote_device_info_.bd_address = splitString(msg.msg)[10];
    }
    break;
  case kINQUIRY_RESULT:
    // In the meantime, we may have got an incoming connection and we do not
    // need to try to connect. So only do this in case we are still in the
    // INQUIRY state
    if (current_state_ == STATE_INQUIRY) {
      if (!wt32i_.getInquiredDevices().empty()) {
        remote_device_info_.bd_address = wt32i_.getInquiredDevices().at(0);
        wt32i_.connectHFPAG(remote_device_info_.bd_address);
        setState(STATE_CONNECTING);
      }
    }
    break;
  case kNAME_RESULT:
    // Store friendly name
    remote_device_info_.bd_friendly_name = splitString(msg.msg, "\"")[1];
    updateStatusmessage();
    break;
  case kHFPAG_READY:
    // Indication that HFP-AG connection was successful
    wt32i_.indicateNetworkAvailable();
    wt32i_.list();
    setState(STATE_CONNECTED);
    break;
  case kHFPAG_CALLING:
    // Indication that an outgoing phone call is requested
    wt32i_.connect();
    break;
  case kHFPAG_CONNECT:
    // Phone call established
    setState(STATE_CALL_RUNNING);
    break;
  case kHFPAG_NO_CARRIER:
    // Phone call ended
    setState(STATE_CONNECTED);
    break;
  case kHFPAG_UNKOWN:
    // Handle AT command
    wt32i_.handleMessage_HFPAG_UNKNOWN(msg);
    break;
  case kNOCARRIER_ERROR_LINK_LOSS:
    // Connection try was unsuccessful, get back to inquiry
    remote_device_info_.bd_address = "";
    remote_device_info_.bd_friendly_name = "";
    setState(STATE_INQUIRY);
    break;
  case kSSP_CONFIRM:
    wt32i_.sendSSPConfirmation(splitString(msg.msg)[2]);
    break;
  default:
    break;
  }
}

/**
 * @brief Helper function to change state machine states.
 * Currently only used to print a debug message on state change
 *
 * @param state
 */
void BTTRX_FSM::setState(state_t state) {
  current_state_ = state;
  switch (state) {
  case STATE_INIT:
    serial_.dbg_println("STATE: INIT");
    break;
  case STATE_CONFIGURE:
    serial_.dbg_println("STATE: CONFIGURE");
    break;
  case STATE_INQUIRY:
    serial_.dbg_println("STATE: INQUIRY");
    break;
  case STATE_CONNECTING:
    serial_.dbg_println("STATE: CONNECTING");
    break;
  case STATE_CONNECTED:
    serial_.dbg_println("STATE: CONNECTED");
    break;
  case STATE_CALL_RUNNING:
    serial_.dbg_println("STATE: CALL_RUNNING");
    break;
  default:
    serial_.dbg_println("ERROR: Trying to go into unkown state");
    break;
  }

  updateStatusmessage();
}

/**
 * @brief Check for PTT Timeout and Delayed PTT off, handle Button presses
 *
 */
void BTTRX_FSM::handlePTTDuringCall() {
  ptt_output_.checkForTimeout(bttrx_control_.getPTTTimeout());
  ptt_output_.checkForDelayedOff();

  switch (bttrx_control_.getPTTMode()) {
  case kDirect:
    handlePTTDirect();
    break;
  case kToggle:
    handlePTTWiredToggle();
    handlePTTBLEToggle();
    break;
  case kWillimode:
    handlePTTWiredWillimode();
    handlePTTBLEToggle();
    break;
  default:
    break;
  }
}

/**
 * @brief Handle press of wired and BLE PTT button in Direct Mode
 */
void BTTRX_FSM::handlePTTDirect() {
  // Hold Button for PTT
  if (ptt_button_.isPressedEdge() || ble_button_.isPressedEdge()) {
    ptt_output_.on();
#ifdef ARDUINO
    bttrx_display_.setTransmitMessage("<<< ON AIR >>>");
#endif // ARDUINO
  } else if (ptt_button_.isReleased() &&
             (!ble_button_.isConnected() ||
              (ble_button_.isConnected() && ble_button_.isReleased()))) {
    ptt_output_.delayed_off(bttrx_control_.getPTTHangTime());
#ifdef ARDUINO
    bttrx_display_.setTransmitMessage("idle");
#endif // ARDUINO
  }
}

/**
 * @brief Handle press of wired PTT button in Toggle Mode
 */
void BTTRX_FSM::handlePTTWiredToggle() {
  // Press Button to assert PTT, press again to release PTT
  if (ptt_button_.isPressedEdge()) {
    ptt_output_.toggle(bttrx_control_.getPTTHangTime());
#ifdef ARDUINO
    if (ptt_output_.getState()) {
      bttrx_display_.setTransmitMessage("<<< ON AIR >>>");
    } else {
      bttrx_display_.setTransmitMessage("idle");
    }
#endif // ARDUINO
  }
}

/**
 * @brief If Willimode is enabled, wired PTT behaves as follows
 * Holding it longer than 1s -> nothing happens
 * Holding it shorter than 1s -> toggle PTT
 */
void BTTRX_FSM::handlePTTWiredWillimode() {
  static ulong start_time = 0;
  if (ptt_button_.isPressedEdge()) {
    start_time = millis();
  }
  if (ptt_button_.isReleasedEdge()) {
    ulong stop_time = millis();
    if (stop_time - start_time < PTT_TIMEOUT_WILLIMODE) {
      ptt_output_.toggle(bttrx_control_.getPTTHangTime());
#ifdef ARDUINO
      if (ptt_output_.getState()) {
        bttrx_display_.setTransmitMessage("<<< ON AIR >>>");
      } else {
        bttrx_display_.setTransmitMessage("idle");
      }
#endif // ARDUINO
    }
  }
}

/**
 * @brief Handle press of BLE button in case of Toggle and Willimode
 */
void BTTRX_FSM::handlePTTBLEToggle() {
  // Press Button to assert PTT, press again to release PTT
  if (ble_button_.isPressedEdge()) {
    ptt_output_.toggle(bttrx_control_.getPTTHangTime());
#ifdef ARDUINO
    if (ptt_output_.getState()) {
      bttrx_display_.setTransmitMessage("<<< ON AIR >>>");
    } else {
      bttrx_display_.setTransmitMessage("idle");
    }
#endif // ARDUINO
  }
}
