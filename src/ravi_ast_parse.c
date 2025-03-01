/*
A parser and syntax tree builder for Ravi. This is work in progress.
Once ready it will be used to create a new byte code generator for Ravi.

The parser will perform following actions:

a) Generate syntax tree
b) Perform type checking (Ravi enhancement)
*/

#include "ravi_ast.h"

/* forward declarations */
static struct ast_node *parse_expression(struct parser_state *);
static void parse_statement_list(struct parser_state *, struct ast_node_list **list);
static struct ast_node *parse_statement(struct parser_state *);
static struct ast_node *new_function(struct parser_state *parser);
static struct ast_node *end_function(struct parser_state *parser);
static struct block_scope *new_scope(struct parser_state *parser);
static void end_scope(struct parser_state *parser);
static struct ast_node *new_literal_expression(struct parser_state *parser, ravitype_t type);
static struct ast_node *generate_label(struct parser_state *parser, TString *label);
static struct ast_container *new_ast_container(lua_State *L);

static void add_symbol(struct ast_container *container, struct lua_symbol_list **list, struct lua_symbol *sym) {
  ptrlist_add((struct ptr_list **)list, sym, &container->ptrlist_allocator);
}

static void add_ast_node(struct ast_container *container, struct ast_node_list **list, struct ast_node *node) {
  ptrlist_add((struct ptr_list **)list, node, &container->ptrlist_allocator);
}

static l_noret error_expected(LexState *ls, int token) {
  luaX_syntaxerror(ls, luaO_pushfstring(ls->L, "%s expected", luaX_token2str(ls, token)));
}

static int testnext(LexState *ls, int c) {
  if (ls->t.token == c) {
    luaX_next(ls);
    return 1;
  }
  else
    return 0;
}

static void check(LexState *ls, int c) {
  if (ls->t.token != c)
    error_expected(ls, c);
}

static void checknext(LexState *ls, int c) {
  check(ls, c);
  luaX_next(ls);
}

/*============================================================*/
/* GRAMMAR RULES */
/*============================================================*/

/*
** check whether current token is in the follow set of a block.
** 'until' closes syntactical blocks, but do not close scope,
** so it is handled in separate.
*/
static int block_follow(LexState *ls, int withuntil) {
  switch (ls->t.token) {
    case TK_ELSE:
    case TK_ELSEIF:
    case TK_END:
    case TK_EOS:
      return 1;
    case TK_UNTIL:
      return withuntil;
    default:
      return 0;
  }
}

static void check_match(LexState *ls, int what, int who, int where) {
  if (!testnext(ls, what)) {
    if (where == ls->linenumber)
      error_expected(ls, what);
    else {
      luaX_syntaxerror(ls, luaO_pushfstring(ls->L, "%s expected (to close %s at line %d)", luaX_token2str(ls, what),
                                            luaX_token2str(ls, who), where));
    }
  }
}

/* Check that current token is a name, and advance */
static TString *check_name_and_next(LexState *ls) {
  TString *ts;
  check(ls, TK_NAME);
  ts = ls->t.seminfo.ts;
  luaX_next(ls);
  return ts;
}

/* Check that current token is a name, and advance */
static TString *str_checkname(LexState *ls) {
  TString *ts;
  check(ls, TK_NAME);
  ts = ls->t.seminfo.ts;
  luaX_next(ls);
  return ts;
}

/* create a new local variable in function scope, and set the
 * variable type (RAVI - added type tt) */
static struct lua_symbol *new_local_symbol(struct parser_state *parser, TString *name, ravitype_t tt,
                                           TString *usertype) {
  struct block_scope *scope = parser->current_scope;
  struct lua_symbol *symbol = dmrC_allocator_allocate(&parser->container->symbol_allocator, 0);
  set_typename(symbol->value_type, tt, usertype);
  symbol->symbol_type = SYM_LOCAL;
  symbol->var.block = scope;
  symbol->var.var_name = name;
  add_symbol(parser->container, &scope->symbol_list, symbol);  // Add to the end of the symbol list
  add_symbol(parser->container, &scope->function->function_expr.locals, symbol);
  // Note that Lua allows multiple local declarations of the same name
  // so a new instance just gets added to the end
  return symbol;
}

/* create a new label */
static struct lua_symbol *new_label(struct parser_state *parser, TString *name) {
  struct block_scope *scope = parser->current_scope;
  assert(scope);
  struct lua_symbol *symbol = dmrC_allocator_allocate(&parser->container->symbol_allocator, 0);
  set_type(symbol->value_type, RAVI_TANY);
  symbol->symbol_type = SYM_LABEL;
  symbol->label.block = scope;
  symbol->label.label_name = name;
  add_symbol(parser->container, &scope->symbol_list,
             symbol);  // Add to the end of the symbol list
                       // Note that Lua allows multiple local declarations of the same name
                       // so a new instance just gets added to the end
  return symbol;
}

/* create a new local variable
 */
static struct lua_symbol *new_localvarliteral_(struct parser_state *parser, const char *name, size_t sz) {
  return new_local_symbol(parser, luaX_newstring(parser->ls, name, sz), RAVI_TANY, NULL);
}

/* create a new local variable
 */
#define new_localvarliteral(parser, name) new_localvarliteral_(parser, "" name, (sizeof(name) / sizeof(char)) - 1)

static struct lua_symbol *search_for_variable_in_block(struct block_scope *scope, const TString *varname) {
  struct lua_symbol *symbol;
  // Lookup in reverse order so that we discover the
  // most recently added local symbol - as Lua allows same
  // symbol to be declared local more than once in a scope
  // Should also work with nesting as the function when parsed
  // will only know about vars declared in parent function until
  // now.
  FOR_EACH_PTR_REVERSE(scope->symbol_list, symbol) {
    switch (symbol->symbol_type) {
      case SYM_LOCAL: {
        if (varname == symbol->var.var_name) {
          return symbol;
        }
        break;
      }
      default:
        break;
    }
  }
  END_FOR_EACH_PTR_REVERSE(symbol);
  return NULL;
}

// static struct lua_symbol *search_for_label_in_block(struct block_scope *scope, const TString *name) {
//  struct lua_symbol *symbol;
//  // Lookup in reverse order so that we discover the
//  // most recently added local symbol - as Lua allows same
//  // symbol to be declared local more than once in a scope
//  // Should also work with nesting as the function when parsed
//  // will only know about vars declared in parent function until
//  // now.
//  FOR_EACH_PTR_REVERSE(scope->symbol_list, symbol) {
//    switch (symbol->symbol_type) {
//      case SYM_LABEL: {
//        if (name == symbol->var.var_name) { return symbol; }
//        break;
//      }
//      default: break;
//    }
//  }
//  END_FOR_EACH_PTR_REVERSE(symbol);
//  return NULL;
//}

/* Each function has a list of upvalues, searches this list for given name
 */
static struct lua_symbol *search_upvalue_in_function(struct ast_node *function, const TString *name) {
  struct lua_symbol *symbol;
  FOR_EACH_PTR(function->function_expr.upvalues, symbol) {
    switch (symbol->symbol_type) {
      case SYM_UPVALUE: {
        assert(symbol->upvalue.var->symbol_type == SYM_LOCAL);
        if (name == symbol->upvalue.var->var.var_name) {
          return symbol;
        }
        break;
      }
      default:
        break;
    }
  }
  END_FOR_EACH_PTR(symbol);
  return NULL;
}

/* Each function has a list of upvalues, searches this list for given name, and adds it if not found.
 * Returns true if added, false means the function already has the upvalue.
 */
static bool add_upvalue_in_function(struct parser_state *parser, struct ast_node *function, struct lua_symbol *sym) {
  struct lua_symbol *symbol;
  FOR_EACH_PTR(function->function_expr.upvalues, symbol) {
    switch (symbol->symbol_type) {
      case SYM_UPVALUE: {
        assert(symbol->upvalue.var->symbol_type == SYM_LOCAL);
        if (sym == symbol->upvalue.var) {
          return false;
        }
        break;
      }
      default:
        break;
    }
  }
  END_FOR_EACH_PTR(symbol);
  struct lua_symbol *upvalue = dmrC_allocator_allocate(&parser->container->symbol_allocator, 0);
  upvalue->symbol_type = SYM_UPVALUE;
  upvalue->upvalue.var = sym;
  upvalue->upvalue.function = function;
  copy_type(upvalue->value_type, sym->value_type);
  add_symbol(parser->container, &function->function_expr.upvalues, upvalue);
  return true;
}

