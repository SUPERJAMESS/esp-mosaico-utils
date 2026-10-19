/* Private path validation, request decoding and filesystem metadata helpers.
 * Included by esp_iris_files.c; the file service owns all volume state. */

static size_t bounded_string_length(const char *value, size_t capacity)
{
    size_t size = 0;
    while (size < capacity && value[size] != '\0') {
        ++size;
    }
    return size;
}

/* ESP-IDF's VFS exposes stat(), but not lstat(). The supported backing file
 * systems (LittleFS, SPIFFS and FATFS) do not implement symbolic links. */
static int file_stat(const char *path, struct stat *metadata)
{
    return stat(path, metadata);
}

static bool valid_utf8(const uint8_t *value, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const uint8_t first = value[offset++];
        if (first < 0x80U) {
            if (first == 0 || first < 0x20U || first == 0x7fU) {
                return false;
            }
            continue;
        }
        size_t continuation = 0;
        uint32_t codepoint = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation = 1;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation = 2;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation = 3;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (continuation > size - offset) {
            return false;
        }
        for (size_t i = 0; i < continuation; ++i) {
            const uint8_t next = value[offset++];
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6) | (next & 0x3fU);
        }
        if ((continuation == 2 && codepoint < 0x800U) ||
            (continuation == 3 && codepoint < 0x10000U) ||
            codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

static bool valid_volume_id(const char *id)
{
    if (id == NULL) {
        return false;
    }
    const size_t size = bounded_string_length(
        id, ESP_IRIS_FILE_VOLUME_ID_MAX + 1U);
    if (size == 0 || size > ESP_IRIS_FILE_VOLUME_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        const char value = id[i];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == '_' || value == '-')) {
            return false;
        }
    }
    return true;
}

static bool valid_relative_path(const uint8_t *path, size_t size)
{
    if (size == 0) {
        return true;
    }
    if (path == NULL || size > ESP_IRIS_FILE_PATH_MAX || path[0] == '/' ||
        path[size - 1U] == '/' || !valid_utf8(path, size)) {
        return false;
    }
    size_t component_start = 0;
    for (size_t i = 0; i <= size; ++i) {
        if (i < size && path[i] != '/') {
            if (path[i] == '\\') {
                return false;
            }
            continue;
        }
        const size_t component_size = i - component_start;
        if (component_size == 0 ||
            (component_size == 1 && path[component_start] == '.') ||
            (component_size == 2 && path[component_start] == '.' &&
             path[component_start + 1U] == '.')) {
            return false;
        }
        component_start = i + 1U;
    }
    return true;
}

static esp_iris_file_status_t status_from_errno(int value)
{
    switch (value) {
    case ENOENT:
        return ESP_IRIS_FILE_STATUS_NOT_FOUND;
    case ENOTDIR:
        return ESP_IRIS_FILE_STATUS_NOT_DIRECTORY;
    case EACCES:
    case EPERM:
    case EROFS:
        return ESP_IRIS_FILE_STATUS_READ_ONLY;
    case ENOMEM:
        return ESP_IRIS_FILE_STATUS_NO_MEMORY;
    case EBUSY:
        return ESP_IRIS_FILE_STATUS_BUSY;
#ifdef EEXIST
    case EEXIST:
        return ESP_IRIS_FILE_STATUS_EXISTS;
#endif
#ifdef ENOTEMPTY
    case ENOTEMPTY:
        return ESP_IRIS_FILE_STATUS_NOT_EMPTY;
#endif
#ifdef ENOSPC
    case ENOSPC:
        return ESP_IRIS_FILE_STATUS_NO_SPACE;
#endif
#ifdef EXDEV
    case EXDEV:
        return ESP_IRIS_FILE_STATUS_NOT_SUPPORTED;
#endif
    default:
        return ESP_IRIS_FILE_STATUS_IO;
    }
}

