package com.nucleoos.mind.core

import java.net.Inet4Address
import java.net.NetworkInterface

object Net {
    /**
     * IPv4 locale utilizzabile dai client sulla stessa rete (Wi‑Fi o hotspot).
     * Enumeriamo le interfacce invece di usare WifiManager così funziona anche
     * quando il telefono è in modalità hotspot.
     */
    fun localIpv4(): String {
        try {
            val ifaces = NetworkInterface.getNetworkInterfaces() ?: return "0.0.0.0"
            for (iface in ifaces) {
                if (!iface.isUp || iface.isLoopback) continue
                val name = iface.name.lowercase()
                // Salta interfacce virtuali/VPN dove possibile.
                if (name.startsWith("dummy")) continue
                for (addr in iface.inetAddresses) {
                    if (addr is Inet4Address && !addr.isLoopbackAddress) {
                        val ip = addr.hostAddress ?: continue
                        // Preferisci indirizzi privati di LAN/hotspot.
                        if (ip.startsWith("192.168.") || ip.startsWith("10.") ||
                            ip.startsWith("172.") || ip.startsWith("169.254.")
                        ) {
                            return ip
                        }
                    }
                }
            }
        } catch (_: Exception) {
        }
        return "0.0.0.0"
    }
}
