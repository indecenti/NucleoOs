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
    _rotation = _pri->getRotation();
    _invert = _pri->getInvert();
    _write_depth = _pri->getWriteDepth();
    _read_depth = _pri->getReadDepth();
    if (_solo) {                                   // the external glass, whole and unshifted
        _width = _ext->width();
        _height = _ext->height();
        _ox = _oy = 0;
        return;
    }
    _width = _pri->width();
    _height = _pri->height();
    extdisp_mirror_origin(_ext->width(), _ext->height(), sx(_width), sy(_height), &_ox, &_oy);
}

bool Panel_Mirror::set_fit(bool on)
{
    if (on == _fit) return true;
    if (_pri_open) return false;
    _fit = on;
    sync_dims();
    _wx0 = _wy0 = 0; _wx1 = _wy1 = -1; _cx = _cy = 0;
    return true;
}

void Panel_Mirror::pri_row(int y, uint16_t *out)
{
    pixelcopy_t pc(nullptr, _ext->getWriteDepth(), _pri->getReadDepth(), false);
    _pri->readRect(0, y, _pri->width(), 1, out, &pc);
}

bool Panel_Mirror::ext_row(int y, uint16_t *out, uint16_t *tmp)
{
    if (_solo) return false;
    const int ew = _ext->width(), pw = _pri->width();
    for (int x = 0; x < ew; x++) out[x] = 0;
    const int fy = y - _oy;
    if (fy < 0 || fy >= frame_h()) return true;
    pri_row(_fit ? (3 * fy + 2) / 4 : fy, tmp);           // the inverse of sy(): the source row of panel row y
    for (int x = 0; x < pw; x++)
        for (int k = sx(x), e = sx(x + 1); k < e; k++) if (_ox + k < ew) out[_ox + k] = tmp[x];
    return true;
}

// Fit helpers. Integer nearest mapping: internal column x covers external columns [sx(x), sx(x+1)), so
// adjacent primitives tile the panel with no gap or overlap, and 240 -> 320 / 135 -> 180 exactly.
void Panel_Mirror::ext_fill(int x, int y, int w, int h, uint32_t rawcolor)
{
    const int X0 = sx(x), X1 = sx(x + w), Y0 = sy(y), Y1 = sy(y + h);
    if (X1 > X0 && Y1 > Y0) _ext->writeFillRectPreclipped(X0 + _ox, Y0 + _oy, X1 - X0, Y1 - Y0, rawcolor);
}

void Panel_Mirror::emit_row(int x, int y, int n)
{
    const int X0 = sx(x), X1 = sx(x + n), Y0 = sy(y), Y1 = sy(y + 1);
    if (n <= 0 || X1 <= X0) return;
    for (int i = 0; i < n; i++)
        for (int k = sx(x + i) - X0, e = sx(x + i + 1) - X0; k < e; k++) _out[k] = _row[i];
    for (int yy = Y0; yy < Y1; yy++) {
        pixelcopy_t pc(_out, _ext->getWriteDepth(), _ext->getWriteDepth(), false);
        _ext->writeImage(X0 + _ox, yy + _oy, X1 - X0, 1, &pc, false);
    }
}

bool Panel_Mirror::set_solo(bool on)
{
    if (on == _solo) return true;
    if (_pri_open || (on && _ext_off)) return false;   // a frame is mid-flight / nothing to be solo on
    _solo = on;
    sync_dims();
    _wx0 = _wy0 = 0; _wx1 = _wy1 = -1; _cx = _cy = 0;   // no pixel stream survives a switch
    _pri->setBrightness(pri_level(_bri));
    return true;
}

bool Panel_Mirror::set_ext_off(bool on)
{
    if (on == _ext_off) return true;
    if (_pri_open || _solo) return false;
    _ext_off = on;
    _wx0 = _wy0 = 0; _wx1 = _wy1 = -1; _cx = _cy = 0;
    return true;
}

