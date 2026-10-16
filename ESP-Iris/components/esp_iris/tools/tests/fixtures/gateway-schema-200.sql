-- Iris 0.2 schema 200, captured from main c415e03 before segmented storage.

        CREATE TABLE IF NOT EXISTS settings (
            key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_ns INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS devices (
            device_id TEXT PRIMARY KEY, alias TEXT UNIQUE,
            first_seen_ns INTEGER NOT NULL, last_seen_ns INTEGER NOT NULL,
            cached_json TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS sessions (
            id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT NOT NULL,
            boot_id TEXT, session_id TEXT, endpoint TEXT, link_role TEXT NOT NULL,
            started_ns INTEGER NOT NULL, ended_ns INTEGER
        );
        CREATE TABLE IF NOT EXISTS operations (
            operation_id TEXT PRIMARY KEY, device_id TEXT NOT NULL,
            actor_type TEXT NOT NULL, actor_name TEXT NOT NULL,
            action TEXT NOT NULL, params_json TEXT NOT NULL,
            status TEXT NOT NULL, result_json TEXT, error TEXT,
            created_ns INTEGER NOT NULL, started_ns INTEGER,
            finished_ns INTEGER, queue_position INTEGER NOT NULL DEFAULT 0,
            progress_json TEXT, request_fingerprint TEXT
        );
        CREATE INDEX IF NOT EXISTS operations_device_created
            ON operations(device_id, created_ns DESC);
        CREATE TABLE IF NOT EXISTS events (
            event_id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT,
            category TEXT NOT NULL, host_receive_ns INTEGER NOT NULL,
            payload_json TEXT NOT NULL, capture_id TEXT
        );
        CREATE INDEX IF NOT EXISTS events_device_cursor
            ON events(device_id, event_id);
        CREATE TABLE IF NOT EXISTS system_audit (
            audit_id INTEGER PRIMARY KEY AUTOINCREMENT,
            actor_type TEXT NOT NULL, actor_name TEXT NOT NULL,
            action TEXT NOT NULL, details_json TEXT NOT NULL,
            created_ns INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS agent_tokens (
            token_id TEXT PRIMARY KEY, name TEXT NOT NULL UNIQUE,
            token_hash TEXT NOT NULL, created_ns INTEGER NOT NULL,
            last_used_ns INTEGER, revoked_ns INTEGER,
            scopes_json TEXT NOT NULL DEFAULT '["files.read"]'
        );
        CREATE TABLE IF NOT EXISTS log_index (
            id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT NOT NULL,
            path TEXT NOT NULL, first_event_id INTEGER NOT NULL,
            last_event_id INTEGER NOT NULL, first_ns INTEGER NOT NULL,
            last_ns INTEGER NOT NULL, compressed_bytes INTEGER NOT NULL DEFAULT 0,
            UNIQUE(device_id, path)
        );
        CREATE TABLE IF NOT EXISTS firmware_artifacts (
            artifact_id TEXT PRIMARY KEY, binary_sha256 TEXT NOT NULL UNIQUE,
            elf_sha256 TEXT NOT NULL, project_name TEXT NOT NULL,
            version TEXT NOT NULL, manifest_json TEXT NOT NULL,
            created_ns INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS firmware_artifacts_elf_sha
            ON firmware_artifacts(elf_sha256, created_ns DESC);

        CREATE TABLE IF NOT EXISTS operation_reconciliations (
            reconciliation_id TEXT PRIMARY KEY, operation_id TEXT NOT NULL
            REFERENCES operations(operation_id), record_json TEXT NOT NULL);
        CREATE INDEX IF NOT EXISTS reconciliation_operation ON operation_reconciliations(operation_id);
        CREATE TABLE IF NOT EXISTS console_captures (
            capture_id TEXT PRIMARY KEY, endpoint TEXT NOT NULL, device_id TEXT,
            first_ns INTEGER NOT NULL, last_ns INTEGER NOT NULL,
            bytes_received INTEGER NOT NULL DEFAULT 0, binding_offset INTEGER);
        CREATE INDEX IF NOT EXISTS captures_device ON console_captures(device_id, first_ns);
        CREATE INDEX IF NOT EXISTS captures_endpoint ON console_captures(endpoint, first_ns);
        CREATE INDEX IF NOT EXISTS events_capture ON events(capture_id, event_id);

PRAGMA user_version=200;
