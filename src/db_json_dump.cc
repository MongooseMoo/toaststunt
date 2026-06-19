#include "db_json_dump.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "db.h"
#include "db_json.h"
#include "db_private.h"
#include "map.h"
#include "server.h"
#include "tasks.h"
#include "unparse.h"
#include "utils.h"
#include "waif.h"

#define JSON_PROP_MAPPED(Mmap, Mbit) ((Mmap)[(Mbit) / 32] & (1 << ((Mbit) % 32)))
#define JSON_N_MAPPABLE_PROPS (WAIF_MAPSZ * 32)

static yajl_gen_status
gen_string(yajl_gen g, const char *s)
{
    return yajl_gen_string(g, (const unsigned char *)s, std::strlen(s));
}

static bool
gen_key(yajl_gen g, const char *key)
{
    return gen_string(g, key) == yajl_gen_status_ok;
}

struct Json_Dump_Context {
    std::unordered_map<Object *, Objid> anon_ids;
    std::vector<Object *> anons;
    std::unordered_map<Waif *, unsigned int> waif_ids;
    std::vector<Waif *> waifs;
};

static bool write_db_var(yajl_gen g, Var value, Json_Dump_Context *context);
static void collect_db_var(Var value, Json_Dump_Context *context);

struct Map_Write_Data {
    yajl_gen g;
    Json_Dump_Context *context;
    bool ok;
};

static int
write_map_item(Var key, Var value, void *data, int first)
{
    Map_Write_Data *map_data = (Map_Write_Data *)data;

    if (!map_data->ok)
        return 1;

    map_data->ok = yajl_gen_map_open(map_data->g) == yajl_gen_status_ok
                   && gen_key(map_data->g, "key")
                   && write_db_var(map_data->g, key, map_data->context)
                   && gen_key(map_data->g, "value")
                   && write_db_var(map_data->g, value, map_data->context)
                   && yajl_gen_map_close(map_data->g) == yajl_gen_status_ok;

    return map_data->ok ? 0 : 1;
}

static Objid
anon_id_for_value(Var value, Json_Dump_Context *context)
{
    if (!is_valid(value))
        return NOTHING;

    Object *object = value.v.anon;
    auto found = context->anon_ids.find(object);
    if (found != context->anon_ids.end())
        return found->second;

    Objid oid = dbpriv_assign_anonymous_object(object);
    context->anon_ids[object] = oid;
    context->anons.push_back(object);
    return oid;
}

static bool
write_anon_ref(yajl_gen g, Var value, Json_Dump_Context *context)
{
    Objid oid = anon_id_for_value(value, context);

    return yajl_gen_map_open(g) == yajl_gen_status_ok
           && gen_key(g, "type")
           && gen_string(g, "anon_ref") == yajl_gen_status_ok
           && gen_key(g, "id")
           && yajl_gen_integer(g, oid) == yajl_gen_status_ok
           && yajl_gen_map_close(g) == yajl_gen_status_ok;
}

static unsigned int
waif_id_for_value(Var value, Json_Dump_Context *context)
{
    Waif *waif = value.v.waif;
    auto found = context->waif_ids.find(waif);
    if (found != context->waif_ids.end())
        return found->second;

    unsigned int id = (unsigned int)context->waifs.size();
    context->waif_ids[waif] = id;
    context->waifs.push_back(waif);
    return id;
}

static bool
write_waif_ref(yajl_gen g, Var value, Json_Dump_Context *context)
{
    unsigned int id = waif_id_for_value(value, context);

    return yajl_gen_map_open(g) == yajl_gen_status_ok
           && gen_key(g, "type")
           && gen_string(g, "waif_ref") == yajl_gen_status_ok
           && gen_key(g, "id")
           && yajl_gen_integer(g, id) == yajl_gen_status_ok
           && yajl_gen_map_close(g) == yajl_gen_status_ok;
}

