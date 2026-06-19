#ifndef DB_BACKEND_H
#define DB_BACKEND_H

#define DB_MAX_DUMP_TARGETS 2

enum DB_Backend_Kind {
    DB_BACKEND_NATIVE_TEXT,
    DB_BACKEND_JSON_V20,
    DB_BACKEND_UNSUPPORTED
};

struct DB_Dump_Target {
    DB_Backend_Kind backend;
    const char *path;
};

DB_Backend_Kind db_backend_for_input(const char *path);
DB_Backend_Kind db_backend_for_output(const char *path);

int db_parse_dump_targets(const char *primary_path,
                          const char *secondary_path,
                          DB_Dump_Target *targets,
                          int max_targets);

#endif