void Panel_Mirror::set_pri_off(bool on)
{
    _pri_off = on;
    _pri->setBrightness(pri_level(_bri));
}

// The internal panel nests under the mirror's own count: its internal startWrite/endWrite pairs (readRect,
// readCommand, setRotation...) then never close the outer transaction early. The external panel is NOT
// opened here — see the bus rule in the header.
void Panel_Mirror::beginTransaction(void) { _frame++; if (!_solo) { _pri_open++; _pri->startWrite(); } }
void Panel_Mirror::endTransaction(void)
{
    if (_frame && --_frame == 0) drop_hold();     // the frame is over: the bus goes back
    if (_pri_open) { _pri_open--; _pri->endWrite(); }
}

// ---- frame hold ------------------------------------------------------------------------------------------
void Panel_Mirror::drop_hold(void)
{
    if (!_held) return;
    _held = false;
    bus_close();
}

void Panel_Mirror::release_hold(void)
{
    if (_held && (!_self || _self() == _owner)) drop_hold();
}

void Panel_Mirror::ext_begin(void)
{
    if (_held) {
        if (_now() - _held_t0 <= _budget) return;  // still inside the frame and the budget: already open
        drop_hold();                               // budget spent: let a waiting SD transfer through
    }
    bus_open();
    if (_now && _self && _frame > 0 && !_hold_off) { _held = true; _owner = _self(); _held_t0 = _now(); }
}

void Panel_Mirror::ext_end(void)
{
    if (_held) {
        if (_now() - _held_t0 > _budget) drop_hold();
        return;
    }
    bus_close();
}

void Panel_Mirror::setBrightness(uint8_t brightness)
{
    _bri = brightness;
    _pri->setBrightness(pri_level(brightness));
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
void Panel_Mirror::setSleep(bool flg)
{
    _pri->setSleep(flg);
    if (_lock) _lock();
    _ext->setSleep(flg);
    if (_unlock) _unlock();
}
void Panel_Mirror::setPowerSave(bool flg)
{
    _pri->setPowerSave(flg);
    if (_lock) _lock();
    _ext->setPowerSave(flg);
    if (_unlock) _unlock();
}

void Panel_Mirror::writeCommand(uint32_t cmd, uint_fast8_t length) { _pri->writeCommand(cmd, length); }
void Panel_Mirror::writeData(uint32_t data, uint_fast8_t length)   { _pri->writeData(data, length); }

void Panel_Mirror::initDMA(void)    { _pri->initDMA(); }
void Panel_Mirror::waitDMA(void)    { _pri->waitDMA(); }
bool Panel_Mirror::dmaBusy(void)    { return _pri->dmaBusy(); }
void Panel_Mirror::waitDisplay(void) { _pri->waitDisplay(); }
bool Panel_Mirror::displayBusy(void) { return _pri->displayBusy(); }
void Panel_Mirror::display(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h) { _pri->display(x, y, w, h); }
bool Panel_Mirror::isReadable(void) const { return !_solo && _pri->isReadable(); }

uint32_t Panel_Mirror::readCommand(uint_fast16_t cmd, uint_fast8_t index, uint_fast8_t length) { return _pri->readCommand(cmd, index, length); }
uint32_t Panel_Mirror::readData(uint_fast8_t index, uint_fast8_t length) { return _pri->readData(index, length); }
void Panel_Mirror::readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void *dst, pixelcopy_t *param)
{
    if (_solo) return;                            // write-only glass: the destination is left as it was
    _pri->readRect(x, y, w, h, dst, param);
}
int32_t Panel_Mirror::getScanLine(void) { return _pri->getScanLine(); }

// ---- self-contained primitives: one external transaction each, offset into the centred frame ----------

void Panel_Mirror::drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t rawcolor)
{
    if (!_solo) _pri->drawPixelPreclipped(x, y, rawcolor);
    if (_ext_off || _dark) return;
    ext_begin();
    if (fitting()) ext_fill(x, y, 1, 1, rawcolor);
    else _ext->drawPixelPreclipped(x + _ox, y + _oy, rawcolor);
    ext_end();
}

