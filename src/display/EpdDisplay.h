#pragma once

#include <stddef.h>
#include <stdint.h>

#include "epd_driver.h"

class EpdDisplay {
 public:
  enum class Align {
    Left,
    Center,
    Right,
  };

  bool begin();
  bool isReady() const {
    return framebuffer_ != nullptr && areaBuffer_ != nullptr && baseFramebuffer_ != nullptr && displayedFramebuffer_ != nullptr;
  }

  void clearBuffer(uint8_t color = 0xFF);
  void saveBaseBuffer();
  void restoreBaseBuffer();
  void fillRect(const Rect_t& rect, uint8_t color);
  void drawRect(const Rect_t& rect, uint8_t color);
  void drawRoundRect(const Rect_t& rect, int32_t radius, uint8_t color);
  void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color);
  void drawCircle(int32_t x, int32_t y, int32_t radius, uint8_t color);
  void fillCircle(int32_t x, int32_t y, int32_t radius, uint8_t color);
  void fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint8_t color);

  void drawSmallText(const char* text, int32_t x, int32_t y, uint8_t scale, uint8_t color);
  void drawSmallTextBold(const char* text, int32_t x, int32_t y, uint8_t scale, uint8_t color);
  void drawSmallTextAligned(const char* text, int32_t x, int32_t y, uint8_t scale, uint8_t color, Align align);
  void drawSmallTextBoldAligned(const char* text, int32_t x, int32_t y, uint8_t scale, uint8_t color, Align align);
  void drawLargeTextAligned(const char* text, int32_t x, int32_t baselineY, uint8_t color, Align align);
  void drawLargeTextBoldAligned(const char* text, int32_t x, int32_t baselineY, uint8_t color, Align align);

  void fullRefresh();
  void quickFullRefresh();
  void recoverPanel(uint32_t durationMs = 0);
  void updateAreas(const Rect_t* areas, size_t count);
  void updateAreasAndReinforce(const Rect_t* areas, size_t count, const Rect_t* reinforceAreas, size_t reinforceCount);
  void reinforceArea(const Rect_t& area);

  uint8_t* framebuffer() { return framebuffer_; }

 private:
  uint8_t* framebuffer_ = nullptr;
  uint8_t* baseFramebuffer_ = nullptr;
  uint8_t* displayedFramebuffer_ = nullptr;
  uint8_t* areaBuffer_ = nullptr;
  uint8_t* eraseAreaBuffer_ = nullptr;
  uint8_t* drawAreaBuffer_ = nullptr;
  uint8_t* reinforceAreaBuffer_ = nullptr;
  size_t areaBufferSize_ = 0;
  bool partialPipelineReady_ = false;

  bool clipRect(Rect_t& rect) const;
  void expandRect(Rect_t& rect, int32_t padding) const;
  bool findChangedArea(const Rect_t& area, Rect_t& changed) const;
  bool allocateOptionalPartialBuffers(size_t frameSize);
  void buildEraseOldBuffer(const Rect_t& area, uint8_t* targetBuffer);
  void buildDrawNewBuffer(const Rect_t& area, uint8_t* targetBuffer);
  void buildReinforceBuffer(const Rect_t& area, uint8_t* targetBuffer);
  void copyFramebufferArea(uint8_t* dst, const uint8_t* src, const Rect_t& area);
  uint8_t readPixel4From(const uint8_t* buffer, int32_t x, int32_t y) const;
  uint8_t readPixel4(int32_t x, int32_t y) const;
  void writePixel4To(uint8_t* buffer, int32_t x, int32_t y, uint8_t value);
  int32_t smallTextWidth(const char* text, uint8_t scale) const;
  void drawSmallGlyph(uint32_t codepoint, int32_t x, int32_t y, uint8_t scale, uint8_t color);
};
