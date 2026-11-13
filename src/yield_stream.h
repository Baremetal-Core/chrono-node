// Chrono Node - Stream adapter for reading network responses on the net task.
//
// Stream::timedRead() busy-waits for each byte (up to the client timeout)
// without yielding. On the network task (core 0) a slow response then starves
// the idle task and the task watchdog resets the clock after 5 s. This adapter
// sleeps one tick whenever no byte is ready, so waiting costs no CPU.
#pragma once

#include <Arduino.h>

class YieldingStream : public Stream {
 public:
  explicit YieldingStream(Stream& in) : in_(in) { setTimeout(in.getTimeout()); }

  int available() override { return in_.available(); }
  int peek() override { return in_.peek(); }
  int read() override {
    const int c = in_.read();
    if (c < 0) vTaskDelay(1);
    return c;
  }
  size_t write(uint8_t) override { return 0; }

 private:
  Stream& in_;
};
