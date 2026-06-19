#include "db_json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "dependencies/yajl/yajl_gen.h"
#include "dependencies/yajl/yajl_lex.h"
#include "dependencies/yajl/yajl_parse.h"
#include "map.h"
#include "storage.h"

static const char *
db_json_error_name(enum error e)
{
    switch (e) {
        case E_NONE: return "E_NONE";
        case E_TYPE: return "E_TYPE";
        case E_DIV: return "E_DIV";
        case E_PERM: return "E_PERM";
        case E_PROPNF: return "E_PROPNF";
        case E_VERBNF: return "E_VERBNF";
        case E_VARNF: return "E_VARNF";
        case E_INVIND: return "E_INVIND";
        case E_RECMOVE: return "E_RECMOVE";
        case E_MAXREC: return "E_MAXREC";
        case E_RANGE: return "E_RANGE";
        case E_ARGS: return "E_ARGS";
        case E_NACC: return "E_NACC";
        case E_INVARG: return "E_INVARG";
        case E_QUOTA: return "E_QUOTA";
        case E_FLOAT: return "E_FLOAT";
        case E_FILE: return "E_FILE";
        case E_EXEC: return "E_EXEC";
        case E_INTRPT: return "E_INTRPT";
    }

    return "E_NONE";
}

static bool
db_json_error_value(const std::string& name, enum error *error)
{
    struct Error_Name {
        const char *name;
        enum error error;
    };

    static const Error_Name names[] = {
        { "E_NONE", E_NONE }, { "E_TYPE", E_TYPE }, { "E_DIV", E_DIV },
        { "E_PERM", E_PERM }, { "E_PROPNF", E_PROPNF }, { "E_VERBNF", E_VERBNF },
        { "E_VARNF", E_VARNF }, { "E_INVIND", E_INVIND }, { "E_RECMOVE", E_RECMOVE },
        { "E_MAXREC", E_MAXREC }, { "E_RANGE", E_RANGE }, { "E_ARGS", E_ARGS },
        { "E_NACC", E_NACC }, { "E_INVARG", E_INVARG }, { "E_QUOTA", E_QUOTA },
        { "E_FLOAT", E_FLOAT }, { "E_FILE", E_FILE }, { "E_EXEC", E_EXEC },
        { "E_INTRPT", E_INTRPT },
    };

    for (const auto& item : names) {
        if (name == item.name) {
            *error = item.error;
            return true;
        }
    }

    return false;
}

enum class Json_Kind {
    Map,
    Array,
    String,
    Integer,
    Bool,
    Null,
};

struct Json_Node {
    Json_Kind kind;
    std::string string_value;
    long long integer_value = 0;
    bool bool_value = false;
    std::vector<std::pair<std::string, Json_Node *>> map_items;
    std::vector<Json_Node *> array_items;
};

struct Json_Parse_Context {
    std::vector<std::unique_ptr<Json_Node>> nodes;
    std::vector<Json_Node *> stack;
    std::string pending_key;
    Json_Node *root = nullptr;
    bool ok = true;
};

static Json_Node *
new_json_node(Json_Parse_Context *ctx, Json_Kind kind)
{
    ctx->nodes.emplace_back(new Json_Node);
    Json_Node *node = ctx->nodes.back().get();
    node->kind = kind;
    return node;
}

static void
add_json_node(Json_Parse_Context *ctx, Json_Node *node)
{
    if (ctx->stack.empty()) {
        if (ctx->root)
            ctx->ok = false;
        else
            ctx->root = node;
        return;
    }

    Json_Node *parent = ctx->stack.back();
    if (parent->kind == Json_Kind::Array) {
        parent->array_items.push_back(node);
    } else if (parent->kind == Json_Kind::Map && !ctx->pending_key.empty()) {
        parent->map_items.push_back(std::make_pair(ctx->pending_key, node));
        ctx->pending_key.clear();
    } else {
        ctx->ok = false;
    }
}

static int
json_null(void *ctx)
{
    Json_Parse_Context *parse = (Json_Parse_Context *)ctx;
    add_json_node(parse, new_json_node(parse, Json_Kind::Null));
    return parse->ok;
}

