# Template ownership

Application templates contain the initial source and build configuration for an
application. `hello_world/` and `blank_game/` retain their public paths; custom
templates can live in the consuming workspace. Each declares its files in a
schema v1 `mosaico-template.json`.

`common/git_quality/` is a shared configuration profile. It contains Git and
editor settings, formatter and hook configuration, project policy and setup
instructions. Its schema v1 descriptor lists the files to supplement after
rendering an application template. Existing application files take precedence.
The descriptor itself is not copied into applications.

Executable tools live outside templates:

- `../tools/quality/check_project.py` implements the shared quality checks. Hook
  entries refer to it in the workspace's utils checkout; applications do not
  receive private copies. Pin that checkout's revision with workspace dependencies.
- `../tools/mosaico_cli/template.py` only validates and renders schema v1 data.
- `../tools/mosaico_cli/project_defaults.py` renders the shared profile and checks
  merge conflicts. `project_git.py` owns isolated Git initialization.
- `../tools/mosaico_cli/scaffold.py` coordinates creation and exclusive writes;
  `project_setup.py` supplies setup results and presentation.

Shared rule changes belong in the checker and `../tests/test_quality_checks.py`.
Creation and real hook integration are covered by
`../tests/test_project_management.py`. Application-specific checkers stay with the
application's generator, referenced by its `tools/quality/policy.json`.
