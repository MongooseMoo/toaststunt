#include "db_json_load.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "collection.h"
#include "db.h"
#include "db_io.h"
#include "db_private.h"
#include "dependencies/yajl/yajl_lex.h"
#include "dependencies/yajl/yajl_parse.h"
#include "list.h"
#include "log.h"
#include "map.h"
#include "parser.h"
#include "server.h"
#include "storage.h"
#include "tasks.h"
#include "utils.h"
#include "version.h"
#include "waif.h"

#define JSON_PROP_MAPPED(Mmap, Mbit) ((Mmap)[(Mbit) / 32] & (1 << ((Mbit) % 32)))
#define JSON_MAP_PROP(Mmap, Mbit) (Mmap)[(Mbit) / 32] |= 1 << ((Mbit) % 32)
#define JSON_N_MAPPABLE_PROPS (WAIF_MAPSZ * 32)

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

struct Json_Document {
    std::vector<std::unique_ptr<Json_Node>> nodes;
    Json_Node *root = nullptr;
};

struct Json_Parse_Context {
    Json_Document *doc;
    std::vector<Json_Node *> stack;
    std::string pending_key;
    bool ok = true;
};

static Json_Node *
new_json_node(Json_Parse_Context *ctx, Json_Kind kind)
{
    ctx->doc->nodes.emplace_back(new Json_Node);
    Json_Node *node = ctx->doc->nodes.back().get();
    node->kind = kind;
    return node;
}