static int
json_bool(void *ctx, int value)
{
    Json_Parse_Context *parse = (Json_Parse_Context *)ctx;
    Json_Node *node = new_json_node(parse, Json_Kind::Bool);
    node->bool_value = value != 0;
    add_json_node(parse, node);
    return parse->ok;
}

static int
json_integer(void *ctx, long value)
{
    Json_Parse_Context *parse = (Json_Parse_Context *)ctx;
    Json_Node *node = new_json_node(parse, Json_Kind::Integer);
    node->integer_value = value;
    add_json_node(parse, node);
    return parse->ok;
}

static int
json_string(void *ctx, const unsigned char *value, unsigned int len)
{
    Json_Parse_Context *parse = (Json_Parse_Context *)ctx;
    Json_Node *node = new_json_node(parse, Json_Kind::String);
    node->string_value.assign((const char *)value, len);
    add_json_node(parse, node);
    return parse->ok;
}

static int
json_map_key(void *ctx, const unsigned char *key, unsigned int len)
{
    Json_Parse_Context *parse = (Json_Parse_Context *)ctx;
    parse->pending_key.assign((const char *)key, len);
    return 1;
}

static int
json_start_map(void *ctx)
{
    Json_Parse_Context *parse = (Json_Parse_Context *)ctx;
    Json_Node *node = new_json_node(parse, Json_Kind::Map);
    add_json_node(parse, node);
    parse->stack.push_back(node);
    return parse->ok;
}

static int
json_end_map(void *ctx)
{
    Json_Parse_Context *parse = (Json_Parse_Context *)ctx;
    if (parse->stack.empty() || parse->stack.back()->kind != Json_Kind::Map)
        parse->ok = false;
    else
        parse->stack.pop_back();
    return parse->ok;
}

static int
json_start_array(void *ctx)
{
    Json_Parse_Context *parse = (Json_Parse_Context *)ctx;
    Json_Node *node = new_json_node(parse, Json_Kind::Array);
    add_json_node(parse, node);
    parse->stack.push_back(node);
    return parse->ok;
}

static int
json_end_array(void *ctx)
{
    Json_Parse_Context *parse = (Json_Parse_Context *)ctx;
    if (parse->stack.empty() || parse->stack.back()->kind != Json_Kind::Array)
        parse->ok = false;
    else
        parse->stack.pop_back();
    return parse->ok;
}

static Json_Node *
json_map_get(Json_Node *node, const char *key)
{
    if (!node || node->kind != Json_Kind::Map)
        return nullptr;

    for (auto& item : node->map_items)
        if (item.first == key)
            return item.second;

    return nullptr;
}

static bool
json_string_value(Json_Node *node, std::string *value)
{
    if (!node || node->kind != Json_Kind::String)
        return false;
    *value = node->string_value;
    return true;
}

static Var
db_json_new_list(int size)
{
    Var list;
    Var *ptr = (Var *)mymalloc((size + 1) * sizeof(Var), M_LIST);
    list.type = TYPE_LIST;
    list.v.list = ptr;
    list.v.list[0].type = TYPE_INT;
    list.v.list[0].v.num = size;
    return list;
}

static bool db_json_node_to_var(Json_Node *node, Var *value);

static bool
db_json_array_to_list(Json_Node *node, Var *value)
{
    if (!node || node->kind != Json_Kind::Array)
        return false;

    Var list = db_json_new_list((int)node->array_items.size());
    for (unsigned int i = 0; i < node->array_items.size(); i++) {
        if (!db_json_node_to_var(node->array_items[i], &list.v.list[i + 1]))
            return false;
    }

    *value = list;
    return true;
}

static bool
db_json_array_to_map(Json_Node *node, Var *value)
{
    if (!node || node->kind != Json_Kind::Array)
        return false;

    Var map = new_map();
    for (auto *entry : node->array_items) {
        Var key;
        Var item_value;
        if (!db_json_node_to_var(json_map_get(entry, "key"), &key)
                || !db_json_node_to_var(json_map_get(entry, "value"), &item_value))
            return false;
        map = mapinsert(map, key, item_value);
    }

    *value = map;
    return true;
}

