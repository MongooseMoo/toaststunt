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

#include <unordered_map>
#include <vector>

#include "ast.h"
#include "decompile.h"
#include "opcode.h"
#include "program.h"
#include "server.h"
#include "storage.h"
#include "utils.h"

static Program *program;
static Expr **expr_stack;
static int top_expr_stack;

/* The PCs whose lines are wanted ("hot" bytes) all lie in one bytecode vector
 * of the program: one PC for a traceback, a bitmap of PCs for coverage.
 * Decompiling marks the tree node each hot byte belongs to; a mark on an
 * expression moves up to the statement that holds it.  A statement can be
 * marked at more than one position. */
enum { TOP = 1, ENDBODY = 2, BOTTOM = 4 };

static Byte *hot_base;                      /* start of the vector with hot bytes */
static unsigned hot_size;                   /* its length; 0 when nothing is hot */
static unsigned hot_pc;                     /* the one hot PC, when hot_bits is null */
static const unsigned char *hot_bits;       /* or one bit per PC */
static std::unordered_map<void *, unsigned> hot_marks;  /* node -> positions */
static bool any_marked;                     /* a hot byte was matched to a node */

static inline bool
is_hot_byte(const Byte *ptr)
{
    if (ptr < hot_base || ptr >= hot_base + hot_size)
        return false;
    unsigned pc = ptr - hot_base;
    return hot_bits ? (hot_bits[pc / 8] >> (pc % 8)) & 1 : pc == hot_pc;
}

static inline void *
mark_hot(void *node, unsigned position)
{
    hot_marks[node] |= position;
    any_marked = true;
    return node;
}

static inline bool
is_marked(void *node, unsigned position = TOP)
{
    if (!any_marked)
        return false;
    auto mark = hot_marks.find(node);
    return mark != hot_marks.end() && (mark->second & position);
}

/* Free a node the tree will not keep.  Returns whether it was marked, so the
 * caller can pass the mark on; the freed address may be handed out again and
 * must not stay marked. */
static bool
drop_node(void *node)
{
    bool marked = any_marked && hot_marks.erase(node) > 0;

    dealloc_node(node);
    return marked;
}

static int lineno;

static void
push_expr(Expr * expr)
{
    expr_stack[top_expr_stack++] = expr;
}

static Expr *
pop_expr(void)
{
    if (!top_expr_stack)
        panic_moo("Empty expr stack in DECOMPILE!");
    return expr_stack[--top_expr_stack];
}

#define ADD_STMT(stmt)      \
    do {                \
        Stmt *temp = stmt;      \
        \
        *stmt_sink = temp;      \
        stmt_sink = &(temp->next);  \
        stmt_start = ptr - bc.vector;\
    } while (0);

#define ADD_ARM(arm)        \
    do {                \
        Cond_Arm *temp = arm;   \
        \
        *arm_sink = temp;       \
        arm_sink = &(temp->next);   \
    } while (0);

#define SKIP_BYTES(n)  ((void)(ptr += n))
#define READ_BYTES(n)           \
    (ptr += n,                \
     (n == 1              \
      ? ptr[-1]               \
      : (n == 2               \
         ? ((unsigned) ptr[-2] << 8)  \
         + ptr[-1]          \
         : ((unsigned) ptr[-4] << 24) \
         + ((unsigned) ptr[-3] << 16)   \
         + ((unsigned) ptr[-2] << 8)    \
         + ptr[-1])))

#define READ_LABEL()    READ_BYTES(bc.numbytes_label)
#define READ_LITERAL()  program->literals[READ_BYTES(bc.numbytes_literal)]
#define READ_FORK() program->fork_vectors[READ_BYTES(bc.numbytes_fork)]
#define READ_ID()   READ_BYTES(bc.numbytes_var_name)
#define READ_STACK()    SKIP_BYTES(bc.numbytes_stack)

#define READ_JUMP(is_hot)   read_jump(bc.numbytes_label, &ptr, &is_hot)

static unsigned
read_jump(unsigned numbytes_label, Byte ** p, int *is_hot)
{
    Byte *ptr = *p;
    unsigned label;

    *is_hot = is_hot_byte(ptr);
    if (*ptr++ != OP_JUMP)
        panic_moo("Missing JUMP in DECOMPILE!");

    label = READ_BYTES(numbytes_label);
    *p = ptr;

    return label;
}

#define HOT(is_hot, n)      (node = n, (is_hot) ? mark_hot(node, TOP) : node)
#define HOT1(is_hot, kid, n)    HOT(is_hot || is_marked(kid), n)
#define HOT2(is_hot, kid1, kid2, n) \
    HOT1(is_hot || is_marked(kid1), kid2, n)
#define HOT3(is_hot, kid1, kid2, kid3, n) \
    HOT2(is_hot || is_marked(kid1), kid2, kid3, n)
#define HOT4(is_hot, kid1, kid2, kid3, kid4, n) \
    HOT3(is_hot || is_marked(kid1), kid2, kid3, kid4, n)

#define HOT_OP(n)       HOT(op_hot, n)
#define HOT_OP1(kid, n)     HOT1(op_hot, kid, n)
#define HOT_OP2(kid1, kid2, n)  HOT2(op_hot, kid1, kid2, n)
#define HOT_OP3(kid1, kid2, kid3, n)    \
    HOT3(op_hot, kid1, kid2, kid3, n)

