# Shared application quality checks

`check_project.py` is a Python 3.8-compatible tool, independent of CLI rendering
and firmware dependencies. Generated applications reference this implementation
from their pre-commit configuration. Keep common rules here; templates contain
only configuration defaults and project-specific extension settings.

The current working directory identifies the application, never the checker's
installation directory. For direct use from another directory:

```sh
python check_project.py components --project /path/to/application
python check_project.py cppcheck --project /path/to/application --compile-commands /path/to/compile_commands.json
```

The project must have its own Git root. Policy is read from its
`tools/quality/policy.json`. The tool does not import the consuming workspace or
CLI configuration. Generated-source commands run from the application root.
Cppcheck requires the executable on PATH; the generated manual hook provides
its pinned dependency in an isolated environment.

The utils revision selected by the workspace owns the checker's version. Updating
that dependency updates shared checks without copying scripts to each project.
For reproducible checks, use the same reviewed utils revision locally and in CI.
Relative hook paths survive moving the workspace as a whole; when moving only an
application, update its hook entries to the new dependency location.
