package com.nucleoos.mind.ui

import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import android.graphics.Bitmap
import android.graphics.Color as AColor
import com.google.zxing.BarcodeFormat
import com.google.zxing.qrcode.QRCodeWriter

/** Genera un QR code dell'URL per il pairing istantaneo del client. */
fun qrBitmap(content: String, size: Int = 480): ImageBitmap? {
    return try {
        val matrix = QRCodeWriter().encode(content, BarcodeFormat.QR_CODE, size, size)
        val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        for (x in 0 until size) {
            for (y in 0 until size) {
                bmp.setPixel(x, y, if (matrix[x, y]) AColor.WHITE else AColor.TRANSPARENT)
            }
        }
        bmp.asImageBitmap()
    } catch (_: Exception) {
        null
    }
}
