# AST Functionality for ToastStunt MOO

This document outlines the investigation, planning, and design for adding Abstract Syntax Tree (AST) functionality to ToastStunt, allowing MOO code to parse, manipulate, and reconstruct MOO code programmatically.

## Goals

- Expose ToastStunt's internal parser to MOO code via builtin functions
- Allow MOO programs to parse code strings and verbrefs into structured data
- Enable reconstruction of code from AST representations
- Support code analysis, transformation, and generation use cases
- Maintain security and performance standards

## Use Cases

1. **Code Analysis**: Analyze complexity, dependencies, unused variables
2. **Code Transformation**: Automated refactoring, optimization, code generation  
3. **Dynamic Code Generation**: Build complex code structures programmatically
4. **Educational Tools**: Syntax highlighting, formatting, teaching tools
5. **Template Systems**: Advanced templating with code parsing/substitution
6. **Security Analysis**: Scan code for dangerous patterns

## API Design (Preliminary)

### Core Functions

```moo
// Parse LIST of code lines or verbref to AST
parse_ast(code_lines [, options]) -> AST_map or E_INVARG
parse_ast({obj, verb} [, options]) -> AST_map or E_INVARG

// Convert AST back to LIST of code lines
unparse_ast(ast_map [, options]) -> code_lines or E_INVARG

// Validate AST structure
validate_ast(ast_map) -> 1 or 0
```

### Advanced Functions (Future)

```moo
// Compile AST directly to bytecode
compile_ast(ast_map) -> program_object or E_INVARG

// Apply transformations to AST
ast_transform(ast_map, transformer_func) -> transformed_ast
```

## AST Representation Format

**Root MAP Structure** (all AST functions return this format):
```moo
[
  "ast_version" -> 1,           // Schema version for compatibility
  "db_version" -> N,            // Database version used during parsing  
  "first_user_slot" -> 24,      // Index where user variables start
  "ids_user" -> ["x", "result"], // User variable names (built-ins excluded)
  "body" -> stmt_seq_node       // Always STMT_SEQ containing statement list
]
```

**Individual AST Node Format** (typed, per-node schema):
```moo
// Root is always a statement sequence
["type" -> "STMT_SEQ", "stmts" -> [stmt1, stmt2, ...]]

// Function call: foo(1, 2)
["type" -> "EXPR_CALL", "name" -> "foo", "args" -> [lit1, lit2]]

// Literals: 42, "hello"  
["type" -> "LIT_NUM", "value" -> 42]
["type" -> "LIT_STR", "value" -> "hello"]

// Variable reference: player
["type" -> "ID_VAR", "name" -> "player", "id" -> 5]

// Binary operation: x + y
["type" -> "EXPR_PLUS", "lhs" -> var_x, "rhs" -> var_y]

// If statement: if (x > 0) return x; endif  
["type" -> "STMT_COND", "arms" -> [arm1]]
```

**Input/Output Format**: All functions use **LIST of strings** (same as verb code):
```moo
// Input to parse_ast() 
code_lines = {"x = 1 + 2;", "return x;"}

// Output from unparse_ast()
code_lines = {"x = 1 + 2;", "return x;"}
```

## Core Integration Findings

### Unparser Context Requirement (VERIFIED)
- **Issue**: Unparser functions (`unparse_expr`, `unparse_stmt`) require Program* context
- **Root Cause**: Variable IDs need resolution via `prog->var_names[id]` 
- **Verification**: Only `prog->var_names[]` accessed (src/unparse.cc:426,441,751,838,841)
- **Solution**: Create minimal Program with ONLY `var_names` and `num_var_names` populated
- **Safety**: All other Program fields can be zero/NULL - unparser never reads them

**Minimal Program Context** (verified safe from unparse.cc analysis):
```c
Program* create_program_context(Var ids_user, int first_user_slot) {
    Program *prog = (Program*)calloc(1, sizeof(Program)); // Zero all fields
    
    // Build complete var_names: built-ins + user variables
    Names *names = new_builtin_names(current_db_version);
    for (int i = 1; i <= ids_user.v.list[0].v.num; i++) {
        find_or_add_name(&names, ids_user.v.list[i].v.str);
    }
    
    prog->num_var_names = names->size;
    prog->var_names = names->names;
    return prog;
}
```

**Global Usage Pattern** (verified from unparse.cc:859):
```c
// Set global prog pointer before calling unparse functions
extern Program *prog;  // Static global in unparse.cc
prog = create_program_context(ast["ids_user"], ast["first_user_slot"]);
unparse_stmt(stmt_ast, 0);  // Now has access to var_names
```

### Statement Sequence Integration (VERIFIED)
- **Current AST**: Uses linked list via `stmt->next` pointer (src/include/ast.h:202)
- **No STMT_SEQ**: Existing AST has no explicit sequence node type
- **Conversion Required**: Must convert linked list ↔ LIST of MAP statements
- **Implementation**: Create STMT_SEQ as virtual node type (MOO-side only)

**Statement Linked List Structure** (verified from ast.h:201-205):
```c
struct Stmt {
    Stmt *next;            // Line 202 - linked list pointer
    enum Stmt_Kind kind;   // Line 203 - STMT_COND, STMT_EXPR, etc.
    union Stmt_Data s;     // Line 204 - statement-specific data
};
```

**Conversion Pattern**:
```c
// AST→MAP: Convert linked list to STMT_SEQ MAP
Var stmt_list_to_map(Stmt *first_stmt) {
    Var seq_map = new_map();
    Var stmts_list = new_list(0);
    
    for (Stmt *stmt = first_stmt; stmt; stmt = stmt->next) {
        Var stmt_map = stmt_to_map(stmt);  // Convert individual statement
        stmts_list = listappend(stmts_list, stmt_map);
    }
    
    seq_map = mapinsert(seq_map, str_to_var("type"), str_to_var("STMT_SEQ"));
    seq_map = mapinsert(seq_map, str_to_var("stmts"), stmts_list);
    return seq_map;
}

// MAP→AST: Convert STMT_SEQ MAP to linked list
Stmt* map_to_stmt_list(Var seq_map) {
    Var stmts_list = mapfind(seq_map, str_to_var("stmts"));
    Stmt *first = NULL, *prev = NULL;
    
    for (int i = 1; i <= stmts_list.v.list[0].v.num; i++) {
        Stmt *stmt = map_to_stmt(stmts_list.v.list[i]);  // Convert MAP to statement
        if (prev) prev->next = stmt; else first = stmt;
        prev = stmt;
    }
    return first;
}
```

### Builtin Function Resolution (VERIFIED)
- **Forward Lookup**: `name_func_by_num(unsigned n)` → string name (src/functions.cc:178)
- **Reverse Lookup**: `number_func_by_name(const char *name)` → unsigned ID (src/functions.cc:187)
- **Error Handling**: Returns `FUNC_NOT_FOUND` constant when function unknown
- **Table Access**: Both use global `bf_table[]` with case-insensitive matching

**Function Resolution Functions** (verified from functions.cc):
```c
// Convert function ID to name (for AST→MAP conversion)
const char* name_func_by_num(unsigned n) {
    if (n >= top_bf_table) return func_not_found_msg;
    return bf_table[n].name;  // Direct table lookup
}

// Convert function name to ID (for MAP→AST conversion)  
unsigned number_func_by_name(const char *name) {
    for (unsigned i = 0; i < top_bf_table; i++) {
        if (!strcasecmp(name, bf_table[i].name)) return i;  // Case-insensitive
    }
    return FUNC_NOT_FOUND;  // Defined as MAX_FUNC in functions.h:79
}
```

**EXPR_CALL Conversion Pattern**:
```c
// AST→MAP: Include both name and ID for portability
Var call_to_map(Expr *expr) {
    Var call_map = new_map();
    call_map = mapinsert(call_map, str_to_var("type"), str_to_var("EXPR_CALL"));
    call_map = mapinsert(call_map, str_to_var("func_name"), 
                        str_to_var(name_func_by_num(expr->e.call.func)));
    call_map = mapinsert(call_map, str_to_var("func_id"), 
                        var_from_num(expr->e.call.func));
    return call_map;
}

// MAP→AST: Prefer name, fallback to ID  
unsigned map_to_func_id(Var call_map) {
    Var name_var = mapfind(call_map, str_to_var("func_name"));
    if (name_var.type == TYPE_STR) {
        unsigned id = number_func_by_name(name_var.v.str);
        if (id != FUNC_NOT_FOUND) return id;
    }
    
    Var id_var = mapfind(call_map, str_to_var("func_id"));
    if (id_var.type == TYPE_INT && id_var.v.num < top_bf_table) {
        return id_var.v.num;  // Fallback to ID if name lookup fails
    }
    
    return FUNC_NOT_FOUND;  // Error: neither name nor valid ID
}
```

### File Structure and Placement (PLANNED)

**New Files to Create**:
```
src/ast_builtins.cc         // AST builtin functions (bf_parse_ast, bf_unparse_ast, etc.)
src/include/ast_builtins.h  // Header for AST builtin declarations
src/ast_convert.cc          // AST↔MAP conversion functions  
src/include/ast_convert.h   // Header for conversion function declarations
```

**Builtin Function Pattern** (verified from src/map.cc:1139-1145):
```c
// In ast_builtins.cc - follow existing builtin patterns
static Package bf_parse_ast(Var arglist, Byte next, void *vdata, Objid progr);
static Package bf_unparse_ast(Var arglist, Byte next, void *vdata, Objid progr);
static Package bf_validate_ast(Var arglist, Byte next, void *vdata, Objid progr);

void register_ast(void) {
    register_function("parse_ast", 1, 2, bf_parse_ast, TYPE_LIST);
    register_function("unparse_ast", 1, 2, bf_unparse_ast, TYPE_MAP);  
    register_function("validate_ast", 1, 2, bf_validate_ast, TYPE_MAP);
}
```

**Registration Integration** (add to src/functions.cc):
```c
// Add to functions.cc registration table around line 57
register_map,
register_ast,        // NEW: Add AST function registration
register_numbers,
```

**File Organization**:
- `ast_builtins.cc/h`: Public builtin functions exposed to MOO
- `ast_convert.cc/h`: Internal conversion between AST structs and MOO MAPs 
- `ast.cc/ast.h`: Keep existing AST structure definitions (no changes)
- Follow existing ToastStunt patterns for error handling, memory management

**Build System**: Add to CMakeLists.txt like other source files
**Testing**: Create `test/tests/test_ast.rb` following existing test patterns

### Memory Management Patterns (VERIFIED)

**AST Memory Management** (verified from src/ast.cc:43-86):
- AST uses pooled allocation: `alloc_ast()` and `end_code_allocation()`
- Pool automatically freed at end: `myfree(pool[i].ptr, pool[i].type)`
- **Critical**: AST structures temporary - must convert to MOO Vars before pool cleanup

