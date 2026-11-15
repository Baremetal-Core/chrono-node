// Chrono Node - lets network code borrow the clock's animation memory.
//
// A TLS connection needs ~45 KB of contiguous heap, and the clock's animation
// buffers (glyph cache, date strip, glow) take ~50 KB. Together they can run
// the heap dry. While a lease is held, the display frees those buffers and
// holds the current frame still; it takes them back when the last lease ends.
#pragma once

namespace mem_lease {

// Network side. acquire() waits (up to 1 s) until the display has let go.
void acquire();
void release();

class Scope {
 public:
  Scope() { acquire(); }
  ~Scope() { release(); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

// Display side.
bool requested();          // true while any lease is held
void setReleased(bool r);  // the display holds no animation buffers

}  // namespace mem_lease
