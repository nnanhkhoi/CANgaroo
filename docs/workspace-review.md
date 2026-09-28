# Focused workspace review

Static review of CAN/UDS usability and reliability, 2026-09-27. These are remaining issues supported by the code paths below. Adapter timing and hot-plug behavior have not been tested on hardware. P1 means configuration loss; P2 means incorrect behavior under the stated conditions.

## P1: Opening Setup can discard configuration before confirmation

Trigger: save a setup, change the number of connected adapters, then open Setup and press Cancel. A custom setup whose network count differs from the discovered adapter count has the same problem.

[`MainWindow::showSetupDialog()`](../src/mainwindow.cpp#L1338) first copies the current setup, then calls `setDefaultSetup()` on the live backend. If the old and discovered network counts differ, it replaces the copy with defaults too. Cancel does not restore the original. DBC assignments, network names and interface settings can therefore be lost just by opening the dialog.

Scoped fix: discover devices without replacing the live setup; keep unavailable interfaces and their settings in the dialog's working copy. Apply that copy only on Accept. Verify both Accept and Cancel with a missing adapter and a custom network count.

## P2: SLCAN transmit waits behind a 100 ms receive timeout

Trigger: send a request on an otherwise quiet SLCAN bus while the listener is waiting for input. Short periodic intervals can accumulate multiple frames during the same wait.

[`SLCANInterface::sendMessage()`](../src/driver/SLCANDriver/SLCANInterface.cpp#L416) only appends to a queue. [`readMessage()`](../src/driver/SLCANDriver/SLCANInterface.cpp#L512) drains that queue before waiting for serial input, using the [100 ms timeout supplied by `BusListener`](../src/driver/BusListener.cpp#L71). Enqueueing TX does not wake that wait. This permits roughly 100 ms of extra request latency and batches short-period transmissions.

Scoped fix: wake the serial worker when TX arrives, keeping serial I/O in its owning thread. A shorter bounded receive wait is a smaller interim option with a CPU/wakeup tradeoff. Verify idle-bus send latency and cyclic spacing with a controlled serial fixture or adapter before claiming timing guarantees.

## P2: DBC lookup mixes networks and standard/extended identifiers

Trigger: load DBCs for two networks that reuse a CAN ID, or define standard and extended messages with the same numeric ID.

[`MeasurementSetup::rebuildMessageCache()`](../src/core/MeasurementSetup.cpp#L109) combines all networks into one map and strips the extended-ID flag. Later entries overwrite earlier entries with the same numeric ID. [`findDbMessage()`](../src/core/MeasurementSetup.cpp#L147) looks up only that ID, ignoring the incoming interface and frame format. Trace names, decoded signals and database associations can come from the wrong message.

Scoped fix: key CAN database lookup by network/interface, extended flag and numeric ID; define how duplicate definitions within one network are resolved. Add regression cases with different signal layouts on identical IDs across networks and across standard/extended formats.
