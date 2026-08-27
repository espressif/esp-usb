/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp/tinyusb_mtp_internal.h"
#include "mtp/tinyusb_mtp_codec.h"

#if CONFIG_TINYUSB_MTP_ENABLED

static const char *TAG = "tinyusb_mtp_codec";

const char *mtp_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void mtp_copy_display_name(const char *filename, bool directory, char *buffer, size_t buffer_size)
{
    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    int len = snprintf(buffer, buffer_size, "%s", filename ? filename : "");
    if (len < 0 || len >= (int)buffer_size || directory) {
        return;
    }

    // Name and DisplayName are metadata fields; expose a user-facing stem without changing ObjectFileName.
    char *dot = strrchr(buffer, '.');
    if (dot != NULL && dot != buffer) {
        *dot = '\0';
    }
}

void mtp_make_persistent_uid(const mtp_object_t *object, uint8_t uid[16])
{
    // Windows maps this to a stable WPD object identity during a device session.
    uint32_t words[4] = {
        object->storage ? object->storage->storage_id : 0,
        object->handle,
        (object->storage ? object->storage->storage_id : 0) ^ 0x4d545055U,
        object->handle ^ 0xa5a5a5a5U,
    };
    memcpy(uid, words, sizeof(words));
}

bool mtp_path_is_child_of(const char *path, const char *parent)
{
    size_t parent_len = strlen(parent);
    return strncmp(path, parent, parent_len) == 0 && (path[parent_len] == '/' || path[parent_len] == '\0');
}

char *mtp_join_path(const char *base, const char *name)
{
    const size_t base_len = strlen(base);
    const bool needs_slash = base_len > 0 && base[base_len - 1] != '/';
    const size_t len = base_len + (needs_slash ? 1 : 0) + strlen(name) + 1;
    char *path = malloc(len);
    if (path == NULL) {
        ESP_LOGE(TAG, "failed to allocate path for %s/%s", base, name);
        return NULL;
    }
    snprintf(path, len, "%s%s%s", base, needs_slash ? "/" : "", name);
    return path;
}

char *mtp_make_internal_path(const char *dir, uint32_t handle, const char *prefix)
{
    for (uint32_t attempt = 0; attempt < 16; attempt++) {
        char name[40];
        int len = snprintf(name, sizeof(name), "%s%08" PRIx32 "_%02" PRIu32 ".tmp", prefix, handle, attempt);
        if (len <= 0 || len >= (int)sizeof(name)) {
            ESP_LOGE(TAG, "failed to format MTP internal name");
            return NULL;
        }

        char *path = mtp_join_path(dir, name);
        if (path == NULL) {
            return NULL;
        }
        struct stat st;
        if (stat(path, &st) != 0 && errno == ENOENT) {
            return path;
        }
        free(path);
    }

    ESP_LOGE(TAG, "failed to allocate unique MTP internal path under %s", dir);
    return NULL;
}

bool mtp_name_is_safe(const char *name)
{
    if (name == NULL || name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == '/' || *p == '\\' || *p == ':') {
            return false;
        }
    }
    return true;
}

bool mtp_name_is_internal_temp(const char *name)
{
    return name != NULL && (strncmp(name, MTP_TEMP_NAME_PREFIX, strlen(MTP_TEMP_NAME_PREFIX)) == 0 ||
                            strncmp(name, MTP_BACKUP_NAME_PREFIX, strlen(MTP_BACKUP_NAME_PREFIX)) == 0);
}

uint16_t mtp_format_from_name(const char *name, bool directory)
{
    if (directory) {
        return MTP_OBJ_FORMAT_ASSOCIATION;
    }

    const char *ext = strrchr(name, '.');
    if (ext == NULL) {
        return MTP_OBJ_FORMAT_UNDEFINED;
    }
    ext++;
    if (strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "log") == 0 || strcasecmp(ext, "json") == 0 || strcasecmp(ext, "lua") == 0) {
        return MTP_OBJ_FORMAT_TEXT;
    }
    if (strcasecmp(ext, "png") == 0) {
        return MTP_OBJ_FORMAT_PNG;
    }
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
        return MTP_OBJ_FORMAT_EXIF_JPEG;
    }
    if (strcasecmp(ext, "mp3") == 0) {
        return MTP_OBJ_FORMAT_MP3;
    }
    if (strcasecmp(ext, "wav") == 0) {
        return MTP_OBJ_FORMAT_WAV;
    }
    if (strcasecmp(ext, "mp4") == 0) {
        return MTP_OBJ_FORMAT_MP4;
    }
    return MTP_OBJ_FORMAT_UNDEFINED;
}

