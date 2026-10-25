// See extdisp_mirror.hpp. Platform-neutral LovyanGFX code: the host gate compiles this file unchanged.
#include "extdisp_mirror.hpp"
#include "extdisp_probe.h"

using namespace lgfx;

void Panel_Mirror::attach(IPanel *primary, IPanel *ext)
{
    _pri = primary;
    _ext = ext;
    sync_dims();
}

// LGFXBase reads width/height/depth/rotation straight from the panel's fields (non-virtual getters), so
// the mirror must carry the internal panel's values, refreshed after anything that can change them.
void Panel_Mirror::sync_dims(void)
{
    _width = _pri->width();
    _height = _pri->height();
    _rotation = _pri->getRotation();
    _invert = _pri->getInvert();
    _write_depth = _pri->getWriteDepth();
    _read_depth = _pri->getReadDepth();
    extdisp_mirror_origin(_ext->width(), _ext->height(), _width, _height, &_ox, &_oy);
}

// The internal panel nests under the mirror's own count: its internal startWrite/endWrite pairs (readRect,
// readCommand, setRotation...) then never close the outer transaction early. The external panel is NOT
// opened here — see the bus rule in the header.
void Panel_Mirror::beginTransaction(void) { _pri->startWrite(); }
void Panel_Mirror::endTransaction(void)   { _pri->endWrite(); }

void Panel_Mirror::setBrightness(uint8_t brightness)
{
    _pri->setBrightness(brightness);
    if (_bl_cb) _bl_cb(brightness);
}

color_depth_t Panel_Mirror::setColorDepth(color_depth_t depth)
{
    color_depth_t r = _pri->setColorDepth(depth);
    _ext->setColorDepth(r);                       // same wire format on both, so raw colours pass through
    sync_dims();
    return r;
}

// Invert and rotation are properties of the internal glass (the ST7789 needs invert, the ILI9341 does not;
// the external landscape orientation is fixed at bring-up). Only the internal panel takes them.
void Panel_Mirror::setInvert(bool invert)     { _pri->setInvert(invert); sync_dims(); }
void Panel_Mirror::setRotation(uint_fast8_t r) { _pri->setRotation(r); sync_dims(); }
void Panel_Mirror::setSleep(bool flg)         { _pri->setSleep(flg); _ext->setSleep(flg); }
void Panel_Mirror::setPowerSave(bool flg)     { _pri->setPowerSave(flg); _ext->setPowerSave(flg); }

void Panel_Mirror::writeCommand(uint32_t cmd, uint_fast8_t length) { _pri->writeCommand(cmd, length); }
void Panel_Mirror::writeData(uint32_t data, uint_fast8_t length)   { _pri->writeData(data, length); }

void Panel_Mirror::initDMA(void)    { _pri->initDMA(); }
void Panel_Mirror::waitDMA(void)    { _pri->waitDMA(); }
bool Panel_Mirror::dmaBusy(void)    { return _pri->dmaBusy(); }
void Panel_Mirror::waitDisplay(void) { _pri->waitDisplay(); }
bool Panel_Mirror::displayBusy(void) { return _pri->displayBusy(); }
void Panel_Mirror::display(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h) { _pri->display(x, y, w, h); }
bool Panel_Mirror::isReadable(void) const { return _pri->isReadable(); }

uint32_t Panel_Mirror::readCommand(uint_fast16_t cmd, uint_fast8_t index, uint_fast8_t length) { return _pri->readCommand(cmd, index, length); }
uint32_t Panel_Mirror::readData(uint_fast8_t index, uint_fast8_t length) { return _pri->readData(index, length); }
void Panel_Mirror::readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void *dst, pixelcopy_t *param)
{
    _pri->readRect(x, y, w, h, dst, param);
}
int32_t Panel_Mirror::getScanLine(void) { return _pri->getScanLine(); }

// ---- self-contained primitives: one external transaction each, offset into the centred frame ----------

void Panel_Mirror::drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t rawcolor)
{
    _pri->drawPixelPreclipped(x, y, rawcolor);
    ext_begin();
    _ext->drawPixelPreclipped(x + _ox, y + _oy, rawcolor);
    ext_end();
}

void Panel_Mirror::writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t rawcolor)
{
    _pri->writeFillRectPreclipped(x, y, w, h, rawcolor);
    ext_begin();
    _ext->writeFillRectPreclipped(x + _ox, y + _oy, w, h, rawcolor);
    ext_end();
}