/* Searches for a variable starting from current scope, going up the
 * scope chain within the current function. If the variable is not found in any scope of the function, then
 * search the function's upvalue list. Repeat the exercise in parent function until either
 * the symbol is found or we exhaust the search. NULL is returned if search was
 * exhausted.
 */
static struct lua_symbol *search_for_variable(struct parser_state *parser, const TString *varname, bool *is_local) {
  *is_local = false;
  struct block_scope *current_scope = parser->current_scope;
  struct ast_node *start_function = parser->current_function;
  assert(current_scope && current_scope->function == parser->current_function);
  while (current_scope) {
    struct ast_node *current_function = current_scope->function;
    while (current_scope && current_function == current_scope->function) {
      struct lua_symbol *symbol = search_for_variable_in_block(current_scope, varname);
      if (symbol) {
        *is_local = (current_function == start_function);
        return symbol;
      }
      current_scope = current_scope->parent;
    }
    // search upvalues in the function
    struct lua_symbol *symbol = search_upvalue_in_function(current_function, varname);
    if (symbol)
      return symbol;
    // try in parent function
  }
  return NULL;
}

/* Searches for a label in current function
 */
// static struct lua_symbol *search_for_label(struct parser_state *parser, const TString *name) {
//  struct block_scope *current_scope = parser->current_scope;
//  while (current_scope && current_scope->function == parser->current_function) {
//    struct lua_symbol *symbol = search_for_label_in_block(current_scope, name);
//    if (symbol) return symbol;
//    current_scope = current_scope->parent;
//  }
//  return NULL;
//}

/* Adds an upvalue to current_function and its parents until var_function; var_function being where the symbol
 * exists as a local or an upvalue. If the symbol is found in a function's upvalue list then there is no need to
 * check parent functions.
 */
static void add_upvalue_in_levels_upto(struct parser_state *parser, struct ast_node *current_function,
                                       struct ast_node *var_function, struct lua_symbol *symbol) {
  assert(current_function != var_function);
  while (current_function && current_function != var_function) {
    bool added = add_upvalue_in_function(parser, current_function, symbol);
    if (!added)
      // this function already has it so we are done
      break;
    current_function = current_function->function_expr.parent_function;
  }
}

/* Creates a symbol reference to the name; the returned symbol reference
 * may be local, upvalue or global.
 */
static struct ast_node *new_symbol_reference(struct parser_state *parser) {
  TString *varname = check_name_and_next(parser->ls);
  bool is_local = false;
  struct lua_symbol *symbol = search_for_variable(parser, varname, &is_local);
  if (symbol) {
    // we found a local or upvalue
    if (!is_local) {
      // If the local symbol occurred in a parent function then we
      // need to construct an upvalue. Lua requires that the upvalue be
      // added to all functions in the tree up to the function where the local
      // is defined.
      add_upvalue_in_levels_upto(parser, parser->current_function, symbol->var.block->function, symbol);
      // TODO Following search could be avoided if above returned the symbol
      symbol = search_upvalue_in_function(parser->current_function, varname);
    }
    else if (symbol->symbol_type == SYM_UPVALUE && symbol->upvalue.function != parser->current_function) {
      // We found an upvalue but it is not at the same level
      // Ensure all levels have the upvalue
      add_upvalue_in_levels_upto(parser, parser->current_function, symbol->upvalue.function, symbol->upvalue.var);
      // TODO Following search could be avoided if above returned the symbol
      symbol = search_upvalue_in_function(parser->current_function, varname);
    }
  }
  else {
    // Return global symbol
    struct lua_symbol *global = dmrC_allocator_allocate(&parser->container->symbol_allocator, 0);
    global->symbol_type = SYM_GLOBAL;
    global->var.var_name = varname;
    global->var.block = NULL;
    set_type(global->value_type, RAVI_TANY);  // Globals are always ANY type
    // We don't add globals to any scope so that they are
    // always looked up
    symbol = global;
  }
  struct ast_node *symbol_expr = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  symbol_expr->type = AST_SYMBOL_EXPR;
  symbol_expr->symbol_expr.type = symbol->value_type;
  symbol_expr->symbol_expr.var = symbol;
  return symbol_expr;
}

/*============================================================*/
/* GRAMMAR RULES */
/*============================================================*/

static struct ast_node *new_string_literal(struct parser_state *parser, const TString *ts) {
  struct ast_node *node = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  node->type = AST_LITERAL_EXPR;
  set_type(node->literal_expr.type, RAVI_TSTRING);
  node->literal_expr.u.s = ts;
  return node;
}

static struct ast_node *new_field_selector(struct parser_state *parser, const TString *ts) {
  struct ast_node *index = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  index->type = AST_FIELD_SELECTOR_EXPR;
  index->index_expr.expr = new_string_literal(parser, ts);
  set_type(index->index_expr.type, RAVI_TANY);
  return index;
}

/*
 * Parse ['.' | ':'] NAME
 */
static struct ast_node *parse_field_selector(struct parser_state *parser) {
  LexState *ls = parser->ls;
  /* fieldsel -> ['.' | ':'] NAME */
  luaX_next(ls); /* skip the dot or colon */
  TString *ts = check_name_and_next(ls);
  return new_field_selector(parser, ts);
}

/*
 * Parse '[' expr ']
 */
static struct ast_node *parse_yindex(struct parser_state *parser) {
  LexState *ls = parser->ls;
  /* index -> '[' expr ']' */
  luaX_next(ls); /* skip the '[' */
  struct ast_node *expr = parse_expression(parser);
  checknext(ls, ']');

  struct ast_node *index = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  index->type = AST_Y_INDEX_EXPR;
  index->index_expr.expr = expr;
  set_type(index->index_expr.type, RAVI_TANY);
  return index;
}

/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/

static struct ast_node *new_indexed_assign_expr(struct parser_state *parser, struct ast_node *index_expr,
                                                struct ast_node *value_expr) {
  struct ast_node *set = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  set->type = AST_INDEXED_ASSIGN_EXPR;
  set->indexed_assign_expr.index_expr = index_expr;
  set->indexed_assign_expr.value_expr = value_expr;
  set->indexed_assign_expr.type = value_expr->common_expr.type; /* type of indexed assignment is same as the value*/
  return set;
}

static struct ast_node *parse_recfield(struct parser_state *parser) {
  LexState *ls = parser->ls;
  /* recfield -> (NAME | '['exp1']') = exp1 */
  struct ast_node *index_expr;
  if (ls->t.token == TK_NAME) {
    const TString *ts = check_name_and_next(ls);
    index_expr = new_field_selector(parser, ts);
  }
  else /* ls->t.token == '[' */
    index_expr = parse_yindex(parser);
  checknext(ls, '=');
  struct ast_node *value_expr = parse_expression(parser);
  return new_indexed_assign_expr(parser, index_expr, value_expr);
}

static struct ast_node *parse_listfield(struct parser_state *parser) {
  /* listfield -> exp */
  struct ast_node *value_expr = parse_expression(parser);
  return new_indexed_assign_expr(parser, NULL, value_expr);
}

static struct ast_node *parse_field(struct parser_state *parser) {
  LexState *ls = parser->ls;
  /* field -> listfield | recfield */
  switch (ls->t.token) {
    case TK_NAME: {                  /* may be 'listfield' or 'recfield' */
      if (luaX_lookahead(ls) != '=') /* expression? */
        return parse_listfield(parser);
      else
        return parse_recfield(parser);
      break;
    }
    case '[': {
      return parse_recfield(parser);
      break;
    }
    default: {
      return parse_listfield(parser);
      break;
    }
  }
  return NULL;
}

