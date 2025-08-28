/******************************************************************************
  ToastStunt AST Map Converter
  
  This module provides conversion between AST nodes and MOO Var (LIST/MAP) representations.
  Implements the visitor pattern for type-safe AST traversal and conversion.
  
  Functions:
  - expr_to_map(Expr *expr, Symtab *symtab) -> Var MAP
  - stmt_to_map(Stmt *stmt, Symtab *symtab) -> Var MAP  
  - map_to_expr(Var map, Symtab *symtab) -> Expr*
  - map_to_stmt(Var map, Symtab *symtab) -> Stmt*
  
  Schema Version: 1 (using typed schema with lowercase keys)
  Memory: All Expr/Stmt pointers allocated in AST pool, Var results use MOO GC
 *****************************************************************************/

#include "ast_map.h"
#include "ast.h"
#include "functions.h"
#include "list.h"
#include "log.h"
#include "map.h"
#include "parser.h"
#include "storage.h"
#include "streams.h"
#include "structures.h"
#include "unparse.h"
#include "utils.h"
#include "version.h"

// Forward declarations

// AST Schema Version - increment when breaking changes occur
static const int AST_SCHEMA_VERSION = 1;

// External parser globals 
extern void *parser_data;
extern Parser_Client parser_client;

// Parser state structure (from parser.y)
struct parser_state {
    Var         code;           /* a list of strings */
    int         cur_string;     /* which string? */
    int         cur_char;       /* which character in that string? */
    Var         errors;         /* a list of strings */
};

// Standard schema field names (lowercase for consistency)
static const char *FIELD_TYPE = "type";
static const char *FIELD_VERSION = "ast_version";
static const char *FIELD_SYMTAB = "variables";

// Size constants for arrays (match enum sizes)
static const int SizeOf_Stmt_Kind = 11; // STMT_COND through STMT_CONTINUE

// Expression type strings (must match enum Expr_Kind order)
static const char *expr_type_names[SizeOf_Expr_Kind] = {
    "var",        // EXPR_VAR      (0)
    "id",         // EXPR_ID       (1)
    "prop",       // EXPR_PROP     (2)
    "verb",       // EXPR_VERB     (3)
    "index",      // EXPR_INDEX    (4)
    "range",      // EXPR_RANGE    (5)
    "asgn",       // EXPR_ASGN     (6)
    "call",       // EXPR_CALL     (7)
    "plus",       // EXPR_PLUS     (8)
    "minus",      // EXPR_MINUS    (9)
    "times",      // EXPR_TIMES    (10)
    "divide",     // EXPR_DIVIDE   (11)
    "mod",        // EXPR_MOD      (12)
    "exp",        // EXPR_EXP      (13)
    "negate",     // EXPR_NEGATE   (14)
    "and",        // EXPR_AND      (15)
    "or",         // EXPR_OR       (16)
    "not",        // EXPR_NOT      (17)
    "eq",         // EXPR_EQ       (18)
    "ne",         // EXPR_NE       (19)
    "lt",         // EXPR_LT       (20)
    "le",         // EXPR_LE       (21)
    "gt",         // EXPR_GT       (22)
    "ge",         // EXPR_GE       (23)
    "in",         // EXPR_IN       (24)
    "list",       // EXPR_LIST     (25)
    "cond",       // EXPR_COND     (26)
    "catch",      // EXPR_CATCH    (27)
    "length",     // EXPR_LENGTH   (28) - retired
    "scatter",    // EXPR_SCATTER  (29)
    "map",        // EXPR_MAP      (30)
    "first",      // EXPR_FIRST    (31)
    "last",       // EXPR_LAST     (32)
    "bitor",      // EXPR_BITOR    (33)
    "bitand",     // EXPR_BITAND   (34)
    "bitxor",     // EXPR_BITXOR   (35)
    "bitshl",     // EXPR_BITSHL   (36)
    "bitshr",     // EXPR_BITSHR   (37)
    "complement"  // EXPR_COMPLEMENT (38)
};

// Statement type strings (must match enum Stmt_Kind order)  
static const char *stmt_type_names[SizeOf_Stmt_Kind] = {
    "cond",       // STMT_COND
    "list",       // STMT_LIST
    "range",      // STMT_RANGE
    "while",      // STMT_WHILE
    "fork",       // STMT_FORK
    "expr",       // STMT_EXPR
    "return",     // STMT_RETURN
    "try_except", // STMT_TRY_EXCEPT
    "try_finally",// STMT_TRY_FINALLY
    "break",      // STMT_BREAK
    "continue"    // STMT_CONTINUE
};

/*********** Symbol Table Management ***********/

// Convert symbol table to MOO LIST of STRs for serialization
static Var symtab_to_list(Symtab *symtab) {
    if (!symtab || symtab->num_names == 0) {
        return new_list(0);
    }
    
    Var list = new_list(symtab->num_names);
    for (int i = 0; i < symtab->num_names; i++) {
        list.v.list[i + 1].type = TYPE_STR;
        list.v.list[i + 1].v.str = str_ref(symtab->names[i]);
    }
    return list;
}

// Convert LIST back to Symtab
static Symtab *list_to_symtab(Var list) {
    if (list.type != TYPE_LIST) {
        return create_symtab(0); // Return empty symtab on error
    }
    
    int num_names = list.v.list[0].v.num;
    Symtab *symtab = create_symtab(num_names);
    
    for (int i = 0; i < num_names; i++) {
        if (list.v.list[i + 1].type == TYPE_STR) {
            symtab->names[i] = str_dup(list.v.list[i + 1].v.str);
        } else {
            symtab->names[i] = str_dup(""); // Fallback for invalid entries
        }
    }
    
    return symtab;
}

// Extract symbol table from AST MAP
static Symtab *extract_symtab_from_map(Var map) {
    Var symtab_var;
    if (maplookup(map, str_dup_to_var(FIELD_SYMTAB), &symtab_var, 0) == nullptr) {
        return create_symtab(0); // No symtab field, return empty
    }
    
    return list_to_symtab(symtab_var);
}


/*********** Validation Helpers ***********/

// Validate MAP has required type field with expected value
static int validate_type_field(Var map, const char *expected_type) {
    if (map.type != TYPE_MAP) {
        return 0;
    }
    
    Var type_field;
    if (maplookup(map, str_dup_to_var(FIELD_TYPE), &type_field, 0) == nullptr) {
        return 0;
    }
    if (type_field.type != TYPE_STR) {
        return 0;
    }
    
    return strcmp(type_field.v.str, expected_type) == 0;
}

// Validate and extract AST version from MAP
static int validate_version(Var map) {
    if (map.type != TYPE_MAP) {
        return 0;
    }
    
    Var version_field;
    if (maplookup(map, str_dup_to_var(FIELD_VERSION), &version_field, 0) == nullptr) {
        return 0;
    }
    if (version_field.type != TYPE_INT) {
        return 0;
    }
    
    return version_field.v.num == AST_SCHEMA_VERSION;
}

/*********** Expression Conversion: AST → MAP ***********/

static Var expr_to_map_visitor(Expr *expr, Symtab *symtab);
static Var arg_list_to_list(Arg_List *args, Symtab *symtab);
static Arg_List *list_to_arg_list(Var list, Symtab *symtab);

// Convert literal Var to MAP representation
static Var var_literal_to_map(Var literal) {
    Var result = new_map();
    
    // Set type field
    result = mapinsert(result, str_dup_to_var(FIELD_TYPE), str_dup_to_var("literal"));
    
    // Set value based on type
    switch (literal.type) {
        case TYPE_INT:
            result = mapinsert(result, str_dup_to_var("value_type"), str_dup_to_var("int"));
            result = mapinsert(result, str_dup_to_var("value"), var_ref(literal));
            break;
        case TYPE_FLOAT:
            result = mapinsert(result, str_dup_to_var("value_type"), str_dup_to_var("float"));
            result = mapinsert(result, str_dup_to_var("value"), var_ref(literal));
            break;
        case TYPE_STR:
            result = mapinsert(result, str_dup_to_var("value_type"), str_dup_to_var("str"));
            result = mapinsert(result, str_dup_to_var("value"), var_ref(literal));
            break;
        case TYPE_OBJ:
            result = mapinsert(result, str_dup_to_var("value_type"), str_dup_to_var("obj"));
            result = mapinsert(result, str_dup_to_var("value"), var_ref(literal));
            break;
        case TYPE_ERR:
            result = mapinsert(result, str_dup_to_var("value_type"), str_dup_to_var("err"));
            result = mapinsert(result, str_dup_to_var("value"), var_ref(literal));
            break;
        default:
            // Unsupported literal type
            result = mapinsert(result, str_dup_to_var("value_type"), str_dup_to_var("unknown"));
            result = mapinsert(result, str_dup_to_var("value"), var_ref(zero));
            break;
    }
    
    return result;
}

