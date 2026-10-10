/******************************************************************************
  Syntax trees as MOO values, and the built-in functions that expose them:
  parse_ast(), unparse_ast() and validate_ast().  verb_ast() lives in
  verbs.cc with the other verb built-ins.

  A program is a LIST of statement nodes.  A node is a MAP holding a "type"
  string and the fields of that type; docs/Features/ast.md lists them.
  Variables appear by name, so a tree can be written by hand.

  Trees come from the decompiler, which is also what verb_code() prints, and
  go back to code through the ordinary unparser.
 *****************************************************************************/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "ast.h"
#include "ast_map.h"
#include "bf_register.h"
#include "decompile.h"
#include "functions.h"
#include "list.h"
#include "log.h"
#include "map.h"
#include "parser.h"
#include "program.h"
#include "server.h"
#include "storage.h"
#include "structures.h"
#include "sym_table.h"
#include "unparse.h"
#include "utils.h"
#include "version.h"

/* A tree handed to us from MOO code may not nest deeper than this. */
#define AST_MAX_DEPTH 1000

/*
 * Field names and node types.  Each is one shared string, created when the
 * built-ins are registered.
 */
#define AST_STRINGS(S)							\
    S(type) S(op) S(lhs) S(rhs) S(expr) S(value) S(name) S(var) S(index)	\
    S(object) S(property) S(verb) S(function) S(args) S(base) S(from)	\
    S(to) S(target) S(items) S(entries) S(key) S(condition)		\
    S(consequent) S(alternate) S(codes) S(default) S(kind) S(required)	\
    S(optional) S(rest) S(arms) S(body) S(else) S(excepts) S(finally)	\
    S(delay)								\
    S(literal) S(variable) S(binary) S(unary) S(prop) S(verb_call)	\
    S(range) S(assign) S(scatter) S(call) S(list) S(map) S(conditional)	\
    S(catch) S(first) S(last) S(splice)					\
    S(if) S(for) S(for_range) S(while) S(fork) S(return) S(try_except)	\
    S(try_finally) S(break) S(continue)

#define DECLARE_STRING(s) static Var S_##s;
AST_STRINGS(DECLARE_STRING)
#undef DECLARE_STRING

static const struct {
    enum Expr_Kind kind;
    int arity;
    const char *op;
} operators[] = {
    {EXPR_PLUS, 2, "+"},
    {EXPR_MINUS, 2, "-"},
    {EXPR_TIMES, 2, "*"},
    {EXPR_DIVIDE, 2, "/"},
    {EXPR_MOD, 2, "%"},
    {EXPR_EXP, 2, "^"},
    {EXPR_AND, 2, "&&"},
    {EXPR_OR, 2, "||"},
    {EXPR_EQ, 2, "=="},
    {EXPR_NE, 2, "!="},
    {EXPR_LT, 2, "<"},
    {EXPR_LE, 2, "<="},
    {EXPR_GT, 2, ">"},
    {EXPR_GE, 2, ">="},
    {EXPR_IN, 2, "in"},
    {EXPR_BITOR, 2, "|."},
    {EXPR_BITAND, 2, "&."},
    {EXPR_BITXOR, 2, "^."},
    {EXPR_BITSHL, 2, "<<"},
    {EXPR_BITSHR, 2, ">>"},
    {EXPR_NEGATE, 1, "-"},
    {EXPR_NOT, 1, "!"},
    {EXPR_COMPLEMENT, 1, "~"}
};

/* Indexed by Expr_Kind; the arity is 0 for anything but an operator. */
static int operator_arity[SizeOf_Expr_Kind];
static Var operator_name[SizeOf_Expr_Kind];

/********** syntax tree to value **********/

static Var expr_to_var(Expr * expr, const char **names);
static Var stmts_to_var(Stmt * stmt, const char **names);

static Var
new_node(Var type)
{
    return mapinsert(new_map(), var_ref(S_type), var_ref(type));
}

static Var
put(Var map, Var key, Var value)
{   /* consumes `map' and `value' */
    return mapinsert(map, var_ref(key), value);
}

static Var
put_name(Var map, Var key, const char **names, int id)
{
    return put(map, key, str_ref_to_var(names[id]));
}

static Var
args_to_var(Arg_List * args, const char **names)
{
    Arg_List *arg;
    Var list;
    int n = 0;

    for (arg = args; arg; arg = arg->next)
        n++;
    list = new_list(n);
    for (arg = args, n = 0; arg; arg = arg->next) {
        Var item = expr_to_var(arg->expr, names);

        if (arg->kind == ARG_SPLICE)
            item = put(new_node(S_splice), S_expr, item);
        list.v.list[++n] = item;
    }
    return list;
}

static Var
map_list_to_var(Map_List * entries, const char **names)
{
    Map_List *entry;
    Var list;
    int n = 0;

    for (entry = entries; entry; entry = entry->next)
        n++;
    list = new_list(n);
    for (entry = entries, n = 0; entry; entry = entry->next) {
        Var item = new_map();

        item = put(item, S_key, expr_to_var(entry->key, names));
        item = put(item, S_value, expr_to_var(entry->value, names));
        list.v.list[++n] = item;
    }
    return list;
}