static bool
write_db_list(yajl_gen g, Var value, Json_Dump_Context *context)
{
    if (yajl_gen_map_open(g) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "type") || gen_string(g, "list") != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "value") || yajl_gen_array_open(g) != yajl_gen_status_ok)
        return false;
    for (int i = 1; i <= value.v.list[0].v.num; i++)
        if (!write_db_var(g, value.v.list[i], context))
            return false;
    if (yajl_gen_array_close(g) != yajl_gen_status_ok)
        return false;
    return yajl_gen_map_close(g) == yajl_gen_status_ok;
}

static bool
write_db_map(yajl_gen g, Var value, Json_Dump_Context *context)
{
    Map_Write_Data data = { g, context, true };

    if (yajl_gen_map_open(g) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "type") || gen_string(g, "map") != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "value") || yajl_gen_array_open(g) != yajl_gen_status_ok)
        return false;
    if (mapforeach(value, write_map_item, &data) || !data.ok)
        return false;
    if (yajl_gen_array_close(g) != yajl_gen_status_ok)
        return false;
    return yajl_gen_map_close(g) == yajl_gen_status_ok;
}

static bool
write_db_var(yajl_gen g, Var value, Json_Dump_Context *context)
{
    if (value.type == TYPE_LIST)
        return write_db_list(g, value, context);
    if (value.type == TYPE_MAP)
        return write_db_map(g, value, context);
    if (value.type == TYPE_ANON)
        return write_anon_ref(g, value, context);
    if (value.type == TYPE_WAIF)
        return write_waif_ref(g, value, context);
    return db_json_generate_var(g, value);
}

struct Map_Collect_Data {
    Json_Dump_Context *context;
};

static int
collect_map_item(Var key, Var value, void *data, int first)
{
    Map_Collect_Data *collect = (Map_Collect_Data *)data;

    collect_db_var(key, collect->context);
    collect_db_var(value, collect->context);
    return 0;
}

static void
collect_db_var(Var value, Json_Dump_Context *context)
{
    if (value.type == TYPE_LIST) {
        for (int i = 1; i <= value.v.list[0].v.num; i++)
            collect_db_var(value.v.list[i], context);
    } else if (value.type == TYPE_MAP) {
        Map_Collect_Data data = { context };
        mapforeach(value, collect_map_item, &data);
    } else if (value.type == TYPE_ANON) {
        anon_id_for_value(value, context);
    } else if (value.type == TYPE_WAIF) {
        waif_id_for_value(value, context);
    }
}

static bool
gen_var_field(yajl_gen g, const char *key, Var value, Json_Dump_Context *context)
{
    return gen_key(g, key) && write_db_var(g, value, context);
}

static bool
write_propval(yajl_gen g, Pval *pval, Json_Dump_Context *context)
{
    return yajl_gen_map_open(g) == yajl_gen_status_ok
           && gen_var_field(g, "value", pval->var, context)
           && gen_key(g, "owner")
           && yajl_gen_integer(g, pval->owner) == yajl_gen_status_ok
           && gen_key(g, "perms")
           && yajl_gen_integer(g, pval->perms) == yajl_gen_status_ok
           && yajl_gen_map_close(g) == yajl_gen_status_ok;
}

