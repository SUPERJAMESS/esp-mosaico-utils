# USB session regression tests

Run `python -m pytest tests/test_usb_shutdown.py` from the tools directory.
The harness compiles the production USB transport against a small SDK double,
with automatic ROM download both enabled and disabled, host wait times of 0 and
1500 ms, and tick rates of 100 and 1000 Hz.

On boards without VBUS sensing, unplugging may produce SUSPENDED without
DETACHED. Iris therefore ends the protocol session on suspend, including normal
host power management. Resume makes the transport available for a new handshake;
it does not continue the old protocol session. Reconfiguration while already
configured and CDC close/open also end the affected session.

The console retains startup logs until a host asserts DTR/RTS or sends input,
then allows the terminal's open sequence to settle. A subsequent low DTR is
accepted for stock IDF Monitor. Enumeration alone does not consume the log ring;
bus reset, detach and suspend clear the remembered host open.

Coverage includes:

- Suspend/resume or close/open entirely between worker polls.
- Reconfiguration without DETACHED and refreshing data-interface DTR.
- Discarding queued software RX/TX bytes before a new session.
- Invalidation during read, write or queue cleanup.
- Startup callbacks before CDC publication and the first native console command.
- Bounded startup waits, tick wrap, late host open and stock monitor line states.
- Independent control/data lifetimes, initialization rollback and shutdown.

The SDK double does not model USB DMA, physical disconnect detection, scheduling
on two cores, or host enumeration. Clearing software FIFOs cannot recall transfers
already submitted to the controller. These require hardware verification.
