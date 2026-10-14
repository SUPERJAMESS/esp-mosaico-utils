# ESP-Mosaico Tools

The [0.2 software specification (中文)](docs/mosaico-tools-software-spec-zh.md)
and [migration guide (中文)](docs/migration-0.2.md) describe the breaking release.
The source uses Gateway API 2 and the new test-partition product contract.
Default updates validate the matching reviewed 0.2.0 base bundle; see the
[0.2 validation record (中文)](../esp-mosaico-vibe-mode/docs/validation-0.2.md).
Old devices and old bundles require their separate 0.1 environment.

Workspace-consumed command-line tools for ESP-Mosaico development and device
operations. A firmware workspace pins the containing `esp-mosaico-utils`
repository; the CLI package does not need to be installed into the user's
Python environment.

The user-facing retained firmware is **Vibe Mode** (`firmware_mode=recovery`).
**ROM Download Mode** runs the chip download program without ESP-Iris.
`iris test enter-recovery` enters Vibe Mode; `recover` writes base firmware.
See the [firmware guide](../esp-mosaico-vibe-mode/firmware/vibe-mode/README.md).

Local Gateways are shared project services. Device commands start or reuse one
and retain a renewable client lease for their duration. Every
`python mosaico.py iris run --project <application>` retains an independent
foreground client, including when the Gateway already exists. Ctrl-C releases
only that client. The printed URL opens the Web workbench, whose event connection
also retains a client. With no clients and no active work, the Gateway exits after
**10 idle seconds**. The original launching command does not own its lifetime.
CLI clients renew every 5 seconds and expire after 20 seconds without renewal;
client expiry is separate from the subsequent idle countdown. There is no stop
command and the workbench cannot wake an exited Gateway.

`iris list` and `iris status` do not open unclaimed devices. Device operations can
omit their target: prefer the sole connected device, then the sole existing
ownership (wait for that identity if offline), then the sole available USB
device from live enumeration. Multiple candidates report a selection error.
Unconnected TCP/mDNS endpoints, cached history, ROM and Serial/JTAG interfaces
are not automatic USB candidates. Explicit targets never fall back to another
device, and reservations held by other sessions are never automatically reclaimed.

`iris run` attempts automatic connection once on creation, but stays running
without a device or when selection fails. Later device operations or `iris claim`
can initiate a connection. It does not automatically re-claim a released device.
In a shared session, `iris claim` can omit the target; `iris release` selects
the sole owned device, even offline. Multiple verified transports of one device
count as one. `iris takeover start` requires an explicit device ID or endpoint.
Use the original `--takeover-id` when retrying.

`recover` also attempts automatic ESP-Iris admission before ROM detection;
only an absent target allows the actual recovery to proceed to ROM selection.
An explicit `--hardware-mac` keeps the existing hardware identity workflow.
External `--gateway-profile` commands only select devices already connected
to that Gateway and never auto-acquire USB devices on the CLI host.

Reboots retain the current ownership; a new project session does not inherit old
connection history. Device handoff is initiated only by the receiving project
through `iris takeover start`. Shared same-user SQLite records and OS locks
coordinate ownership without a global service.
## Command structure

```text
mosaico.py
├── doctor
├── project init
├── iris
│   ├── run / status
│   ├── list / claim / release / reconcile
│   ├── takeover start / status / resume / abort / reconcile
│   ├── logs / memory / crash / rpc
│   ├── device-status / screenshot / operation-status
│   ├── app-update
│   ├── system-update
│   └── test enter-recovery / recovery-wifi / bridge-code
└── recover
```

`iris status` passively inspects the selected project, including clients,
keepalive reasons, and the idle countdown. `iris status --all` reads the shared
same-user registry and probes live local Gateways across workspaces, even if this
project has no Gateway. Neither query starts a Gateway, prepares a host runtime,
or retains a client. An absent Gateway returns
`{"running": false, "session": null}` with `--json`; orphaned ownership is
reported separately. `--all` and `--project` are mutually exclusive.

For live device evidence, use `iris device-status --json`; it verifies Device ID,
Boot ID and `stale=false` through the selected project's Gateway. Unlike host
`iris status`, this command can acquire the project's device and query it live.
Use `iris screenshot device.png --json` to save the device image directly, with
image metadata, operation ID and the verified device/boot identity. A reboot during
capture fails verification. Inspect the saved image itself rather than a screenshot
of the Web workbench. `iris operation-status <operation-id> --json` reads an existing
record without acquiring a device or replaying the operation; it may start/reuse the
project Gateway. A successful query preserves failed or unknown operation outcomes
in `operation.status`. Share the workbench URL for developer observation; browser
automation and CLI/Web comparisons are for workbench testing or explicit user requests.

