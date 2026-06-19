#include "db_backend.h"
#include "db_json.h"
#include "functions.h"
#include "map.h"
#include "streams.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

void
panic_moo(const char *message)
{
    std::fprintf(stderr, "panic_moo in test: %s\n", message);
    std::abort();
}

static Var
make_none_var()
{
    Var value;
    value.type = TYPE_NONE;
    return value;
}

Var none = make_none_var();
Var zero = Var::new_int(0);

static int
test_string_compare(const char *lhs, const char *rhs, int case_matters)
{
    if (case_matters)
        return std::strcmp(lhs, rhs);

    while (*lhs && *rhs) {
        int lc = std::tolower((unsigned char)*lhs);
        int rc = std::tolower((unsigned char)*rhs);
        if (lc != rc)
            return lc - rc;
        lhs++;
        rhs++;
    }

    return std::tolower((unsigned char)*lhs) - std::tolower((unsigned char)*rhs);
}

int
compare(Var lhs, Var rhs, int case_matters)
{
    if (lhs.type != rhs.type)
        return lhs.type - rhs.type;
    if (lhs.type == TYPE_INT || lhs.type == TYPE_BOOL)
        return lhs.v.num < rhs.v.num ? -1 : lhs.v.num > rhs.v.num ? 1 : 0;
    if (lhs.type == TYPE_OBJ)
        return lhs.v.obj < rhs.v.obj ? -1 : lhs.v.obj > rhs.v.obj ? 1 : 0;
    if (lhs.type == TYPE_STR)
        return test_string_compare(lhs.v.str ? lhs.v.str : "",
                                   rhs.v.str ? rhs.v.str : "",
                                   case_matters);
    return 0;
}

int
equality(Var lhs, Var rhs, int case_matters)
{
    return compare(lhs, rhs, case_matters) == 0;
}

void complex_free_var(Var) {}
Var complex_var_ref(Var value) { return value; }
Var complex_var_dup(Var value) { return value; }
int var_refcount(Var) { return 1; }
int value_bytes(Var) { return 0; }
int is_true(Var value)
{
    if (value.type == TYPE_BOOL)
        return value.v.truth;
    if (value.type == TYPE_INT)
        return value.v.num != 0;
    return value.type != TYPE_NONE;
}

Var
new_list(int size)
{
    Var list;
    list.type = TYPE_LIST;
    list.v.list = (Var *)mymalloc((size + 1) * sizeof(Var), M_LIST);
    list.v.list[0] = Var::new_int(size);
    return list;
}

Var
listappend(Var list, Var value)
{
    list.v.list[0].v.num++;
    list.v.list = (Var *)myrealloc(list.v.list,
                                   (list.v.list[0].v.num + 1) * sizeof(Var),
                                   M_LIST);
    list.v.list[list.v.list[0].v.num] = value;
    return list;
}

Stream *new_stream(int) { return nullptr; }
void stream_add_string(Stream *, const char *) {}
char *stream_contents(Stream *) { return nullptr; }
void free_stream(Stream *) {}
void unparse_value(Stream *, Var) {}

package
make_error_pack(enum error err)
{
    return make_raise_pack(err, "", zero);
}

package
make_raise_pack(enum error err, const char *msg, Var value)
{
    package p;
    Var code;

    code.type = TYPE_ERR;
    code.v.err = err;
    p.kind = package::BI_RAISE;
    p.u.raise.code = code;
    p.u.raise.msg = msg;
    p.u.raise.value = value;
    return p;
}

package
make_var_pack(Var value)
{
    package p;
    p.kind = package::BI_RETURN;
    p.u.ret = value;
    return p;
}

unsigned
register_function(const char *, int, int, bf_type,...)
{
    return 0;
}