// Convert identifier to MAP representation  
static Var id_to_map(int id, Symtab *symtab) {
    Var result = new_map();
    
    result = mapinsert(result, str_dup_to_var(FIELD_TYPE), str_dup_to_var("identifier"));
    result = mapinsert(result, str_dup_to_var("id"), Var::new_int(id));
    
    // Include variable name if symbol table available
    if (symtab && id >= 0 && id < symtab->num_names) {
        result = mapinsert(result, str_dup_to_var("name"), str_dup_to_var(symtab->names[id]));
    }
    
    return result;
}

// Main expression visitor function
static Var expr_to_map_visitor(Expr *expr, Symtab *symtab) {
    if (!expr) {
        return var_ref(zero); // Null expression
    }
    
    Var result = new_map();
    
    // Set type field based on expression kind
    if (expr->kind >= 0 && expr->kind < SizeOf_Expr_Kind) {
        result = mapinsert(result, str_dup_to_var(FIELD_TYPE), 
                           str_dup_to_var(expr_type_names[expr->kind]));
    } else {
        result = mapinsert(result, str_dup_to_var(FIELD_TYPE), str_dup_to_var("unknown"));
        return result;
    }
    
    // Convert fields based on expression type
    switch (expr->kind) {
        case EXPR_VAR:
            // Literal value
            free_var(result);
            return var_literal_to_map(expr->e.var);
            
        case EXPR_ID:
            // Variable identifier
            free_var(result);
            return id_to_map(expr->e.id, symtab);
            
        case EXPR_ASGN:
            // Assignment: lhs = rhs
            result = mapinsert(result, str_dup_to_var("lhs"), 
                               expr_to_map_visitor(expr->e.bin.lhs, symtab));
            result = mapinsert(result, str_dup_to_var("rhs"), 
                               expr_to_map_visitor(expr->e.bin.rhs, symtab));
            break;
            
        case EXPR_PROP:
            // Property access: obj.prop
            result = mapinsert(result, str_dup_to_var("obj"), 
                               expr_to_map_visitor(expr->e.bin.lhs, symtab));
            result = mapinsert(result, str_dup_to_var("prop"), 
                               expr_to_map_visitor(expr->e.bin.rhs, symtab));
            break;
            
        case EXPR_VERB:
            // Verb call: obj:verb(args)
            result = mapinsert(result, str_dup_to_var("obj"), 
                               expr_to_map_visitor(expr->e.verb.obj, symtab));
            result = mapinsert(result, str_dup_to_var("verb"), 
                               expr_to_map_visitor(expr->e.verb.verb, symtab));
            
            // Convert arguments list to MAP format
            if (expr->e.verb.args) {
                Var args_list = new_list(0);
                for (Arg_List *arg = expr->e.verb.args; arg != nullptr; arg = arg->next) {
                    Var arg_map = new_map();
                    arg_map = mapinsert(arg_map, str_dup_to_var("expr"), 
                                       expr_to_map_visitor(arg->expr, symtab));
                    arg_map = mapinsert(arg_map, str_dup_to_var("kind"), 
                                       str_dup_to_var(arg->kind == ARG_NORMAL ? "normal" : "splice"));
                    args_list = listappend(args_list, arg_map);
                }
                result = mapinsert(result, str_dup_to_var("args"), args_list);
            }
            break;
            
        case EXPR_CALL:
            // Function call: func(args)
            result = mapinsert(result, str_dup_to_var("func"), 
                               str_dup_to_var(name_func_by_num(expr->e.call.func)));
            // Convert arguments list using existing Arg_List conversion
            if (expr->e.call.args) {
                result = mapinsert(result, str_dup_to_var("args"), 
                                   arg_list_to_list(expr->e.call.args, symtab));
            }
            break;
            
        // Binary operators (use same pattern)
        case EXPR_INDEX:
        case EXPR_PLUS:
        case EXPR_MINUS:
        case EXPR_TIMES:
        case EXPR_DIVIDE:
        case EXPR_MOD:
        case EXPR_EXP:
        case EXPR_EQ:
        case EXPR_NE:
        case EXPR_LT:
        case EXPR_LE:
        case EXPR_GT:
        case EXPR_GE:
        case EXPR_IN:
        case EXPR_AND:
        case EXPR_OR:
        case EXPR_BITOR:
        case EXPR_BITAND:
        case EXPR_BITXOR:
        case EXPR_BITSHL:
        case EXPR_BITSHR:
            result = mapinsert(result, str_dup_to_var("lhs"), 
                               expr_to_map_visitor(expr->e.bin.lhs, symtab));
            result = mapinsert(result, str_dup_to_var("rhs"), 
                               expr_to_map_visitor(expr->e.bin.rhs, symtab));
            break;
            
        // Unary operators
        case EXPR_NOT:
        case EXPR_NEGATE:
        case EXPR_COMPLEMENT:
            result = mapinsert(result, str_dup_to_var("expr"), 
                               expr_to_map_visitor(expr->e.expr, symtab));
            break;
            
        case EXPR_COND:
            // Conditional: condition ? consequent | alternate
            result = mapinsert(result, str_dup_to_var("condition"), 
                               expr_to_map_visitor(expr->e.cond.condition, symtab));
            result = mapinsert(result, str_dup_to_var("consequent"), 
                               expr_to_map_visitor(expr->e.cond.consequent, symtab));
            result = mapinsert(result, str_dup_to_var("alternate"), 
                               expr_to_map_visitor(expr->e.cond.alternate, symtab));
            break;
            
        case EXPR_LIST:
            // List literal: {expr1, expr2, ...}
            if (expr->e.list) {
                Var elements = new_list(0);
                int count = 0;
                for (Arg_List *arg = expr->e.list; arg; arg = arg->next) {
                    count++;
                    Var element = new_map();
                    element = mapinsert(element, str_dup_to_var("kind"), 
                                        str_dup_to_var(arg->kind == ARG_SPLICE ? "splice" : "normal"));
                    element = mapinsert(element, str_dup_to_var("expr"), 
                                        expr_to_map_visitor(arg->expr, symtab));
                    elements = listinsert(elements, element, count);
                }
                result = mapinsert(result, str_dup_to_var("elements"), elements);
            }
            break;
            
        case EXPR_RANGE:
            // Range expression: base[from..to] or [from..to]
            result = mapinsert(result, str_dup_to_var("from"), 
                               expr_to_map_visitor(expr->e.range.from, symtab));
            result = mapinsert(result, str_dup_to_var("to"), 
                               expr_to_map_visitor(expr->e.range.to, symtab));
            // Only include base if it exists (for indexed ranges like obj[1..3])
            if (expr->e.range.base) {
                result = mapinsert(result, str_dup_to_var("base"), 
                                   expr_to_map_visitor(expr->e.range.base, symtab));
            }
            break;
            
        case EXPR_MAP:
            // Map literal: ["key" -> value, "k2" -> v2, ...]
            if (expr->e.map) {
                Var elements = new_list(0);
                int count = 0;
                for (Map_List *entry = expr->e.map; entry; entry = entry->next) {
                    count++;
                    Var element = new_map();
                    element = mapinsert(element, str_dup_to_var("key"), 
                                        expr_to_map_visitor(entry->key, symtab));
                    element = mapinsert(element, str_dup_to_var("value"), 
                                        expr_to_map_visitor(entry->value, symtab));
                    elements = listinsert(elements, element, count);
                }
                result = mapinsert(result, str_dup_to_var("elements"), elements);
            }
            break;
            
        case EXPR_FIRST:
        case EXPR_LAST:
            // Nullary operators: ^ and $ (no sub-expressions)
            // No additional fields needed beyond type
            break;
            
        // TODO: Implement remaining expression types
        default:
            // Mark as unimplemented for now
            result = mapinsert(result, str_dup_to_var("_unimplemented"), Var::new_int(1));
            break;
    }
    
    return result;
}