static struct ast_node *parse_table_constructor(struct parser_state *parser) {
  LexState *ls = parser->ls;
  /* constructor -> '{' [ field { sep field } [sep] ] '}'
  sep -> ',' | ';' */
  int line = ls->linenumber;
  checknext(ls, '{');
  struct ast_node *table_expr = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  set_type(table_expr->table_expr.type, RAVI_TTABLE);
  table_expr->table_expr.expr_list = NULL;
  table_expr->type = AST_TABLE_EXPR;
  do {
    if (ls->t.token == '}')
      break;
    struct ast_node *field_expr = parse_field(parser);
    add_ast_node(parser->container, &table_expr->table_expr.expr_list, field_expr);
  } while (testnext(ls, ',') || testnext(ls, ';'));
  check_match(ls, '}', '{', line);
  return table_expr;
}

/* }====================================================================== */

/*
 * We would like to allow user defined types to contain the sequence
 * NAME [. NAME]+
 * The initial NAME is supplied.
 * Returns extended name.
 * Note that the returned string will be anchored in the Lexer and must
 * be anchored somewhere else by the time parsing finishes
 */
static TString *user_defined_type_name(LexState *ls, TString *typename) {
  size_t len = 0;
  if (testnext(ls, '.')) {
    char buffer[128] = {0};
    const char *str = getstr(typename);
    len = strlen(str);
    if (len >= sizeof buffer) {
      luaX_syntaxerror(ls, "User defined type name is too long");
      return typename;
    }
    snprintf(buffer, sizeof buffer, "%s", str);
    do {
      typename = str_checkname(ls);
      str = getstr(typename);
      size_t newlen = len + strlen(str) + 1;
      if (newlen >= sizeof buffer) {
        luaX_syntaxerror(ls, "User defined type name is too long");
        return typename;
      }
      snprintf(buffer + len, sizeof buffer - len, ".%s", str);
      len = newlen;
    } while (testnext(ls, '.'));
    typename = luaX_newstring(ls, buffer, strlen(buffer));
  }
  return typename;
}

/* RAVI Parse
 *   name : type
 *   where type is 'integer', 'integer[]',
 *                 'number', 'number[]'
 */
static struct lua_symbol *declare_local_variable(struct parser_state *parser) {
  LexState *ls = parser->ls;
  /* assume a dynamic type */
  ravitype_t tt = RAVI_TANY;
  TString *name = check_name_and_next(ls);
  TString *pusertype = NULL;
  if (testnext(ls, ':')) {
    TString *typename = str_checkname(ls); /* we expect a type name */
    const char *str = getstr(typename);
    /* following is not very nice but easy as
     * the lexer doesn't need to be changed
     */
    if (strcmp(str, "integer") == 0)
      tt = RAVI_TNUMINT;
    else if (strcmp(str, "number") == 0)
      tt = RAVI_TNUMFLT;
    else if (strcmp(str, "closure") == 0)
      tt = RAVI_TFUNCTION;
    else if (strcmp(str, "table") == 0)
      tt = RAVI_TTABLE;
    else if (strcmp(str, "string") == 0)
      tt = RAVI_TSTRING;
    else if (strcmp(str, "boolean") == 0)
      tt = RAVI_TBOOLEAN;
    else if (strcmp(str, "any") == 0)
      tt = RAVI_TANY;
    else {
      /* default is a userdata type */
      tt = RAVI_TUSERDATA;
      typename = user_defined_type_name(ls, typename);
      // str = getstr(typename);
      pusertype = typename;
    }
    if (tt == RAVI_TNUMFLT || tt == RAVI_TNUMINT) {
      /* if we see [] then it is an array type */
      if (testnext(ls, '[')) {
        checknext(ls, ']');
        tt = (tt == RAVI_TNUMFLT) ? RAVI_TARRAYFLT : RAVI_TARRAYINT;
      }
    }
  }
  return new_local_symbol(parser, name, tt, pusertype);
}

static bool parse_parameter_list(struct parser_state *parser, struct lua_symbol_list **list) {
  LexState *ls = parser->ls;
  /* parlist -> [ param { ',' param } ] */
  int nparams = 0;
  bool is_vararg = false;
  if (ls->t.token != ')') { /* is 'parlist' not empty? */
    do {
      switch (ls->t.token) {
        case TK_NAME: { /* param -> NAME */
                        /* RAVI change - add type */
          struct lua_symbol *symbol = declare_local_variable(parser);
          add_symbol(parser->container, list, symbol);
          nparams++;
          break;
        }
        case TK_DOTS: { /* param -> '...' */
          luaX_next(ls);
          is_vararg = true; /* declared vararg */
          break;
        }
        default:
          luaX_syntaxerror(ls, "<name> or '...' expected");
      }
    } while (!is_vararg && testnext(ls, ','));
  }
  return is_vararg;
}

static void parse_function_body(struct parser_state *parser, struct ast_node *func_ast, int ismethod, int line) {
  LexState *ls = parser->ls;
  /* body ->  '(' parlist ')' block END */
  checknext(ls, '(');
  if (ismethod) {
    struct lua_symbol *symbol = new_localvarliteral(parser, "self"); /* create 'self' parameter */
    add_symbol(parser->container, &func_ast->function_expr.args, symbol);
  }
  bool is_vararg = parse_parameter_list(parser, &func_ast->function_expr.args);
  func_ast->function_expr.is_vararg = is_vararg;
  func_ast->function_expr.is_method = ismethod;
  checknext(ls, ')');
  parse_statement_list(parser, &func_ast->function_expr.function_statement_list);
  check_match(ls, TK_END, TK_FUNCTION, line);
}

/* parse expression list */
static int parse_expression_list(struct parser_state *parser, struct ast_node_list **list) {
  LexState *ls = parser->ls;
  /* explist -> expr { ',' expr } */
  int n = 1; /* at least one expression */
  struct ast_node *expr = parse_expression(parser);
  add_ast_node(parser->container, list, expr);
  while (testnext(ls, ',')) {
    expr = parse_expression(parser);
    add_ast_node(parser->container, list, expr);
    n++;
  }
  return n;
}

/* parse function arguments */
static struct ast_node *parse_function_call(struct parser_state *parser, TString *methodname, int line) {
  LexState *ls = parser->ls;
  struct ast_node *call_expr = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  call_expr->type = AST_FUNCTION_CALL_EXPR;
  call_expr->function_call_expr.method_name = methodname;
  call_expr->function_call_expr.arg_list = NULL;
  set_type(call_expr->function_call_expr.type, RAVI_TANY);
  switch (ls->t.token) {
    case '(': { /* funcargs -> '(' [ explist ] ')' */
      luaX_next(ls);
      if (ls->t.token == ')') /* arg list is empty? */
        ;
      else {
        parse_expression_list(parser, &call_expr->function_call_expr.arg_list);
      }
      check_match(ls, ')', '(', line);
      break;
    }
    case '{': { /* funcargs -> constructor */
      struct ast_node *table_expr = parse_table_constructor(parser);
      add_ast_node(parser->container, &call_expr->function_call_expr.arg_list, table_expr);
      break;
    }
    case TK_STRING: { /* funcargs -> STRING */
      struct ast_node *string_expr = new_literal_expression(parser, RAVI_TSTRING);
      string_expr->literal_expr.u.s = ls->t.seminfo.ts;
      add_ast_node(parser->container, &call_expr->function_call_expr.arg_list, string_expr);
      luaX_next(ls);
      break;
    }
    default: {
      luaX_syntaxerror(ls, "function arguments expected");
    }
  }
  return call_expr;
}

/*
** {======================================================================
** Expression parsing
** =======================================================================
*/

/* primary expression - name or subexpression */
static struct ast_node *parse_primary_expression(struct parser_state *parser) {
  LexState *ls = parser->ls;
  struct ast_node *primary_expr = NULL;
  /* primaryexp -> NAME | '(' expr ')' */
  switch (ls->t.token) {
    case '(': {
      int line = ls->linenumber;
      luaX_next(ls);
      primary_expr = parse_expression(parser);
      check_match(ls, ')', '(', line);
      break;
    }
    case TK_NAME: {
      primary_expr = new_symbol_reference(parser);
      break;
    }
    default: {
      luaX_syntaxerror(ls, "unexpected symbol");
    }
  }
  assert(primary_expr);
  return primary_expr;
}