void Panel_Mirror::writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t rawcolor)
{
    if (!_solo) _pri->writeFillRectPreclipped(x, y, w, h, rawcolor);
    if (_ext_off || _dark) return;
    ext_begin();
    if (fitting()) ext_fill(x, y, w, h, rawcolor);
    else _ext->writeFillRectPreclipped(x + _ox, y + _oy, w, h, rawcolor);
    ext_end();
}

// pixelcopy_t is consumed as it is read (it advances its source position), so the external panel gets a
// copy taken BEFORE the internal write. The external write is synchronous (no DMA): it reads the source
// before this call returns, while the caller's buffer is still alive.
void Panel_Mirror::writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t *param, bool use_dma)
{
    pixelcopy_t copy = *param;
    if (!_solo) _pri->writeImage(x, y, w, h, param, use_dma);
    if (_ext_off || _dark) return;
    if (fitting()) {
        // Opaque, unscaled source in the panel's own 16-bit format: decode each row exactly as the panel
        // driver does (rewind src_x, fp_copy, next src_y) and scale it. Anything else: read back and scale.
        if (copy.transp == pixelcopy_t::NON_TRANSP && copy.dst_bits == 16 && copy.src_y32_add == 0 &&
            copy.src_x32_add == (1u << pixelcopy_t::FP_SCALE) && w <= 240) {
            ext_begin();
            const int16_t src_x = copy.src_x;
            for (uint_fast16_t r = 0; r < h; r++) {
                copy.src_x = src_x;
                copy.fp_copy(_row, 0, w, &copy);
                copy.src_y++;
                emit_row(x, y + r, w);
            }
            ext_end();
        } else {
            mirror_from_primary(x, y, w, h);
        }
        return;
    }
    ext_begin();
    _ext->writeImage(x + _ox, y + _oy, w, h, &copy, false);
    ext_end();
}

// Alpha blending reads the destination back. The external panel's MISO is normally not wired (its SDO
// does not release the shared line), so blend on the internal panel and copy the result across.
void Panel_Mirror::writeImageARGB(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t *param)
{
    if (_solo) {
        // The external glass cannot be read, and the generic ARGB path reads the destination back (an SPI read
        // per block that returns nothing: ~2 s for one anti-aliased icon). Instead write the covered pixels
        // (alpha >= 50 %) as opaque runs and skip the rest — no readback, no buffer beyond the row we own.
        if (param->src_depth == argb8888_t::depth && param->src_y32_add == 0 && w <= 320) {
            const argb8888_t *src = (const argb8888_t *)param->src_data;
            ext_begin();
            for (uint_fast16_t r = 0; r < h; r++) {
                const argb8888_t *row = src + (size_t)(param->src_y + r) * param->src_bitwidth;
                int run = -1;
                for (uint_fast16_t i = 0; i <= w; i++) {
                    bool on = false;
                    if (i < w) {
                        const argb8888_t &px = row[(param->src_x32 + i * param->src_x32_add) >> pixelcopy_t::FP_SCALE];
                        if (px.a >= 128) {
                            uint16_t c = (uint16_t)(((px.r & 0xF8) << 8) | ((px.g & 0xFC) << 3) | (px.b >> 3));
                            _out[i] = (uint16_t)((c >> 8) | (c << 8));   // the panel's byte-swapped RGB565
                            on = true;
                        }
                    }
                    if (on && run < 0) run = (int)i;
                    if (!on && run >= 0) {
                        pixelcopy_t pc(_out + run, _ext->getWriteDepth(), _ext->getWriteDepth(), false);
                        _ext->writeImage(x + run, y + r, i - run, 1, &pc, false);
                        run = -1;
                    }
                }
            }
            ext_end();
            return;
        }
        ext_begin();
        _ext->writeImageARGB(x, y, w, h, param);
        ext_end();
        return;
    }
    _pri->writeImageARGB(x, y, w, h, param);
    if (!_ext_off && !_dark) mirror_from_primary(x, y, w, h);
}

