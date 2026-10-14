/******************************************************************************
  Syntax trees as MOO values.  See ast_map.cc.
 *****************************************************************************/

#ifndef AST_Map_h
#define AST_Map_h 1

#include "program.h"
#include "structures.h"

/* The syntax tree of `program' as a list of statement nodes. */
extern Var program_to_ast(Program * program);

#endif				/* !AST_Map_h */