static bool
db_json_node_to_var(Json_Node *node, Var *value)
{
    std::string type;
    if (!json_string_value(json_map_get(node, "type"), &type))
        return false;

    if (type == "none") {
        value->type = TYPE_NONE;
        return true;
    }
    if (type == "clear") {
        value->type = TYPE_CLEAR;
        return true;
    }
    if (type == "int") {
        Json_Node *raw = json_map_get(node, "value");
        if (!raw || raw->kind != Json_Kind::Integer)
            return false;
        *value = Var::new_int((Num)raw->integer_value);
        return true;
    }
    if (type == "float") {
        std::string raw;
        if (!json_string_value(json_map_get(node, "value"), &raw))
            return false;
        *value = Var::new_float(std::strtod(raw.c_str(), nullptr));
        return true;
    }
    if (type == "str") {
        std::string raw;
        if (!json_string_value(json_map_get(node, "value"), &raw))
            return false;
        *value = str_dup_to_var(raw.c_str());
        return true;
    }
    if (type == "obj") {
        Json_Node *raw = json_map_get(node, "value");
        if (!raw || raw->kind != Json_Kind::Integer)
            return false;
        *value = Var::new_obj((Objid)raw->integer_value);
        return true;
    }
    if (type == "err") {
        std::string raw;
        enum error err;
        if (!json_string_value(json_map_get(node, "value"), &raw)
                || !db_json_error_value(raw, &err))
            return false;
        value->type = TYPE_ERR;
        value->v.err = err;
        return true;
    }
    if (type == "bool") {
        Json_Node *raw = json_map_get(node, "value");
        if (!raw || raw->kind != Json_Kind::Bool)
            return false;
        *value = Var::new_bool(raw->bool_value);
        return true;
    }
    if (type == "list")
        return db_json_array_to_list(json_map_get(node, "value"), value);
    if (type == "map")
        return db_json_array_to_map(json_map_get(node, "value"), value);

    return false;
}

static yajl_gen_status
gen_string(yajl_gen g, const char *s)
{
    return yajl_gen_string(g, (const unsigned char *)s, std::strlen(s));
}

static bool
gen_type(yajl_gen g, const char *type)
{
    return gen_string(g, "type") == yajl_gen_status_ok
           && gen_string(g, type) == yajl_gen_status_ok;
}

static bool
gen_value_key(yajl_gen g)
{
    return gen_string(g, "value") == yajl_gen_status_ok;
}

struct Json_Map_Generate_Data {
    yajl_gen g;
    bool ok;
};

static int
db_json_generate_map_item(Var key, Var value, void *data, int first)
{
    Json_Map_Generate_Data *map_data = (Json_Map_Generate_Data *)data;

    if (!map_data->ok)
        return 1;

    map_data->ok = yajl_gen_map_open(map_data->g) == yajl_gen_status_ok
                   && gen_string(map_data->g, "key") == yajl_gen_status_ok
                   && db_json_generate_var(map_data->g, key)
                   && gen_string(map_data->g, "value") == yajl_gen_status_ok
                   && db_json_generate_var(map_data->g, value)
                   && yajl_gen_map_close(map_data->g) == yajl_gen_status_ok;

    return map_data->ok ? 0 : 1;
}

static bool
db_json_generate_map(yajl_gen g, Var value)
{
    Json_Map_Generate_Data data = { g, true };

    if (!gen_type(g, "map") || !gen_value_key(g)
            || yajl_gen_array_open(g) != yajl_gen_status_ok)
        return false;
    if (mapforeach(value, db_json_generate_map_item, &data) || !data.ok)
        return false;
    return yajl_gen_array_close(g) == yajl_gen_status_ok;
}