static bool
write_object(yajl_gen g, Objid oid, Json_Dump_Context *context)
{
    Object *o;
    Verbdef *v;

    if (yajl_gen_map_open(g) != yajl_gen_status_ok)
        return false;

    if (!gen_key(g, "id") || yajl_gen_integer(g, oid) != yajl_gen_status_ok)
        return false;

    if (!valid(oid)) {
        return gen_key(g, "recycled")
               && yajl_gen_bool(g, 1) == yajl_gen_status_ok
               && yajl_gen_map_close(g) == yajl_gen_status_ok;
    }

    o = dbpriv_find_object(oid);

    if (!gen_key(g, "recycled") || yajl_gen_bool(g, 0) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "name") || gen_string(g, o->name ? o->name : "") != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "flags") || yajl_gen_integer(g, o->flags) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "owner") || yajl_gen_integer(g, o->owner) != yajl_gen_status_ok)
        return false;
    if (!gen_var_field(g, "location", o->location, context))
        return false;
    if (!gen_var_field(g, "last_move", o->last_move, context))
        return false;
    if (!gen_var_field(g, "contents", o->contents, context))
        return false;
    if (!gen_var_field(g, "parents", o->parents, context))
        return false;
    if (!gen_var_field(g, "children", o->children, context))
        return false;

    if (!gen_key(g, "verbs") || yajl_gen_array_open(g) != yajl_gen_status_ok)
        return false;
    for (v = o->verbdefs; v; v = v->next) {
        if (yajl_gen_map_open(g) != yajl_gen_status_ok)
            return false;
        if (!gen_key(g, "name") || gen_string(g, v->name ? v->name : "") != yajl_gen_status_ok)
            return false;
        if (!gen_key(g, "owner") || yajl_gen_integer(g, v->owner) != yajl_gen_status_ok)
            return false;
        if (!gen_key(g, "perms") || yajl_gen_integer(g, v->perms) != yajl_gen_status_ok)
            return false;
        if (!gen_key(g, "prep") || yajl_gen_integer(g, v->prep) != yajl_gen_status_ok)
            return false;
        if (!gen_key(g, "has_program") || yajl_gen_bool(g, v->program != nullptr) != yajl_gen_status_ok)
            return false;
        if (yajl_gen_map_close(g) != yajl_gen_status_ok)
            return false;
    }
    if (yajl_gen_array_close(g) != yajl_gen_status_ok)
        return false;

    if (!gen_key(g, "propdefs") || yajl_gen_array_open(g) != yajl_gen_status_ok)
        return false;
    for (int i = 0; i < o->propdefs.cur_length; i++)
        if (gen_string(g, o->propdefs.l[i].name) != yajl_gen_status_ok)
            return false;
    if (yajl_gen_array_close(g) != yajl_gen_status_ok)
        return false;

    if (!gen_key(g, "propvals") || yajl_gen_array_open(g) != yajl_gen_status_ok)
        return false;
    for (unsigned int i = 0; i < o->nval; i++)
        if (!write_propval(g, &o->propval[i], context))
            return false;
    if (yajl_gen_array_close(g) != yajl_gen_status_ok)
        return false;

    return yajl_gen_map_close(g) == yajl_gen_status_ok;
}

static bool
write_objects_file(const char *path, Json_Dump_Context *context)
{
    char objects_path[4096];
    yajl_gen_config cfg = { 0, "", 1 };
    yajl_gen g = yajl_gen_alloc(&cfg, nullptr);
    const unsigned char *buf;
    unsigned int len;
    FILE *f;
    bool ok = true;
    Objid last_oid = db_last_used_objid();

    if (!g)
        return false;

    if (std::snprintf(objects_path, sizeof(objects_path), "%s/objects/000000.json", path) >= (int)sizeof(objects_path)) {
        yajl_gen_free(g);
        return false;
    }

    if (yajl_gen_array_open(g) != yajl_gen_status_ok)
        ok = false;

    for (Objid oid = 0; ok && oid <= last_oid; oid++) {
        if (valid(oid) && dbpriv_is_assigned_anonymous_object(dbpriv_find_object(oid)))
            continue;
        ok = write_object(g, oid, context);
    }

    if (ok && yajl_gen_array_close(g) != yajl_gen_status_ok)
        ok = false;

    if (ok) {
        yajl_gen_get_buf(g, &buf, &len);
        f = std::fopen(objects_path, "wb");
        if (!f)
            ok = false;
        else {
            ok = std::fwrite(buf, 1, len, f) == len;
            if (std::fclose(f) != 0)
                ok = false;
        }
    }

    yajl_gen_clear(g);
    yajl_gen_free(g);
    return ok;
}