**MOO Var Memory Management** (verified from src/map.cc patterns):
- **Reference Counting**: Use `var_ref()` when storing, `free_var()` when done
- **Creation**: `new_map()`, `new_list()`, `str_to_var()` create new Vars
- **Insertion**: `mapinsert()`, `listappend()` handle reference counting automatically
- **Return Pattern**: `make_var_pack(result)` or `make_error_pack(E_TYPE)`

**Conversion Safety Patterns**:
```c
// SAFE: Convert AST immediately, don't store AST pointers
Var ast_to_map(Stmt *stmt) {
    Var result = new_map();
    
    // Convert all data immediately while AST pool is valid
    result = mapinsert(result, str_to_var("type"), str_to_var("STMT_EXPR"));
    
    // For nested structures: recursive conversion 
    if (stmt->s.expr) {
        Var expr_map = expr_to_map(stmt->s.expr);  // Convert immediately
        result = mapinsert(result, str_to_var("expr"), expr_map);
    }
    
    return result;  // Safe: no AST pointers stored
}

// UNSAFE: Never store AST pointers in Vars or global state
// AST pool will be freed after parsing completes!
```

**Builtin Function Pattern** (verified from src/map.cc:1134):
```c
static Package bf_parse_ast(Var arglist, Byte next, void *vdata, Objid progr) {
    // 1. Validate arguments
    if (arglist.v.list[1].type != TYPE_LIST) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    }
    
    // 2. Process (AST conversion must happen here)
    Var result = parse_code_to_ast_map(arglist.v.list[1]);
    
    // 3. Cleanup and return
    free_var(arglist);
    if (result.type == TYPE_ERR) {
        return make_error_pack(result.v.err);
    }
    return make_var_pack(result);
}
```

**Key Memory Safety Rules**:
1. **Convert AST→MAP immediately** during parse (while pool valid)
2. **Never store AST pointers** beyond the builtin function call
3. **Use var_ref()** when storing Vars, **free_var()** when done
4. **Always free_var(arglist)** before returning from builtin
5. **Return make_var_pack() or make_error_pack()** following existing patterns

## Implementation Design Specifications

### Map Key Normalization (REQUIRED)

**Consistent Key Format**: All MAP keys must be lowercase ASCII strings to prevent case-sensitivity bugs and ensure schema consistency.

**Key Normalization Function**:
```c
// Convert key to canonical lowercase form
Var normalize_ast_key(const char* key) {
    static char buffer[64];  // Sufficient for all AST keys
    const char* src = key;
    char* dst = buffer;
    
    while (*src && (dst - buffer) < sizeof(buffer) - 1) {
        *dst++ = tolower(*src++);  // Convert to lowercase
    }
    *dst = '\0';
    
    return str_to_var(str_dup(buffer));  // Create proper MOO STR
}
```

**Standard AST Keys** (all lowercase, verified consistent):
```c
// Root structure keys
#define AST_KEY_AST_VERSION    "ast_version"
#define AST_KEY_DB_VERSION     "db_version" 
#define AST_KEY_FIRST_USER     "first_user_slot"
#define AST_KEY_IDS_USER       "ids_user"
#define AST_KEY_BODY           "body"

// Node type keys
#define AST_KEY_TYPE           "type"
#define AST_KEY_STMTS          "stmts"
#define AST_KEY_ARGS           "args"
#define AST_KEY_VALUE          "value"
#define AST_KEY_NAME           "name"
#define AST_KEY_ID             "id"

// Expression keys  
#define AST_KEY_LHS            "lhs"
#define AST_KEY_RHS            "rhs"
#define AST_KEY_CONDITION      "condition"
#define AST_KEY_EXPR           "expr"
#define AST_KEY_FUNC_NAME      "func_name"
#define AST_KEY_FUNC_ID        "func_id"

// Statement keys
#define AST_KEY_ARMS           "arms"
#define AST_KEY_OTHERWISE      "otherwise"
#define AST_KEY_BODY_STMT      "body"
```

**Usage Pattern**:
```c
// CORRECT: Use normalized keys
Var ast_map = new_map();
ast_map = mapinsert(ast_map, normalize_ast_key("type"), str_to_var("EXPR_PLUS"));
ast_map = mapinsert(ast_map, normalize_ast_key("lhs"), lhs_map);

// INCORRECT: Raw string keys (case-sensitive bugs)
ast_map = mapinsert(ast_map, str_to_var("Type"), str_to_var("EXPR_PLUS"));  // BAD
```

### String Storage Patterns (REQUIRED)

**Proper MOO STR Creation**: All strings must use `str_to_var()` with `str_dup()` to ensure proper reference counting and memory management.

**String Handling Functions**:
```c
// Create MOO STR from C string (proper way)
Var make_ast_string(const char* cstr) {
    if (!cstr) return str_to_var("");  // Handle NULL gracefully
    return str_to_var(str_dup(cstr));  // Proper MOO string creation
}

// Create AST node type string (optimized for common types)
Var make_ast_type(const char* type_name) {
    // These are static strings, safe to use directly
    return str_to_var(str_dup(type_name));
}

// Extract C string from Var safely
const char* get_ast_string(Var string_var) {
    if (string_var.type != TYPE_STR) return NULL;
    return string_var.v.str;  // Direct access to string data
}
```

**String Safety Patterns**:
```c
// CORRECT: Proper string creation and storage
Var node_name = make_ast_string(name_func_by_num(func_id));
ast_map = mapinsert(ast_map, normalize_ast_key("func_name"), node_name);

// CORRECT: Type constants
ast_map = mapinsert(ast_map, normalize_ast_key("type"), make_ast_type("EXPR_CALL"));

// CORRECT: Variable names from symbol table
Var var_name = make_ast_string(prog->var_names[expr->e.id]);
ast_map = mapinsert(ast_map, normalize_ast_key("name"), var_name);

// INCORRECT: Direct string literals (no reference counting)
ast_map = mapinsert(ast_map, str_to_var("type"), str_to_var("EXPR_CALL"));  // BAD

// INCORRECT: Using C strings directly in Vars
Var bad_var;
bad_var.type = TYPE_STR;
bad_var.v.str = "literal";  // BAD: No memory management
```

**String Validation Patterns**:
```c
// Validate string type and extract safely
int extract_ast_string(Var map, const char* key, char** result) {
    Var key_var = normalize_ast_key(key);
    Var value_var = mapfind(map, key_var);
    free_var(key_var);
    
    if (value_var.type != TYPE_STR) {
        *result = NULL;
        return 0;  // Type mismatch
    }
    
    *result = value_var.v.str;
    return 1;  // Success
}
```

### Version Compatibility System (REQUIRED)

**Version Checking Strategy**: Graceful rejection of unknown AST versions with clear error messages and forward/backward compatibility support.

**Version Constants**:
```c
// AST schema version constants
#define AST_VERSION_1        1     // Initial AST implementation
#define AST_VERSION_CURRENT  AST_VERSION_1
#define AST_VERSION_MIN      1     // Minimum supported version
#define AST_VERSION_MAX      1     // Maximum supported version

// Database version validation  
#define DB_VERSION_MIN       DBV_Prehistory  
#define DB_VERSION_MAX       Num_DB_Versions - 1
```

**Version Validation Functions**:
```c
// Check if AST version is supported
int validate_ast_version(int ast_version) {
    return (ast_version >= AST_VERSION_MIN && ast_version <= AST_VERSION_MAX);
}

// Check if DB version is supported
int validate_db_version(int db_version) {
    return (db_version >= DB_VERSION_MIN && db_version <= DB_VERSION_MAX);
}

// Extract and validate version fields from AST MAP
Package validate_ast_versions(Var ast_map, int* ast_version, int* db_version) {
    // Extract AST version
    Var ast_ver_var = mapfind(ast_map, normalize_ast_key("ast_version"));
    if (ast_ver_var.type != TYPE_INT) {
        return make_error_pack(E_TYPE);  // Missing or wrong type
    }
    *ast_version = ast_ver_var.v.num;
    
    // Extract DB version  
    Var db_ver_var = mapfind(ast_map, normalize_ast_key("db_version"));
    if (db_ver_var.type != TYPE_INT) {
        return make_error_pack(E_TYPE);  // Missing or wrong type
    }
    *db_version = db_ver_var.v.num;
    
    // Validate versions
    if (!validate_ast_version(*ast_version)) {
        return make_error_pack(E_INVARG);  // Unsupported AST version
    }
    
    if (!validate_db_version(*db_version)) {
        return make_error_pack(E_INVARG);  // Unsupported DB version
    }
    
    return make_var_pack(var_from_num(1));  // Success
}
```

**Version Compatibility Patterns**:
```c
// Create AST with current versions
Var create_ast_root(Var body_map, Var ids_user) {
    Var root = new_map();
    
    // Set current versions
    root = mapinsert(root, normalize_ast_key("ast_version"), 
                    var_from_num(AST_VERSION_CURRENT));
    root = mapinsert(root, normalize_ast_key("db_version"), 
                    var_from_num(current_db_version));
    
    // Add other fields
    root = mapinsert(root, normalize_ast_key("first_user_slot"),
                    var_from_num(first_user_slot(current_db_version)));
    root = mapinsert(root, normalize_ast_key("ids_user"), ids_user);
    root = mapinsert(root, normalize_ast_key("body"), body_map);
    
    return root;
}

// Validate incoming AST before processing
Package validate_incoming_ast(Var ast_map) {
    int ast_version, db_version;
    Package version_check = validate_ast_versions(ast_map, &ast_version, &db_version);
    
    if (version_check.kind != BI_RETURN) {
        return version_check;  // Version validation failed
    }
    
    // Future: Add version-specific migration logic here
    // if (ast_version < AST_VERSION_CURRENT) {
    //     return migrate_ast_v1_to_v2(ast_map);
    // }
    
    return make_var_pack(var_from_num(1));  // Compatible
}
```

**Error Messages for Version Mismatches**:
```c
// Enhanced error reporting (future improvement)
const char* ast_version_error_message(int requested_version) {
    if (requested_version < AST_VERSION_MIN) {
        return "AST version too old - please regenerate AST";
    } else if (requested_version > AST_VERSION_MAX) {
        return "AST version too new - server upgrade required";  
    }
    return "Invalid AST version";
}
```

### AST Schema Validation (REQUIRED)

**Validation Strategy**: Comprehensive schema validation with detailed error reporting and required field checking.