static bool mtp_utf8_decode_char(const char **src, uint32_t *codepoint)
{
    const uint8_t *s = (const uint8_t *)*src;
    if (s[0] < 0x80) {
        *codepoint = s[0];
        *src += 1;
        return true;
    }

    uint32_t cp = 0;
    size_t len = 0;
    if ((s[0] & 0xe0) == 0xc0) {
        cp = s[0] & 0x1f;
        len = 2;
        if (cp == 0) {
            return false;
        }
    } else if ((s[0] & 0xf0) == 0xe0) {
        cp = s[0] & 0x0f;
        len = 3;
    } else if ((s[0] & 0xf8) == 0xf0) {
        cp = s[0] & 0x07;
        len = 4;
    } else {
        return false;
    }

    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            return false;
        }
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        return false;
    }

    *codepoint = cp;
    *src += len;
    return true;
}

static bool mtp_utf8_append_codepoint(char *dst, size_t dst_size, size_t *out, uint32_t codepoint)
{
    if (codepoint < 0x80) {
        if (*out + 1 >= dst_size) {
            return false;
        }
        dst[(*out)++] = (char)codepoint;
    } else if (codepoint < 0x800) {
        if (*out + 2 >= dst_size) {
            return false;
        }
        dst[(*out)++] = (char)(0xc0 | (codepoint >> 6));
        dst[(*out)++] = (char)(0x80 | (codepoint & 0x3f));
    } else if (codepoint < 0x10000) {
        if (*out + 3 >= dst_size) {
            return false;
        }
        dst[(*out)++] = (char)(0xe0 | (codepoint >> 12));
        dst[(*out)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        dst[(*out)++] = (char)(0x80 | (codepoint & 0x3f));
    } else {
        if (*out + 4 >= dst_size) {
            return false;
        }
        dst[(*out)++] = (char)(0xf0 | (codepoint >> 18));
        dst[(*out)++] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        dst[(*out)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        dst[(*out)++] = (char)(0x80 | (codepoint & 0x3f));
    }
    return true;
}

static uint32_t mtp_utf8_next_codepoint_or_replacement(const char **src)
{
    const char *start = *src;
    uint32_t codepoint = 0;
    if (mtp_utf8_decode_char(src, &codepoint)) {
        return codepoint;
    }
    *src = start + 1;
    return '_';
}

static size_t mtp_utf8_count_utf16_units(const char *value)
{
    size_t units = 0;
    const char *cursor = value ? value : "";
    while (*cursor && units < UINT8_MAX - 1U) {
        const char *before = cursor;
        uint32_t codepoint = mtp_utf8_next_codepoint_or_replacement(&cursor);
        size_t add_units = codepoint > 0xffff ? 2U : 1U;
        if (units + add_units > UINT8_MAX - 1U) {
            cursor = before;
            break;
        }
        units += add_units;
    }
    return units;
}

uint32_t mtp_container_add_utf8_string(mtp_container_info_t *container, const char *value)
{
    size_t units = mtp_utf8_count_utf16_units(value);
    if (units == 0) {
        return mtp_container_add_uint8(container, 0);
    }

    uint32_t added = mtp_container_add_uint8(container, (uint8_t)units + 1U);
    size_t written_units = 0;
    const char *cursor = value ? value : "";
    while (*cursor && written_units < units) {
        uint32_t codepoint = mtp_utf8_next_codepoint_or_replacement(&cursor);
        if (codepoint > 0xffff) {
            codepoint -= 0x10000;
            added += mtp_container_add_uint16(container, (uint16_t)(0xd800 | (codepoint >> 10)));
            added += mtp_container_add_uint16(container, (uint16_t)(0xdc00 | (codepoint & 0x3ff)));
            written_units += 2U;
        } else {
            added += mtp_container_add_uint16(container, (uint16_t)codepoint);
            written_units++;
        }
    }
    added += mtp_container_add_uint16(container, 0);
    return added;
}

bool mtp_utf8_to_mtp_string_payload(const char *value, uint8_t *payload, size_t payload_size, uint32_t *payload_len)
{
    if (value == NULL || payload == NULL || payload_len == NULL || payload_size < 1) {
        return false;
    }

    size_t units = 0;
    const char *cursor = value;
    while (*cursor) {
        uint32_t codepoint = 0;
        if (!mtp_utf8_decode_char(&cursor, &codepoint)) {
            return false;
        }
        size_t add_units = codepoint > 0xffff ? 2U : 1U;
        if (units + add_units > UINT8_MAX - 1U) {
            return false;
        }
        units += add_units;
    }

    if (units == 0) {
        payload[0] = 0;
        *payload_len = 1;
        return true;
    }

    size_t required = 1U + (units + 1U) * sizeof(uint16_t);
    if (required > payload_size) {
        return false;
    }
    payload[0] = (uint8_t)units + 1U;

    size_t out = 1;
    cursor = value;
    while (*cursor) {
        uint32_t codepoint = 0;
        if (!mtp_utf8_decode_char(&cursor, &codepoint)) {
            return false;
        }
        if (codepoint > 0xffff) {
            codepoint -= 0x10000;
            uint16_t high = (uint16_t)(0xd800 | (codepoint >> 10));
            uint16_t low = (uint16_t)(0xdc00 | (codepoint & 0x3ff));
            payload[out++] = (uint8_t)high;
            payload[out++] = (uint8_t)(high >> 8);
            payload[out++] = (uint8_t)low;
            payload[out++] = (uint8_t)(low >> 8);
        } else {
            payload[out++] = (uint8_t)codepoint;
            payload[out++] = (uint8_t)(codepoint >> 8);
        }
    }
    payload[out++] = 0;
    payload[out++] = 0;
    *payload_len = (uint32_t)out;
    return true;
}

bool mtp_utf16_to_utf8_name(const uint8_t *src, size_t src_size, char *dst, size_t dst_size)
{
    if (src == NULL || dst == NULL || dst_size == 0 || src_size < 1) {
        return false;
    }

    uint8_t count = *src++;
    if (count == 0) {
        dst[0] = '\0';
        return false;
    }
    if ((size_t)count > (src_size - 1) / sizeof(uint16_t)) {
        ESP_LOGW(TAG, "truncated MTP UTF-16 object name");
        return false;
    }

    size_t out = 0;
    for (uint8_t i = 0; i + 1 < count; i++) {
        uint16_t ch = (uint16_t)src[i * 2U] | ((uint16_t)src[i * 2U + 1U] << 8);
        if (ch == 0) {
            break;
        }

        uint32_t codepoint = ch;
        if (ch >= 0xd800 && ch <= 0xdbff) {
            if (i + 2U >= count) {
                return false;
            }
            uint16_t low = (uint16_t)src[(i + 1U) * 2U] | ((uint16_t)src[(i + 1U) * 2U + 1U] << 8);
            if (low < 0xdc00 || low > 0xdfff) {
                return false;
            }
            codepoint = 0x10000 + (((uint32_t)ch - 0xd800) << 10) + ((uint32_t)low - 0xdc00);
            i++;
        } else if (ch >= 0xdc00 && ch <= 0xdfff) {
            return false;
        }

        if (!mtp_utf8_append_codepoint(dst, dst_size, &out, codepoint)) {
            ESP_LOGW(TAG, "MTP UTF-16 object name is too long after UTF-8 conversion");
            return false;
        }
    }
    dst[out] = '\0';
    return mtp_name_is_safe(dst);
}

void mtp_time_to_date_string(time_t value, char *buf, size_t buf_size)
{
    if (buf_size == 0) {
        return;
    }
    buf[0] = '\0';
    if (value <= 0) {
        return;
    }

    struct tm tm_value;
    if (gmtime_r(&value, &tm_value) == NULL) {
        return;
    }
    if (strftime(buf, buf_size, "%Y%m%dT%H%M%S", &tm_value) == 0) {
        buf[0] = '\0';
    }
}

static bool mtp_builder_reserve(mtp_payload_builder_t *builder, uint32_t add_len)
{
    if (builder->len > UINT32_MAX - add_len) {
        ESP_LOGE(TAG, "MTP payload builder length overflow");
        return false;
    }
    uint32_t need = builder->len + add_len;
    if (builder->measure_only) {
        return true;
    }
    if (need <= builder->cap) {
        return true;
    }

    uint32_t new_cap = builder->cap ? builder->cap : 64;
    while (new_cap < need) {
        if (new_cap > UINT32_MAX / 2U) {
            new_cap = need;
            break;
        }
        new_cap *= 2U;
    }

    uint8_t *new_data = realloc(builder->data, new_cap);
    if (new_data == NULL) {
        ESP_LOGE(TAG, "failed to grow MTP payload to %" PRIu32 " bytes", new_cap);
        return false;
    }
    builder->data = new_data;
    builder->cap = new_cap;
    return true;
}

bool mtp_builder_append_raw(mtp_payload_builder_t *builder, const void *data, uint32_t len)
{
    if (!mtp_builder_reserve(builder, len)) {
        return false;
    }
    if (!builder->measure_only) {
        memcpy(builder->data + builder->len, data, len);
    }
    builder->len += len;
    return true;
}

bool mtp_builder_append_uint8(mtp_payload_builder_t *builder, uint8_t value)
{
    return mtp_builder_append_raw(builder, &value, sizeof(value));
}

bool mtp_builder_append_uint16(mtp_payload_builder_t *builder, uint16_t value)
{
    return mtp_builder_append_raw(builder, &value, sizeof(value));
}

bool mtp_builder_append_uint32(mtp_payload_builder_t *builder, uint32_t value)
{
    return mtp_builder_append_raw(builder, &value, sizeof(value));
}

bool mtp_builder_append_uint64(mtp_payload_builder_t *builder, uint64_t value)
{
    return mtp_builder_append_raw(builder, &value, sizeof(value));
}

bool mtp_builder_append_cstring(mtp_payload_builder_t *builder, const char *value)
{
    size_t len = strlen(value);
    if (len >= UINT8_MAX) {
        len = UINT8_MAX - 1U;
    }
    if (len == 0) {
        return mtp_builder_append_uint8(builder, 0);
    }
    uint8_t count = (uint8_t)len + 1U;
    if (!mtp_builder_append_uint8(builder, count)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        uint16_t ch = i < len ? (uint8_t)value[i] : 0;
        if (!mtp_builder_append_uint16(builder, ch)) {
            return false;
        }
    }
    return true;
}

bool mtp_builder_append_utf8_string(mtp_payload_builder_t *builder, const char *value)
{
    size_t units = mtp_utf8_count_utf16_units(value);
    if (units == 0) {
        return mtp_builder_append_uint8(builder, 0);
    }
    if (!mtp_builder_append_uint8(builder, (uint8_t)units + 1U)) {
        return false;
    }

    size_t written_units = 0;
    const char *cursor = value ? value : "";
    while (*cursor && written_units < units) {
        uint32_t codepoint = mtp_utf8_next_codepoint_or_replacement(&cursor);
        if (codepoint > 0xffff) {
            codepoint -= 0x10000;
            if (!mtp_builder_append_uint16(builder, (uint16_t)(0xd800 | (codepoint >> 10))) ||
                    !mtp_builder_append_uint16(builder, (uint16_t)(0xdc00 | (codepoint & 0x3ff)))) {
                return false;
            }
            written_units += 2U;
        } else if (!mtp_builder_append_uint16(builder, (uint16_t)codepoint)) {
            return false;
        } else {
            written_units++;
        }
    }
    return mtp_builder_append_uint16(builder, 0);
}


#endif
