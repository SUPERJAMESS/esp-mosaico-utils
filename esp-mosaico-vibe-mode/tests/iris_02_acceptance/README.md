# Supplemental 0.2 hardware acceptance

Install `tests/firmware/iris_acceptance` on the S31 through the consuming
workspace's `mosaico.py`. The optional H2 and C2 devices must already run the
Iris `services_h2` / `services_c2` fixtures. Retain their owning Gateway with
`mosaico.py iris run` and claim the devices through the same project session.
Run with the prepared Iris host Python, which provides `aiohttp`:

```sh
python run.py --gateway http://127.0.0.1:PORT --device S31_DEVICE_ID \
  --control-device H2_DEVICE_ID --control-device C2_DEVICE_ID \
  --output /absolute/path/to/new-evidence-directory
```

The script never opens a device port or writes firmware. It checks control
responsiveness during a two-second RPC, bounded oversized responses,
concurrent operation deduplication, parameter conflicts, and replay of a timed
out operation without repeating its side effect. A device deadline does not
cancel an already running application callback; its failed operation is still
retained and deduplicated.

With two control devices it also checks simultaneous, individually identified
RPC payloads on all three links, control-only screenshots, bulk-stream
rejection, and hardware reset of each H2/C2 device. A device-filtered Follow
subscription must receive both ROM and second-stage bootloader logs, and the
other devices must keep their Boot IDs. These resets are intentional test
actions. JTAG debug is not exercised.

`result.json` retains per-case outcomes, identities and timings. `http.jsonl`,
raw boot logs, Follow events and PNG files retain the supporting evidence.
Use a fresh output directory; failed evidence is never overwritten.

The independent `firmware/iris_internal_budget/run_test.py` covers full-size
screenshots and mirror/RPC load. Workbench hardware interaction is in Iris's
`tools/frontend/tests/hardware-console-02.spec.ts`; explicitly provide its
Gateway URL, Device ID and a read-only RPC service/method in the environment.
Its installed firmware must provide a registered screen backend (for example,
Vibe Mode); the RPC-only `iris_acceptance` fixture does not. Do not run those
workloads concurrently on the same screen owner.
