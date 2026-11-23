/* host stub of graphx.h - drawing is a no-op on the host */
#pragma once
#include <stdint.h>
extern uint16_t host_palette[256];
#define gfx_palette host_palette
#define gfx_RGBTo1555(r,g,b) ((uint16_t)((((r)>>3)<<10)|(((g)>>3)<<5)|((b)>>3)))
static inline void gfx_Begin(void) {}
static inline void gfx_End(void) {}
static inline void gfx_SetDrawBuffer(void) {}
static inline void gfx_SwapDraw(void) {}
static inline void gfx_SetMonospaceFont(uint8_t s) { (void)s; }
static inline void gfx_SetTextTransparentColor(uint8_t c) { (void)c; }
static inline void gfx_SetTextFGColor(uint8_t c) { (void)c; }
static inline void gfx_SetTextBGColor(uint8_t c) { (void)c; }
static inline void gfx_SetColor(uint8_t c) { (void)c; }
static inline void gfx_SetTextXY(int x, int y) { (void)x; (void)y; }
static inline void gfx_PrintString(const char *s) { (void)s; }
static inline void gfx_PrintChar(char c) { (void)c; }
static inline void gfx_FillRectangle_NoClip(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
static inline void gfx_Rectangle_NoClip(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
static inline void gfx_HorizLine_NoClip(int x, int y, int w) { (void)x; (void)y; (void)w; }
static inline void gfx_Line_NoClip(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
