package com.inotia4.qol

import android.app.Activity

/** Reads the foreground Activity when a modal is implemented outside libgame.so. */
object UiActivityTracker {
    data class CheckResult(val blockingActivityName: String?, val failed: Boolean)

    const val AGREEMENT_ACTIVITY =
        "com.com2us.module.activeuser.useragree.AgreementUIActivity"

    fun check(): CheckResult {
        return try {
            CheckResult(agreementActivity()?.let { AGREEMENT_ACTIVITY }, false)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.UI, "foreground activity lookup failed", t)
            CheckResult(null, true)
        }
    }

    /** 同意页前台时的存活实例，无则 null；ActivityThread 内部结构不可用时抛异常，
     *  让 [check] 能区分「未检测到」与「检测失败」（fail-closed 依据）。 */
    fun agreementActivity(): Activity? {
        val threadClass = Class.forName("android.app.ActivityThread")
        val thread = threadClass.getMethod("currentActivityThread").invoke(null)
            ?: throw IllegalStateException("ActivityThread not available")
        val activitiesField = threadClass.getDeclaredField("mActivities")
        activitiesField.isAccessible = true
        val activities = activitiesField.get(thread) as? Map<*, *>
            ?: throw IllegalStateException("ActivityThread.mActivities not available")
        return activities.values.asSequence()
            .mapNotNull { record ->
                if (record == null) return@mapNotNull null
                val pausedField = record.javaClass.getDeclaredField("paused")
                pausedField.isAccessible = true
                val activityField = record.javaClass.getDeclaredField("activity")
                activityField.isAccessible = true
                val activity = activityField.get(record) as? Activity
                if (activity != null && !pausedField.getBoolean(record)) activity else null
            }
            .firstOrNull { it.javaClass.name == AGREEMENT_ACTIVITY }
    }
}