**Node Type Validation Table**:
```c
// Schema definition for each AST node type
typedef struct {
    const char* node_type;
    const char* required_fields[8];  // NULL-terminated list
    const char* optional_fields[8];  // NULL-terminated list
} ast_node_schema;

static ast_node_schema ast_schemas[] = {
    // Root node
    {"STMT_SEQ", {"type", "stmts", NULL}, {NULL}},
    
    // Expression nodes
    {"EXPR_CALL", {"type", "func_name", "args", NULL}, {"func_id", NULL}},
    {"EXPR_PLUS", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_MINUS", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_TIMES", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_DIVIDE", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_MOD", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_EQ", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_NE", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_LT", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_LE", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_GT", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_GE", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_AND", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_OR", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_NOT", {"type", "expr", NULL}, {NULL}},
    {"EXPR_NEGATE", {"type", "expr", NULL}, {NULL}},
    {"EXPR_COMPLEMENT", {"type", "expr", NULL}, {NULL}},
    {"EXPR_PROP", {"type", "obj", "prop", NULL}, {NULL}},
    {"EXPR_VERB", {"type", "obj", "verb", "args", NULL}, {NULL}},
    {"EXPR_INDEX", {"type", "obj", "index", NULL}, {NULL}},
    {"EXPR_RANGE", {"type", "obj", "from", "to", NULL}, {NULL}},
    {"EXPR_COND", {"type", "condition", "consequent", "alternate", NULL}, {NULL}},
    {"EXPR_ASGN", {"type", "lhs", "rhs", NULL}, {NULL}},
    {"EXPR_LIST", {"type", "elements", NULL}, {NULL}},
    {"EXPR_MAP", {"type", "pairs", NULL}, {NULL}},
    
    // Literal nodes
    {"LIT_NUM", {"type", "value", NULL}, {NULL}},
    {"LIT_STR", {"type", "value", NULL}, {NULL}},
    {"LIT_OBJ", {"type", "value", NULL}, {NULL}},
    {"LIT_ERR", {"type", "value", NULL}, {NULL}},
    
    // Identifier nodes
    {"ID_VAR", {"type", "name", "id", NULL}, {NULL}},
    
    // Statement nodes
    {"STMT_EXPR", {"type", "expr", NULL}, {NULL}},
    {"STMT_RETURN", {"type", NULL}, {"expr", NULL}},
    {"STMT_COND", {"type", "arms", NULL}, {"otherwise", NULL}},
    {"STMT_WHILE", {"type", "condition", "body", NULL}, {"id", NULL}},
    {"STMT_LIST", {"type", "id", "expr", "body", NULL}, {"index", NULL}},
    {"STMT_RANGE", {"type", "id", "from", "to", "body", NULL}, {NULL}},
    {"STMT_FORK", {"type", "time", "body", NULL}, {"id", NULL}},
    {"STMT_TRY_EXCEPT", {"type", "body", "excepts", NULL}, {NULL}},
    {"STMT_TRY_FINALLY", {"type", "body", "handler", NULL}, {NULL}},
    {"STMT_BREAK", {"type", NULL}, {"id", NULL}},
    {"STMT_CONTINUE", {"type", NULL}, {"id", NULL}},
    
    {NULL, {NULL}, {NULL}}  // Terminator
};
```

**Validation Functions**:
```c
// Find schema for node type
static ast_node_schema* find_node_schema(const char* node_type) {
    for (int i = 0; ast_schemas[i].node_type; i++) {
        if (!strcasecmp(ast_schemas[i].node_type, node_type)) {
            return &ast_schemas[i];
        }
    }
    return NULL;  // Unknown node type
}

// Validate single AST node against schema
int validate_ast_node(Var node_map, char** error_msg) {
    // Extract node type
    Var type_var = mapfind(node_map, normalize_ast_key("type"));
    if (type_var.type != TYPE_STR) {
        *error_msg = str_dup("Missing or invalid 'type' field");
        return 0;
    }
    
    const char* node_type = type_var.v.str;
    ast_node_schema* schema = find_node_schema(node_type);
    if (!schema) {
        *error_msg = str_dup("Unknown node type");
        return 0;
    }
    
    // Check required fields
    for (int i = 0; schema->required_fields[i]; i++) {
        const char* field = schema->required_fields[i];
        Var field_var = mapfind(node_map, normalize_ast_key(field));
        
        if (field_var.type == TYPE_NONE) {  // Field missing
            char* msg = (char*)mymalloc(100, M_STRING);
            sprintf(msg, "Missing required field: %s", field);
            *error_msg = msg;
            return 0;
        }
    }
    
    // Recursively validate child nodes
    // ... (implement based on field types)
    
    return 1;  // Valid
}

// Main validation function for builtin
static Package bf_validate_ast(Var arglist, Byte next, void *vdata, Objid progr) {
    // Validate argument count and type
    if (arglist.v.list[0].v.num != 1 || arglist.v.list[1].type != TYPE_MAP) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    }
    
    Var ast_map = arglist.v.list[1];
    
    // Validate versions first
    Package version_check = validate_incoming_ast(ast_map);
    if (version_check.kind != BI_RETURN) {
        free_var(arglist);
        return version_check;
    }
    
    // Validate body structure
    Var body_var = mapfind(ast_map, normalize_ast_key("body"));
    if (body_var.type != TYPE_MAP) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    }
    
    // Validate AST nodes recursively
    char* error_msg = NULL;
    int valid = validate_ast_node(body_var, &error_msg);
    
    free_var(arglist);
    
    if (!valid) {
        // Return structured error (future enhancement)
        if (error_msg) {
            myfree(error_msg, M_STRING);
        }
        return make_error_pack(E_INVARG);
    }
    
    return make_var_pack(var_from_num(1));  // Valid
}
```

### Error Propagation Mechanism
- **Parse Error Structure** (verified from parser.cc:3447-3452):
  ```c
  struct parser_state {
      Var code;     // Input: LIST of strings
      Var errors;   // Output: LIST of error message strings
  }
  ```
- **Error Collection**: Errors stored as LIST of STR via `my_error()` callback (parser.cc:3455-3463)
- **Error Format**: Each error is a string message added to errors LIST via `listappend()`
- **Integration Point**: `parse_list_as_program(Var code, Var *errors)` (parser.cc:3488)
- **Usage Pattern**: Parse returns Program* or NULL, errors LIST always populated

## Testing Strategy

### Current Testing Infrastructure (verified from test/ directory)
- **Framework**: Ruby Test::Unit with custom MOO test helpers
- **Pattern**: Tests interact with running MOO server via socket connection
- **Setup**: Server runs in one terminal, tests run in another (`make`)
- **Location**: All tests in `test/tests/*.rb`, helpers in `test/tests/lib/`
- **Key Files**:
  - `test_helper.rb`: Main test infrastructure
  - `moo_support.rb`: MOO server communication helpers
  - `test_moocode_parsing.rb`: Existing code parsing tests (perfect template)

### Test Architecture (verified from test/tests/lib/moo_support.rb)
- Tests connect via socket to running MOO server
- Helper functions: `command()`, `simplify()`, `run_test_as()`
- Object creation: `create()`, `add_verb()`, `set_verb_code()`
- Code introspection: `verb_code()`, `disassemble()`
- Error handling: Predefined E_* constants

### AST Test Requirements (test_ast.rb)

**Test Pattern** (based on test_moocode_parsing.rb):
```ruby
def test_parse_ast_simple_expression
  run_test_as('programmer') do
    result = simplify(command('; return parse_ast({"x = 1;"});'))
    assert_equal "STMT_SEQ", result["body"]["type"]
    assert_equal "STMT_EXPR", result["body"]["stmts"][1]["type"]
  end
end

def test_unparse_ast_round_trip
  run_test_as('programmer') do
    code = {"x = 1 + 2;", "return x;"}
    ast = simplify(command("; return parse_ast(#{code});"))
    result = simplify(command("; return unparse_ast(#{ast});"))
    assert_equal code, result
  end
end
```

**Test Categories Required**:
1. **Basic Parse/Unparse**: Simple expressions, statements, round-trip fidelity
2. **Error Handling**: Invalid syntax, malformed AST, type mismatches  
3. **Complex Constructs**: Nested control flow, function calls, operators
4. **Edge Cases**: Empty code, large code, special characters
5. **Performance**: Large ASTs, stress testing
6. **Security**: Permission checking, memory limits

2. **Integration Tests**  
   - Full parse -> unparse -> parse roundtrips
   - Integration with verb system
   - Complex nested structures

3. **Functional Tests**
   - Real MOO code examples
   - All MOO language constructs
   - Edge cases in MOO syntax

4. **Performance Tests**
   - Large code files
   - Deeply nested structures
   - Memory usage patterns
   - Performance regression testing

5. **Security Tests**
   - Malicious code input
   - Resource exhaustion attacks
   - Permission boundary testing
   - Information disclosure prevention

6. **Error Handling Tests**
   - Invalid AST structures
   - Malformed code strings
   - Missing verbs/objects
   - Permission denied scenarios

### TDD Approach

1. **Start with simple expressions**: `1 + 2`, `"hello"`, `variable`
2. **Build up complexity**: function calls, conditionals, loops
3. **Add statements**: assignments, returns, complex control flow
4. **Test edge cases**: empty code, syntax errors, deeply nested structures
5. **Security testing**: malicious input, resource limits
6. **Performance validation**: large inputs, complex structures

### Test Examples (Preliminary)

```ruby
# Basic parsing test
def test_parse_simple_expression
  result = call_function("parse_ast", "1 + 2")
  assert_equal("EXPR_BINARY", result["type"])
  assert_equal("+", result["operator"])
  # ... more assertions
end

# Roundtrip test  
def test_parse_unparse_roundtrip
  original = "if (x > 0) return x; else return -x; endif"
  ast = call_function("parse_ast", original)
  reconstructed = call_function("unparse_ast", ast)
  # Should be functionally equivalent even if formatting differs
  ast2 = call_function("parse_ast", reconstructed)
  assert_ast_equivalent(ast, ast2)
end

# Security test
def test_parse_resource_limits
  huge_code = "(" * 10000 + "1" + ")" * 10000
  result = call_function("parse_ast", huge_code)
  assert_equal("E_QUOTA", result) # Should hit resource limits
end
```

## Core Investigation (Focused Approach)

**Key Constraint: MUST reuse existing parser - no separate parser needed.**

### Critical Questions to Answer

1. **What AST node types does `parser.y` create?**
   - Examine Bison grammar rules and AST node construction
   - Identify all node types (expressions, statements, literals, etc.)
   - Understand node structure and relationships

2. **How do we hook into existing parser?**
   - Find parsing entry points in current code
   - Understand how to capture AST before compilation
   - Identify integration points for builtin functions

3. **What's the minimal MAP representation?**
   - Each AST node as MAP: `["type" -> "NODE_TYPE", ...]`
   - Add only necessary fields per node type (lhs/rhs, args, condition, etc.)
   - Keep it simple - MOO handles nested structures fine

4. **How do existing builtins work?**
   - Registration patterns
   - Argument parsing and error handling
   - Integration with MOO type system

### Security Considerations

1. **Input Validation**
   - Size limits on code strings
   - Parsing depth limits  
   - Resource consumption controls
   - Malicious code pattern detection

2. **Permission Model**
   - **Use existing verb permission system** - no new permissions needed
   - `parse_ast({obj, verb})` requires same permissions as reading the verb
   - `compile_ast_to_verb()` would require same permissions as writing to the verb
   - Leverages proven, existing security model