/* variable or field access or function call */
static struct ast_node *parse_suffixed_expression(struct parser_state *parser) {
  LexState *ls = parser->ls;
  /* suffixedexp ->
  primaryexp { '.' NAME | '[' exp ']' | ':' NAME funcargs | funcargs } */
  int line = ls->linenumber;
  struct ast_node *suffixed_expr = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  suffixed_expr->suffixed_expr.primary_expr = parse_primary_expression(parser);
  suffixed_expr->type = AST_SUFFIXED_EXPR;
  suffixed_expr->suffixed_expr.type = suffixed_expr->suffixed_expr.primary_expr->common_expr.type;
  suffixed_expr->suffixed_expr.suffix_list = NULL;
  for (;;) {
    switch (ls->t.token) {
      case '.': { /* fieldsel */
        struct ast_node *suffix = parse_field_selector(parser);
        add_ast_node(parser->container, &suffixed_expr->suffixed_expr.suffix_list, suffix);
        set_type(suffixed_expr->suffixed_expr.type, RAVI_TANY);
        break;
      }
      case '[': { /* '[' exp1 ']' */
        struct ast_node *suffix = parse_yindex(parser);
        add_ast_node(parser->container, &suffixed_expr->suffixed_expr.suffix_list, suffix);
        set_type(suffixed_expr->suffixed_expr.type, RAVI_TANY);
        break;
      }
      case ':': { /* ':' NAME funcargs */
        luaX_next(ls);
        TString *methodname = check_name_and_next(ls);
        struct ast_node *suffix = parse_function_call(parser, methodname, line);
        add_ast_node(parser->container, &suffixed_expr->suffixed_expr.suffix_list, suffix);
        break;
      }
      case '(':
      case TK_STRING:
      case '{': { /* funcargs */
        struct ast_node *suffix = parse_function_call(parser, NULL, line);
        add_ast_node(parser->container, &suffixed_expr->suffixed_expr.suffix_list, suffix);
        break;
      }
      default:
        return suffixed_expr;
    }
  }
}

static struct ast_node *new_literal_expression(struct parser_state *parser, ravitype_t type) {
  struct ast_node *expr = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  expr->type = AST_LITERAL_EXPR;
  set_type(expr->literal_expr.type, type);
  expr->literal_expr.u.i = 0; /* initialize */
  return expr;
}

static struct ast_node *parse_simple_expression(struct parser_state *parser) {
  LexState *ls = parser->ls;
  /* simpleexp -> FLT | INT | STRING | NIL | TRUE | FALSE | ... |
  constructor | FUNCTION body | suffixedexp */
  struct ast_node *expr = NULL;
  switch (ls->t.token) {
    case TK_FLT: {
      expr = new_literal_expression(parser, RAVI_TNUMFLT);
      expr->literal_expr.u.n = ls->t.seminfo.r;
      break;
    }
    case TK_INT: {
      expr = new_literal_expression(parser, RAVI_TNUMINT);
      expr->literal_expr.u.i = ls->t.seminfo.i;
      break;
    }
    case TK_STRING: {
      expr = new_literal_expression(parser, RAVI_TSTRING);
      expr->literal_expr.u.s = ls->t.seminfo.ts;
      break;
    }
    case TK_NIL: {
      expr = new_literal_expression(parser, RAVI_TNIL);
      expr->literal_expr.u.i = -1;
      break;
    }
    case TK_TRUE: {
      expr = new_literal_expression(parser, RAVI_TBOOLEAN);
      expr->literal_expr.u.i = 1;
      break;
    }
    case TK_FALSE: {
      expr = new_literal_expression(parser, RAVI_TBOOLEAN);
      expr->literal_expr.u.i = 0;
      break;
    }
    case TK_DOTS: { /* vararg */
      // Not handled yet
      expr = NULL;
      break;
    }
    case '{': { /* constructor */
      return parse_table_constructor(parser);
    }
    case TK_FUNCTION: {
      luaX_next(ls);
      struct ast_node *function_ast = new_function(parser);
      parse_function_body(parser, function_ast, 0, ls->linenumber);
      end_function(parser);
      return function_ast;
    }
    default: {
      return parse_suffixed_expression(parser);
    }
  }
  luaX_next(ls);
  return expr;
}

static UnOpr get_unary_opr(int op) {
  switch (op) {
    case TK_NOT:
      return OPR_NOT;
    case '-':
      return OPR_MINUS;
    case '~':
      return OPR_BNOT;
    case '#':
      return OPR_LEN;
    case TK_TO_INTEGER:
      return OPR_TO_INTEGER;
    case TK_TO_NUMBER:
      return OPR_TO_NUMBER;
    case TK_TO_INTARRAY:
      return OPR_TO_INTARRAY;
    case TK_TO_NUMARRAY:
      return OPR_TO_NUMARRAY;
    case TK_TO_TABLE:
      return OPR_TO_TABLE;
    case TK_TO_STRING:
      return OPR_TO_STRING;
    case TK_TO_CLOSURE:
      return OPR_TO_CLOSURE;
    case '@':
      return OPR_TO_TYPE;
    default:
      return OPR_NOUNOPR;
  }
}

static BinOpr get_binary_opr(int op) {
  switch (op) {
    case '+':
      return OPR_ADD;
    case '-':
      return OPR_SUB;
    case '*':
      return OPR_MUL;
    case '%':
      return OPR_MOD;
    case '^':
      return OPR_POW;
    case '/':
      return OPR_DIV;
    case TK_IDIV:
      return OPR_IDIV;
    case '&':
      return OPR_BAND;
    case '|':
      return OPR_BOR;
    case '~':
      return OPR_BXOR;
    case TK_SHL:
      return OPR_SHL;
    case TK_SHR:
      return OPR_SHR;
    case TK_CONCAT:
      return OPR_CONCAT;
    case TK_NE:
      return OPR_NE;
    case TK_EQ:
      return OPR_EQ;
    case '<':
      return OPR_LT;
    case TK_LE:
      return OPR_LE;
    case '>':
      return OPR_GT;
    case TK_GE:
      return OPR_GE;
    case TK_AND:
      return OPR_AND;
    case TK_OR:
      return OPR_OR;
    default:
      return OPR_NOBINOPR;
  }
}

static const struct {
  lu_byte left;  /* left priority for each binary operator */
  lu_byte right; /* right priority */
} priority[] = {
    /* ORDER OPR */
    {10, 10}, {10, 10},         /* '+' '-' */
    {11, 11}, {11, 11},         /* '*' '%' */
    {14, 13},                   /* '^' (right associative) */
    {11, 11}, {11, 11},         /* '/' '//' */
    {6, 6},   {4, 4},   {5, 5}, /* '&' '|' '~' */
    {7, 7},   {7, 7},           /* '<<' '>>' */
    {9, 8},                     /* '..' (right associative) */
    {3, 3},   {3, 3},   {3, 3}, /* ==, <, <= */
    {3, 3},   {3, 3},   {3, 3}, /* ~=, >, >= */
    {2, 2},   {1, 1}            /* and, or */
};

#define UNARY_PRIORITY 12 /* priority for unary operators */