#define HOT_POS(is_hot, n, pos) \
    if (is_hot) { mark_hot(n, pos); }
#define HOT_BOTTOM(is_hot, n)   HOT_POS(is_hot, n, BOTTOM)

#define DECOMPILE(bc, start, end, stmt_sink, arm_sink)  \
    (ptr = decompile(bc, start, end, stmt_sink, arm_sink))

static Byte *
decompile(Bytecodes bc, Byte * start, Byte * end, Stmt ** stmt_sink,
          Cond_Arm ** arm_sink)
{
    /*** The reader will likely find it useful to consult the file
     *** `MOOCodeSequences.txt' in this directory while reading this function.
     ***/

    Byte *ptr = start;
    unsigned stmt_start = start - bc.vector;
    unsigned jump_len = bc.numbytes_label + 1;
    Stmt *s;
    Expr *e;
    enum Expr_Kind kind;
    void *node;
    int asgn_hot = 0;

    if (stmt_sink)
        *stmt_sink = nullptr;
    if (arm_sink)
        *arm_sink = nullptr;

    while (ptr < end) {
        int op_hot = is_hot_byte(ptr);
        Opcode op = (Opcode)(*ptr++);

        if (IS_PUSH_n(op)) {
            e = alloc_expr(EXPR_ID);
            e->e.id = PUSH_n_INDEX(op);
            push_expr((Expr *)HOT_OP(e));
            continue;
#ifdef BYTECODE_REDUCE_REF
        } else if (IS_PUSH_CLEAR_n(op)) {
            e = alloc_expr(EXPR_ID);
            e->e.id = PUSH_CLEAR_n_INDEX(op);
            push_expr((Expr *)HOT_OP(e));
            continue;
#endif              /* BYTECODE_REDUCE_REF */
        } else if (IS_PUT_n(op)) {
            e = alloc_expr(EXPR_ID);
            e->e.id = PUT_n_INDEX(op);
            e = alloc_binary(EXPR_ASGN, e, pop_expr());
            push_expr((Expr *)HOT_OP1(e->e.bin.rhs, e));
            continue;
        } else if (IS_OPTIM_NUM_OPCODE(op)) {
            e = alloc_var(TYPE_INT);
            e->e.var.v.num = OPCODE_TO_OPTIM_NUM(op);
            push_expr((Expr *)HOT_OP(e));
            continue;
        }
        switch (op) {
            case OP_IF:
            {
                unsigned next = READ_LABEL();
                Expr *condition = pop_expr();
                Stmt *arm_stmts;
                Cond_Arm *arm;
                unsigned done;
                int jump_hot;

                s = alloc_stmt(STMT_COND);
                DECOMPILE(bc, ptr, bc.vector + next - jump_len, &arm_stmts, nullptr);
                arm = s->s.cond.arms = alloc_cond_arm(condition, arm_stmts);
                HOT_OP1(condition, arm);
                done = READ_JUMP(jump_hot);
                HOT_BOTTOM(jump_hot, arm);
                DECOMPILE(bc, ptr, bc.vector + done, &(s->s.cond.otherwise),
                          &(arm->next));

                ADD_STMT(s);
            }
            break;
            case OP_EIF:
            {
                unsigned next = READ_LABEL();
                Expr *condition = pop_expr();
                Stmt *arm_stmts;
                Cond_Arm *arm;
                unsigned done;
                int jump_hot;

                DECOMPILE(bc, ptr, bc.vector + next - jump_len, &arm_stmts, nullptr);
                ADD_ARM(arm = alloc_cond_arm(condition, arm_stmts));
                HOT_OP1(condition, arm);
                done = READ_JUMP(jump_hot);
                HOT_BOTTOM(jump_hot, arm);
                if (bc.vector + done != end)
                    panic_moo("ELSEIF jumps to wrong place in DECOMPILE!");
                stmt_start = ptr - bc.vector;
            }
            break;
            case OP_FOR_RANGE:
            {
                unsigned top = (ptr - 1) - bc.vector;
                int id = READ_ID();
                unsigned done = READ_LABEL();
                Expr *to = pop_expr();
                Expr *from = pop_expr();
                int jump_hot;

                s = alloc_stmt(STMT_RANGE);
                s->s.range.id = id;
                s->s.range.from = from;
                s->s.range.to = to;
                DECOMPILE(bc, ptr, bc.vector + done - jump_len,
                          &(s->s.range.body), nullptr);
                if (top != READ_JUMP(jump_hot))
                    panic_moo("FOR_RANGE jumps to wrong place in DECOMPILE!");
                HOT_BOTTOM(jump_hot, s);
                ADD_STMT((Stmt *)HOT_OP2(from, to, s));
            }
            break;
            case OP_WHILE:
                s = alloc_stmt(STMT_WHILE);
                s->s.loop.id = -1;
finish_while:
                {
                    unsigned top = stmt_start;
                    unsigned done = READ_LABEL();
                    Expr *condition = pop_expr();
                    int jump_hot;

                    s->s.loop.condition = condition;
                    DECOMPILE(bc, ptr, bc.vector + done - jump_len,
                              &(s->s.loop.body), nullptr);
                    if (top != READ_JUMP(jump_hot))
                        panic_moo("WHILE jumps to wrong place in DECOMPILE!");
                    HOT_BOTTOM(jump_hot, s);
                    ADD_STMT((Stmt *)HOT_OP1(condition, s));
                }
                break;
            case OP_FORK:
            case OP_FORK_WITH_ID:
            {
                Bytecodes fbc;
                int id;
                Expr *time = pop_expr();

                fbc = READ_FORK();
                id = (op == OP_FORK ? -1 : READ_ID());
                s = alloc_stmt(STMT_FORK);
                s->s.fork.id = id;
                s->s.fork.time = time;
                (void) decompile(fbc, fbc.vector, fbc.vector + fbc.size,
                                 &(s->s.fork.body), nullptr);
                HOT_BOTTOM(is_hot_byte(fbc.vector + fbc.size - 1), s);
                ADD_STMT((Stmt *)HOT_OP1(time, s));
            }
            break;
            case OP_POP:
                s = alloc_stmt(STMT_EXPR);
                e = s->s.expr = pop_expr();
                ADD_STMT((Stmt *)HOT_OP1(e, s));
                break;
            case OP_RETURN:
            case OP_RETURN0:
                s = alloc_stmt(STMT_RETURN);
                e = s->s.expr = (op == OP_RETURN ? pop_expr() : nullptr);
                ADD_STMT((Stmt *)HOT(op_hot || (e && is_marked(e)), s));
                break;
            case OP_DONE:
                if (ptr != end)
                    panic_moo("DONE not at end in DECOMPILE!");
                break;
            case OP_IMM:
                e = alloc_expr(EXPR_VAR);
                e->e.var = var_ref(READ_LITERAL());
                push_expr((Expr *)HOT_OP(e));
                break;
            case OP_G_PUSH:
#ifdef BYTECODE_REDUCE_REF
            case OP_G_PUSH_CLEAR:
#endif              /* BYTECODE_REDUCE_REF */
                e = alloc_expr(EXPR_ID);
                e->e.id = READ_ID();
                push_expr((Expr *)HOT_OP(e));
                break;
            case OP_AND:
            case OP_OR:
            {
                unsigned done = READ_LABEL();

                e = pop_expr();
                DECOMPILE(bc, ptr, bc.vector + done, nullptr, nullptr);
                if (ptr != bc.vector + done)
                    panic_moo("AND/OR jumps to wrong place in DECOMPILE!");
                e = alloc_binary(op == OP_AND ? EXPR_AND : EXPR_OR,
                                 e, pop_expr());
                push_expr((Expr *)HOT_OP2(e->e.bin.lhs, e->e.bin.rhs, e));
            }
            break;
            case OP_UNARY_MINUS:
            case OP_NOT:
                e = alloc_expr(op == OP_NOT ? EXPR_NOT : EXPR_NEGATE);
                e->e.expr = pop_expr();
                push_expr((Expr *)HOT_OP1(e->e.expr, e));
                break;
            case OP_GET_PROP:
            case OP_PUSH_GET_PROP:
                kind = EXPR_PROP;
                goto finish_binary;
            case OP_EQ:
                kind = EXPR_EQ;
                goto finish_binary;
            case OP_NE:
                kind = EXPR_NE;
                goto finish_binary;
            case OP_LT:
                kind = EXPR_LT;
                goto finish_binary;
            case OP_LE:
                kind = EXPR_LE;
                goto finish_binary;
            case OP_GT:
                kind = EXPR_GT;
                goto finish_binary;
            case OP_GE:
                kind = EXPR_GE;
                goto finish_binary;
            case OP_IN:
                kind = EXPR_IN;
                goto finish_binary;
            case OP_ADD:
                kind = EXPR_PLUS;
                goto finish_binary;
            case OP_MINUS:
                kind = EXPR_MINUS;
                goto finish_binary;
            case OP_MULT:
                kind = EXPR_TIMES;
                goto finish_binary;
            case OP_DIV:
                kind = EXPR_DIVIDE;
                goto finish_binary;
            case OP_MOD:
                kind = EXPR_MOD;
                goto finish_binary;
            case OP_REF:
            case OP_PUSH_REF:
                kind = EXPR_INDEX;
finish_binary:
                e = pop_expr();
                e = alloc_binary(kind, pop_expr(), e);
                push_expr((Expr *)HOT_OP2(e->e.bin.lhs, e->e.bin.rhs, e));
                break;
            case OP_RANGE_REF:
            {
                Expr *e1 = pop_expr();
                Expr *e2 = pop_expr();

                e = alloc_expr(EXPR_RANGE);
                e->e.range.base = pop_expr();
                e->e.range.from = e2;
                e->e.range.to = e1;
                push_expr((Expr *)HOT_OP3(e1, e2, e->e.range.base, e));
            }
            break;
            case OP_BI_FUNC_CALL:
            {
                Expr *a = pop_expr();

                if (a->kind != EXPR_LIST)
                    panic_moo("Missing arglist for BI_FUNC_CALL in DECOMPILE!");
                e = alloc_expr(EXPR_CALL);
                e->e.call.args = a->e.list;
                bool args_hot = drop_node(a);
                e->e.call.func = READ_BYTES(1);
                push_expr((Expr *)HOT(op_hot || args_hot, e));
            }
            break;
            case OP_CALL_VERB:
            {
                Expr *a = pop_expr();
                Expr *e2 = pop_expr();

                if (a->kind != EXPR_LIST)
                    panic_moo("Missing arglist for CALL_VERB in DECOMPILE!");
                e = alloc_verb(pop_expr(), e2, a->e.list);
                bool args_hot = drop_node(a);
                push_expr((Expr *)HOT2(op_hot || args_hot, e->e.verb.obj, e2, e));
            }
            break;
            case OP_IF_QUES:
            {
                unsigned label = READ_LABEL();
                int jump_hot;

                e = alloc_expr(EXPR_COND);
                e->e.cond.condition = pop_expr();
                DECOMPILE(bc, ptr, bc.vector + label - jump_len, nullptr, nullptr);
                label = READ_JUMP(jump_hot);
                e->e.cond.consequent = pop_expr();
                DECOMPILE(bc, ptr, bc.vector + label, nullptr, nullptr);
                if (ptr != bc.vector + label)
                    panic_moo("THEN jumps to wrong place in DECOMPILE!");
                e->e.cond.alternate = pop_expr();
                push_expr((Expr *)HOT3(op_hot || jump_hot, e->e.cond.condition,
                                       e->e.cond.consequent, e->e.cond.alternate,
                                       e));
            }
            break;
            case OP_PUT_TEMP:
                /* Ignore; following RANGESET or INDEXSET does the work */
                if (op_hot)
                    asgn_hot = 1;
                break;
            case OP_INDEXSET:
                /* Most of the lvalue has already been constructed on the stack.
                 * Add the final indexing.
                 */
            {
                Expr *rvalue = pop_expr();
                Expr *index = pop_expr();

                e = alloc_binary(EXPR_INDEX, pop_expr(), index);
                push_expr((Expr *)HOT3(op_hot || asgn_hot,
                                       e->e.bin.lhs, index, rvalue,
                                       alloc_binary(EXPR_ASGN, e, rvalue)));
            }
finish_indexed_assignment:
                /* The remainder of this complex assignment code sequence is
                 * useless for the purpose of decompilation.
                 */
            asgn_hot = 0;
            while (*ptr != OP_PUSH_TEMP) {
                if (is_hot_byte(ptr))   /* it's our assignment expression */
                    mark_hot(expr_stack[top_expr_stack - 1], TOP);
                ptr++;
            }
            if (is_hot_byte(ptr))   /* the closing PUSH_TEMP is part of it too */
                mark_hot(expr_stack[top_expr_stack - 1], TOP);
            ptr++;
            break;
            case OP_G_PUT:
                e = alloc_expr(EXPR_ID);
                e->e.id = READ_ID();
                e = alloc_binary(EXPR_ASGN, e, pop_expr());
                push_expr((Expr *)HOT_OP1(e->e.bin.rhs, e));
                break;
            case OP_PUT_PROP:
            {
                Expr *rvalue = pop_expr();

                e = pop_expr();
                e = alloc_binary(EXPR_PROP, pop_expr(), e);
                push_expr((Expr *)HOT_OP3(e->e.bin.lhs, e->e.bin.rhs, rvalue,
                                          alloc_binary(EXPR_ASGN, e, rvalue)));
            }
            break;
            case OP_MAP_CREATE:
                e = alloc_expr(EXPR_MAP);
                e->e.map = nullptr;
                push_expr((Expr *)HOT_OP(e));
                break;
            case OP_MAP_INSERT:
            {
                Expr *key, *value;

                key = pop_expr();
                value = pop_expr();
                e = pop_expr();
                if (e->e.map) {
                    Map_List *last = e->e.map;
                    while (last->next) last = last->next;
                    last->next = alloc_map_list(key, value);
                }
                else
                    e->e.map = alloc_map_list(key, value);
                push_expr((Expr *)HOT_OP2(key, value, e));
            }
            break;
            case OP_MAKE_EMPTY_LIST:
                e = alloc_expr(EXPR_LIST);
                e->e.list = nullptr;
                push_expr((Expr *)HOT_OP(e));
                break;
            case OP_MAKE_SINGLETON_LIST:
                e = alloc_expr(EXPR_LIST);
                e->e.list = alloc_arg_list(ARG_NORMAL, pop_expr());
                push_expr((Expr *)HOT_OP1(e->e.list->expr, e));
                break;
            case OP_CHECK_LIST_FOR_SPLICE:
                e = alloc_expr(EXPR_LIST);
                e->e.list = alloc_arg_list(ARG_SPLICE, pop_expr());
                push_expr((Expr *)HOT_OP1(e->e.list->expr, e));
                break;
            case OP_LIST_ADD_TAIL:
            case OP_LIST_APPEND:
            {
                Expr *list;
                Arg_List *a;

                e = pop_expr();
                list = pop_expr();
                if (list->kind != EXPR_LIST)
                    panic_moo("Missing list expression in DECOMPILE!");
                for (a = list->e.list; a->next; a = a->next);
                a->next = alloc_arg_list(op == OP_LIST_APPEND ? ARG_SPLICE
                                         : ARG_NORMAL, e);
                push_expr((Expr *)HOT_OP1(e, list));
            }
            break;
            case OP_PUSH_TEMP:
            case OP_JUMP:
                panic_moo("Unexpected opcode in DECOMPILE!");
                break;
            case OP_EXTENDED:
            {
                Extended_Opcode eop = (Extended_Opcode)(*ptr++);

                switch (eop) {
                    case EOP_RANGESET:
                        /* Most of the lvalue has already been constructed on the
                         * stack.  Add the final subranging.
                         */
                    {
                        Expr *rvalue = pop_expr();

                        e = alloc_expr(EXPR_RANGE);
                        e->e.range.to = pop_expr();
                        e->e.range.from = pop_expr();
                        e->e.range.base = pop_expr();
                        push_expr((Expr *)HOT4(op_hot || asgn_hot,
                                               e->e.range.base, e->e.range.from,
                                               e->e.range.to, rvalue,
                                               alloc_binary(EXPR_ASGN, e, rvalue)));
                    }
                    goto finish_indexed_assignment;
                    case EOP_FIRST:
                        READ_STACK();
                        e = alloc_expr(EXPR_FIRST);
                        push_expr((Expr *)HOT_OP(e));
                        break;
                    case EOP_LAST:
                        READ_STACK();
                        e = alloc_expr(EXPR_LAST);
                        push_expr((Expr *)HOT_OP(e));
                        break;
                    case EOP_EXP:
                        kind = EXPR_EXP;
                        goto finish_binary;
                    case EOP_SCATTER:
                    {
                        Scatter *sc, **scp;
                        int nargs = *ptr++;
                        int rest = (ptr++, *ptr++); /* skip nreq */
                        int *next_label = nullptr;
                        int i, done, is_hot = op_hot;

                        for (i = 1, scp = &sc;
                                i <= nargs;
                                i++, scp = &((*scp)->next)) {
                            int id = READ_ID();
                            int label = READ_LABEL();

                            *scp = alloc_scatter(i == rest ? SCAT_REST :
                                                 label == 0 ? SCAT_REQUIRED
                                                 : SCAT_OPTIONAL,
                                                 id, nullptr);
                            if (label > 1) {
                                (*scp)->label = label;
                                if (next_label)
                                    *next_label = label;
                                next_label = &((*scp)->next_label);
                            } else
                                (*scp)->label = 0;
                        }
                        done = READ_LABEL();
                        if (next_label)
                            *next_label = done;
                        e = alloc_expr(EXPR_SCATTER);
                        e->e.scatter = sc;
                        for (sc = e->e.scatter; sc; sc = sc->next)
                            if (sc->label) {
                                Expr *defallt;

                                if (ptr != bc.vector + sc->label)
                                    panic_moo("Misplaced default in DECOMPILE!");
                                DECOMPILE(bc, ptr,
                                          bc.vector + sc->next_label - 1,
                                          nullptr, nullptr);
                                defallt = pop_expr();
                                HOT1(is_hot, defallt, e);
                                if (defallt->kind != EXPR_ASGN
                                        || defallt->e.bin.lhs->kind != EXPR_ID
                                        || defallt->e.bin.lhs->e.id != sc->id)
                                    panic_moo("Wrong variable in DECOMPILE!");
                                sc->expr = defallt->e.bin.rhs;
                                drop_node(defallt->e.bin.lhs);
                                drop_node(defallt);
                                is_hot = (is_hot || is_hot_byte(ptr));
                                if (*ptr++ != OP_POP)
                                    panic_moo("Missing default POP in DECOMPILE!");
                            }
                        e = alloc_binary(EXPR_ASGN, e, pop_expr());
                        push_expr((Expr *)HOT2(is_hot, e->e.bin.lhs, e->e.bin.rhs, e));
                        if (ptr != bc.vector + done)
                            panic_moo("Not at end of scatter in DECOMPILE!");
                    }
                    break;
                    case EOP_PUSH_LABEL:
                        e = alloc_var(TYPE_INT);
                        e->e.var.v.num = READ_LABEL();
                        push_expr((Expr *)HOT_OP(e));
                        break;
                    case EOP_CATCH:
                    {
                        Expr *label_expr = pop_expr();
                        Expr *codes = pop_expr();
                        Expr *try_expr, *default_expr = nullptr;
                        Arg_List *a = nullptr;  /* silence warning */
                        int label, done;
                        int is_hot = op_hot;

                        if (label_expr->kind != EXPR_VAR
                                || label_expr->e.var.type != TYPE_INT)
                            panic_moo("Not a catch label in DECOMPILE!");
                        label = label_expr->e.var.v.num;
                        if (drop_node(label_expr))
                            is_hot = 1;
                        if (codes->kind == EXPR_LIST)
                            a = codes->e.list;
                        else if (codes->kind == EXPR_VAR
                                 && codes->e.var.type == TYPE_INT
                                 && codes->e.var.v.num == 0)
                            a = nullptr;
                        else
                            panic_moo("Not a codes expression in DECOMPILE!");
                        if (drop_node(codes))
                            is_hot = 1;
                        DECOMPILE(bc, ptr, end, nullptr, nullptr);
                        /* Step over OP_EXTENDED whether or not is_hot is already set. */
                        if (is_hot_byte(ptr++))
                            is_hot = 1;
                        if (*ptr++ != EOP_END_CATCH)
                            panic_moo("Missing END_CATCH in DECOMPILE!");
                        done = READ_LABEL();
                        try_expr = pop_expr();
                        if (ptr != bc.vector + label)
                            panic_moo("Misplaced handler in DECOMPILE!");
                        is_hot = (is_hot || is_hot_byte(ptr));
                        op = (Opcode)(*ptr++);
                        if (op == OPTIM_NUM_TO_OPCODE(1)) {
                            /* No default expression */
                            is_hot = (is_hot || is_hot_byte(ptr));
                            if (*ptr++ != OP_REF)
                                panic_moo("Missing REF in DECOMPILE!");
                            default_expr = nullptr;
                        } else if (op == OP_POP) {
                            /* There's a default expression */
                            DECOMPILE(bc, ptr, bc.vector + done, nullptr, nullptr);
                            default_expr = pop_expr();
                        } else
                            panic_moo("Bad default expression in DECOMPILE!");
                        if (ptr != bc.vector + done)
                            panic_moo("CATCH ends in wrong place in DECOMPILE!");
                        e = alloc_expr(EXPR_CATCH);
                        e->e._catch._try = try_expr;
                        e->e._catch.codes = a;
                        e->e._catch.except = default_expr;
                        push_expr((Expr *)HOT1(is_hot || (default_expr
                                                          && is_marked(default_expr)),
                                               try_expr, e));
                    }
                    break;
                    case EOP_END_CATCH:
                        /* Early exit; main logic is in CATCH case, above. */
                        return ptr - 2;
                    case EOP_TRY_EXCEPT:
                    {
                        Expr *label_expr;
                        Except_Arm *ex;
                        Arg_List *a = nullptr;  /* silence warning */
                        int count = *ptr++, label;
                        unsigned done;

                        s = (Stmt *)HOT_OP(alloc_stmt(STMT_TRY_EXCEPT));
                        s->s._catch.excepts = nullptr;
                        while (count--) {
                            label_expr = pop_expr();
                            if (label_expr->kind != EXPR_VAR
                                    || label_expr->e.var.type != TYPE_INT)
                                panic_moo("Not an except label in DECOMPILE!");
                            label = label_expr->e.var.v.num;
                            bool arm_hot = drop_node(label_expr);
                            e = pop_expr();
                            if (e->kind == EXPR_LIST)
                                a = e->e.list;
                            else if (e->kind == EXPR_VAR
                                     && e->e.var.type == TYPE_INT
                                     && e->e.var.v.num == 0)
                                a = nullptr;
                            else
                                panic_moo("Not a codes expression in DECOMPILE!");
                            if (drop_node(e))
                                arm_hot = true;
                            ex = alloc_except(-1, a, nullptr);
                            ex->label = label;
                            ex->next = s->s._catch.excepts;
                            HOT(arm_hot, ex);
                            s->s._catch.excepts = ex;
                        }
                        DECOMPILE(bc, ptr, end, &(s->s._catch.body), nullptr);
                        HOT_POS(is_hot_byte(ptr++), s, ENDBODY);
                        if (*ptr++ != EOP_END_EXCEPT)
                            panic_moo("Missing END_EXCEPT in DECOMPILE!");
                        done = READ_LABEL();
                        for (ex = s->s._catch.excepts; ex; ex = ex->next) {
                            Byte *stop;
                            int jump_hot = 0;

                            if (ex->label != ptr - bc.vector)
                                panic_moo("Not at start of handler in DECOMPILE!");
                            op_hot = is_hot_byte(ptr);
                            op = (Opcode)(*ptr++);
                            if (op == OP_G_PUT) {
                                ex->id = READ_ID();
                                op = (Opcode)(*ptr++);
                            } else if (IS_PUT_n(op)) {
                                ex->id = PUT_n_INDEX(op);
                                op = (Opcode)(*ptr++);
                            }
                            HOT(op_hot || is_hot_byte(ptr - 1), ex);
                            if (op != OP_POP)
                                panic_moo("Missing POP in DECOMPILE!");
                            if (ex->next)
                                stop = bc.vector + ex->next->label - jump_len;
                            else
                                stop = bc.vector + done;
                            DECOMPILE(bc, ptr, stop, &(ex->stmt), nullptr);
                            if (ex->next && READ_JUMP(jump_hot) != done)
                                panic_moo("EXCEPT jumps to wrong place in "
                                          "DECOMPILE!");
                            HOT_BOTTOM(jump_hot, ex);
                        }
                        if (ptr - bc.vector != done)
                            panic_moo("EXCEPTS end in wrong place in DECOMPILE!");
                        ADD_STMT(s);
                    }
                    break;
                    case EOP_END_EXCEPT:
                        /* Early exit; main logic is in TRY_EXCEPT case, above. */
                        return ptr - 2;
                    case EOP_TRY_FINALLY:
                    {
                        int label = READ_LABEL();

                        s = (Stmt *)HOT_OP(alloc_stmt(STMT_TRY_FINALLY));
                        DECOMPILE(bc, ptr, end, &(s->s.finally.body), nullptr);
                        HOT_POS(is_hot_byte(ptr++), s, ENDBODY);
                        if (*ptr++ != EOP_END_FINALLY)
                            panic_moo("Missing END_FINALLY in DECOMPILE!");
                        if (ptr - bc.vector != label)
                            panic_moo("FINALLY handler in wrong place in "
                                      "DECOMPILE!");
                        DECOMPILE(bc, ptr, end, &(s->s.finally.handler), nullptr);
                        HOT_BOTTOM(is_hot_byte(ptr++), s);
                        if (*ptr++ != EOP_CONTINUE)
                            panic_moo("Missing CONTINUE in DECOMPILE!");
                        ADD_STMT(s);
                    }
                    break;
                    case EOP_END_FINALLY:
                    case EOP_CONTINUE:
                        /* Early exit; main logic is in TRY_FINALLY case, above. */
                        return ptr - 2;
                    case EOP_WHILE_ID:
                        s = alloc_stmt(STMT_WHILE);
                        s->s.loop.id = READ_ID();
                        goto finish_while;
                    case EOP_EXIT:
                    case EOP_EXIT_ID:
                        s = alloc_stmt(STMT_BREAK);
                        if (eop == EOP_EXIT_ID)
                            s->s.exit = READ_ID();
                        else
                            s->s.exit = -1;
                        READ_STACK();
                        {
                            unsigned label = READ_LABEL();

                            if (label < ptr - bc.vector)
                                s->kind = STMT_CONTINUE;
                        }
                        ADD_STMT((Stmt *)HOT_OP(s));
                        break;

                    case EOP_FOR_LIST_1:
                    {
                        unsigned top = (ptr - 2) - bc.vector;
                        int id = READ_ID();
                        unsigned done = READ_LABEL();
                        Expr *iter = pop_expr();
                        Expr *list = pop_expr();
                        int jump_hot;
                        bool iter_hot = false;

                        if (iter->kind != EXPR_VAR
                                || iter->e.var.type != TYPE_NONE)
                            panic_moo("Not a `none' value in DECOMPILE!");
                        else
                            iter_hot = drop_node(iter);
                        s = alloc_stmt(STMT_LIST);
                        s->s.list.id = id;
                        s->s.list.index = -1;
                        s->s.list.expr = list;
                        DECOMPILE(bc, ptr, bc.vector + done - jump_len,
                                  &(s->s.list.body), nullptr);
                        if (top != READ_JUMP(jump_hot))
                            panic_moo("FOR_LIST_1 jumps to wrong place in DECOMPILE!");
                        HOT_BOTTOM(jump_hot, s);
                        ADD_STMT((Stmt *)HOT1(op_hot || iter_hot, list, s));
                    }
                    break;

                    case EOP_FOR_LIST_2:
                    {
                        unsigned top = (ptr - 2) - bc.vector;
                        int id = READ_ID();
                        int index = READ_ID();
                        unsigned done = READ_LABEL();
                        Expr *iter = pop_expr();
                        Expr *list = pop_expr();
                        int jump_hot;
                        bool iter_hot = false;

                        if (iter->kind != EXPR_VAR
                                || iter->e.var.type != TYPE_NONE)
                            panic_moo("Not a `none' value in DECOMPILE!");
                        else
                            iter_hot = drop_node(iter);
                        s = alloc_stmt(STMT_LIST);
                        s->s.list.id = id;
                        s->s.list.index = index;
                        s->s.list.expr = list;
                        DECOMPILE(bc, ptr, bc.vector + done - jump_len,
                                  &(s->s.list.body), nullptr);
                        if (top != READ_JUMP(jump_hot))
                            panic_moo("FOR_LIST_2 jumps to wrong place in DECOMPILE!");
                        HOT_BOTTOM(jump_hot, s);
                        ADD_STMT((Stmt *)HOT1(op_hot || iter_hot, list, s));
                    }
                    break;

                    case EOP_BITOR:
                        kind = EXPR_BITOR;
                        goto finish_extended_binary;
                    case EOP_BITAND:
                        kind = EXPR_BITAND;
                        goto finish_extended_binary;
                    case EOP_BITXOR:
                        kind = EXPR_BITXOR;
                        goto finish_extended_binary;
                    case EOP_BITSHL:
                        kind = EXPR_BITSHL;
                        goto finish_extended_binary;
                    case EOP_BITSHR:
                        kind = EXPR_BITSHR;
finish_extended_binary:
                        e = pop_expr();
                        e = alloc_binary(kind, pop_expr(), e);
                        push_expr((Expr *)HOT_OP2(e->e.bin.lhs, e->e.bin.rhs, e));
                        break;

                    case EOP_COMPLEMENT:
                        e = alloc_expr(EXPR_COMPLEMENT);
                        e->e.expr = pop_expr();
                        push_expr((Expr *)HOT_OP1(e->e.expr, e));
                        break;

                    default:
                        panic_moo("Unknown extended opcode in DECOMPILE!");
                }
            }
            break;
            default:
                panic_moo("Unknown opcode in DECOMPILE!");
        }
    }

    if (ptr != end)
        panic_moo("Overshot end in DECOMPILE!");

    return ptr;
}