static Var
scatter_to_var(Scatter * scatter, const char **names)
{
    Scatter *sc;
    Var list;
    int n = 0;

    for (sc = scatter; sc; sc = sc->next)
        n++;
    list = new_list(n);
    for (sc = scatter, n = 0; sc; sc = sc->next) {
        Var item = new_map();

        item = put(item, S_kind, var_ref(sc->kind == SCAT_REQUIRED ? S_required
                                         : sc->kind == SCAT_OPTIONAL ? S_optional
                                         : S_rest));
        item = put_name(item, S_var, names, sc->id);
        if (sc->expr)
            item = put(item, S_default, expr_to_var(sc->expr, names));
        list.v.list[++n] = item;
    }
    return list;
}

static Var
expr_to_var(Expr * expr, const char **names)
{
    Var node;

    switch (expr->kind) {
        case EXPR_VAR:
            return put(new_node(S_literal), S_value, var_ref(expr->e.var));

        case EXPR_ID:
            return put_name(new_node(S_variable), S_name, names, expr->e.id);

        case EXPR_PROP:
            node = new_node(S_prop);
            node = put(node, S_object, expr_to_var(expr->e.bin.lhs, names));
            return put(node, S_property, expr_to_var(expr->e.bin.rhs, names));

        case EXPR_VERB:
            node = new_node(S_verb_call);
            node = put(node, S_object, expr_to_var(expr->e.verb.obj, names));
            node = put(node, S_verb, expr_to_var(expr->e.verb.verb, names));
            return put(node, S_args, args_to_var(expr->e.verb.args, names));

        case EXPR_INDEX:
            node = new_node(S_index);
            node = put(node, S_base, expr_to_var(expr->e.bin.lhs, names));
            return put(node, S_index, expr_to_var(expr->e.bin.rhs, names));

        case EXPR_RANGE:
            node = new_node(S_range);
            node = put(node, S_base, expr_to_var(expr->e.range.base, names));
            node = put(node, S_from, expr_to_var(expr->e.range.from, names));
            return put(node, S_to, expr_to_var(expr->e.range.to, names));

        case EXPR_ASGN:
            node = new_node(S_assign);
            node = put(node, S_target, expr_to_var(expr->e.bin.lhs, names));
            return put(node, S_value, expr_to_var(expr->e.bin.rhs, names));

        case EXPR_CALL:
            node = new_node(S_call);
            node = put(node, S_function,
                       str_dup_to_var(name_func_by_num(expr->e.call.func)));
            return put(node, S_args, args_to_var(expr->e.call.args, names));

        case EXPR_LIST:
            return put(new_node(S_list), S_items,
                       args_to_var(expr->e.list, names));

        case EXPR_MAP:
            return put(new_node(S_map), S_entries,
                       map_list_to_var(expr->e.map, names));

        case EXPR_SCATTER:
            return put(new_node(S_scatter), S_items,
                       scatter_to_var(expr->e.scatter, names));

        case EXPR_COND:
            node = new_node(S_conditional);
            node = put(node, S_condition,
                       expr_to_var(expr->e.cond.condition, names));
            node = put(node, S_consequent,
                       expr_to_var(expr->e.cond.consequent, names));
            return put(node, S_alternate,
                       expr_to_var(expr->e.cond.alternate, names));

        case EXPR_CATCH:
            node = new_node(S_catch);
            node = put(node, S_expr, expr_to_var(expr->e._catch._try, names));
            if (expr->e._catch.codes)
                node = put(node, S_codes,
                           args_to_var(expr->e._catch.codes, names));
            if (expr->e._catch.except)
                node = put(node, S_default,
                           expr_to_var(expr->e._catch.except, names));
            return node;

        case EXPR_FIRST:
            return new_node(S_first);

        case EXPR_LAST:
            return new_node(S_last);

        default:
            break;
    }

    if (operator_arity[expr->kind] == 2) {
        node = new_node(S_binary);
        node = put(node, S_op, var_ref(operator_name[expr->kind]));
        node = put(node, S_lhs, expr_to_var(expr->e.bin.lhs, names));
        return put(node, S_rhs, expr_to_var(expr->e.bin.rhs, names));
    } else if (operator_arity[expr->kind] == 1) {
        node = new_node(S_unary);
        node = put(node, S_op, var_ref(operator_name[expr->kind]));
        return put(node, S_expr, expr_to_var(expr->e.expr, names));
    }

    panic_moo("EXPR_TO_VAR: Unknown Expr_Kind");
    return zero;
}

