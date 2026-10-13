package com.inotia4.qol

/** Standalone pure-state checks for event-driven startup and observer registration retries. */
object RuntimeObserverRetryStateTest {
    @JvmStatic
    fun main(args: Array<String>) {
        rejectedApiSubscriptionCanRetryOnActivityEvent()
        validApiTokenSurvivesReplayTimeout()
        rejectedHealthSubscriptionCanRetryOnRealEvent()
        startupProgressCompletesOnlyAfterRuntimeReady()
        coalescedEventsDoNotCreateAnUnboundedRetry()
        healthSnapshotAndPresentationRevisionsAreStable()
    }

    private fun rejectedApiSubscriptionCanRetryOnActivityEvent() {
        val token = RetryableObserverToken()
        var attempts = 0

        check(!token.ensure { attempts++; 0L })
        check(!token.isEstablished())
        check(token.ensure { attempts++; 21L })
        check(token.currentId() == 21L)
        check(token.ensure { attempts++; 22L })
        check(attempts == 2)
    }

    private fun validApiTokenSurvivesReplayTimeout() {
        val token = RetryableObserverToken()
        var registrations = 0

        check(token.ensure { registrations++; 35L })
        check(
            runtimeEffectBootstrapState(
                apiSubscriptionEstablished = token.isEstablished(),
                initialSnapshotReceived = false,
            ) == RuntimeEffectBootstrapState.INITIAL_SNAPSHOT_PENDING,
        )

        check(token.ensure { registrations++; 36L })
        check(registrations == 1)
        check(
            runtimeEffectBootstrapState(
                apiSubscriptionEstablished = token.isEstablished(),
                initialSnapshotReceived = true,
            ) == RuntimeEffectBootstrapState.READY,
        )
        check(
            runtimeEffectBootstrapState(
                apiSubscriptionEstablished = false,
                initialSnapshotReceived = false,
            ) == RuntimeEffectBootstrapState.SUBSCRIPTION_NOT_ESTABLISHED,
        )
    }

    private fun rejectedHealthSubscriptionCanRetryOnRealEvent() {
        val healthObserver = RetryableObserverToken()
        var registrations = 0

        check(!healthObserver.ensure { registrations++; 0L })
        check(!healthObserver.isEstablished())
        check(healthObserver.ensure { registrations++; 47L })
        check(healthObserver.currentId() == 47L)
        check(healthObserver.ensure { registrations++; 48L })
        check(registrations == 2)
    }

    private fun startupProgressCompletesOnlyAfterRuntimeReady() {
        val progress = ActivityStartupProgress()
        var moduleRuntimeInitializations = 0

        check(progress.requestActivityEvent())
        progress.beginAttempt()
        if (progress.needsModuleRuntimeInitialization) {
            moduleRuntimeInitializations++
            progress.markModuleRuntimeInitialized()
        }
        check(!progress.finishAttempt(runtimeReady = false))

        check(progress.requestActivityEvent())
        progress.beginAttempt()
        if (progress.needsModuleRuntimeInitialization) {
            moduleRuntimeInitializations++
            progress.markModuleRuntimeInitialized()
        }
        check(moduleRuntimeInitializations == 1)
        check(!progress.finishAttempt(runtimeReady = true))
        check(progress.isCompleted)
        check(!progress.requestActivityEvent())
    }

    private fun coalescedEventsDoNotCreateAnUnboundedRetry() {
        val progress = ActivityStartupProgress()

        check(progress.requestActivityEvent())
        progress.beginAttempt()
        check(!progress.requestActivityEvent())
        check(progress.finishAttempt(runtimeReady = false))

        progress.beginAttempt()
        check(!progress.finishAttempt(runtimeReady = false))
        check(progress.requestActivityEvent())
    }

    private fun healthSnapshotAndPresentationRevisionsAreStable() {
        val snapshots = ModuleHealthSnapshotState()
        check(snapshots.accept(ModuleHealthSnapshot(4L, 1L, 1L, mainMenuSeen = true)))
        check(!snapshots.accept(ModuleHealthSnapshot(4L, 0L, 0L, mainMenuSeen = false)))
        check(snapshots.accept(ModuleHealthSnapshot(5L, 2L, 0L, mainMenuSeen = false)))
        check(snapshots.mainMenuSeenSticky)
        check(snapshots.snapshot?.knownMask == 2L)

        val sequence = ModuleHealthPresentationSequence()
        val first = sequence.next(
            ModuleHealthPresentation(listOf("Native", "Native", ""), visible = true),
        ) ?: error("first presentation must allocate a revision")
        check(first.revision == 1L)
        check(first.presentation.unavailableNames == listOf("Native"))

        var immutableNames = false
        try {
            (first.presentation.unavailableNames as MutableList<String>).add("帧钩子")
        } catch (_: UnsupportedOperationException) {
            immutableNames = true
        }
        check(immutableNames)
        check(
            sequence.next(ModuleHealthPresentation(listOf("Native"), visible = true)) == null,
        )

        val second = sequence.next(
            ModuleHealthPresentation(listOf("Native", "帧钩子"), visible = true),
        ) ?: error("changed presentation must allocate a new revision")
        check(second.revision == 2L)
    }
}