Ownership mutations (`iris claim/release/reconcile` and takeover actions) can
start/join shared Gateways. Takeover status remains passive. A device claim is
not a keepalive: use `iris run` when its ownership must persist across commands.
Active takeover requests hold the receiver until validation completes. Interrupted
takeover reservations remain protected and require explicit
reconciliation; they are never silently released to another project.

Gateways must provide the 0.2 API and shared-lifetime capability. The 0.2
ownership database and private state are isolated from earlier installations;
stop old Gateways through their original tools before migrating. Remote
profiles keep their externally managed lifetimes.

`iris app-update` installs code-only changes with an identical device partition table.
Prefer `iris system-update` for a new application, layout change, or changed resources. It
applies the images selected by a validated update bundle, which can also target
Vibe Mode alone. `recover` restores the base firmware, including when ESP-Iris
is unreachable. `iris test` groups individual Vibe Mode test operations:

| Command | Preconditions and result |
| --- | --- |
| `enter-recovery` | A reachable application restarts into Vibe Mode; verify the same Device ID and a new Boot ID. If already in Vibe Mode, return its current status. |
| `recovery-wifi` | Requires Vibe Mode over USB; submit the SSID and password and wait for Wi-Fi connectivity. |
| `bridge-code` | Requires Vibe Mode over USB, configured Bridge service and network connectivity; open the device's download page and return the pairing code, validity and website URL. |

Only the public command paths above are accepted. Replace old script commands
using the [0.2 migration table](docs/migration-0.2.md); there are no compatibility
aliases. `iris logs` follows by default (`--snapshot` prints retained logs only);
`iris memory --follow` enables continuous memory sampling.

The consuming repository owns a `.mosaico.json` file with `schema_version: 2`.
All configured relative
paths are resolved from the directory containing that file. Vibe Mode firmware
source and its reviewed bundle live under `../esp-mosaico-vibe-mode/firmware/vibe-mode` and are resolved
from this checkout so the CLI and Vibe Mode implementation are versioned
together. ESP-Iris is included alongside this project in the workspace, making
the workspace the single source of its device-side Iris implementation.

From a consuming workspace, prefer its root launcher:

```sh
python3 mosaico.py doctor
python3 mosaico.py iris app-update --project projects/app
python3 mosaico.py iris system-update --project projects/app
python3 mosaico.py iris test enter-recovery
```

For direct source-tree testing, pass the consuming workspace explicitly:

```sh
python3 /path/to/esp-mosaico-utils/mosaico-tools/mosaico.py \
  --workspace /path/to/firmware-workspace doctor
```

`iris app-update` updates only the application OTA partition. `iris system-update` builds
and submits the workspace's atomic application, partition table, and declared resource
bundle by default; use `--skip-build` or `--bundle PATH` to reuse artifacts.
`iris test enter-recovery` asks a reachable normal application to boot the retained
Vibe Mode firmware without building or installing firmware. It waits for the same
Device ID to reconnect in Vibe Mode with a new Boot ID; use `--device-id` when
more than one device is connected and `--timeout` to change the 30-second
transition limit. Both numeric Boot IDs and exact `boot_id_text` fields are
included in JSON output so 64-bit identities remain lossless for JavaScript
consumers.

The CLI searches the current directory and its parents for `.mosaico.json`.
Use `--workspace PATH` to select another workspace explicitly.

Create a normal application with `python mosaico.py project init my_app`. The consuming
workspace supplies a JSON template description through `workspace.init_template`;
there is no implicit template. Sources, file lists, text rules and resource paths
are maintained by that workspace. The creator supplements missing Git and quality
configuration and initializes an independent Git repository on branch `main`.
The same setup applies to `game create/new`, including Engine examples. Existing
template configuration takes precedence; preserved filenames are reported.
Output goes under `workspace.projects_dir`;
`default_project` is unchanged. The generic renderer handles path variables,
validated replacements, exclusive creation and failure cleanup without assuming
an application layout. See [the template format](../esp-mosaico-vibe-mode/docs/project-template.md).