static uint64_t weak_etag(const struct stat *metadata)
{
    uint64_t value = 1469598103934665603ULL;
    const uint64_t fields[] = {
        (uint64_t)metadata->st_mode,
        metadata->st_size < 0 ? 0 : (uint64_t)metadata->st_size,
        metadata->st_mtime < 0 ? 0 : (uint64_t)metadata->st_mtime,
    };
    const uint8_t *bytes = (const uint8_t *)fields;
    for (size_t i = 0; i < sizeof(fields); ++i) {
        value ^= bytes[i];
        value *= 1099511628211ULL;
    }
    return value;
}

static uint8_t file_kind(const struct stat *metadata)
{
    if (S_ISREG(metadata->st_mode)) {
        return ESP_IRIS_FILE_KIND_REGULAR;
    }
    if (S_ISDIR(metadata->st_mode)) {
        return ESP_IRIS_FILE_KIND_DIRECTORY;
    }
    return 0;
}

static esp_iris_file_status_t directory_is_empty(const char *path,
                                                 bool *out_empty)
{
    DIR *directory = opendir(path);
    if (directory == NULL) {
        return status_from_errno(errno);
    }
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            *out_empty = false;
            (void)closedir(directory);
            return ESP_IRIS_FILE_STATUS_OK;
        }
    }
    const int read_errno = errno;
    const int close_result = closedir(directory);
    if (read_errno != 0) {
        return status_from_errno(read_errno);
    }
    if (close_result != 0) {
        return status_from_errno(errno);
    }
    *out_empty = true;
    return ESP_IRIS_FILE_STATUS_OK;
}

static iris_file_volume_t *find_volume(const uint8_t *id, size_t size)
{
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_FILE_VOLUMES; ++i) {
        iris_file_volume_t *volume = &s_volumes[i];
        if (volume->used && strlen(volume->id) == size &&
            memcmp(volume->id, id, size) == 0) {
            return volume;
        }
    }
    return NULL;
}