/*********** Public Interface Functions ***********/

Var expr_to_map(Expr *expr, Symtab *symtab) {
    if (!expr) {
        return var_ref(zero);
    }
    
    Var result = expr_to_map_visitor(expr, symtab);
    
    // Add version and symbol table to top-level
    result = mapinsert(result, str_dup_to_var(FIELD_VERSION), Var::new_int(AST_SCHEMA_VERSION));
    if (symtab) {
        result = mapinsert(result, str_dup_to_var(FIELD_SYMTAB), symtab_to_list(symtab));
    }
    
    return result;
}

// Statement visitor function
static Var stmt_to_map_visitor(Stmt *stmt, Symtab *symtab) {
    if (!stmt) {
        return var_ref(zero); // Null statement
    }
    
    Var result = new_map();
    
    // Set type field based on statement kind
    if (stmt->kind >= 0 && stmt->kind < SizeOf_Stmt_Kind) {
        result = mapinsert(result, str_dup_to_var(FIELD_TYPE), 
                           str_dup_to_var(stmt_type_names[stmt->kind]));
    } else {
        result = mapinsert(result, str_dup_to_var(FIELD_TYPE), str_dup_to_var("unknown"));
        return result;
    }
    
    // Convert fields based on statement type
    switch (stmt->kind) {
        case STMT_COND: {
            // Conditional statement: if/elseif/else
            Var arms_list = new_list(0);
            
            // Convert each condition arm
            for (Cond_Arm *arm = stmt->s.cond.arms; arm != nullptr; arm = arm->next) {
                Var arm_map = new_map();
                arm_map = mapinsert(arm_map, str_dup_to_var("condition"), 
                                   expr_to_map_visitor(arm->condition, symtab));
                arm_map = mapinsert(arm_map, str_dup_to_var("stmt"), 
                                   stmt_to_map_visitor(arm->stmt, symtab));
                arms_list = listappend(arms_list, arm_map);
            }
            result = mapinsert(result, str_dup_to_var("arms"), arms_list);
            
            // Convert otherwise clause if present
            if (stmt->s.cond.otherwise) {
                result = mapinsert(result, str_dup_to_var("otherwise"), 
                                   stmt_to_map_visitor(stmt->s.cond.otherwise, symtab));
            }
            break;
        }
        
        case STMT_WHILE: {
            // While loop: while (condition) body; endwhile
            result = mapinsert(result, str_dup_to_var("condition"), 
                               expr_to_map_visitor(stmt->s.loop.condition, symtab));
            result = mapinsert(result, str_dup_to_var("body"), 
                               stmt_to_map_visitor(stmt->s.loop.body, symtab));
            
            // Include variable ID for named loops (-1 for unnamed)
            result = mapinsert(result, str_dup_to_var("id"), Var::new_int(stmt->s.loop.id));
            
            // Include variable name if symbol table available and loop is named
            if (symtab && stmt->s.loop.id >= 0 && stmt->s.loop.id < symtab->num_names) {
                result = mapinsert(result, str_dup_to_var("name"), 
                                   str_dup_to_var(symtab->names[stmt->s.loop.id]));
            }
            break;
        }
        
        case STMT_LIST: {
            // For-in loop: for x in (list) body; endfor
            result = mapinsert(result, str_dup_to_var("expr"), 
                               expr_to_map_visitor(stmt->s.list.expr, symtab));
            result = mapinsert(result, str_dup_to_var("body"), 
                               stmt_to_map_visitor(stmt->s.list.body, symtab));
            
            // Include variable ID and name for loop variable
            result = mapinsert(result, str_dup_to_var("id"), Var::new_int(stmt->s.list.id));
            if (symtab && stmt->s.list.id >= 0 && stmt->s.list.id < symtab->num_names) {
                result = mapinsert(result, str_dup_to_var("name"), 
                                   str_dup_to_var(symtab->names[stmt->s.list.id]));
            }
            
            // Include index variable if used (-1 if not used)
            result = mapinsert(result, str_dup_to_var("index"), Var::new_int(stmt->s.list.index));
            if (symtab && stmt->s.list.index >= 0 && stmt->s.list.index < symtab->num_names) {
                result = mapinsert(result, str_dup_to_var("index_name"), 
                                   str_dup_to_var(symtab->names[stmt->s.list.index]));
            }
            break;
        }
        
        case STMT_RANGE: {
            // For-range loop: for x in [from..to] body; endfor
            result = mapinsert(result, str_dup_to_var("from"), 
                               expr_to_map_visitor(stmt->s.range.from, symtab));
            result = mapinsert(result, str_dup_to_var("to"), 
                               expr_to_map_visitor(stmt->s.range.to, symtab));
            result = mapinsert(result, str_dup_to_var("body"), 
                               stmt_to_map_visitor(stmt->s.range.body, symtab));
            
            // Include variable ID and name for loop variable
            result = mapinsert(result, str_dup_to_var("id"), Var::new_int(stmt->s.range.id));
            if (symtab && stmt->s.range.id >= 0 && stmt->s.range.id < symtab->num_names) {
                result = mapinsert(result, str_dup_to_var("name"), 
                                   str_dup_to_var(symtab->names[stmt->s.range.id]));
            }
            break;
        }
        
        case STMT_FORK: {
            // Fork statement: fork (time) body; endfork
            result = mapinsert(result, str_dup_to_var("time"), 
                               expr_to_map_visitor(stmt->s.fork.time, symtab));
            result = mapinsert(result, str_dup_to_var("body"), 
                               stmt_to_map_visitor(stmt->s.fork.body, symtab));
            
            // Include variable ID for named forks (-1 for unnamed)
            result = mapinsert(result, str_dup_to_var("id"), Var::new_int(stmt->s.fork.id));
            if (symtab && stmt->s.fork.id >= 0 && stmt->s.fork.id < symtab->num_names) {
                result = mapinsert(result, str_dup_to_var("name"), 
                                   str_dup_to_var(symtab->names[stmt->s.fork.id]));
            }
            break;
        }
        
        case STMT_EXPR: {
            // Expression statement: expr;
            result = mapinsert(result, str_dup_to_var("expr"), 
                               expr_to_map_visitor(stmt->s.expr, symtab));
            break;
        }
        
        case STMT_RETURN: {
            // Return statement: return expr; or return;
            if (stmt->s.expr) {
                result = mapinsert(result, str_dup_to_var("expr"), 
                                   expr_to_map_visitor(stmt->s.expr, symtab));
            } else {
                result = mapinsert(result, str_dup_to_var("expr"), var_ref(zero));
            }
            break;
        }
        
        case STMT_TRY_EXCEPT: {
            // Try-except statement: try body; except err (codes) handler; endtry
            result = mapinsert(result, str_dup_to_var("body"), 
                               stmt_to_map_visitor(stmt->s._catch.body, symtab));
            
            // Convert exception handlers  
            Var excepts_list = new_list(0);
            for (Except_Arm *arm = stmt->s._catch.excepts; arm != nullptr; arm = arm->next) {
                Var except_map = new_map();
                except_map = mapinsert(except_map, str_dup_to_var("id"), Var::new_int(arm->id));
                except_map = mapinsert(except_map, str_dup_to_var("stmt"), 
                                       stmt_to_map_visitor(arm->stmt, symtab));
                
                // Include variable name if symbol table available
                if (symtab && arm->id >= 0 && arm->id < symtab->num_names) {
                    except_map = mapinsert(except_map, str_dup_to_var("name"), 
                                           str_dup_to_var(symtab->names[arm->id]));
                }
                
                // Convert exception codes (Arg_List) to MAP format
                if (arm->codes) {
                    except_map = mapinsert(except_map, str_dup_to_var("codes"), 
                                           arg_list_to_list(arm->codes, symtab));
                }
                
                excepts_list = listappend(excepts_list, except_map);
            }
            result = mapinsert(result, str_dup_to_var("excepts"), excepts_list);
            break;
        }
        
        case STMT_TRY_FINALLY: {
            // Try-finally statement: try body; finally handler; endtry
            result = mapinsert(result, str_dup_to_var("body"), 
                               stmt_to_map_visitor(stmt->s.finally.body, symtab));
            result = mapinsert(result, str_dup_to_var("handler"), 
                               stmt_to_map_visitor(stmt->s.finally.handler, symtab));
            break;
        }
        
        case STMT_BREAK:
        case STMT_CONTINUE: {
            // Break/continue statements: break; continue; 
            // No additional fields needed - type is sufficient
            break;
        }
        
        default:
            // Mark as unimplemented for now
            result = mapinsert(result, str_dup_to_var("_unimplemented"), Var::new_int(1));
            break;
    }
    
    return result;
}

