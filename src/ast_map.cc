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
static char *unparse_expr_to_string(Expr *expr, Symtab *symtab);

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
static const char *FIELD_SYMTAB = "symtab";

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
    "map",        // EXPR_MAP      (27)
    "bitor",      // EXPR_BITOR    (28)
    "bitand",     // EXPR_BITAND   (29)
    "bitxor",     // EXPR_BITXOR   (30)
    "bitshl",     // EXPR_BITSHL   (31)
    "bitshr",     // EXPR_BITSHR   (32)
    "complement", // EXPR_COMPLEMENT (33)
    "scatter",    // EXPR_SCATTER  (34)
    "catch",      // EXPR_CATCH    (35)
    "first",      // EXPR_FIRST    (36)
    "last"        // EXPR_LAST     (37)
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

// Convert MOO LIST of STRs back to symbol table
static Symtab *list_to_symtab(Var list) {
    if (list.type != TYPE_LIST) {
        return nullptr;
    }
    
    int num_names = list.v.list[0].v.num;
    if (num_names == 0) {
        return nullptr; // Empty symbol table
    }
    
    Symtab *symtab = (Symtab *)mymalloc(sizeof(Symtab), M_AST);
    symtab->num_names = num_names;
    symtab->names = (char **)mymalloc(num_names * sizeof(char *), M_AST);
    
    for (int i = 0; i < num_names; i++) {
        if (list.v.list[i + 1].type != TYPE_STR) {
            // Invalid symbol table format
            myfree(symtab->names, M_AST);
            myfree(symtab, M_AST);
            return nullptr;
        }
        symtab->names[i] = str_dup(list.v.list[i + 1].v.str);
    }
    
    return symtab;
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
            // TODO: Convert args list
            break;
            
        case EXPR_CALL:
            // Function call: func(args)
            result = mapinsert(result, str_dup_to_var("func"), 
                               str_dup_to_var(name_func_by_num(expr->e.call.func)));
            // TODO: Convert args list
            break;
            
        // Binary operators (use same pattern)
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
                
                // TODO: Convert codes (Arg_List) - needs Arg_List to MAP conversion
                
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
        case EXPR_ASGN:
        case EXPR_PROP:
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
                if (lhs) dealloc_node(lhs);
                if (rhs) dealloc_node(rhs);
                return nullptr;
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
            
            // Handle arguments (optional field)
            Arg_List *args = nullptr;
            Var args_field;
            if (maplookup(map, str_dup_to_var("args"), &args_field, 0) != nullptr) {
                if (args_field.type == TYPE_LIST) {
                    // Convert arguments to Arg_List
                    Arg_List **last_ptr = &args;
                    
                    for (int i = 1; i <= args_field.v.list[0].v.num; i++) {
                        Var arg_element = args_field.v.list[i];
                        if (arg_element.type != TYPE_MAP) {
                            // Cleanup and return error
                            while (args) {
                                Arg_List *next = args->next;
                                if (args->expr) dealloc_node(args->expr);
                                myfree(args, M_AST);
                                args = next;
                            }
                            return nullptr;
                        }
                        
                        // Extract expr field from argument
                        Var expr_field;
                        if (maplookup(arg_element, str_dup_to_var("expr"), &expr_field, 0) == nullptr) {
                            // Cleanup and return error
                            while (args) {
                                Arg_List *next = args->next;
                                if (args->expr) dealloc_node(args->expr);
                                myfree(args, M_AST);
                                args = next;
                            }
                            return nullptr;
                        }
                        
                        Expr *expr = map_to_expr_visitor(expr_field, symtab);
                        if (!expr) {
                            // Cleanup and return error
                            while (args) {
                                Arg_List *next = args->next;
                                if (args->expr) dealloc_node(args->expr);
                                myfree(args, M_AST);
                                args = next;
                            }
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
                if (condition) dealloc_node(condition);
                if (consequent) dealloc_node(consequent);
                if (alternate) dealloc_node(alternate);
                return nullptr;
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
                    // Cleanup and return error
                    while (arg_list) {
                        Arg_List *next = arg_list->next;
                        if (arg_list->expr) dealloc_node(arg_list->expr);
                        myfree(arg_list, M_AST);
                        arg_list = next;
                    }
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
                    free_stmt(result);
                    return nullptr;
                }
                
                Var condition_field, stmt_field;
                if (maplookup(arm_map, str_dup_to_var("condition"), &condition_field, 0) == nullptr ||
                    maplookup(arm_map, str_dup_to_var("stmt"), &stmt_field, 0) == nullptr) {
                    free_stmt(result);
                    return nullptr;
                }
                
                Expr *condition = map_to_expr_visitor(condition_field, symtab);
                Stmt *stmt = map_to_stmt_visitor(stmt_field, symtab);
                
                if (!condition || !stmt) {
                    if (condition) dealloc_node(condition);
                    if (stmt) free_stmt(stmt);
                    free_stmt(result);
                    return nullptr;
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
                    free_stmt(result);
                    return nullptr;
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
                if (condition) dealloc_node(condition);
                if (body) free_stmt(body);
                return nullptr;
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
                    free_stmt(result);
                    return nullptr;
                }
            } else {
                result->s.expr = nullptr;
            }
            return result;
        }
        
        case STMT_BREAK:
        case STMT_CONTINUE: {
            // Simple statements with no additional data
            return alloc_stmt(kind);
        }
        
        // TODO: Add remaining statement types as needed
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

// Helper function to convert Var to string for literal unparsing
static char *var_to_string(Var v) {
    char *result = nullptr;
    
    switch (v.type) {
        case TYPE_INT:
            result = (char *)mymalloc(32, M_STRING);
            sprintf(result, "%lld", (long long)v.v.num);
            break;
            
        case TYPE_FLOAT:
            result = (char *)mymalloc(64, M_STRING);
            sprintf(result, "%.17g", v.v.fnum);
            break;
            
        case TYPE_STR:
            // Need to escape and quote the string
            result = (char *)mymalloc(strlen(v.v.str) * 2 + 3, M_STRING);  // Worst case: all chars need escaping + quotes
            sprintf(result, "\"%s\"", v.v.str);  // Basic implementation - TODO: proper escaping
            break;
            
        default:
            result = (char *)mymalloc(16, M_STRING);
            strcpy(result, "<?>");  // Unknown type placeholder
            break;
    }
    
    return result;
}

// Helper function to join argument lists with commas
static char *join_args(Arg_List *args, Symtab *symtab) {
    if (!args) {
        char *result = (char *)mymalloc(1, M_STRING);
        result[0] = '\0';
        return result;
    }
    
    // Calculate total size needed and build argument list
    int total_size = 0;
    int count = 0;
    
    // First pass: calculate size needed
    for (Arg_List *arg = args; arg; arg = arg->next) {
        char *arg_str = unparse_expr_to_string(arg->expr, symtab);
        total_size += strlen(arg_str);
        myfree(arg_str, M_STRING);
        count++;
        if (arg->next) total_size += 2; // ", " separator
    }
    
    char *result = (char *)mymalloc(total_size + 1, M_STRING);
    result[0] = '\0';
    
    // Second pass: build the string
    int first = 1;
    for (Arg_List *arg = args; arg; arg = arg->next) {
        if (!first) {
            strcat(result, ", ");
        }
        char *arg_str = unparse_expr_to_string(arg->expr, symtab);
        strcat(result, arg_str);
        myfree(arg_str, M_STRING);
        first = 0;
    }
    
    return result;
}

// Helper function to unparse a single expression to string
static char *unparse_expr_to_string(Expr *expr, Symtab *symtab) {
    if (!expr) {
        char *result = (char *)mymalloc(1, M_STRING);
        result[0] = '\0';
        return result;
    }
    
    char *result = nullptr;
    
    switch (expr->kind) {
        case EXPR_VAR:
            // Literal values
            result = var_to_string(expr->e.var);
            break;
            
        case EXPR_CALL: {
            // Function call: func_name(args)
            const char *func_name = name_func_by_num(expr->e.call.func);
            char *args_str = join_args(expr->e.call.args, symtab);
            
            result = (char *)mymalloc(strlen(func_name) + strlen(args_str) + 3, M_STRING);
            sprintf(result, "%s(%s)", func_name, args_str);
            
            myfree(args_str, M_STRING);
            break;
        }
        
        case EXPR_PLUS: {
            // Arithmetic: lhs + rhs
            char *lhs = unparse_expr_to_string(expr->e.bin.lhs, symtab);
            char *rhs = unparse_expr_to_string(expr->e.bin.rhs, symtab);
            
            result = (char *)mymalloc(strlen(lhs) + strlen(rhs) + 4, M_STRING);
            sprintf(result, "%s + %s", lhs, rhs);
            
            myfree(lhs, M_STRING);
            myfree(rhs, M_STRING);
            break;
        }
        
        case EXPR_MINUS: {
            // Arithmetic: lhs - rhs
            char *lhs = unparse_expr_to_string(expr->e.bin.lhs, symtab);
            char *rhs = unparse_expr_to_string(expr->e.bin.rhs, symtab);
            
            result = (char *)mymalloc(strlen(lhs) + strlen(rhs) + 4, M_STRING);
            sprintf(result, "%s - %s", lhs, rhs);
            
            myfree(lhs, M_STRING);
            myfree(rhs, M_STRING);
            break;
        }
        
        case EXPR_TIMES: {
            // Arithmetic: lhs * rhs
            char *lhs = unparse_expr_to_string(expr->e.bin.lhs, symtab);
            char *rhs = unparse_expr_to_string(expr->e.bin.rhs, symtab);
            
            result = (char *)mymalloc(strlen(lhs) + strlen(rhs) + 4, M_STRING);
            sprintf(result, "%s * %s", lhs, rhs);
            
            myfree(lhs, M_STRING);
            myfree(rhs, M_STRING);
            break;
        }
        
        case EXPR_DIVIDE: {
            // Arithmetic: lhs / rhs
            char *lhs = unparse_expr_to_string(expr->e.bin.lhs, symtab);
            char *rhs = unparse_expr_to_string(expr->e.bin.rhs, symtab);
            
            result = (char *)mymalloc(strlen(lhs) + strlen(rhs) + 4, M_STRING);
            sprintf(result, "%s / %s", lhs, rhs);
            
            myfree(lhs, M_STRING);
            myfree(rhs, M_STRING);
            break;
        }
        
        case EXPR_LIST: {
            // List literal: {elem1, elem2, ...}
            if (!expr->e.list) {
                result = (char *)mymalloc(3, M_STRING);
                strcpy(result, "{}");
                break;
            }
            
            // Use join_args to handle the argument list
            char *elements_str = join_args(expr->e.list, symtab);
            result = (char *)mymalloc(strlen(elements_str) + 3, M_STRING);
            sprintf(result, "{%s}", elements_str);
            myfree(elements_str, M_STRING);
            break;
        }
        
        case EXPR_EQ: {
            // Equality: lhs == rhs
            char *lhs = unparse_expr_to_string(expr->e.bin.lhs, symtab);
            char *rhs = unparse_expr_to_string(expr->e.bin.rhs, symtab);
            
            result = (char *)mymalloc(strlen(lhs) + strlen(rhs) + 5, M_STRING);
            sprintf(result, "%s == %s", lhs, rhs);
            
            myfree(lhs, M_STRING);
            myfree(rhs, M_STRING);
            break;
        }
        
        case EXPR_NE: {
            // Inequality: lhs != rhs
            char *lhs = unparse_expr_to_string(expr->e.bin.lhs, symtab);
            char *rhs = unparse_expr_to_string(expr->e.bin.rhs, symtab);
            
            result = (char *)mymalloc(strlen(lhs) + strlen(rhs) + 5, M_STRING);
            sprintf(result, "%s != %s", lhs, rhs);
            
            myfree(lhs, M_STRING);
            myfree(rhs, M_STRING);
            break;
        }
        
        case EXPR_LT: {
            // Less than: lhs < rhs
            char *lhs = unparse_expr_to_string(expr->e.bin.lhs, symtab);
            char *rhs = unparse_expr_to_string(expr->e.bin.rhs, symtab);
            
            result = (char *)mymalloc(strlen(lhs) + strlen(rhs) + 4, M_STRING);
            sprintf(result, "%s < %s", lhs, rhs);
            
            myfree(lhs, M_STRING);
            myfree(rhs, M_STRING);
            break;
        }
        
        case EXPR_LE: {
            // Less or equal: lhs <= rhs
            char *lhs = unparse_expr_to_string(expr->e.bin.lhs, symtab);
            char *rhs = unparse_expr_to_string(expr->e.bin.rhs, symtab);
            
            result = (char *)mymalloc(strlen(lhs) + strlen(rhs) + 5, M_STRING);
            sprintf(result, "%s <= %s", lhs, rhs);
            
            myfree(lhs, M_STRING);
            myfree(rhs, M_STRING);
            break;
        }
        
        case EXPR_GT: {
            // Greater than: lhs > rhs
            char *lhs = unparse_expr_to_string(expr->e.bin.lhs, symtab);
            char *rhs = unparse_expr_to_string(expr->e.bin.rhs, symtab);
            
            result = (char *)mymalloc(strlen(lhs) + strlen(rhs) + 4, M_STRING);
            sprintf(result, "%s > %s", lhs, rhs);
            
            myfree(lhs, M_STRING);
            myfree(rhs, M_STRING);
            break;
        }
        
        case EXPR_GE: {
            // Greater or equal: lhs >= rhs
            char *lhs = unparse_expr_to_string(expr->e.bin.lhs, symtab);
            char *rhs = unparse_expr_to_string(expr->e.bin.rhs, symtab);
            
            result = (char *)mymalloc(strlen(lhs) + strlen(rhs) + 5, M_STRING);
            sprintf(result, "%s >= %s", lhs, rhs);
            
            myfree(lhs, M_STRING);
            myfree(rhs, M_STRING);
            break;
        }
        
        default:
            // Unimplemented expression type
            result = (char *)mymalloc(32, M_STRING);
            sprintf(result, "/* %s */", expr_type_names[expr->kind]);
            break;
    }
    
    return result;
}

// Helper function to unparse a single statement to string
static char *unparse_single_stmt(Stmt *stmt, Symtab *symtab) {
    if (!stmt) {
        return nullptr;
    }
    
    char *result = nullptr;
    
    switch (stmt->kind) {
        case STMT_RETURN: {
            if (stmt->s.expr) {
                // return expr;
                char *expr_str = unparse_expr_to_string(stmt->s.expr, symtab);
                result = (char *)mymalloc(strlen(expr_str) + 9, M_STRING);
                sprintf(result, "return %s;", expr_str);
                myfree(expr_str, M_STRING);
            } else {
                // bare return;
                result = (char *)mymalloc(8, M_STRING);
                strcpy(result, "return;");
            }
            break;
        }
        
        case STMT_EXPR: {
            // Expression statement: expr;
            char *expr_str = unparse_expr_to_string(stmt->s.expr, symtab);
            result = (char *)mymalloc(strlen(expr_str) + 2, M_STRING);
            sprintf(result, "%s;", expr_str);
            myfree(expr_str, M_STRING);
            break;
        }
        
        default:
            // Unimplemented statement type
            result = (char *)mymalloc(64, M_STRING);
            sprintf(result, "/* unimplemented stmt: %s */", stmt_type_names[stmt->kind]);
            break;
    }
    
    return result;
}

// Convert Stmt* to Var list - real implementation
static Var unparse_stmt_to_var(Stmt *stmt, Symtab *symtab) {
    if (!stmt) {
        return new_list(0);  // Return empty list for null stmt
    }
    
    // Handle statement sequences (linked list via next pointer)
    Var lines = new_list(0);
    int line_count = 0;
    
    for (Stmt *s = stmt; s; s = s->next) {
        char *line = unparse_single_stmt(s, symtab);
        if (line) {
            line_count++;
            lines = listinsert(lines, str_dup_to_var(line), line_count);
            myfree(line, M_STRING);
        }
    }
    
    return lines;
}

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
    
    // Create temporary symbol table (empty for now)
    Symtab *symtab = create_symtab(0);
    
    // Allocate AST in temporary pool 
    begin_code_allocation();
    
    Stmt *stmt = map_to_stmt(ast_map, symtab);
    if (!stmt) {
        end_code_allocation(0);  // Clean up on error
        free_symtab(symtab);
        return make_error_pack(E_INVARG);
    }
    
    // Convert AST to code using wrapper function
    Var result = unparse_stmt_to_var(stmt, symtab);
    
    end_code_allocation(0);  // Free entire AST pool
    free_symtab(symtab);
    return make_var_pack(result);
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