static Var
stmt_to_var(Stmt * stmt, const char **names)
{
    Var node, list;
    Cond_Arm *arm;
    Except_Arm *ex;
    int n = 0;

    switch (stmt->kind) {
        case STMT_COND:
            for (arm = stmt->s.cond.arms; arm; arm = arm->next)
                n++;
            list = new_list(n);
            for (arm = stmt->s.cond.arms, n = 0; arm; arm = arm->next) {
                Var item = new_map();

                item = put(item, S_condition,
                           expr_to_var(arm->condition, names));
                item = put(item, S_body, stmts_to_var(arm->stmt, names));
                list.v.list[++n] = item;
            }
            node = put(new_node(S_if), S_arms, list);
            if (stmt->s.cond.otherwise)
                node = put(node, S_else,
                           stmts_to_var(stmt->s.cond.otherwise, names));
            return node;

        case STMT_LIST:
            node = put_name(new_node(S_for), S_var, names, stmt->s.list.id);
            if (stmt->s.list.index >= 0)
                node = put_name(node, S_index, names, stmt->s.list.index);
            node = put(node, S_expr, expr_to_var(stmt->s.list.expr, names));
            return put(node, S_body, stmts_to_var(stmt->s.list.body, names));

        case STMT_RANGE:
            node = put_name(new_node(S_for_range), S_var, names,
                            stmt->s.range.id);
            node = put(node, S_from, expr_to_var(stmt->s.range.from, names));
            node = put(node, S_to, expr_to_var(stmt->s.range.to, names));
            return put(node, S_body, stmts_to_var(stmt->s.range.body, names));

        case STMT_WHILE:
            node = new_node(S_while);
            if (stmt->s.loop.id >= 0)
                node = put_name(node, S_name, names, stmt->s.loop.id);
            node = put(node, S_condition,
                       expr_to_var(stmt->s.loop.condition, names));
            return put(node, S_body, stmts_to_var(stmt->s.loop.body, names));

        case STMT_FORK:
            node = new_node(S_fork);
            if (stmt->s.fork.id >= 0)
                node = put_name(node, S_var, names, stmt->s.fork.id);
            node = put(node, S_delay, expr_to_var(stmt->s.fork.time, names));
            return put(node, S_body, stmts_to_var(stmt->s.fork.body, names));

        case STMT_EXPR:
            return put(new_node(S_expr), S_expr,
                       expr_to_var(stmt->s.expr, names));

        case STMT_RETURN:
            node = new_node(S_return);
            if (stmt->s.expr)
                node = put(node, S_value, expr_to_var(stmt->s.expr, names));
            return node;

        case STMT_TRY_EXCEPT:
            for (ex = stmt->s._catch.excepts; ex; ex = ex->next)
                n++;
            list = new_list(n);
            for (ex = stmt->s._catch.excepts, n = 0; ex; ex = ex->next) {
                Var item = new_map();

                if (ex->id >= 0)
                    item = put_name(item, S_var, names, ex->id);
                if (ex->codes)
                    item = put(item, S_codes, args_to_var(ex->codes, names));
                item = put(item, S_body, stmts_to_var(ex->stmt, names));
                list.v.list[++n] = item;
            }
            node = new_node(S_try_except);
            node = put(node, S_body, stmts_to_var(stmt->s._catch.body, names));
            return put(node, S_excepts, list);

        case STMT_TRY_FINALLY:
            node = new_node(S_try_finally);
            node = put(node, S_body,
                       stmts_to_var(stmt->s.finally.body, names));
            return put(node, S_finally,
                       stmts_to_var(stmt->s.finally.handler, names));

        case STMT_BREAK:
        case STMT_CONTINUE:
            node = new_node(stmt->kind == STMT_BREAK ? S_break : S_continue);
            if (stmt->s.exit >= 0)
                node = put_name(node, S_name, names, stmt->s.exit);
            return node;
    }

    panic_moo("STMT_TO_VAR: Unknown Stmt_Kind");
    return zero;
}

static Var
stmts_to_var(Stmt * stmts, const char **names)
{
    Stmt *stmt;
    Var list;
    int n = 0;

    for (stmt = stmts; stmt; stmt = stmt->next)
        n++;
    list = new_list(n);
    for (stmt = stmts, n = 0; stmt; stmt = stmt->next)
        list.v.list[++n] = stmt_to_var(stmt, names);
    return list;
}

Var
program_to_ast(Program * program)
{
    Stmt *stmts = decompile_program(program, MAIN_VECTOR);
    Var ast = stmts_to_var(stmts, program->var_names);

    free_stmt(stmts);
    return ast;
}

/********** value to syntax tree **********/

/*
 * Everything is allocated between begin_code_allocation() and
 * end_code_allocation(), so a tree abandoned half-built is freed in one go.
 * A function here that fails has called fail() and returns 0 or a null
 * pointer; its callers add their own position to the path on the way out.
 */

struct Builder {
    Names *names;
    int depth;
    std::string error;		/* what is wrong */
    std::string path;		/* and where, outermost part first */
};

static Expr *build_expr(Builder * b, Var value);
static int build_stmts(Builder * b, Var list, Stmt ** stmts);

static void
fail(Builder * b, const char *fmt, ...)
{
    char buffer[200];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    b->error = buffer;
}

static void
at_key(Builder * b, Var key)
{
    b->path.insert(0, std::string(".") + key.v.str);
}

static void
at_index(Builder * b, int i)
{
    b->path.insert(0, "[" + std::to_string(i) + "]");
}

static int
find(Var map, Var key, Var * value)
{
    return maplookup(map, key, value, 1) != nullptr;
}

static int
find_required(Builder * b, Var map, Var key, Var * value)
{
    if (find(map, key, value))
        return 1;
    fail(b, "missing \"%s\"", key.v.str);
    return 0;
}

static int
check_type(Builder * b, Var value, var_type type)
{
    if (value.type == type)
        return 1;
    fail(b, "expected %s, found %s", parse_type(type), parse_type(value.type));
    return 0;
}

