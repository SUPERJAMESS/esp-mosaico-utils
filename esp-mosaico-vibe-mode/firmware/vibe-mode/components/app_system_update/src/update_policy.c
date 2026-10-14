// SPDX-License-Identifier: Apache-2.0
#include "sdkconfig.h"
#include "factory_system_update.h"
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE && CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND
#include "update_internal.h"

typedef struct {
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    const char *label;
    uint32_t offset;
    uint32_t size;
} immutable_partition_contract_t;

static const immutable_partition_contract_t s_immutable_partitions[] = {
    {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata", 0x9000, 0x2000},
    {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY, "phy_init", 0xb000, 0x1000},
    {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "sysmeta", 0xc000, 0x14000},
    {ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, MOSAICO_RECOVERY_PARTITION,
     FACTORY_SYSTEM_RECOVERY_OFFSET, FACTORY_SYSTEM_RECOVERY_SIZE},
    {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump", 0x1e0000, 0x20000},
};

esp_err_t update_authorize_component_target(const esp_iris_system_update_component_t *component)
{
    if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER) {
        return component->target_offset == CONFIG_BOOTLOADER_OFFSET_IN_FLASH &&
                       component->size ==
                           CONFIG_PARTITION_TABLE_OFFSET - CONFIG_BOOTLOADER_OFFSET_IN_FLASH
                   ? ESP_OK
                   : ESP_ERR_INVALID_SIZE;
    }
    if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE) {
        return component->target_offset == CONFIG_PARTITION_TABLE_OFFSET &&
                       component->size == FACTORY_SYSTEM_FLASH_SECTOR_BYTES
                   ? ESP_OK
                   : ESP_ERR_INVALID_SIZE;
    }
    if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION) {
        return component->target_offset % FACTORY_SYSTEM_FLASH_SECTOR_BYTES == 0
                   ? ESP_OK
                   : ESP_ERR_INVALID_SIZE;
    }
    if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_RECOVERY) {
        return component->target_offset == FACTORY_SYSTEM_RECOVERY_OFFSET &&
                       component->size == FACTORY_SYSTEM_RECOVERY_SIZE
                   ? ESP_OK
                   : ESP_ERR_INVALID_SIZE;
    }
    if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA) {
        return component->target_offset % FACTORY_SYSTEM_FLASH_SECTOR_BYTES == 0
                   ? ESP_OK
                   : ESP_ERR_INVALID_SIZE;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t update_validate_memory_image(const uint8_t *image, size_t size, const char *name)
{
    ESP_RETURN_ON_FALSE(image != NULL && size >= sizeof(esp_image_header_t), ESP_ERR_IMAGE_INVALID,
                        TAG, "short %s image", name);
    const esp_image_header_t *header = (const esp_image_header_t *)image;
    ESP_RETURN_ON_FALSE(header->magic == ESP_IMAGE_HEADER_MAGIC && header->segment_count > 0 &&
                            header->segment_count <= ESP_IMAGE_MAX_SEGMENTS &&
                            header->chip_id == ESP_CHIP_ID_ESP32S31,
                        ESP_ERR_IMAGE_INVALID, TAG, "invalid %s header", name);
    size_t offset = sizeof(*header);
    uint8_t checksum = 0xef;
    for (uint8_t segment = 0; segment < header->segment_count; ++segment) {
        ESP_RETURN_ON_FALSE(size - offset >= sizeof(esp_image_segment_header_t),
                            ESP_ERR_IMAGE_INVALID, TAG, "truncated segment header");
        esp_image_segment_header_t segment_header;
        memcpy(&segment_header, image + offset, sizeof(segment_header));
        offset += sizeof(segment_header);
        ESP_RETURN_ON_FALSE((segment_header.data_len & 3U) == 0 &&
                                segment_header.data_len <= size - offset,
                            ESP_ERR_IMAGE_INVALID, TAG, "invalid segment size");
        for (uint32_t i = 0; i < segment_header.data_len; ++i) {
            checksum ^= image[offset + i];
        }
        offset += segment_header.data_len;
    }
    const size_t checksum_end = (offset + 1U + 15U) & ~(size_t)15U;
    ESP_RETURN_ON_FALSE(checksum_end <= size && image[checksum_end - 1U] == checksum,
                        ESP_ERR_IMAGE_INVALID, TAG, "%s checksum mismatch", name);
    size_t image_end = checksum_end;
    if (header->hash_appended == 1) {
        ESP_RETURN_ON_FALSE(size - image_end >= 32, ESP_ERR_IMAGE_INVALID, TAG, "missing %s digest",
                            name);
        uint8_t digest[32];
        ESP_RETURN_ON_ERROR(update_hash_memory(image, image_end, digest), TAG, "hash %s image",
                            name);
        ESP_RETURN_ON_FALSE(update_bytes_equal(digest, image + image_end, 32),
                            ESP_ERR_IMAGE_INVALID, TAG, "%s digest mismatch", name);
        image_end += 32;
    }
    for (size_t i = image_end; i < size; ++i) {
        ESP_RETURN_ON_FALSE(image[i] == 0xff, ESP_ERR_IMAGE_INVALID, TAG,
                            "%s padding is not erased", name);
    }
    return ESP_OK;
}

esp_err_t update_validate_recovery_release(const uint8_t *image, size_t size)
{
    const size_t description_offset =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    ESP_RETURN_ON_FALSE(image != NULL && size >= description_offset + sizeof(esp_app_desc_t),
                        ESP_ERR_IMAGE_INVALID, TAG, "missing recovery app description");
    esp_app_desc_t description;
    memcpy(&description, image + description_offset, sizeof(description));
    ESP_RETURN_ON_FALSE(description.magic_word == ESP_APP_DESC_MAGIC_WORD &&
                            memchr(description.version, '\0', sizeof(description.version)) != NULL,
                        ESP_ERR_IMAGE_INVALID, TAG, "invalid recovery app description");
    ESP_RETURN_ON_FALSE(factory_recovery_version_can_replace(esp_app_get_description()->version,
                                                             description.version),
                        ESP_ERR_INVALID_VERSION, TAG,
                        "Recovery downgrade or release-line change is not allowed");
    return ESP_OK;
}

const esp_partition_info_t *update_find_partition_entry(const esp_partition_info_t *entries,
                                                        int count, esp_partition_type_t type,
                                                        esp_partition_subtype_t subtype,
                                                        const char *label)
{
    for (int i = 0; i < count; ++i) {
        if (entries[i].magic == ESP_PARTITION_MAGIC && entries[i].type == type &&
            entries[i].subtype == subtype &&
            strncmp((const char *)entries[i].label, label, sizeof(entries[i].label)) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

static bool update_partition_entry_matches_contract(const esp_partition_info_t *entry,
                                                    const immutable_partition_contract_t *contract)
{
    return entry != NULL && entry->type == contract->type && entry->subtype == contract->subtype &&
           strncmp((const char *)entry->label, contract->label, sizeof(entry->label)) == 0 &&
           entry->pos.offset == contract->offset && entry->pos.size == contract->size &&
           entry->flags == 0;
}

esp_err_t update_require_immutable_contract(const esp_partition_info_t *entries, int count)
{
    for (size_t i = 0; i < sizeof(s_immutable_partitions) / sizeof(s_immutable_partitions[0]);
         ++i) {
        const immutable_partition_contract_t *contract = &s_immutable_partitions[i];
        const esp_partition_info_t *entry = update_find_partition_entry(
            entries, count, contract->type, contract->subtype, contract->label);
        ESP_RETURN_ON_FALSE(update_partition_entry_matches_contract(entry, contract),
                            ESP_ERR_INVALID_VERSION, TAG,
                            "immutable partition contract mismatch: %s", contract->label);
    }
    return ESP_OK;
}

bool update_partition_entry_is_immutable(const esp_partition_info_t *entry)
{
    for (size_t i = 0; i < sizeof(s_immutable_partitions) / sizeof(s_immutable_partitions[0]);
         ++i) {
        if (update_partition_entry_matches_contract(entry, &s_immutable_partitions[i])) {
            return true;
        }
    }
    return false;
}

bool update_partition_entry_requires_remote_layout_image(const esp_partition_info_t *entry)
{
    /* Omitted data may be unused or initialized by the application. Do not
     * synthesize erases/writes; only supplied components own write ranges.
     * Applications still need verified images when replacing the layout. */
    return entry->pos.offset >= 0x200000U && entry->type == ESP_PARTITION_TYPE_APP;
}

const esp_partition_info_t *
update_find_partition_entry_at_offset(const esp_partition_info_t *entries, int count,
                                      uint32_t offset)
{
    for (int i = 0; i < count; ++i) {
        if (entries[i].magic == ESP_PARTITION_MAGIC && entries[i].pos.offset == offset) {
            return &entries[i];
        }
    }
    return NULL;
}

void update_partition_descriptor_from_entry(const esp_partition_info_t *entry,
                                            esp_partition_t *partition)
{
    memset(partition, 0, sizeof(*partition));
    partition->flash_chip = esp_flash_default_chip;
    partition->type = entry->type;
    partition->subtype = entry->subtype;
    partition->address = entry->pos.offset;
    partition->size = entry->pos.size;
    partition->erase_size = FACTORY_SYSTEM_FLASH_SECTOR_BYTES;
    memcpy(partition->label, entry->label, sizeof(entry->label));
    partition->label[sizeof(partition->label) - 1U] = '\0';
    partition->encrypted = (entry->flags & PART_FLAG_ENCRYPTED) != 0;
    partition->readonly = (entry->flags & PART_FLAG_READONLY) != 0;
}

esp_err_t update_resolve_component_targets(update_context_t *ctx,
                                           const esp_partition_info_t *entries, int count)
{
    for (size_t i = 0; i < ctx->transaction.plan_count; ++i) {
        factory_update_plan_component_t *plan = &ctx->transaction.plan[i];
        const esp_iris_system_update_component_t *component = &plan->descriptor;
        const esp_partition_info_t *target = NULL;
        if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION) {
            target = ctx->transaction.remote_bridge
                         ? update_find_partition_entry_at_offset(entries, count,
                                                                 component->target_offset)
                         : update_find_partition_entry(entries, count, ESP_PARTITION_TYPE_APP,
                                                       ESP_PARTITION_SUBTYPE_APP_OTA_0, "main_app");
            ESP_RETURN_ON_FALSE(
                target != NULL && target->type == ESP_PARTITION_TYPE_APP &&
                    (!ctx->transaction.remote_bridge || target->pos.offset >= 0x200000U) &&
                    component->target_offset == target->pos.offset,
                ESP_ERR_INVALID_VERSION, TAG, "application target is not target-layout ota_0");
        } else if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA) {
            target =
                update_find_partition_entry_at_offset(entries, count, component->target_offset);
            ESP_RETURN_ON_FALSE(
                target != NULL && target->type == ESP_PARTITION_TYPE_DATA &&
                    !update_partition_entry_is_immutable(target) &&
                    (!ctx->transaction.remote_bridge || target->pos.offset >= 0x200000U),
                ESP_ERR_INVALID_VERSION, TAG,
                "data target is not a mutable target-layout partition");
        } else {
            continue;
        }
        ESP_RETURN_ON_FALSE(target->flags == 0 && component->size <= target->pos.size,
                            ESP_ERR_INVALID_SIZE, TAG,
                            "component does not fit writable target partition");
        for (size_t previous = 0; previous < i; ++previous) {
            ESP_RETURN_ON_FALSE(
                !ctx->transaction.plan[previous].target_partition_valid ||
                    ctx->transaction.plan[previous].target_partition.address != target->pos.offset,
                ESP_ERR_INVALID_ARG, TAG, "multiple components target the same partition");
        }
        update_partition_descriptor_from_entry(target, &plan->target_partition);
        plan->target_partition_valid = true;
    }
    return ESP_OK;
}

esp_err_t update_bridge_geometry(update_context_t *ctx, const esp_partition_info_t *entries,
                                 int count)
{
    if (!ctx->transaction.remote_bridge)
        return ESP_OK;
    const uint8_t *raw = (const uint8_t *)entries;
    size_t offset = 0;
    bool ended = false, md5 = false;
    for (; offset < 0xc00U; offset += 32U) {
        uint16_t magic = (uint16_t)raw[offset] | ((uint16_t)raw[offset + 1U] << 8U);
        if (magic == ESP_PARTITION_MAGIC && !md5)
            continue;
        if (magic == 0xebebU && !md5) {
            md5 = true;
            for (size_t j = 2; j < 16; ++j)
                ESP_RETURN_ON_FALSE(raw[offset + j] == 0xffU, ESP_ERR_INVALID_ARG, TAG,
                                    "MD5 marker padding");
            continue;
        }
        ESP_RETURN_ON_FALSE(magic == 0xffffU, ESP_ERR_INVALID_ARG, TAG, "partition termination");
        ended = true;
        break;
    }
    ESP_RETURN_ON_FALSE(ended, ESP_ERR_INVALID_ARG, TAG, "partition terminator missing");
    for (; offset < 4096U; ++offset)
        ESP_RETURN_ON_FALSE(raw[offset] == 0xffU, ESP_ERR_INVALID_ARG, TAG,
                            "partition tail not erased");
    ESP_RETURN_ON_FALSE(count >= 5, ESP_ERR_INVALID_ARG, TAG, "protected prefix missing");
    for (int i = 0; i < 5; ++i)
        ESP_RETURN_ON_FALSE(
            update_partition_entry_matches_contract(&entries[i], &s_immutable_partitions[i]),
            ESP_ERR_INVALID_ARG, TAG, "protected prefix order");
    uint32_t capacity = 0, physical = 0;
    ESP_RETURN_ON_ERROR(esp_flash_get_size(NULL, &capacity), TAG, "Flash size");
    ESP_RETURN_ON_ERROR(esp_flash_get_physical_size(NULL, &physical), TAG, "physical Flash size");
    if (physical < capacity)
        capacity = physical;
    for (int i = 0; i < count; ++i) {
        const esp_partition_info_t *p = &entries[i];
        if (p->magic != ESP_PARTITION_MAGIC)
            continue;
        ESP_RETURN_ON_FALSE(p->label[0] != 0, ESP_ERR_INVALID_ARG, TAG, "empty partition label");
        bool nul = false;
        for (size_t label_byte = 0; label_byte < sizeof(p->label); ++label_byte) {
            if (p->label[label_byte] == 0)
                nul = true;
            ESP_RETURN_ON_FALSE(!nul || p->label[label_byte] == 0, ESP_ERR_INVALID_ARG, TAG,
                                "partition label padding");
        }
        const uint64_t end = (uint64_t)p->pos.offset + p->pos.size;
        ESP_RETURN_ON_FALSE(
            p->flags == 0 && p->pos.size > 0 && end <= capacity && p->pos.size % 4096U == 0 &&
                p->pos.offset % (p->type == ESP_PARTITION_TYPE_APP ? 65536U : 4096U) == 0 &&
                (p->type == ESP_PARTITION_TYPE_APP || p->type == ESP_PARTITION_TYPE_DATA) &&
                (p->type != ESP_PARTITION_TYPE_APP ||
                 (p->subtype == ESP_PARTITION_SUBTYPE_APP_TEST &&
                  update_partition_entry_is_immutable(p)) ||
                 (p->subtype >= 0x10 && p->subtype <= 0x1f)) &&
                (p->pos.offset >= 0x200000U || update_partition_entry_is_immutable(p)),
            ESP_ERR_INVALID_ARG, TAG, "remote partition geometry");
        for (int j = 0; j < i; ++j) {
            const esp_partition_info_t *q = &entries[j];
            if (q->magic != ESP_PARTITION_MAGIC)
                continue;
            ESP_RETURN_ON_FALSE(
                memcmp(p->label, q->label, sizeof(p->label)) != 0 &&
                    !(p->type == ESP_PARTITION_TYPE_APP && q->type == ESP_PARTITION_TYPE_APP &&
                      p->subtype == q->subtype) &&
                    !((uint64_t)p->pos.offset < (uint64_t)q->pos.offset + q->pos.size &&
                      (uint64_t)q->pos.offset < end),
                ESP_ERR_INVALID_ARG, TAG, "remote duplicate or overlapping partition");
        }
    }
    return ESP_OK;
}

esp_err_t update_validate_partition_table(update_context_t *ctx, const uint8_t *image)
{
    int target_count = 0;
    const esp_partition_info_t *target_entries = (const esp_partition_info_t *)image;
    ESP_RETURN_ON_ERROR(esp_partition_table_verify(target_entries, true, &target_count), TAG,
                        "invalid target partition table");
    ESP_RETURN_ON_ERROR(update_bridge_geometry(ctx, target_entries, target_count), TAG,
                        "target geometry");
    ESP_RETURN_ON_ERROR(update_require_immutable_contract(target_entries, target_count), TAG,
                        "target immutable partition contract");

    uint8_t *current_image =
        heap_caps_malloc(FACTORY_SYSTEM_FLASH_SECTOR_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(current_image != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate current partition table");
    esp_err_t ret = esp_flash_read(NULL, current_image, CONFIG_PARTITION_TABLE_OFFSET,
                                   FACTORY_SYSTEM_FLASH_SECTOR_BYTES);
    int current_count = 0;
    if (ret == ESP_OK) {
        ret = esp_partition_table_verify((const esp_partition_info_t *)current_image, true,
                                         &current_count);
    }
    if (ret == ESP_OK) {
        ret = update_require_immutable_contract((const esp_partition_info_t *)current_image,
                                                current_count);
    }
    if (ret == ESP_OK)
        ret =
            update_bridge_geometry(ctx, (const esp_partition_info_t *)current_image, current_count);
    free(current_image);
    ESP_RETURN_ON_ERROR(ret, TAG, "current immutable partition contract");
    ESP_RETURN_ON_ERROR(update_resolve_component_targets(ctx, target_entries, target_count), TAG,
                        "target component mapping");
    if (ctx->transaction.remote_bridge && !ctx->transaction.preserve_layout) {
        for (int i = 0; i < target_count; ++i) {
            if (target_entries[i].magic != ESP_PARTITION_MAGIC ||
                !update_partition_entry_requires_remote_layout_image(&target_entries[i]))
                continue;
            bool supplied = false;
            for (size_t j = 0; j < ctx->transaction.plan_count; ++j)
                supplied |= ctx->transaction.plan[j].target_partition_valid &&
                            ctx->transaction.plan[j].target_partition.address ==
                                target_entries[i].pos.offset;
            ESP_RETURN_ON_FALSE(supplied, ESP_ERR_INVALID_ARG, TAG,
                                "remote layout missing partition image");
        }
    }
    ctx->transaction.target_layout_valid = true;
    return ESP_OK;
}

esp_err_t update_verify_application_on_flash(const esp_iris_system_update_component_t *component,
                                             const esp_partition_t *partition)
{
    esp_partition_pos_t position = {
        .offset = component->target_offset,
        .size = partition->size,
    };
    esp_image_metadata_t metadata = {0};
    return esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
}

#endif
