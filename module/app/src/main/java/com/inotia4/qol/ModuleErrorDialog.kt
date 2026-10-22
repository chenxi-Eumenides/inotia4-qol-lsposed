package com.inotia4.qol

import android.app.Activity
import android.app.AlertDialog
import android.os.Handler
import android.os.Looper
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.ScrollView
import android.widget.TextView
import java.lang.ref.WeakReference
import java.util.ArrayDeque

/** Shows queued module errors in a native, scrollable dialog on the supplied Activity. */
object ModuleErrorDialog {
    private const val MAX_PENDING_ERRORS = 8

    private val mainHandler = Handler(Looper.getMainLooper())
    private val lock = Any()
    private val pendingErrors = BoundedModuleErrorQueue(MAX_PENDING_ERRORS)
    private var activityReference = WeakReference<Activity>(null)
    private var drainPosted = false
    private var dialogShowing = false

    /** Retains only a weak Activity reference and displays errors queued before this callback. */
    @JvmStatic
    fun onActivityAvailable(activity: Activity) {
        try {
            synchronized(lock) { activityReference = WeakReference(activity) }
            requestDrain()
        } catch (_: Throwable) {
            // UI dispatch must not interrupt the host Activity lifecycle.
        }
    }

    /** Adds an error to the bounded queue and schedules it when an Activity is available. */
    @JvmStatic
    fun enqueue(errorDetails: String) {
        try {
            pendingErrors.add(errorDetails)
            requestDrain()
        } catch (_: Throwable) {
            // Error presentation must never replace the original logging behavior.
        }
    }

    /** Preserves the direct-call entry point while using the same serialized queue. */
    @JvmStatic
    fun show(activity: Activity, errorDetails: String) {
        onActivityAvailable(activity)
        enqueue(errorDetails)
    }

    private fun requestDrain() {
        val shouldPost = synchronized(lock) {
            if (drainPosted || dialogShowing || pendingErrors.isEmpty() || activityReference.get() == null) {
                false
            } else {
                drainPosted = true
                true
            }
        }
        if (!shouldPost) return

        val posted = try {
            mainHandler.post(::showNext)
        } catch (_: Throwable) {
            false
        }
        if (!posted) synchronized(lock) { drainPosted = false }
    }

    private fun showNext() {
        val next = try {
            synchronized(lock) {
                drainPosted = false
                if (dialogShowing) return
                val activity = activityReference.get() ?: return
                if (!isUsable(activity)) return
                val errorDetails = pendingErrors.poll() ?: return
                dialogShowing = true
                activity to errorDetails
            }
        } catch (t: Throwable) {
            val activity = synchronized(lock) { activityReference.get() }
            reportShowFailure(activity, t)
            return
        }

        val (activity, errorDetails) = next
        try {
            val message = TextView(activity).apply {
                text = errorDetails
                textSize = 14f
                setTextIsSelectable(true)
                setLineSpacing(0f, 1.15f)
                val inset = (20 * resources.displayMetrics.density).toInt()
                setPadding(inset, inset, inset, inset)
            }
            val scrollView = ScrollView(activity).apply {
                isFillViewport = false
                addView(
                    message,
                    ViewGroup.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT,
                    ),
                )
            }
            val dialog = AlertDialog.Builder(activity)
                .setTitle("模块错误")
                .setView(scrollView)
                .setPositiveButton("关闭", null)
                .setCancelable(true)
                .create()
            dialog.setOnDismissListener {
                synchronized(lock) { dialogShowing = false }
                requestDrain()
            }
            dialog.show()
        } catch (t: Throwable) {
            reportShowFailure(activity, t)
        }
    }

    private fun reportShowFailure(activity: Activity?, throwable: Throwable) {
        synchronized(lock) {
            dialogShowing = false
            if (activity != null && activityReference.get() === activity) {
                activityReference = WeakReference(null)
            }
        }
        val reason = when (throwable) {
            is WindowManager.BadTokenException -> "Activity window was detached"
            is IllegalStateException -> "Activity no longer accepts window changes"
            else -> "dialog creation failed (${throwable.javaClass.simpleName})"
        }
        try {
            LogFile.warn(LogDomain.UI, "module error dialog skipped because $reason")
        } catch (_: Throwable) {
            // Reporting a dialog failure must not create another error notification.
        }
    }

    private fun isUsable(activity: Activity): Boolean =
        !activity.isFinishing && !activity.isDestroyed
}

internal class BoundedModuleErrorQueue(private val capacity: Int) {
    private val errors = ArrayDeque<String>()

    init {
        require(capacity > 0)
    }

    @Synchronized
    fun add(errorDetails: String) {
        if (errors.size == capacity) errors.removeFirst()
        errors.addLast(errorDetails)
    }

    @Synchronized
    fun poll(): String? = if (errors.isEmpty()) null else errors.removeFirst()

    @Synchronized
    fun isEmpty(): Boolean = errors.isEmpty()
}
