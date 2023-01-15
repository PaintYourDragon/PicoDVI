#include "PicoDVI.h"

// PicoDVI class encapsulates some of the libdvi functionality -------------
// Subclasses then implement specific display types.

static PicoDVI *dviptr = NULL; // For C access to active C++ object
static volatile bool wait_begin = true;

// Runs on core 1 on startup
void setup1(void) {
  while (wait_begin)
    ; // Wait for DVIGFX*::begin() to do its thing on core 0
  dviptr->_setup();
}

// Runs on core 1 after wait_begin released
void PicoDVI::_setup(void) {
  dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
  dvi_start(&dvi0);
  (*mainloop)(&dvi0);
}

PicoDVI::PicoDVI(const struct dvi_timing &t, vreg_voltage v,
         const struct dvi_serialiser_cfg &c)
    : voltage(v) {
  dvi0.timing = &t;
  memcpy(&dvi0.ser_cfg, &c, sizeof dvi0.ser_cfg);
};

PicoDVI::~PicoDVI(void) {
  dviptr = NULL;
  wait_begin = true;
}

void PicoDVI::begin(void) {
  dviptr = this;
  vreg_set_voltage(voltage);
  delay(10);
  set_sys_clock_khz(dvi0.timing->bit_clk_khz, true); // Run at TMDS bit clock
  dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
}

// DVIGFX16 class provides GFX-compatible 16-bit color framebuffer ---------

static void *gfxptr = NULL; // For C access to active C++ object

DVIGFX16::DVIGFX16(const uint16_t w, const uint16_t h,
                   const struct dvi_timing &t, vreg_voltage v,
                   const struct dvi_serialiser_cfg &c)
    : PicoDVI(t, v, c), GFXcanvas16(w, h) {}

DVIGFX16::~DVIGFX16(void) {
  gfxptr = NULL;
}

static void scanline_callback_GFX16(void) {
  ((DVIGFX16 *)gfxptr)->_scanline_callback();
}

void DVIGFX16::_scanline_callback(void) {
  // Discard any scanline pointers passed back
  uint16_t *bufptr;
  while (queue_try_remove_u32(&dvi0.q_colour_free, &bufptr))
    ;
  bufptr = &getBuffer()[WIDTH * scanline];
  queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
  scanline = (scanline + 1) % HEIGHT;
}

bool DVIGFX16::begin(void) {
  uint16_t *bufptr = getBuffer();
  if ((bufptr)) {
    gfxptr = this;
    mainloop = dvi_scanbuf_main_16bpp; // in libdvi
    dvi0.scanline_callback = scanline_callback_GFX16;
    PicoDVI::begin();
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    bufptr += WIDTH;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    wait_begin = false; // Set core 1 in motion
    return true;
  }
  return false;
}

// DVIGFX8 (8-bit, color-indexed framebuffer) is all manner of dirty pool.
// PicoDVI seems to have some palette support but I couldn't grasp the DMA
// stuff going on, so just doing a brute force thing here for now: in
// addition to the 8-bit framebuffer, two 16-bit (RGB565) scanlines are
// allocated...then, in the scanline callback, pixels are mapped from the
// 8-bit framebuffer through the palette into one of these buffers, allowing
// use of the same dvi_scanbuf_main_16bpp handler as DVIGFX16 above. Not
// optimal, sure...but not pessimal either. The allocation of those 16-bit
// scanlines is weird(tm) though. Rather than a separate malloc (which
// creates a nasty can of worms if that fails after a successful framebuffer
// allocation...unlikely but not impossible), the framebuffer size is
// tweaked so that W*H is always an even number, plus 4 extra 8-bit
// scanlines are added: thus two 16-bit scanlines, word-aligned. That extra
// memory is for us, but allocated by GFX as part of the framebuffer all at
// once. On calling begin(), the HEIGHT value is de-tweaked to the original
// value so clipping won't allow any drawing operations to spill into the
// 16-bit scanlines.

DVIGFX8::DVIGFX8(const uint16_t w, const uint16_t h,
                 const struct dvi_timing &t, vreg_voltage v,
                 const struct dvi_serialiser_cfg &c)
    : PicoDVI(t, v, c), GFXcanvas8(w, ((h + 1) & ~1) + 4) {
}

DVIGFX8::~DVIGFX8(void) {
  gfxptr = NULL;
}

static void scanline_callback_GFX8(void) {
  ((DVIGFX8 *)gfxptr)->_scanline_callback();
}

void __not_in_flash_func(DVIGFX8::_scanline_callback)(void) {
// Idea: try doing the bufptr remove/add stuff first, THEN do the palette
// lookup afterward (for next scanline), so less of a race.

  // Discard any scanline pointers passed back
  uint8_t *b8 = &getBuffer()[WIDTH * scanline];
  uint16_t *b16 = row565[foo];
  for (int i=0; i<WIDTH; i++) b16[i] = palette[b8[i]];
  while (queue_try_remove_u32(&dvi0.q_colour_free, &b16))
    ;
  b16 = row565[foo];
  queue_add_blocking_u32(&dvi0.q_colour_valid, &b16);
  foo = (foo + 1) & 1;
  scanline = (scanline + 1) % HEIGHT;
}

bool DVIGFX8::begin(void) {
  uint8_t *bufptr = getBuffer();
  if ((bufptr)) {
    gfxptr = this;
    HEIGHT -= 4; // Clip rows used by 16bpp scanlines (still word aligned)
    row565[0] = (uint16_t *)&bufptr[WIDTH * HEIGHT];
    row565[1] = row565[0] + WIDTH;
    HEIGHT &= ~1;          // Then clip extra row (if any) that made W*H even
    setRotation(rotation); // So HEIGHT also affects _height
    memset(palette, 0, sizeof palette);
    //mainloop = mainloop8;
    mainloop = dvi_scanbuf_main_16bpp; // in libdvi
    dvi0.scanline_callback = scanline_callback_GFX8;
    PicoDVI::begin();
    for (int i=0; i<WIDTH; i++) {
      row565[0][i] = palette[bufptr[i]];
      row565[1][i] = palette[bufptr[i + WIDTH]];
    }
    uint16_t *b16 = row565[0];
    queue_add_blocking_u32(&dvi0.q_colour_valid, &b16);
    b16 = row565[1];
    queue_add_blocking_u32(&dvi0.q_colour_valid, &b16);
    wait_begin = false; // Set core 1 in motion
    return true;
  }
  return false;
}