/*
** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
** where 'binop' is any binary operator with a priority higher than 'limit'
*/
static struct ast_node *parse_sub_expression(struct parser_state *parser, int limit, BinOpr *untreated_op) {
  LexState *ls = parser->ls;
  BinOpr op;
  UnOpr uop;
  struct ast_node *expr = NULL;
  uop = get_unary_opr(ls->t.token);
  if (uop != OPR_NOUNOPR) {
    // RAVI change - get usertype if @<name>
    TString *usertype = NULL;
    if (uop == OPR_TO_TYPE) {
      usertype = ls->t.seminfo.ts;
      luaX_next(ls);
      // Check and expand to extended name if necessary
      usertype = user_defined_type_name(ls, usertype);
    }
    else {
      luaX_next(ls);
    }
    BinOpr ignored;
    struct ast_node *subexpr = parse_sub_expression(parser, UNARY_PRIORITY, &ignored);
    expr = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
    expr->type = AST_UNARY_EXPR;
    expr->unary_expr.expr = subexpr;
    expr->unary_expr.unary_op = uop;
    expr->unary_expr.type.type_name = usertype;
  }
  else {
    expr = parse_simple_expression(parser);
  }
  /* expand while operators have priorities higher than 'limit' */
  op = get_binary_opr(ls->t.token);
  while (op != OPR_NOBINOPR && priority[op].left > limit) {
    BinOpr nextop;
    luaX_next(ls);
    /* read sub-expression with higher priority */
    struct ast_node *exprright = parse_sub_expression(parser, priority[op].right, &nextop);

    struct ast_node *binexpr = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
    binexpr->type = AST_BINARY_EXPR;
    binexpr->binary_expr.expr_left = expr;
    binexpr->binary_expr.expr_right = exprright;
    binexpr->binary_expr.binary_op = op;
    expr = binexpr;  // Becomes the left expr for next iteration
    op = nextop;
  }
  *untreated_op = op; /* return first untreated operator */
  return expr;
}

static struct ast_node *parse_expression(struct parser_state *parser) {
  BinOpr ignored;
  return parse_sub_expression(parser, 0, &ignored);
}

/* }==================================================================== */

/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/

static struct block_scope *parse_block(struct parser_state *parser, struct ast_node_list **statement_list) {
  /* block -> statlist */
  struct block_scope *scope = new_scope(parser);
  parse_statement_list(parser, statement_list);
  end_scope(parser);
  return scope;
}

/* parse condition in a repeat statement or an if control structure
 * called by repeatstat(), test_then_block()
 */
static struct ast_node *parse_condition(struct parser_state *parser) {
  /* cond -> exp */
  return parse_expression(parser); /* read condition */
}

static struct ast_node *parse_goto_statment(struct parser_state *parser) {
  LexState *ls = parser->ls;
  TString *label;
  if (testnext(ls, TK_GOTO))
    label = check_name_and_next(ls);
  else {
    luaX_next(ls); /* skip break */
    label = luaX_newstring(ls, "break", sizeof "break");
  }
  // Resolve labels in the end?
  struct ast_node *goto_stmt = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  goto_stmt->type = AST_GOTO_STMT;
  goto_stmt->goto_stmt.name = label;
  goto_stmt->goto_stmt.label_stmt = NULL;  // unresolved
  return goto_stmt;
}

/* skip no-op statements */
static void skip_noop_statements(struct parser_state *parser) {
  LexState *ls = parser->ls;
  while (ls->t.token == ';')  //  || ls->t.token == TK_DBCOLON)
    parse_statement(parser);
}

static struct ast_node *generate_label(struct parser_state *parser, TString *label) {
  struct lua_symbol *symbol = new_label(parser, label);
  struct ast_node *label_stmt = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  label_stmt->type = AST_LABEL_STMT;
  label_stmt->label_stmt.symbol = symbol;
  return label_stmt;
}

static struct ast_node *parse_label_statement(struct parser_state *parser, TString *label, int line) {
  (void)line;
  LexState *ls = parser->ls;
  /* label -> '::' NAME '::' */
  checknext(ls, TK_DBCOLON); /* skip double colon */
  /* create new entry for this label */
  struct ast_node *label_stmt = generate_label(parser, label);
  skip_noop_statements(parser); /* skip other no-op statements */
  return label_stmt;
}

static struct ast_node *parse_while_statement(struct parser_state *parser, int line) {
  LexState *ls = parser->ls;
  /* whilestat -> WHILE cond DO block END */
  luaX_next(ls); /* skip WHILE */
  struct ast_node *stmt = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  stmt->type = AST_WHILE_STMT;
  stmt->while_or_repeat_stmt.loop_scope = NULL;
  stmt->while_or_repeat_stmt.loop_statement_list = NULL;
  stmt->while_or_repeat_stmt.condition = parse_condition(parser);
  checknext(ls, TK_DO);
  stmt->while_or_repeat_stmt.loop_scope = parse_block(parser, &stmt->while_or_repeat_stmt.loop_statement_list);
  check_match(ls, TK_END, TK_WHILE, line);
  return stmt;
}

static struct ast_node *parse_repeat_statement(struct parser_state *parser, int line) {
  LexState *ls = parser->ls;
  /* repeatstat -> REPEAT block UNTIL cond */
  luaX_next(ls); /* skip REPEAT */
  struct ast_node *stmt = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  stmt->type = AST_REPEAT_STMT;
  stmt->while_or_repeat_stmt.condition = NULL;
  stmt->while_or_repeat_stmt.loop_statement_list = NULL;
  stmt->while_or_repeat_stmt.loop_scope = new_scope(parser); /* scope block */
  parse_statement_list(parser, &stmt->while_or_repeat_stmt.loop_statement_list);
  check_match(ls, TK_UNTIL, TK_REPEAT, line);
  stmt->while_or_repeat_stmt.condition = parse_condition(parser); /* read condition (inside scope block) */
  end_scope(parser);
  return stmt;
}

/* parse a for loop body for both versions of the for loop */
static void parse_forbody(struct parser_state *parser, struct ast_node *stmt, int line, int nvars, int isnum) {
  (void)line;
  (void)nvars;
  (void)isnum;
  LexState *ls = parser->ls;
  /* forbody -> DO block */
  checknext(ls, TK_DO);
  stmt->for_stmt.for_body = parse_block(parser, &stmt->for_stmt.for_statement_list);
}

/* parse a numerical for loop */
static void parse_fornum_statement(struct parser_state *parser, struct ast_node *stmt, TString *varname, int line) {
  LexState *ls = parser->ls;
  /* fornum -> NAME = exp1,exp1[,exp1] forbody */
  add_symbol(parser->container, &stmt->for_stmt.symbols, new_local_symbol(parser, varname, RAVI_TANY, NULL));
  checknext(ls, '=');
  /* get the type of each expression */
  add_ast_node(parser->container, &stmt->for_stmt.expr_list, parse_expression(parser)); /* initial value */
  checknext(ls, ',');
  add_ast_node(parser->container, &stmt->for_stmt.expr_list, parse_expression(parser)); /* limit */
  if (testnext(ls, ',')) {
    add_ast_node(parser->container, &stmt->for_stmt.expr_list, parse_expression(parser)); /* optional step */
  }
  parse_forbody(parser, stmt, line, 1, 1);
}

/* parse a generic for loop */
static void parse_for_list(struct parser_state *parser, struct ast_node *stmt, TString *indexname) {
  LexState *ls = parser->ls;
  /* forlist -> NAME {,NAME} IN explist forbody */
  int nvars = 4; /* gen, state, control, plus at least one declared var */
  /* create declared variables */
  add_symbol(parser->container, &stmt->for_stmt.symbols, new_local_symbol(parser, indexname, RAVI_TANY, NULL));
  while (testnext(ls, ',')) {
    add_symbol(parser->container, &stmt->for_stmt.symbols,
               new_local_symbol(parser, check_name_and_next(ls), RAVI_TANY, NULL));
    nvars++;
  }
  checknext(ls, TK_IN);
  parse_expression_list(parser, &stmt->for_stmt.expr_list);
  int line = ls->linenumber;
  parse_forbody(parser, stmt, line, nvars - 3, 0);
}