/* The "type" of a node, or null if `value' is not a node at all. */
static const char *
node_type(Builder * b, Var value)
{
    Var type;

    if (!check_type(b, value, TYPE_MAP))
        return nullptr;
    if (!find(value, S_type, &type) || type.type != TYPE_STR) {
        fail(b, "missing \"type\"");
        return nullptr;
    }
    return type.v.str;
}

static int
is_node(Var value, Var type)
{
    Var found;

    return value.type == TYPE_MAP && find(value, S_type, &found)
           && found.type == TYPE_STR && !strcmp(found.v.str, type.v.str);
}

static Expr *
expr_field(Builder * b, Var map, Var key)
{
    Var value;
    Expr *expr;

    if (!find_required(b, map, key, &value))
        return nullptr;
    if (!(expr = build_expr(b, value)))
        at_key(b, key);
    return expr;
}

/* `*expr' is null if the field is absent. */
static int
optional_expr_field(Builder * b, Var map, Var key, Expr ** expr)
{
    Var value;

    *expr = nullptr;
    if (!find(map, key, &value))
        return 1;
    if (!(*expr = build_expr(b, value)))
        at_key(b, key);
    return *expr != nullptr;
}

/* `*id' is -1 if an optional name is absent. */
static int
name_field(Builder * b, Var map, Var key, int optional, int *id)
{
    Var value;

    *id = -1;
    if (!find(map, key, &value)) {
        if (!optional)
            fail(b, "missing \"%s\"", key.v.str);
        return optional;
    }
    if (value.type != TYPE_STR || !ok_identifier(value.v.str)) {
        fail(b, "expected a variable name");
        at_key(b, key);
        return 0;
    }
    *id = find_or_add_name(&b->names, value.v.str);
    return 1;
}

static int
list_field(Builder * b, Var map, Var key, Var * list)
{
    if (!find_required(b, map, key, list))
        return 0;
    if (!check_type(b, *list, TYPE_LIST)) {
        at_key(b, key);
        return 0;
    }
    return 1;
}

static int
build_args(Builder * b, Var list, Arg_List ** args)
{
    Arg_List **tail = args;
    int i;

    *args = nullptr;
    for (i = 1; i <= list.v.list[0].v.num; i++) {
        Var item = list.v.list[i];
        enum Arg_Kind kind = is_node(item, S_splice) ? ARG_SPLICE : ARG_NORMAL;
        Expr *expr = (kind == ARG_SPLICE ? expr_field(b, item, S_expr)
                      : build_expr(b, item));

        if (!expr) {
            at_index(b, i);
            return 0;
        }
        *tail = alloc_arg_list(kind, expr);
        tail = &(*tail)->next;
    }
    return 1;
}

static int
args_field(Builder * b, Var map, Var key, Arg_List ** args)
{
    Var list;

    if (!list_field(b, map, key, &list))
        return 0;
    if (!build_args(b, list, args)) {
        at_key(b, key);
        return 0;
    }
    return 1;
}

/* The error codes of a catch expression or an except clause; null for ANY. */
static int
codes_field(Builder * b, Var map, Arg_List ** codes)
{
    Var list;
    int ok;

    *codes = nullptr;
    if (!find(map, S_codes, &list))
        return 1;
    if ((ok = check_type(b, list, TYPE_LIST)) && list.v.list[0].v.num == 0) {
        fail(b, "must not be empty; leave it out to catch ANY");
        ok = 0;
    }
    if (!ok || !build_args(b, list, codes)) {
        at_key(b, S_codes);
        return 0;
    }
    return 1;
}

static int
body_field(Builder * b, Var map, Var key, int optional, Stmt ** stmts)
{
    Var list;

    *stmts = nullptr;
    if (optional && !find(map, key, &list))
        return 1;
    if (!list_field(b, map, key, &list))
        return 0;
    if (!build_stmts(b, list, stmts)) {
        at_key(b, key);
        return 0;
    }
    return 1;
}

static Expr *
build_literal(Builder * b, Var node)
{
    Var value;
    Expr *expr;

    if (!find_required(b, node, S_value, &value))
        return nullptr;
    switch (value.type) {
        case TYPE_STR:
            expr = alloc_var(TYPE_STR);
            expr->e.var.v.str = alloc_string(value.v.str);
            return expr;
        case TYPE_INT:
        case TYPE_FLOAT:
        case TYPE_OBJ:
        case TYPE_ERR:
            expr = alloc_var(value.type);
            expr->e.var = value;
            return expr;
        default:
            fail(b, "a value of type %s cannot be written as a literal",
                 parse_type(value.type));
            at_key(b, S_value);
            return nullptr;
    }
}

static Expr *
build_variable(Builder * b, Var node)
{
    Expr *expr;
    int id;

    if (!name_field(b, node, S_name, 0, &id))
        return nullptr;
    expr = alloc_expr(EXPR_ID);
    expr->e.id = id;
    return expr;
}

