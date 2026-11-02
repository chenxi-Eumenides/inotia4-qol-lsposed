package com.inotia4.qol.util

/**
 * Standalone pure checks for the direct-route lease flow and status mapping.
 * No Android/JNI dependency: the native enter/leave seam is injected as fakes.
 */
object NativeRouteLeaseDecisionTest {
    @JvmStatic
    fun main(args: Array<String>) {
        // ---- return-code -> HTTP status mapping ----
        check(NativeRouteLease.failure(0) == null)
        check(NativeRouteLease.failure(1)?.status == 400)
        check(NativeRouteLease.failure(2)?.status == 404)
        check(NativeRouteLease.failure(3)?.status == 503)
        check(NativeRouteLease.failure(9)?.status == 403)
        check(NativeRouteLease.failure(10)?.status == 503)
        check(NativeRouteLease.failure(12345)?.status == 500)

        // ---- enter failure: handler not run, no leave ----
        var failedHandlerRan = false
        var failedLeaveCount = 0
        try {
            withRouteLease(
                enter = { _, _, _ -> 9 },
                leave = { failedLeaveCount++; 0 },
                method = 1,
                path = "/api/op/quest/accept",
                onLeaveError = { _, _ -> },
                onFailure = { status, message -> throw LeaseFailure(status, message) },
            ) {
                failedHandlerRan = true
                "unreachable"
            }
            error("expected lease failure")
        } catch (e: LeaseFailure) {
            check(e.status == 403)
        }
        check(!failedHandlerRan && failedLeaveCount == 0)

        // ---- success with a lease token: leave exactly once ----
        var leaveCount = 0
        val body = withRouteLease(
            enter = { _, _, out -> out[0] = 42L; 0 },
            leave = { leaveCount++; 0 },
            method = 0,
            path = "/api/system/features",
            onLeaveError = { _, _ -> },
            onFailure = { status, message -> throw LeaseFailure(status, message) },
        ) { "body" }
        check(body == "body" && leaveCount == 1)

        // ---- success with token 0 (lease_required=false): no leave ----
        var zeroTokenLeave = 0
        val zeroBody = withRouteLease(
            enter = { _, _, _ -> 0 },
            leave = { zeroTokenLeave++; 0 },
            method = 0,
            path = "/api/health",
            onLeaveError = { _, _ -> },
            onFailure = { status, message -> throw LeaseFailure(status, message) },
        ) { "zero" }
        check(zeroBody == "zero" && zeroTokenLeave == 0)

        // ---- handler throws: lease still released ----
        var leaveOnThrow = 0
        try {
            withRouteLease(
                enter = { _, _, out -> out[0] = 7L; 0 },
                leave = { leaveOnThrow++; 0 },
                method = 1,
                path = "/api/feature/autosell/config",
                onLeaveError = { _, _ -> },
                onFailure = { status, message -> throw LeaseFailure(status, message) },
            ) { throw IllegalStateException("handler") }
            error("expected handler exception")
        } catch (e: IllegalStateException) {
            check(e.message == "handler")
        }
        check(leaveOnThrow == 1)

        // ---- leave error is reported, not masked ----
        var observedLeaveCode = 0
        val okBody = withRouteLease(
            enter = { _, _, out -> out[0] = 5L; 0 },
            leave = { 3 },
            method = 1,
            path = "/api/debug/settings-ui/status",
            onLeaveError = { _, code -> observedLeaveCode = code },
            onFailure = { status, message -> throw LeaseFailure(status, message) },
        ) { "ok" }
        check(okBody == "ok" && observedLeaveCode == 3)
    }

    private class LeaseFailure(val status: Int, val message: String) : RuntimeException(message)
}
