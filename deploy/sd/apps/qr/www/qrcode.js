/**
 * NucleoQR — QR encoder, a faithful JS port of the MIT C encoder shipped in M5GFX
 * (lgfx/utility/lgfx_qrcode.c, original by Richard Moore, https://github.com/ricmoo/QRCode/,
 * "heavily inspired by Nayuki"). Ported line-for-line so the web app produces the EXACT same
 * codes the on-device (native) renderer draws. MIT License (c) 2017 Richard Moore.
 *
 * API:  NucleoQR.generate(text, {ecc}) -> { version, size, ecc, mask, get(x,y) } | null
 *       ECC levels: NucleoQR.ECC.LOW (default, matches the native d.qrcode), MEDIUM, QUARTILE, HIGH
 */
(function (global) {
  'use strict';

  // ── Error-correction lookup tables (rows: Medium, Low, High, Quartile) ──
  var NUM_EC_CODEWORDS = [
    [10,16,26,36,48,64,72,88,110,130,150,176,198,216,240,280,308,338,364,416,442,476,504,560,588,644,700,728,784,812,868,924,980,1036,1064,1120,1204,1260,1316,1372],
    [7,10,15,20,26,36,40,48,60,72,80,96,104,120,132,144,168,180,196,224,224,252,270,300,312,336,360,390,420,450,480,510,540,570,570,600,630,660,720,750],
    [17,28,44,64,88,112,130,156,192,224,264,308,352,384,432,480,532,588,650,700,750,816,900,960,1050,1110,1200,1260,1350,1440,1530,1620,1710,1800,1890,1980,2100,2220,2310,2430],
    [13,22,36,52,72,96,108,132,160,192,224,260,288,320,360,408,448,504,546,600,644,690,750,810,870,952,1020,1050,1140,1200,1290,1350,1440,1530,1590,1680,1770,1860,1950,2040]
  ];
  var NUM_EC_BLOCKS = [
    [1,1,1,2,2,4,4,4,5,5,5,8,9,9,10,10,11,13,14,16,17,17,18,20,21,23,25,26,28,29,31,33,35,37,38,40,43,45,47,49],
    [1,1,1,1,1,2,2,2,2,4,4,4,4,4,6,6,6,6,7,8,8,9,9,10,12,12,12,13,14,15,16,17,18,19,19,20,21,22,24,25],
    [1,1,2,4,4,4,5,6,8,8,11,11,16,16,18,16,19,21,25,25,25,34,30,32,35,37,40,42,45,48,51,54,57,60,63,66,70,74,77,81],
    [1,1,2,2,4,4,6,6,8,8,8,10,12,16,12,17,16,18,21,20,23,23,25,27,29,34,34,35,38,40,43,45,48,51,53,56,59,62,65,68]
  ];
  var NUM_RAW_DATA_MODULES = [208,359,567,807,1079,1383,1568,1936,2336,2768,3232,3728,4256,4651,5243,5867,6523,7211,7931,8683,9252,10068,10916,11796,12708,13652,14628,15371,16411,17483,18587,19723,20891,22091,23008,24272,25568,26896,28256,29648];

  var ECC = { LOW: 0, MEDIUM: 1, QUARTILE: 2, HIGH: 3 };
  var MODE_NUMERIC = 0, MODE_ALPHANUMERIC = 1, MODE_BYTE = 2;
  // Maps a logical ECC level to its table/format index (== C: ECC_FORMAT_BITS >> (2*ecc)).
  var ECC_FORMAT_BITS = (0x02 << 6) | (0x03 << 4) | (0x00 << 2) | (0x01 << 0);

  function maxi(a, b) { return a > b ? a : b; }
  function absi(v) { return v < 0 ? -v : v; }

  function getAlphanumeric(c) {
    if (c >= 48 && c <= 57) return c - 48;        // 0-9
    if (c >= 65 && c <= 90) return c - 65 + 10;   // A-Z
    switch (c) {
      case 32: return 36; case 36: return 37; case 37: return 38; case 42: return 39;
      case 43: return 40; case 45: return 41; case 46: return 42; case 47: return 43; case 58: return 44;
    }
    return -1;
  }
  function isAlphanumeric(b) { for (var i = 0; i < b.length; i++) if (getAlphanumeric(b[i]) === -1) return false; return true; }
  function isNumeric(b) { for (var i = 0; i < b.length; i++) if (b[i] < 48 || b[i] > 57) return false; return true; }

  function getModeBits(version, mode) {
    var modeInfo = 0x7bbb80a;
    if (version > 9) modeInfo >>>= 9;
    if (version > 26) modeInfo >>>= 9;
    var result = 8 + ((modeInfo >>> (3 * mode)) & 0x07);
    if (result === 15) result = 16;
    return result;
  }

  // ── BitBucket: { w, cap, data:Uint8Array } (w = bitOffset for buffers, or grid width) ──
  function gridSizeBytes(size) { return ((size * size) + 7) >> 3; }
  function bufSizeBytes(bits) { return (bits + 7) >> 3; }
  function newBuffer(cap) { return { w: 0, cap: cap, data: new Uint8Array(cap) }; }
  function newGrid(size) { return { w: size, cap: gridSizeBytes(size), data: new Uint8Array(gridSizeBytes(size)) }; }

  function appendBits(bb, val, length) {
    var offset = bb.w;
    for (var i = length - 1; i >= 0; i--, offset++) {
      var index = offset >> 3;
      if (bb.cap <= index) return false;
      bb.data[index] |= ((val >>> i) & 1) << (7 - (offset & 7));
    }
    bb.w = offset;
    return true;
  }
  function setBit(g, x, y, on) {
    var offset = y * g.w + x, mask = 1 << (7 - (offset & 7));
    if (on) g.data[offset >> 3] |= mask; else g.data[offset >> 3] &= ~mask;
  }
  function invertBit(g, x, y, invert) {
    var offset = y * g.w + x, mask = 1 << (7 - (offset & 7));
    var on = (g.data[offset >> 3] & mask) !== 0;
    if (on ^ invert) g.data[offset >> 3] |= mask; else g.data[offset >> 3] &= ~mask;
  }
  function getBit(g, x, y) {
    var offset = y * g.w + x;
    return (g.data[offset >> 3] & (1 << (7 - (offset & 7)))) !== 0;
  }

  function applyMask(modules, isFunction, mask) {
    var size = modules.w;
    for (var y = 0; y < size; y++) {
      for (var x = 0; x < size; x++) {
        if (getBit(isFunction, x, y)) continue;
        var invert = false;
        switch (mask) {
          case 0: invert = ((x + y) & 1) === 0; break;
          case 1: invert = (y & 1) === 0; break;
          case 2: invert = (x % 3) === 0; break;
          case 3: invert = ((x + y) % 3) === 0; break;
          case 4: invert = (((x / 3 | 0) + (y >> 1)) & 1) === 0; break;
          case 5: invert = (((x * y) & 1) + (x * y % 3)) === 0; break;
          case 6: invert = ((((x * y) & 1) + (x * y % 3)) & 1) === 0; break;
          case 7: invert = ((((x + y) & 1) + (x * y % 3)) & 1) === 0; break;
        }
        invertBit(modules, x, y, invert);
      }
    }
  }
  function setFunctionModule(modules, isFunction, x, y, on) { setBit(modules, x, y, on); setBit(isFunction, x, y, true); }

  function drawFinderPattern(modules, isFunction, x, y) {
    var size = modules.w;
    for (var i = -4; i <= 4; i++) for (var j = -4; j <= 4; j++) {
      var dist = maxi(absi(i), absi(j)), xx = x + j, yy = y + i;
      if (0 <= xx && xx < size && 0 <= yy && yy < size) setFunctionModule(modules, isFunction, xx, yy, dist !== 2 && dist !== 4);
    }
  }
  function drawAlignmentPattern(modules, isFunction, x, y) {
    for (var i = -2; i <= 2; i++) for (var j = -2; j <= 2; j++)
      setFunctionModule(modules, isFunction, x + j, y + i, maxi(absi(i), absi(j)) !== 1);
  }
  function drawFormatBits(modules, isFunction, ecc, mask) {
    var size = modules.w;
    var data = (ecc << 3) | mask, rem = data, i;
    for (i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    data = (data << 10 | rem) ^ 0x5412;
    for (i = 0; i <= 5; i++) setFunctionModule(modules, isFunction, 8, i, ((data >> i) & 1) !== 0);
    setFunctionModule(modules, isFunction, 8, 7, ((data >> 6) & 1) !== 0);
    setFunctionModule(modules, isFunction, 8, 8, ((data >> 7) & 1) !== 0);
    setFunctionModule(modules, isFunction, 7, 8, ((data >> 8) & 1) !== 0);
    for (i = 9; i < 15; i++) setFunctionModule(modules, isFunction, 14 - i, 8, ((data >> i) & 1) !== 0);
    for (i = 0; i <= 7; i++) setFunctionModule(modules, isFunction, size - 1 - i, 8, ((data >> i) & 1) !== 0);
    for (i = 8; i < 15; i++) setFunctionModule(modules, isFunction, 8, size - 15 + i, ((data >> i) & 1) !== 0);
    setFunctionModule(modules, isFunction, 8, size - 8, true);
  }
  function drawVersion(modules, isFunction, version) {
    if (version < 7) return;
    var rem = version, i;
    for (i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    var data = version << 12 | rem;
    for (i = 0; i < 18; i++) {
      var bit = ((data >> i) & 1) !== 0;
      var a = modules.w - 11 + i % 3, b = (i / 3) | 0;
      setFunctionModule(modules, isFunction, a, b, bit);
      setFunctionModule(modules, isFunction, b, a, bit);
    }
  }
  function drawFunctionPatterns(modules, isFunction, version, ecc) {
    var size = modules.w, i;
    for (i = 0; i < size; i++) {
      setFunctionModule(modules, isFunction, 6, i, (i & 1) === 0);
      setFunctionModule(modules, isFunction, i, 6, (i & 1) === 0);
    }
    drawFinderPattern(modules, isFunction, 3, 3);
    drawFinderPattern(modules, isFunction, size - 4, 3);
    drawFinderPattern(modules, isFunction, 3, size - 4);
    if (version > 1) {
      var alignCount = (version / 7 | 0) + 2, step;
      if (version !== 32) step = (((version * 4 + alignCount * 2 + 1) / (2 * alignCount - 2)) | 0) * 2;
      else step = 26;
      var alignPosition = new Array(alignCount);
      alignPosition[0] = 6;
      var idx = alignCount - 1, size_ = version * 4 + 17;
      for (i = 0, pos = size_ - 7; i < alignCount - 1; i++, pos -= step) alignPosition[idx--] = pos;
      var pos;
      for (i = 0; i < alignCount; i++) for (var j = 0; j < alignCount; j++) {
        if ((i === 0 && j === 0) || (i === 0 && j === alignCount - 1) || (i === alignCount - 1 && j === 0)) continue;
        drawAlignmentPattern(modules, isFunction, alignPosition[i], alignPosition[j]);
      }
    }
    drawFormatBits(modules, isFunction, ecc, 0);
    drawVersion(modules, isFunction, version);
  }
  function drawCodewords(modules, isFunction, codewords) {
    var bitLength = codewords.w, data = codewords.data, size = modules.w, i = 0;
    for (var right = size - 1; right >= 1; right -= 2) {
      if (right === 6) right = 5;
      for (var vert = 0; vert < size; vert++) {
        for (var j = 0; j < 2; j++) {
          var x = right - j;
          var upwards = ((right & 2) === 0) ^ (x < 6);
          var y = upwards ? size - 1 - vert : vert;
          if (!getBit(isFunction, x, y) && i < bitLength) {
            setBit(modules, x, y, ((data[i >> 3] >> (7 - (i & 7))) & 1) !== 0);
            i++;
          }
        }
      }
    }
  }

  var PN1 = 3, PN2 = 3, PN3 = 40, PN4 = 10;
  function getPenaltyScore(modules) {
    var result = 0, size = modules.w, x, y;
    for (y = 0; y < size; y++) {
      var colorX = getBit(modules, 0, y);
      for (x = 1, runX = 1; x < size; x++) {
        var cx = getBit(modules, x, y);
        if (cx !== colorX) { colorX = cx; runX = 1; }
        else { runX++; if (runX === 5) result += PN1; else if (runX > 5) result++; }
      }
      var runX;
    }
    for (x = 0; x < size; x++) {
      var colorY = getBit(modules, x, 0);
      for (y = 1, runY = 1; y < size; y++) {
        var cy = getBit(modules, x, y);
        if (cy !== colorY) { colorY = cy; runY = 1; }
        else { runY++; if (runY === 5) result += PN1; else if (runY > 5) result++; }
      }
      var runY;
    }
    var black = 0;
    for (y = 0; y < size; y++) {
      var bitsRow = 0, bitsCol = 0;
      for (x = 0; x < size; x++) {
        var color = getBit(modules, x, y);
        if (x > 0 && y > 0) {
          var cUL = getBit(modules, x - 1, y - 1), cUR = getBit(modules, x, y - 1), cL = getBit(modules, x - 1, y);
          if (color === cUL && color === cUR && color === cL) result += PN2;
        }
        bitsRow = ((bitsRow << 1) & 0x7FF) | (color ? 1 : 0);
        bitsCol = ((bitsCol << 1) & 0x7FF) | (getBit(modules, y, x) ? 1 : 0);
        if (x >= 10) {
          if (bitsRow === 0x05D || bitsRow === 0x5D0) result += PN3;
          if (bitsCol === 0x05D || bitsCol === 0x5D0) result += PN3;
        }
        if (color) black++;
      }
    }
    var total = size * size;
    for (var k = 0; black * 20 < (9 - k) * total || black * 20 > (11 + k) * total; k++) result += PN4;
    return result;
  }

  function rsMultiply(x, y) {
    var z = 0;
    for (var i = 7; i >= 0; i--) {
      z = (z << 1) ^ ((z >> 7) * 0x11D);
      z ^= ((y >> i) & 1) * x;
    }
    return z & 0xFF;
  }
  function rsInit(degree) {
    var coeff = new Uint8Array(degree);
    coeff[degree - 1] = 1;
    var root = 1;
    for (var i = 0; i < degree; i++) {
      for (var j = 0; j < degree; j++) {
        coeff[j] = rsMultiply(coeff[j], root);
        if (j + 1 < degree) coeff[j] ^= coeff[j + 1];
      }
      root = (root << 1) ^ ((root >> 7) * 0x11D);
    }
    return coeff;
  }
  // result[resBase + j*stride] holds the remainder, MSB..LSB across the stride.
  function rsGetRemainder(degree, coeff, data, dataOff, length, result, resBase, stride) {
    for (var i = 0; i < length; i++) {
      var factor = data[dataOff + i] ^ result[resBase];
      for (var j = 1; j < degree; j++) result[resBase + (j - 1) * stride] = result[resBase + j * stride];
      result[resBase + (degree - 1) * stride] = 0;
      for (j = 0; j < degree; j++) result[resBase + j * stride] ^= rsMultiply(coeff[j], factor);
    }
  }

  function encodeDataCodewords(cw, bytes, version) {
    var length = bytes.length, i;
    if (isNumeric(bytes)) {
      appendBits(cw, 1 << MODE_NUMERIC, 4);
      appendBits(cw, length, getModeBits(version, MODE_NUMERIC));
      var acc = 0, n = 0;
      for (i = 0; i < length; i++) {
        acc = acc * 10 + (bytes[i] - 48); n++;
        if (n === 3) { if (!appendBits(cw, acc, 10)) return -1; acc = 0; n = 0; }
      }
      if (n > 0) appendBits(cw, acc, n * 3 + 1);
      return MODE_NUMERIC;
    } else if (isAlphanumeric(bytes)) {
      appendBits(cw, 1 << MODE_ALPHANUMERIC, 4);
      appendBits(cw, length, getModeBits(version, MODE_ALPHANUMERIC));
      var ad = 0, ac = 0;
      for (i = 0; i < length; i++) {
        ad = ad * 45 + getAlphanumeric(bytes[i]); ac++;
        if (ac === 2) { if (!appendBits(cw, ad, 11)) return -1; ad = 0; ac = 0; }
      }
      if (ac > 0) appendBits(cw, ad, 6);
      return MODE_ALPHANUMERIC;
    } else {
      appendBits(cw, 1 << MODE_BYTE, 4);
      appendBits(cw, length, getModeBits(version, MODE_BYTE));
      for (i = 0; i < length; i++) if (!appendBits(cw, bytes[i], 8)) return -1;
      return MODE_BYTE;
    }
  }

  function performErrorCorrection(version, ecc, cw) {
    var numBlocks = NUM_EC_BLOCKS[ecc][version - 1];
    var totalEcc = NUM_EC_CODEWORDS[ecc][version - 1];
    var moduleCount = NUM_RAW_DATA_MODULES[version - 1];
    var blockEccLen = (totalEcc / numBlocks) | 0;
    var numShortBlocks = numBlocks - (((moduleCount / 8) | 0) % numBlocks);
    var shortBlockLen = (((moduleCount / 8) | 0) / numBlocks) | 0;
    var shortDataBlockLen = shortBlockLen - blockEccLen;

    var result = new Uint8Array(cw.cap);
    var coeff = rsInit(blockEccLen);
    var offset = 0, dataBytes = cw.data, blockNum, index, stride;

    for (var i = 0; i < shortDataBlockLen; i++) {
      index = i; stride = shortDataBlockLen;
      for (blockNum = 0; blockNum < numBlocks; blockNum++) {
        if (offset === cw.cap) return false;
        result[offset++] = dataBytes[index];
        if (blockNum === numShortBlocks) stride++;
        index += stride;
      }
    }
    index = shortDataBlockLen * (numShortBlocks + 1); stride = shortDataBlockLen;
    for (blockNum = 0; blockNum < numBlocks - numShortBlocks; blockNum++) {
      if (offset === cw.cap) return false;
      result[offset++] = dataBytes[index];
      if (blockNum === 0) stride++;
      index += stride;
    }
    var blockSize = shortDataBlockLen, dbOff = 0;
    for (blockNum = 0; blockNum < numBlocks; blockNum++) {
      if (blockNum === numShortBlocks) blockSize++;
      if (offset + blockNum >= cw.cap) return false;
      rsGetRemainder(blockEccLen, coeff, dataBytes, dbOff, blockSize, result, offset + blockNum, numBlocks);
      dbOff += blockSize;
    }
    cw.data.set(result);
    cw.w = moduleCount;
    return true;
  }

  function initBytes(version, ecc, bytes) {
    var size = version * 4 + 17;
    var eccFmt = (ECC_FORMAT_BITS >> (2 * ecc)) & 0x03;
    var moduleCount = NUM_RAW_DATA_MODULES[version - 1];
    var dataCapacity = (moduleCount >> 3) - NUM_EC_CODEWORDS[eccFmt][version - 1];

    var cw = newBuffer(bufSizeBytes(moduleCount));
    var mode = encodeDataCodewords(cw, bytes, version);
    if (mode < 0) return null;

    var padding = (dataCapacity * 8) - cw.w;
    if (padding < 0) return null;
    if (padding > 4) padding = 4;
    appendBits(cw, 0, padding);
    appendBits(cw, 0, (8 - (cw.w & 7)) & 7);
    for (var padByte = 0xEC; cw.w < dataCapacity * 8; padByte ^= 0xEC ^ 0x11) appendBits(cw, padByte, 8);

    var modules = newGrid(size);
    var isFunction = newGrid(size);
    drawFunctionPatterns(modules, isFunction, version, eccFmt);
    if (!performErrorCorrection(version, eccFmt, cw)) return null;
    drawCodewords(modules, isFunction, cw);

    var mask = 0, minPenalty = 0xFFFFFFFF;
    for (var i = 0; i < 8; i++) {
      drawFormatBits(modules, isFunction, eccFmt, i);
      applyMask(modules, isFunction, i);
      var p = getPenaltyScore(modules);
      if (p < minPenalty) { mask = i; minPenalty = p; }
      applyMask(modules, isFunction, i);
    }
    drawFormatBits(modules, isFunction, eccFmt, mask);
    applyMask(modules, isFunction, mask);

    var data = modules.data;
    return {
      version: version, size: size, ecc: ecc, mask: mask,
      get: function (x, y) {
        if (x < 0 || y < 0 || x >= size || y >= size) return false;
        var off = y * size + x;
        return (data[off >> 3] & (1 << (7 - (off & 7)))) !== 0;
      }
    };
  }

  // Public: encode text, auto-growing the version until it fits (mirrors LovyanGFX d.qrcode).
  function generate(text, opts) {
    opts = opts || {};
    var ecc = (opts.ecc != null) ? opts.ecc : ECC.LOW;
    var bytes = new TextEncoder().encode(String(text));
    for (var version = (opts.minVersion || 1); version <= 40; version++) {
      var qr = initBytes(version, ecc, bytes);
      if (qr) return qr;
    }
    return null;
  }

  global.NucleoQR = { generate: generate, ECC: ECC };
})(typeof window !== 'undefined' ? window : this);