3. **Information Disclosure**
   - Parser error message sanitization
   - Preventing inference of hidden code
   - Line number and location information handling

4. **Resource Protection**
   - Memory usage limits
   - CPU time limits
   - Concurrent operation limits
   - Cleanup and resource management

## Implementation Plan (High Level)

1. **Phase 1: Investigation**
   - Analyze current parser and AST structures
   - Study builtin function patterns
   - Design detailed AST representation
   - Create comprehensive test suite

2. **Phase 2: Core Implementation**
   - Implement basic `parse_ast()` for expressions
   - Implement basic `unparse_ast()` for expressions
   - Add fundamental test coverage
   - Establish security boundaries

3. **Phase 3: Full Language Support**
   - Extend to statements and full programs
   - Add verbref support
   - Implement `validate_ast()`
   - Comprehensive testing

4. **Phase 4: Advanced Features**
   - Performance optimization
   - Advanced API functions
   - Documentation and examples
   - Production hardening

## Design Decisions for Implementation

### Version 1 Scope Decisions

**Locations**: Decided to skip line/column tracking in v1 for simplicity. Can be added in v2 as additive enhancement.

**Versioning**: Include versioning metadata in root MAP for graceful schema evolution.

**Root MAP Structure**: All AST MAPs will include these top-level fields:
- `"ast_version" -> 1` - Schema version for compatibility checking
- `"db_version" -> N` - Database version used during parsing (from `current_db_version`)
- `"ids" -> [...]` - Symbol table: LIST of variable names for id↔name mappings
- `"body" -> MAP` - The actual AST content (program/expression)

**Conversion Architecture**: Keep conversion code separate from core AST structs to avoid parser churn. Implement as visitor pattern in new `ast_map.cc` translation unit:
```c
// Conversion functions with symbol table and options support
Var expr_to_map(const Expr*, const Symtab*, const Options*);
Var stmt_to_map(const Stmt*, const Symtab*, const Options*);
Expr* map_to_expr(Var, Symtab*, const Options*, Error_Collector*);
Stmt* map_to_stmt(Var, Symtab*, const Options*, Error_Collector*);
```

**Identifiers**: Symbol table (Symtab) encapsulates id↔name mappings and special IDs. Build during parse, serialize into root `"ids"` field, restore on MAP→AST conversion. This ensures portable AST representations across database versions.

**Schema**: Will canonicalize field names and split literal/identifier types before implementation to prevent breaking changes later. Need consistent operand names (`"lhs"`, `"rhs"`, `"args"`, etc.) and stable `"type"` enums.

**Unparser**: Investigate reusing existing `unparse.cc` instead of custom templating to leverage existing precedence handling.

## Canonical AST Schema

### Standard Field Names

**All AST nodes use these consistent field patterns:**

**Core Fields:**
- `"type"` - Node type (always "EXPR_*" or "STMT_*" or "LIT_*" or "ID_*")
- `"lhs"` / `"rhs"` - Left/right operands for binary expressions
- `"operand"` - Single operand for unary expressions  
- `"args"` - Argument list (LIST of expressions)
- `"condition"` - Boolean test expression
- `"body"` - Statement block or expression list
- `"name"` - Variable/function name (string)
- `"value"` - Literal value for constants

**Control Flow Fields:**
- `"arms"` - LIST of condition/body pairs for if/elseif
- `"otherwise"` - Else clause body
- `"target"` - Loop variable or break/continue target

**Node Type Conventions:**
- **Literals**: `"LIT_NUM"`, `"LIT_STR"`, `"LIT_OBJ"`, `"LIT_ERR"`
- **Identifiers**: `"ID_VAR"` (variable reference)
- **Expressions**: `"EXPR_PLUS"`, `"EXPR_CALL"`, `"EXPR_PROP"`, etc.
- **Statements**: `"STMT_IF"`, `"STMT_FOR"`, `"STMT_RETURN"`, etc.

### Literal vs Identifier Split

**Literals (constant values):**
```moo
["type" -> "LIT_NUM", "value" -> 42]
["type" -> "LIT_STR", "value" -> "hello"]  
["type" -> "LIT_OBJ", "value" -> #1234]
["type" -> "LIT_ERR", "value" -> E_PERM]
```

**Identifiers (variable references):**
```moo
["type" -> "ID_VAR", "name" -> "player", "id" -> 15]
```

### Binary Expression Standard:
```moo
["type" -> "EXPR_PLUS", "lhs" -> expr1, "rhs" -> expr2]
["type" -> "EXPR_EQ", "lhs" -> expr1, "rhs" -> expr2]
```

### Function Call Standard:
```moo
["type" -> "EXPR_CALL", "name" -> "length", "args" -> [expr1, expr2]]
```

**Unparser**: Investigate reusing existing `unparse.cc` instead of custom templating to leverage existing precedence handling.

## AST Structure Discovery

### Expression Types (from ast.h)
```c
enum Expr_Kind {
    EXPR_VAR, EXPR_ID,                    // Variables and identifiers
    EXPR_PROP, EXPR_VERB,                 // Property and verb access
    EXPR_INDEX, EXPR_RANGE,               // Indexing and ranges
    EXPR_ASGN, EXPR_CALL,                 // Assignment and function calls
    EXPR_PLUS, EXPR_MINUS, EXPR_TIMES,    // Arithmetic operators
    EXPR_DIVIDE, EXPR_MOD, EXPR_EXP,
    EXPR_NEGATE,
    EXPR_AND, EXPR_OR, EXPR_NOT,          // Logical operators
    EXPR_EQ, EXPR_NE, EXPR_LT, EXPR_LE,   // Comparison operators
    EXPR_GT, EXPR_GE, EXPR_IN,
    EXPR_LIST, EXPR_COND, EXPR_CATCH,     // Collections and control
    EXPR_MAP, EXPR_FIRST, EXPR_LAST,      // Maps and list operations
    EXPR_BITOR, EXPR_BITAND, EXPR_BITXOR, // Bitwise operators
    EXPR_BITSHL, EXPR_BITSHR, EXPR_COMPLEMENT,
    EXPR_SCATTER, EXPR_LENGTH             // Special constructs
};
```

### Statement Types (from ast.h)
```c
enum Stmt_Kind {
    STMT_COND,                            // if/elseif/else
    STMT_LIST, STMT_RANGE,                // for loops  
    STMT_WHILE, STMT_FORK,                // while loops and forking
    STMT_EXPR, STMT_RETURN,               // expression statements and returns
    STMT_TRY_EXCEPT, STMT_TRY_FINALLY,    // exception handling
    STMT_BREAK, STMT_CONTINUE             // loop control
};
```

### Key Data Structures
- `Expr` struct has `kind` (enum) and `e` (union of different expr data)
- `Stmt` struct has `kind` (enum) and `s` (union of different stmt data)  
- Each has `next` pointer for linked lists of statements/expressions
- Binary expressions use `struct Expr_Binary { Expr *lhs, *rhs; }`
- Function calls use `struct Expr_Call { unsigned func; Arg_List *args; }`
- Verb calls use `struct Expr_Verb { Expr *obj, *verb; Arg_List *args; }`

### Parser Architecture Discovery

**Key Functions Found:**
- `parse_program(DB_Version, Parser_Client, void*)` - Main parser entry point
- `parse_list_as_program(Var code, Var *errors)` - Parse MOO list of strings 
- `generate_code(Stmt*, DB_Version)` - Convert AST to bytecode
- Global `prog_start` - Points to root AST after parsing

**Parser Flow:**
1. `parse_list_as_program()` calls `parse_program()`
2. `parse_program()` calls `yyparse()` (Bison parser)
3. Bison creates AST nodes using `alloc_stmt()`, `alloc_expr()` etc.
4. Root AST stored in global `prog_start`
5. `generate_code(prog_start)` converts AST to bytecode

**Hook Opportunity:**
We can create a new function that calls `parse_program()` but captures the AST before `generate_code()` destroys it!

**AST Conversion Architecture:**
Implement conversion as visitor pattern in separate `ast_map.cc` translation unit:

```c
// In ast_map.h:
extern Var expr_to_map(const Expr*, const Symtab*, const Options*);
extern Var stmt_to_map(const Stmt*, const Symtab*, const Options*); 
extern Expr *map_to_expr(Var, Symtab*, const Options*, Error_Collector*);
extern Stmt *map_to_stmt(Var, Symtab*, const Options*, Error_Collector*);

// In ast_map.cc:
Var expr_to_map(const Expr *expr, const Symtab *symtab, const Options *opts) {
    Var result = new_map();
    result = mapinsert(result, str_dup_to_var("type"), 
                      str_dup_to_var(expr_kind_name(expr->kind)));
    
    switch (expr->kind) {
        case EXPR_PLUS:
            result = mapinsert(result, str_dup_to_var("lhs"), 
                              expr_to_map(expr->e.bin.lhs, symtab, opts));
            result = mapinsert(result, str_dup_to_var("rhs"), 
                              expr_to_map(expr->e.bin.rhs, symtab, opts));
            break;
        case EXPR_ID:
            result = mapinsert(result, str_dup_to_var("name"),
                              str_dup_to_var(symtab_id_to_name(symtab, expr->e.id)));
            result = mapinsert(result, str_dup_to_var("id"), Var{TYPE_INT, .v.num = expr->e.id});
            break;
        // ... cases for all expression types
    }
    return result;
}
```

**Builtin Functions with Proper Memory Management:**
```c
static package bf_parse_ast(Var arglist, Byte next, void *vdata, Objid progr) {
    // Parse code, capture AST in prog_start, convert to MAP
    Symtab *symtab = build_symtab_from_parse();
    Var result = stmt_to_map(prog_start, symtab, default_options);
    return make_var_pack(result);
}

static package bf_unparse_ast(Var arglist, Byte next, void *vdata, Objid progr) {
    // Allocate AST in temporary pool
    begin_code_allocation();
    
    Symtab *symtab = symtab_from_map(arglist.v.list[1]);
    Stmt *stmt = map_to_stmt(arglist.v.list[1], symtab, default_options, &errors);
    if (!stmt) {
        end_code_allocation(0);  // Clean up on error
        return make_error_pack(E_INVARG);
    }
    
    // Convert AST to code immediately while still allocated
    Var result = unparse_stmt_to_var(stmt);  // Uses existing unparse.cc
    
    end_code_allocation(0);  // Free entire AST pool
    return make_var_pack(result);  // Return MOO-managed result
}
```

**Key Memory Management Rule**: Never return raw C AST pointers to MOO. Always convert AST→MAP during parse, and during unparse allocate AST temporarily under `begin_code_allocation()`, convert to code, then `end_code_allocation(0)` to free the pool.

### AST ↔ MOO MAP Conversion

**Parse Direction (Code → AST):**
Each AST node becomes a MAP. Examples:

```moo
// "1 + 2" becomes:
["type" -> "EXPR_PLUS",
 "lhs" -> ["type" -> "LIT_NUM", "value" -> 1],
 "rhs" -> ["type" -> "LIT_NUM", "value" -> 2]]

// "foo(bar, 42)" becomes:
["type" -> "EXPR_CALL", "name" -> "foo",
 "args" -> [["type" -> "ID_VAR", "name" -> "bar", "id" -> 3],
            ["type" -> "LIT_NUM", "value" -> 42]]]

// "if (x > 0) return x; endif" becomes:
["type" -> "STMT_COND",
 "arms" -> [["condition" -> ["type" -> "EXPR_GT", 
                            "lhs" -> ["type" -> "ID_VAR", "name" -> "x", "id" -> 2],
                            "rhs" -> ["type" -> "LIT_NUM", "value" -> 0]],
             "body" -> ["type" -> "STMT_RETURN", 
                       "expr" -> ["type" -> "ID_VAR", "name" -> "x", "id" -> 2]]]],
 "otherwise" -> 0]
```

**Unparse Direction (AST → Code):**
Simple templating based on node type:
```c
// In bf_unparse_ast():
switch (node_type) {
    case "EXPR_BINARY":
        return unparse_lhs + " " + op + " " + unparse_rhs;
    case "EXPR_CALL": 
        return func_name + "(" + join_args + ")";
    case "STMT_IF":
        return "if (" + condition + ") " + body + " endif";
}
```

**Key Insights:** 
- We're not decompiling bytecode - we're just converting between our own AST representation formats (C structs ↔ MOO MAPs ↔ code strings)
- **AST structs should know how to convert themselves** - Add methods directly to AST structures for clean OOP design

### API Usage Examples

```moo
// Parse code strings directly
ast = parse_ast({"x = 1 + 2;", "return x;"});

// Get AST from existing verb
ast = verb_ast(#1234, "process_data");

// Convert AST back to code
code_lines = unparse_ast(ast);

// Write modified AST back to a verb  
set_verb_ast(#1234, "new_process_data", modified_ast);

// Validate AST structure
if (validate_ast(ast))
    // AST is well-formed
else  
    // AST has structural problems
endif
```

## Complete Implementation Plan

### Core Functions to Implement

1. **`parse_ast({code})`** - Parse LIST of code strings
   - Takes LIST of code strings (same format as verb code)
   - Call existing `parse_program()` function
   - Capture AST from `prog_start` before `generate_code()`
   - Convert C++ AST structures to MOO MAPs recursively
   - Return MAP representation or E_INVARG on parse error

2. **`verb_ast(obj, verb)`** - Get verb AST directly
   - Extract code from verb on object (using existing verb system)
   - Use same permission checks as reading verbs
   - Call `parse_ast()` on the extracted code
   - Return MAP representation or E_PERM/E_VERBNF/E_INVARG

3. **`unparse_ast(ast_map)`** - Convert AST back to code
   - Validate MAP structure has correct "type" field
   - Recursively convert MAPs back to code strings via templating
   - Handle formatting (indentation, spacing)
   - Return LIST of code strings or E_INVARG on invalid AST

4. **`set_verb_ast(obj, verb, ast_map)`** - Write AST to verb
   - Call `unparse_ast()` to get code strings
   - Write code to verb using existing verb system
   - Use same permission checks as writing verbs
   - Return 1 on success or E_PERM/E_VERBNF/E_INVARG

5. **`validate_ast(ast_map)`** - Validate AST structure
   - Check MAP has required fields for its type
   - Validate child nodes recursively
   - Return 1 for valid, 0 for invalid

### Implementation Strategy

**Phase 1: Core parsing infrastructure**
- Implement `parse_ast({code})` for simple expressions: `1 + 2`, `foo()`, `x.y`
- Implement `unparse_ast(ast_map)` and `validate_ast(ast_map)`
- Get parse → unparse → parse roundtrip working
- Establish patterns for MAP conversion

**Phase 2: Verb integration**  
- Implement `verb_ast(obj, verb)` using existing verb system
- Implement `set_verb_ast(obj, verb, ast_map)` 
- Add comprehensive permission checking and error handling

**Phase 3: Full language support**
- Extend to statements, control flow, complex constructs
- Comprehensive testing and edge cases

**Phase 3: Polish & optimization**
- Performance tuning for large ASTs
- Better error messages
- Documentation and examples

### Key Design Decisions Made

✅ **Reuse existing parser** - Hook into `parse_program()`  
✅ **AST self-conversion** - Add conversion methods directly to AST structs in `ast.cc`
✅ **Simple MAP representation** - `["type" -> "...", ...]` pattern  
✅ **Templating for unparse** - Not decompilation, just code generation  
✅ **Use existing permissions** - Leverage verb read/write permissions  
✅ **TDD approach** - Start simple, build complexity incrementally

### Architecture Benefits

✅ **Clean encapsulation** - Conversion logic stays with AST data structures  
✅ **Maintainable** - Adding new AST nodes requires adding conversion methods  
✅ **Simple builtins** - Functions just call `expr_to_map()` / `map_to_expr()`  
✅ **Easy testing** - Test conversion for each AST node type independently  
✅ **Better organization** - Related code stays together in `ast.cc`

### Memory Management Strategy

**✅ Lean on MOO's Proven Memory System:**
- MOO has handled complex nested structures for decades
- Garbage collection and reference counting are battle-tested
- MAP/LIST operations are well-optimized

**✅ Clean Two-System Handoff (Same as Existing Parser):**
```c
// Existing parser pattern that works perfectly:
begin_code_allocation();        // AST pool for temporary structures
yyparse();                      // Create AST nodes in pool  
result = generate_code(ast);    // Convert AST to permanent structures
end_code_allocation(0);         // Free entire AST pool
return result;                  // Return permanent result

// Our pattern (identical approach):
begin_code_allocation();        // AST pool for temporary structures
yyparse();                      // Create AST nodes in pool
result = stmt_to_map(ast);      // Convert AST to MOO MAPs
end_code_allocation(0);         // Free entire AST pool  
return result;                  // Return MOO-managed MAPs
```

**✅ YAGNI Approach:**
- No need for artificial limits (recursion depth, etc.) until proven necessary
- Trust MOO's existing deep structure handling
- Start simple, add safeguards only if real problems emerge  

### Ready for Implementation

We have everything needed to start coding:
- ✅ AST structure understanding
- ✅ Parser integration points identified  
- ✅ Builtin function patterns documented
- ✅ Bidirectional conversion approach planned
- ✅ Security model (reuse existing permissions)
- ✅ Testing strategy outlined

## 🚀 Investigation & Planning Complete!

**What We've Accomplished:**
- ✅ Complete understanding of ToastStunt's AST architecture
- ✅ Clean, maintainable API design matching MOO conventions  
- ✅ Solid architectural approach with AST self-conversion
- ✅ Realistic memory management strategy
- ✅ Clear implementation phases and testing approach

**Key Technical Decisions:**
- **Reuse existing parser** - Hook into `parse_program()`, capture `prog_start`
- **AST self-conversion** - Add `expr_to_map()`/`map_to_expr()` methods to `ast.cc`
- **Trust MOO memory system** - Same two-system handoff pattern as existing parser
- **Clean API** - `parse_ast()`, `verb_ast()`, `set_verb_ast()`, etc.
- **YAGNI approach** - Start simple, add complexity only when needed

**Ready to implement with confidence!** All major design questions resolved.

## ToastStunt API Reference - CRITICAL FOR IMPLEMENTATION

**Map Operations (CORRECT API):**
```c
// WRONG: Var value = map_get(map, key);
// CORRECT:
Var value;
if (maplookup(map, key, &value, 0) == nullptr) {
    // Key not found, handle appropriately
    return nullptr; // or appropriate error handling
}
// value now contains the result

// WRONG: result = map_insert(result, key, value);
// CORRECT:
result = mapinsert(result, key, value);
```

**Memory Management (CORRECT API):**
```c
// AST allocation - use existing functions:
Expr *expr = alloc_expr(EXPR_PLUS);  // Basic allocation
Stmt *stmt = alloc_stmt(STMT_COND);  // Statement allocation

// CORRECT expression structure access:
expr->e.var = var_ref(value);        // For EXPR_VAR
expr->e.id = variable_id;            // For EXPR_ID  
expr->e.bin.lhs = left_expr;         // For binary operators
expr->e.bin.rhs = right_expr;
expr->e.expr = sub_expr;             // For unary operators

// Memory cleanup - find correct function names:
// free_expr() and free_stmt() - verify these exist or find alternatives
```

**Statement/Expression Types:**
```c
// Make sure to include proper headers for:
SizeOf_Stmt_Kind    // Size of statement enum
SizeOf_Expr_Kind    // Size of expression enum  
stmt_type_names[]   // Array of statement type names
expr_type_names[]   // Array of expression type names
```

**Key Implementation Notes:**
- maplookup() returns rbnode* (nullptr if not found) and stores value via pointer
- mapinsert() returns new map with inserted key/value
- All AST nodes allocated in AST pool via alloc_expr()/alloc_stmt()
- Stmt and Expr use union access patterns (stmt->s.cond, expr->e.bin, etc.)

## Detailed Implementation Information

### Parser Setup (ANSWER: How to call parse_program)

**Exact Parser Calling Pattern:**
```c
// From parse_list_as_program() - we can copy this exact pattern:
struct parser_state {
    Var code;           // LIST of code strings
    int cur_string;     // which string we're parsing
    int cur_char;       // which character in that string
    Var errors;         // LIST of error messages
};

// Parser client structure (already exists):
static Parser_Client list_parser_client = { my_error, 0, my_getc };

// How to call parser:
struct parser_state state;
state.code = code_list;          // MOO LIST of strings
state.cur_string = 1;            // Start at first string
state.cur_char = 0;              // Start at first character
state.errors = new_list(0);      // Empty error list

Program *program = parse_program(current_db_version, list_parser_client, &state);
// After parsing, prog_start global contains the AST
// state.errors contains any parse errors
```

**Key Insight:** We can reuse the existing `list_parser_client` and parser state setup. The parser expects a LIST of strings (same format as verb code).

### MOO Data Manipulation Functions (ANSWER: Verified functions)

**MAP Functions (verified in map.h):**
```c
extern Var new_map(void);                          // Create empty MAP
extern Var mapinsert(Var map, Var key, Var value); // Insert key/value pair  
extern const rbnode *maplookup(Var map, Var key, Var *value, int case_matters); // Find value
extern Num maplength(Var map);                     // Get MAP size
extern int mapempty(Var map);                      // Check if MAP is empty
```