static void
add_json_node(Json_Parse_Context *ctx, Json_Node *node)
{
    if (ctx->stack.empty()) {
        if (ctx->doc->root)
            ctx->ok = false;
        else
            ctx->doc->root = node;
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

static bool
read_file(const char *path, std::string *contents)
{
    FILE *f = std::fopen(path, "rb");
    char buffer[8192];

    if (!f)
        return false;

    contents->clear();
    while (true) {
        size_t n = std::fread(buffer, 1, sizeof(buffer), f);
        if (n)
            contents->append(buffer, n);
        if (n < sizeof(buffer)) {
            bool ok = std::feof(f) != 0;
            std::fclose(f);
            return ok;
        }
    }
}

static bool
parse_json_file(const char *path, Json_Document *doc)
{
    std::string contents;
    if (!read_file(path, &contents))
        return false;

    Json_Parse_Context parse = { doc };
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

    yajl_status status = yajl_parse(hand, (const unsigned char *)contents.data(),
                                    (unsigned int)contents.size());
    if (status == yajl_status_ok)
        status = yajl_parse_complete(hand);
    yajl_free(hand);

    return status == yajl_status_ok && parse.ok && doc->root && parse.stack.empty();
}

static bool
join_path(char *buffer, size_t buffer_size, const char *base, const char *leaf)
{
    return std::snprintf(buffer, buffer_size, "%s/%s", base, leaf) < (int)buffer_size;
}

static Json_Node *
map_get(Json_Node *node, const char *key)
{
    if (!node || node->kind != Json_Kind::Map)
        return nullptr;

    for (auto& item : node->map_items)
        if (item.first == key)
            return item.second;

    return nullptr;
}

static bool
string_value(Json_Node *node, std::string *value)
{
    if (!node || node->kind != Json_Kind::String)
        return false;
    *value = node->string_value;
    return true;
}

static bool
int_value(Json_Node *node, Num *value)
{
    if (!node || node->kind != Json_Kind::Integer)
        return false;
    *value = (Num)node->integer_value;
    return true;
}

static bool
bool_value(Json_Node *node, bool *value)
{
    if (!node || node->kind != Json_Kind::Bool)
        return false;
    *value = node->bool_value;
    return true;
}

static bool
array_value(Json_Node *node, std::vector<Json_Node *> *items)
{
    if (!node || node->kind != Json_Kind::Array)
        return false;
    *items = node->array_items;
    return true;
}

static bool
error_value(const std::string& name, enum error *error)
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

static bool node_to_var(Json_Node *node, Var *value);

static bool
array_to_list(Json_Node *node, Var *value)
{
    std::vector<Json_Node *> items;
    if (!array_value(node, &items))
        return false;

    Var list = new_list((int)items.size());
    for (unsigned int i = 0; i < items.size(); i++) {
        if (!node_to_var(items[i], &list.v.list[i + 1]))
            return false;
    }

    *value = list;
    return true;
}

static bool
array_to_map(Json_Node *node, Var *value)
{
    std::vector<Json_Node *> items;
    if (!array_value(node, &items))
        return false;

    Var map = new_map();
    for (auto *entry : items) {
        Var key;
        Var item_value;
        if (!node_to_var(map_get(entry, "key"), &key)
                || !node_to_var(map_get(entry, "value"), &item_value))
            return false;
        map = mapinsert(map, key, item_value);
    }

    *value = map;
    return true;
}

static bool
node_to_var(Json_Node *node, Var *value)
{
    std::string type;
    if (!string_value(map_get(node, "type"), &type))
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
        Num raw;
        if (!int_value(map_get(node, "value"), &raw))
            return false;
        *value = Var::new_int(raw);
        return true;
    }
    if (type == "float") {
        std::string raw;
        if (!string_value(map_get(node, "value"), &raw))
            return false;
        *value = Var::new_float(std::strtod(raw.c_str(), nullptr));
        return true;
    }
    if (type == "str") {
        std::string raw;
        if (!string_value(map_get(node, "value"), &raw))
            return false;
        *value = str_dup_to_var(raw.c_str());
        return true;
    }
    if (type == "obj") {
        Num raw;
        if (!int_value(map_get(node, "value"), &raw))
            return false;
        *value = Var::new_obj((Objid)raw);
        return true;
    }
    if (type == "err") {
        std::string raw;
        enum error err;
        if (!string_value(map_get(node, "value"), &raw) || !error_value(raw, &err))
            return false;
        value->type = TYPE_ERR;
        value->v.err = err;
        return true;
    }
    if (type == "bool") {
        bool raw;
        if (!bool_value(map_get(node, "value"), &raw))
            return false;
        *value = Var::new_bool(raw);
        return true;
    }
    if (type == "list")
        return array_to_list(map_get(node, "value"), value);
    if (type == "map")
        return array_to_map(map_get(node, "value"), value);
    if (type == "anon_ref") {
        Num raw;
        if (!int_value(map_get(node, "id"), &raw))
            return false;
        *value = dbpriv_read_anonymous_object((Objid)raw);
        return true;
    }
    if (type == "waif_ref") {
        Num raw;
        if (!int_value(map_get(node, "id"), &raw) || raw < 0)
            return false;
        return waif_json_ref((unsigned int)raw, value) != 0;
    }

    return false;
}

static bool
json_file_names(const char *path, std::vector<std::string> *files)
{
    DIR *dir = opendir(path);
    if (!dir)
        return false;

    files->clear();
    while (dirent *entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.size() >= 5 && name.substr(name.size() - 5) == ".json")
            files->push_back(name);
    }
    closedir(dir);
    std::sort(files->begin(), files->end());
    return true;
}

static bool
expect_empty_array_file(const char *path, const char *key)
{
    Json_Document doc;
    std::vector<Json_Node *> items;

    if (!parse_json_file(path, &doc) || !array_value(map_get(doc.root, key), &items))
        return false;
    return items.empty();
}

static bool
read_propval(Json_Node *node, Pval *pval)
{
    Num raw;
    if (!node_to_var(map_get(node, "value"), &pval->var))
        return false;
    if (!int_value(map_get(node, "owner"), &raw))
        return false;
    pval->owner = (Objid)raw;
    if (!int_value(map_get(node, "perms"), &raw))
        return false;
    pval->perms = (short)raw;
    return true;
}

