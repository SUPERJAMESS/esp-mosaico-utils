# Workspace project template descriptions

`mosaico.py project init NAME` reads the JSON file selected by
`workspace.init_template` in the consuming workspace's `.mosaico.json`.
Relative configuration paths resolve from the workspace root. The setting has
no default: a workspace without it can use other commands, but `project init` reports a
configuration error. A directory is not a template description.

The workspace owns the application sources **and** the description. The tools
own the format, rendering, validation and exclusive output creation. The renderer
has no predefined application file list, name or source language. After rendering,
the creator supplements missing files from its shared Git/quality template and
initializes an independent Git repository on `main`. Editing application template
sources and rules still requires no tool change.

## Description format (schema version 1)

For example, a workspace can keep `templates/basic/description.json` beside
`entry.txt` and `payload.dat`:

```json
{
  "schema_version": 1,
  "paths": {
    "common": {
      "base": "workspace",
      "path": "shared/library",
      "required": "directory"
    }
  },
  "files": [
    {
      "source": "entry.txt",
      "destination": "source/entry.txt",
      "replacements": [
        {"pattern": "^App: original$", "replacement": "App: {{project_name}}"},
        {"pattern": "^Link: original$", "replacement": "Link: {{common|json}}"}
      ],
      "append": "\nProject: {{project_path}}\n"
    },
    {"source": "payload.dat", "destination": "assets/payload.dat"}
  ]
}
```

`schema_version` and a nonempty `files` array are required. `paths` is optional.
Unsupported fields and versions are rejected before any files are written.

Each file entry has:

| Field | Meaning |
| --- | --- |
| `source` | Required file path relative to the description's directory. |
| `destination` | File path relative to the generated project; defaults to `source`. |
| `replacements` | Optional ordered array of text replacement rules. |
| `append` | Optional text appended after all replacements. |

Source and destination paths use `/` separators. Absolute paths, traversal,
Windows reserved names/characters, source symlinks, sources outside the template
directory, duplicate destinations (including case-only differences) and
file/directory conflicts are rejected. `.git` metadata is forbidden in generated
paths. Only listed application files are copied. Without
text rules or `append`, a file is copied byte for byte, including binary files.

Each replacement requires a Python regular expression `pattern` and a literal
`replacement` string. Matching uses `MULTILINE`; transformed sources must be UTF-8
and CRLF is normalized to LF. `count` is an optional positive integer, default 1;
the actual number of matches must equal it. Missing or ambiguous matches fail
before output creation. Replacement text supports the variables below, but does
not expand regex backreferences or execute code. Source text itself is not
interpolated. `append` supports the same variables.

## Variables and resource paths

`{{project_name}}` is the validated application name. `{{project_path}}` is the
generated project's path relative to the workspace root, with `/` separators.

Each key in `paths` declares another variable. Names use letters, digits and
underscores, cannot start with a digit, and cannot replace a built-in variable.
Every entry requires `base` and `path`. Supported bases are:

| Base | Anchor |
| --- | --- |
| `workspace` | Consuming workspace root. |
| `template` | Directory containing this description. |
| `bsp` | Workspace-configured `dependencies.bsp`. |
| `esp_iris` | Workspace-configured `dependencies.esp_iris`. |

`path` is relative to that anchor; `.` selects the anchor itself. When a path
variable is used, its value is computed relative to the **destination file's
parent directory**, so a template may move files or use nested project layouts.
On Windows, the destination and referenced resources must share a drive.

The optional `required` field is `file` or `directory`. It checks for that
resource during preflight. Omit it for dependencies that may be downloaded or
initialized later; the tool never does this automatically during `project init`.

Variables support `{{name|format}}`:

| Format | Result |
| --- | --- |
| `raw` (default) | Unquoted text. |
| `json` | A JSON string, also usable as a YAML double-quoted value. |
| `cmake` | Escape CMake string characters; surrounding quotes and variables such as `${CMAKE_CURRENT_LIST_DIR}` belong in the description. |
| `shell` | Quote a command argument for the host platform, as in CLI command hints. |

Unknown variables and formats are errors. Descriptions are data: they cannot
import Python, run commands or choose executable rendering hooks.

## Output and failure handling

The command validates and renders all entries before reserving the destination.
Existing targets are rejected, including empty directories and symlinks. A write
failure removes only files and directories created by this operation; unexpected
external content is retained and reported in `cleanup_errors`.

`--dry-run` performs the same preflight without creating a project or run-log
directory. JSON output identifies the description in `template` and returns the
actual destination file list in `files`. Device commands and the workspace's
`default_project` are unaffected.

Creation requires Git 2.28+. Missing Git fails before writing; a Git initialization
failure retains the new source directory and reports a recovery command. No files
are staged, no commit or remote is created, and hooks are not installed. Templates
may supply their own management files: exact filenames are preserved, not merged,
and reported in `quality.preserved_files`. Case-only or file/directory collisions
with the shared files fail. The JSON result includes `git.status` (`planned` or
`initialized`), `git.branch`, and the quality guide and setup commands. `files`
includes the supplemented management files, excluding `.git` internals. Shared
configuration comes from `mosaico-tools/templates/common/git_quality`, itself a
schema v1 profile. Generic quality checks live in `mosaico-tools/tools/quality`;
generated hooks reference that workspace dependency through relative paths.
Projects retain only their policy, configuration and guide. Keep the workspace's
utils revision pinned and available when running these hooks.