**Var Creation (verified in structures.h and existing code):**
```c
// String Var (verified inline function in structures.h):
Var str_dup_to_var(const char *s);  // Creates string Var with str_dup()

// Manual string creation (pattern from crypto.cc:291):
Var str_var;
str_var.type = TYPE_STR;
str_var.v.str = str_dup("string_content");

// Number Var (pattern from list.cc:634):
Var num_var;
num_var.type = TYPE_INT;
num_var.v.num = 42;

// List creation (verified in list.cc:52):
Var new_list(int size);  // Creates LIST with specified size
```

**Memory Management (verified in utils.h):**
```c
extern void free_var(Var v);     // Free a Var
extern Var var_ref(Var v);       // Reference count increment  
extern Var var_dup(Var v);       // Duplicate a Var
```

### AST Expression Node Fields (ANSWER: Verified field mappings)

**EXPR_VAR and EXPR_ID (verified in ast.h and ast.cc):**
```c
// EXPR_VAR - literal values (numbers, strings, etc)
// MOO code: 42, "hello", 3.14, #1234, $nothing, E_PERM
// Uses: expr->e.var (Var type)
// Created by: alloc_var(var_type) in ast.cc
// Contains the actual literal value

// EXPR_ID - variable identifiers  
// MOO code: x, player, args, this, verb, caller
// Uses: expr->e.id (int type)
// The int is from find_id(char *name) - variable name lookup
// Contains variable ID number, not the actual value
```

