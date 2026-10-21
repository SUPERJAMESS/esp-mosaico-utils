import base64

from iris_gateway.store import GatewayStore


def append_capture(store, capture):
    return store.append_event("console", {"kind": "console_raw", "capture_id": capture,
        "endpoint": "uart:test", "offset": 0, "data_base64": base64.b64encode(b"ROM").decode()})


def bind(store, capture):
    return store.append_event("console", {"kind": "console_binding", "capture_id": capture,
        "endpoint": "uart:test", "offset": 3, "boot_id": 42}, "device")


def test_current_capture_binding_avoids_scanning_historical_segments(tmp_path, monkeypatch):
    store = GatewayStore(tmp_path)
    capture = "a" * 32
    first = append_capture(store, capture)

    def history_query(**kwargs):
        raise AssertionError("current capture must not scan historical database files")

    monkeypatch.setattr(store.events.segments, "query", history_query)
    try:
        binding = bind(store, capture)
        assert binding["history_start_event_id"] == first["event_id"]
        assert binding["history_end_event_id"] == first["event_id"]
    finally:
        store.close()


def test_restarted_writer_falls_back_to_retained_capture_history(tmp_path):
    capture = "b" * 32
    store = GatewayStore(tmp_path)
    first = append_capture(store, capture)
    store.close()
    store = GatewayStore(tmp_path)
    try:
        assert bind(store, capture)["history_start_event_id"] == first["event_id"]
    finally:
        store.close()


def test_pruned_capture_does_not_reuse_cached_event_range(tmp_path):
    capture = "c" * 32
    store = GatewayStore(tmp_path)
    first = append_capture(store, capture)
    segment = store.db.execute("SELECT start_id FROM event_segments").fetchone()[0]
    # Model retention on another connection: the writer's in-memory cache
    # survives, while its first segment has been retired from the catalog.
    for path in store.events.segments.files(segment):
        if path.exists():
            path.unlink()
    store.db.execute("DELETE FROM event_segments WHERE start_id=?", (segment,))
    store.set_setting("events.pruned_through", first["event_id"])
    try:
        assert "history_start_event_id" not in bind(store, capture)
    finally:
        store.close()


def test_range_cache_is_bounded_and_eviction_keeps_history_queryable(tmp_path, monkeypatch):
    store = GatewayStore(tmp_path)
    segments = store.events.segments
    monkeypatch.setattr(segments, "CAPTURE_RANGE_LIMIT", 2)
    try:
        first = append_capture(store, "d" * 32)
        append_capture(store, "e" * 32)
        append_capture(store, "f" * 32)
        assert len(segments._capture_ranges) == 2
        assert bind(store, "d" * 32)["history_start_event_id"] == first["event_id"]
    finally:
        store.close()


def test_another_writer_invalidates_cached_capture_bounds(tmp_path):
    capture = "a" * 32
    store = GatewayStore(tmp_path)
    append_capture(store, capture)
    other = GatewayStore(tmp_path)
    try:
        last = other.append_event("console", {"kind": "log", "capture_id": capture,
                                             "text": "later boot output"})
        assert bind(store, capture)["history_end_event_id"] == last["event_id"]
    finally:
        other.close()
        store.close()
