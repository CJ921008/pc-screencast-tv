package com.lanscreencast.tv

import java.util.concurrent.atomic.AtomicReference

class SessionGate {
    private val owner = AtomicReference<String?>(null)
    fun acquire(id: String): Boolean = owner.compareAndSet(null, id)
    fun release(id: String) { owner.compareAndSet(id, null) }
}