static Expr *
build_operator(Builder * b, Var node, int arity)
{
    Var op;
    Expr *lhs, *rhs, *expr;
    unsigned i;

    if (!find_required(b, node, S_op, &op))
        return nullptr;
    for (i = 0; i < Arraysize(operators); i++)
        if (operators[i].arity == arity && op.type == TYPE_STR
                && !strcmp(op.v.str, operators[i].op))
            break;
    if (i == Arraysize(operators)) {
        fail(b, "not a %s operator", arity == 1 ? "unary" : "binary");
        at_key(b, S_op);
        return nullptr;
    }
    if (arity == 1) {
        if (!(lhs = expr_field(b, node, S_expr)))
            return nullptr;
        expr = alloc_expr(operators[i].kind);
        expr->e.expr = lhs;
        return expr;
    }
    if (!(lhs = expr_field(b, node, S_lhs))
            || !(rhs = expr_field(b, node, S_rhs)))
        return nullptr;
    return alloc_binary(operators[i].kind, lhs, rhs);
}

static Expr *
build_binary(Builder * b, Var node)
{
    return build_operator(b, node, 2);
}

static Expr *
build_unary(Builder * b, Var node)
{
    return build_operator(b, node, 1);
}

static Expr *
build_prop(Builder * b, Var node)
{
    Expr *object, *property;

    if (!(object = expr_field(b, node, S_object))
            || !(property = expr_field(b, node, S_property)))
        return nullptr;
    return alloc_binary(EXPR_PROP, object, property);
}

static Expr *
build_verb_call(Builder * b, Var node)
{
    Expr *object, *verb;
    Arg_List *args;

    if (!(object = expr_field(b, node, S_object))
            || !(verb = expr_field(b, node, S_verb))
            || !args_field(b, node, S_args, &args))
        return nullptr;
    return alloc_verb(object, verb, args);
}

static Expr *
build_index(Builder * b, Var node)
{
    Expr *base, *index;

    if (!(base = expr_field(b, node, S_base))
            || !(index = expr_field(b, node, S_index)))
        return nullptr;
    return alloc_binary(EXPR_INDEX, base, index);
}

static Expr *
build_range(Builder * b, Var node)
{
    Expr *base, *from, *to, *expr;

    if (!(base = expr_field(b, node, S_base))
            || !(from = expr_field(b, node, S_from))
            || !(to = expr_field(b, node, S_to)))
        return nullptr;
    expr = alloc_expr(EXPR_RANGE);
    expr->e.range.base = base;
    expr->e.range.from = from;
    expr->e.range.to = to;
    return expr;
}

static Scatter *
build_scatter_item(Builder * b, Var item)
{
    static const struct {
        Var *name;
        enum Scatter_Kind kind;
    } kinds[] = {
        {&S_required, SCAT_REQUIRED},
        {&S_optional, SCAT_OPTIONAL},
        {&S_rest, SCAT_REST}
    };
    Var kind;
    Expr *value;
    unsigned i;
    int id;

    if (!check_type(b, item, TYPE_MAP)
            || !find_required(b, item, S_kind, &kind))
        return nullptr;
    for (i = 0; i < Arraysize(kinds); i++)
        if (kind.type == TYPE_STR && !strcmp(kind.v.str, kinds[i].name->v.str))
            break;
    if (i == Arraysize(kinds)) {
        fail(b, "expected \"required\", \"optional\" or \"rest\"");
        at_key(b, S_kind);
        return nullptr;
    }
    if (!name_field(b, item, S_var, 0, &id)
            || !optional_expr_field(b, item, S_default, &value))
        return nullptr;
    if (value && kinds[i].kind != SCAT_OPTIONAL) {
        fail(b, "only an optional target can have a default");
        return nullptr;
    }
    return alloc_scatter(kinds[i].kind, id, value);
}

static Expr *
build_scatter(Builder * b, Var node)
{
    Var items;
    Scatter *first = nullptr, **tail = &first;
    Expr *expr;
    int i;

    if (!list_field(b, node, S_items, &items))
        return nullptr;
    for (i = 1; i <= items.v.list[0].v.num; i++) {
        if (!(*tail = build_scatter_item(b, items.v.list[i]))) {
            at_index(b, i);
            at_key(b, S_items);
            return nullptr;
        }
        tail = &(*tail)->next;
    }
    expr = alloc_expr(EXPR_SCATTER);
    expr->e.scatter = first;
    return expr;
}

/* The same test the parser applies to the left side of an assignment. */
static int
assignable(Expr * expr)
{
    if (expr->kind == EXPR_RANGE)
        expr = expr->e.range.base;
    while (expr->kind == EXPR_INDEX)
        expr = expr->e.bin.lhs;
    return expr->kind == EXPR_ID || expr->kind == EXPR_PROP;
}

static Expr *
build_assign(Builder * b, Var node)
{
    Var target;
    Expr *lhs, *rhs;

    if (!find_required(b, node, S_target, &target))
        return nullptr;
    if (is_node(target, S_scatter))
        lhs = build_scatter(b, target);
    else if ((lhs = build_expr(b, target)) && !assignable(lhs)) {
        fail(b, "cannot be assigned to");
        lhs = nullptr;
    }
    if (!lhs) {
        at_key(b, S_target);
        return nullptr;
    }
    if (!(rhs = expr_field(b, node, S_value)))
        return nullptr;
    return alloc_binary(EXPR_ASGN, lhs, rhs);
}