static bool
write_anon_object(yajl_gen g, Object *o, Json_Dump_Context *context)
{
    if (yajl_gen_map_open(g) != yajl_gen_status_ok)
        return false;

    if (!gen_key(g, "id") || yajl_gen_integer(g, o->id) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "name") || gen_string(g, o->name ? o->name : "") != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "flags") || yajl_gen_integer(g, o->flags) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "owner") || yajl_gen_integer(g, o->owner) != yajl_gen_status_ok)
        return false;
    if (!gen_var_field(g, "parents", o->parents, context))
        return false;

    if (!gen_key(g, "propdefs") || yajl_gen_array_open(g) != yajl_gen_status_ok)
        return false;
    for (int i = 0; i < o->propdefs.cur_length; i++)
        if (gen_string(g, o->propdefs.l[i].name) != yajl_gen_status_ok)
            return false;
    if (yajl_gen_array_close(g) != yajl_gen_status_ok)
        return false;

    if (!gen_key(g, "propvals") || yajl_gen_array_open(g) != yajl_gen_status_ok)
        return false;
    for (unsigned int i = 0; i < o->nval; i++)
        if (!write_propval(g, &o->propval[i], context))
            return false;
    if (yajl_gen_array_close(g) != yajl_gen_status_ok)
        return false;

    return yajl_gen_map_close(g) == yajl_gen_status_ok;
}

static bool
write_anons_file(const char *path, Json_Dump_Context *context)
{
    char anons_path[4096];
    yajl_gen_config cfg = { 0, "", 1 };
    yajl_gen g = yajl_gen_alloc(&cfg, nullptr);
    const unsigned char *buf;
    unsigned int len;
    FILE *f;
    bool ok = true;

    if (!g)
        return false;

    if (std::snprintf(anons_path, sizeof(anons_path), "%s/anons/000000.json", path) >= (int)sizeof(anons_path)) {
        yajl_gen_free(g);
        return false;
    }

    if (yajl_gen_array_open(g) != yajl_gen_status_ok)
        ok = false;

    for (unsigned int i = 0; ok && i < context->anons.size(); i++)
        ok = write_anon_object(g, context->anons[i], context);

    if (ok && yajl_gen_array_close(g) != yajl_gen_status_ok)
        ok = false;

    if (ok) {
        yajl_gen_get_buf(g, &buf, &len);
        f = std::fopen(anons_path, "wb");
        if (!f)
            ok = false;
        else {
            ok = std::fwrite(buf, 1, len, f) == len;
            if (std::fclose(f) != 0)
                ok = false;
        }
    }

    yajl_gen_clear(g);
    yajl_gen_free(g);
    return ok;
}

static void
collect_anon_object(Object *o, Json_Dump_Context *context)
{
    collect_db_var(o->parents, context);
    for (unsigned int i = 0; i < o->nval; i++)
        collect_db_var(o->propval[i].var, context);
}

static void
collect_waif(Waif *w, Json_Dump_Context *context)
{
    Num len;
    Var *val;

    waif_update_propdefs_for_saving(w);

    len = w->propdefs ? w->propdefs->length : 0;
    val = w->propvals;
    for (Num i = 0; i < len; i++) {
        if ((i < JSON_N_MAPPABLE_PROPS && JSON_PROP_MAPPED(w->map, i))
                || i >= JSON_N_MAPPABLE_PROPS)
            collect_db_var(*val, context);
        if (i >= JSON_N_MAPPABLE_PROPS || JSON_PROP_MAPPED(w->map, i))
            ++val;
    }
}

static void
collect_related_values(Json_Dump_Context *context)
{
    unsigned int anon_index = 0;
    unsigned int waif_index = 0;

    while (anon_index < context->anons.size()
            || waif_index < context->waifs.size()) {
        while (anon_index < context->anons.size())
            collect_anon_object(context->anons[anon_index++], context);
        while (waif_index < context->waifs.size())
            collect_waif(context->waifs[waif_index++], context);
    }
}

static void
collect_object_values(Object *o, Json_Dump_Context *context)
{
    collect_db_var(o->location, context);
    collect_db_var(o->last_move, context);
    collect_db_var(o->contents, context);
    collect_db_var(o->parents, context);
    collect_db_var(o->children, context);
    for (unsigned int i = 0; i < o->nval; i++)
        collect_db_var(o->propval[i].var, context);
}

