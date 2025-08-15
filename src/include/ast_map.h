/******************************************************************************
  ToastStunt AST Map Converter - Header
  
  Public interface for converting between AST nodes and MOO MAP representations.
 *****************************************************************************/

#ifndef AST_MAP_H
#define AST_MAP_H

#include "structures.h"

// Forward declarations
struct Expr;
struct Stmt;

// Symbol table structure for variable name ↔ ID mappings
typedef struct {
    int num_names;
    char **names;     // Array of variable names (indexed by ID)
} Symtab;

// Main conversion functions
extern Var expr_to_map(struct Expr *expr, Symtab *symtab);
extern Var stmt_to_map(struct Stmt *stmt, Symtab *symtab);

extern struct Expr *map_to_expr(Var map, Symtab *symtab);
extern struct Stmt *map_to_stmt(Var map, Symtab *symtab);

// Symbol table management
extern Symtab *create_symtab(int num_names);
extern void free_symtab(Symtab *symtab);
extern int symtab_add_name(Symtab *symtab, const char *name);
extern const char *symtab_get_name(Symtab *symtab, int id);

// Builtin function registration
extern void register_ast(void);

#endif /* AST_MAP_H */