"""Application GSP version policy delegates to shared acquisition."""
import importlib.util
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("gsp_tools_download", ROOT / "tools/gsp-sim/fetch_gspc.py")
fetch = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fetch)


def test_upgrade_bootstrap_ignores_old_managed_component():
    # Reconfigure resolves GSPC before the component manager installs the new
    # runtime. The upgrade command must not select that old runtime's compiler.
    for simulator, version in ((False, fetch.PINNED_GSPC_VERSION),
                               (True, fetch.PINNED_GSP_VERSION)):
        argv = ['fetch_gspc.py', '--pinned'] + (['--sim'] if simulator else [])
        with patch.object(fetch.sys, 'argv', argv), \
                patch.object(fetch, 'resolve_release', return_value=Path('/tmp/tool')) as release, \
                patch.object(fetch, 'resolve_gsp_root', side_effect=AssertionError('stale component consulted')):
            assert fetch.main() == 0
        assert release.call_args.kwargs['version'] == version
        assert release.call_args.kwargs['product'] == ('sim' if simulator else 'gspc')


def test_explicit_tool_pin_overrides_application_and_managed_component():
    for simulator, version in ((False, '0.5.0'), (True, '1.4.0')):
        argv = ['fetch_gspc.py', '--tool-version', version] + (['--sim'] if simulator else [])
        with patch.object(fetch.sys, 'argv', argv), \
                patch.object(fetch, 'resolve_release', return_value=Path('/tmp/tool')) as release, \
                patch.object(fetch, 'resolve_gsp_root', side_effect=AssertionError('component consulted')):
            assert fetch.main() == 0
        assert release.call_args.kwargs['version'] == version
        assert release.call_args.kwargs['product'] == ('sim' if simulator else 'gspc')