bool
db_json_generate_var(yajl_gen g, Var value)
{
    char float_buffer[64];

    if (value.type == TYPE_ANON || value.type == TYPE_WAIF)
        return false;

    if (yajl_gen_map_open(g) != yajl_gen_status_ok)
        return false;

    switch (value.type) {
        case TYPE_NONE:
            if (!gen_type(g, "none"))
                return false;
            break;
        case TYPE_CLEAR:
            if (!gen_type(g, "clear"))
                return false;
            break;
        case TYPE_INT:
            if (!gen_type(g, "int") || !gen_value_key(g)
                    || yajl_gen_integer(g, value.v.num) != yajl_gen_status_ok)
                return false;
            break;
        case TYPE_FLOAT:
            std::snprintf(float_buffer, sizeof(float_buffer), "%.17g", value.v.fnum);
            if (!gen_type(g, "float") || !gen_value_key(g)
                    || gen_string(g, float_buffer) != yajl_gen_status_ok)
                return false;
            break;
        case TYPE_STR:
            if (!gen_type(g, "str") || !gen_value_key(g)
                    || gen_string(g, value.v.str ? value.v.str : "") != yajl_gen_status_ok)
                return false;
            break;
        case TYPE_OBJ:
            if (!gen_type(g, "obj") || !gen_value_key(g)
                    || yajl_gen_integer(g, value.v.obj) != yajl_gen_status_ok)
                return false;
            break;
        case TYPE_ERR:
            if (!gen_type(g, "err") || !gen_value_key(g)
                    || gen_string(g, db_json_error_name(value.v.err)) != yajl_gen_status_ok)
                return false;
            break;
        case TYPE_BOOL:
            if (!gen_type(g, "bool") || !gen_value_key(g)
                    || yajl_gen_bool(g, value.v.truth) != yajl_gen_status_ok)
                return false;
            break;
        case TYPE_LIST:
            if (!gen_type(g, "list") || !gen_value_key(g)
                    || yajl_gen_array_open(g) != yajl_gen_status_ok)
                return false;
            for (int i = 1; i <= value.v.list[0].v.num; i++)
                if (!db_json_generate_var(g, value.v.list[i]))
                    return false;
            if (yajl_gen_array_close(g) != yajl_gen_status_ok)
                return false;
            break;
        case TYPE_MAP:
            if (!db_json_generate_map(g, value))
                return false;
            break;
        default:
            return false;
    }

    return yajl_gen_map_close(g) == yajl_gen_status_ok;
}

bool
db_json_parse_var(const char *json, Var *value)
{
    if (!json || !value)
        return false;

    Json_Parse_Context parse;
    yajl_callbacks callbacks = {
        json_null,
        json_bool,
        json_integer,
        nullptr,
        nullptr,
        json_string,
        json_start_map,
        json_map_key,
        json_end_map,
        json_start_array,
        json_end_array
    };
    yajl_parser_config cfg = { 1, 1 };
    yajl_handle hand = yajl_alloc(&callbacks, &cfg, nullptr, &parse);
    if (!hand)
        return false;

    size_t len = std::strlen(json);
    yajl_status status = yajl_parse(hand, (const unsigned char *)json, (unsigned int)len);
    if (status == yajl_status_ok)
        status = yajl_parse_complete(hand);

    yajl_free(hand);

    return status == yajl_status_ok
           && parse.ok
           && parse.root
           && parse.stack.empty()
           && db_json_node_to_var(parse.root, value);
}

static bool
make_dir(const char *path)
{
    return mkdir(path, 0700) == 0;
}

static bool
join_path(char *buffer, size_t buffer_size, const char *base, const char *leaf)
{
    return std::snprintf(buffer, buffer_size, "%s/%s", base, leaf) < (int)buffer_size;
}

static bool
write_bytes(const char *path, const unsigned char *bytes, unsigned int len)
{
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;

    bool ok = std::fwrite(bytes, 1, len, f) == len;
    if (std::fclose(f) != 0)
        ok = false;

    return ok;
}

static bool
write_empty_array_file(const char *path, const char *key)
{
    yajl_gen_config cfg = { 0, "", 0 };
    yajl_gen g = yajl_gen_alloc(&cfg, nullptr);
    const unsigned char *buf;
    unsigned int len;
    bool ok;

    if (!g)
        return false;

    ok = yajl_gen_map_open(g) == yajl_gen_status_ok
         && yajl_gen_string(g, (const unsigned char *)key, std::strlen(key)) == yajl_gen_status_ok
         && yajl_gen_array_open(g) == yajl_gen_status_ok
         && yajl_gen_array_close(g) == yajl_gen_status_ok
         && yajl_gen_map_close(g) == yajl_gen_status_ok;

    if (ok) {
        yajl_gen_get_buf(g, &buf, &len);
        ok = write_bytes(path, buf, len);
    }

    yajl_gen_clear(g);
    yajl_gen_free(g);

    return ok;
}