// Same for copyRect (scroll): do it on the internal panel, then copy the destination rows across.
void Panel_Mirror::copyRect(uint_fast16_t dst_x, uint_fast16_t dst_y, uint_fast16_t w, uint_fast16_t h, uint_fast16_t src_x, uint_fast16_t src_y)
{
    if (_solo) return;                            // needs a readback the external glass cannot give
    _pri->copyRect(dst_x, dst_y, w, h, src_x, src_y);
    if (!_ext_off && !_dark) mirror_from_primary(dst_x, dst_y, w, h);
}

// In 64-px runs (192 B of stack, safe on the 8 KB game tasks that draw direct), ONE external transaction per
// row: short enough that an SD transfer waits at most one row, few enough that a full-screen alpha overlay
// costs 135 bus claims instead of 540.
void Panel_Mirror::mirror_from_primary(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || !_pri->isReadable()) return;
    if (fitting()) {                              // whole rows (<= 240 px), read in the panel's write format
        pixelcopy_t pc_read(nullptr, _ext->getWriteDepth(), _pri->getReadDepth(), false);
        for (int row = 0; row < h; row++) {
            _pri->readRect(x, y + row, w, 1, _row, &pc_read);
            ext_begin();
            emit_row(x, y + row, w);
            ext_end();
        }
        return;
    }
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
    if (!_solo) _pri->setWindow(xs, ys, xe, ye);
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

// Fit: walk the stream from the tracked cursor as row runs (x, y, n) inside the window — never longer than
// a row, so each run fits _row — under ONE external transaction. Past the window's end it wraps to the top.
template <typename F>
void Panel_Mirror::fit_stream(uint32_t len, F &&row_run)
{
    const int ww = _wx1 - _wx0 + 1;
    if (ww <= 0 || _wy1 < _wy0) return;
    ext_begin();
    while (len) {
        if (_cy > _wy1) { _cx = _wx0; _cy = _wy0; }
        uint32_t n = (uint32_t)(_wx1 - _cx + 1);
        if (n > len) n = len;
        row_run(_cx, _cy, (int)n);
        _cx += (int)n;
        if (_cx > _wx1) { _cx = _wx0; _cy++; }
        len -= n;
    }
    ext_end();
}

void Panel_Mirror::writeBlock(uint32_t rawcolor, uint32_t len)
{
    if (!_solo) _pri->writeBlock(rawcolor, len);
    if (_ext_off || _dark) return;
    if (fitting()) { fit_stream(len, [&](int x, int y, int n) { ext_fill(x, y, n, 1, rawcolor); }); return; }
    ext_stream(len, [&](uint32_t n) { _ext->writeBlock(rawcolor, n); });
}

void Panel_Mirror::writePixels(pixelcopy_t *param, uint32_t len, bool use_dma)
{
    pixelcopy_t copy = *param;
    if (!_solo) _pri->writePixels(param, len, use_dma);
    if (_ext_off || _dark) return;
    if (fitting()) {
        if (copy.transp == pixelcopy_t::NON_TRANSP && copy.dst_bits == 16) {
            fit_stream(len, [&](int x, int y, int n) { copy.fp_copy(_row, 0, n, &copy); emit_row(x, y, n); });
        } else {                                  // read the touched window rows back
            const int ww = _wx1 - _wx0 + 1, y0 = _cy;
            uint32_t off = (uint32_t)(_cx - _wx0) + len;
            _cy += (int)(off / (uint32_t)(ww > 0 ? ww : 1)); _cx = _wx0 + (int)(off % (uint32_t)(ww > 0 ? ww : 1));
            int y1 = _cy > _wy1 ? _wy1 : _cy;
            mirror_from_primary(_wx0, y0, ww, y1 - y0 + 1);
        }
        return;
    }
    ext_stream(len, [&](uint32_t n) { _ext->writePixels(&copy, n, false); });
}