static esp_iris_file_status_t decode_path_request_with_trailer(
    const iris_file_work_t *work, iris_file_volume_t **out_volume,
    char out_relative[ESP_IRIS_FILE_PATH_MAX + 1U], size_t trailer_size,
    const uint8_t **out_trailer)
{
    if (work->payload_size < IRIS_FILE_PATH_REQUEST_HEADER_SIZE) {
        return ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    const uint8_t volume_size = work->payload[0];
    const uint16_t path_size = iris_get_le16(work->payload + 2);
    if (work->payload[1] != 0 || volume_size == 0 ||
        volume_size > ESP_IRIS_FILE_VOLUME_ID_MAX ||
        path_size > ESP_IRIS_FILE_PATH_MAX ||
        work->payload_size != IRIS_FILE_PATH_REQUEST_HEADER_SIZE +
                                  volume_size + path_size + trailer_size) {
        return ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    iris_file_volume_t *volume = find_volume(
        work->payload + IRIS_FILE_PATH_REQUEST_HEADER_SIZE, volume_size);
    if (volume == NULL) {
        return ESP_IRIS_FILE_STATUS_NOT_FOUND;
    }
    const uint8_t *relative = work->payload +
        IRIS_FILE_PATH_REQUEST_HEADER_SIZE + volume_size;
    if (!valid_relative_path(relative, path_size)) {
        return ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    memcpy(out_relative, relative, path_size);
    out_relative[path_size] = '\0';
    *out_volume = volume;
    if (out_trailer != NULL) {
        *out_trailer = relative + path_size;
    }
    return ESP_IRIS_FILE_STATUS_OK;
}

static esp_iris_file_status_t decode_path_request(
    const iris_file_work_t *work, iris_file_volume_t **out_volume,
    char out_relative[ESP_IRIS_FILE_PATH_MAX + 1U])
{
    return decode_path_request_with_trailer(work, out_volume, out_relative,
                                            0, NULL);
}

static esp_iris_file_status_t resolve_existing_path(
    const iris_file_volume_t *volume, const char *relative,
    char out[IRIS_FILE_FULL_PATH_SIZE], struct stat *out_metadata)
{
    if (volume->status != NULL) {
        const esp_err_t ready = volume->status(volume->user_ctx);
        if (ready != ESP_OK) {
            return ready == ESP_ERR_INVALID_STATE ? ESP_IRIS_FILE_STATUS_BUSY
                                                  : ESP_IRIS_FILE_STATUS_IO;
        }
        /* Deferred registration cannot validate the root until mount completes.
         * Check it for every new path, including paths below the root. */
        struct stat root;
        if (file_stat(volume->base_path, &root) != 0) {
            return status_from_errno(errno);
        }
        if (!S_ISDIR(root.st_mode) || S_ISLNK(root.st_mode)) {
            return ESP_IRIS_FILE_STATUS_NOT_SUPPORTED;
        }
    }
    const size_t base_size = strlen(volume->base_path);
    const size_t relative_size = strlen(relative);
    if (base_size + (relative_size > 0 ? 1U : 0U) + relative_size + 1U >
        IRIS_FILE_FULL_PATH_SIZE) {
        return ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    memcpy(out, volume->base_path, base_size + 1U);
    if (relative_size == 0) {
        if (file_stat(out, out_metadata) != 0) {
            return status_from_errno(errno);
        }
        return S_ISLNK(out_metadata->st_mode)
            ? ESP_IRIS_FILE_STATUS_NOT_SUPPORTED
            : ESP_IRIS_FILE_STATUS_OK;
    }

    const char *component = relative;
    size_t used = base_size;
    while (*component != '\0') {
        const char *separator = strchr(component, '/');
        const size_t component_size = separator != NULL
            ? (size_t)(separator - component) : strlen(component);
        out[used++] = '/';
        memcpy(out + used, component, component_size);
        used += component_size;
        out[used] = '\0';
        if (file_stat(out, out_metadata) != 0) {
            return status_from_errno(errno);
        }
        if (S_ISLNK(out_metadata->st_mode)) {
            return ESP_IRIS_FILE_STATUS_NOT_SUPPORTED;
        }
        component = separator != NULL ? separator + 1 : component + component_size;
    }
    return ESP_IRIS_FILE_STATUS_OK;
}

static esp_iris_file_status_t resolve_parent_path(
    const iris_file_volume_t *volume, const char *relative,
    char out_parent[IRIS_FILE_FULL_PATH_SIZE],
    char out_target[IRIS_FILE_FULL_PATH_SIZE])
{
    if (relative[0] == '\0') {
        return ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    char parent_relative[ESP_IRIS_FILE_PATH_MAX + 1U];
    const char *separator = strrchr(relative, '/');
    const char *name = separator == NULL ? relative : separator + 1;
    const size_t parent_size = separator == NULL
        ? 0 : (size_t)(separator - relative);
    memcpy(parent_relative, relative, parent_size);
    parent_relative[parent_size] = '\0';
    struct stat metadata;
    esp_iris_file_status_t status = resolve_existing_path(
        volume, parent_relative, out_parent, &metadata);
    if (status != ESP_IRIS_FILE_STATUS_OK) {
        return status;
    }
    if (!S_ISDIR(metadata.st_mode)) {
        return ESP_IRIS_FILE_STATUS_NOT_DIRECTORY;
    }
    const int written = snprintf(out_target, IRIS_FILE_FULL_PATH_SIZE,
                                 "%s/%s", out_parent, name);
    return written < 0 || (size_t)written >= IRIS_FILE_FULL_PATH_SIZE
        ? ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT
        : ESP_IRIS_FILE_STATUS_OK;
}

static esp_iris_file_status_t target_metadata(
    const char *path, bool *out_exists, struct stat *out_metadata)
{
    if (file_stat(path, out_metadata) == 0) {
        *out_exists = true;
        return S_ISLNK(out_metadata->st_mode)
            ? ESP_IRIS_FILE_STATUS_NOT_SUPPORTED
            : ESP_IRIS_FILE_STATUS_OK;
    }
    if (errno == ENOENT) {
        *out_exists = false;
        memset(out_metadata, 0, sizeof(*out_metadata));
        return ESP_IRIS_FILE_STATUS_OK;
    }
    return status_from_errno(errno);
}
