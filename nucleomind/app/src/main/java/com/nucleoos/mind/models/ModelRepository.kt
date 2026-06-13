package com.nucleoos.mind.models

import android.content.Context
import com.nucleoos.mind.core.ServerState
import java.io.File

/** Gestione dei modelli installati nello storage privato dell'app. */
object ModelRepository {

    private const val DIR = "models"

    fun dir(context: Context): File =
        File(context.filesDir, DIR).apply { mkdirs() }

    fun installed(context: Context): List<File> =
        dir(context).listFiles()?.filter { it.isFile && it.length() > 0 }?.sortedBy { it.name }
            ?: emptyList()

    fun isInstalled(context: Context, fileName: String): Boolean =
        File(dir(context), fileName).let { it.exists() && it.length() > 0 }

    fun refresh(context: Context) {
        ServerState.setInstalled(installed(context).map { it.name })
    }

    fun delete(context: Context, fileName: String): Boolean {
        val ok = File(dir(context), fileName).delete()
        refresh(context)
        return ok
    }

    fun sizeMb(context: Context, fileName: String): Int =
        (File(dir(context), fileName).length() / (1024 * 1024)).toInt()
}