Var stmt_to_map(Stmt *stmt, Symtab *symtab) {
    if (!stmt) {
        return var_ref(zero);
    }
    
    Var result = stmt_to_map_visitor(stmt, symtab);
    
    // Add version and symbol table to top-level
    result = mapinsert(result, str_dup_to_var(FIELD_VERSION), Var::new_int(AST_SCHEMA_VERSION));
    if (symtab) {
        result = mapinsert(result, str_dup_to_var(FIELD_SYMTAB), symtab_to_list(symtab));
    }
    
    return result;
}

// Reverse conversion: MAP → Expr  
static Expr *map_to_expr_visitor(Var map, Symtab *symtab) {
    if (map.type != TYPE_MAP) {
        return nullptr;
    }
    
    // Get type field
    Var type_field;
    if (maplookup(map, str_dup_to_var(FIELD_TYPE), &type_field, 0) == nullptr) {
        return nullptr; // Invalid map - missing type field
    }
    if (type_field.type != TYPE_STR) {
        return nullptr;
    }
    
    const char *type_name = type_field.v.str;
    
    // Handle special literal and identifier cases
    if (strcmp(type_name, "literal") == 0) {
        Var value_field;
        if (maplookup(map, str_dup_to_var("value"), &value_field, 0) == nullptr) {
            return nullptr;
        }
        Expr *result = alloc_expr(EXPR_VAR);
        result->e.var = var_ref(value_field);
        return result;
    }
    
    if (strcmp(type_name, "identifier") == 0) {
        Var id_field;
        if (maplookup(map, str_dup_to_var("id"), &id_field, 0) == nullptr) {
            return nullptr;
        }
        if (id_field.type != TYPE_INT) {
            return nullptr;
        }
        Expr *result = alloc_expr(EXPR_ID);
        result->e.id = id_field.v.num;
        return result;
    }
    
    // Find expression kind by name
    Expr_Kind kind = (Expr_Kind)-1;
    for (int i = 0; i < SizeOf_Expr_Kind; i++) {
        if (strcmp(type_name, expr_type_names[i]) == 0) {
            kind = (Expr_Kind)i;
            break;
        }
    }
    
    if (kind == (Expr_Kind)-1) {
        return nullptr; // Unknown expression type
    }
    
    
    // Create expression based on kind
    switch (kind) {
        case EXPR_PROP: {
            // Property access: obj.prop (uses "obj" and "prop" fields, not "lhs"/"rhs")
            Var obj_field, prop_field;
            if (maplookup(map, str_dup_to_var("obj"), &obj_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("prop"), &prop_field, 0) == nullptr) {
                return nullptr;
            }
            
            Expr *obj = map_to_expr_visitor(obj_field, symtab);
            Expr *prop = map_to_expr_visitor(prop_field, symtab);
            
            if (!obj || !prop) {
                return nullptr; // Automatic cleanup via allocation pool
            }
            
            return alloc_binary(EXPR_PROP, obj, prop);
        }
        
        case EXPR_ASGN:
        case EXPR_INDEX:
        case EXPR_PLUS:
        case EXPR_MINUS:
        case EXPR_TIMES:
        case EXPR_DIVIDE:
        case EXPR_MOD:
        case EXPR_EXP:
        case EXPR_EQ:
        case EXPR_NE:
        case EXPR_LT:
        case EXPR_LE:
        case EXPR_GT:
        case EXPR_GE:
        case EXPR_IN:
        case EXPR_AND:
        case EXPR_OR:
        case EXPR_BITOR:
        case EXPR_BITAND:
        case EXPR_BITXOR:
        case EXPR_BITSHL:
        case EXPR_BITSHR: {
            // Binary operators
            Var lhs_field, rhs_field;
            if (maplookup(map, str_dup_to_var("lhs"), &lhs_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("rhs"), &rhs_field, 0) == nullptr) {
                return nullptr;
            }
            
            Expr *lhs = map_to_expr_visitor(lhs_field, symtab);
            Expr *rhs = map_to_expr_visitor(rhs_field, symtab);
            
            if (!lhs || !rhs) {
                return nullptr; // Automatic cleanup via allocation pool
            }
            
            return alloc_binary(kind, lhs, rhs);
        }
        
        case EXPR_CALL: {
            // Function call: func_name(args)
            Var func_field;
            if (maplookup(map, str_dup_to_var("func"), &func_field, 0) == nullptr) {
                return nullptr;
            }
            if (func_field.type != TYPE_STR) {
                return nullptr;
            }
            
            // Look up function by name
            unsigned func_num = number_func_by_name(func_field.v.str);
            if (func_num == 0) {
                return nullptr; // Unknown function
            }
            
            // Handle arguments (optional field) using existing helper function
            Arg_List *args = nullptr;
            Var args_field;
            if (maplookup(map, str_dup_to_var("args"), &args_field, 0) != nullptr) {
                args = list_to_arg_list(args_field, symtab);
                // If args conversion fails, it's not critical - just use null
            }
            
            Expr *result = alloc_expr(EXPR_CALL);
            result->e.call.func = func_num;
            result->e.call.args = args;
            return result;
        }
        
        case EXPR_NOT:
        case EXPR_NEGATE:
        case EXPR_COMPLEMENT: {
            // Unary operators
            Var expr_field;
            if (maplookup(map, str_dup_to_var("expr"), &expr_field, 0) == nullptr) {
                return nullptr;
            }
            Expr *expr = map_to_expr_visitor(expr_field, symtab);
            
            if (!expr) {
                return nullptr;
            }
            
            Expr *result = alloc_expr(kind);
            result->e.expr = expr;
            return result;
        }
        
        case EXPR_COND: {
            // Conditional expression
            Var condition_field, consequent_field, alternate_field;
            if (maplookup(map, str_dup_to_var("condition"), &condition_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("consequent"), &consequent_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("alternate"), &alternate_field, 0) == nullptr) {
                return nullptr;
            }
            
            Expr *condition = map_to_expr_visitor(condition_field, symtab);
            Expr *consequent = map_to_expr_visitor(consequent_field, symtab);
            Expr *alternate = map_to_expr_visitor(alternate_field, symtab);
            
            if (!condition || !consequent || !alternate) {
                return nullptr; // Automatic cleanup via allocation pool
            }
            
            Expr *result = alloc_expr(EXPR_COND);
            result->e.cond.condition = condition;
            result->e.cond.consequent = consequent;
            result->e.cond.alternate = alternate;
            return result;
        }
        
        case EXPR_LIST: {
            // List literal: {expr1, expr2, ...}
            Var elements_field;
            if (maplookup(map, str_dup_to_var("elements"), &elements_field, 0) == nullptr) {
                // Empty list
                Expr *result = alloc_expr(EXPR_LIST);
                result->e.list = nullptr;
                return result;
            }
            
            if (elements_field.type != TYPE_LIST) {
                return nullptr;
            }
            
            // Convert elements to Arg_List
            Arg_List *arg_list = nullptr;
            Arg_List **last_ptr = &arg_list;
            
            for (int i = 1; i <= elements_field.v.list[0].v.num; i++) {
                Var element = elements_field.v.list[i];
                if (element.type != TYPE_MAP) {
                    // Cleanup and return error
                    while (arg_list) {
                        Arg_List *next = arg_list->next;
                        if (arg_list->expr) dealloc_node(arg_list->expr);
                        myfree(arg_list, M_AST);
                        arg_list = next;
                    }
                    return nullptr;
                }
                
                // Extract expr field from element
                Var expr_field;
                if (maplookup(element, str_dup_to_var("expr"), &expr_field, 0) == nullptr) {
                    // Cleanup and return error
                    while (arg_list) {
                        Arg_List *next = arg_list->next;
                        if (arg_list->expr) dealloc_node(arg_list->expr);
                        myfree(arg_list, M_AST);
                        arg_list = next;
                    }
                    return nullptr;
                }
                
                Expr *expr = map_to_expr_visitor(expr_field, symtab);
                if (!expr) {
                    // Automatic cleanup via allocation pool
                    return nullptr;
                }
                
                // Create new Arg_List node
                Arg_List *new_arg = (Arg_List *)mymalloc(sizeof(Arg_List), M_AST);
                new_arg->expr = expr;
                new_arg->kind = ARG_NORMAL; // Default to normal arguments
                new_arg->next = nullptr;
                
                // Add to list
                *last_ptr = new_arg;
                last_ptr = &new_arg->next;
            }
            
            Expr *result = alloc_expr(EXPR_LIST);
            result->e.list = arg_list;
            return result;
        }
        
        case EXPR_VERB: {
            // Verb call: obj:verb(args)
            Var obj_field, verb_field, args_field;
            if (maplookup(map, str_dup_to_var("obj"), &obj_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("verb"), &verb_field, 0) == nullptr) {
                return nullptr;
            }
            
            Expr *obj = map_to_expr_visitor(obj_field, symtab);
            Expr *verb = map_to_expr_visitor(verb_field, symtab);
            
            if (!obj || !verb) {
                return nullptr; // Automatic cleanup via allocation pool
            }
            
            // Handle arguments (optional field)
            Arg_List *args = nullptr;
            if (maplookup(map, str_dup_to_var("args"), &args_field, 0) != nullptr) {
                if (args_field.type == TYPE_LIST) {
                    // Convert list of argument expressions
                    Arg_List **last_ptr = &args;
                    
                    for (int i = 1; i <= args_field.v.list[0].v.num; i++) {
                        Var arg_map = args_field.v.list[i];
                        
                        // Skip invalid arguments
                        if (arg_map.type != TYPE_MAP) {
                            continue;
                        }
                        
                        // Get expression field from argument map
                        Var expr_field;
                        if (maplookup(arg_map, str_dup_to_var("expr"), &expr_field, 0) != nullptr) {
                            Expr *expr = map_to_expr_visitor(expr_field, symtab);
                            if (!expr) {
                                // Automatic cleanup via allocation pool
                                return nullptr;
                            }
                            
                            // Create new Arg_List node
                            Arg_List *new_arg = (Arg_List *)mymalloc(sizeof(Arg_List), M_AST);
                            new_arg->expr = expr;
                            new_arg->kind = ARG_NORMAL;
                            new_arg->next = nullptr;
                            
                            // Add to list
                            *last_ptr = new_arg;
                            last_ptr = &new_arg->next;
                        }
                    }
                }
            }
            
            Expr *result = alloc_expr(EXPR_VERB);
            result->e.verb.obj = obj;
            result->e.verb.verb = verb;
            result->e.verb.args = args;
            return result;
        }
        
        case EXPR_RANGE: {
            // Range expression: base[from..to] or [from..to]
            Var from_field, to_field;
            if (maplookup(map, str_dup_to_var("from"), &from_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("to"), &to_field, 0) == nullptr) {
                return nullptr;
            }
            
            Expr *from = map_to_expr_visitor(from_field, symtab);
            Expr *to = map_to_expr_visitor(to_field, symtab);
            if (!from || !to) {
                return nullptr;
            }
            
            Expr *result = alloc_expr(EXPR_RANGE);
            result->e.range.from = from;
            result->e.range.to = to;
            result->e.range.base = nullptr; // Default to no base
            
            // Base is optional - only present for indexed ranges like obj[1..3]
            Var base_field;
            if (maplookup(map, str_dup_to_var("base"), &base_field, 0) != nullptr) {
                // Only try to convert base if the field exists and is valid
                Expr *base = map_to_expr_visitor(base_field, symtab);
                if (base) {  // Only set base if conversion succeeds
                    result->e.range.base = base;
                }
                // If base conversion fails, we still continue with base=nullptr
            }
            
            return result;
        }
        
        case EXPR_MAP: {
            // Map literal: ["key" -> value, "k2" -> v2, ...]
            Var elements_field;
            if (maplookup(map, str_dup_to_var("elements"), &elements_field, 0) == nullptr) {
                // Empty map - this is correct behavior
                Expr *result = alloc_expr(EXPR_MAP);
                result->e.map = nullptr;
                return result;
            }
            
            if (elements_field.type != TYPE_LIST) {
                return nullptr;
            }
            
            // Convert elements to Map_List
            Map_List *map_list = nullptr;
            Map_List *last_entry = nullptr;
            
            for (int i = 1; i <= elements_field.v.list[0].v.num; i++) {
                Var element = elements_field.v.list[i];
                if (element.type != TYPE_MAP) {
                    // Cleanup and return error
                    while (map_list) {
                        Map_List *next = map_list->next;
                        if (map_list->key) dealloc_node(map_list->key);
                        if (map_list->value) dealloc_node(map_list->value);
                        myfree(map_list, M_AST);
                        map_list = next;
                    }
                    return nullptr;
                }
                
                // Extract key and value fields from element
                Var key_field, value_field;
                if (maplookup(element, str_dup_to_var("key"), &key_field, 0) == nullptr ||
                    maplookup(element, str_dup_to_var("value"), &value_field, 0) == nullptr) {
                    // Cleanup and return error
                    while (map_list) {
                        Map_List *next = map_list->next;
                        if (map_list->key) dealloc_node(map_list->key);
                        if (map_list->value) dealloc_node(map_list->value);
                        myfree(map_list, M_AST);
                        map_list = next;
                    }
                    return nullptr;
                }
                
                Expr *key = map_to_expr_visitor(key_field, symtab);
                Expr *value = map_to_expr_visitor(value_field, symtab);
                if (!key || !value) {
                    // Cleanup and return error
                    if (key) dealloc_node(key);
                    if (value) dealloc_node(value);
                    while (map_list) {
                        Map_List *next = map_list->next;
                        if (map_list->key) dealloc_node(map_list->key);
                        if (map_list->value) dealloc_node(map_list->value);
                        myfree(map_list, M_AST);
                        map_list = next;
                    }
                    return nullptr;
                }
                
                Map_List *entry = alloc_map_list(key, value);
                entry->next = nullptr;
                
                // Build list in correct order (first to last)
                if (!map_list) {
                    map_list = entry;
                    last_entry = entry;
                } else {
                    last_entry->next = entry;
                    last_entry = entry;
                }
            }
            
            Expr *result = alloc_expr(EXPR_MAP);
            result->e.map = map_list;
            return result;
        }
        
        case EXPR_FIRST:
        case EXPR_LAST:
            // Nullary operators: ^ and $ (no sub-expressions to convert)
            return alloc_expr(kind);
            
        // TODO: Add more expression types as needed
        default:
            return nullptr; // Unimplemented expression type
    }
}