static void
collect_object_values(Json_Dump_Context *context)
{
    Objid last_oid = db_last_used_objid();

    for (Objid oid = 0; oid <= last_oid; oid++) {
        if (!valid(oid))
            continue;

        Object *o = dbpriv_find_object(oid);
        if (dbpriv_object_has_flag(o, FLAG_ANONYMOUS))
            continue;

        collect_object_values(o, context);
    }
}

static void
collect_assigned_anons(Json_Dump_Context *context)
{
    Objid last_oid = db_last_used_objid();

    for (Objid oid = 0; oid <= last_oid; oid++) {
        if (!valid(oid))
            continue;

        Object *o = dbpriv_find_object(oid);
        if (!dbpriv_is_assigned_anonymous_object(o))
            continue;
        if (context->anon_ids.find(o) != context->anon_ids.end())
            continue;

        context->anon_ids[o] = oid;
        context->anons.push_back(o);
    }
}

static bool
write_waif_propval(yajl_gen g, Num index, Var value, Json_Dump_Context *context)
{
    return yajl_gen_map_open(g) == yajl_gen_status_ok
           && gen_key(g, "index")
           && yajl_gen_integer(g, index) == yajl_gen_status_ok
           && gen_key(g, "value")
           && write_db_var(g, value, context)
           && yajl_gen_map_close(g) == yajl_gen_status_ok;
}

static bool
write_waif_record(yajl_gen g, unsigned int id, Waif *w, Json_Dump_Context *context)
{
    Num len;
    Var *val;

    waif_update_propdefs_for_saving(w);
    len = w->propdefs ? w->propdefs->length : 0;

    if (yajl_gen_map_open(g) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "id") || yajl_gen_integer(g, id) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "class") || yajl_gen_integer(g, w->_class) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "owner") || yajl_gen_integer(g, w->owner) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "propdefs_length") || yajl_gen_integer(g, len) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "propvals") || yajl_gen_array_open(g) != yajl_gen_status_ok)
        return false;

    val = w->propvals;
    for (Num i = 0; i < len; i++) {
        if ((i < JSON_N_MAPPABLE_PROPS && JSON_PROP_MAPPED(w->map, i))
                || (i >= JSON_N_MAPPABLE_PROPS && val->type != TYPE_CLEAR)) {
            if (!write_waif_propval(g, i, *val, context))
                return false;
        }
        if (i >= JSON_N_MAPPABLE_PROPS || JSON_PROP_MAPPED(w->map, i))
            ++val;
    }

    if (yajl_gen_array_close(g) != yajl_gen_status_ok)
        return false;

    return yajl_gen_map_close(g) == yajl_gen_status_ok;
}

static bool
write_waifs_file(const char *path, Json_Dump_Context *context)
{
    char waifs_path[4096];
    yajl_gen_config cfg = { 0, "", 1 };
    yajl_gen g = yajl_gen_alloc(&cfg, nullptr);
    const unsigned char *buf;
    unsigned int len;
    FILE *f;
    bool ok = true;

    if (!g)
        return false;

    if (std::snprintf(waifs_path, sizeof(waifs_path), "%s/waifs.json", path) >= (int)sizeof(waifs_path)) {
        yajl_gen_free(g);
        return false;
    }

    ok = yajl_gen_map_open(g) == yajl_gen_status_ok
         && gen_key(g, "waifs")
         && yajl_gen_array_open(g) == yajl_gen_status_ok;

    for (unsigned int i = 0; ok && i < context->waifs.size(); i++)
        ok = write_waif_record(g, i, context->waifs[i], context);

    if (ok && yajl_gen_array_close(g) != yajl_gen_status_ok)
        ok = false;
    if (ok && yajl_gen_map_close(g) != yajl_gen_status_ok)
        ok = false;

    if (ok) {
        yajl_gen_get_buf(g, &buf, &len);
        f = std::fopen(waifs_path, "wb");
        if (!f)
            ok = false;
        else {
            ok = std::fwrite(buf, 1, len, f) == len;
            if (std::fclose(f) != 0)
                ok = false;
        }
    }

    yajl_gen_clear(g);
    yajl_gen_free(g);
    return ok;
}