Names use 1–31 ASCII letters, digits or underscores, starting with a letter,
and cannot be Windows reserved names. Existing destinations are rejected.
Use `--dry-run` to validate and list files without writing, or `--json` for stable
output. Initialization requires Git 2.28+ but needs neither ESP-IDF nor a Gateway
or connected device. It does not stage files, create a commit, configure a remote
or install hooks. Enable the pinned checks from the new project directory using
the commands printed by creation; see its `tools/quality/README.md` for first-run
staging, formatting fixes and manual C/C++ analysis. The default checks cover
text, ESP-IDF AStyle formatting, Ruff, configuration syntax, secrets, artifacts,
file permissions and project component conventions. The Mosaico hooks call the
shared checker in this utils checkout through relative paths; pin the workspace's
utils revision alongside other dependencies. Only configuration and policy are
copied into the project. See [template ownership](templates/README.md) for the
separation between application templates, shared defaults and executable tools.

When multiple ESP32-S31 devices are already in ROM download mode, select the
target by its factory eFuse Base MAC. The CLI reads every registered ROM
endpoint without writing, repeats the MAC check immediately before flashing,
and verifies the same MAC in Vibe Mode after re-enumeration:

```sh
python mosaico.py recover --hardware-mac 30:ed:a0:12:34:56 --source current
```

After upgrading from an older ESP-Iris release, the live Device ID changes once
from the NVS-stored random value to the deterministic hardware-derived value.
The Gateway retains the old ID and its operations as offline history; refresh
saved `--device-id` values with `python mosaico.py iris list`. Upgrade Vibe Mode and
normal firmware together, since mixed versions use different identity schemes.
The pairing token and other retained NVS state are not erased.

Run the self-contained tool tests with:

```sh
python3 -m pytest -q ../esp-mosaico-vibe-mode/tests
```



See [component boundaries](../docs/component-boundaries.md) for API compatibility, source provenance and Recovery contracts.

Local System Update bundles are inspected before device admission; the CLI records
each image's offset, size, and hash. `app-update` layout mismatches return
`partition_layout_mismatch`, both table hashes, and a product system-update command.
The command never rewrites a project's layout or silently expands OTA write scope.

Built normal applications must declare the product role/board/layout/Recovery ABI
in effective `build/config/sdkconfig.json`, including when reusing artifacts.
Both update paths require Gateway capability `update-acceptance/v1` and validate
the final role and product contract after the healthy boot and image identity checks.
Device ID selection can verify multiple candidates; an explicit endpoint is strict,
and failed new probes release their endpoint/device reservations.

### ROM recovery operations

`mosaico.py recover` prepares its bundle before submitting a single local Gateway
operation for ROM flashing and Vibe Mode verification. ROM identity probes use
the same process-owned endpoint locks. The operation ID, raw executor output and
verification evidence are retained; there are no maintenance leases or renewals.
A wait timeout leaves the running writer protected. Inspect its operation ID
before retrying. A completed failed operation does not retain a permanent device
reservation. The CLI and Workbench expose offline, connecting, idle, busy and
needs-recovery states independently of project ownership and firmware mode.

To request an owned device from a newly started project Gateway:

```sh
python mosaico.py iris takeover start --project projects/my_app --endpoint /dev/ttyACM0
python mosaico.py iris takeover start --project projects/my_app --device-id <DEVICE_ID> --force --timeout 120
```

The default requires an idle device. `--force` stops admission of new work and
cancels queued operations, waits for active writes, then stops mirrors and
cooperatively cancels background Jobs. Timeout keeps the original owner and does
not kill writes. A log monitor or `iris run` client alone never blocks handoff.
Keep the reported takeover ID when querying or retrying an interrupted request:

```sh
python mosaico.py iris takeover status --project projects/my_app --takeover-id <ID>
python mosaico.py iris takeover resume --project projects/my_app --takeover-id <ID>
```

`resume` continues validation in the original receiving session. If no reservation
was created, retry `start` with the original ID. `abort` runs in the original
owning project; it never rolls back a completed takeover or reclaims an accepting
live receiver. `reconcile` is available to a participant project only after both
original sessions have exited and physical locks are free. All four record
commands require `--takeover-id`; `status` never starts a Gateway.

## Upload Iris applications to Mosaico Ideas

`project upload` creates or updates an application draft. It never submits a
review, withdraws pending review, publishes a version, connects a device or
starts the local Gateway. The current platform accepts **unsigned `.irisfw`**
for ESP32-S31 (chip ID 32), containing exactly one application plus optional
data/partition-table components. Vibe Mode/bootloader components and nonzero
flags are rejected. The server validates the bundle against its administrator's
current active Iris Header; applications do not select a Header.

Keep `mosaico-ideas.json`, README, covers and resources in the application directory:

```json
{
  "schema": "mosaico-ideas/project-upload/v1",
  "project": {
    "title": "Temperature sensor",
    "categories": ["sensors"],
    "boards": [],
    "tags": ["iris", "sensor"],
    "license": "MIT",
    "external_links": []
  },
  "content": {
    "readme": "README.md",
    "covers": ["assets/cover.png"],
    "resources": ["assets/wiring.png"],
    "attachments": ["attachments/schematic.pdf"]
  },
  "release": {"changelog": "Improve reconnect behavior"}
}
```

The machine-readable schema is [mosaico-ideas.schema.json](mosaico-ideas.schema.json).
The unpublished pre-release manifest name and schema identifier have been replaced;
use `mosaico-ideas.json` with `mosaico-ideas/project-upload/v1` for new and existing projects.
No application ID, Header ID or credential belongs in the manifest or bundle.
All file paths are application-relative; absolute paths, `..`, escaping symlinks
and duplicate declared files are rejected. Link README resources using Markdown
inline images/links or reference definitions; these files are discovered and
uploaded automatically, then destinations are replaced with platform asset URLs.
External HTTP(S) links and code blocks remain unchanged. `content.resources`
entries must be referenced from README; use `attachments` for standalone files.
Use `%20` or angle destinations for spaces and percent-encode parentheses in
filenames. Raw HTML resource links are not rewritten.

Limits: 9 covers, 20 attachments, 100 resource files/100 MiB total; images up to
10 MiB each, other resources 20 MiB, and the Iris archive 32 MiB. README must be
UTF-8, at most 100,000 characters and 400,000 bytes. The manifest is at most 64 KiB.

```sh
python mosaico.py account login --open-browser
python mosaico.py account status
python mosaico.py project upload --project projects/my_app
```

Login displays a verification page and a separate user code. Approve the code
in your browser; the CLI stores its token privately under
`state_root("esp-mosaico")/0.2/mosaico-ideas/` (POSIX directory/file modes 0700/0600).
Log in again for 0.2. Earlier credentials and resumable-upload records stay in
their original directories and are never imported or rewritten.
Native Windows account directory permissions apply. `account logout` revokes
the selected token. `MAKER_SPARK_TOKEN` takes precedence over saved credentials;
logout with that environment variable revokes it, without deleting a different
saved token. Tokens never appear in JSON or run logs.

The default server is `https://mosaico-ideas.espressif.com`. Server selection uses
the first non-empty value in this order: `--server`, `MOSAICO_IDEAS_SERVER`,
`MAKER_SPARK_SERVER` (legacy), then the default. Use `MOSAICO_IDEAS_SERVER` for
new configurations; `MAKER_SPARK_SERVER` remains supported for existing setups.
HTTPS is required except for localhost development. Supply only the origin
(no `/api/v1` or other path); the CLI adds API paths automatically.
The token environment variable remains `MAKER_SPARK_TOKEN`.

Interactive upload asks whether to create a new application or update an owned
one, showing the complete application ID, version and status. It asks for
confirmation again and warns when replacing an existing draft. An update is
never inferred from directory names or previous selections. CI/`--json` usage
requires `--create` or `--update APPLICATION_ID`, and `--yes` explicitly confirms
the operation, including replacement of an existing draft:

```sh
# Inject MAKER_SPARK_TOKEN through the CI secret store, not a command argument.
python mosaico.py project upload --project projects/my_app --create --yes --json
python mosaico.py project upload --project projects/my_app --skip-build --update APPLICATION_ID --yes --json
python mosaico.py project upload --project projects/my_app --bundle dist/app.irisfw --version 0.1.2 --update APPLICATION_ID --yes --json
```

By default the CLI runs the existing `system-update-bundle` build target.
`--skip-build` uses the existing build and its `project_description.json`;
`--bundle` inspects the specified archive without a build. Local build version,
bundle `release` and explicit `--version`, when present, must match exactly.
A supplied bundle without `release` requires `--version`. Versions are at most
80 characters, begin with a letter/digit, and use letters, digits, `.`, `_`, `+`
and `-`; no normalization occurs (bundle release itself is limited to 64 characters).

Success JSON includes `application_id`, `revision_id`, `version`, `bundle_sha256`,
`status: "draft"` and `draft_url`. Retry the same command after a lost response:
private resumable state preserves idempotency keys and completed assets. S3
precondition failures are verified through completion; expired sessions are
renewed. Local servers without S3 use the dedicated Iris multipart endpoint.
Network errors report a platform request ID where available, without signed URLs.
A version conflict requires refreshing the draft rather than overriding changes.

Run upload-specific unit tests with:

```sh
python3 -m unittest discover -s mosaico-tools/tests -v
```