static bool
read_verbdefs(Json_Node *node, Object *object)
{
    std::vector<Json_Node *> items;
    Verbdef **previous = &object->verbdefs;

    if (!array_value(node, &items))
        return false;

    object->verbdefs = nullptr;
    for (auto *item : items) {
        std::string name;
        Num raw;
        Verbdef *verb = (Verbdef *)mymalloc(sizeof(Verbdef), M_VERBDEF);

        if (!string_value(map_get(item, "name"), &name))
            return false;
        verb->name = str_dup(name.c_str());
        if (!int_value(map_get(item, "owner"), &raw))
            return false;
        verb->owner = (Objid)raw;
        if (!int_value(map_get(item, "perms"), &raw))
            return false;
        verb->perms = (short)raw;
        if (!int_value(map_get(item, "prep"), &raw))
            return false;
        verb->prep = (short)raw;
        verb->program = nullptr;
        verb->next = nullptr;

        *previous = verb;
        previous = &verb->next;
    }

    return true;
}

static bool
read_propdefs(Json_Node *node, Object *object)
{
    std::vector<Json_Node *> items;

    if (!array_value(node, &items))
        return false;

    object->propdefs.cur_length = (int)items.size();
    object->propdefs.max_length = (int)items.size();
    object->propdefs.l = nullptr;

    if (!items.empty())
        object->propdefs.l = (Propdef *)mymalloc((unsigned)(items.size() * sizeof(Propdef)), M_PROPDEF);

    for (unsigned int i = 0; i < items.size(); i++) {
        std::string name;
        if (!string_value(items[i], &name))
            return false;
        const char *owned_name = str_dup(name.c_str());
        object->propdefs.l[i] = dbpriv_new_propdef(owned_name);
        free_str(owned_name);
    }

    return true;
}

static bool
read_propvals(Json_Node *node, Object *object)
{
    std::vector<Json_Node *> items;

    if (!array_value(node, &items))
        return false;

    object->nval = (unsigned int)items.size();
    object->propval = nullptr;
    if (!items.empty())
        object->propval = (Pval *)mymalloc((unsigned)(items.size() * sizeof(Pval)), M_PVAL);

    for (unsigned int i = 0; i < items.size(); i++)
        if (!read_propval(items[i], &object->propval[i]))
            return false;

    return true;
}

static bool
read_object(Json_Node *node, Objid expected_id)
{
    Num raw;
    bool recycled = false;

    if (!int_value(map_get(node, "id"), &raw) || (Objid)raw != expected_id)
        return false;
    if (!bool_value(map_get(node, "recycled"), &recycled))
        return false;

    if (recycled) {
        dbpriv_new_recycled_object();
        return true;
    }

    Object *object = dbpriv_new_object(expected_id);
    std::string name;

    if (!string_value(map_get(node, "name"), &name))
        return false;
    object->name = str_dup(name.c_str());

    if (!int_value(map_get(node, "flags"), &raw))
        return false;
    object->flags = (int)raw;
    if (!int_value(map_get(node, "owner"), &raw))
        return false;
    object->owner = (Objid)raw;

    if (!node_to_var(map_get(node, "location"), &object->location))
        return false;
    if (!node_to_var(map_get(node, "last_move"), &object->last_move))
        return false;
    if (!node_to_var(map_get(node, "contents"), &object->contents))
        return false;
    if (!node_to_var(map_get(node, "parents"), &object->parents))
        return false;
    if (!node_to_var(map_get(node, "children"), &object->children))
        return false;

    object->waif_propdefs = nullptr;

    return read_verbdefs(map_get(node, "verbs"), object)
           && read_propdefs(map_get(node, "propdefs"), object)
           && read_propvals(map_get(node, "propvals"), object);
}