static bool
write_pending_finalization_file(const char *path, Var pending, Json_Dump_Context *context)
{
    char pending_path[4096];
    yajl_gen_config cfg = { 0, "", 1 };
    yajl_gen g = yajl_gen_alloc(&cfg, nullptr);
    const unsigned char *buf;
    unsigned int len;
    FILE *f;
    bool ok = true;

    if (!g)
        return false;

    if (std::snprintf(pending_path, sizeof(pending_path), "%s/pending-finalization.json", path) >= (int)sizeof(pending_path)) {
        yajl_gen_free(g);
        return false;
    }

    ok = yajl_gen_map_open(g) == yajl_gen_status_ok
         && gen_key(g, "pending")
         && yajl_gen_array_open(g) == yajl_gen_status_ok;

    for (int i = 1; ok && i <= pending.v.list[0].v.num; i++)
        ok = write_db_var(g, pending.v.list[i], context);

    if (ok && yajl_gen_array_close(g) != yajl_gen_status_ok)
        ok = false;
    if (ok && yajl_gen_map_close(g) != yajl_gen_status_ok)
        ok = false;

    if (ok) {
        yajl_gen_get_buf(g, &buf, &len);
        f = std::fopen(pending_path, "wb");
        if (!f)
            ok = false;
        else {
            ok = std::fwrite(buf, 1, len, f) == len;
            if (std::fclose(f) != 0)
                ok = false;
        }
    }

    yajl_gen_clear(g);
    yajl_gen_free(g);
    return ok;
}

static bool
write_active_connections_file(const char *path, Var connections)
{
    char connections_path[4096];
    yajl_gen_config cfg = { 0, "", 1 };
    yajl_gen g = yajl_gen_alloc(&cfg, nullptr);
    const unsigned char *buf;
    unsigned int len;
    FILE *f;
    bool ok = true;

    if (!g)
        return false;

    if (std::snprintf(connections_path, sizeof(connections_path), "%s/active-connections.json", path) >= (int)sizeof(connections_path)) {
        yajl_gen_free(g);
        return false;
    }

    ok = yajl_gen_map_open(g) == yajl_gen_status_ok
         && gen_key(g, "connections")
         && yajl_gen_array_open(g) == yajl_gen_status_ok;

    for (int i = 1; ok && i <= connections.v.list[0].v.num; i++) {
        Var connection = connections.v.list[i];
        ok = yajl_gen_map_open(g) == yajl_gen_status_ok
             && gen_key(g, "player")
             && yajl_gen_integer(g, connection.v.list[1].v.obj) == yajl_gen_status_ok
             && gen_key(g, "listener")
             && yajl_gen_integer(g, connection.v.list[2].v.obj) == yajl_gen_status_ok
             && yajl_gen_map_close(g) == yajl_gen_status_ok;
    }

    if (ok && yajl_gen_array_close(g) != yajl_gen_status_ok)
        ok = false;
    if (ok && yajl_gen_map_close(g) != yajl_gen_status_ok)
        ok = false;

    if (ok) {
        yajl_gen_get_buf(g, &buf, &len);
        f = std::fopen(connections_path, "wb");
        if (!f)
            ok = false;
        else {
            ok = std::fwrite(buf, 1, len, f) == len;
            if (std::fclose(f) != 0)
                ok = false;
        }
    }

    yajl_gen_clear(g);
    yajl_gen_free(g);
    return ok;
}

static bool
read_all_from_file(FILE *file, std::string *contents)
{
    char buffer[8192];

    contents->clear();
    rewind(file);
    while (true) {
        size_t n = std::fread(buffer, 1, sizeof(buffer), file);
        if (n)
            contents->append(buffer, n);
        if (n < sizeof(buffer))
            return std::feof(file) != 0;
    }
}