static Expr *
build_call(Builder * b, Var node)
{
    Var name;
    Arg_List *args;
    Expr *expr;
    unsigned func;

    if (!find_required(b, node, S_function, &name))
        return nullptr;
    if (name.type != TYPE_STR
            || (func = number_func_by_name(name.v.str)) == FUNC_NOT_FOUND) {
        fail(b, "not a built-in function");
        at_key(b, S_function);
        return nullptr;
    }
    if (!args_field(b, node, S_args, &args))
        return nullptr;
    expr = alloc_expr(EXPR_CALL);
    expr->e.call.func = func;
    expr->e.call.args = args;
    return expr;
}

static Expr *
build_list(Builder * b, Var node)
{
    Arg_List *items;
    Expr *expr;

    if (!args_field(b, node, S_items, &items))
        return nullptr;
    expr = alloc_expr(EXPR_LIST);
    expr->e.list = items;
    return expr;
}

static Expr *
build_map(Builder * b, Var node)
{
    Var entries;
    Map_List *first = nullptr, **tail = &first;
    Expr *key, *value, *expr;
    int i;

    if (!list_field(b, node, S_entries, &entries))
        return nullptr;
    for (i = 1; i <= entries.v.list[0].v.num; i++) {
        Var entry = entries.v.list[i];

        if (!check_type(b, entry, TYPE_MAP)
                || !(key = expr_field(b, entry, S_key))
                || !(value = expr_field(b, entry, S_value))) {
            at_index(b, i);
            at_key(b, S_entries);
            return nullptr;
        }
        *tail = alloc_map_list(key, value);
        tail = &(*tail)->next;
    }
    expr = alloc_expr(EXPR_MAP);
    expr->e.map = first;
    return expr;
}

static Expr *
build_conditional(Builder * b, Var node)
{
    Expr *condition, *consequent, *alternate, *expr;

    if (!(condition = expr_field(b, node, S_condition))
            || !(consequent = expr_field(b, node, S_consequent))
            || !(alternate = expr_field(b, node, S_alternate)))
        return nullptr;
    expr = alloc_expr(EXPR_COND);
    expr->e.cond.condition = condition;
    expr->e.cond.consequent = consequent;
    expr->e.cond.alternate = alternate;
    return expr;
}

static Expr *
build_catch(Builder * b, Var node)
{
    Expr *_try, *except, *expr;
    Arg_List *codes;

    if (!(_try = expr_field(b, node, S_expr))
            || !codes_field(b, node, &codes)
            || !optional_expr_field(b, node, S_default, &except))
        return nullptr;
    expr = alloc_expr(EXPR_CATCH);
    expr->e._catch._try = _try;
    expr->e._catch.codes = codes;
    expr->e._catch.except = except;
    return expr;
}

static Expr *
build_first(Builder * b, Var node)
{
    return alloc_expr(EXPR_FIRST);
}

static Expr *
build_last(Builder * b, Var node)
{
    return alloc_expr(EXPR_LAST);
}

static Expr *
build_misplaced_scatter(Builder * b, Var node)
{
    fail(b, "a scatter can only be the target of an assignment");
    return nullptr;
}

static Expr *
build_misplaced_splice(Builder * b, Var node)
{
    fail(b, "a splice can only appear in an argument list or a list");
    return nullptr;
}

static const struct {
    Var *type;
    Expr *(*build) (Builder *, Var);
} expr_builders[] = {
    {&S_literal, build_literal},
    {&S_variable, build_variable},
    {&S_binary, build_binary},
    {&S_unary, build_unary},
    {&S_prop, build_prop},
    {&S_verb_call, build_verb_call},
    {&S_index, build_index},
    {&S_range, build_range},
    {&S_assign, build_assign},
    {&S_call, build_call},
    {&S_list, build_list},
    {&S_map, build_map},
    {&S_conditional, build_conditional},
    {&S_catch, build_catch},
    {&S_first, build_first},
    {&S_last, build_last},
    {&S_scatter, build_misplaced_scatter},
    {&S_splice, build_misplaced_splice}
};

static Expr *
build_expr(Builder * b, Var value)
{
    const char *type = node_type(b, value);
    Expr *expr = nullptr;
    unsigned i;

    if (!type)
        return nullptr;
    if (++b->depth > AST_MAX_DEPTH)
        fail(b, "nested more than %d levels deep", AST_MAX_DEPTH);
    else {
        for (i = 0; i < Arraysize(expr_builders); i++)
            if (!strcmp(type, expr_builders[i].type->v.str))
                break;
        if (i < Arraysize(expr_builders))
            expr = (*expr_builders[i].build) (b, value);
        else
            fail(b, "\"%s\" is not an expression", type);
    }
    b->depth--;
    return expr;
}

static Stmt *
build_if(Builder * b, Var node)
{
    Var arms;
    Cond_Arm *first = nullptr, **tail = &first;
    Stmt *body, *stmt;
    Expr *condition;
    int i;

    if (!list_field(b, node, S_arms, &arms))
        return nullptr;
    if (arms.v.list[0].v.num == 0) {
        fail(b, "must not be empty");
        at_key(b, S_arms);
        return nullptr;
    }
    for (i = 1; i <= arms.v.list[0].v.num; i++) {
        Var arm = arms.v.list[i];

        if (!check_type(b, arm, TYPE_MAP)
                || !(condition = expr_field(b, arm, S_condition))
                || !body_field(b, arm, S_body, 0, &body)) {
            at_index(b, i);
            at_key(b, S_arms);
            return nullptr;
        }
        *tail = alloc_cond_arm(condition, body);
        tail = &(*tail)->next;
    }
    if (!body_field(b, node, S_else, 1, &body))
        return nullptr;
    stmt = alloc_stmt(STMT_COND);
    stmt->s.cond.arms = first;
    stmt->s.cond.otherwise = body;
    return stmt;
}

