package com.inotia4.qol

/** Standalone pure checks for module-health visibility and unknown-fact handling. */
object ModuleHealthMonitorDecisionTest {
    @JvmStatic
    fun main(args: Array<String>) {
        val empty = ModuleHealthDecision.decide(
            snapshot = null,
            nativeLibraryStateKnown = true,
            nativeLibraryLoaded = true,
            mainMenuSeenSticky = false,
            activityVisible = true,
        )
        check(empty.unavailableNames.isEmpty() && !empty.visible)

        val libraryNotYetObserved = ModuleHealthDecision.decide(
            snapshot = null,
            nativeLibraryStateKnown = false,
            nativeLibraryLoaded = false,
            mainMenuSeenSticky = false,
            activityVisible = true,
        )
        check(libraryNotYetObserved.unavailableNames.isEmpty() && !libraryNotYetObserved.visible)

        val initializationNotAttempted = ModuleHealthDecision.decide(
            snapshot = null,
            nativeLibraryStateKnown = true,
            nativeLibraryLoaded = true,
            mainMenuSeenSticky = false,
            activityVisible = true,
        )
        check(initializationNotAttempted.unavailableNames.isEmpty() && !initializationNotAttempted.visible)

        val libgameLoadPending = ModuleHealthDecision.decide(
            snapshot = ModuleHealthSnapshot(
                revision = 1L,
                knownMask = 0L,
                failedMask = 0L,
                mainMenuSeen = false,
            ),
            nativeLibraryStateKnown = true,
            nativeLibraryLoaded = true,
            mainMenuSeenSticky = false,
            activityVisible = true,
        )
        check(libgameLoadPending.unavailableNames.isEmpty() && !libgameLoadPending.visible)

        val providerAbsent = ModuleHealthDecision.decide(
            snapshot = ModuleHealthSnapshot(0L, 0L, 0L, false),
            nativeLibraryStateKnown = true,
            nativeLibraryLoaded = true,
            mainMenuSeenSticky = false,
            activityVisible = true,
        )
        check(providerAbsent.unavailableNames.isEmpty() && !providerAbsent.visible)

        val nativeUnavailable = ModuleHealthDecision.decide(
            snapshot = null,
            nativeLibraryStateKnown = true,
            nativeLibraryLoaded = false,
            mainMenuSeenSticky = false,
            activityVisible = true,
        )
        check(nativeUnavailable.unavailableNames == listOf("Native") && nativeUnavailable.visible)

        val nativeUnavailableBeforeFocus = ModuleHealthDecision.decide(
            snapshot = null,
            nativeLibraryStateKnown = true,
            nativeLibraryLoaded = false,
            mainMenuSeenSticky = false,
            activityVisible = false,
        )
        check(nativeUnavailableBeforeFocus.unavailableNames == listOf("Native"))
        check(!nativeUnavailableBeforeFocus.visible)

        val hostReportedNativeFailure = ModuleHealthDecision.decide(
            snapshot = ModuleHealthSnapshot(
                revision = 2L,
                knownMask = ModuleHealthDecision.NATIVE_BIT,
                failedMask = ModuleHealthDecision.NATIVE_BIT,
                mainMenuSeen = false,
            ),
            nativeLibraryStateKnown = true,
            nativeLibraryLoaded = true,
            mainMenuSeenSticky = false,
            activityVisible = true,
        )
        check(hostReportedNativeFailure.unavailableNames == listOf("Native"))

        val frameFailure = ModuleHealthDecision.decide(
            snapshot = ModuleHealthSnapshot(
                revision = 1L,
                knownMask = ModuleHealthDecision.FRAME_SOURCE_BIT,
                failedMask = ModuleHealthDecision.FRAME_SOURCE_BIT,
                mainMenuSeen = false,
            ),
            nativeLibraryStateKnown = true,
            nativeLibraryLoaded = true,
            mainMenuSeenSticky = false,
            activityVisible = true,
        )
        check(frameFailure.unavailableNames == listOf("帧钩子") && frameFailure.visible)

        val gatedHostFailures = ModuleHealthDecision.decide(
            snapshot = ModuleHealthSnapshot(
                revision = 2L,
                knownMask = ModuleHealthDecision.ASSEMBLY_BIT or ModuleHealthDecision.CONFIG_STORE_BIT,
                failedMask = ModuleHealthDecision.ASSEMBLY_BIT or ModuleHealthDecision.CONFIG_STORE_BIT,
                mainMenuSeen = false,
            ),
            nativeLibraryStateKnown = true,
            nativeLibraryLoaded = true,
            mainMenuSeenSticky = false,
            activityVisible = true,
        )
        check(gatedHostFailures.unavailableNames.isEmpty() && !gatedHostFailures.visible)

        val stickyHostFailures = ModuleHealthDecision.decide(
            snapshot = ModuleHealthSnapshot(
                revision = 3L,
                knownMask = ModuleHealthDecision.ASSEMBLY_BIT or ModuleHealthDecision.CONFIG_STORE_BIT,
                failedMask = ModuleHealthDecision.ASSEMBLY_BIT or ModuleHealthDecision.CONFIG_STORE_BIT,
                mainMenuSeen = false,
            ),
            nativeLibraryStateKnown = true,
            nativeLibraryLoaded = true,
            mainMenuSeenSticky = true,
            activityVisible = true,
        )
        check(stickyHostFailures.unavailableNames == listOf("模块初始化", "配置存储"))
        check(stickyHostFailures.visible)

        val hiddenActivity = ModuleHealthDecision.decide(
            snapshot = ModuleHealthSnapshot(
                revision = 4L,
                knownMask = ModuleHealthDecision.NATIVE_BIT,
                failedMask = ModuleHealthDecision.NATIVE_BIT,
                mainMenuSeen = false,
            ),
            nativeLibraryStateKnown = true,
            nativeLibraryLoaded = true,
            mainMenuSeenSticky = false,
            activityVisible = false,
        )
        check(hiddenActivity.unavailableNames == listOf("Native") && !hiddenActivity.visible)
    }
}
