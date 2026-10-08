package com.lanscreencast.tv

/** Thread-confined signaling state; ICE can arrive before remote SDP completes. */
class SignalState {
    enum class Stage { WAITING, PENDING, ACCEPTED, OFFER, ACTIVE }
    var stage = Stage.WAITING
        private set
    private val ice = ArrayList<Triple<String, String, Int>>()
    var remoteReady = false
        private set
    fun hello() { check(stage == Stage.WAITING); stage = Stage.PENDING }
    fun accept() { check(stage == Stage.PENDING); stage = Stage.ACCEPTED }
    fun offer() { check(stage == Stage.ACCEPTED); stage = Stage.OFFER }
    fun activate() { check(stage == Stage.OFFER); stage = Stage.ACTIVE }
    fun candidate(candidate: String, mid: String, index: Int): List<Triple<String, String, Int>> {
        check(stage == Stage.OFFER || stage == Stage.ACCEPTED || stage == Stage.ACTIVE)
        val value = Triple(candidate, mid, index)
        if (remoteReady) return listOf(value)
        check(ice.size < 128)
        ice.add(value)
        return emptyList()
    }
    fun remoteDescriptionReady(): List<Triple<String, String, Int>> {
        remoteReady = true
        return ice.toList().also { ice.clear() }
    }
    fun reset() { stage = Stage.WAITING; remoteReady = false; ice.clear() }
}