static bool
read_object_files(const char *path)
{
    char objects_dir[4096];
    char file_path[4096];
    std::vector<std::string> files;
    Objid expected_id = 0;

    if (!join_path(objects_dir, sizeof(objects_dir), path, "objects"))
        return false;
    if (!json_file_names(objects_dir, &files))
        return false;

    for (const auto& file : files) {
        Json_Document doc;
        std::vector<Json_Node *> objects;
        if (!join_path(file_path, sizeof(file_path), objects_dir, file.c_str()))
            return false;
        if (!parse_json_file(file_path, &doc) || !array_value(doc.root, &objects))
            return false;
        for (auto *object : objects) {
            if (!read_object(object, expected_id))
                return false;
            expected_id++;
        }
    }

    return true;
}

static void
add_anon_to_parent_map(Object *object)
{
    Var parent;
    int i, c;

    if (object->parents.type == TYPE_OBJ && valid(object->parents.v.obj))
        dbpriv_add_anon(object->parents.v.obj, object);
    else if (object->parents.type == TYPE_LIST)
        FOR_EACH(parent, object->parents, i, c)
            if (parent.type == TYPE_OBJ && valid(parent.v.obj))
                dbpriv_add_anon(parent.v.obj, object);
}

static bool
read_anon_record(Json_Node *node)
{
    Num raw;
    std::string name;

    if (!int_value(map_get(node, "id"), &raw))
        return false;
    Var anon = dbpriv_read_anonymous_object((Objid)raw);
    if (anon.v.anon == nullptr)
        return false;

    Object *object = anon.v.anon;

    if (!string_value(map_get(node, "name"), &name))
        return false;
    object->name = str_dup(name.c_str());
    if (!int_value(map_get(node, "flags"), &raw))
        return false;
    object->flags = (int)raw;
    if (!int_value(map_get(node, "owner"), &raw))
        return false;
    object->owner = (Objid)raw;
    if (!node_to_var(map_get(node, "parents"), &object->parents))
        return false;

    object->children = new_list(0);
    object->location = Var::new_obj(NOTHING);
    object->last_move = var_ref(zero);
    object->contents = new_list(0);
    object->verbdefs = nullptr;
    object->waif_propdefs = nullptr;

    if (!read_propdefs(map_get(node, "propdefs"), object)
            || !read_propvals(map_get(node, "propvals"), object))
        return false;

    add_anon_to_parent_map(object);
    return true;
}

static bool
read_anon_files(const char *path)
{
    char anons_dir[4096];
    char file_path[4096];
    std::vector<std::string> files;

    if (!join_path(anons_dir, sizeof(anons_dir), path, "anons"))
        return false;
    if (!json_file_names(anons_dir, &files))
        return false;

    for (const auto& file : files) {
        Json_Document doc;
        std::vector<Json_Node *> anons;
        if (!join_path(file_path, sizeof(file_path), anons_dir, file.c_str()))
            return false;
        if (!parse_json_file(file_path, &doc) || !array_value(doc.root, &anons))
            return false;
        for (auto *anon : anons)
            if (!read_anon_record(anon))
                return false;
    }

    return true;
}

struct Json_Loaded_Waif {
    Json_Node *node;
    Waif *waif;
    Num propdefs_length;
};

static bool
read_waif_shell(Json_Node *node, std::vector<Json_Loaded_Waif> *loaded)
{
    Num raw;
    Num propdefs_length;

    if (!int_value(map_get(node, "id"), &raw)
            || raw < 0
            || (unsigned int)raw != loaded->size())
        return false;

    Waif *waif = (Waif *)mymalloc(sizeof(Waif), M_WAIF);
    if (!int_value(map_get(node, "class"), &raw))
        return false;
    waif->_class = (Objid)raw;
    if (!int_value(map_get(node, "owner"), &raw))
        return false;
    waif->owner = (Objid)raw;
    if (!int_value(map_get(node, "propdefs_length"), &propdefs_length)
            || propdefs_length < 0)
        return false;

    waif->propdefs = nullptr;
    waif->propvals = nullptr;
    for (int i = 0; i < WAIF_MAPSZ; i++)
        waif->map[i] = 0;

    if (!waif_json_register_loaded((unsigned int)loaded->size(), waif))
        return false;

    Json_Loaded_Waif item = { node, waif, propdefs_length };
    loaded->push_back(item);
    return true;
}