**EXPR_PROP and EXPR_VERB (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// EXPR_PROP - property access (obj.prop)
// MOO code: player.name, this.location, #1.description
// Uses: expr->e.bin (struct Expr_Binary)  
// expr->e.bin.lhs = object expression
// expr->e.bin.rhs = property name expression
// Verified in code_gen.cc - generates OP_GET_PROP/OP_PUT_PROP

// EXPR_VERB - verb calls (obj:verb(args))  
// MOO code: player:tell("hi"), this:do_look(where)
// Uses: expr->e.verb (struct Expr_Verb)
// expr->e.verb.obj = object expression
// expr->e.verb.verb = verb name expression  
// expr->e.verb.args = Arg_List of arguments
```

**EXPR_INDEX and EXPR_RANGE (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// EXPR_INDEX - array/list indexing (obj[index])
// MOO code: list[1], args[i], data["key"], players[player]
// Uses: expr->e.bin (struct Expr_Binary)
// expr->e.bin.lhs = array/list expression
// expr->e.bin.rhs = index expression
// Verified in code_gen.cc:568, unparse.cc:662, parser.cc:2213 - generates OP_PUSH_REF/OP_INDEXSET

// EXPR_RANGE - range operations (obj[from..to])
// MOO code: list[1..3], args[2..$], data[start..end]
// Uses: expr->e.range (struct Expr_Range)
// expr->e.range.base = array/list expression 
// expr->e.range.from = start index expression
// expr->e.range.to = end index expression
// Verified in code_gen.cc:561, unparse.cc:669, parser.cc:2222-2225 - generates EOP_RANGESET
```

**EXPR_ASGN and EXPR_CALL (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// EXPR_ASGN - assignment expressions (lhs = rhs)
// MOO code: x = 5, player.name = "Bob", list[1] = value
// Uses: expr->e.bin (struct Expr_Binary)
// expr->e.bin.lhs = left-hand side expression (variable being assigned to)
// expr->e.bin.rhs = right-hand side expression (value being assigned)
// Verified in code_gen.cc:838, unparse.cc:737, parser.cc:2271 - generates various PUT opcodes

// EXPR_CALL - builtin function calls (func(args))
// MOO code: length(list), tostr(42), players(), time()
// Uses: expr->e.call (struct Expr_Call)
// expr->e.call.func = unsigned int (builtin function number from name_func_by_num())
// expr->e.call.args = Arg_List of arguments
// Verified in code_gen.cc:809, unparse.cc:743, parser.cc:2293 - generates OP_BI_FUNC_CALL
```

**Binary Arithmetic Operators (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// EXPR_PLUS, EXPR_MINUS, EXPR_TIMES, EXPR_DIVIDE, EXPR_MOD, EXPR_EXP - arithmetic operators
// MOO code: a + b, x - y, 3 * 4, total / count, n % 10, 2 ^ 8
// All use: expr->e.bin (struct Expr_Binary)
// expr->e.bin.lhs = left operand expression
// expr->e.bin.rhs = right operand expression
// Verified in code_gen.cc:692-705, unparse.cc:296-301, parser.cc:2316,2324,2332,2340,2348,2356
// Generate opcodes: OP_ADD, OP_MINUS, OP_MULT, OP_DIV, OP_MOD, EOP_EXP
// Precedence: EXP=9, TIMES/DIVIDE/MOD=8, PLUS/MINUS=7

// EXPR_NEGATE - unary negation operator (-)
// MOO code: -x, -5, -(a + b)
// Uses: expr->e.expr (single Expr* field)
// expr->e.expr = expression to negate
// Verified in code_gen.cc:643, unparse.cc:718, parser.cc:2491 - generates OP_UNARY_MINUS
// Precedence: 10
```

**Logical Operators (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// EXPR_AND, EXPR_OR - binary logical operators (&& ||)
// MOO code: x && y, valid(player) || admin(this), a && b && c
// Both use: expr->e.bin (struct Expr_Binary)
// expr->e.bin.lhs = left operand expression
// expr->e.bin.rhs = right operand expression  
// Verified in code_gen.cc:630-636, unparse.cc:288-289, parser.cc:2364,2372
// Generate opcodes: OP_AND, OP_OR with short-circuit evaluation
// Precedence: 3 (same for both)

// EXPR_NOT - unary logical negation operator (!)
// MOO code: !x, !valid(obj), !(a && b)
// Uses: expr->e.expr (single Expr* field)
// expr->e.expr = expression to negate
// Verified in code_gen.cc:644-646, unparse.cc:723, parser.cc:2501 - generates OP_NOT
// Precedence: 10
```

**Comparison Operators (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// EXPR_EQ, EXPR_NE, EXPR_LT, EXPR_LE, EXPR_GT, EXPR_GE, EXPR_IN - comparison operators
// MOO code: x == y, name != "Bob", age < 18, score >= 100, item in list
// All use: expr->e.bin (struct Expr_Binary)
// expr->e.bin.lhs = left operand expression
// expr->e.bin.rhs = right operand expression
// Verified in code_gen.cc:671-690, unparse.cc:290-295, parser.cc:2404,2412,2420,2428,2436,2444,2452
// Generate opcodes: OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_IN
// Operators: == != < <= > >= in
// Precedence: 4 (same for all comparison operators)
```

**Collection and Conditional Expressions (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// EXPR_LIST - list literals {a, b, c}
// MOO code: {1, 2, 3}, {player, this}, {@args, last}, {}
// Uses: expr->e.list (Arg_List* - linked list of arguments)
// expr->e.list->expr = expression for each list element  
// expr->e.list->kind = ARG_NORMAL or ARG_SPLICE (@rest)
// expr->e.list->next = pointer to next argument
// Verified in code_gen.cc:806, unparse.cc:754, parser.cc:2525 - generates OP_MAKE_EMPTY_LIST, OP_LIST_APPEND
// Precedence: 12

// EXPR_MAP - map literals [key1 -> val1, key2 -> val2] 
// MOO code: ["name" -> "Bob", "age" -> 25], [], [1 -> "first"]
// Uses: expr->e.map (Map_List* - linked list of key/value pairs)
// expr->e.map->key = expression for map key
// expr->e.map->value = expression for map value
// expr->e.map->next = pointer to next key/value pair
// Verified in code_gen.cc:803, unparse.cc:760, parser.cc:2534 - generates OP_MAP_CREATE, OP_MAP_INSERT
// Precedence: 12

// EXPR_COND - conditional expressions (condition ? consequent | alternate)
// MOO code: x > 0 ? x | -x, valid(obj) ? obj.name | "invalid"
// Uses: expr->e.cond (struct Expr_Cond)
// expr->e.cond.condition = test expression
// expr->e.cond.consequent = expression when true
// expr->e.cond.alternate = expression when false
// Verified in code_gen.cc:821, unparse.cc:710, parser.cc:2553 - generates conditional jumps
// Precedence: 2 (lower than most operators)
```

**Advanced Control Flow Expressions (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// EXPR_CATCH - exception handling expressions (`expr ! codes => default)
// MOO code: `obj.prop ! E_PROPNF => "none"`, `length(x) ! ANY`
// Uses: expr->e._catch (struct Expr_Catch)
// expr->e._catch._try = expression to evaluate (may throw)
// expr->e._catch.codes = Arg_List of error codes to catch
// expr->e._catch.except = default expression when exception caught (optional)
// Verified in code_gen.cc:920, unparse.cc:772, parser.cc:2564 - generates OP_CATCH, OP_TRY_EXCEPT
// Precedence: 12

// EXPR_SCATTER - scatter assignment expressions {?var1, @rest, var2} = list
// MOO code: {a, b, c} = list, {?opt, @rest} = args
// Uses: expr->e.scatter (Scatter* - linked list of scatter elements)
// expr->e.scatter->kind = SCAT_REQUIRED, SCAT_OPTIONAL, SCAT_REST
// expr->e.scatter->id = variable ID (from find_id)  
// expr->e.scatter->expr = optional default expression
// expr->e.scatter->next = pointer to next scatter element
// Verified in code_gen.cc:842, unparse.cc:766, parser.cc:2279 - generates EOP_SCATTER
// Used in assignment contexts for destructuring lists
```

**Bitwise Operators (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// EXPR_BITOR, EXPR_BITAND, EXPR_BITXOR, EXPR_BITSHL, EXPR_BITSHR - bitwise operators
// MOO code: a |. b, x &. y, n ^. m, bits << 2, value >> 1
// All use: expr->e.bin (struct Expr_Binary)
// expr->e.bin.lhs = left operand expression
// expr->e.bin.rhs = right operand expression
// Verified in code_gen.cc:732-745, unparse.cc:302-306, parser.cc:2380,2388,2396,2460,2468
// Generate opcodes: EOP_BITOR, EOP_BITAND, EOP_BITXOR, EOP_BITSHL, EOP_BITSHR
// Operators: |. &. ^. << >>
// Precedence: BITOR/BITAND/BITXOR=5, BITSHL/BITSHR=6

// EXPR_COMPLEMENT - unary bitwise complement operator (~)
// MOO code: ~x, ~flags, ~(a |. b)
// Uses: expr->e.expr (single Expr* field)
// expr->e.expr = expression to complement
// Verified in code_gen.cc:648, unparse.cc:728, parser.cc:2510 - generates EOP_COMPLEMENT
// Precedence: 10
```

### AST Statement Node Fields (ANSWER: Verified field mappings)

**Control Flow Statements (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// STMT_COND - conditional statements (if/elseif/else/endif)
// MOO code: if (x > 0) return x; elseif (x < 0) return -x; else return 0; endif
// Uses: stmt->s.cond (struct Stmt_Cond)
// stmt->s.cond.arms = Cond_Arm* (linked list of condition/statement pairs)
// stmt->s.cond.otherwise = Stmt* (else clause, optional)
// Cond_Arm: condition=Expr*, stmt=Stmt*, next=Cond_Arm*
// Verified in code_gen.cc:965, unparse.cc:504, parser.cc:1737

// STMT_WHILE - while loops
// MOO code: while (condition) body; endwhile, named: for x while (condition) body; endfor
// Uses: stmt->s.loop (struct Stmt_Loop)
// stmt->s.loop.id = int (variable ID for named loops, -1 for unnamed)
// stmt->s.loop.condition = Expr* (loop condition)
// stmt->s.loop.body = Stmt* (loop body statements)
// Verified in code_gen.cc:1041, unparse.cc:521, parser.cc:1821
```

**Loop Statements (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// STMT_LIST - for-in loops over lists
// MOO code: for x in (list) body; endfor, indexed: for x,i in (list) body; endfor
// Uses: stmt->s.list (struct Stmt_List)
// stmt->s.list.id = int (variable ID for loop variable)
// stmt->s.list.index = int (variable ID for index variable, -1 if not used)
// stmt->s.list.expr = Expr* (list/collection expression)
// stmt->s.list.body = Stmt* (loop body statements)
// Verified in code_gen.cc:990, unparse.cc:507, parser.cc:1756

// STMT_RANGE - for-range loops over numeric ranges
// MOO code: for x in [1..10] body; endfor
// Uses: stmt->s.range (struct Stmt_Range)
// stmt->s.range.id = int (variable ID for loop variable)
// stmt->s.range.from = Expr* (start of range)
// stmt->s.range.to = Expr* (end of range)
// stmt->s.range.body = Stmt* (loop body statements)
// Verified in code_gen.cc:1020, unparse.cc:510, parser.cc:1800
```

**Execution Control Statements (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// STMT_FORK - forking/threading statements
// MOO code: fork (5) body; endfork, named: fork x (5) body; endfork
// Uses: stmt->s.fork (struct Stmt_Fork)
// stmt->s.fork.id = int (variable ID for named forks, -1 for unnamed)
// stmt->s.fork.time = Expr* (delay time expression)
// stmt->s.fork.body = Stmt* (forked code statements)
// Verified in code_gen.cc:1065, unparse.cc:513, parser.cc:1861

// STMT_EXPR - expression statements
// MOO code: player:tell("hello");, x = 5;, length(list);
// Uses: stmt->s.expr (Expr*)
// stmt->s.expr = the expression to evaluate (for side effects)
// Verified in code_gen.cc:1076, unparse.cc:516, parser.cc:1893

// STMT_RETURN - return statements
// MOO code: return;, return value;, return x + y;
// Uses: stmt->s.expr (Expr*)
// stmt->s.expr = expression to return (NULL for bare return)
// Verified in code_gen.cc:1081, unparse.cc:535, parser.cc:1942
```

**Exception Handling Statements (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// STMT_TRY_EXCEPT - try-except blocks
// MOO code: try body; except err (E_PERM) handler; endtry
// Uses: stmt->s._catch (struct Stmt_Catch)
// stmt->s._catch.body = Stmt* (try block statements)
// stmt->s._catch.excepts = Except_Arm* (linked list of exception handlers)
// Except_Arm: id=int, codes=Arg_List*, stmt=Stmt*, next=Except_Arm*
// Verified in code_gen.cc:1089, unparse.cc:544, parser.cc:1966

// STMT_TRY_FINALLY - try-finally blocks  
// MOO code: try body; finally cleanup; endtry
// Uses: stmt->s.finally (struct Stmt_Finally)
// stmt->s.finally.body = Stmt* (try block statements)
// stmt->s.finally.handler = Stmt* (finally block statements)
// Verified in code_gen.cc:1126, unparse.cc:547, parser.cc:1976
```

**Loop Control Statements (verified in ast.h and code_gen.cc/unparse.cc):**
```c
// STMT_BREAK, STMT_CONTINUE - loop control
// MOO code: break;, break loop_name;, continue;, continue loop_name;
// Uses: stmt->s.exit (int)
// stmt->s.exit = -1 for unnamed, variable ID for named loop targets
// Both statements use the same union field but different kinds
// Verified in code_gen.cc:1145-1146, unparse.cc:559-560, parser.cc:1903,1923
```

## Function Call Representation

**EXPR_CALL** nodes use the `func` field to reference builtin functions by number:

- **Field**: `expr->e.call.func` (unsigned int)
- **Purpose**: Index into global builtin function table `bf_table[]`
- **Conversion**: Use `name_func_by_num(unsigned n)` to get function name
- **Reverse lookup**: Use `number_func_by_name(const char *name)` to get number
- **Valid range**: 0 to `top_bf_table-1` (currently max 256 functions)
- **Error value**: `FUNC_NOT_FOUND` (256) indicates invalid function

**Example mappings:**
```c
// Function number -> name lookup (src/functions.cc:178)
const char *name_func_by_num(unsigned n) {
    if (n >= top_bf_table)
        return "no such function";
    else
        return bf_table[n].name;
}

// Name -> function number lookup (src/functions.cc:187)  
unsigned number_func_by_name(const char *name) {
    for (i = 0; i < top_bf_table; i++)
        if (!strcasecmp(name, bf_table[i].name))
            return i;
    return FUNC_NOT_FOUND;
}
```

**MOO example**: `length(list)` becomes EXPR_CALL with func=N where bf_table[N].name=="length"

**Verification**: Used in unparse.cc:744 `name_func_by_num(expr->e.call.func)`

## Binary Operator Encoding

**Binary expressions** encode operators using the `kind` field directly - no separate operator encoding needed:

- **Field**: `expr->kind` (enum Expr_Kind)
- **Purpose**: The expression kind IS the operator type
- **Access**: Use `expr->e.bin.lhs` and `expr->e.bin.rhs` for operands
- **String mapping**: Use `binop_string[expr->kind]` array for operator text

**Complete operator mappings (src/unparse.cc:285-307):**
```c
// Operator kind -> string mappings
{EXPR_IN, " in "},        // membership test
{EXPR_OR, " || "},        // logical or  
{EXPR_AND, " && "},       // logical and
{EXPR_EQ, " == "},        // equality
{EXPR_NE, " != "},        // inequality
{EXPR_LT, " < "},         // less than
{EXPR_LE, " <= "},        // less or equal
{EXPR_GT, " > "},         // greater than
{EXPR_GE, " >= "},        // greater or equal
{EXPR_PLUS, " + "},       // addition
{EXPR_MINUS, " - "},      // subtraction
{EXPR_TIMES, " * "},      // multiplication
{EXPR_DIVIDE, " / "},     // division
{EXPR_MOD, " % "},        // modulo
{EXPR_EXP, " ^ "},        // exponentiation
{EXPR_BITOR, " |. "},     // bitwise or
{EXPR_BITAND, " &. "},    // bitwise and
{EXPR_BITXOR, " ^. "},    // bitwise xor
{EXPR_BITSHL, " << "},    // left shift
{EXPR_BITSHR, " >> "}     // right shift
```

**MOO examples**: 
- `a + b` becomes EXPR_PLUS with lhs=a, rhs=b
- `x && y` becomes EXPR_AND with lhs=x, rhs=y  
- `n << 2` becomes EXPR_BITSHL with lhs=n, rhs=2

**Verification**: Used in unparse.cc:699 `binop_string[expr->kind]`

## Verb Code Extraction and Setting Functions

**Key Functions for Verb Code Access:**

**Getting Verb Code (from bf_verb_code in verbs.cc:469):**
```c
// Core workflow: obj.verb -> db_verb_handle -> Program* -> LIST of strings
db_verb_handle h = find_described_verb(obj, desc);
if (!h.ptr) return E_VERBNF;
if (!db_verb_allows(h, progr, VF_READ)) return E_PERM;

Program *program = db_verb_program(h);  // Get compiled Program
Var code = new_list(0);
unparse_program(program, lister, &code, parens, indent, MAIN_VECTOR);
// Result: code is LIST of strings (verb source lines)
```

**Setting Verb Code (from bf_set_verb_code in verbs.cc:504):**
```c  
// Core workflow: LIST of strings -> Program* -> db_verb_handle
db_verb_handle h = find_described_verb(obj, desc);
if (!h.ptr) return E_VERBNF;
if (!is_programmer(progr) || !db_verb_allows(h, progr, VF_WRITE)) return E_PERM;

Program *program = parse_list_as_program(code, &errors);  // Parse strings to Program
if (program) {
    db_set_verb_program(h, program);  // Set compiled Program on verb
}
return errors;  // LIST of parse errors or empty list on success
```

**Essential Functions (verified signatures):**
```c
// From verbs.h:22 - Find verb by object and descriptor
db_verb_handle find_described_verb(Var obj, Var desc);

// From db.h:636-637 - Get/set compiled Program on verb  
Program *db_verb_program(db_verb_handle);
void db_set_verb_program(db_verb_handle, Program *);

// From unparse.h:29 - Convert Program to source lines
void unparse_program(Program *, Unparser_Receiver, void *data,
                    int fully_parenthesize, int indent_lines, int f_index);

// Callback function type for unparse_program (unparse.h:27)
typedef void (*Unparser_Receiver)(void *data, const char *line);

// Example receiver (verbs.cc:458) - appends lines to LIST
static void lister(void *data, const char *line) {
    Var *result = (Var *)data;
    Var v;
    v.type = TYPE_STR;
    v.v.str = str_dup(line);
    *result = listappend(*result, v);
}
```

**Permission Checking (verified in source):**
- **Read**: `db_verb_allows(h, progr, VF_READ)` - Same as reading verb code
- **Write**: `is_programmer(progr) && db_verb_allows(h, progr, VF_WRITE)` - Same as setting verb code

**Error Handling:**
- `E_VERBNF` - Verb not found
- `E_PERM` - Permission denied  
- `E_TYPE` - Invalid argument types
- `E_INVARG` - Invalid verb descriptor
- Parse errors returned as LIST from `parse_list_as_program()`

## Builtin Function Argument Parsing Patterns

**Standard Builtin Function Signature:**
```c
static package bf_function_name(Var arglist, Byte next, void *vdata, Objid progr)
```

**Argument List Structure:**
```c
// arglist.v.list[0].v.num = number of arguments passed
// arglist.v.list[1] = first argument
// arglist.v.list[2] = second argument  
// arglist.v.list[N] = Nth argument
```

**Common Argument Parsing Patterns:**

**Basic Argument Access:**
```c
int nargs = arglist.v.list[0].v.num;          // Get argument count
Var first_arg = arglist.v.list[1];            // Get first argument
Var second_arg = arglist.v.list[2];           // Get second argument
```

**Optional Arguments (from bf_verb_code verbs.cc:474):**
```c
int nargs = arglist.v.list[0].v.num;
Var obj = arglist.v.list[1];                  // Required argument
Var desc = arglist.v.list[2];                 // Required argument
int parens = nargs >= 3 && is_true(arglist.v.list[3]);     // Optional bool
int indent = nargs < 4 || is_true(arglist.v.list[4]);      // Optional bool (default true)
```

**Type Checking (from bf_is_member collection.cc:79):**
```c
Var rhs = arglist.v.list[2];
if (rhs.type != TYPE_LIST && rhs.type != TYPE_MAP) {
    free_var(arglist);
    return make_error_pack(E_INVARG);
}
```

**Object Validation (from bf_verb_code verbs.cc:480):**
```c
if (!obj.is_object()) {
    free_var(arglist);
    return make_error_pack(E_TYPE);
} else if (!is_valid(obj)) {
    free_var(arglist);
    return make_error_pack(E_INVARG);
}
```

**Error Return Patterns:**
```c
// Type errors
return make_error_pack(E_TYPE);

// Invalid argument errors  
return make_error_pack(E_INVARG);

// Permission errors
return make_error_pack(E_PERM);

// Success with value
return make_var_pack(result_var);

// Success without value
return no_var_pack();
```

**Memory Management:**
```c
// ALWAYS free arglist before returning
free_var(arglist);
return make_error_pack(E_TYPE);

// OR at end of function
free_var(arglist);
return make_var_pack(result);
```

**Function Registration (from verbs.cc:672, collection.cc:95):**
```c
// register_function(name, min_args, max_args, function_ptr, arg_types...)
register_function("verb_code", 2, 4, bf_verb_code,
                  TYPE_ANY, TYPE_ANY, TYPE_ANY, TYPE_ANY);
                  
register_function("is_member", 2, 3, bf_is_member, 
                  TYPE_ANY, TYPE_ANY, TYPE_INT);
```

**Registration Parameters:**
- `name` - Function name in MOO (string)
- `min_args` - Minimum required arguments  
- `max_args` - Maximum allowed arguments
- `function_ptr` - Pointer to bf_* function
- `arg_types...` - Expected types for each argument (TYPE_ANY, TYPE_STR, TYPE_INT, etc.)

## Symbol Table Mechanics

**Symbol Table Structure (sym_table.h:24-28):**
```c
typedef struct {
    unsigned max_size;
    unsigned size;
    const char **names;    // Array of variable name strings
} Names;
```

**Variable ID Assignment Process:**
```c
// In parser.y - find_id() is called for each variable reference
static int find_id(char *name) {
    int slot = find_or_add_name(&local_names, name);  // Add to symbol table
    dealloc_string(name);
    return slot;  // Return integer ID for this variable name
}

// find_or_add_name() in sym_table.cc:139 - case-insensitive lookup/add
unsigned find_or_add_name(Names **names, const char *str) {
    // Search existing names (case-insensitive)
    for (i = 0; i < (*names)->size; i++)
        if (!strcasecmp((*names)->names[i], str))
            return i;  // Return existing ID
    
    // Add new name, grow array if needed
    (*names)->names[(*names)->size] = str_dup(str);
    return (*names)->size++;  // Return new ID
}
```

**Built-in Variable IDs (sym_table.h:36-71):**
- **SLOT_PLAYER (5)** = "player"
- **SLOT_THIS (6)** = "this"  
- **SLOT_CALLER (7)** = "caller"
- **SLOT_VERB (8)** = "verb"
- **SLOT_ARGS (9)** = "args"
- Plus type constants: SLOT_NUM (0)="NUM", SLOT_STR (2)="STR", etc.
- **Database version specific**: SLOT_FLOAT (17), SLOT_MAP (18), SLOT_WAIF (20), etc.

**Symbol Table Lifecycle in parse_program() (parser.y:1225):**
```c
Program *parse_program(DB_Version version, Parser_Client c, void *data) {
    // 1. Initialize symbol table with builtins
    local_names = new_builtin_names(version);
    
    // 2. Parse code - find_id() adds user variables starting after builtins
    begin_code_allocation();
    yyparse();  // Sets prog_start to root AST node
    end_code_allocation(nerrors > 0);
    
    // 3. Symbol table now contains: [builtins...] [user_vars...]
    // 4. prog_start contains AST with variable IDs from symbol table
}
```

**Key Insights for AST Implementation:**
- **IDs are indices**: Variable ID N = `local_names->names[N]`
- **Built-ins first**: User variables get IDs starting after `first_user_slot(version)`
- **Case-insensitive**: Variable lookup uses `strcasecmp()`
- **Symbol table available**: After parsing, `local_names` contains complete name→ID mapping
- **AST has IDs**: EXPR_ID nodes contain integer IDs that index into `local_names`

**Access Functions (sym_table.h:30-34):**
```c
extern Names *new_builtin_names(DB_Version);     // Create builtin symbol table
extern int first_user_slot(DB_Version);          // First slot for user variables  
extern unsigned find_or_add_name(Names**, const char*);  // Add variable
extern int find_name(Names*, const char*);       // Lookup existing variable
extern void free_names(Names*);                  // Cleanup symbol table
```

## AST Memory Lifecycle Timing

**Critical AST Capture Window (from parse_program in parser.y:1230-1232):**
```c
Program *parse_program(DB_Version version, Parser_Client c, void *data) {
    // Setup
    local_names = new_builtin_names(version);
    
    // CRITICAL WINDOW STARTS HERE
    begin_code_allocation();    // Start temporary AST pool
    yyparse();                  // Creates AST, sets prog_start, builds local_names
    
    // *** CAPTURE POINT: Both prog_start and local_names are valid here ***
    // Must convert AST→MAP here before end_code_allocation()
    
    end_code_allocation(nerrors > 0);  // Frees AST pool (if errors OR success)
    // CRITICAL WINDOW ENDS HERE - prog_start becomes invalid
}
```

**AST Capture Strategy for our builtins:**
```c
static package bf_parse_ast(Var arglist, Byte next, void *vdata, Objid progr) {
    // Use modified parser that captures AST before cleanup
    begin_code_allocation();
    local_names = new_builtin_names(current_db_version);
    
    // Parse code
    yyparse();  // Sets prog_start, builds local_names
    
    if (nerrors == 0) {
        // CAPTURE AST HERE - both prog_start and local_names valid
        Var result = stmt_to_map(prog_start, local_names, options);
        end_code_allocation(0);  // Free AST pool
        return make_var_pack(result);
    } else {
        end_code_allocation(1);  // Free AST pool with errors
        return make_error_pack(E_INVARG);
    }
}
```

**Memory Management Rules:**
1. **AST Pool Lifetime**: From `begin_code_allocation()` to `end_code_allocation()`
2. **Capture Window**: After `yyparse()`, before `end_code_allocation()`  
3. **Symbol Table**: `local_names` remains valid until overwritten by next parse
4. **AST Pointers**: `prog_start` becomes invalid after `end_code_allocation()`
5. **Conversion Timing**: Must call `stmt_to_map()` within the capture window

**For MAP→AST Direction (unparse_ast):**
```c
static package bf_unparse_ast(Var arglist, Byte next, void *vdata, Objid progr) {
    // Create temporary AST pool for reconstruction
    begin_code_allocation();
    
    // Build AST from MAP
    Symtab *symtab = symtab_from_map(arglist);
    Stmt *stmt = map_to_stmt(arglist, symtab, options, &errors);
    
    if (stmt && !errors) {
        // Convert AST to code while still allocated
        Var result = unparse_stmt_to_var(stmt);
        end_code_allocation(0);  // Free AST pool
        return make_var_pack(result);
    } else {
        end_code_allocation(1);  // Free AST pool with errors
        return make_error_pack(E_INVARG);
    }
}
```

**Key Insights:**
- **Never return AST pointers** - Always convert to MOO data within the pool lifetime
- **Symbol table persists** - Can access `local_names` after AST cleanup
- **Pool allocation is fast** - Safe to create/destroy pools for each operation
- **Error handling** - Always call `end_code_allocation()` even on errors

## Unparse Integration Analysis

**Existing Unparse Infrastructure (unparse.cc):**
```c
// Public interface - only works with Program* (compiled bytecode)
void unparse_program(Program *, Unparser_Receiver, void *data,
                    int fully_parenthesize, int indent_lines, int f_index);

// Internal functions - static, but exactly what we need!
static void unparse_stmt(Stmt *stmt, int indent);    // Handles statement lists
static void unparse_expr(Stream *str, Expr *e);      // Handles single expressions  
```

**How Existing Unparse Works:**
```c
void unparse_program(Program *p, Unparser_Receiver r, void *data, ...) {
    Stmt *stmt = decompile_program(p, f_index);  // Convert bytecode→AST
    
    // Set global state for unparse functions
    prog = p;                    // For variable name lookup: prog->var_names[id]
    receiver = r;                // Callback function for output lines  
    receiver_data = data;        // Data passed to callback
    
    list_prg(stmt, ...);         // Calls unparse_stmt() internally
    free_stmt(stmt);
}

// Output function used throughout unparse
static void output(Stream *str) {
    (*receiver)(receiver_data, reset_stream(str));  // Send line to callback
}
```

**Problem**: Static functions + global state makes direct AST unparsing impossible from outside.

**Solution**: Add wrapper functions to unparse.h and unparse.cc:
```c
// Add to unparse.h:
extern void unparse_stmt_to_receiver(Stmt *stmt, Program *prog,
                                     Unparser_Receiver receiver, void *data,
                                     int indent);
extern void unparse_expr_to_receiver(Expr *expr, Program *prog,
                                     Unparser_Receiver receiver, void *data);

// Add to unparse.cc:
void unparse_stmt_to_receiver(Stmt *stmt, Program *prog_context,
                             Unparser_Receiver r, void *data, int indent) {
    // Set global state (same as unparse_program)
    prog = prog_context;          // Need Program* for variable names
    receiver = r;
    receiver_data = data;
    fully_parenthesize = 0;
    indent_code = 1;
    
    unparse_stmt(stmt, indent);   // Call existing static function
}
```

**Alternative Approach**: Create minimal Program* wrapper:
```c
// For our AST builtins - create minimal Program with just symbol table
Var unparse_stmt_to_var(Stmt *stmt, Names *symtab) {
    Program *temp_prog = create_minimal_program(symtab);  // Wrapper with var_names
    Var result = new_list(0);
    
    unparse_stmt_to_receiver(stmt, temp_prog, lister, &result, 0);
    free_minimal_program(temp_prog);
    return result;
}
```

**Key Insights:**
- **Reuse existing unparse logic** - Don't reimplement precedence/formatting
- **Need Program context** - Variable names come from `prog->var_names[id]`  
- **Add wrapper functions** - Expose static functions with proper setup
- **Maintain compatibility** - Don't change existing unparse_program interface


*This concludes the investigation and planning phase. Implementation can now begin with a solid technical foundation.*