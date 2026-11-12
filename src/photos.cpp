#include "photos.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <atomic>

#include "config.h"

namespace photos {
namespace {

constexpr const char* kDir = "/photos";
constexpr const char* kUploadPath = "/photos/upload.tmp";
// Larger images would decode at 1/8 scale and still not fit the screen.
constexpr uint16_t kMaxW = 320 * 8;
constexpr uint16_t kMaxH = 240 * 8;

bool mounted = false;
std::atomic<uint32_t> inUse{0};  // bit n set: slot n has a photo (read by both tasks)
File upload;
size_t uploadLen = 0;

int readByte(File& f) { return f.available() ? f.read() : -1; }

int readU16(File& f) {
  const int hi = readByte(f), lo = readByte(f);
  return hi < 0 || lo < 0 ? -1 : (hi << 8) | lo;
}

}  // namespace

bool begin() {
  mounted = LittleFS.begin(true);
  if (!mounted) {
    Serial.println("[PHOTO] ERROR: LittleFS mount failed");
    return false;
  }
  if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
  uint32_t mask = 0;
  for (int s = 1; s <= config::kPhotoSlots; ++s) {
    if (LittleFS.exists(path(s))) mask |= 1u << s;
  }
  inUse = mask;
  if (LittleFS.exists(kUploadPath)) LittleFS.remove(kUploadPath);  // interrupted upload
  Serial.printf("[PHOTO] %d photo(s), %u of %u KB used\n", count(), usedBytes() / 1024,
                totalBytes() / 1024);
  return true;
}

bool available() { return mounted; }

String path(int slot) { return String(kDir) + "/" + slot + ".jpg"; }

bool exists(int slot) {
  return slot >= 1 && slot <= config::kPhotoSlots && (inUse.load() & (1u << slot));
}

int list(int* slots, int max) {
  int n = 0;
  for (int s = 1; s <= config::kPhotoSlots && n < max; ++s) {
    if (exists(s)) slots[n++] = s;
  }
  return n;
}

int count() {
  int slots[config::kPhotoSlots];
  return list(slots, config::kPhotoSlots);
}

size_t sizeOf(int slot) {
  if (!exists(slot)) return 0;
  File f = LittleFS.open(path(slot), "r");
  const size_t n = f ? f.size() : 0;
  f.close();
  return n;
}

bool remove(int slot) {
  // Unlink first: an upload finishing meanwhile must not see the slot as free
  // while the old file still exists.
  if (!exists(slot) || !LittleFS.remove(path(slot))) return false;
  inUse &= ~(1u << slot);
  return true;
}

void clear() {
  for (int s = 1; s <= config::kPhotoSlots; ++s) remove(s);
}

size_t usedBytes() { return mounted ? LittleFS.usedBytes() : 0; }
size_t totalBytes() { return mounted ? LittleFS.totalBytes() : 0; }

bool checkJpeg(const String& file, uint16_t& w, uint16_t& h, String& err) {
  File f = LittleFS.open(file, "r");
  if (!f) {
    err = "can't open the file";
    return false;
  }
  bool ok = false;
  err = "not a JPEG image";
  if (readByte(f) == 0xFF && readByte(f) == 0xD8) {
    // Walk the marker segments up to the frame header (SOFn).
    while (true) {
      int b = readByte(f);
      if (b != 0xFF) break;
      int marker;
      do marker = readByte(f); while (marker == 0xFF);
      if (marker < 0 || marker == 0xD9 || marker == 0xDA) break;  // EOI/SOS before a frame
      if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;  // no length
      const int len = readU16(f);
      if (len < 2) break;
      if (marker == 0xC0) {
        // Baseline frame. Mirror tjpgd's limits: 8-bit, grey or YCbCr, luma
        // sampled 1x1, 2x1 or 2x2 and chroma 1x1.
        const int precision = readByte(f);
        const int hh = readU16(f), ww = readU16(f);
        const int ncomp = readByte(f);
        if (hh <= 0 || ww <= 0 || precision != 8 || (ncomp != 1 && ncomp != 3)) {
          err = "unsupported JPEG format";
          break;
        }
        bool samplingOk = true;
        for (int c = 0; c < ncomp; ++c) {
          readByte(f);  // component id
          const int sampling = readByte(f);
          readByte(f);  // quantisation table
          if (c == 0) {
            samplingOk &= ncomp == 1 || sampling == 0x11 || sampling == 0x21 || sampling == 0x22;
          } else {
            samplingOk &= sampling == 0x11;
          }
        }
        if (!samplingOk) {
          err = "unsupported JPEG colour sampling";
          break;
        }
        h = hh;
        w = ww;
        if (w > kMaxW || h > kMaxH) {
          err = "image too large (max 2560x1920)";
        } else {
          ok = true;
        }
        break;
      }
      if (marker >= 0xC1 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
          marker != 0xCC) {
        err = "progressive or unusual JPEG; re-save it as a baseline JPEG";
        break;
      }
      if (!f.seek(f.position() + len - 2)) break;
    }
  }
  f.close();
  if (ok) err = "";
  return ok;
}

bool uploadBegin(String& err) {
  uploadAbort();
  if (!mounted) {
    err = "photo storage unavailable";
    return false;
  }
  if (count() >= config::kPhotoSlots) {
    err = "all " + String(config::kPhotoSlots) + " photo slots are used; delete one first";
    return false;
  }
  upload = LittleFS.open(kUploadPath, "w");
  uploadLen = 0;
  if (!upload) {
    err = "can't create the file";
    return false;
  }
  return true;
}

bool uploadWrite(const uint8_t* data, size_t len, String& err) {
  if (!upload) return false;
  uploadLen += len;
  if (uploadLen > config::kPhotoMaxBytes) {
    err = "photo larger than " + String(config::kPhotoMaxBytes / 1024) + " KB";
    uploadAbort();
    return false;
  }
  if (upload.write(data, len) != len) {
    err = "storage full";
    uploadAbort();
    return false;
  }
  return true;
}

int uploadFinish(String& err) {
  if (!upload) return 0;
  upload.close();
  uint16_t w = 0, h = 0;
  if (!checkJpeg(kUploadPath, w, h, err)) {
    LittleFS.remove(kUploadPath);
    return 0;
  }
  for (int s = 1; s <= config::kPhotoSlots; ++s) {
    if (exists(s)) continue;
    if (!LittleFS.rename(kUploadPath, path(s))) break;
    inUse |= 1u << s;
    Serial.printf("[PHOTO] Saved photo %d (%ux%u, %u bytes)\n", s, w, h, uploadLen);
    return s;
  }
  err = "no free photo slot";
  LittleFS.remove(kUploadPath);
  return 0;
}

void uploadAbort() {
  if (upload) upload.close();
  if (mounted && LittleFS.exists(kUploadPath)) LittleFS.remove(kUploadPath);
  uploadLen = 0;
}

}  // namespace photos