// pixelcopy_t is consumed as it is read (it advances its source position), so the external panel gets a
// copy taken BEFORE the internal write. The external write is synchronous (no DMA): it reads the source
// before this call returns, while the caller's buffer is still alive.
void Panel_Mirror::writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t *param, bool use_dma)
{
    pixelcopy_t copy = *param;
    _pri->writeImage(x, y, w, h, param, use_dma);
    ext_begin();
    _ext->writeImage(x + _ox, y + _oy, w, h, &copy, false);
    ext_end();
}

// Alpha blending reads the destination back. The external panel's MISO is normally not wired (its SDO
// does not release the shared line), so blend on the internal panel and copy the result across.
void Panel_Mirror::writeImageARGB(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t *param)
{
    _pri->writeImageARGB(x, y, w, h, param);
    mirror_from_primary(x, y, w, h);
}

// Same for copyRect (scroll): do it on the internal panel, then copy the destination rows across.
void Panel_Mirror::copyRect(uint_fast16_t dst_x, uint_fast16_t dst_y, uint_fast16_t w, uint_fast16_t h, uint_fast16_t src_x, uint_fast16_t src_y)
{
    _pri->copyRect(dst_x, dst_y, w, h, src_x, src_y);
    mirror_from_primary(dst_x, dst_y, w, h);
}

// In 64-px runs (192 B of stack, safe on the 8 KB game tasks that draw direct), ONE external transaction per
// row: short enough that an SD transfer waits at most one row, few enough that a full-screen alpha overlay
// costs 135 bus claims instead of 540.
void Panel_Mirror::mirror_from_primary(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || !_pri->isReadable()) return;
    RGBColor buf[64];
    pixelcopy_t pc_read(nullptr, RGBColor::depth, _pri->getReadDepth(), false);
    for (int row = 0; row < h; row++) {
        ext_begin();
        for (int x0 = 0; x0 < w; x0 += 64) {
            int n = w - x0 < 64 ? w - x0 : 64;
            _pri->readRect(x + x0, y + row, n, 1, buf, &pc_read);
            pixelcopy_t pc_write(buf, _ext->getWriteDepth(), RGBColor::depth, false);
            _ext->writeImage(x + x0 + _ox, y + row + _oy, n, 1, &pc_write, false);
        }
        ext_end();
    }
}

// ---- the pixel stream: setWindow, then any number of writeBlock / writePixels calls ---------------------

void Panel_Mirror::setWindow(uint_fast16_t xs, uint_fast16_t ys, uint_fast16_t xe, uint_fast16_t ye)
{
    _pri->setWindow(xs, ys, xe, ye);
    _wx0 = xs; _wy0 = ys; _wx1 = xe; _wy1 = ye;   // addressed on the external panel lazily, per chunk
    _cx = xs; _cy = ys;
}

// Re-address the external window from the cursor and write `len` pixels through write_n(n) in at most two
// pieces: the rest of a partly written row, then whole rows to the end of the window (the panel wraps rows
// inside its window on its own). Past the window's end the stream wraps to its top, as the panel does.
template <typename F>
void Panel_Mirror::ext_stream(uint32_t len, F &&write_n)
{
    const int ww = _wx1 - _wx0 + 1;
    if (ww <= 0 || _wy1 < _wy0) return;
    ext_begin();
    while (len) {
        if (_cy > _wy1) { _cx = _wx0; _cy = _wy0; }
        uint32_t n;
        if (_cx != _wx0) {
            n = (uint32_t)(_wx1 - _cx + 1);
            if (n > len) n = len;
            _ext->setWindow(_cx + _ox, _cy + _oy, _wx1 + _ox, _cy + _oy);
        } else {
            n = (uint32_t)ww * (uint32_t)(_wy1 - _cy + 1);
            if (n > len) n = len;
            _ext->setWindow(_wx0 + _ox, _cy + _oy, _wx1 + _ox, _wy1 + _oy);
        }
        write_n(n);
        uint32_t off = (uint32_t)(_cx - _wx0) + n;
        _cy += (int)(off / (uint32_t)ww);
        _cx = _wx0 + (int)(off % (uint32_t)ww);
        len -= n;
    }
    ext_end();
}

void Panel_Mirror::writeBlock(uint32_t rawcolor, uint32_t len)
{
    _pri->writeBlock(rawcolor, len);
    ext_stream(len, [&](uint32_t n) { _ext->writeBlock(rawcolor, n); });
}

void Panel_Mirror::writePixels(pixelcopy_t *param, uint32_t len, bool use_dma)
{
    pixelcopy_t copy = *param;
    _pri->writePixels(param, len, use_dma);
    ext_stream(len, [&](uint32_t n) { _ext->writePixels(&copy, n, false); });
}