Expr *map_to_expr(Var map, Symtab *symtab) {
    if (map.type != TYPE_MAP) {
        return nullptr;
    }
    
    // Validate version
    if (!validate_version(map)) {
        return nullptr;
    }
    
    return map_to_expr_visitor(map, symtab);
}

// Reverse conversion: MAP → Stmt
static Stmt *map_to_stmt_visitor(Var map, Symtab *symtab) {
    if (map.type != TYPE_MAP) {
        return nullptr;
    }
    
    // Get type field
    Var type_field;
    if (maplookup(map, str_dup_to_var(FIELD_TYPE), &type_field, 0) == nullptr) {
        return nullptr; // Invalid map - missing type field
    }
    if (type_field.type != TYPE_STR) {
        return nullptr;
    }
    
    const char *type_name = type_field.v.str;
    
    // Find statement kind by name
    Stmt_Kind kind = (Stmt_Kind)-1;
    for (int i = 0; i < SizeOf_Stmt_Kind; i++) {
        if (strcmp(type_name, stmt_type_names[i]) == 0) {
            kind = (Stmt_Kind)i;
            break;
        }
    }
    
    if (kind == (Stmt_Kind)-1) {
        return nullptr; // Unknown statement type
    }
    
    // Create statement based on kind
    switch (kind) {
        case STMT_COND: {
            // Conditional statement: if/elseif/else
            Var arms_field;
            if (maplookup(map, str_dup_to_var("arms"), &arms_field, 0) == nullptr) {
                return nullptr;
            }
            if (arms_field.type != TYPE_LIST) {
                return nullptr;
            }
            
            Stmt *result = alloc_stmt(STMT_COND);
            result->s.cond.arms = nullptr;
            result->s.cond.otherwise = nullptr;
            
            // Convert arms list
            Cond_Arm **arm_ptr = &result->s.cond.arms;
            for (int i = 1; i <= arms_field.v.list[0].v.num; i++) {
                Var arm_map = arms_field.v.list[i];
                if (arm_map.type != TYPE_MAP) {
                    return nullptr; // Automatic cleanup via allocation pool
                }
                
                Var condition_field, stmt_field;
                if (maplookup(arm_map, str_dup_to_var("condition"), &condition_field, 0) == nullptr ||
                    maplookup(arm_map, str_dup_to_var("stmt"), &stmt_field, 0) == nullptr) {
                    return nullptr; // Automatic cleanup via allocation pool
                }
                
                Expr *condition = map_to_expr_visitor(condition_field, symtab);
                Stmt *stmt = map_to_stmt_visitor(stmt_field, symtab);
                
                if (!condition || !stmt) {
                    return nullptr; // Automatic cleanup via allocation pool
                }
                
                *arm_ptr = alloc_cond_arm(condition, stmt);
                arm_ptr = &(*arm_ptr)->next;
            }
            
            // Convert otherwise clause if present
            Var otherwise_field;
            if (maplookup(map, str_dup_to_var("otherwise"), &otherwise_field, 0) != nullptr && 
                otherwise_field.type != TYPE_NONE) {
                result->s.cond.otherwise = map_to_stmt_visitor(otherwise_field, symtab);
                if (!result->s.cond.otherwise) {
                    return nullptr; // Automatic cleanup via allocation pool
                }
            }
            
            return result;
        }
        
        case STMT_WHILE: {
            // While loop
            Var condition_field, body_field, id_field;
            if (maplookup(map, str_dup_to_var("condition"), &condition_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("body"), &body_field, 0) == nullptr) {
                return nullptr;
            }
            // id field is optional for WHILE
            maplookup(map, str_dup_to_var("id"), &id_field, 0);
            
            Expr *condition = map_to_expr_visitor(condition_field, symtab);
            Stmt *body = map_to_stmt_visitor(body_field, symtab);
            
            if (!condition || !body) {
                return nullptr; // Automatic cleanup via allocation pool
            }
            
            Stmt *result = alloc_stmt(STMT_WHILE);
            result->s.loop.condition = condition;
            result->s.loop.body = body;
            result->s.loop.id = id_field.v.num;
            return result;
        }
        
        case STMT_EXPR: {
            // Expression statement
            Var expr_field;
            if (maplookup(map, str_dup_to_var("expr"), &expr_field, 0) == nullptr) {
                return nullptr;
            }
            Expr *expr = map_to_expr_visitor(expr_field, symtab);
            
            if (!expr) {
                return nullptr;
            }
            
            Stmt *result = alloc_stmt(STMT_EXPR);
            result->s.expr = expr;
            return result;
        }
        
        case STMT_RETURN: {
            // Return statement
            Var expr_field;
            // Expression is optional for return statements
            maplookup(map, str_dup_to_var("expr"), &expr_field, 0);
            
            Stmt *result = alloc_stmt(STMT_RETURN);
            if (expr_field.type != TYPE_NONE) {
                result->s.expr = map_to_expr_visitor(expr_field, symtab);
                if (!result->s.expr) {
                    return nullptr; // Automatic cleanup via allocation pool
                }
            } else {
                result->s.expr = nullptr;
            }
            return result;
        }
        
        case STMT_LIST: {
            // For-in-list loop: for x in (list) body; endfor
            Var expr_field, body_field, id_field, index_field;
            if (maplookup(map, str_dup_to_var("expr"), &expr_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("body"), &body_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("id"), &id_field, 0) == nullptr) {
                return nullptr;
            }
            
            Expr *expr = map_to_expr_visitor(expr_field, symtab);
            Stmt *body = map_to_stmt_visitor(body_field, symtab);
            
            if (!expr || !body || id_field.type != TYPE_INT) {
                return nullptr; // Automatic cleanup via allocation pool
            }
            
            Stmt *result = alloc_stmt(STMT_LIST);
            result->s.list.expr = expr;
            result->s.list.body = body;
            result->s.list.id = id_field.v.num;
            
            // Handle optional index variable (-1 if not used)
            if (maplookup(map, str_dup_to_var("index"), &index_field, 0) != nullptr &&
                index_field.type == TYPE_INT) {
                result->s.list.index = index_field.v.num;
            } else {
                result->s.list.index = -1;
            }
            
            return result;
        }
        
        case STMT_RANGE: {
            // For-in-range loop: for x in [from..to] body; endfor
            Var from_field, to_field, body_field, id_field;
            if (maplookup(map, str_dup_to_var("from"), &from_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("to"), &to_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("body"), &body_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("id"), &id_field, 0) == nullptr) {
                return nullptr;
            }
            
            Expr *from = map_to_expr_visitor(from_field, symtab);
            Expr *to = map_to_expr_visitor(to_field, symtab);
            Stmt *body = map_to_stmt_visitor(body_field, symtab);
            
            if (!from || !to || !body || id_field.type != TYPE_INT) {
                return nullptr; // Automatic cleanup via allocation pool
            }
            
            Stmt *result = alloc_stmt(STMT_RANGE);
            result->s.range.from = from;
            result->s.range.to = to;
            result->s.range.body = body;
            result->s.range.id = id_field.v.num;
            
            return result;
        }
        
        case STMT_BREAK:
        case STMT_CONTINUE: {
            // Simple statements - need to initialize s.exit field
            Stmt *result = alloc_stmt(kind);
            result->s.exit = -1;  // -1 means no named loop
            return result;
        }
        
        case STMT_TRY_EXCEPT: {
            // Try-except statement: try body; except err (codes) handler; endtry
            Var body_field, excepts_field;
            if (maplookup(map, str_dup_to_var("body"), &body_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("excepts"), &excepts_field, 0) == nullptr) {
                return nullptr;
            }
            if (excepts_field.type != TYPE_LIST) {
                return nullptr;
            }
            
            Stmt *body = map_to_stmt_visitor(body_field, symtab);
            if (!body) {
                return nullptr; // Automatic cleanup via allocation pool
            }
            
            Stmt *result = alloc_stmt(STMT_TRY_EXCEPT);
            result->s._catch.body = body;
            result->s._catch.excepts = nullptr;
            
            // Convert exception handlers list
            Except_Arm **except_ptr = &result->s._catch.excepts;
            for (int i = 1; i <= excepts_field.v.list[0].v.num; i++) {
                Var except_map = excepts_field.v.list[i];
                if (except_map.type != TYPE_MAP) {
                    return nullptr; // Automatic cleanup via allocation pool
                }
                
                Var id_field, stmt_field;
                if (maplookup(except_map, str_dup_to_var("id"), &id_field, 0) == nullptr ||
                    maplookup(except_map, str_dup_to_var("stmt"), &stmt_field, 0) == nullptr ||
                    id_field.type != TYPE_INT) {
                    return nullptr; // Automatic cleanup via allocation pool
                }
                
                Stmt *except_stmt = map_to_stmt_visitor(stmt_field, symtab);
                if (!except_stmt) {
                    return nullptr; // Automatic cleanup via allocation pool
                }
                
                // Convert exception codes from MAP to Arg_List
                Arg_List *codes = nullptr;
                Var codes_field;
                if (maplookup(except_map, str_dup_to_var("codes"), &codes_field, 0) != nullptr) {
                    codes = list_to_arg_list(codes_field, symtab);
                    // If codes conversion fails, it's not critical - just use null
                }
                
                *except_ptr = alloc_except(id_field.v.num, codes, except_stmt);
                except_ptr = &(*except_ptr)->next;
            }
            
            return result;
        }
        
        case STMT_TRY_FINALLY: {
            // Try-finally statement: try body; finally handler; endtry
            Var body_field, handler_field;
            if (maplookup(map, str_dup_to_var("body"), &body_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("handler"), &handler_field, 0) == nullptr) {
                return nullptr;
            }
            
            Stmt *body = map_to_stmt_visitor(body_field, symtab);
            Stmt *handler = map_to_stmt_visitor(handler_field, symtab);
            
            if (!body || !handler) {
                return nullptr; // Automatic cleanup via allocation pool
            }
            
            Stmt *result = alloc_stmt(STMT_TRY_FINALLY);
            result->s.finally.body = body;
            result->s.finally.handler = handler;
            return result;
        }
        
        case STMT_FORK: {
            // Fork statement: fork (time) body; endfork
            Var time_field, body_field, id_field;
            if (maplookup(map, str_dup_to_var("time"), &time_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("body"), &body_field, 0) == nullptr ||
                maplookup(map, str_dup_to_var("id"), &id_field, 0) == nullptr) {
                return nullptr;
            }
            
            if (id_field.type != TYPE_INT) {
                return nullptr;
            }
            
            Expr *time = map_to_expr_visitor(time_field, symtab);
            Stmt *body = map_to_stmt_visitor(body_field, symtab);
            
            if (!time || !body) {
                return nullptr; // Automatic cleanup via allocation pool
            }
            
            Stmt *result = alloc_stmt(STMT_FORK);
            result->s.fork.time = time;
            result->s.fork.body = body;
            result->s.fork.id = id_field.v.num;
            return result;
        }
        
        default:
            return nullptr; // Unimplemented statement type
    }
}