static bool
write_task_queue_file(const char *path, Json_Dump_Context *context)
{
    char tasks_path[4096];
    FILE *task_file = tmpfile();
    FILE *seed_file = nullptr;
    std::vector<unsigned long> saved_waif_maps;
    std::string payload;
    yajl_gen_config cfg = { 0, "", 1 };
    yajl_gen g;
    const unsigned char *buf;
    unsigned int len;
    FILE *out;
    bool ok = true;

    if (!task_file)
        return false;

    waif_before_saving();
    if (!context->waifs.empty()) {
        saved_waif_maps.resize(context->waifs.size() * WAIF_MAPSZ);
        seed_file = tmpfile();
        if (!seed_file) {
            waif_after_saving();
            std::fclose(task_file);
            return false;
        }
        dbpriv_set_dbio_output(seed_file);
        for (unsigned int i = 0; i < context->waifs.size(); i++) {
            Var waif = Var::new_waif(context->waifs[i]);
            std::memcpy(&saved_waif_maps[i * WAIF_MAPSZ],
                        context->waifs[i]->map,
                        sizeof(unsigned long) * WAIF_MAPSZ);
            write_waif(waif);
        }
        std::fclose(seed_file);
    }
    dbpriv_set_dbio_output(task_file);
    write_task_queue();
    if (std::fflush(task_file) != 0)
        ok = false;
    if (ok && !read_all_from_file(task_file, &payload))
        ok = false;
    for (unsigned int i = 0; i < context->waifs.size(); i++)
        std::memcpy(context->waifs[i]->map,
                    &saved_waif_maps[i * WAIF_MAPSZ],
                    sizeof(unsigned long) * WAIF_MAPSZ);
    waif_after_saving();
    std::fclose(task_file);

    if (!ok)
        return false;

    if (std::snprintf(tasks_path, sizeof(tasks_path), "%s/tasks/queued.json", path) >= (int)sizeof(tasks_path))
        return false;

    g = yajl_gen_alloc(&cfg, nullptr);
    if (!g)
        return false;

    ok = yajl_gen_map_open(g) == yajl_gen_status_ok
         && gen_key(g, "native_task_queue_lines")
         && yajl_gen_array_open(g) == yajl_gen_status_ok;

    for (size_t start = 0; ok && start < payload.size();) {
        size_t end = payload.find('\n', start);
        if (end == std::string::npos)
            end = payload.size();
        ok = yajl_gen_string(g, (const unsigned char *)payload.data() + start,
                             (unsigned int)(end - start)) == yajl_gen_status_ok;
        start = end + 1;
    }

    if (ok && yajl_gen_array_close(g) != yajl_gen_status_ok)
        ok = false;
    if (ok && yajl_gen_map_close(g) != yajl_gen_status_ok)
        ok = false;

    if (ok) {
        yajl_gen_get_buf(g, &buf, &len);
        out = std::fopen(tasks_path, "wb");
        if (!out)
            ok = false;
        else {
            ok = std::fwrite(buf, 1, len, out) == len;
            if (std::fclose(out) != 0)
                ok = false;
        }
    }

    yajl_gen_clear(g);
    yajl_gen_free(g);
    return ok;
}

static void
collect_program_line(void *data, const char *line)
{
    std::vector<std::string> *lines = (std::vector<std::string> *)data;
    lines->push_back(line ? line : "");
}

static bool
write_program_record(yajl_gen g, Objid oid, int verb_index, Program *program)
{
    std::vector<std::string> lines;

    unparse_program(program, collect_program_line, &lines, 1, 0, MAIN_VECTOR);

    if (yajl_gen_map_open(g) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "object") || yajl_gen_integer(g, oid) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "verb_index") || yajl_gen_integer(g, verb_index) != yajl_gen_status_ok)
        return false;
    if (!gen_key(g, "code") || yajl_gen_array_open(g) != yajl_gen_status_ok)
        return false;
    for (std::vector<std::string>::const_iterator it = lines.begin(); it != lines.end(); ++it)
        if (gen_string(g, it->c_str()) != yajl_gen_status_ok)
            return false;
    if (yajl_gen_array_close(g) != yajl_gen_status_ok)
        return false;

    return yajl_gen_map_close(g) == yajl_gen_status_ok;
}