/* Decompile VECTOR of PROG.  The hot bytes are in PC_VECTOR: the bits of
 * PC_BITS if given, else the single PC, else (PC < 0) none. */
static Stmt *
program_to_tree(Program * prog, int vector, int pc_vector, int pc,
                const unsigned char *pc_bits)
{
    Stmt *result;
    Bytecodes bc;
    int i, sum;

    program = prog;
    bc = (pc_vector == MAIN_VECTOR
          ? program->main_vector
          : program->fork_vectors[pc_vector]);

    hot_bits = pc_bits;
    hot_base = bc.vector;
    if (pc_bits)
        hot_size = bc.size;
    else if (pc < 0)
        hot_size = 0;
    else if (pc < bc.size) {
        hot_size = bc.size;
        hot_pc = pc;
    } else
        panic_moo("Illegal PC in FIND_LINE_NUMBER!");

    sum = program->main_vector.max_stack;
    for (i = 0; i < program->fork_vectors_size; i++)
        sum += program->fork_vectors[i].max_stack;
    expr_stack = (Expr **)mymalloc(sum * sizeof(Expr *), M_DECOMPILE);
    top_expr_stack = 0;

    bc = (vector == MAIN_VECTOR
          ? program->main_vector
          : program->fork_vectors[vector]);

    begin_code_allocation();
    decompile(bc, bc.vector, bc.vector + bc.size, &result, nullptr);
    end_code_allocation(0);

    myfree(expr_stack, M_DECOMPILE);

    return result;
}

