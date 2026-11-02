package com.inotia4.qol

/** Standalone checks for bounded error presentation and the dialog detail format. */
object ModuleErrorDialogQueueTest {
    @JvmStatic
    fun main(args: Array<String>) {
        boundedQueueDropsOldestErrors()
        queueAcceptsConcurrentErrorsWithinItsBound()
        dialogDetailsIncludeDomainMessageAndThrowableSummary()
    }

    private fun boundedQueueDropsOldestErrors() {
        val queue = BoundedModuleErrorQueue(capacity = 2)
        queue.add("first")
        queue.add("second")
        queue.add("third")

        check(queue.poll() == "second")
        check(queue.poll() == "third")
        check(queue.poll() == null)
    }

    private fun queueAcceptsConcurrentErrorsWithinItsBound() {
        val queue = BoundedModuleErrorQueue(capacity = 16)
        val workers = (0 until 4).map { worker ->
            Thread {
                repeat(100) { sequence -> queue.add("$worker:$sequence") }
            }.apply { start() }
        }
        workers.forEach { it.join() }

        val retained = buildList {
            while (true) add(queue.poll() ?: break)
        }
        check(retained.size == 16)
        check(retained.all { it.matches(Regex("\\d+:\\d+")) })
    }

    private fun dialogDetailsIncludeDomainMessageAndThrowableSummary() {
        val stack = "java.lang.IllegalStateException: broken @ Worker.run(Worker.kt:12)"
        check(
            formatModuleErrorDialogDetails(LogDomain.HTTP, "request failed", stack) ==
                "domain=http\nmessage=request failed\nthrowable=$stack",
        )
        check(
            formatModuleErrorDialogDetails(LogDomain.UI, "view unavailable", null) ==
                "domain=ui\nmessage=view unavailable",
        )
    }
}
