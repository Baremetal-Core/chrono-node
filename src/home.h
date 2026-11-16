// Chrono Node - Home Assistant devices on the clock.
//
// Talks to Home Assistant's REST API with a long-lived access token: reads
// each chosen device's state (GET /api/states/<id>) and switches it with a
// service call (POST /api/services/<domain>/<service>). Runs on the network
// task: the states are polled every few seconds only while the Home screen
// shows, and right after a switch. http:// on the LAN is the usual setup
// (mDNS names like falcon.local resolve); https:// is verified against the
// built-in CA bundle.
#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "config.h"

namespace home {

enum class Kind : uint8_t { Toggle, Run, OnOff };  // toggle / scene, script / climate, TV

struct Device {
  char entity[64];
  char name[32];   // friendly name, or the entity id
  char state[20];  // as Home Assistant reports it; "" until read
  Kind kind;
  bool pending;    // switched, waiting for the new state
  bool target;     // while pending: on or off once it has switched (shown meanwhile)
};

// Reads the settings; call again after they change.
void begin();
bool configured();  // URL, token and at least one device

// Network task: polls and sends switch requests when due.
void service();

// Any task.
int devices(Device* out);  // up to config::kHaMaxDevices; returns how many
uint32_t version();        // changes when a device's state or name changes
void keepFresh();          // the web page shows the states: poll for a while
void setActive(bool homeScreenShown);
bool requestSwitch(int index);
bool isOn(const Device& d);
bool shownOn(const Device& d);  // the target while switching, so the switch flips at once

// Network task (web handlers): the devices Home Assistant has, of the kinds
// the clock can switch, as a JSON array [{"id": ..., "name": ...}].
bool listEntities(String& json, String& error);

String lastErrorText();  // "" after a successful request
void printStatus();

}  // namespace home
