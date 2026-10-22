#!/usr/bin/env python3
"""Create a build-local BQ27220 profile-read error guard for pinned 0.1.3."""
import argparse
import hashlib
import json
from pathlib import Path

INPUT_SHA256 = "7e4c98eb6804a0395f6a8c9d06f1b334178d0db46dc7487d202593d2c6a25c10"
OUTPUT_SHA256 = "3d14cbc732f6d5423f78d1a05725b2ae681d0ad238a756090b44450d31b1571d"

REPLACEMENTS = (
    (
        "static esp_err_t bq27220_verify_profile(bq27220_handle_t handle, const bq27220_config_t *config, bool include_learned_capacity)\n{\n",
        "static esp_err_t bq27220_verify_profile(bq27220_handle_t handle, const bq27220_config_t *config,\n"
        "                                         bool include_learned_capacity, bool *mismatch)\n{\n"
        "    if (mismatch) *mismatch = false;\n",
    ),
    (
        '        if (actual != fields[i].value) {\n'
        '            ESP_LOGW(TAG, "Profile verify mismatch at %s (0x%04x): expected=%u actual=%u", fields[i].name,\n'
        '                     fields[i].address, fields[i].value, actual);\n'
        '            return ESP_ERR_INVALID_STATE;\n'
        '        }\n',
        '        if (actual != fields[i].value) {\n'
        '            ESP_LOGW(TAG, "Profile verify mismatch at %s (0x%04x): expected=%u actual=%u", fields[i].name,\n'
        '                     fields[i].address, fields[i].value, actual);\n'
        '            if (mismatch) *mismatch = true;\n'
        '            return ESP_ERR_INVALID_STATE;\n'
        '        }\n',
    ),
    (
        '    ret = bq27220_verify_profile(handle, config, false);\n'
        '    if (ret == ESP_OK) {\n'
        '        ESP_LOGI(TAG, "Skip battery profile update: verification=full");\n'
        '        ESP_GOTO_ON_ERROR(bq27220_seal(handle), err, TAG, "Failed to seal after profile check");\n'
        '        return handle;\n'
        '    }\n',
        '    bool mismatch = false;\n'
        '    ret = bq27220_verify_profile(handle, config, false, &mismatch);\n'
        '    if (ret == ESP_OK) {\n'
        '        ESP_LOGI(TAG, "Skip battery profile update: verification=full");\n'
        '        ESP_GOTO_ON_ERROR(bq27220_seal(handle), err, TAG, "Failed to seal after profile check");\n'
        '        return handle;\n'
        '    }\n'
        '    if (ret != ESP_ERR_INVALID_STATE || !mismatch) {\n'
        '        ESP_LOGE(TAG, "Battery profile verification failed without a confirmed mismatch: %s", esp_err_to_name(ret));\n'
        '        goto err;\n'
        '    }\n',
    ),
    (
        '    ESP_GOTO_ON_ERROR(bq27220_verify_profile(handle, config, true), exit_cfg_update, TAG, "Battery profile immediate verification failed");\n',
        '    ESP_GOTO_ON_ERROR(bq27220_verify_profile(handle, config, true, NULL), exit_cfg_update, TAG, "Battery profile immediate verification failed");\n',
    ),
)


def transform(source: str) -> str:
    if hashlib.sha256(source.encode()).hexdigest() != INPUT_SHA256:
        raise ValueError("Unexpected BQ27220 driver source; review the component change before patching")
    result = source
    for old, new in REPLACEMENTS:
        if result.count(old) != 1:
            raise ValueError("Expected exactly one matching block in BQ27220 driver source")
        result = result.replace(old, new, 1)
    if hashlib.sha256(result.encode()).hexdigest() != OUTPUT_SHA256:
        raise ValueError("Patched BQ27220 output hash did not match audited candidate")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve():
        parser.error("Refusing to overwrite the original BQ27220 source")
    try:
        result = transform(args.source.read_text())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if not args.output.exists() or args.output.read_text() != result:
            args.output.write_text(result)
        print(json.dumps({
            "input_sha256": INPUT_SHA256,
            "output_sha256": OUTPUT_SHA256,
            "output": str(args.output),
        }))
    except (OSError, ValueError) as error:
        parser.exit(1, f"BQ27220 profile-read guard patch failed: {error}\n")


if __name__ == "__main__":
    main()