/* initial parsing of a for loop - calls fornum() or forlist() */
static struct ast_node *parse_for_statement(struct parser_state *parser, int line) {
  LexState *ls = parser->ls;
  /* forstat -> FOR (fornum | forlist) END */
  TString *varname;
  struct ast_node *stmt = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  stmt->type = AST_NONE;
  stmt->for_stmt.symbols = NULL;
  stmt->for_stmt.expr_list = NULL;
  stmt->for_stmt.for_body = NULL;
  stmt->for_stmt.for_statement_list = NULL;
  new_scope(parser);                 // For the loop variables
  luaX_next(ls);                     /* skip 'for' */
  varname = check_name_and_next(ls); /* first variable name */
  switch (ls->t.token) {
    case '=':
      stmt->type = AST_FORNUM_STMT;
      parse_fornum_statement(parser, stmt, varname, line);
      break;
    case ',':
    case TK_IN:
      stmt->type = AST_FORIN_STMT;
      parse_for_list(parser, stmt, varname);
      break;
    default:
      luaX_syntaxerror(ls, "'=' or 'in' expected");
  }
  check_match(ls, TK_END, TK_FOR, line);
  end_scope(parser);
  return stmt;
}

/* parse if cond then block - called from parse_if_statement() */
static struct ast_node *parse_if_cond_then_block(struct parser_state *parser) {
  LexState *ls = parser->ls;
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  luaX_next(ls); /* skip IF or ELSEIF */
  struct ast_node *test_then_block = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  test_then_block->type = AST_NONE;                                      // This is not an AST node on its own
  test_then_block->test_then_block.condition = parse_expression(parser); /* read condition */
  test_then_block->test_then_block.test_then_scope = NULL;
  test_then_block->test_then_block.test_then_statement_list = NULL;
  checknext(ls, TK_THEN);
  if (ls->t.token == TK_GOTO || ls->t.token == TK_BREAK) {
    test_then_block->test_then_block.test_then_scope = new_scope(parser);
    struct ast_node *stmt = parse_goto_statment(parser); /* handle goto/break */
    add_ast_node(parser->container, &test_then_block->test_then_block.test_then_statement_list, stmt);
    skip_noop_statements(parser); /* skip other no-op statements */
    if (block_follow(ls, 0)) {    /* 'goto' is the entire block? */
      end_scope(parser);
      return test_then_block; /* and that is it */
    }
    else { /* must skip over 'then' part if condition is false */
      ;
    }
  }
  else { /* regular case (not goto/break) */
    test_then_block->test_then_block.test_then_scope = new_scope(parser);
  }
  parse_statement_list(parser, &test_then_block->test_then_block.test_then_statement_list); /* 'then' part */
  end_scope(parser);
  return test_then_block;
}

static struct ast_node *parse_if_statement(struct parser_state *parser, int line) {
  LexState *ls = parser->ls;
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  struct ast_node *stmt = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  stmt->type = AST_IF_STMT;
  stmt->if_stmt.if_condition_list = NULL;
  stmt->if_stmt.else_block = NULL;
  stmt->if_stmt.else_statement_list = NULL;
  struct ast_node *test_then_block = parse_if_cond_then_block(parser); /* IF cond THEN block */
  add_ast_node(parser->container, &stmt->if_stmt.if_condition_list, test_then_block);
  while (ls->t.token == TK_ELSEIF) {
    test_then_block = parse_if_cond_then_block(parser); /* ELSEIF cond THEN block */
    add_ast_node(parser->container, &stmt->if_stmt.if_condition_list, test_then_block);
  }
  if (testnext(ls, TK_ELSE))
    stmt->if_stmt.else_block = parse_block(parser, &stmt->if_stmt.else_statement_list); /* 'else' part */
  check_match(ls, TK_END, TK_IF, line);
  return stmt;
}

static struct ast_node *parse_local_function_statement(struct parser_state *parser) {
  LexState *ls = parser->ls;
  struct lua_symbol *symbol =
      new_local_symbol(parser, check_name_and_next(ls), RAVI_TFUNCTION, NULL); /* new local variable */
  struct ast_node *function_ast = new_function(parser);
  parse_function_body(parser, function_ast, 0, ls->linenumber); /* function created in next register */
  end_function(parser);
  struct ast_node *stmt = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  stmt->type = AST_LOCAL_STMT;
  stmt->local_stmt.var_list = NULL;
  stmt->local_stmt.expr_list = NULL;
  add_symbol(parser->container, &stmt->local_stmt.var_list, symbol);
  add_ast_node(parser->container, &stmt->local_stmt.expr_list, function_ast);
  return stmt;
}

static struct ast_node *parse_local_statement(struct parser_state *parser) {
  LexState *ls = parser->ls;
  /* stat -> LOCAL NAME {',' NAME} ['=' explist] */
  struct ast_node *node = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  node->type = AST_LOCAL_STMT;
  node->local_stmt.var_list = NULL;
  node->local_stmt.expr_list = NULL;
  int nvars = 0;
  do {
    /* local name : type = value */
    struct lua_symbol *symbol = declare_local_variable(parser);
    add_symbol(parser->container, &node->local_stmt.var_list, symbol);
    nvars++;
    if (nvars >= MAXVARS)
      luaX_syntaxerror(ls, "too many local variables");
  } while (testnext(ls, ','));
  if (testnext(ls, '=')) /* nexps = */
    parse_expression_list(parser, &node->local_stmt.expr_list);
  else {
    /* nexps = 0; */
    ;
  }
  return node;
}

/* parse a function name specification with base symbol, optional selectors and optional method name
 */
static struct ast_node *parse_function_name(struct parser_state *parser) {
  LexState *ls = parser->ls;
  /* funcname -> NAME {fieldsel} [':' NAME] */
  struct ast_node *function_stmt = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  function_stmt->type = AST_FUNCTION_STMT;
  function_stmt->function_stmt.function_expr = NULL;
  function_stmt->function_stmt.method_name = NULL;
  function_stmt->function_stmt.selectors = NULL;
  function_stmt->function_stmt.name = new_symbol_reference(parser);
  while (ls->t.token == '.') {
    add_ast_node(parser->container, &function_stmt->function_stmt.selectors, parse_field_selector(parser));
  }
  if (ls->t.token == ':') {
    function_stmt->function_stmt.method_name = parse_field_selector(parser);
  }
  return function_stmt;
}

static struct ast_node *parse_function_statement(struct parser_state *parser, int line) {
  LexState *ls = parser->ls;
  /* funcstat -> FUNCTION funcname body */
  luaX_next(ls); /* skip FUNCTION */
  struct ast_node *function_stmt = parse_function_name(parser);
  int ismethod = function_stmt->function_stmt.method_name != NULL;
  struct ast_node *function_ast = new_function(parser);
  parse_function_body(parser, function_ast, ismethod, line);
  end_function(parser);
  function_stmt->function_stmt.function_expr = function_ast;
  return function_stmt;
}

/* parse function call with no returns or assignment statement */
static struct ast_node *parse_expression_statement(struct parser_state *parser) {
  struct ast_node *stmt = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  stmt->type = AST_EXPR_STMT;
  stmt->expression_stmt.var_expr_list = NULL;
  stmt->expression_stmt.expr_list = NULL;
  LexState *ls = parser->ls;
  /* stat -> func | assignment */
  /* Until we see '=' we do not know if this is an assignment or expr list*/
  struct ast_node_list *current_list = NULL;
  add_ast_node(parser->container, &current_list, parse_suffixed_expression(parser));
  while (testnext(ls, ',')) { /* assignment -> ',' suffixedexp assignment */
    add_ast_node(parser->container, &current_list, parse_suffixed_expression(parser));
  }
  if (ls->t.token == '=') { /* stat -> assignment ? */
    checknext(ls, '=');
    stmt->expression_stmt.var_expr_list = current_list;
    current_list = NULL;
    parse_expression_list(parser, &current_list);
  }
  stmt->expression_stmt.expr_list = current_list;
  // TODO Check that if not assignment then it is a function call
  return stmt;
}

static struct ast_node *parse_return_statement(struct parser_state *parser) {
  LexState *ls = parser->ls;
  /* stat -> RETURN [explist] [';'] */
  struct ast_node *return_stmt = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  return_stmt->type = AST_RETURN_STMT;
  return_stmt->return_stmt.expr_list = NULL;
  if (block_follow(ls, 1) || ls->t.token == ';')
    /* nret = 0*/; /* return no values */
  else {
    /*nret = */
    parse_expression_list(parser, &return_stmt->return_stmt.expr_list); /* optional return values */
  }
  testnext(ls, ';'); /* skip optional semicolon */
  return return_stmt;
}