static bool
write_manifest(const char *path, int engine_db_version)
{
    yajl_gen_config cfg = { 0, "", 0 };
    yajl_gen g = yajl_gen_alloc(&cfg, nullptr);
    const unsigned char *buf;
    unsigned int len;
    bool ok = true;

    if (!g)
        return false;

    const char *format_key = "format";
    const char *format_value = "toaststunt-json-db";
    const char *format_version_key = "format_version";
    const char *engine_db_version_key = "engine_db_version";
    const char *chunk_size_key = "chunk_size";

#define CHECK_YAJL(expr) \
    do { \
        if ((expr) != yajl_gen_status_ok) \
            ok = false; \
    } while (0)

    CHECK_YAJL(yajl_gen_map_open(g));
    CHECK_YAJL(yajl_gen_string(g, (const unsigned char *)format_key, std::strlen(format_key)));
    CHECK_YAJL(yajl_gen_string(g, (const unsigned char *)format_value, std::strlen(format_value)));
    CHECK_YAJL(yajl_gen_string(g, (const unsigned char *)format_version_key, std::strlen(format_version_key)));
    CHECK_YAJL(yajl_gen_integer(g, 20));
    CHECK_YAJL(yajl_gen_string(g, (const unsigned char *)engine_db_version_key, std::strlen(engine_db_version_key)));
    CHECK_YAJL(yajl_gen_integer(g, engine_db_version));
    CHECK_YAJL(yajl_gen_string(g, (const unsigned char *)chunk_size_key, std::strlen(chunk_size_key)));
    CHECK_YAJL(yajl_gen_integer(g, 1000));
    CHECK_YAJL(yajl_gen_map_close(g));

#undef CHECK_YAJL

    if (ok) {
        yajl_gen_get_buf(g, &buf, &len);
        ok = write_bytes(path, buf, len);
    }

    yajl_gen_clear(g);
    yajl_gen_free(g);

    return ok;
}

bool
db_json_write_skeleton_dump(const char *path, int engine_db_version)
{
    char temp_path[4096];
    char child_path[4096];

    if (!path || path[0] == '\0')
        return false;

    if (std::snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld",
                      path, (long)getpid()) >= (int)sizeof(temp_path))
        return false;

    if (!make_dir(temp_path))
        return false;

    const char *dirs[] = { "objects", "anons", "programs", "tasks" };
    for (unsigned int i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (!join_path(child_path, sizeof(child_path), temp_path, dirs[i]))
            return false;
        if (!make_dir(child_path))
            return false;
    }

    if (!join_path(child_path, sizeof(child_path), temp_path, "manifest.json"))
        return false;
    if (!write_manifest(child_path, engine_db_version))
        return false;

    if (!join_path(child_path, sizeof(child_path), temp_path, "pending-finalization.json"))
        return false;
    if (!write_empty_array_file(child_path, "pending"))
        return false;

    if (!join_path(child_path, sizeof(child_path), temp_path, "active-connections.json"))
        return false;
    if (!write_empty_array_file(child_path, "connections"))
        return false;

    if (!join_path(child_path, sizeof(child_path), temp_path, "waifs.json"))
        return false;
    if (!write_empty_array_file(child_path, "waifs"))
        return false;

    if (!join_path(child_path, sizeof(child_path), temp_path, "tasks/queued.json"))
        return false;
    if (!write_empty_array_file(child_path, "queued"))
        return false;

    if (!join_path(child_path, sizeof(child_path), temp_path, "tasks/suspended.json"))
        return false;
    if (!write_empty_array_file(child_path, "suspended"))
        return false;

    if (!join_path(child_path, sizeof(child_path), temp_path, "tasks/interrupted.json"))
        return false;
    if (!write_empty_array_file(child_path, "interrupted"))
        return false;

    return rename(temp_path, path) == 0;
}

bool
db_json_var_to_json(Var value, std::string *json)
{
    yajl_gen_config cfg = { 0, "", 1 };
    yajl_gen g = yajl_gen_alloc(&cfg, nullptr);
    const unsigned char *buf;
    unsigned int len;

    if (!json || !g)
        return false;

    if (!db_json_generate_var(g, value)) {
        yajl_gen_clear(g);
        yajl_gen_free(g);
        return false;
    }

    yajl_gen_get_buf(g, &buf, &len);
    json->assign((const char *)buf, len);

    yajl_gen_clear(g);
    yajl_gen_free(g);

    return true;
}
