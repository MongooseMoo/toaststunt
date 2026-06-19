#include "db_backend.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

static bool
ends_with(const char *path, const char *suffix)
{
    if (!path || !suffix)
        return false;

    size_t path_len = std::strlen(path);
    size_t suffix_len = std::strlen(suffix);

    if (path_len < suffix_len)
        return false;

    return std::strcmp(path + path_len - suffix_len, suffix) == 0;
}

static bool
is_directory(const char *path)
{
    struct stat st;

    if (!path || stat(path, &st) != 0)
        return false;

    return S_ISDIR(st.st_mode);
}

static bool
read_file(const char *path, char *buffer, size_t buffer_size)
{
    FILE *f;
    size_t nread;

    if (!path || !buffer || buffer_size == 0)
        return false;

    f = std::fopen(path, "rb");
    if (!f)
        return false;

    nread = std::fread(buffer, 1, buffer_size - 1, f);
    buffer[nread] = '\0';

    std::fclose(f);
    return true;
}

static DB_Backend_Kind
json_manifest_backend(const char *dir)
{
    char manifest_path[4096];
    char manifest[4096];

    if (std::snprintf(manifest_path, sizeof(manifest_path),
                      "%s/manifest.json", dir) >= (int)sizeof(manifest_path))
        return DB_BACKEND_UNSUPPORTED;

    if (!read_file(manifest_path, manifest, sizeof(manifest)))
        return DB_BACKEND_UNSUPPORTED;

    if (!std::strstr(manifest, "\"format\"")
            || !std::strstr(manifest, "\"toaststunt-json-db\""))
        return DB_BACKEND_UNSUPPORTED;

    const char *version_key = std::strstr(manifest, "\"format_version\"");
    if (!version_key)
        return DB_BACKEND_UNSUPPORTED;

    const char *colon = std::strchr(version_key, ':');
    if (!colon)
        return DB_BACKEND_UNSUPPORTED;

    char *end = nullptr;
    long version = std::strtol(colon + 1, &end, 10);
    if (end != colon + 1 && version == 20)
        return DB_BACKEND_JSON_V20;

    return DB_BACKEND_UNSUPPORTED;
}

DB_Backend_Kind
db_backend_for_input(const char *path)
{
    if (is_directory(path))
        return json_manifest_backend(path);

    return DB_BACKEND_NATIVE_TEXT;
}

DB_Backend_Kind
db_backend_for_output(const char *path)
{
    if (ends_with(path, ".v20"))
        return DB_BACKEND_JSON_V20;

    return DB_BACKEND_NATIVE_TEXT;
}

int
db_parse_dump_targets(const char *primary_path,
                      const char *secondary_path,
                      DB_Dump_Target *targets,
                      int max_targets)
{
    int count = 0;

    if (!primary_path || !targets || max_targets <= 0)
        return 0;

    targets[count].path = primary_path;
    targets[count].backend = db_backend_for_output(primary_path);
    count++;

    if (secondary_path && secondary_path[0] != '\0' && count < max_targets) {
        targets[count].path = secondary_path;
        targets[count].backend = DB_BACKEND_JSON_V20;
        count++;
    }

    return count;
}