Stmt *
decompile_program(Program * prog, int vector)
{
    return program_to_tree(prog, vector, MAIN_VECTOR, -1, nullptr);
}

static void
add_hot_lines(Stmt * stmt, std::vector<unsigned> &lines)
{
    /* Invariants: on entry, lineno is number of first line of STMT
     *                   on exit, lineno is number of first line after STMT
     * The line of every marked node and position is appended to LINES, in
     * the order the walk meets them.
     */

    for (; stmt; stmt = stmt->next) {
        if (is_marked(stmt, TOP))
            lines.push_back(lineno);

        switch (stmt->kind) {
            case STMT_COND:
            {
                Cond_Arm *arm;

                for (arm = stmt->s.cond.arms; arm; arm = arm->next) {
                    if (is_marked(arm, TOP))
                        lines.push_back(lineno);
                    lineno++;   /* advance to arm body */
                    add_hot_lines(arm->stmt, lines);
                    if (is_marked(arm, BOTTOM))
                        lines.push_back(lineno - 1);    /* Blame last line of arm */
                }
                if (stmt->s.cond.otherwise) {
                    lineno++;   /* Skip `else' line */
                    add_hot_lines(stmt->s.cond.otherwise, lines);
                }
            }
            break;
            case STMT_LIST:
                lineno++;       /* Skip `for' line */
                add_hot_lines(stmt->s.list.body, lines);
                break;
            case STMT_RANGE:
                lineno++;       /* Skip `for' line */
                add_hot_lines(stmt->s.range.body, lines);
                break;
            case STMT_WHILE:
                lineno++;       /* Skip `while' line */
                add_hot_lines(stmt->s.loop.body, lines);
                break;
            case STMT_FORK:
                lineno++;       /* Skip `fork' line */
                add_hot_lines(stmt->s.fork.body, lines);
                break;
            case STMT_EXPR:
            case STMT_RETURN:
            case STMT_BREAK:
            case STMT_CONTINUE:
                /* Nothing more to do */
                break;
            case STMT_TRY_EXCEPT:
            {
                Except_Arm *ex;

                lineno++;   /* Skip `try' line */
                add_hot_lines(stmt->s._catch.body, lines);
                if (is_marked(stmt, ENDBODY))
                    lines.push_back(lineno - 1);    /* blame it on last line of body */
                for (ex = stmt->s._catch.excepts; ex; ex = ex->next) {
                    if (is_marked(ex, TOP))
                        lines.push_back(lineno);
                    lineno++;   /* skip `except' line */
                    add_hot_lines(ex->stmt, lines);
                    if (is_marked(ex, BOTTOM))
                        lines.push_back(lineno - 1);    /* blame last line of handler */
                }
            }
            break;
            case STMT_TRY_FINALLY:
                lineno++;       /* Skip `try' line */
                add_hot_lines(stmt->s.finally.body, lines);
                if (is_marked(stmt, ENDBODY))
                    lines.push_back(lineno);
                lineno++;       /* Skip `finally' line */
                add_hot_lines(stmt->s.finally.handler, lines);
                break;
            default:
                panic_moo("Unknown statement kind in ADD_HOT_LINES!");
        }

        if (is_marked(stmt, BOTTOM))
            lines.push_back(lineno);

        lineno++;       /* Count this statement */
    }
}

