// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define FACTORY_NETWORK_SSID_BYTES 33U
#define FACTORY_NETWORK_PASSWORD_BYTES 65U

/* Match the station's fixed byte limits at every credential write boundary. */
static inline bool factory_network_ssid_valid(const char *ssid)
{
    if (!ssid) return false;
    size_t n = 0;
    while (n < FACTORY_NETWORK_SSID_BYTES && ssid[n]) ++n;
    return n && n < FACTORY_NETWORK_SSID_BYTES;
}
static inline bool factory_network_password_valid(const char *password)
{
    if (!password) return false;
    size_t n = 0;
    while (n < FACTORY_NETWORK_PASSWORD_BYTES && password[n]) ++n;
    if (!n || (n >= 8 && n <= 63)) return true;
    if (n != 64) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = password[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}