Stmt *map_to_stmt(Var map, Symtab *symtab) {
    if (map.type != TYPE_MAP) {
        return nullptr;
    }
    
    // Validate version
    if (!validate_version(map)) {
        return nullptr;
    }
    
    return map_to_stmt_visitor(map, symtab);
}

/*********** Arg_List Conversion ***********/

// Convert Arg_List to list of MAPs
static Var arg_list_to_list(Arg_List *args, Symtab *symtab) {
    Var result = new_list(0);
    
    for (Arg_List *arg = args; arg != nullptr; arg = arg->next) {
        Var arg_map = new_map();
        arg_map = mapinsert(arg_map, str_dup_to_var("kind"), 
                            str_dup_to_var(arg->kind == ARG_NORMAL ? "normal" : "splice"));
        arg_map = mapinsert(arg_map, str_dup_to_var("expr"), 
                            expr_to_map_visitor(arg->expr, symtab));
        result = listappend(result, arg_map);
    }
    
    return result;
}

// Convert list of MAPs to Arg_List
static Arg_List *list_to_arg_list(Var list, Symtab *symtab) {
    if (list.type != TYPE_LIST) {
        return nullptr;
    }
    
    Arg_List *result = nullptr;
    Arg_List **arg_ptr = &result;
    
    for (int i = 1; i <= list.v.list[0].v.num; i++) {
        Var arg_map = list.v.list[i];
        if (arg_map.type != TYPE_MAP) {
            return nullptr; // Invalid - all args should be maps
        }
        
        Var kind_field, expr_field;
        if (maplookup(arg_map, str_dup_to_var("kind"), &kind_field, 0) == nullptr ||
            maplookup(arg_map, str_dup_to_var("expr"), &expr_field, 0) == nullptr) {
            return nullptr; // Missing required fields
        }
        
        if (kind_field.type != TYPE_STR) {
            return nullptr; // Invalid kind field
        }
        
        enum Arg_Kind kind;
        if (strcmp(kind_field.v.str, "normal") == 0) {
            kind = ARG_NORMAL;
        } else if (strcmp(kind_field.v.str, "splice") == 0) {
            kind = ARG_SPLICE;
        } else {
            return nullptr; // Unknown arg kind
        }
        
        Expr *expr = map_to_expr_visitor(expr_field, symtab);
        if (!expr) {
            return nullptr; // Invalid expression
        }
        
        *arg_ptr = alloc_arg_list(kind, expr);
        arg_ptr = &(*arg_ptr)->next;
    }
    
    return result;
}