static struct ast_node *parse_do_statement(struct parser_state *parser, int line) {
  luaX_next(parser->ls); /* skip DO */
  struct ast_node *stmt = dmrC_allocator_allocate(&parser->container->ast_node_allocator, 0);
  stmt->type = AST_DO_STMT;
  stmt->do_stmt.do_statement_list = NULL;
  stmt->do_stmt.scope = parse_block(parser, &stmt->do_stmt.do_statement_list);
  check_match(parser->ls, TK_END, TK_DO, line);
  return stmt;
}

/* parse a statement */
static struct ast_node *parse_statement(struct parser_state *parser) {
  LexState *ls = parser->ls;
  int line = ls->linenumber; /* may be needed for error messages */
  struct ast_node *stmt = NULL;
  switch (ls->t.token) {
    case ';': {      /* stat -> ';' (empty statement) */
      luaX_next(ls); /* skip ';' */
      break;
    }
    case TK_IF: { /* stat -> ifstat */
      stmt = parse_if_statement(parser, line);
      break;
    }
    case TK_WHILE: { /* stat -> whilestat */
      stmt = parse_while_statement(parser, line);
      break;
    }
    case TK_DO: { /* stat -> DO block END */
      stmt = parse_do_statement(parser, line);
      break;
    }
    case TK_FOR: { /* stat -> forstat */
      stmt = parse_for_statement(parser, line);
      break;
    }
    case TK_REPEAT: { /* stat -> repeatstat */
      stmt = parse_repeat_statement(parser, line);
      break;
    }
    case TK_FUNCTION: { /* stat -> funcstat */
      stmt = parse_function_statement(parser, line);
      break;
    }
    case TK_LOCAL: {                 /* stat -> localstat */
      luaX_next(ls);                 /* skip LOCAL */
      if (testnext(ls, TK_FUNCTION)) /* local function? */
        stmt = parse_local_function_statement(parser);
      else
        stmt = parse_local_statement(parser);
      break;
    }
    case TK_DBCOLON: { /* stat -> label */
      luaX_next(ls);   /* skip double colon */
      stmt = parse_label_statement(parser, check_name_and_next(ls), line);
      break;
    }
    case TK_RETURN: { /* stat -> retstat */
      luaX_next(ls);  /* skip RETURN */
      stmt = parse_return_statement(parser);
      break;
    }
    case TK_BREAK:  /* stat -> breakstat */
    case TK_GOTO: { /* stat -> 'goto' NAME */
      stmt = parse_goto_statment(parser);
      break;
    }
    default: { /* stat -> func | assignment */
      stmt = parse_expression_statement(parser);
      break;
    }
  }
  return stmt;
}

/* Parses a sequence of statements */
/* statlist -> { stat [';'] } */
static void parse_statement_list(struct parser_state *parser, struct ast_node_list **list) {
  LexState *ls = parser->ls;
  while (!block_follow(ls, 1)) {
    bool was_return = ls->t.token == TK_RETURN;
    struct ast_node *stmt = parse_statement(parser);
    if (stmt)
      add_ast_node(parser->container, list, stmt);
    if (was_return)
      break; /* 'return' must be last statement */
  }
}

/* Starts a new scope. If the current function has no main block
 * defined then the new scope becomes its main block. The new scope
 * gets existing scope as its parent even if that belongs to parent
 * function.
 */
static struct block_scope *new_scope(struct parser_state *parser) {
  struct ast_container *container = parser->container;
  struct block_scope *scope = dmrC_allocator_allocate(&container->block_scope_allocator, 0);
  scope->symbol_list = NULL;
  // scope->do_statement_list = NULL;
  scope->function = parser->current_function;
  assert(scope->function && scope->function->type == AST_FUNCTION_EXPR);
  scope->parent = parser->current_scope;
  parser->current_scope = scope;
  if (!parser->current_function->function_expr.main_block)
    parser->current_function->function_expr.main_block = scope;
  return scope;
}

static void end_scope(struct parser_state *parser) {
  assert(parser->current_scope);
  struct block_scope *scope = parser->current_scope;
  parser->current_scope = scope->parent;
  assert(parser->current_scope != NULL || scope == parser->current_function->function_expr.main_block);
}

/* Creates a new function AST node and starts the function scope.
New function becomes child of current function if any, and scope is linked
to previous scope which may be of parent function.
*/
static struct ast_node *new_function(struct parser_state *parser) {
  struct ast_container *container = parser->container;
  struct ast_node *node = dmrC_allocator_allocate(&container->ast_node_allocator, 0);
  node->type = AST_FUNCTION_EXPR;
  set_type(node->function_expr.type, RAVI_TFUNCTION);
  node->function_expr.is_method = false;
  node->function_expr.is_vararg = false;
  node->function_expr.args = NULL;
  node->function_expr.child_functions = NULL;
  node->function_expr.upvalues = NULL;
  node->function_expr.locals = NULL;
  node->function_expr.main_block = NULL;
  node->function_expr.function_statement_list = NULL;
  node->function_expr.parent_function = parser->current_function;
  if (parser->current_function) {
    // Make this function a child of current function
    add_ast_node(parser->container, &parser->current_function->function_expr.child_functions, node);
  }
  parser->current_function = node;
  new_scope(parser); /* Start function scope */
  return node;
}

/* Ends the function node and closes the scope for the function. The
 * function being closed becomes the current AST node, while parent function/scope
 * become current function/scope.
 */
static struct ast_node *end_function(struct parser_state *parser) {
  assert(parser->current_function);
  end_scope(parser);
  struct ast_node *function = parser->current_function;
  parser->current_function = function->function_expr.parent_function;
  return function;
}

/* mainfunc() equivalent - parses a Lua script, also known as chunk.
The code is wrapped in a vararg function */
static void parse_lua_chunk(struct parser_state *parser) {
  luaX_next(parser->ls);                                   /* read first token */
  parser->container->main_function = new_function(parser); /* vararg function wrapper */
  parser->container->main_function->function_expr.is_vararg = true;
  parse_statement_list(parser, &parser->container->main_function->function_expr.function_statement_list);
  end_function(parser);
  assert(parser->current_function == NULL);
  assert(parser->current_scope == NULL);
  check(parser->ls, TK_EOS);
  raviA_ast_typecheck(parser->container);
}

static void parser_state_init(struct parser_state *parser, LexState *ls, struct ast_container *container) {
  parser->ls = ls;
  parser->container = container;
  parser->current_function = NULL;
  parser->current_scope = NULL;
}

/*
** Parse the given source 'chunk' and build an abstract
** syntax tree; return 0 on success / non-zero return code on
** failure
** On success will push a userdata object containing the abstract
** syntax tree.
** On failure push an error message.
*/
static int parse_to_ast(lua_State *L, ZIO *z, Mbuffer *buff, const char *name, int firstchar) {
  struct ast_container *container = new_ast_container(L);
  LexState lexstate;
  lexstate.h = luaH_new(L); /* create table for scanner */
  sethvalue(L, L->top, lexstate.h);
  setuservalue(L, uvalue(L->top - 1), L->top); /* set the table as container's uservalue */
  luaD_inctop(L);
  TString *src = luaS_new(L, name); /* create and anchor TString */
  setsvalue(L, L->top, src);
  luaD_inctop(L);
  lexstate.buff = buff;
  lexstate.dyd = NULL; /* Unlike standard Lua parser / code generator we do not use this */
  luaX_setinput(L, &lexstate, z, src, firstchar);
  struct parser_state parser_state;
  parser_state_init(&parser_state, &lexstate, container);
  lua_lock(L);                     // Workaround for ZIO (used by lexer) which assumes lua_lock()
  parse_lua_chunk(&parser_state);  // FIXME must be protected call
  lua_unlock(L);
  L->top--; /* remove source name */
  L->top--; /* remove scanner table */
  return 0; /* OK */
}

/*
** ravi parser context
*/
struct parser_context { /* data to 'f_parser' */
  ZIO *z;
  Mbuffer buff; /* dynamic structure used by the scanner */
  Dyndata dyd;  /* dynamic structures used by the parser */
  const char *mode;
  const char *name;
};