static bool
read_waif_values(Json_Loaded_Waif *loaded)
{
    std::vector<Json_Node *> propvals;
    std::vector<Var> values;
    std::vector<unsigned char> present;
    Num mapped_count = 0;
    Num allocated_count;

    if (!array_value(map_get(loaded->node, "propvals"), &propvals))
        return false;

    values.resize((size_t)loaded->propdefs_length);
    present.resize((size_t)loaded->propdefs_length);
    for (Num i = 0; i < loaded->propdefs_length; i++) {
        values[(size_t)i].type = TYPE_CLEAR;
        present[(size_t)i] = 0;
    }

    for (auto *propval : propvals) {
        Num index;
        if (!int_value(map_get(propval, "index"), &index)
                || index < 0
                || index >= loaded->propdefs_length
                || present[(size_t)index])
            return false;
        if (!node_to_var(map_get(propval, "value"), &values[(size_t)index]))
            return false;
        present[(size_t)index] = 1;
        if (index < JSON_N_MAPPABLE_PROPS)
            mapped_count++;
    }

    allocated_count = mapped_count;
    if (loaded->propdefs_length > JSON_N_MAPPABLE_PROPS)
        allocated_count += loaded->propdefs_length - JSON_N_MAPPABLE_PROPS;

    if (allocated_count > 0)
        loaded->waif->propvals = (Var *)mymalloc((unsigned)(allocated_count * sizeof(Var)), M_WAIF_XTRA);

    Var *out = loaded->waif->propvals;
    for (Num i = 0; i < loaded->propdefs_length; i++) {
        if (i < JSON_N_MAPPABLE_PROPS) {
            if (present[(size_t)i]) {
                JSON_MAP_PROP(loaded->waif->map, i);
                *out++ = values[(size_t)i];
            }
        } else {
            *out++ = values[(size_t)i];
        }
    }

    return true;
}

static bool
read_waifs_file(const char *path)
{
    char file_path[4096];
    Json_Document doc;
    std::vector<Json_Node *> waifs;
    std::vector<Json_Loaded_Waif> loaded;

    if (!join_path(file_path, sizeof(file_path), path, "waifs.json"))
        return false;
    if (!parse_json_file(file_path, &doc) || !array_value(map_get(doc.root, "waifs"), &waifs))
        return false;

    for (auto *waif : waifs)
        if (!read_waif_shell(waif, &loaded))
            return false;

    for (unsigned int i = 0; i < loaded.size(); i++)
        if (!read_waif_values(&loaded[i]))
            return false;

    return true;
}

static bool
read_pending_finalization_file(const char *path)
{
    char file_path[4096];
    Json_Document doc;
    std::vector<Json_Node *> pending;

    if (!join_path(file_path, sizeof(file_path), path, "pending-finalization.json"))
        return false;
    if (!parse_json_file(file_path, &doc) || !array_value(map_get(doc.root, "pending"), &pending))
        return false;

    Var values = new_list((int)pending.size());
    for (unsigned int i = 0; i < pending.size(); i++)
        if (!node_to_var(pending[i], &values.v.list[i + 1]))
            return false;

    read_values_pending_finalization_from_json(values);
    return true;
}

static bool
read_active_connections_file(const char *path)
{
    char file_path[4096];
    Json_Document doc;
    std::vector<Json_Node *> connections;

    if (!join_path(file_path, sizeof(file_path), path, "active-connections.json"))
        return false;
    if (!parse_json_file(file_path, &doc) || !array_value(map_get(doc.root, "connections"), &connections))
        return false;

    Var values = new_list((int)connections.size());
    for (unsigned int i = 0; i < connections.size(); i++) {
        Num raw;
        Var connection = new_list(2);

        if (!int_value(map_get(connections[i], "player"), &raw))
            return false;
        connection.v.list[1] = Var::new_obj((Objid)raw);
        if (!int_value(map_get(connections[i], "listener"), &raw))
            return false;
        connection.v.list[2] = Var::new_obj((Objid)raw);
        values.v.list[i + 1] = connection;
    }

    read_active_connections_from_json(values);
    return true;
}