/* Decompile PROG once and append to LINES the line of every hot byte that
 * belongs to one.  Leaves lineno one past the last line of PROG.  Returns
 * whether any hot byte was matched to a node. */
static bool
lines_of_hot_bytes(Program * prog, int vector, int pc, const unsigned char *pc_bits,
                   std::vector<unsigned> &lines)
{
    Stmt *tree = program_to_tree(prog, MAIN_VECTOR, vector, pc, pc_bits);
    bool matched;

    lineno = prog->first_lineno;
    add_hot_lines(tree, lines);
    free_stmt(tree);

    matched = any_marked;
    hot_size = 0;
    any_marked = false;
    std::unordered_map<void *, unsigned>().swap(hot_marks);
    return matched;
}

void
find_line_numbers(Program * prog, int vector, const unsigned char *pc_bits,
                  std::vector<unsigned> &lines)
{
    lines_of_hot_bytes(prog, vector, 0, pc_bits, lines);
}

unsigned
find_line_number(Program * prog, int vector, unsigned pc)
{
    std::vector<unsigned> lines;
    unsigned size, line;

    if (prog->cached_lineno_pc == pc && prog->cached_lineno_vec == vector)
        return prog->cached_lineno;

    size = (vector == MAIN_VECTOR ? prog->main_vector : prog->fork_vectors[vector]).size;
    /* The final instruction of a vector has no node; it gets the line after
     * the program, or its `endfork'. */
    if (!lines_of_hot_bytes(prog, vector, pc, nullptr, lines) && pc != size - 1)
        panic_moo("Can't do job in FIND_LINE_NUMBER!");
    line = lines.empty() ? lineno : lines.front();

    prog->cached_lineno_vec = vector;
    prog->cached_lineno_pc = pc;
    prog->cached_lineno = line;
    return line;
}