static bool
write_programs_file(const char *path)
{
    char programs_path[4096];
    yajl_gen_config cfg = { 0, "", 1 };
    yajl_gen g = yajl_gen_alloc(&cfg, nullptr);
    const unsigned char *buf;
    unsigned int len;
    FILE *f;
    bool ok = true;
    Objid last_oid = db_last_used_objid();

    if (!g)
        return false;

    if (std::snprintf(programs_path, sizeof(programs_path), "%s/programs/000000.json", path) >= (int)sizeof(programs_path)) {
        yajl_gen_free(g);
        return false;
    }

    if (yajl_gen_array_open(g) != yajl_gen_status_ok)
        ok = false;

    for (Objid oid = 0; ok && oid <= last_oid; oid++) {
        if (!valid(oid))
            continue;

        Object *o = dbpriv_find_object(oid);
        int verb_index = 0;
        for (Verbdef *v = o->verbdefs; ok && v; v = v->next) {
            if (v->program)
                ok = write_program_record(g, oid, verb_index, v->program);
            verb_index++;
        }
    }

    if (ok && yajl_gen_array_close(g) != yajl_gen_status_ok)
        ok = false;

    if (ok) {
        yajl_gen_get_buf(g, &buf, &len);
        f = std::fopen(programs_path, "wb");
        if (!f)
            ok = false;
        else {
            ok = std::fwrite(buf, 1, len, f) == len;
            if (std::fclose(f) != 0)
                ok = false;
        }
    }

    yajl_gen_clear(g);
    yajl_gen_free(g);
    return ok;
}

static bool
write_users_file(const char *path)
{
    char users_path[4096];
    yajl_gen_config cfg = { 0, "", 1 };
    yajl_gen g = yajl_gen_alloc(&cfg, nullptr);
    const unsigned char *buf;
    unsigned int len;
    FILE *f;
    bool ok = true;
    Var users = db_all_users();

    if (!g)
        return false;

    if (std::snprintf(users_path, sizeof(users_path), "%s/users.json", path) >= (int)sizeof(users_path)) {
        yajl_gen_free(g);
        return false;
    }

    ok = yajl_gen_map_open(g) == yajl_gen_status_ok
         && gen_key(g, "users")
         && yajl_gen_array_open(g) == yajl_gen_status_ok;

    for (int i = 1; ok && i <= users.v.list[0].v.num; i++)
        ok = yajl_gen_integer(g, users.v.list[i].v.obj) == yajl_gen_status_ok;

    if (ok && yajl_gen_array_close(g) != yajl_gen_status_ok)
        ok = false;
    if (ok && yajl_gen_map_close(g) != yajl_gen_status_ok)
        ok = false;

    if (ok) {
        yajl_gen_get_buf(g, &buf, &len);
        f = std::fopen(users_path, "wb");
        if (!f)
            ok = false;
        else {
            ok = std::fwrite(buf, 1, len, f) == len;
            if (std::fclose(f) != 0)
                ok = false;
        }
    }

    yajl_gen_clear(g);
    yajl_gen_free(g);
    return ok;
}

bool
db_json_write_database_dump(const char *path, int engine_db_version)
{
    if (!db_json_write_skeleton_dump(path, engine_db_version))
        return false;

    Json_Dump_Context context;
    Var pending = values_pending_finalization_for_json();
    Var connections = active_connections_for_json();
    bool ok;

    collect_object_values(&context);
    collect_db_var(pending, &context);
    collect_related_values(&context);
    ok = write_task_queue_file(path, &context);
    collect_assigned_anons(&context);
    collect_related_values(&context);

    ok = ok
         && write_objects_file(path, &context)
         && write_pending_finalization_file(path, pending, &context)
         && write_active_connections_file(path, connections)
         && write_anons_file(path, &context)
         && write_waifs_file(path, &context)
         && write_programs_file(path)
         && write_users_file(path);
    free_var(connections);
    free_var(pending);
    return ok;
}
