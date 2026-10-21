from iris_gateway.transfer_progress import TransferProgress


def test_percentage_bursts_do_not_force_per_chunk_disk_commits(monkeypatch):
    clock = [100.0]
    monkeypatch.setattr("iris_gateway.transfer_progress.time.monotonic", lambda: clock[0])
    gate = TransferProgress()
    accepted = []
    for chunk in range(100):
        clock[0] = 100.0 + chunk / 100
        progress = {"stage": "transferring", "bytes_received": chunk, "bytes_total": 100}
        if gate.accept(progress):
            accepted.append(chunk)
    assert accepted == [0]
    clock[0] = 101.0
    assert gate.accept({"stage": "transferring", "bytes_received": 99, "bytes_total": 100})


def test_phase_and_component_changes_are_immediate(monkeypatch):
    monkeypatch.setattr("iris_gateway.transfer_progress.time.monotonic", lambda: 100.0)
    gate = TransferProgress()
    for stage, component in [("erasing", 1), ("transferring", 1), ("verifying", 1),
                              ("erasing", 2), ("transferring", 2), ("transferring", 3),
                              ("committing", 3)]:
        assert gate.accept({"stage": stage, "component_id": component,
                            "bytes_received": 10, "bytes_total": 100})


def test_final_byte_count_is_immediate_and_operations_are_independent(monkeypatch):
    monkeypatch.setattr("iris_gateway.transfer_progress.time.monotonic", lambda: 100.0)
    first, second = TransferProgress(), TransferProgress()
    progress = {"stage": "transferring", "bytes_received": 1, "bytes_total": 100}
    assert first.accept(progress)
    assert not first.accept(progress)
    assert second.accept(progress)
    assert first.accept(dict(progress, bytes_received=100))
