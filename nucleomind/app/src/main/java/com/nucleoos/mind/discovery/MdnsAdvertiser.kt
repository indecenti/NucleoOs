package com.nucleoos.mind.discovery

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import com.nucleoos.mind.core.ServerState

/**
 * Annuncia il server come servizio mDNS/DNS-SD `_anima._tcp`, così il Cardputer
 * (o qualsiasi client) lo scopre automaticamente sulla LAN senza configurare IP.
 */
class MdnsAdvertiser(context: Context) {

    private val nsd = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private var listener: NsdManager.RegistrationListener? = null

    fun register(port: Int) {
        unregister()
        val info = NsdServiceInfo().apply {
            serviceName = "NucleoMind"
            serviceType = "_anima._tcp."
            setPort(port)
            setAttribute("api", "openai")
            setAttribute("engine", ServerState.state.value.engineName)
        }
        val l = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) {
                ServerState.log("mDNS annunciato: ${info.serviceName} (_anima._tcp:$port)")
            }

            override fun onRegistrationFailed(info: NsdServiceInfo, errorCode: Int) {
                ServerState.log("mDNS registrazione fallita (err $errorCode)")
            }

            override fun onServiceUnregistered(info: NsdServiceInfo) {}
            override fun onUnregistrationFailed(info: NsdServiceInfo, errorCode: Int) {}
        }
        listener = l
        try {
            nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, l)
        } catch (e: Exception) {
            ServerState.log("mDNS errore: ${e.message}")
        }
    }

    fun unregister() {
        listener?.let {
            try {
                nsd.unregisterService(it)
            } catch (_: Exception) {
            }
        }
        listener = null
    }
}