/*********** Symbol Table Implementation ***********/

Symtab *create_symtab(int num_names) {
    if (num_names < 0) {
        return nullptr;
    }
    
    Symtab *symtab = (Symtab *)mymalloc(sizeof(Symtab), M_AST);
    symtab->num_names = num_names;
    
    if (num_names > 0) {
        symtab->names = (char **)mymalloc(num_names * sizeof(char *), M_AST);
        for (int i = 0; i < num_names; i++) {
            symtab->names[i] = nullptr;
        }
    } else {
        symtab->names = nullptr;
    }
    
    return symtab;
}

void free_symtab(Symtab *symtab) {
    if (!symtab) {
        return;
    }
    
    if (symtab->names) {
        for (int i = 0; i < symtab->num_names; i++) {
            if (symtab->names[i]) {
                free_str(symtab->names[i]);
            }
        }
        myfree(symtab->names, M_AST);
    }
    
    myfree(symtab, M_AST);
}

int symtab_add_name(Symtab *symtab, const char *name) {
    if (!symtab || !name) {
        return -1;
    }
    
    // Find first available slot
    for (int i = 0; i < symtab->num_names; i++) {
        if (symtab->names[i] == nullptr) {
            symtab->names[i] = str_dup(name);
            return i;
        }
    }
    
    // No available slots
    return -1;
}

const char *symtab_get_name(Symtab *symtab, int id) {
    if (!symtab || id < 0 || id >= symtab->num_names) {
        return nullptr;
    }
    
    return symtab->names[id];
}

/*********** Unparse Wrapper Functions ***********/

// Structure to collect unparse output into a Var list
typedef struct {
    Var *result;     // Pointer to the result list
    int line_count;  // Current number of lines
} unparse_collector;

// Callback function to collect unparse output lines
static void collect_unparse_line(void *data, const char *line) {
    unparse_collector *collector = (unparse_collector *)data;
    
    // Extend the list to hold one more line
    collector->line_count++;
    *collector->result = listinsert(*collector->result, str_dup_to_var(line), collector->line_count);
}

// Create minimal Program context for unparsing (needs variable names)
static Program *create_minimal_program(Symtab *symtab) {
    Program *prog = (Program *)mymalloc(sizeof(Program), M_PROGRAM);
    memset(prog, 0, sizeof(Program));
    
    // Set up variable names array from symbol table
    if (symtab && symtab->num_names > 0) {
        prog->num_var_names = symtab->num_names;
        prog->var_names = (const char **)mymalloc(symtab->num_names * sizeof(char *), M_NAMES);
        
        for (int i = 0; i < symtab->num_names; i++) {
            prog->var_names[i] = symtab->names[i] ? symtab->names[i] : "";
        }
    } else {
        prog->num_var_names = 0;
        prog->var_names = nullptr;
    }
    
    return prog;
}

