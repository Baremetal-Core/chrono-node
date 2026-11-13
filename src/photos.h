// Chrono Node - user photos stored as JPEG files in LittleFS.
//
// Photos live in slots 1..kPhotoSlots as /photos/<slot>.jpg. They're kept in
// the LittleFS data partition, so firmware updates (OTA or USB) don't touch
// them. Uploads arrive on the network task: they're written to a temporary
// file, checked, then renamed into a free slot. Deleting and drawing happen
// on the main task.
#pragma once

#include <WString.h>
#include <stddef.h>
#include <stdint.h>

namespace photos {

// Mounts LittleFS, formatting it on first use. Returns false if unavailable.
bool begin();
bool available();

// Slots in use, ascending. Returns how many were written to `slots`.
int list(int* slots, int max);
int count();
bool exists(int slot);
size_t sizeOf(int slot);
// "/photos/<slot>.jpg"
String path(int slot);

bool remove(int slot);
void clear();

size_t usedBytes();
size_t totalBytes();

// Upload, called on the network task. On failure `err` says why.
bool uploadBegin(String& err);
bool uploadWrite(const uint8_t* data, size_t len, String& err);
// Returns the slot the photo was stored in, or 0 on failure.
int uploadFinish(String& err);
void uploadAbort();

// Reads the JPEG header. Only baseline JPEGs can be decoded on the device.
bool checkJpeg(const String& file, uint16_t& w, uint16_t& h, String& err);

}  // namespace photos