static void
expect_true(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void
expect_kind(DB_Backend_Kind actual, DB_Backend_Kind expected, const char *message)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void
expect_json(Var value, const char *expected, const char *message)
{
    std::string json;
    if (!db_json_var_to_json(value, &json)) {
        std::fprintf(stderr, "FAIL: %s: encoding failed\n", message);
        failures++;
        return;
    }

    if (json != expected) {
        std::fprintf(stderr, "FAIL: %s: got %s\n", message, json.c_str());
        failures++;
    }
}

static void
expect_parse_type(const char *json, var_type type, const char *message)
{
    Var value;
    if (!db_json_parse_var(json, &value)) {
        std::fprintf(stderr, "FAIL: %s: parsing failed\n", message);
        failures++;
        return;
    }

    if (value.type != type) {
        std::fprintf(stderr, "FAIL: %s: got type %d\n", message, value.type);
        failures++;
    }
}

static void
expect_parse_int(const char *json, Num expected, const char *message)
{
    Var value;
    if (!db_json_parse_var(json, &value)) {
        std::fprintf(stderr, "FAIL: %s: parsing failed\n", message);
        failures++;
        return;
    }

    if (value.type != TYPE_INT || value.v.num != expected) {
        std::fprintf(stderr, "FAIL: %s: got type %d value %lld\n",
                     message, value.type, (long long)value.v.num);
        failures++;
    }
}

static void
expect_parse_string(const char *json, const char *expected, const char *message)
{
    Var value;
    if (!db_json_parse_var(json, &value)) {
        std::fprintf(stderr, "FAIL: %s: parsing failed\n", message);
        failures++;
        return;
    }

    if (value.type != TYPE_STR || std::strcmp(value.v.str, expected) != 0) {
        std::fprintf(stderr, "FAIL: %s: got type %d value %s\n",
                     message, value.type, value.type == TYPE_STR ? value.v.str : "");
        failures++;
    }
}

static void
write_file(const char *path, const char *contents)
{
    FILE *f = std::fopen(path, "w");
    expect_true(f != nullptr, "opened test file for writing");
    if (!f)
        return;
    std::fputs(contents, f);
    std::fclose(f);
}

static bool
path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool
file_contains(const char *path, const char *needle)
{
    char buffer[4096];
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return false;
    size_t nread = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[nread] = '\0';
    std::fclose(f);
    return std::strstr(buffer, needle) != nullptr;
}

int
main()
{
    char root_template[] = "/tmp/toaststunt-db-backend-test.XXXXXX";
    char *root = mkdtemp(root_template);
    expect_true(root != nullptr, "created temp directory");
    if (!root)
        return 1;

    char json_dir[512];
    std::snprintf(json_dir, sizeof(json_dir), "%s/world.v20", root);
    expect_true(mkdir(json_dir, 0700) == 0, "created json directory");

    char manifest[512];
    std::snprintf(manifest, sizeof(manifest), "%s/manifest.json", json_dir);
    write_file(manifest,
               "{\n"
               "  \"format\": \"toaststunt-json-db\",\n"
               "  \"format_version\": 20\n"
               "}\n");

    char bad_dir[512];
    std::snprintf(bad_dir, sizeof(bad_dir), "%s/bad.v20", root);
    expect_true(mkdir(bad_dir, 0700) == 0, "created bad json directory");

    char bad_manifest[512];
    std::snprintf(bad_manifest, sizeof(bad_manifest), "%s/manifest.json", bad_dir);
    write_file(bad_manifest,
               "{\n"
               "  \"format\": \"toaststunt-json-db\",\n"
               "  \"format_version\": 99\n"
               "}\n");

    char bad_dir_with_20[512];
    std::snprintf(bad_dir_with_20, sizeof(bad_dir_with_20), "%s/bad-with-20.v20", root);
    expect_true(mkdir(bad_dir_with_20, 0700) == 0, "created bad json directory with unrelated 20");

    char bad_manifest_with_20[512];
    std::snprintf(bad_manifest_with_20, sizeof(bad_manifest_with_20),
                  "%s/manifest.json", bad_dir_with_20);
    write_file(bad_manifest_with_20,
               "{\n"
               "  \"format\": \"toaststunt-json-db\",\n"
               "  \"format_version\": 99,\n"
               "  \"engine_db_version\": 20\n"
               "}\n");

    expect_kind(db_backend_for_input("Minimal.db"), DB_BACKEND_NATIVE_TEXT,
                "plain db input uses native backend");
    expect_kind(db_backend_for_output("Minimal.db.new"), DB_BACKEND_NATIVE_TEXT,
                "plain db output uses native backend");
    expect_kind(db_backend_for_output("world.v20"), DB_BACKEND_JSON_V20,
                ".v20 output uses json backend");
    expect_kind(db_backend_for_input(json_dir), DB_BACKEND_JSON_V20,
                "manifest v20 directory input uses json backend");
    expect_kind(db_backend_for_input(bad_dir), DB_BACKEND_UNSUPPORTED,
                "unknown manifest version is unsupported");
    expect_kind(db_backend_for_input(bad_dir_with_20), DB_BACKEND_UNSUPPORTED,
                "unrelated 20 does not make manifest v20");

    DB_Dump_Target targets[DB_MAX_DUMP_TARGETS];
    int count = db_parse_dump_targets("out.db", nullptr, targets, DB_MAX_DUMP_TARGETS);
    expect_true(count == 1, "native-only dump has one target");
    expect_kind(targets[0].backend, DB_BACKEND_NATIVE_TEXT,
                "native-only target uses native backend");

    count = db_parse_dump_targets("out.v20", nullptr, targets, DB_MAX_DUMP_TARGETS);
    expect_true(count == 1, "json-only dump has one target");
    expect_kind(targets[0].backend, DB_BACKEND_JSON_V20,
                "json-only target uses json backend");

    count = db_parse_dump_targets("out.db", "out.v20", targets, DB_MAX_DUMP_TARGETS);
    expect_true(count == 2, "dual dump has two targets");
    expect_kind(targets[0].backend, DB_BACKEND_NATIVE_TEXT,
                "first dual target uses native backend");
    expect_kind(targets[1].backend, DB_BACKEND_JSON_V20,
                "second dual target uses json backend");

    count = db_parse_dump_targets("out.db", "json-dump", targets, DB_MAX_DUMP_TARGETS);
    expect_true(count == 2, "explicit json-v20 dual dump has two targets");
    expect_kind(targets[0].backend, DB_BACKEND_NATIVE_TEXT,
                "explicit json-v20 first target uses native backend");
    expect_kind(targets[1].backend, DB_BACKEND_JSON_V20,
                "explicit json-v20 secondary target uses json backend");

    char dump_dir[512];
    std::snprintf(dump_dir, sizeof(dump_dir), "%s/dump.v20", root);
    expect_true(db_json_write_skeleton_dump(dump_dir, 19),
                "skeleton json dump succeeds");

    char dump_manifest[512];
    std::snprintf(dump_manifest, sizeof(dump_manifest), "%s/manifest.json", dump_dir);
    expect_true(path_exists(dump_manifest), "skeleton dump writes manifest");
    expect_true(file_contains(dump_manifest, "\"format\":\"toaststunt-json-db\""),
                "manifest contains json db format");
    expect_true(file_contains(dump_manifest, "\"format_version\":20"),
                "manifest contains format version");
    expect_true(file_contains(dump_manifest, "\"engine_db_version\":19"),
                "manifest contains engine db version");

    char dump_objects[512];
    std::snprintf(dump_objects, sizeof(dump_objects), "%s/objects", dump_dir);
    expect_true(path_exists(dump_objects), "skeleton dump creates objects dir");

    char dump_anons[512];
    std::snprintf(dump_anons, sizeof(dump_anons), "%s/anons", dump_dir);
    expect_true(path_exists(dump_anons), "skeleton dump creates anons dir");

    char dump_programs[512];
    std::snprintf(dump_programs, sizeof(dump_programs), "%s/programs", dump_dir);
    expect_true(path_exists(dump_programs), "skeleton dump creates programs dir");

    char dump_tasks[512];
    std::snprintf(dump_tasks, sizeof(dump_tasks), "%s/tasks", dump_dir);
    expect_true(path_exists(dump_tasks), "skeleton dump creates tasks dir");

    char pending_file[512];
    std::snprintf(pending_file, sizeof(pending_file), "%s/pending-finalization.json", dump_dir);
    expect_true(file_contains(pending_file, "\"pending\":[]"),
                "skeleton dump writes empty pending finalization file");

    char connections_file[512];
    std::snprintf(connections_file, sizeof(connections_file), "%s/active-connections.json", dump_dir);
    expect_true(file_contains(connections_file, "\"connections\":[]"),
                "skeleton dump writes empty active connections file");

    char queued_file[512];
    std::snprintf(queued_file, sizeof(queued_file), "%s/tasks/queued.json", dump_dir);
    expect_true(file_contains(queued_file, "\"queued\":[]"),
                "skeleton dump writes empty queued tasks file");

    char suspended_file[512];
    std::snprintf(suspended_file, sizeof(suspended_file), "%s/tasks/suspended.json", dump_dir);
    expect_true(file_contains(suspended_file, "\"suspended\":[]"),
                "skeleton dump writes empty suspended tasks file");

    char interrupted_file[512];
    std::snprintf(interrupted_file, sizeof(interrupted_file), "%s/tasks/interrupted.json", dump_dir);
    expect_true(file_contains(interrupted_file, "\"interrupted\":[]"),
                "skeleton dump writes empty interrupted tasks file");

    Var value;
    value.type = TYPE_NONE;
    expect_json(value, "{\"type\":\"none\"}", "encodes none");

    value.type = TYPE_CLEAR;
    expect_json(value, "{\"type\":\"clear\"}", "encodes clear");

    expect_json(Var::new_int(42), "{\"type\":\"int\",\"value\":42}", "encodes int");
    expect_json(Var::new_float(1.25), "{\"type\":\"float\",\"value\":\"1.25\"}", "encodes float");

    value.type = TYPE_STR;
    value.v.str = "hello";
    expect_json(value, "{\"type\":\"str\",\"value\":\"hello\"}", "encodes string");

    expect_json(Var::new_obj(123), "{\"type\":\"obj\",\"value\":123}", "encodes object");

    value.type = TYPE_ERR;
    value.v.err = E_PERM;
    expect_json(value, "{\"type\":\"err\",\"value\":\"E_PERM\"}", "encodes error");

    expect_json(Var::new_bool(1), "{\"type\":\"bool\",\"value\":true}", "encodes true");
    expect_json(Var::new_bool(0), "{\"type\":\"bool\",\"value\":false}", "encodes false");

    Var list_values[3];
    list_values[0] = Var::new_int(2);
    list_values[1] = Var::new_int(7);
    list_values[2] = Var::new_obj(8);
    value.type = TYPE_LIST;
    value.v.list = list_values;
    expect_json(value,
                "{\"type\":\"list\",\"value\":[{\"type\":\"int\",\"value\":7},{\"type\":\"obj\",\"value\":8}]}",
                "encodes list");

    value.type = TYPE_ANON;
    value.v.anon = nullptr;
    std::string json;
    expect_true(!db_json_var_to_json(value, &json), "anon without registry fails");

    value.type = TYPE_WAIF;
    value.v.waif = nullptr;
    expect_true(!db_json_var_to_json(value, &json), "waif without registry fails");

    expect_parse_type("{\"type\":\"none\"}", TYPE_NONE, "parses none");
    expect_parse_type("{\"type\":\"clear\"}", TYPE_CLEAR, "parses clear");
    expect_parse_int("{\"type\":\"int\",\"value\":42}", 42, "parses int");

    Var parsed;
    expect_true(db_json_parse_var("{\"type\":\"float\",\"value\":\"1.25\"}", &parsed),
                "parses float");
    expect_true(parsed.type == TYPE_FLOAT && parsed.v.fnum == 1.25,
                "parsed float has expected value");

    expect_parse_string("{\"type\":\"str\",\"value\":\"hello\"}", "hello",
                        "parses string");

    expect_true(db_json_parse_var("{\"type\":\"obj\",\"value\":123}", &parsed),
                "parses object");
    expect_true(parsed.type == TYPE_OBJ && parsed.v.obj == 123,
                "parsed object has expected value");

    expect_true(db_json_parse_var("{\"type\":\"err\",\"value\":\"E_PERM\"}", &parsed),
                "parses error");
    expect_true(parsed.type == TYPE_ERR && parsed.v.err == E_PERM,
                "parsed error has expected value");

    expect_true(db_json_parse_var("{\"type\":\"bool\",\"value\":true}", &parsed),
                "parses true");
    expect_true(parsed.type == TYPE_BOOL && parsed.v.truth,
                "parsed true has expected value");
    expect_true(db_json_parse_var("{\"type\":\"bool\",\"value\":false}", &parsed),
                "parses false");
    expect_true(parsed.type == TYPE_BOOL && !parsed.v.truth,
                "parsed false has expected value");

    expect_true(db_json_parse_var(
                    "{\"type\":\"list\",\"value\":[{\"type\":\"int\",\"value\":7},{\"type\":\"list\",\"value\":[{\"type\":\"obj\",\"value\":8}]}]}",
                    &parsed),
                "parses nested list");
    expect_true(parsed.type == TYPE_LIST
                && parsed.v.list[0].v.num == 2
                && parsed.v.list[1].type == TYPE_INT
                && parsed.v.list[1].v.num == 7
                && parsed.v.list[2].type == TYPE_LIST
                && parsed.v.list[2].v.list[1].type == TYPE_OBJ
                && parsed.v.list[2].v.list[1].v.obj == 8,
                "nested list has expected values");

    Var map = new_map();
    map = mapinsert(map, str_dup_to_var("answer"), Var::new_int(42));
    expect_true(db_json_var_to_json(map, &json), "encodes map");
    expect_true(db_json_parse_var(json.c_str(), &parsed), "parses map");
    expect_true(parsed.type == TYPE_MAP && maplength(parsed) == 1,
                "map round-trip has expected shape");

    expect_true(!db_json_parse_var("{\"type\":\"anon_ref\",\"id\":17}", &parsed),
                "anon ref without registry fails");
    expect_true(!db_json_parse_var("{\"type\":\"waif_ref\",\"id\":42}", &parsed),
                "waif ref without registry fails");

    return failures == 0 ? 0 : 1;
}