static Stmt *
build_for(Builder * b, Var node)
{
    Stmt *body, *stmt;
    Expr *expr;
    int id, index;

    if (!name_field(b, node, S_var, 0, &id)
            || !name_field(b, node, S_index, 1, &index)
            || !(expr = expr_field(b, node, S_expr))
            || !body_field(b, node, S_body, 0, &body))
        return nullptr;
    stmt = alloc_stmt(STMT_LIST);
    stmt->s.list.id = id;
    stmt->s.list.index = index;
    stmt->s.list.expr = expr;
    stmt->s.list.body = body;
    return stmt;
}

static Stmt *
build_for_range(Builder * b, Var node)
{
    Stmt *body, *stmt;
    Expr *from, *to;
    int id;

    if (!name_field(b, node, S_var, 0, &id)
            || !(from = expr_field(b, node, S_from))
            || !(to = expr_field(b, node, S_to))
            || !body_field(b, node, S_body, 0, &body))
        return nullptr;
    stmt = alloc_stmt(STMT_RANGE);
    stmt->s.range.id = id;
    stmt->s.range.from = from;
    stmt->s.range.to = to;
    stmt->s.range.body = body;
    return stmt;
}

static Stmt *
build_while(Builder * b, Var node)
{
    Stmt *body, *stmt;
    Expr *condition;
    int id;

    if (!name_field(b, node, S_name, 1, &id)
            || !(condition = expr_field(b, node, S_condition))
            || !body_field(b, node, S_body, 0, &body))
        return nullptr;
    stmt = alloc_stmt(STMT_WHILE);
    stmt->s.loop.id = id;
    stmt->s.loop.condition = condition;
    stmt->s.loop.body = body;
    return stmt;
}

static Stmt *
build_fork(Builder * b, Var node)
{
    Stmt *body, *stmt;
    Expr *delay;
    int id;

    if (!name_field(b, node, S_var, 1, &id)
            || !(delay = expr_field(b, node, S_delay))
            || !body_field(b, node, S_body, 0, &body))
        return nullptr;
    stmt = alloc_stmt(STMT_FORK);
    stmt->s.fork.id = id;
    stmt->s.fork.time = delay;
    stmt->s.fork.body = body;
    return stmt;
}

static Stmt *
build_expr_stmt(Builder * b, Var node)
{
    Stmt *stmt;
    Expr *expr;

    if (!(expr = expr_field(b, node, S_expr)))
        return nullptr;
    stmt = alloc_stmt(STMT_EXPR);
    stmt->s.expr = expr;
    return stmt;
}

static Stmt *
build_return(Builder * b, Var node)
{
    Stmt *stmt;
    Expr *value;

    if (!optional_expr_field(b, node, S_value, &value))
        return nullptr;
    stmt = alloc_stmt(STMT_RETURN);
    stmt->s.expr = value;
    return stmt;
}

static Stmt *
build_try_except(Builder * b, Var node)
{
    Var excepts;
    Except_Arm *first = nullptr, **tail = &first;
    Stmt *body, *handler, *stmt;
    Arg_List *codes;
    int i, id;

    if (!body_field(b, node, S_body, 0, &body)
            || !list_field(b, node, S_excepts, &excepts))
        return nullptr;
    if (excepts.v.list[0].v.num == 0) {
        fail(b, "must not be empty");
        at_key(b, S_excepts);
        return nullptr;
    }
    for (i = 1; i <= excepts.v.list[0].v.num; i++) {
        Var except = excepts.v.list[i];

        if (!check_type(b, except, TYPE_MAP)
                || !name_field(b, except, S_var, 1, &id)
                || !codes_field(b, except, &codes)
                || !body_field(b, except, S_body, 0, &handler)) {
            at_index(b, i);
            at_key(b, S_excepts);
            return nullptr;
        }
        *tail = alloc_except(id, codes, handler);
        tail = &(*tail)->next;
    }
    stmt = alloc_stmt(STMT_TRY_EXCEPT);
    stmt->s._catch.body = body;
    stmt->s._catch.excepts = first;
    return stmt;
}

static Stmt *
build_try_finally(Builder * b, Var node)
{
    Stmt *body, *handler, *stmt;

    if (!body_field(b, node, S_body, 0, &body)
            || !body_field(b, node, S_finally, 0, &handler))
        return nullptr;
    stmt = alloc_stmt(STMT_TRY_FINALLY);
    stmt->s.finally.body = body;
    stmt->s.finally.handler = handler;
    return stmt;
}

static Stmt *
build_loop_exit(Builder * b, Var node, enum Stmt_Kind kind)
{
    Stmt *stmt;
    int id;

    if (!name_field(b, node, S_name, 1, &id))
        return nullptr;
    stmt = alloc_stmt(kind);
    stmt->s.exit = id;
    return stmt;
}

static Stmt *
build_break(Builder * b, Var node)
{
    return build_loop_exit(b, node, STMT_BREAK);
}