static void checkmode(lua_State *L, const char *mode, const char *x) {
  if (mode && strchr(mode, x[0]) == NULL) {
    luaO_pushfstring(L, "attempt to load a %s chunk (mode is '%s')", x, mode);
    luaD_throw(L, LUA_ERRSYNTAX);
  }
}

static void ravi_parser_func(lua_State *L, void *ud) {
  struct parser_context *p = cast(struct parser_context *, ud);
  lua_lock(L);         // Workaroud for zgetc() which assumes lua_lock()
  int c = zgetc(p->z); /* read first character */
  lua_unlock(L);
  checkmode(L, p->mode, "text");
  parse_to_ast(L, p->z, &p->buff, p->name, c);
}

static int protected_ast_builder(lua_State *L, ZIO *z, const char *name, const char *mode) {
  struct parser_context p;
  int status;
  L->nny++; /* cannot yield during parsing */
  p.z = z;
  p.name = name;
  p.mode = mode;
  p.dyd.actvar.arr = NULL;
  p.dyd.actvar.size = 0;
  p.dyd.gt.arr = NULL;
  p.dyd.gt.size = 0;
  p.dyd.label.arr = NULL;
  p.dyd.label.size = 0;
  luaZ_initbuffer(L, &p.buff);
  status = luaD_pcall(L, ravi_parser_func, &p, savestack(L, L->top), L->errfunc);
  luaZ_freebuffer(L, &p.buff);
  luaM_freearray(L, p.dyd.actvar.arr, p.dyd.actvar.size);
  luaM_freearray(L, p.dyd.gt.arr, p.dyd.gt.size);
  luaM_freearray(L, p.dyd.label.arr, p.dyd.label.size);
  L->nny--;
  return status;
}

/*
**
** Builds an Abstract Syntax Tree (encapsulated in userdata type) from the given
** input buffer. This function returns 0 if all OK
* - On success a userdata object representing the tree,
*   else an error message will be pushed on to the stack
**
** This is part of the new experimental (wip) implementation of new
** parser and code generator
*/
static int build_ast_from_reader(lua_State *L, lua_Reader reader, void *data, const char *chunkname, const char *mode) {
  ZIO z;
  int status;
  if (!chunkname)
    chunkname = "?";
  luaZ_init(L, &z, reader, data);
  status = protected_ast_builder(L, &z, chunkname, mode);
  return status;
}

/*
** reserved slot, above all arguments, to hold a copy of the returned
** string to avoid it being collected while parsed. 'load' has four
** optional arguments (chunk, source name, mode, and environment).
*/
#define RESERVEDSLOT 5

/*
** Reader for generic 'load' function: 'lua_load' uses the
** stack for internal stuff, so the reader cannot change the
** stack top. Instead, it keeps its resulting string in a
** reserved slot inside the stack.
*/
static const char *generic_reader(lua_State *L, void *ud, size_t *size) {
  (void)(ud); /* not used */
  luaL_checkstack(L, 2, "too many nested functions");
  lua_pushvalue(L, 1); /* get function */
  lua_call(L, 0, 1);   /* call it */
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1); /* pop result */
    *size = 0;
    return NULL;
  }
  else if (!lua_isstring(L, -1))
    luaL_error(L, "reader function must return a string");
  lua_replace(L, RESERVEDSLOT); /* save string in reserved slot */
  return lua_tolstring(L, RESERVEDSLOT, size);
}

typedef struct string_buffer {
  const char *s;
  size_t size;
} String_buffer;

/* lua_Reader implementation */
static const char *string_reader(lua_State *L, void *ud, size_t *size) {
  String_buffer *ls = (String_buffer *)ud;
  (void)L; /* not used */
  if (ls->size == 0)
    return NULL;
  *size = ls->size;
  ls->size = 0;
  return ls->s;
}

/*
 * Builds an Abstract Syntax Tree (encapsulated in userdata type) from the given
 * input buffer. This function returns 0 if all OK
 * - On success a userdata object representing the tree,
 *   else an error message will be pushed on to the stack
 */
static int build_ast_from_buffer(lua_State *L, const char *buff, size_t size, const char *name, const char *mode) {
  String_buffer ls;
  ls.s = buff;
  ls.size = size;
  return build_ast_from_reader(L, string_reader, &ls, name, mode);
}

static int build_ast(lua_State *L) {
  int status;
  size_t l;
  const char *s = lua_tolstring(L, 1, &l);
  const char *mode = luaL_optstring(L, 3, "bt");
  // int env = (!lua_isnone(L, 4) ? 4 : 0);  /* 'env' index or 0 if no 'env' */
  if (s != NULL) { /* loading a string? */
    const char *chunkname = luaL_optstring(L, 2, s);
    status = build_ast_from_buffer(L, s, l, chunkname, mode);
  }
  else { /* loading from a reader function */
    const char *chunkname = luaL_optstring(L, 2, "=(load)");
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_settop(L, RESERVEDSLOT); /* create reserved slot */
    status = build_ast_from_reader(L, generic_reader, NULL, chunkname, mode);
  }
  if (status != 0) {
    lua_pushnil(L);
    lua_insert(L, -2); /* put before error message */
    return 2;          /* return nil plus error message */
  }
  return 1;
}

static const char *AST_type = "Ravi.AST";

#define test_Ravi_AST(L, idx) ((struct ast_container *)luaL_testudata(L, idx, AST_type))
#define check_Ravi_AST(L, idx) ((struct ast_container *)luaL_checkudata(L, idx, AST_type))

/* Converts the AST to a string representation */
static int ast_container_to_string(lua_State *L) {
  struct ast_container *container = check_Ravi_AST(L, 1);
  membuff_t mbuf;
  membuff_init(&mbuf, 1024);
  raviA_print_ast_node(&mbuf, container->main_function, 0);
  lua_pushstring(L, mbuf.buf);
  membuff_free(&mbuf);
  return 1;
}

static struct ast_container *new_ast_container(lua_State *L) {
  struct ast_container *container = (struct ast_container *)lua_newuserdata(L, sizeof(struct ast_container));
  dmrC_allocator_init(&container->ast_node_allocator, "ast nodes", sizeof(struct ast_node), sizeof(double), CHUNK);
  dmrC_allocator_init(&container->ptrlist_allocator, "ptrlists", sizeof(struct ptr_list), sizeof(double), CHUNK);
  dmrC_allocator_init(&container->block_scope_allocator, "block scopes", sizeof(struct block_scope), sizeof(double),
                      CHUNK);
  dmrC_allocator_init(&container->symbol_allocator, "symbols", sizeof(struct lua_symbol), sizeof(double), CHUNK);
  container->main_function = NULL;
  container->killed = false;
  luaL_getmetatable(L, AST_type);
  lua_setmetatable(L, -2);
  return container;
}

/* __gc function for AST */
static int collect_ast_container(lua_State *L) {
  struct ast_container *container = check_Ravi_AST(L, 1);
  if (!container->killed) {
    dmrC_allocator_destroy(&container->symbol_allocator);
    dmrC_allocator_destroy(&container->block_scope_allocator);
    dmrC_allocator_destroy(&container->ast_node_allocator);
    dmrC_allocator_destroy(&container->ptrlist_allocator);
    container->killed = true;
  }
  return 0;
}

static const luaL_Reg container_methods[] = {
    {"tostring", ast_container_to_string}, {"release", collect_ast_container}, {NULL, NULL}};

static const luaL_Reg astlib[] = {
    /* Entrypoint for new AST */
    {"parse", build_ast},
    {NULL, NULL}};

LUAMOD_API int raviopen_ast_library(lua_State *L) {
  luaL_newmetatable(L, AST_type);
  lua_pushcfunction(L, collect_ast_container);
  lua_setfield(L, -2, "__gc");
  lua_pushvalue(L, -1);           /* push metatable */
  lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  luaL_setfuncs(L, container_methods, 0);
  lua_pop(L, 1);

  luaL_newlib(L, astlib);
  return 1;
}
