#ifndef DB_JSON_H
#define DB_JSON_H

#include <string>

#include "yajl/yajl_gen.h"
#include "structures.h"

bool db_json_write_skeleton_dump(const char *path, int engine_db_version);
bool db_json_generate_var(yajl_gen g, Var value);
bool db_json_var_to_json(Var value, std::string *json);
bool db_json_parse_var(const char *json, Var *value);

#endif
