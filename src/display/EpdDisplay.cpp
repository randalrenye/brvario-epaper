#include "EpdDisplay.h"

#include <Arduino.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "config/AppConfig.h"
#include "firasans.h"

namespace {

static constexpr size_t kUiPsramReserveBytes = 2U * 1024U * 1024U;
static constexpr uint8_t kOptionalPartialBufferCount = 3;
static constexpr uint8_t kPanelRecoveryPolarityCycles = 2;
static constexpr uint8_t kPanelRecoveryFinalClearCycles = 2;
static constexpr uint16_t kPanelRecoverySettleMs = 320;
static constexpr uint16_t kPanelRecoveryLongSettleMs = 1500;
static constexpr uint16_t kPanelRecoveryFinalSettleMs = 420;
static constexpr uint16_t kPanelRecoveryFinalClearDelayMs = 80;
static constexpr uint32_t kPanelRecoveryLongLogIntervalMs = 5UL * 60UL * 1000UL;
static constexpr uint32_t kPanelRecoveryLongWhiteHoldMs = 3UL * 60UL * 1000UL;
static constexpr size_t kMaxPartialAreasPerBatch = 24;
static constexpr uint32_t kSlowDisplayOperationMs = 1200UL;

const uint8_t* glyph5x7(uint32_t c) {
  static const uint8_t blank[5] = {0, 0, 0, 0, 0};
  static const uint8_t unknown[5] = {0x02, 0x01, 0x51, 0x09, 0x06};

  static const uint8_t n0[5] = {0x3E, 0x51, 0x49, 0x45, 0x3E};
  static const uint8_t n1[5] = {0x00, 0x42, 0x7F, 0x40, 0x00};
  static const uint8_t n2[5] = {0x42, 0x61, 0x51, 0x49, 0x46};
  static const uint8_t n3[5] = {0x21, 0x41, 0x45, 0x4B, 0x31};
  static const uint8_t n4[5] = {0x18, 0x14, 0x12, 0x7F, 0x10};
  static const uint8_t n5[5] = {0x27, 0x45, 0x45, 0x45, 0x39};
  static const uint8_t n6[5] = {0x3C, 0x4A, 0x49, 0x49, 0x30};
  static const uint8_t n7[5] = {0x01, 0x71, 0x09, 0x05, 0x03};
  static const uint8_t n8[5] = {0x36, 0x49, 0x49, 0x49, 0x36};
  static const uint8_t n9[5] = {0x06, 0x49, 0x49, 0x29, 0x1E};

  static const uint8_t a[5] = {0x7E, 0x11, 0x11, 0x11, 0x7E};
  static const uint8_t b[5] = {0x7F, 0x49, 0x49, 0x49, 0x36};
  static const uint8_t c_[5] = {0x3E, 0x41, 0x41, 0x41, 0x22};
  static const uint8_t d[5] = {0x7F, 0x41, 0x41, 0x22, 0x1C};
  static const uint8_t e[5] = {0x7F, 0x49, 0x49, 0x49, 0x41};
  static const uint8_t f[5] = {0x7F, 0x09, 0x09, 0x09, 0x01};
  static const uint8_t g[5] = {0x3E, 0x41, 0x49, 0x49, 0x7A};
  static const uint8_t h[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
  static const uint8_t i[5] = {0x00, 0x41, 0x7F, 0x41, 0x00};
  static const uint8_t j[5] = {0x20, 0x40, 0x41, 0x3F, 0x01};
  static const uint8_t k[5] = {0x7F, 0x08, 0x14, 0x22, 0x41};
  static const uint8_t l[5] = {0x7F, 0x40, 0x40, 0x40, 0x40};
  static const uint8_t m[5] = {0x7F, 0x02, 0x0C, 0x02, 0x7F};
  static const uint8_t n[5] = {0x7F, 0x04, 0x08, 0x10, 0x7F};
  static const uint8_t o[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
  static const uint8_t p[5] = {0x7F, 0x09, 0x09, 0x09, 0x06};
  static const uint8_t q[5] = {0x3E, 0x41, 0x51, 0x21, 0x5E};
  static const uint8_t r[5] = {0x7F, 0x09, 0x19, 0x29, 0x46};
  static const uint8_t s[5] = {0x46, 0x49, 0x49, 0x49, 0x31};
  static const uint8_t t[5] = {0x01, 0x01, 0x7F, 0x01, 0x01};
  static const uint8_t u[5] = {0x3F, 0x40, 0x40, 0x40, 0x3F};
  static const uint8_t v[5] = {0x1F, 0x20, 0x40, 0x20, 0x1F};
  static const uint8_t w[5] = {0x3F, 0x40, 0x38, 0x40, 0x3F};
  static const uint8_t x[5] = {0x63, 0x14, 0x08, 0x14, 0x63};
  static const uint8_t y[5] = {0x07, 0x08, 0x70, 0x08, 0x07};
  static const uint8_t z[5] = {0x61, 0x51, 0x49, 0x45, 0x43};
  static const uint8_t la[5] = {0x20, 0x54, 0x54, 0x54, 0x78};
  static const uint8_t lb[5] = {0x7F, 0x48, 0x44, 0x44, 0x38};
  static const uint8_t lc[5] = {0x38, 0x44, 0x44, 0x44, 0x20};
  static const uint8_t ld[5] = {0x38, 0x44, 0x44, 0x48, 0x7F};
  static const uint8_t le[5] = {0x38, 0x54, 0x54, 0x54, 0x18};
  static const uint8_t lf[5] = {0x08, 0x7E, 0x09, 0x01, 0x02};
  static const uint8_t lg[5] = {0x0C, 0x52, 0x52, 0x52, 0x3E};
  static const uint8_t lh[5] = {0x7F, 0x08, 0x04, 0x04, 0x78};
  static const uint8_t li[5] = {0x00, 0x44, 0x7D, 0x40, 0x00};
  static const uint8_t lj[5] = {0x20, 0x40, 0x44, 0x3D, 0x00};
  static const uint8_t lk[5] = {0x7F, 0x10, 0x28, 0x44, 0x00};
  static const uint8_t ll[5] = {0x00, 0x41, 0x7F, 0x40, 0x00};
  static const uint8_t lm[5] = {0x7C, 0x04, 0x18, 0x04, 0x78};
  static const uint8_t ln[5] = {0x7C, 0x08, 0x04, 0x04, 0x78};
  static const uint8_t lo[5] = {0x38, 0x44, 0x44, 0x44, 0x38};
  static const uint8_t lp[5] = {0x7C, 0x14, 0x14, 0x14, 0x08};
  static const uint8_t lq[5] = {0x08, 0x14, 0x14, 0x18, 0x7C};
  static const uint8_t lr[5] = {0x7C, 0x08, 0x04, 0x04, 0x08};
  static const uint8_t ls[5] = {0x48, 0x54, 0x54, 0x54, 0x20};
  static const uint8_t lt[5] = {0x04, 0x3F, 0x44, 0x40, 0x20};
  static const uint8_t lu[5] = {0x3C, 0x40, 0x40, 0x20, 0x7C};
  static const uint8_t lv[5] = {0x1C, 0x20, 0x40, 0x20, 0x1C};
  static const uint8_t lw[5] = {0x3C, 0x40, 0x30, 0x40, 0x3C};
  static const uint8_t lx[5] = {0x44, 0x28, 0x10, 0x28, 0x44};
  static const uint8_t ly[5] = {0x0C, 0x50, 0x50, 0x50, 0x3C};
  static const uint8_t lz[5] = {0x44, 0x64, 0x54, 0x4C, 0x44};

  static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
  static const uint8_t plus[5] = {0x08, 0x08, 0x3E, 0x08, 0x08};
  static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
  static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
  static const uint8_t slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};
  static const uint8_t percent[5] = {0x23, 0x13, 0x08, 0x64, 0x62};
  static const uint8_t star[5] = {0x02, 0x05, 0x02, 0x00, 0x00};
  static const uint8_t exclaim[5] = {0x00, 0x00, 0x5F, 0x00, 0x00};
  static const uint8_t quote[5] = {0x00, 0x07, 0x00, 0x07, 0x00};
  static const uint8_t hash[5] = {0x14, 0x7F, 0x14, 0x7F, 0x14};
  static const uint8_t dollar[5] = {0x24, 0x2A, 0x7F, 0x2A, 0x12};
  static const uint8_t amp[5] = {0x36, 0x49, 0x55, 0x22, 0x50};
  static const uint8_t apostrophe[5] = {0x00, 0x05, 0x03, 0x00, 0x00};
  static const uint8_t parenOpen[5] = {0x00, 0x1C, 0x22, 0x41, 0x00};
  static const uint8_t parenClose[5] = {0x00, 0x41, 0x22, 0x1C, 0x00};
  static const uint8_t comma[5] = {0x00, 0x50, 0x30, 0x00, 0x00};
  static const uint8_t underscore[5] = {0x40, 0x40, 0x40, 0x40, 0x40};
  static const uint8_t equal[5] = {0x14, 0x14, 0x14, 0x14, 0x14};
  static const uint8_t question[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
  static const uint8_t at[5] = {0x3E, 0x41, 0x5D, 0x59, 0x4E};
  static const uint8_t bracketOpen[5] = {0x00, 0x7F, 0x41, 0x41, 0x00};
  static const uint8_t bracketClose[5] = {0x00, 0x41, 0x41, 0x7F, 0x00};
  static const uint8_t backslash[5] = {0x02, 0x04, 0x08, 0x10, 0x20};
  static const uint8_t caret[5] = {0x04, 0x02, 0x01, 0x02, 0x04};
  static const uint8_t braceOpen[5] = {0x08, 0x36, 0x41, 0x41, 0x00};
  static const uint8_t braceClose[5] = {0x00, 0x41, 0x41, 0x36, 0x08};
  static const uint8_t pipe[5] = {0x00, 0x00, 0x7F, 0x00, 0x00};
  static const uint8_t semicolon[5] = {0x00, 0x56, 0x36, 0x00, 0x00};
  static const uint8_t lessThan[5] = {0x08, 0x14, 0x22, 0x41, 0x00};
  static const uint8_t greaterThan[5] = {0x00, 0x41, 0x22, 0x14, 0x08};
  static const uint8_t tilde[5] = {0x08, 0x04, 0x08, 0x10, 0x08};

  switch (c) {
    case ' ': return blank;
    case '0': return n0;
    case '1': return n1;
    case '2': return n2;
    case '3': return n3;
    case '4': return n4;
    case '5': return n5;
    case '6': return n6;
    case '7': return n7;
    case '8': return n8;
    case '9': return n9;
    case 'A': return a;
    case 'B': return b;
    case 'C': return c_;
    case 'D': return d;
    case 'E': return e;
    case 'F': return f;
    case 'G': return g;
    case 'H': return h;
    case 'I': return i;
    case 'J': return j;
    case 'K': return k;
    case 'L': return l;
    case 'M': return m;
    case 'N': return n;
    case 'O': return o;
    case 'P': return p;
    case 'Q': return q;
    case 'R': return r;
    case 'S': return s;
    case 'T': return t;
    case 'U': return u;
    case 'V': return v;
    case 'W': return w;
    case 'X': return x;
    case 'Y': return y;
    case 'Z': return z;
    case 'a': return la;
    case 'b': return lb;
    case 'c': return lc;
    case 'd': return ld;
    case 'e': return le;
    case 'f': return lf;
    case 'g': return lg;
    case 'h': return lh;
    case 'i': return li;
    case 'j': return lj;
    case 'k': return lk;
    case 'l': return ll;
    case 'm': return lm;
    case 'n': return ln;
    case 'o': return lo;
    case 'p': return lp;
    case 'q': return lq;
    case 'r': return lr;
    case 's': return ls;
    case 't': return lt;
    case 'u': return lu;
    case 'v': return lv;
    case 'w': return lw;
    case 'x': return lx;
    case 'y': return ly;
    case 'z': return lz;
    case '-': return dash;
    case '+': return plus;
    case '.': return dot;
    case ':': return colon;
    case '/': return slash;
    case '%': return percent;
    case '*': return star;
    case '!': return exclaim;
    case '"': return quote;
    case '#': return hash;
    case '$': return dollar;
    case '&': return amp;
    case '\'': return apostrophe;
    case '(': return parenOpen;
    case ')': return parenClose;
    case ',': return comma;
    case '_': return underscore;
    case '=': return equal;
    case '?': return question;
    case '@': return at;
    case '[': return bracketOpen;
    case ']': return bracketClose;
    case '\\': return backslash;
    case '^': return caret;
    case '{': return braceOpen;
    case '}': return braceClose;
    case '|': return pipe;
    case ';': return semicolon;
    case '<': return lessThan;
    case '>': return greaterThan;
    case '~': return tilde;
    default: return unknown;
  }
}

enum class Diacritic : uint8_t {
  None,
  Cedilla,
};

uint32_t baseGlyphCodepoint(uint32_t codepoint) {
  switch (codepoint) {
    case 0x00C0:  // U+00C0
    case 0x00C1:  // U+00C1
    case 0x00C2:  // U+00C2
    case 0x00C3:  // U+00C3
      return 'A';
    case 0x00C7:  // U+00C7
      return 'C';
    case 0x00C8:  // U+00C8
    case 0x00C9:  // U+00C9
    case 0x00CA:  // U+00CA
      return 'E';
    case 0x00CC:  // U+00CC
    case 0x00CD:  // U+00CD
      return 'I';
    case 0x00D2:  // U+00D2
    case 0x00D3:  // U+00D3
    case 0x00D4:  // U+00D4
    case 0x00D5:  // U+00D5
      return 'O';
    case 0x00DA:  // U+00DA
    case 0x00DC:  // U+00DC
      return 'U';
    case 0x00E0:  // U+00E0
    case 0x00E1:  // U+00E1
    case 0x00E2:  // U+00E2
    case 0x00E3:  // U+00E3
      return 'a';
    case 0x00E7:  // U+00E7
      return 'c';
    case 0x00E8:  // U+00E8
    case 0x00E9:  // U+00E9
    case 0x00EA:  // U+00EA
      return 'e';
    case 0x00EC:  // U+00EC
    case 0x00ED:  // U+00ED
      return 'i';
    case 0x00F2:  // U+00F2
    case 0x00F3:  // U+00F3
    case 0x00F4:  // U+00F4
    case 0x00F5:  // U+00F5
      return 'o';
    case 0x00FA:  // U+00FA
    case 0x00FC:  // U+00FC
      return 'u';
    default:
      return codepoint;
  }
}

Diacritic diacriticFor(uint32_t codepoint) {
  switch (codepoint) {
    case 0x00C7:  // U+00C7
    case 0x00E7:  // U+00E7
      return Diacritic::Cedilla;
    default:
      return Diacritic::None;
  }
}

uint32_t decodeUtf8(const char*& cursor) {
  const uint8_t first = static_cast<uint8_t>(*cursor++);
  if (first < 0x80) return first;

  if ((first & 0xE0) == 0xC0) {
    const uint8_t second = static_cast<uint8_t>(*cursor);
    if ((second & 0xC0) == 0x80) {
      ++cursor;
      return (static_cast<uint32_t>(first & 0x1F) << 6) | static_cast<uint32_t>(second & 0x3F);
    }
  } else if ((first & 0xF0) == 0xE0) {
    const uint8_t second = static_cast<uint8_t>(*cursor);
    const uint8_t third = static_cast<uint8_t>(*(cursor + 1));
    if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80) {
      cursor += 2;
      return (static_cast<uint32_t>(first & 0x0F) << 12) | (static_cast<uint32_t>(second & 0x3F) << 6) |
             static_cast<uint32_t>(third & 0x3F);
    }
  }

  return '?';
}

int32_t clampInt32(int32_t value, int32_t lo, int32_t hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

size_t areaStrideBytes(const Rect_t& area) {
  return static_cast<size_t>(area.width / 2 + area.width % 2);
}

Rect_t unionRect(const Rect_t& a, const Rect_t& b) {
  const int32_t x0 = a.x < b.x ? a.x : b.x;
  const int32_t y0 = a.y < b.y ? a.y : b.y;
  const int32_t x1 = (a.x + a.width) > (b.x + b.width) ? (a.x + a.width) : (b.x + b.width);
  const int32_t y1 = (a.y + a.height) > (b.y + b.height) ? (a.y + a.height) : (b.y + b.height);
  return {x0, y0, x1 - x0, y1 - y0};
}

bool rectsTouchOrOverlap(const Rect_t& a, const Rect_t& b) {
  return a.x <= b.x + b.width && b.x <= a.x + a.width &&
         a.y <= b.y + b.height && b.y <= a.y + a.height;
}

void writePixel4ToPackedArea(uint8_t* buffer, const Rect_t& area, int32_t localX, int32_t localY, uint8_t value) {
  if (!buffer || localX < 0 || localX >= area.width || localY < 0 || localY >= area.height) return;
  value &= 0x0F;
  const size_t stride = areaStrideBytes(area);
  uint8_t& packed = buffer[static_cast<size_t>(localY) * stride + localX / 2];
  if (localX & 1) {
    packed = (packed & 0x0F) | (value << 4);
  } else {
    packed = (packed & 0xF0) | value;
  }
}

}  // namespace

bool EpdDisplay::begin() {
  const size_t frameSize = static_cast<size_t>(EPD_WIDTH) * EPD_HEIGHT / 2;
  const size_t requiredBytes = frameSize * 4U;
  framebuffer_ = static_cast<uint8_t*>(heap_caps_malloc(frameSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  baseFramebuffer_ = static_cast<uint8_t*>(heap_caps_malloc(frameSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  displayedFramebuffer_ = static_cast<uint8_t*>(heap_caps_malloc(frameSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  areaBuffer_ = static_cast<uint8_t*>(heap_caps_malloc(frameSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  areaBufferSize_ = frameSize;

  if (!framebuffer_ || !baseFramebuffer_ || !displayedFramebuffer_ || !areaBuffer_) {
    Serial.println("EPD framebuffer allocation failed");
    return false;
  }

  partialPipelineReady_ = allocateOptionalPartialBuffers(frameSize);
  Serial.printf("EPD UI PSRAM: base=%lu KB, parcial extra=%s, livre=%lu KB, maior bloco=%lu KB.\n",
                static_cast<unsigned long>(requiredBytes / 1024U),
                partialPipelineReady_ ? "ON" : "fallback",
                static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
                static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U));

  clearBuffer();
  epd_init();
  return true;
}

void EpdDisplay::clearBuffer(uint8_t color) {
  if (!framebuffer_) return;
  memset(framebuffer_, color, static_cast<size_t>(EPD_WIDTH) * EPD_HEIGHT / 2);
}

void EpdDisplay::saveBaseBuffer() {
  if (!framebuffer_ || !baseFramebuffer_) return;
  memcpy(baseFramebuffer_, framebuffer_, static_cast<size_t>(EPD_WIDTH) * EPD_HEIGHT / 2);
}

void EpdDisplay::restoreBaseBuffer() {
  if (!framebuffer_ || !baseFramebuffer_) return;
  memcpy(framebuffer_, baseFramebuffer_, static_cast<size_t>(EPD_WIDTH) * EPD_HEIGHT / 2);
}

void EpdDisplay::fillRect(const Rect_t& rect, uint8_t color) {
  if (!framebuffer_) return;
  Rect_t clipped = rect;
  if (!clipRect(clipped)) return;
  epd_fill_rect(clipped.x, clipped.y, clipped.width, clipped.height, color, framebuffer_);
}

void EpdDisplay::drawRect(const Rect_t& rect, uint8_t color) {
  if (!framebuffer_) return;
  Rect_t clipped = rect;
  if (!clipRect(clipped)) return;
  epd_draw_rect(clipped.x, clipped.y, clipped.width, clipped.height, color, framebuffer_);
}

void EpdDisplay::drawRoundRect(const Rect_t& rect, int32_t radius, uint8_t color) {
  if (!framebuffer_ || rect.width <= 1 || rect.height <= 1) return;

  Rect_t clipped = rect;
  if (!clipRect(clipped)) return;

  const int32_t maxRadius = ((clipped.width < clipped.height ? clipped.width : clipped.height) - 1) / 2;
  if (radius < 1) radius = 1;
  if (radius > maxRadius) radius = maxRadius;

  const int32_t left = clipped.x;
  const int32_t top = clipped.y;
  const int32_t right = clipped.x + clipped.width - 1;
  const int32_t bottom = clipped.y + clipped.height - 1;

  drawLine(left + radius, top, right - radius, top, color);
  drawLine(left + radius, bottom, right - radius, bottom, color);
  drawLine(left, top + radius, left, bottom - radius, color);
  drawLine(right, top + radius, right, bottom - radius, color);

  const int32_t tlx = left + radius;
  const int32_t tly = top + radius;
  const int32_t trx = right - radius;
  const int32_t try_ = top + radius;
  const int32_t brx = right - radius;
  const int32_t bry = bottom - radius;
  const int32_t blx = left + radius;
  const int32_t bly = bottom - radius;

  auto put = [&](int32_t x, int32_t y) {
    writePixel4To(framebuffer_, x, y, color);
  };

  int32_t x = 0;
  int32_t y = radius;
  int32_t decision = 1 - radius;

  while (x <= y) {
    put(tlx - x, tly - y);
    put(tlx - y, tly - x);
    put(trx + x, try_ - y);
    put(trx + y, try_ - x);
    put(brx + x, bry + y);
    put(brx + y, bry + x);
    put(blx - x, bly + y);
    put(blx - y, bly + x);

    ++x;
    if (decision < 0) {
      decision += 2 * x + 1;
    } else {
      --y;
      decision += 2 * (x - y) + 1;
    }
  }
}

void EpdDisplay::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color) {
  if (!framebuffer_) return;
  epd_draw_line(x0, y0, x1, y1, color, framebuffer_);
}

void EpdDisplay::drawCircle(int32_t x, int32_t y, int32_t radius, uint8_t color) {
  if (!framebuffer_) return;
  epd_draw_circle(x, y, radius, color, framebuffer_);
}

void EpdDisplay::fillCircle(int32_t x, int32_t y, int32_t radius, uint8_t color) {
  if (!framebuffer_) return;
  epd_fill_circle(x, y, radius, color, framebuffer_);
}

void EpdDisplay::fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint8_t color) {
  if (!framebuffer_) return;
  epd_fill_triangle(x0, y0, x1, y1, x2, y2, color, framebuffer_);
}

void EpdDisplay::drawSmallText(const char* text, int32_t x, int32_t y, uint8_t scale, uint8_t color) {
  if (!text || !framebuffer_) return;
  int32_t cursor = x;
  const char* reader = text;
  while (*reader) {
    drawSmallGlyph(decodeUtf8(reader), cursor, y, scale, color);
    cursor += 6 * scale;
  }
}

void EpdDisplay::drawSmallTextBold(const char* text, int32_t x, int32_t y, uint8_t scale, uint8_t color) {
  drawSmallText(text, x, y, scale, color);
  drawSmallText(text, x + 1, y, scale, color);
  drawSmallText(text, x, y + 1, scale, color);
}

void EpdDisplay::drawSmallTextAligned(const char* text, int32_t x, int32_t y, uint8_t scale, uint8_t color, Align align) {
  int32_t drawX = x;
  const int32_t width = smallTextWidth(text, scale);
  if (align == Align::Center) {
    drawX -= width / 2;
  } else if (align == Align::Right) {
    drawX -= width;
  }
  drawSmallText(text, drawX, y, scale, color);
}

void EpdDisplay::drawSmallTextBoldAligned(const char* text, int32_t x, int32_t y, uint8_t scale, uint8_t color, Align align) {
  int32_t drawX = x;
  const int32_t width = smallTextWidth(text, scale);
  if (align == Align::Center) {
    drawX -= width / 2;
  } else if (align == Align::Right) {
    drawX -= width;
  }
  drawSmallTextBold(text, drawX, y, scale, color);
}

void EpdDisplay::drawLargeTextAligned(const char* text, int32_t x, int32_t baselineY, uint8_t color, Align align) {
  if (!text || !framebuffer_ || *text == '\0') return;

  FontProperties props;
  props.fg_color = color >> 4;
  props.bg_color = 15;
  props.fallback_glyph = '?';
  props.flags = 0;

  int32_t measureX = 0;
  int32_t measureY = baselineY;
  int32_t x1 = 0;
  int32_t y1 = 0;
  int32_t w = 0;
  int32_t h = 0;
  get_text_bounds(&FiraSans, text, &measureX, &measureY, &x1, &y1, &w, &h, &props);

  int32_t cursorX = x - x1;
  if (align == Align::Center) {
    cursorX = x - (x1 + w / 2);
  } else if (align == Align::Right) {
    cursorX = x - (x1 + w);
  }

  int32_t cursorY = baselineY;
  write_mode(&FiraSans, text, &cursorX, &cursorY, framebuffer_, BLACK_ON_WHITE, &props);
}

void EpdDisplay::drawLargeTextBoldAligned(const char* text, int32_t x, int32_t baselineY, uint8_t color, Align align) {
  drawLargeTextAligned(text, x, baselineY, color, align);
  drawLargeTextAligned(text, x + 1, baselineY, color, align);
  drawLargeTextAligned(text, x - 1, baselineY, color, align);
  drawLargeTextAligned(text, x, baselineY + 1, color, align);
}

void EpdDisplay::fullRefresh() {
  if (!isReady()) return;
  const uint32_t startedMs = millis();
  epd_poweron();
  epd_clear();
  epd_draw_grayscale_image(epd_full_screen(), framebuffer_);
  epd_poweroff();
  copyFramebufferArea(displayedFramebuffer_, framebuffer_, epd_full_screen());
  const uint32_t elapsedMs = millis() - startedMs;
  if (elapsedMs >= kSlowDisplayOperationMs) {
    Serial.printf("EPD: full refresh concluiu em %lu ms.\n", static_cast<unsigned long>(elapsedMs));
  }
}

void EpdDisplay::quickFullRefresh() {
  if (!isReady()) return;
  const Rect_t full = epd_full_screen();
  epd_poweron();
  epd_clear_area_cycles(full, 1, 50);
  epd_draw_grayscale_image(full, framebuffer_);
  for (uint8_t pass = 0; pass < AppConfig::kTransitionContrastPasses; ++pass) {
    buildReinforceBuffer(full, areaBuffer_);
    epd_draw_image(full, areaBuffer_, BLACK_ON_WHITE);
  }
  epd_poweroff();
  copyFramebufferArea(displayedFramebuffer_, framebuffer_, full);
}

void EpdDisplay::recoverPanel(uint32_t durationMs) {
  if (!isReady()) return;

  const bool longRecovery = durationMs > 0;
  const uint32_t startedMs = millis();
  uint32_t lastLogMs = startedMs;
  uint32_t passCount = 0;

  if (longRecovery) {
    Serial.printf("EPD: iniciando recuperacao longa do painel por %lu minutos.\n",
                  static_cast<unsigned long>(durationMs / 60000UL));
  } else {
    Serial.println("EPD: iniciando recuperacao robusta do painel.");
  }

  do {
    for (uint8_t cycle = 0; cycle < kPanelRecoveryPolarityCycles; ++cycle) {
      const uint16_t settleMs = longRecovery ? kPanelRecoveryLongSettleMs : kPanelRecoverySettleMs;
      clearBuffer(AppConfig::kWhite);
      fullRefresh();
      delay(settleMs);
      clearBuffer(AppConfig::kBlack);
      fullRefresh();
      delay(settleMs);
      ++passCount;
    }

    if (longRecovery && millis() - lastLogMs >= kPanelRecoveryLongLogIntervalMs) {
      const uint32_t elapsedMinutes = (millis() - startedMs) / 60000UL;
      Serial.printf("EPD: recuperacao longa em andamento, %lu min, ciclos=%lu.\n",
                    static_cast<unsigned long>(elapsedMinutes),
                    static_cast<unsigned long>(passCount));
      lastLogMs = millis();
    }
  } while (longRecovery && millis() - startedMs < durationMs);

  clearBuffer(AppConfig::kWhite);
  fullRefresh();
  if (longRecovery) {
    Serial.println("EPD: tela branca final por alguns minutos.");
    delay(kPanelRecoveryLongWhiteHoldMs);
  }
  delay(kPanelRecoveryFinalSettleMs);

  const Rect_t full = epd_full_screen();
  epd_poweron();
  epd_clear_area_cycles(full, kPanelRecoveryFinalClearCycles, kPanelRecoveryFinalClearDelayMs);
  epd_poweroff();
  copyFramebufferArea(displayedFramebuffer_, framebuffer_, full);
  if (longRecovery) {
    Serial.printf("EPD: recuperacao longa concluida, ciclos=%lu.\n", static_cast<unsigned long>(passCount));
  } else {
    Serial.println("EPD: recuperacao robusta do painel concluida.");
  }
}

void EpdDisplay::updateAreas(const Rect_t* areas, size_t count) {
  updateAreasAndReinforce(areas, count, nullptr, 0);
}

void EpdDisplay::updateAreasAndReinforce(const Rect_t* areas, size_t count, const Rect_t* reinforceAreas, size_t reinforceCount) {
  if (!isReady()) return;

  const uint32_t startedMs = millis();
  Rect_t changedAreas[kMaxPartialAreasPerBatch] = {};
  size_t changedCount = 0;

  if (areas && count > 0) {
    for (size_t i = 0; i < count; ++i) {
      Rect_t area = areas[i];
      if (!clipRect(area)) continue;

      Rect_t changed;
      if (!findChangedArea(area, changed)) continue;
      expandRect(changed, AppConfig::kDirtyPaddingPx);

      size_t mergeIndex = changedCount;
      for (size_t j = 0; j < changedCount; ++j) {
        if (rectsTouchOrOverlap(changedAreas[j], changed)) {
          changedAreas[j] = unionRect(changedAreas[j], changed);
          clipRect(changedAreas[j]);
          mergeIndex = j;
          break;
        }
      }

      if (mergeIndex == changedCount) {
        if (changedCount < kMaxPartialAreasPerBatch) {
          changedAreas[changedCount++] = changed;
        } else {
          changedAreas[changedCount - 1] = unionRect(changedAreas[changedCount - 1], changed);
          clipRect(changedAreas[changedCount - 1]);
          mergeIndex = changedCount - 1;
        }
      }

      if (mergeIndex < changedCount) {
        for (size_t j = 0; j < changedCount;) {
          if (j != mergeIndex && rectsTouchOrOverlap(changedAreas[mergeIndex], changedAreas[j])) {
            changedAreas[mergeIndex] = unionRect(changedAreas[mergeIndex], changedAreas[j]);
            clipRect(changedAreas[mergeIndex]);
            changedAreas[j] = changedAreas[changedCount - 1];
            --changedCount;
            if (mergeIndex == changedCount) {
              mergeIndex = j;
            }
            continue;
          }
          ++j;
        }
      }
    }
  }

  const bool hasChangedArea = changedCount > 0;
  bool hasReinforceArea = false;
  if (reinforceAreas && reinforceCount > 0) {
    for (size_t i = 0; i < reinforceCount; ++i) {
      Rect_t reinforce = reinforceAreas[i];
      if (clipRect(reinforce)) {
        hasReinforceArea = true;
        break;
      }
    }
  }

  if (!hasChangedArea && !hasReinforceArea) return;

  const bool pipelineFirstChanged = hasChangedArea && partialPipelineReady_ && eraseAreaBuffer_ && drawAreaBuffer_;
  if (pipelineFirstChanged) {
    buildEraseOldBuffer(changedAreas[0], eraseAreaBuffer_);
    buildDrawNewBuffer(changedAreas[0], drawAreaBuffer_);
  }

  Rect_t firstReinforce = {0, 0, 0, 0};
  bool pipelineFirstReinforce = false;
  if (hasReinforceArea && partialPipelineReady_ && reinforceAreaBuffer_) {
    for (size_t i = 0; i < reinforceCount; ++i) {
      Rect_t reinforce = reinforceAreas[i];
      if (!clipRect(reinforce)) continue;
      firstReinforce = reinforce;
      buildReinforceBuffer(firstReinforce, reinforceAreaBuffer_);
      pipelineFirstReinforce = true;
      break;
    }
  }

  epd_poweron();
  if (hasChangedArea) {
    for (size_t i = 0; i < changedCount; ++i) {
      const Rect_t& changed = changedAreas[i];
      const bool usePrepared = pipelineFirstChanged && i == 0;
      if (!usePrepared) {
        buildEraseOldBuffer(changed, areaBuffer_);
      }
      for (uint8_t pass = 0; pass < AppConfig::kWhiteErasePasses; ++pass) {
        epd_draw_image(changed, usePrepared ? eraseAreaBuffer_ : areaBuffer_, WHITE_ON_WHITE);
      }
      if (!usePrepared) {
        buildDrawNewBuffer(changed, areaBuffer_);
      }
      epd_draw_image(changed, usePrepared ? drawAreaBuffer_ : areaBuffer_, BLACK_ON_WHITE);
      copyFramebufferArea(displayedFramebuffer_, framebuffer_, changed);
    }
  }

  if (hasReinforceArea) {
    bool consumedFirstReinforce = false;
    for (size_t i = 0; i < reinforceCount; ++i) {
      Rect_t reinforce = reinforceAreas[i];
      if (!clipRect(reinforce)) continue;
      const bool usePreparedReinforce = pipelineFirstReinforce && !consumedFirstReinforce &&
                                        reinforce.x == firstReinforce.x && reinforce.y == firstReinforce.y &&
                                        reinforce.width == firstReinforce.width && reinforce.height == firstReinforce.height;
      for (uint8_t pass = 0; pass < AppConfig::kContrastReinforcePasses; ++pass) {
        if (!usePreparedReinforce) {
          buildReinforceBuffer(reinforce, areaBuffer_);
        }
        epd_draw_image(reinforce, usePreparedReinforce ? reinforceAreaBuffer_ : areaBuffer_, BLACK_ON_WHITE);
      }
      consumedFirstReinforce = consumedFirstReinforce || usePreparedReinforce;
      copyFramebufferArea(displayedFramebuffer_, framebuffer_, reinforce);
    }
  }
  epd_poweroff();

  const uint32_t elapsedMs = millis() - startedMs;
  if (elapsedMs >= kSlowDisplayOperationMs) {
    Serial.printf("EPD: refresh parcial lento=%lu ms areas=%u reforcos=%u.\n",
                  static_cast<unsigned long>(elapsedMs),
                  static_cast<unsigned>(changedCount),
                  static_cast<unsigned>(reinforceCount));
  }
}

void EpdDisplay::reinforceArea(const Rect_t& area) {
  if (!isReady()) return;

  Rect_t clipped = area;
  if (!clipRect(clipped)) return;

  epd_poweron();
  for (uint8_t pass = 0; pass < AppConfig::kContrastReinforcePasses; ++pass) {
    buildReinforceBuffer(clipped, reinforceAreaBuffer_ ? reinforceAreaBuffer_ : areaBuffer_);
    epd_draw_image(clipped, reinforceAreaBuffer_ ? reinforceAreaBuffer_ : areaBuffer_, BLACK_ON_WHITE);
  }
  epd_poweroff();
  copyFramebufferArea(displayedFramebuffer_, framebuffer_, clipped);
}

bool EpdDisplay::clipRect(Rect_t& rect) const {
  const int32_t x0 = clampInt32(rect.x, 0, EPD_WIDTH);
  const int32_t y0 = clampInt32(rect.y, 0, EPD_HEIGHT);
  const int32_t x1 = clampInt32(rect.x + rect.width, 0, EPD_WIDTH);
  const int32_t y1 = clampInt32(rect.y + rect.height, 0, EPD_HEIGHT);
  if (x1 <= x0 || y1 <= y0) {
    return false;
  }
  rect.x = x0;
  rect.y = y0;
  rect.width = x1 - x0;
  rect.height = y1 - y0;
  return true;
}

void EpdDisplay::expandRect(Rect_t& rect, int32_t padding) const {
  rect.x -= padding;
  rect.y -= padding;
  rect.width += padding * 2;
  rect.height += padding * 2;
  clipRect(rect);
}

bool EpdDisplay::allocateOptionalPartialBuffers(size_t frameSize) {
  const size_t optionalBytes = frameSize * kOptionalPartialBufferCount;
  const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

  if (freeBefore < optionalBytes + kUiPsramReserveBytes || largestBefore < frameSize) {
    Serial.printf("EPD UI PSRAM: extras ignorados, precisa=%lu KB, livre=%lu KB, maior=%lu KB, reserva=%lu KB.\n",
                  static_cast<unsigned long>(optionalBytes / 1024U),
                  static_cast<unsigned long>(freeBefore / 1024U),
                  static_cast<unsigned long>(largestBefore / 1024U),
                  static_cast<unsigned long>(kUiPsramReserveBytes / 1024U));
    return false;
  }

  eraseAreaBuffer_ = static_cast<uint8_t*>(heap_caps_malloc(frameSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  drawAreaBuffer_ = static_cast<uint8_t*>(heap_caps_malloc(frameSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  reinforceAreaBuffer_ = static_cast<uint8_t*>(heap_caps_malloc(frameSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (eraseAreaBuffer_ && drawAreaBuffer_ && reinforceAreaBuffer_) {
    return true;
  }

  if (eraseAreaBuffer_) {
    heap_caps_free(eraseAreaBuffer_);
    eraseAreaBuffer_ = nullptr;
  }
  if (drawAreaBuffer_) {
    heap_caps_free(drawAreaBuffer_);
    drawAreaBuffer_ = nullptr;
  }
  if (reinforceAreaBuffer_) {
    heap_caps_free(reinforceAreaBuffer_);
    reinforceAreaBuffer_ = nullptr;
  }

  Serial.println("EPD UI PSRAM: extras falharam, usando buffer parcial unico.");
  return false;
}

bool EpdDisplay::findChangedArea(const Rect_t& area, Rect_t& changed) const {
  int32_t minX = EPD_WIDTH;
  int32_t minY = EPD_HEIGHT;
  int32_t maxX = -1;
  int32_t maxY = -1;

  for (int32_t y = area.y; y < area.y + area.height; ++y) {
    for (int32_t x = area.x; x < area.x + area.width; ++x) {
      if (readPixel4From(framebuffer_, x, y) != readPixel4From(displayedFramebuffer_, x, y)) {
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
      }
    }
  }

  if (maxX < minX || maxY < minY) {
    return false;
  }

  changed = {minX, minY, maxX - minX + 1, maxY - minY + 1};
  return true;
}

void EpdDisplay::buildEraseOldBuffer(const Rect_t& area, uint8_t* targetBuffer) {
  if (!targetBuffer) return;
  const size_t length = areaStrideBytes(area) * static_cast<size_t>(area.height);
  memset(targetBuffer, 0xFF, length);

  for (int32_t localY = 0; localY < area.height; ++localY) {
    const int32_t screenY = area.y + localY;
    for (int32_t localX = 0; localX < area.width; ++localX) {
      const int32_t screenX = area.x + localX;
      const uint8_t oldPixel = readPixel4From(displayedFramebuffer_, screenX, screenY);
      const uint8_t newPixel = readPixel4From(framebuffer_, screenX, screenY);
      if (oldPixel != newPixel && oldPixel != 0x0F) {
        writePixel4ToPackedArea(targetBuffer, area, localX, localY, 0x00);
      }
    }
  }
}

void EpdDisplay::buildDrawNewBuffer(const Rect_t& area, uint8_t* targetBuffer) {
  if (!targetBuffer) return;
  const size_t length = areaStrideBytes(area) * static_cast<size_t>(area.height);
  memset(targetBuffer, 0xFF, length);

  for (int32_t localY = 0; localY < area.height; ++localY) {
    const int32_t screenY = area.y + localY;
    for (int32_t localX = 0; localX < area.width; ++localX) {
      const int32_t screenX = area.x + localX;
      const uint8_t oldPixel = readPixel4From(displayedFramebuffer_, screenX, screenY);
      const uint8_t newPixel = readPixel4From(framebuffer_, screenX, screenY);
      if (oldPixel != newPixel && newPixel != 0x0F) {
        writePixel4ToPackedArea(targetBuffer, area, localX, localY, newPixel);
      }
    }
  }
}

void EpdDisplay::buildReinforceBuffer(const Rect_t& area, uint8_t* targetBuffer) {
  if (!targetBuffer) return;
  const size_t length = areaStrideBytes(area) * static_cast<size_t>(area.height);
  memset(targetBuffer, 0xFF, length);

  for (int32_t localY = 0; localY < area.height; ++localY) {
    const int32_t screenY = area.y + localY;
    for (int32_t localX = 0; localX < area.width; ++localX) {
      const int32_t screenX = area.x + localX;
      const uint8_t pixel = readPixel4From(framebuffer_, screenX, screenY);
      if (pixel != 0x0F) {
        writePixel4ToPackedArea(targetBuffer, area, localX, localY, pixel);
      }
    }
  }
}

void EpdDisplay::copyFramebufferArea(uint8_t* dst, const uint8_t* src, const Rect_t& area) {
  if (!dst || !src) return;
  Rect_t clipped = area;
  if (!clipRect(clipped)) return;
  for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
    for (int32_t x = clipped.x; x < clipped.x + clipped.width; ++x) {
      writePixel4To(dst, x, y, readPixel4From(src, x, y));
    }
  }
}

uint8_t EpdDisplay::readPixel4From(const uint8_t* buffer, int32_t x, int32_t y) const {
  if (!buffer || x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) {
    return 0x0F;
  }
  const uint8_t packed = buffer[static_cast<size_t>(y) * EPD_WIDTH / 2 + x / 2];
  return (x & 1) ? (packed >> 4) & 0x0F : packed & 0x0F;
}

uint8_t EpdDisplay::readPixel4(int32_t x, int32_t y) const {
  return readPixel4From(framebuffer_, x, y);
}

void EpdDisplay::writePixel4To(uint8_t* buffer, int32_t x, int32_t y, uint8_t value) {
  if (!buffer || x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return;
  value &= 0x0F;
  uint8_t& packed = buffer[static_cast<size_t>(y) * EPD_WIDTH / 2 + x / 2];
  if (x & 1) {
    packed = (packed & 0x0F) | (value << 4);
  } else {
    packed = (packed & 0xF0) | value;
  }
}

int32_t EpdDisplay::smallTextWidth(const char* text, uint8_t scale) const {
  if (!text || *text == '\0') return 0;
  int32_t count = 0;
  const char* reader = text;
  while (*reader) {
    decodeUtf8(reader);
    ++count;
  }
  return count * 6 * scale - scale;
}

void EpdDisplay::drawSmallGlyph(uint32_t codepoint, int32_t x, int32_t y, uint8_t scale, uint8_t color) {
  const Diacritic diacritic = diacriticFor(codepoint);
  const uint32_t baseCodepoint = baseGlyphCodepoint(codepoint);
  const uint8_t* glyph = glyph5x7(baseCodepoint);
  for (uint8_t col = 0; col < 5; ++col) {
    const uint8_t bits = glyph[col];
    for (uint8_t row = 0; row < 7; ++row) {
      if (bits & (1 << row)) {
        Rect_t pixel = {
          static_cast<int32_t>(x + col * scale),
          static_cast<int32_t>(y + row * scale),
          scale,
          scale,
        };
        fillRect(pixel, color);
      }
    }
  }

  switch (diacritic) {
    case Diacritic::Cedilla:
      fillRect({static_cast<int32_t>(x + 2 * scale), static_cast<int32_t>(y + 7 * scale), scale, scale}, color);
      fillRect({static_cast<int32_t>(x + 3 * scale), static_cast<int32_t>(y + 8 * scale), scale, scale}, color);
      break;
    case Diacritic::None:
    default:
      break;
  }
}
