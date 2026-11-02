# `module_health`

`module_health` publishes the read-only `module.health` capability (v1.0). It has no plugin
dependencies, HTTP endpoints, hooks, or resources. While enabled, it samples the Host facts table
on a scheduler task every 30 logic frames. Its `module_health.enabled` feature is a Hidden toggle
enabled by default; it is not included in the player feature catalog.

The capability forwards the Host SDK v1.25 `module_health` facts table. A snapshot contains:

| Field | Meaning |
|---|---|
| `revision` | Monotonic revision for changes to the health snapshot |
| `known_mask` | Facts whose state has been observed |
| `failed_mask` | Known failed facts; always a subset of `known_mask` |
| `main_menu_seen` | Sticky flag set after a logic-frame sample observes `main_menu*` |

Mask bits are `Native` (bit 0), `Frame hooks` (bit 1), `Module initialization` (bit 2), and
`Config storage` (bit 3). A clear `known_mask` bit means unknown, not healthy or failed. An absent
`module_health` provider is read through the resident Host SDK facts table by the trusted
NativeBridge path; provider install failure alone does not set a failed bit.

Plugin failures, API availability, and OP permission are not module-health facts. Missing game
libraries and other retryable preconditions remain unknown until a relevant Host operation can
establish a result. Reads return the mutex-protected snapshot; they do not query plugin registries or
the config service. Host assembly publishes the durable config bootstrap result atomically.

The Host is the source of truth and publishes revisioned snapshots through its observer event path:
subscribing replays the current snapshot, and later callbacks are sent when a fact changes. Observer
callbacks run outside the snapshot mutex. Kotlin consumes these callbacks; there is no one-second
native health polling loop. `main_menu_seen` is set by a logic-frame observation and remains sticky.
If a health subscription is unavailable, unknown facts stay unknown rather than being inferred as
healthy or failed.