static Stmt *
build_continue(Builder * b, Var node)
{
    return build_loop_exit(b, node, STMT_CONTINUE);
}

static const struct {
    Var *type;
    Stmt *(*build) (Builder *, Var);
} stmt_builders[] = {
    {&S_if, build_if},
    {&S_for, build_for},
    {&S_for_range, build_for_range},
    {&S_while, build_while},
    {&S_fork, build_fork},
    {&S_expr, build_expr_stmt},
    {&S_return, build_return},
    {&S_try_except, build_try_except},
    {&S_try_finally, build_try_finally},
    {&S_break, build_break},
    {&S_continue, build_continue}
};

static Stmt *
build_stmt(Builder * b, Var value)
{
    const char *type = node_type(b, value);
    Stmt *stmt = nullptr;
    unsigned i;

    if (!type)
        return nullptr;
    if (++b->depth > AST_MAX_DEPTH)
        fail(b, "nested more than %d levels deep", AST_MAX_DEPTH);
    else {
        for (i = 0; i < Arraysize(stmt_builders); i++)
            if (!strcmp(type, stmt_builders[i].type->v.str))
                break;
        if (i < Arraysize(stmt_builders))
            stmt = (*stmt_builders[i].build) (b, value);
        else
            fail(b, "\"%s\" is not a statement", type);
    }
    b->depth--;
    return stmt;
}

static int
build_stmts(Builder * b, Var list, Stmt ** stmts)
{
    Stmt **tail = stmts;
    int i;

    *stmts = nullptr;
    for (i = 1; i <= list.v.list[0].v.num; i++) {
        if (!(*tail = build_stmt(b, list.v.list[i]))) {
            at_index(b, i);
            return 0;
        }
        tail = &(*tail)->next;
    }
    return 1;
}

/*
 * Build the statements of `ast'.  On success the caller owns `*stmts' and
 * `b->names'; on failure there is nothing to free and `b' says what went
 * wrong.
 */
static int
ast_to_stmts(Builder * b, Var ast, Stmt ** stmts)
{
    int ok;

    b->names = new_builtin_names(current_db_version);
    b->depth = 0;
    begin_code_allocation();
    ok = check_type(b, ast, TYPE_LIST) && build_stmts(b, ast, stmts);
    end_code_allocation(!ok);
    if (!ok)
        free_names(b->names);
    return ok;
}

/********** built-in functions **********/

static void
add_line(void *data, const char *line)
{
    Var *code = (Var *) data;

    *code = listappend(*code, str_dup_to_var(line));
}

static package
bf_parse_ast(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (code) */
    Var code = arglist.v.list[1];
    Var errors, ast;
    Program *program;
    int i;

    for (i = 1; i <= code.v.list[0].v.num; i++)
        if (code.v.list[i].type != TYPE_STR) {
            free_var(arglist);
            return make_error_pack(E_TYPE);
        }
    program = parse_list_as_program(code, &errors);
    free_var(arglist);

    if (!program)
        return make_raise_pack(E_INVARG, errors.v.list[0].v.num > 0
                               ? errors.v.list[1].v.str : "Parse error",
                               errors);
    free_var(errors);
    ast = program_to_ast(program);
    free_program(program);

    return make_var_pack(ast);
}

static package
bf_unparse_ast(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (ast [, fully-paren [, indent]]) */
    int nargs = arglist.v.list[0].v.num;
    int parens = nargs >= 2 && is_true(arglist.v.list[2]);
    int indent = nargs < 3 || is_true(arglist.v.list[3]);
    Builder b;
    Stmt *stmts;
    Var code;
    int ok = ast_to_stmts(&b, arglist.v.list[1], &stmts);

    free_var(arglist);
    if (!ok) {
        std::string message = "Invalid AST";

        if (b.path.length() > 200)
            b.path.replace(200, std::string::npos, "...");
        if (!b.path.empty())
            message += " at " + b.path;
        message += ": " + b.error;
        return make_raise_pack(E_INVARG, message.c_str(), var_ref(zero));
    }

    code = new_list(0);
    unparse_stmts(stmts, b.names->names, add_line, &code, parens, indent);
    free_stmt(stmts);
    free_names(b.names);

    return make_var_pack(code);
}

static package
bf_validate_ast(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (ast) */
    Builder b;
    Stmt *stmts;
    int ok = ast_to_stmts(&b, arglist.v.list[1], &stmts);

    free_var(arglist);
    if (ok) {
        free_stmt(stmts);
        free_names(b.names);
    }

    return make_var_pack(Var::new_int(ok));
}

void
register_ast(void)
{
    unsigned i;

#define CREATE_STRING(s) S_##s = str_dup_to_var(#s);
    AST_STRINGS(CREATE_STRING)
#undef CREATE_STRING

    for (i = 0; i < Arraysize(operators); i++) {
        operator_arity[operators[i].kind] = operators[i].arity;
        operator_name[operators[i].kind] = str_dup_to_var(operators[i].op);
    }

    register_function("parse_ast", 1, 1, bf_parse_ast, TYPE_LIST);
    register_function("unparse_ast", 1, 3, bf_unparse_ast,
                      TYPE_LIST, TYPE_ANY, TYPE_ANY);
    register_function("validate_ast", 1, 1, bf_validate_ast, TYPE_ANY);
}
