"""Check the real title worker against visible rows in the production UI."""
import subprocess
import sys

import pytest

from test_ideas_worker_host import build_worker, COMPONENTS, FIXTURE
from test_recovery_ui_host import native_build  # noqa: F401


@pytest.fixture(scope="module")
def title_worker(tmp_path_factory, native_build):
    library = native_build[0]
    return build_worker(
        tmp_path_factory.mktemp("title-worker"),
        main=FIXTURE / "title_cache.c",
        includes=[COMPONENTS / name / "include" for name in ("app_ui", "app_device")]
                 + [COMPONENTS / "app_ui/private"],
        link=[library, "-Wl,-rpath," + str(library.parent)],
        extra_codes=range(42732, 42747),
    )


@pytest.mark.skipif(sys.platform != "linux", reason="SDK fixture uses Linux linker wrappers")
@pytest.mark.parametrize("count", [8, 9, 16])
def test_visible_titles_settle_and_hidden_list_does_not_block_open(title_worker, tmp_path, count):
    executable, releases = title_worker
    result = subprocess.run([str(executable), str(count), str(releases), str(tmp_path)],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
