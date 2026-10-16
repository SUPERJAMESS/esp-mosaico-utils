"""Executable HTTP contract for the ESP-Iris Gateway adapter."""

from __future__ import annotations

from typing import Any

from .device_state import DEVICE_STATES


def build_openapi(auth_required: bool) -> dict[str, Any]:
    control_paths = {
        "/v2/consoles/reset": "Reset an owned UART/Serial-JTAG console (run, ROM, or attach)",
        "/v2/devices/{device_id}/rpc/raw": "Raw RPC",
        "/v2/devices/{device_id}/restart": "Restart device",
        "/v2/devices/{device_id}/factory-recovery": "Enter factory recovery",
        "/v2/devices/{device_id}/ota": "Validated OTA",
        "/v2/devices/{device_id}/system-update": "Authenticated system update",
        "/v2/devices/{device_id}/input": "Pointer or touch gesture",
        "/v2/devices/{device_id}/console": "Submit one console command line",
        "/v2/devices/{device_id}/screenshot": "Capture screenshot",
        "/v2/devices/{device_id}/mirror/start": "Start media mirror",
        "/v2/devices/{device_id}/mirror/stop": "Stop media mirror",
    }
    paths: dict[str, Any] = {
        "/v2/export": {"post": {"summary": "Create a bounded background evidence export",
            "requestBody": {"content": {"application/json": {"schema": {"type": "object", "additionalProperties": False,
                "properties": {"device_id": {"type": "string"}, "operation_id": {"type": "string"},
                               "since_ns": {"type": "integer", "minimum": 0}, "until_ns": {"type": "integer", "minimum": 0},
                               "retain": {"type": "boolean", "default": False}}}}}},
            "responses": {"202": {"description": "Job with status_url and download_url"},
                          "429": {"description": "Export concurrency or disk budget exhausted"}}}},
        "/v2/exports": {"get": {"summary": "List retained export jobs, progress and failures"}},
        "/v2/exports/{job_id}": {"get": {"summary": "Export progress, checksum, expiry and failure reason"},
                                 "delete": {"summary": "Delete a terminal export and its archive"}},
        "/v2/exports/{job_id}/cancel": {"post": {"summary": "Cancel an active export and reclaim temporary files"}},
        "/v2/exports/{job_id}/download": {"get": {"summary": "Stream completed ZIP with X-Checksum-SHA256; retry until expiry"}},
        "/v2/project": {"get": {"summary": "Local project session, discovery and shared ownership; project Gateways only"}},
        "/v2/project/clients": {"post": {"summary": "Register an idempotent project client lease; requires current session_id"}},
        "/v2/project/clients/{client_id}/renew": {"post": {"summary": "Renew a client lease using its private lease_token and session_id"}},
        "/v2/project/clients/{client_id}/release": {"post": {"summary": "Release only this client's lease; does not stop the Gateway"}},
        "/v2/project/acquire": {"post": {
            "summary": "Acquire a device; explicit Serial/JTAG selection permits that console",
            "requestBody": {"content": {"application/json": {"schema": {
                "type": "object", "properties": {
                    "device_id": {"type": ["string", "null"]},
                    "endpoint": {"type": ["string", "null"]},
                    "baudrate": {"type": "integer", "minimum": 9600, "maximum": 3000000,
                                 "description": "Requires an explicit serial endpoint; defaults to 115200"},
                    "auto": {"type": "boolean"}, "allow_none": {"type": "boolean"},
                    "pairing_token": {"type": ["string", "null"]},
                    "timeout": {"type": "number"},
                },
            }}}},
        }},
        "/v2/project/release": {"post": {"summary": "Release an idle owned device"}},
        "/v2/project/takeovers": {"post": {
            "summary": "Request a device from its current owner into this receiving session",
            "requestBody": {"required": True, "content": {"application/json": {"schema": {
                "type": "object", "additionalProperties": False,
                "properties": {"device_id": {"type": ["string", "null"]}, "endpoint": {"type": ["string", "null"]},
                               "takeover_id": {"type": "string", "format": "uuid"},
                               "force": {"type": "boolean", "default": False},
                               "timeout": {"type": "number", "exclusiveMinimum": 0, "maximum": 3600, "default": 120}},
                "oneOf": [
                    {"required": ["device_id"], "properties": {"device_id": {"type": "string"}, "endpoint": {"type": "null"}}},
                    {"required": ["endpoint"], "properties": {"endpoint": {"type": "string"}, "device_id": {"type": "null"}}},
                ],
            }}}},
        }},
        "/v2/project/takeovers/{takeover_id}": {"get": {"summary": "Read durable takeover state"}},
        "/v2/project/takeovers/{takeover_id}/resume": {"post": {"summary": "Continue receiver identity validation"}},
        "/v2/project/takeovers/{takeover_id}/abort": {"post": {"summary": "Roll back an incomplete takeover from the original owner"}},
        "/v2/project/takeovers/{takeover_id}/reconcile": {"post": {"summary": "Resolve reserved ownership after both original sessions died"}},
        "/v2/project/reconcile": {"post": {"summary": "Explicitly reconcile a dead owner's ordinary device claim"}},
        "/v2/health": {"get": {"summary": "Gateway health"}},
        "/v2/auth/login": {"post": {"summary": "Developer password login"}},
        "/v2/devices": {"get": {"summary": "Connected and cached devices"}},
        "/v2/host-operations": {
            "post": {"summary": "Submit a local process-owned ROM operation",
                     "description": "Requires a private local request file ID; executable commands are never accepted over HTTP."}
        },
        "/v2/devices/{device_id}": {
            "get": {"summary": "Current or cached status"},
            "delete": {
                "summary": "Remove an offline device from inventory",
                "description": "Preserves operations, events, logs, and audit history.",
            },
        },
        "/v2/devices/{device_id}/memory": {
            "get": {"summary": "Live internal RAM, SPIRAM and task stack watermarks"}
        },
        "/v2/devices/{device_id}/system-inventory": {
            "get": {"summary": "Live bootloader and partition-table inventory"}
        },
        "/v2/devices/{device_id}/crashes": {
            "get": {"summary": "Live crash metadata and archived diagnosis"}
        },
        "/v2/devices/{device_id}/crashes/core-dump": {
            "get": {"summary": "Preserve and download the retained Core Dump"}
        },
        "/v2/devices/{device_id}/crashes/archive": {
            "post": {"summary": "Archive and decode current crash evidence"}
        },
        "/v2/mode": {
            "get": {"summary": "Get global mode"},
            "put": {"summary": "Switch develop or observe mode"},
        },
        "/v2/events": {"get": {"summary": "Cursor-based event history"}},
        "/v2/events/ws": {"get": {"summary": "Resumable event WebSocket"}},
        "/v2/operations": {"get": {"summary": "Device operation records"}},
        "/v2/operations/{operation_id}/reconcile": {
            "post": {"summary": "Append a read-only observation of an uncertain operation; never replay writes"}
        },
        "/v2/operations/{operation_id}/reconciliations": {
            "get": {"summary": "Append-only reconciliation evidence; original status is unchanged"}
        },
        "/v2/devices/{device_id}/files/volumes": {
            "get": {"summary": "Registered file volumes and capabilities"}
        },
        "/v2/devices/{device_id}/files/stat": {
            "get": {"summary": "File or directory metadata"}
        },
        "/v2/devices/{device_id}/files": {
            "get": {"summary": "Paginated directory entries"}
        },
        "/v2/devices/{device_id}/file": {
            "get": {"summary": "Stream a file with HTTP Range support"},
            "put": {"summary": "Stream a create or atomic file replacement"},
            "delete": {"summary": "Delete a file or empty directory"},
        },
        "/v2/devices/{device_id}/directories": {
            "post": {"summary": "Create one directory"}
        },
        "/v2/devices/{device_id}/file-rename": {
            "post": {"summary": "Rename within one logical volume"}
        },
        "/v2/firmware-artifacts": {
            "get": {"summary": "Archived firmware bundles"},
            "post": {"summary": "Archive BIN, ELF and map as one validated bundle"},
        },
        "/v2/system-audit": {"get": {"summary": "Gateway system audit"}},
        "/v2/metrics": {"get": {"summary": "Gateway process metrics"}},

    }
    for path, summary in control_paths.items():
        paths[path] = {"post": {"summary": summary}}
    compatibility_schema = {
        "type": "object", "additionalProperties": False,
        "description": "Explicit device expectations, checked before writes and bound to the operation ID.",
        "properties": {
            **{field: {"type": "string", "minLength": 1, "maxLength": 64}
               for field in ("chip_target", "product_contract", "board_id", "layout_id")},
            "recovery_abi": {"type": "integer", "minimum": 1, "maximum": 65535},
        },
    }
    paths["/v2/devices/{device_id}/ota"]["post"]["requestBody"] = {
        "required": True,
        "content": {
            "application/json": {
                "schema": {
                    "type": "object",
                    "required": ["artifact_id"],
                    "properties": {
                        "artifact_id": {"type": "string"},
                        "compatibility": compatibility_schema,
                        "preconditions": {
                            "type": "object", "additionalProperties": False,
                            "description": "Checked on live Recovery before OTA BEGIN; bound to the operation ID.",
                            "properties": {
                                "recovery_version": {"type": "string", "minLength": 1, "maxLength": 64},
                                "partition_table_sha256": {"type": "string", "pattern": "^[0-9a-fA-F]{64}$"},
                            },
                        },
                        "execution_mode": {
                            "type": "string",
                            "enum": ["recovery", "application"],
                            "default": "recovery",
                        },
                        "validation_mode": {
                            "type": "string",
                            "enum": ["elf_sha256", "version"],
                            "default": "elf_sha256",
                        },
                    },
                }
            }
        },
    }
    paths["/v2/devices/{device_id}/console"]["post"].update({
        "description": "Write one UTF-8 line plus LF to the live console control link, serialized with Iris records. "
                       "No RPC handler is required. sent=true confirms host transport write, not firmware execution. "
                       "No automatic replay after errors or reconnect. Gateway discovery/protocol records are reserved.",
        "requestBody": {"required": True, "content": {"application/json": {"schema": {
            "type": "object", "required": ["line"], "properties": {
                "line": {"type": "string", "description": "1–255 printable UTF-8 bytes; no CR, LF or control characters"},
            },
        }}}},
        "responses": {"200": {"description": "Text written (or existing operation reused); execution remains unconfirmed"}},
    })
    paths["/v2/devices/{device_id}/factory-recovery"]["post"]["requestBody"] = {
        "required": False,
        "content": {"application/json": {"schema": {
            "type": "object",
            "properties": {"wait": {"type": "boolean", "default": False},
                           "timeout": {"type": "number", "minimum": 0.1, "maximum": 30, "default": 30}},
        }}},
    }
    paths["/v2/devices/{device_id}/system-update"]["post"]["requestBody"] = {
        "required": True,
        "content": {
            "application/vnd.esp-iris.system-update+zip": {
                "schema": {"type": "string", "format": "binary"}
            }
        },
    }
    paths["/v2/devices/{device_id}/system-update"]["post"]["parameters"] = [{
        "in": "header", "name": "X-Iris-Compatibility", "required": False,
        "content": {"application/json": {"schema": compatibility_schema}},
    }]
    paths["/v2/host-operations"]["post"]["requestBody"] = {
        "required": True, "content": {"application/json": {"schema": {
            "type": "object", "additionalProperties": False,
            "required": ["request_id"], "properties": {"request_id": {"type": "string", "format": "uuid"}},
        }}},
    }
    paths["/v2/devices"]["get"]["responses"] = {
        "200": {"description": "Device inventory", "content": {"application/json": {"schema": {
            "type": "object", "properties": {"devices": {"type": "array", "items": {"$ref": "#/components/schemas/Device"}}},
        }}}},
    }
    paths["/v2/devices/{device_id}/jobs/{job_id}"] = {
        "get": {"summary": "Query job"},
        "delete": {"summary": "Cancel job"},
    }
    document = {
        "openapi": "3.1.0",
        "info": {
            "title": "ESP-Iris Developer Gateway",
            "version": "1.0.0",
            "description": "Gateway-only device control and observation API.",
        },
        "servers": [{"url": "/"}],
        "components": {
            "schemas": {"Device": {
                "type": "object", "required": ["device_id", "state"],
                "properties": {
                    "device_id": {"type": "string"},
                    "state": {"type": "string", "enum": list(DEVICE_STATES)},
                    "firmware_mode": {"type": "string", "enum": ["normal", "recovery", "rom", "unknown"]},
                    "console_available": {"type": "boolean", "description": "Live text control link available"},
                    "owner_session_id": {"type": ["string", "null"]},
                    "busy_reasons": {"type": "array", "items": {"type": "object"}},
                },
            }},
            "securitySchemes": {
                "cookieAuth": {
                    "type": "apiKey",
                    "in": "cookie",
                    "name": "esp_iris_session",
                },
                "agentToken": {"type": "http", "scheme": "bearer"},
            }
        },
        "paths": paths,
    }
    if auth_required:
        document["security"] = [{"cookieAuth": []}, {"agentToken": []}]
    return document


__all__ = ["build_openapi"]