static bool
read_users_file(const char *path)
{
    char file_path[4096];
    Json_Document doc;
    std::vector<Json_Node *> users;

    if (!join_path(file_path, sizeof(file_path), path, "users.json"))
        return false;
    if (!parse_json_file(file_path, &doc) || !array_value(map_get(doc.root, "users"), &users))
        return false;

    Var user_list = new_list((int)users.size());
    for (unsigned int i = 0; i < users.size(); i++) {
        Num raw;
        if (!int_value(users[i], &raw))
            return false;
        user_list.v.list[i + 1] = Var::new_obj((Objid)raw);
    }

    dbpriv_set_all_users(user_list);
    return true;
}

static bool
code_list_from_json(Json_Node *node, Var *code)
{
    std::vector<Json_Node *> lines;
    if (!array_value(node, &lines))
        return false;

    *code = new_list((int)lines.size());
    for (unsigned int i = 0; i < lines.size(); i++) {
        std::string line;
        if (!string_value(lines[i], &line))
            return false;
        code->v.list[i + 1] = str_dup_to_var(line.c_str());
    }

    return true;
}

static bool
read_program_record(Json_Node *node)
{
    Num raw;
    Objid oid;
    Num verb_index;
    Var code;
    Var errors;

    if (!int_value(map_get(node, "object"), &raw))
        return false;
    oid = (Objid)raw;
    if (!valid(oid))
        return false;
    if (!int_value(map_get(node, "verb_index"), &verb_index))
        return false;

    db_verb_handle handle = db_find_indexed_verb(Var::new_obj(oid), (unsigned)verb_index + 1);
    if (!handle.ptr)
        return false;

    if (!code_list_from_json(map_get(node, "code"), &code))
        return false;
    Program *program = parse_list_as_program(code, &errors);
    free_var(code);
    if (!program) {
        errlog("DB_JSON_LOAD: Unparsable program #%" PRIdN ":%" PRIdN ".\n", oid, verb_index);
        free_var(errors);
        return false;
    }

    free_var(errors);
    db_set_verb_program(handle, program);
    return true;
}

static bool
read_program_files(const char *path)
{
    char programs_dir[4096];
    char file_path[4096];
    std::vector<std::string> files;

    if (!join_path(programs_dir, sizeof(programs_dir), path, "programs"))
        return false;
    if (!json_file_names(programs_dir, &files))
        return false;

    for (const auto& file : files) {
        Json_Document doc;
        std::vector<Json_Node *> programs;
        if (!join_path(file_path, sizeof(file_path), programs_dir, file.c_str()))
            return false;
        if (!parse_json_file(file_path, &doc) || !array_value(doc.root, &programs))
            return false;
        for (auto *program : programs)
            if (!read_program_record(program))
                return false;
    }

    return true;
}

static bool
read_manifest(const char *path)
{
    char file_path[4096];
    Json_Document doc;
    std::string format;
    Num raw;

    if (!join_path(file_path, sizeof(file_path), path, "manifest.json"))
        return false;
    if (!parse_json_file(file_path, &doc))
        return false;
    if (!string_value(map_get(doc.root, "format"), &format)
            || format != "toaststunt-json-db")
        return false;
    if (!int_value(map_get(doc.root, "format_version"), &raw) || raw != 20)
        return false;
    if (!int_value(map_get(doc.root, "engine_db_version"), &raw)
            || !check_db_version((DB_Version)raw))
        return false;

    dbio_input_version = (DB_Version)raw;
    return true;
}

