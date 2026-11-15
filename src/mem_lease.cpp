#include "mem_lease.h"

#include <Arduino.h>

#include <atomic>

extern TaskHandle_t loopTaskHandle;  // Arduino core (main.cpp)

namespace mem_lease {
namespace {

std::atomic<int> holders{0};
std::atomic<bool> released{true};

}  // namespace

void acquire() {
  ++holders;
  // The display frees its buffers from the loop task; never wait on ourselves.
  if (xTaskGetCurrentTaskHandle() == loopTaskHandle) return;
  for (int i = 0; i < 100 && !released; ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

void release() { --holders; }

bool requested() { return holders > 0; }

void setReleased(bool r) { released = r; }

}  // namespace mem_lease