// Free minimal Program context
static void free_minimal_program(Program *prog) {
    if (prog) {
        if (prog->var_names) {
            myfree(prog->var_names, M_NAMES);
        }
        myfree(prog, M_PROGRAM);
    }
}

// Helper functions removed - now using integrated unparse.cc functionality

// Expression unparsing now handled by unparse.cc integration

// Add a public function to unparse.cc for statement unparsing
extern char *unparse_stmt_to_string(Stmt *stmt, Symtab *symtab);


/*********** AST Builtin Functions ***********/


// Global variables to capture AST during parsing  
static Var captured_ast;
static bool ast_capture_success = false;
static bool ast_capture_initialized = false;

static void init_ast_capture() {
    if (!ast_capture_initialized) {
        captured_ast.type = TYPE_NONE;
        ast_capture_initialized = true;
    }
}

// Parser function from parser.h
extern Program *parse_list_as_program(Var code, Var *errors);

// AST capture callback function
static void ast_capture_callback(Stmt *prog_start, Names *local_names, DB_Version version) {
    if (!prog_start) {
        ast_capture_success = false;
        return;
    }
    
    // Create symbol table from local_names
    Symtab *symtab = create_symtab(local_names ? local_names->size : 0);
    if (local_names) {
        for (int i = 0; i < local_names->size; i++) {
            if (local_names->names[i]) {
                symtab->names[i] = str_dup(local_names->names[i]);
            }
        }
    }
    
    // Convert AST to MAP while it's still in memory
    Var result = stmt_to_map_visitor(prog_start, symtab);
    
    // Add metadata
    result = mapinsert(result, str_dup_to_var("ast_version"), Var::new_int(AST_SCHEMA_VERSION));
    
    if (local_names && local_names->size > 0) {
        Var var_names = new_list(local_names->size);
        for (int i = 0; i < local_names->size; i++) {
            var_names.v.list[i+1] = str_dup_to_var(local_names->names[i] ? local_names->names[i] : "");
        }
        result = mapinsert(result, str_dup_to_var("variables"), var_names);
    }
    
    // Clean up symbol table
    free_symtab(symtab);
    
    // Save result globally
    if (captured_ast.type != TYPE_NONE) {
        free_var(captured_ast);
    }
    captured_ast = result;
    ast_capture_success = true;
}

// Real AST capture using parser callback
Var parse_list_as_ast(Var code, Var *errors) {
    // Initialize if needed
    init_ast_capture();
    
    // Reset capture state
    ast_capture_success = false;
    if (captured_ast.type != TYPE_NONE) {
        free_var(captured_ast);
        captured_ast.type = TYPE_NONE;
    }
    
    // Set up callback to capture AST
    set_ast_capture_callback(ast_capture_callback);
    
    // Parse using existing function - callback will capture AST
    Program *program = parse_list_as_program(code, errors);
    
    // Clear callback
    set_ast_capture_callback(NULL);
    
    // Check if AST was captured successfully
    if (ast_capture_success && captured_ast.type != TYPE_NONE) {
        // Return captured AST
        Var result = captured_ast;
        captured_ast.type = TYPE_NONE; // Don't free it since we're returning it
        
        if (program) free_program(program);
        return result;
    } else {
        // Parse failed or callback didn't fire - return empty MAP
        if (program) free_program(program);
        return new_map();
    }
}

static package bf_parse_ast(Var arglist, Byte next, void *vdata, Objid progr) {
    
    // Validate arguments: parse_ast(code_list)
    if (arglist.v.list[0].v.num != 1) {
        return make_error_pack(E_ARGS);
    }
    
    Var code = arglist.v.list[1];
    if (code.type != TYPE_LIST) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    }
    
    // Check permissions (wizard-only for now)
    if (!is_wizard(progr)) {
        free_var(arglist);
        return make_error_pack(E_PERM);
    }
    
    // Use real AST capture
    Var errors;
    Var result = parse_list_as_ast(code, &errors);
    
    // Check if parsing succeeded
    Var parse_error_var;
    if (maplookup(result, str_dup_to_var("parse_error"), &parse_error_var, 0) != nullptr) {
        // Parse error occurred
        if (errors.type == TYPE_LIST && errors.v.list[0].v.num > 0) {
            // Return first error message
            free_var(result);
            free_var(errors);
            free_var(arglist);
            return make_error_pack(E_INVARG);
        } else {
            // Generic parse error
            free_var(result);
            free_var(errors);
            free_var(arglist);
            return make_error_pack(E_INVARG);
        }
    }
    
    // Success - return AST
    free_var(errors);
    free_var(arglist);
    return make_var_pack(result);
}

static package bf_unparse_ast(Var arglist, Byte next, void *vdata, Objid progr) {
    // Validate arguments
    if (arglist.v.list[0].v.num != 1) {
        free_var(arglist);
        return make_error_pack(E_ARGS);
    }
    
    Var ast_map = arglist.v.list[1];
    if (ast_map.type != TYPE_MAP) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    }
    
    // Validate AST MAP version
    if (!validate_version(ast_map)) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    }
    
    // Allocate AST in temporary pool 
    begin_code_allocation();
    
    // Extract symbol table from AST MAP (after starting allocation tracking)
    Symtab *symtab = extract_symtab_from_map(ast_map);
    
    Stmt *stmt = map_to_stmt(ast_map, symtab);
    if (!stmt) {
        end_code_allocation(1);  // Abort and clean up everything
        return make_error_pack(E_INVARG);
    }
    
    // Use the new unparse_stmt_to_string function
    char *unparsed_code = unparse_stmt_to_string(stmt, symtab);
    
    // Create result list by splitting multi-line string into separate lines
    Var result_list = new_list(0);
    if (unparsed_code && strlen(unparsed_code) > 0) {
        // Split the string on newlines
        char *line_start = unparsed_code;
        char *line_end;
        
        while ((line_end = strchr(line_start, '\n')) != nullptr) {
            // Create string for this line
            int line_len = line_end - line_start;
            char *line_str = (char *)mymalloc(line_len + 1, M_STRING);
            strncpy(line_str, line_start, line_len);
            line_str[line_len] = '\0';
            
            Var line_var = str_dup_to_var(line_str);
            result_list = listappend(result_list, line_var);
            
            myfree(line_str, M_STRING);
            line_start = line_end + 1;
        }
        
        // Handle the last line (if no trailing newline)
        if (*line_start != '\0') {
            Var line_var = str_dup_to_var(line_start);
            result_list = listappend(result_list, line_var);
        }
    }
    
    // Free the string
    if (unparsed_code) {
        myfree(unparsed_code, M_STRING);
    }
    
    end_code_allocation(1);  // Clean up AST allocations
    free_var(arglist);
    
    return make_var_pack(result_list);
}

static package bf_validate_ast(Var arglist, Byte next, void *vdata, Objid progr) {
    // Validate arguments
    if (arglist.v.list[0].v.num != 1) {
        free_var(arglist);
        return make_error_pack(E_ARGS);
    }
    
    Var ast_map = arglist.v.list[1];
    if (ast_map.type != TYPE_MAP) {
        free_var(arglist);
        return make_var_pack(Var::new_int(0)); // Invalid
    }
    
    // Check version compatibility
    if (!validate_version(ast_map)) {
        free_var(arglist);
        return make_var_pack(Var::new_int(0)); // Invalid
    }
    
    // Try to convert MAP to AST to validate structure
    Symtab *symtab = create_symtab(0);
    begin_code_allocation();
    
    Stmt *stmt = map_to_stmt(ast_map, symtab);
    int is_valid = (stmt != nullptr) ? 1 : 0;
    
    end_code_allocation(0);  // Clean up
    free_symtab(symtab);
    free_var(arglist);
    
    return make_var_pack(Var::new_int(is_valid));
}

void register_ast(void) {
    register_function("parse_ast", 1, 1, bf_parse_ast, TYPE_LIST);
    register_function("unparse_ast", 1, 1, bf_unparse_ast, TYPE_MAP);
    register_function("validate_ast", 1, 1, bf_validate_ast, TYPE_MAP);
}