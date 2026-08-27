package com.inotia4.export

import android.app.Activity

/** Reads the foreground Activity when a modal is implemented outside libgame.so. */
object UiActivityTracker {
    data class CheckResult(val blockingActivityName: String?, val failed: Boolean)

    private const val AGREEMENT_ACTIVITY =
        "com.com2us.module.activeuser.useragree.AgreementUIActivity"

    fun check(): CheckResult {
        return try {
            val threadClass = Class.forName("android.app.ActivityThread")
            val thread = threadClass.getMethod("currentActivityThread").invoke(null)
                ?: return CheckResult(null, true)
            val activitiesField = threadClass.getDeclaredField("mActivities")
            activitiesField.isAccessible = true
            val activities = activitiesField.get(thread) as? Map<*, *>
                ?: return CheckResult(null, true)
            activities.values.asSequence()
                .mapNotNull { record ->
                    if (record == null) return@mapNotNull null
                    val pausedField = record.javaClass.getDeclaredField("paused")
                    pausedField.isAccessible = true
                    val activityField = record.javaClass.getDeclaredField("activity")
                    activityField.isAccessible = true
                    val activity = activityField.get(record) as? Activity
                    if (activity != null && !pausedField.getBoolean(record)) activity else null
                }
                .map { it.javaClass.name }
                .firstOrNull { it == AGREEMENT_ACTIVITY }
                .let { CheckResult(it, false) }
        } catch (t: Throwable) {
            LogFile.logError("foreground activity lookup failed", t)
            CheckResult(null, true)
        }
    }
}
