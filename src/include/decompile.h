/******************************************************************************
  Copyright (c) 1995, 1996 Xerox Corporation.  All rights reserved.
  Portions of this code were written by Stephen White, aka ghond.
  Use and copying of this software and preparation of derivative works based
  upon this software are permitted.  Any distribution of this software or
  derivative works must comply with all applicable United States export
  control laws.  This software is made available AS IS, and Xerox Corporation
  makes no warranty about the software, its performance or its conformity to
  any specification.  Any person obtaining a copy of this software is requested
  to send their name and post office or electronic mail address to:
    Pavel Curtis
    Xerox PARC
    3333 Coyote Hill Rd.
    Palo Alto, CA 94304
    Pavel@Xerox.Com
 *****************************************************************************/

#include <vector>

#include "ast.h"
#include "program.h"

extern Stmt *decompile_program(Program * program, int vector);
extern unsigned find_line_number(Program * program, int vector, unsigned pc);
/* Appends to LINES the line of every PC whose bit is set in PC_BITS (one bit
 * per byte of VECTOR), decompiling PROGRAM once.  The PCs need not be ones a
 * traceback can name; a PC that belongs to no line adds nothing.  LINES may
 * receive a line more than once. */
extern void find_line_numbers(Program * program, int vector,
                              const unsigned char *pc_bits,
                              std::vector<unsigned> &lines);