static bool
read_task_state_files(const char *path)
{
    char file_path[4096];
    Json_Document doc;
    std::string payload;

    if (!join_path(file_path, sizeof(file_path), path, "tasks/queued.json")
            || !parse_json_file(file_path, &doc)) {
        errlog("DB_JSON_LOAD: Invalid queued task state.\n");
        return false;
    }

    if (string_value(map_get(doc.root, "native_task_queue"), &payload)) {
    } else {
        std::vector<Json_Node *> lines;
        if (!array_value(map_get(doc.root, "native_task_queue_lines"), &lines))
            lines.clear();
        for (auto *line : lines) {
            std::string text;
            if (!string_value(line, &text))
                return false;
            payload += text;
            payload += '\n';
        }
    }

    if (!payload.empty()) {
        FILE *task_file = tmpfile();
        if (!task_file)
            return false;
        if (std::fwrite(payload.data(), 1, payload.size(), task_file) != payload.size()) {
            std::fclose(task_file);
            return false;
        }
        rewind(task_file);
        dbpriv_set_dbio_input(task_file);
        int ok = read_task_queue();
        std::fclose(task_file);
        return ok != 0;
    }

    if (!expect_empty_array_file(file_path, "queued")) {
        errlog("DB_JSON_LOAD: Non-empty or invalid queued task state is not implemented yet.\n");
        return false;
    }
    if (!join_path(file_path, sizeof(file_path), path, "tasks/suspended.json")
            || !expect_empty_array_file(file_path, "suspended")) {
        errlog("DB_JSON_LOAD: Non-empty or invalid suspended task state is not implemented yet.\n");
        return false;
    }
    if (!join_path(file_path, sizeof(file_path), path, "tasks/interrupted.json")
            || !expect_empty_array_file(file_path, "interrupted")) {
        errlog("DB_JSON_LOAD: Non-empty or invalid interrupted task state is not implemented yet.\n");
        return false;
    }

    return true;
}

bool
db_json_load_database(const char *path)
{
    if (!path || path[0] == '\0')
        return false;

    waif_before_loading();

    oklog("LOADING: Reading JSON-v20 manifest ...\n");
    if (!read_manifest(path)) {
        errlog("DB_JSON_LOAD: Bad manifest.\n");
        return false;
    }

    oklog("LOADING: Reading JSON-v20 users ...\n");
    if (!read_users_file(path)) {
        errlog("DB_JSON_LOAD: Bad users file.\n");
        return false;
    }

    oklog("LOADING: Reading JSON-v20 task runtime state ...\n");
    if (!read_task_state_files(path))
        return false;

    oklog("LOADING: Reading JSON-v20 WAIFs ...\n");
    if (!read_waifs_file(path)) {
        errlog("DB_JSON_LOAD: Bad WAIF file.\n");
        return false;
    }

    oklog("LOADING: Reading JSON-v20 pending finalization ...\n");
    if (!read_pending_finalization_file(path)) {
        errlog("DB_JSON_LOAD: Bad pending finalization file.\n");
        return false;
    }

    oklog("LOADING: Reading JSON-v20 active connections ...\n");
    if (!read_active_connections_file(path)) {
        errlog("DB_JSON_LOAD: Bad active connections file.\n");
        return false;
    }

    oklog("LOADING: Reading JSON-v20 objects ...\n");
    if (!read_object_files(path)) {
        errlog("DB_JSON_LOAD: Bad object file.\n");
        return false;
    }

    oklog("LOADING: Reading JSON-v20 anonymous objects ...\n");
    if (!read_anon_files(path)) {
        errlog("DB_JSON_LOAD: Bad anonymous object file.\n");
        return false;
    }

    if (!dbpriv_validate_hierarchies()) {
        errlog("DB_JSON_LOAD: Errors in object hierarchies.\n");
        return false;
    }

    oklog("LOADING: Reading JSON-v20 verb programs ...\n");
    if (!read_program_files(path)) {
        errlog("DB_JSON_LOAD: Bad program file.\n");
        return false;
    }

    dbpriv_after_load();
    waif_after_loading();

    return true;
}
