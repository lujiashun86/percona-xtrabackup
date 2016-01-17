/*
   Copyright (c) 2000, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


/* A lexical scanner on a temporary buffer with a yacc interface */

#include "sql_lex.h"

#include "mysql_version.h"             // MYSQL_VERSION_ID
#include "sp_head.h"                   // sp_head
#include "sql_class.h"                 // THD
#include "sql_parse.h"                 // add_to_list
#include "parse_tree_helpers.h"
#include "sql_hints.yy.h"
#include "sql_lex_hints.h"
#include "sql_yacc.h"
#include "sql_optimizer.h"             // JOIN
#include "sql_plugin.h"                // plugin_unlock_list
#include "sql_show.h"                  // append_identifier
#include "sql_table.h"                 // primary_key_name
#include "sql_insert.h"                // Sql_cmd_insert_base
#include "lex_token.h"


extern int HINT_PARSER_parse(THD *thd,
                             Hint_scanner *scanner,
                             PT_hint_list **ret);

static int lex_one_token(YYSTYPE *yylval, THD *thd);

/*
  We are using pointer to this variable for distinguishing between assignment
  to NEW row field (when parsing trigger definition) and structured variable.
*/

sys_var *trg_new_row_fake_var= (sys_var*) 0x01;

/**
  LEX_STRING constant for null-string to be used in parser and other places.
*/
const LEX_STRING null_lex_str= {NULL, 0};
const LEX_STRING empty_lex_str= {(char *) "", 0};
/**
  Mapping from enum values in enum_binlog_stmt_unsafe to error codes.

  @note The order of the elements of this array must correspond to
  the order of elements in enum_binlog_stmt_unsafe.
*/
const int
Query_tables_list::binlog_stmt_unsafe_errcode[BINLOG_STMT_UNSAFE_COUNT] =
{
  ER_BINLOG_UNSAFE_LIMIT,
  ER_BINLOG_UNSAFE_SYSTEM_TABLE,
  ER_BINLOG_UNSAFE_AUTOINC_COLUMNS,
  ER_BINLOG_UNSAFE_UDF,
  ER_BINLOG_UNSAFE_SYSTEM_VARIABLE,
  ER_BINLOG_UNSAFE_SYSTEM_FUNCTION,
  ER_BINLOG_UNSAFE_NONTRANS_AFTER_TRANS,
  ER_BINLOG_UNSAFE_MULTIPLE_ENGINES_AND_SELF_LOGGING_ENGINE,
  ER_BINLOG_UNSAFE_MIXED_STATEMENT,
  ER_BINLOG_UNSAFE_INSERT_IGNORE_SELECT,
  ER_BINLOG_UNSAFE_INSERT_SELECT_UPDATE,
  ER_BINLOG_UNSAFE_WRITE_AUTOINC_SELECT,
  ER_BINLOG_UNSAFE_REPLACE_SELECT,
  ER_BINLOG_UNSAFE_CREATE_IGNORE_SELECT,
  ER_BINLOG_UNSAFE_CREATE_REPLACE_SELECT,
  ER_BINLOG_UNSAFE_CREATE_SELECT_AUTOINC,
  ER_BINLOG_UNSAFE_UPDATE_IGNORE,
  ER_BINLOG_UNSAFE_INSERT_TWO_KEYS,
  ER_BINLOG_UNSAFE_AUTOINC_NOT_FIRST,
  ER_BINLOG_UNSAFE_FULLTEXT_PLUGIN
};


/* Longest standard keyword name */

#define TOCK_NAME_LENGTH 24

/* 
  Names of the index hints (for error messages). Keep in sync with 
  index_hint_type 
*/

const char * index_hint_type_name[] =
{
  "IGNORE INDEX", 
  "USE INDEX", 
  "FORCE INDEX"
};


/**
  @note The order of the elements of this array must correspond to
  the order of elements in type_enum
*/
const char *st_select_lex::type_str[SLT_total]=
{ "NONE",
  "PRIMARY",
  "SIMPLE",
  "DERIVED",
  "SUBQUERY",
  "UNION",
  "UNION RESULT",
  "MATERIALIZED"
};


bool lex_init(void)
{
  DBUG_ENTER("lex_init");

  for (CHARSET_INFO **cs= all_charsets;
       cs < all_charsets + array_elements(all_charsets) - 1 ;
       cs++)
  {
    if (*cs && (*cs)->ctype && is_supported_parser_charset(*cs))
    {
      if (init_state_maps(*cs))
        DBUG_RETURN(true); // OOM
    }
  }

  DBUG_RETURN(false);
}


void lex_free(void)
{					// Call this when daemon ends
  DBUG_ENTER("lex_free");
  DBUG_VOID_RETURN;
}


void
st_parsing_options::reset()
{
  allows_variable= TRUE;
  allows_select_into= TRUE;
  allows_select_procedure= TRUE;
}

/**
 Cleans slave connection info.
*/
void struct_slave_connection::reset()
{
  user= 0;
  password= 0;
  plugin_auth= 0;
  plugin_dir= 0;
}

/**
  Perform initialization of Lex_input_stream instance.

  Basically, a buffer for a pre-processed query. This buffer should be large
  enough to keep a multi-statement query. The allocation is done once in
  Lex_input_stream::init() in order to prevent memory pollution when
  the server is processing large multi-statement queries.
*/

bool Lex_input_stream::init(THD *thd, const char* buff, size_t length)
{
  DBUG_EXECUTE_IF("bug42064_simulate_oom",
                  DBUG_SET("+d,simulate_out_of_memory"););

  query_charset= thd->charset();

  m_cpp_buf= (char*) thd->alloc(length + 1);

  DBUG_EXECUTE_IF("bug42064_simulate_oom",
                  DBUG_SET("-d,bug42064_simulate_oom");); 

  if (m_cpp_buf == NULL)
    return TRUE;

  m_thd= thd;
  reset(buff, length);

  return FALSE;
}


/**
  Prepare Lex_input_stream instance state for use for handling next SQL statement.

  It should be called between two statements in a multi-statement query.
  The operation resets the input stream to the beginning-of-parse state,
  but does not reallocate m_cpp_buf.
*/

void
Lex_input_stream::reset(const char *buffer, size_t length)
{
  yylineno= 1;
  yytoklen= 0;
  yylval= NULL;
  lookahead_token= -1;
  lookahead_yylval= NULL;
  skip_digest= false;
  /*
    Lex_input_stream modifies the query string in one special case (sic!).
    yyUnput() modifises the string when patching version comments.
    This is done to prevent newer slaves from executing a different
    statement than older masters.

    For now, cast away const here. This means that e.g. SHOW PROCESSLIST
    can see partially patched query strings. It would be better if we
    could replicate the query string as is and have the slave take the
    master version into account.
  */
  m_ptr= const_cast<char*>(buffer);
  m_tok_start= NULL;
  m_tok_end= NULL;
  m_end_of_query= buffer + length;
  m_buf= buffer;
  m_buf_length= length;
  m_echo= TRUE;
  m_cpp_tok_start= NULL;
  m_cpp_tok_end= NULL;
  m_body_utf8= NULL;
  m_cpp_utf8_processed_ptr= NULL;
  next_state= MY_LEX_START;
  found_semicolon= NULL;
  ignore_space= MY_TEST(m_thd->variables.sql_mode & MODE_IGNORE_SPACE);
  stmt_prepare_mode= FALSE;
  multi_statements= TRUE;
  in_comment=NO_COMMENT;
  m_underscore_cs= NULL;
  m_cpp_ptr= m_cpp_buf;
}


/**
  The operation is called from the parser in order to
  1) designate the intention to have utf8 body;
  1) Indicate to the lexer that we will need a utf8 representation of this
     statement;
  2) Determine the beginning of the body.

  @param thd        Thread context.
  @param begin_ptr  Pointer to the start of the body in the pre-processed
                    buffer.
*/

void Lex_input_stream::body_utf8_start(THD *thd, const char *begin_ptr)
{
  DBUG_ASSERT(begin_ptr);
  DBUG_ASSERT(m_cpp_buf <= begin_ptr && begin_ptr <= m_cpp_buf + m_buf_length);

  size_t body_utf8_length=
    (m_buf_length / thd->variables.character_set_client->mbminlen) *
    my_charset_utf8_bin.mbmaxlen;

  m_body_utf8= (char *) thd->alloc(body_utf8_length + 1);
  m_body_utf8_ptr= m_body_utf8;
  *m_body_utf8_ptr= 0;

  m_cpp_utf8_processed_ptr= begin_ptr;
}

/**
  @brief The operation appends unprocessed part of pre-processed buffer till
  the given pointer (ptr) and sets m_cpp_utf8_processed_ptr to end_ptr.

  The idea is that some tokens in the pre-processed buffer (like character
  set introducers) should be skipped.

  Example:
    CPP buffer: SELECT 'str1', _latin1 'str2';
    m_cpp_utf8_processed_ptr -- points at the "SELECT ...";
    In order to skip "_latin1", the following call should be made:
      body_utf8_append(<pointer to "_latin1 ...">, <pointer to " 'str2'...">)

  @param ptr      Pointer in the pre-processed buffer, which specifies the
                  end of the chunk, which should be appended to the utf8
                  body.
  @param end_ptr  Pointer in the pre-processed buffer, to which
                  m_cpp_utf8_processed_ptr will be set in the end of the
                  operation.
*/

void Lex_input_stream::body_utf8_append(const char *ptr,
                                        const char *end_ptr)
{
  DBUG_ASSERT(m_cpp_buf <= ptr && ptr <= m_cpp_buf + m_buf_length);
  DBUG_ASSERT(m_cpp_buf <= end_ptr && end_ptr <= m_cpp_buf + m_buf_length);

  if (!m_body_utf8)
    return;

  if (m_cpp_utf8_processed_ptr >= ptr)
    return;

  size_t bytes_to_copy= ptr - m_cpp_utf8_processed_ptr;

  memcpy(m_body_utf8_ptr, m_cpp_utf8_processed_ptr, bytes_to_copy);
  m_body_utf8_ptr += bytes_to_copy;
  *m_body_utf8_ptr= 0;

  m_cpp_utf8_processed_ptr= end_ptr;
}


/**
  The operation appends unprocessed part of the pre-processed buffer till
  the given pointer (ptr) and sets m_cpp_utf8_processed_ptr to ptr.

  @param ptr  Pointer in the pre-processed buffer, which specifies the end
              of the chunk, which should be appended to the utf8 body.
*/

void Lex_input_stream::body_utf8_append(const char *ptr)
{
  body_utf8_append(ptr, ptr);
}

/**
  The operation converts the specified text literal to the utf8 and appends
  the result to the utf8-body.

  @param thd      Thread context.
  @param txt      Text literal.
  @param txt_cs   Character set of the text literal.
  @param end_ptr  Pointer in the pre-processed buffer, to which
                  m_cpp_utf8_processed_ptr will be set in the end of the
                  operation.
*/

void Lex_input_stream::body_utf8_append_literal(THD *thd,
                                                const LEX_STRING *txt,
                                                const CHARSET_INFO *txt_cs,
                                                const char *end_ptr)
{
  if (!m_cpp_utf8_processed_ptr)
    return;

  LEX_STRING utf_txt;

  if (!my_charset_same(txt_cs, &my_charset_utf8_general_ci))
  {
    thd->convert_string(&utf_txt,
                        &my_charset_utf8_general_ci,
                        txt->str, txt->length,
                        txt_cs);
  }
  else
  {
    utf_txt.str= txt->str;
    utf_txt.length= txt->length;
  }

  /* NOTE: utf_txt.length is in bytes, not in symbols. */

  memcpy(m_body_utf8_ptr, utf_txt.str, utf_txt.length);
  m_body_utf8_ptr += utf_txt.length;
  *m_body_utf8_ptr= 0;

  m_cpp_utf8_processed_ptr= end_ptr;
}

void Lex_input_stream::add_digest_token(uint token, LEX_YYSTYPE yylval)
{
  if (m_digest != NULL)
  {
    m_digest= digest_add_token(m_digest, token, yylval);
  }
}

void Lex_input_stream::reduce_digest_token(uint token_left, uint token_right)
{
  if (m_digest != NULL)
  {
    m_digest= digest_reduce_token(m_digest, token_left, token_right);
  }
}


LEX::~LEX()
{
  destroy_query_tables_list();
  plugin_unlock_list(NULL, plugins.begin(), plugins.size());
  unit= NULL;                     // Created in mem_root - no destructor
  select_lex= NULL;
  m_current_select= NULL;
}

/**
  Reset a LEX object so that it is ready for a new query preparation
  and execution.
  Pointers to query expression and query block objects are set to NULL.
  This is correct, as they point into a mem_root that has been recycled.
*/

void LEX::reset()
{
  context_stack.empty();
  unit= NULL;
  select_lex= NULL;
  m_current_select= NULL;
  all_selects_list= NULL;
  load_set_str_list.empty();

  bulk_insert_row_cnt= 0;

  load_update_list.empty();
  load_value_list.empty();

  call_value_list.empty();

  purge_value_list.empty();

  kill_value_list.empty();

  set_var_list.empty();
  param_list.empty();
  view_list.empty();
  prepared_stmt_params.empty();
  auxiliary_table_list.empty();
  describe= DESCRIBE_NONE;
  subqueries= false;
  context_analysis_only= 0;
  derived_tables= 0;
  safe_to_cache_query= true;
  insert_table= NULL;
  insert_table_leaf= NULL;
  parsing_options.reset();
  length= 0;
  part_info= NULL;
  duplicates= DUP_ERROR;
  ignore= false;
  spname= NULL;
  sphead= NULL;
  set_sp_current_parsing_ctx(NULL);
  m_sql_cmd= NULL;
  proc_analyse= NULL;
  query_tables= NULL;
  reset_query_tables_list(false);
  expr_allows_subselect= true;
  use_only_table_context= false;
  contains_plaintext_password= false;
  keep_diagnostics= DA_KEEP_NOTHING;

  name.str= NULL;
  name.length= 0;
  event_parse_data= NULL;
  profile_options= PROFILE_NONE;
  select_number= 0;
  allow_sum_func= 0;
  in_sum_func= NULL;
  server_options.reset();
  explain_format= NULL;
  is_lex_started= true;
  used_tables= 0;
  reset_slave_info.all= false;
  alter_tablespace_info= NULL;
  mi.channel= NULL;

  wild= NULL;
  exchange= NULL;
  is_set_password_sql= false;
  mark_broken(false);
  max_execution_time= 0;
  parse_gcol_expr= false;
  opt_hints_global= NULL;
  binlog_need_explicit_defaults_ts= false;
}


/**
  Call lex_start() before every query that is to be prepared and executed.
  Because of this, it's critical not to do too many things here.  (We already
  do too much)

  The function creates a select_lex and a select_lex_unit object.
  These objects should rather be created by the parser bottom-up.
*/

bool lex_start(THD *thd)
{
  DBUG_ENTER("lex_start");

  LEX *lex= thd->lex;

  lex->thd= thd;
  lex->reset();
  // Initialize the cost model to be used for this query
  thd->init_cost_model();

  const bool status= lex->new_top_level_query();
  DBUG_ASSERT(lex->current_select() == NULL);
  lex->m_current_select= lex->select_lex;

  DBUG_RETURN(status);
}


/**
  Call this function after preparation and execution of a query.
*/

void lex_end(LEX *lex)
{
  DBUG_ENTER("lex_end");
  DBUG_PRINT("enter", ("lex: 0x%lx", (long) lex));

  /* release used plugins */
  if (!lex->plugins.empty()) /* No function call and no mutex if no plugins. */
  {
    plugin_unlock_list(0, lex->plugins.begin(), lex->plugins.size());
  }
  lex->plugins.clear();

  delete lex->sphead;
  lex->sphead= NULL;
  lex->clear_values_map();

  DBUG_VOID_RETURN;
}


st_select_lex *LEX::new_empty_query_block()
{
  st_select_lex *select=
    new (thd->mem_root) st_select_lex(NULL, NULL, NULL, NULL, NULL, NULL);
  if (select == NULL)
    return NULL;             /* purecov: inspected */

  select->parent_lex= this;

  return select;
}


/**
  Create new select_lex_unit and select_lex objects for a query block,
  which can be either a top-level query or a subquery.
  For the second and subsequent query block of a UNION query, use
  LEX::new_union_query() instead.
  Set the new select_lex as the current select_lex of the LEX object.

  @param curr_select    current query specification

  @return new query specification if successful, NULL if error
*/
st_select_lex *LEX::new_query(st_select_lex *curr_select)
{
  DBUG_ENTER("LEX::new_query");

  if (curr_select != NULL &&
      curr_select->nest_level >= (int) MAX_SELECT_NESTING)
  {
    my_error(ER_TOO_HIGH_LEVEL_OF_NESTING_FOR_SELECT,MYF(0),MAX_SELECT_NESTING);
    DBUG_RETURN(NULL);
  }

  Name_resolution_context *outer_context= current_context();

  SELECT_LEX *const select= new_empty_query_block();
  if (!select)
    DBUG_RETURN(NULL);       /* purecov: inspected */

  SELECT_LEX_UNIT *const sel_unit=
    new (thd->mem_root) SELECT_LEX_UNIT(curr_select ?
                                        curr_select->parsing_place :
                                        CTX_NONE);
  if (!sel_unit)
    DBUG_RETURN(NULL);       /* purecov: inspected */

  sel_unit->thd= thd;

  // Link the new "unit" below the current select_lex, if any
  if (curr_select != NULL)
    sel_unit->include_down(this, curr_select);

  select->include_down(this, sel_unit);

  select->include_in_global(&all_selects_list);

  if (select->set_context(NULL))
    DBUG_RETURN(NULL);        /* purecov: inspected */
  /*
    Assume that a subquery has an outer name resolution context.
    If not (ie. if this is a derived table), set it to NULL later
  */
  if (select_lex == NULL)    // Outer-most query block
  {
  }
  else if (select->outer_select()->parsing_place == CTX_ON)
  {
    /*
      This subquery is part of an ON clause, so we need to link the
      name resolution context for this subquery with the ON context.

      @todo outer_context is not the same as
      &select_lex->outer_select()->context in one case:
        (SELECT 1 as a) UNION (SELECT 2) ORDER BY (SELECT a);
      When we create the select_lex for the subquery in ORDER BY,
      1) outer_context is the context of the second SELECT of the UNION
      2) select_lex->outer_select() is the fake select_lex, which context
         is the one of the first SELECT of the UNION (see
         st_select_lex_unit::add_fake_select_lex()).
      2) is the correct context, per the documentation. 1) is not, and using
      it leads to a resolving error for the query above.
      We should fix 1) and then use it unconditionally here.
    */
    select->context.outer_context= outer_context;
  }
  else if (select->outer_select()->parsing_place == CTX_DERIVED)
  {
    // Currently, outer references are not allowed for a derived table
    DBUG_ASSERT(select->context.outer_context == NULL);
  }
  else
  {
    select->context.outer_context= &select->outer_select()->context;
  }
  /*
    in subquery is SELECT query and we allow resolution of names in SELECT
    list
  */
  select->context.resolve_in_select_list= true;

  DBUG_RETURN(select);
}


/**
  Create new select_lex object for all branches of a UNION except the left-most
  one.
  Set the new select_lex as the current select_lex of the LEX object.

  @param curr_select current query specification
  @param distinct True if part of UNION DISTINCT query

  @return new query specification if successful, NULL if an error occurred.
*/

st_select_lex *LEX::new_union_query(st_select_lex *curr_select, bool distinct)
{
  DBUG_ENTER("LEX::new_union_query");

  DBUG_ASSERT(unit != NULL && select_lex != NULL);

  // Is this the outer-most query expression?
  bool const outer_most= curr_select->master_unit() == unit;
  /*
     Only the last SELECT can have INTO. Since the grammar won't allow INTO in
     a nested SELECT, we make this check only when creating a query block on
     the outer-most level:
  */
  if (outer_most && result)
  {
    my_error(ER_WRONG_USAGE, MYF(0), "UNION", "INTO");
    DBUG_RETURN(NULL);
  }
  if (proc_analyse)
  {
    my_error(ER_WRONG_USAGE, MYF(0), "UNION", "SELECT ... PROCEDURE ANALYSE()");
    DBUG_RETURN(NULL);
  }

  if (curr_select->order_list.first && !curr_select->braces)
  {
    my_error(ER_WRONG_USAGE, MYF(0), "UNION", "ORDER BY");
    DBUG_RETURN(NULL);
  }

  if (curr_select->explicit_limit && !curr_select->braces)
  {
    my_error(ER_WRONG_USAGE, MYF(0), "UNION", "LIMIT");
    DBUG_RETURN(NULL);
  }

  SELECT_LEX *const select= new_empty_query_block();
  if (!select)
    DBUG_RETURN(NULL);       /* purecov: inspected */

  select->include_neighbour(this, curr_select);

  SELECT_LEX_UNIT *const sel_unit= select->master_unit();

  if (!sel_unit->fake_select_lex && sel_unit->add_fake_select_lex(thd))
    DBUG_RETURN(NULL);       /* purecov: inspected */

  if (select->set_context(sel_unit->first_select()->context.outer_context))
    DBUG_RETURN(NULL);       /* purecov: inspected */

  select->include_in_global(&all_selects_list);

  select->linkage= UNION_TYPE;

  if (distinct)           /* UNION DISTINCT - remember position */
    sel_unit->union_distinct= select;

  /*
    By default we assume that this is a regular subquery, in which resolution
    of names in SELECT list is allowed.
  */
  select->context.resolve_in_select_list= true;

  DBUG_RETURN(select);
}


/**
  Given a LEX object, create a query expression object (select_lex_unit) and
  a query block object (select_lex).

  @return false if successful, true if error
*/

bool LEX::new_top_level_query()
{
  DBUG_ENTER("LEX::new_top_level_query");

  // Assure that the LEX does not contain any query expression already
  DBUG_ASSERT(unit == NULL &&
              select_lex == NULL);

  // Check for the special situation when using INTO OUTFILE and LOAD DATA.
  DBUG_ASSERT(result == 0);

  select_lex= new_query(NULL);
  if (select_lex == NULL)
    DBUG_RETURN(true);     /* purecov: inspected */

  unit= select_lex->master_unit();

  DBUG_RETURN(false);
}


/**
  Initialize a LEX object, a query expression object (select_lex_unit) and
  a query block object (select_lex).
  All objects are passed as pointers so they can be stack-allocated.
  The purpose of this structure is for short-lived procedures that need a
  LEX and a query block object.

  Do not extend the struct with more query objects after creation.

  The struct can be abandoned after use, no cleanup is needed.

  @param sel_unit  Pointer to the query expression object
  @param select    Pointer to the query block object
*/

void LEX::new_static_query(SELECT_LEX_UNIT *sel_unit, SELECT_LEX *select)

{
  DBUG_ENTER("LEX::new_static_query");

  reset();

  DBUG_ASSERT(unit == NULL && select_lex == NULL && current_select() == NULL);

  select->parent_lex= this;

  sel_unit->thd= thd;
  select->include_down(this, sel_unit);

  select->include_in_global(&all_selects_list);

  (void)select->set_context(NULL);

  select_lex= select;
  unit= sel_unit;

  set_current_select(select);

  select->context.resolve_in_select_list= true;

  DBUG_VOID_RETURN;
}


Yacc_state::~Yacc_state()
{
  if (yacc_yyss)
  {
    my_free(yacc_yyss);
    my_free(yacc_yyvs);
    my_free(yacc_yyls);
  }
}


static bool consume_optimizer_hints(Lex_input_stream *lip)
{
  const my_lex_states *state_map= lip->query_charset->state_maps->main_map;
  int whitespace= 0;
  uchar c= lip->yyPeek();
  size_t newlines= 0;

  for (; state_map[c] == MY_LEX_SKIP; whitespace++, c= lip->yyPeekn(whitespace))
  {
    if (c == '\n')
      newlines++;
  }

  if (lip->yyPeekn(whitespace) == '/' && lip->yyPeekn(whitespace + 1) == '*' &&
      lip->yyPeekn(whitespace + 2) == '+')
  {
    lip->yylineno+= newlines;
    lip->yySkipn(whitespace); // skip whitespace

    Hint_scanner hint_scanner(lip->m_thd, lip->yylineno, lip->get_ptr(),
                              lip->get_end_of_query() - lip->get_ptr(),
                              lip->m_digest);
    PT_hint_list *hint_list= NULL;
    int rc= HINT_PARSER_parse(lip->m_thd, &hint_scanner, &hint_list);
    if (rc == 2)
      return true; // Bison's internal OOM error
    else if (rc == 1)
    {
      /*
        This branch is for 2 cases:
        1. YYABORT in the hint parser grammar (we use it to process OOM errors),
        2. open commentary error.
      */
      lip->start_token(); // adjust error message text pointer to "/*+"
      return true;
    }
    else
    {
      lip->yylineno= hint_scanner.get_lineno();
      lip->yySkipn(hint_scanner.get_ptr() - lip->get_ptr());
      lip->yylval->optimizer_hints= hint_list; // NULL in case of syntax error
      lip->m_digest= hint_scanner.get_digest(); // NULL is digest buf. is full.
      return false;
    }
  }
  else
    return false;
}


static int find_keyword(Lex_input_stream *lip, uint len, bool function)
{
  const char *tok= lip->get_tok_start();

  const SYMBOL *symbol= function ?
    Lex_hash::sql_keywords_and_funcs.get_hash_symbol(tok, len) :
    Lex_hash::sql_keywords.get_hash_symbol(tok, len);

  if (symbol)
  {
    lip->yylval->symbol.symbol=symbol;
    lip->yylval->symbol.str= (char*) tok;
    lip->yylval->symbol.length=len;

    if ((symbol->tok == NOT_SYM) &&
        (lip->m_thd->variables.sql_mode & MODE_HIGH_NOT_PRECEDENCE))
      return NOT2_SYM;
    if ((symbol->tok == OR_OR_SYM) &&
	!(lip->m_thd->variables.sql_mode & MODE_PIPES_AS_CONCAT))
      return OR2_SYM;

    lip->yylval->optimizer_hints= NULL;
    if (symbol->group & SG_HINTABLE_KEYWORDS)
    {
      lip->add_digest_token(symbol->tok, lip->yylval);
      if (consume_optimizer_hints(lip))
        return ABORT_SYM;
      lip->skip_digest= true;
    }

    return symbol->tok;
  }
  return 0;
}

/*
  Check if name is a keyword

  SYNOPSIS
    is_keyword()
    name      checked name (must not be empty)
    len       length of checked name

  RETURN VALUES
    0         name is a keyword
    1         name isn't a keyword
*/

bool is_keyword(const char *name, size_t len)
{
  DBUG_ASSERT(len != 0);
  return Lex_hash::sql_keywords.get_hash_symbol(name, len) != NULL;
}

/**
  Check if name is a sql function

    @param name      checked name

    @return is this a native function or not
    @retval 0         name is a function
    @retval 1         name isn't a function
*/

bool is_lex_native_function(const LEX_STRING *name)
{
  DBUG_ASSERT(name != NULL);
  return Lex_hash::sql_keywords_and_funcs.get_hash_symbol(name->str,
      (uint) name->length) != NULL;
}

/* make a copy of token before ptr and set yytoklen */

static LEX_STRING get_token(Lex_input_stream *lip, uint skip, uint length)
{
  LEX_STRING tmp;
  lip->yyUnget();                       // ptr points now after last token char
  tmp.length=lip->yytoklen=length;
  tmp.str= lip->m_thd->strmake(lip->get_tok_start() + skip, tmp.length);

  lip->m_cpp_text_start= lip->get_cpp_tok_start() + skip;
  lip->m_cpp_text_end= lip->m_cpp_text_start + tmp.length;

  return tmp;
}

/* 
 todo: 
   There are no dangerous charsets in mysql for function 
   get_quoted_token yet. But it should be fixed in the 
   future to operate multichar strings (like ucs2)
*/

static LEX_STRING get_quoted_token(Lex_input_stream *lip,
                                   uint skip,
                                   uint length, char quote)
{
  LEX_STRING tmp;
  const char *from, *end;
  char *to;
  lip->yyUnget();                       // ptr points now after last token char
  tmp.length= lip->yytoklen=length;
  tmp.str=(char*) lip->m_thd->alloc(tmp.length+1);
  from= lip->get_tok_start() + skip;
  to= tmp.str;
  end= to+length;

  lip->m_cpp_text_start= lip->get_cpp_tok_start() + skip;
  lip->m_cpp_text_end= lip->m_cpp_text_start + length;

  for ( ; to != end; )
  {
    if ((*to++= *from++) == quote)
    {
      from++;					// Skip double quotes
      lip->m_cpp_text_start++;
    }
  }
  *to= 0;					// End null for safety
  return tmp;
}


/*
  Return an unescaped text literal without quotes
  Fix sometimes to do only one scan of the string
*/

static char *get_text(Lex_input_stream *lip, int pre_skip, int post_skip)
{
  uchar c,sep;
  uint found_escape=0;
  const CHARSET_INFO *cs= lip->m_thd->charset();

  lip->tok_bitmap= 0;
  sep= lip->yyGetLast();                        // String should end with this
  while (! lip->eof())
  {
    c= lip->yyGet();
    lip->tok_bitmap|= c;
    {
      int l;
      if (use_mb(cs) &&
          (l = my_ismbchar(cs,
                           lip->get_ptr() -1,
                           lip->get_end_of_query()))) {
        lip->skip_binary(l-1);
        continue;
      }
    }
    if (c == '\\' &&
        !(lip->m_thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES))
    {					// Escaped character
      found_escape=1;
      if (lip->eof())
	return 0;
      lip->yySkip();
    }
    else if (c == sep)
    {
      if (c == lip->yyGet())            // Check if two separators in a row
      {
        found_escape=1;                 // duplicate. Remember for delete
	continue;
      }
      else
        lip->yyUnget();

      /* Found end. Unescape and return string */
      const char *str, *end;
      char *start;

      str= lip->get_tok_start();
      end= lip->get_ptr();
      /* Extract the text from the token */
      str += pre_skip;
      end -= post_skip;
      DBUG_ASSERT(end >= str);

      if (!(start= static_cast<char *>(lip->m_thd->alloc((uint) (end-str)+1))))
	return (char*) "";		// Sql_alloc has set error flag

      lip->m_cpp_text_start= lip->get_cpp_tok_start() + pre_skip;
      lip->m_cpp_text_end= lip->get_cpp_ptr() - post_skip;

      if (!found_escape)
      {
	lip->yytoklen=(uint) (end-str);
	memcpy(start,str,lip->yytoklen);
	start[lip->yytoklen]=0;
      }
      else
      {
        char *to;

	for (to=start ; str != end ; str++)
	{
	  int l;
	  if (use_mb(cs) &&
              (l = my_ismbchar(cs, str, end))) {
	      while (l--)
		  *to++ = *str++;
	      str--;
	      continue;
	  }
	  if (!(lip->m_thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES) &&
              *str == '\\' && str+1 != end)
	  {
	    switch(*++str) {
	    case 'n':
	      *to++='\n';
	      break;
	    case 't':
	      *to++= '\t';
	      break;
	    case 'r':
	      *to++ = '\r';
	      break;
	    case 'b':
	      *to++ = '\b';
	      break;
	    case '0':
	      *to++= 0;			// Ascii null
	      break;
	    case 'Z':			// ^Z must be escaped on Win32
	      *to++='\032';
	      break;
	    case '_':
	    case '%':
	      *to++= '\\';		// remember prefix for wildcard
	      /* Fall through */
	    default:
              *to++= *str;
	      break;
	    }
	  }
	  else if (*str == sep)
	    *to++= *str++;		// Two ' or "
	  else
	    *to++ = *str;
	}
	*to=0;
	lip->yytoklen=(uint) (to-start);
      }
      return start;
    }
  }
  return 0;					// unexpected end of query
}


uint Lex_input_stream::get_lineno(const char *raw_ptr)
{
  DBUG_ASSERT(m_buf <= raw_ptr && raw_ptr < m_end_of_query);
  if (!(m_buf <= raw_ptr && raw_ptr < m_end_of_query))
    return 1;

  uint ret= 1;
  const CHARSET_INFO *cs= m_thd->charset();
  for (const char *c= m_buf; c < raw_ptr; c++)
  {
    uint mb_char_len;
    if (use_mb(cs) && (mb_char_len= my_ismbchar(cs, c, m_end_of_query)))
    {
      c+= mb_char_len - 1; // skip the rest of the multibyte character
      continue; // we don't expect '\n' there
    }
    if (*c == '\n')
      ret++;
  }
  return ret;
}


/*
** Calc type of integer; long integer, longlong integer or real.
** Returns smallest type that match the string.
** When using unsigned long long values the result is converted to a real
** because else they will be unexpected sign changes because all calculation
** is done with longlong or double.
*/

static const char *long_str="2147483647";
static const uint long_len=10;
static const char *signed_long_str="-2147483648";
static const char *longlong_str="9223372036854775807";
static const uint longlong_len=19;
static const char *signed_longlong_str="-9223372036854775808";
static const uint signed_longlong_len=19;
static const char *unsigned_longlong_str="18446744073709551615";
static const uint unsigned_longlong_len=20;

static inline uint int_token(const char *str,uint length)
{
  if (length < long_len)			// quick normal case
    return NUM;
  bool neg=0;

  if (*str == '+')				// Remove sign and pre-zeros
  {
    str++; length--;
  }
  else if (*str == '-')
  {
    str++; length--;
    neg=1;
  }
  while (*str == '0' && length)
  {
    str++; length --;
  }
  if (length < long_len)
    return NUM;

  uint smaller,bigger;
  const char *cmp;
  if (neg)
  {
    if (length == long_len)
    {
      cmp= signed_long_str+1;
      smaller=NUM;				// If <= signed_long_str
      bigger=LONG_NUM;				// If >= signed_long_str
    }
    else if (length < signed_longlong_len)
      return LONG_NUM;
    else if (length > signed_longlong_len)
      return DECIMAL_NUM;
    else
    {
      cmp=signed_longlong_str+1;
      smaller=LONG_NUM;				// If <= signed_longlong_str
      bigger=DECIMAL_NUM;
    }
  }
  else
  {
    if (length == long_len)
    {
      cmp= long_str;
      smaller=NUM;
      bigger=LONG_NUM;
    }
    else if (length < longlong_len)
      return LONG_NUM;
    else if (length > longlong_len)
    {
      if (length > unsigned_longlong_len)
        return DECIMAL_NUM;
      cmp=unsigned_longlong_str;
      smaller=ULONGLONG_NUM;
      bigger=DECIMAL_NUM;
    }
    else
    {
      cmp=longlong_str;
      smaller=LONG_NUM;
      bigger= ULONGLONG_NUM;
    }
  }
  while (*cmp && *cmp++ == *str++) ;
  return ((uchar) str[-1] <= (uchar) cmp[-1]) ? smaller : bigger;
}


/**
  Given a stream that is advanced to the first contained character in 
  an open comment, consume the comment.  Optionally, if we are allowed, 
  recurse so that we understand comments within this current comment.

  At this level, we do not support version-condition comments.  We might 
  have been called with having just passed one in the stream, though.  In 
  that case, we probably want to tolerate mundane comments inside.  Thus,
  the case for recursion.

  @retval  Whether EOF reached before comment is closed.
*/
bool consume_comment(Lex_input_stream *lip, int remaining_recursions_permitted)
{
  uchar c;
  while (! lip->eof())
  {
    c= lip->yyGet();

    if (remaining_recursions_permitted > 0)
    {
      if ((c == '/') && (lip->yyPeek() == '*'))
      {
        lip->yySkip(); /* Eat asterisk */
        consume_comment(lip, remaining_recursions_permitted-1);
        continue;
      }
    }

    if (c == '*')
    {
      if (lip->yyPeek() == '/')
      {
        lip->yySkip(); /* Eat slash */
        return FALSE;
      }
    }

    if (c == '\n')
      lip->yylineno++;
  }

  return TRUE;
}


/*
  yylex() function implementation for the main parser

  @param yylval         [out]  semantic value of the token being parsed (yylval)
  @param yylloc         [out]  "location" of the token being parsed (yylloc)
  @param thd            THD

  @return               token number

  @note
  MYSQLlex remember the following states from the following MYSQLlex():

  - MY_LEX_EOQ			Found end of query
  - MY_LEX_OPERATOR_OR_IDENT	Last state was an ident, text or number
				(which can't be followed by a signed number)
*/

int MYSQLlex(YYSTYPE *yylval, YYLTYPE *yylloc, THD *thd)
{
  Lex_input_stream *lip= & thd->m_parser_state->m_lip;
  int token;

  if (lip->lookahead_token >= 0)
  {
    /*
      The next token was already parsed in advance,
      return it.
    */
    token= lip->lookahead_token;
    lip->lookahead_token= -1;
    *yylval= *(lip->lookahead_yylval);
    yylloc->cpp.start= lip->get_cpp_tok_start();
    yylloc->cpp.end= lip->get_cpp_ptr();
    yylloc->raw.start= lip->get_tok_start();
    yylloc->raw.end= lip->get_ptr();
    lip->lookahead_yylval= NULL;
    lip->add_digest_token(token, yylval);
    return token;
  }

  token= lex_one_token(yylval, thd);
  yylloc->cpp.start= lip->get_cpp_tok_start();
  yylloc->raw.start= lip->get_tok_start();

  switch(token) {
  case WITH:
    /*
      Parsing 'WITH' 'ROLLUP' or 'WITH' 'CUBE' requires 2 look ups,
      which makes the grammar LALR(2).
      Replace by a single 'WITH_ROLLUP' or 'WITH_CUBE' token,
      to transform the grammar into a LALR(1) grammar,
      which sql_yacc.yy can process.
    */
    token= lex_one_token(yylval, thd);
    switch(token) {
    case CUBE_SYM:
      yylloc->cpp.end= lip->get_cpp_ptr();
      yylloc->raw.end= lip->get_ptr();
      lip->add_digest_token(WITH_CUBE_SYM, yylval);
      return WITH_CUBE_SYM;
    case ROLLUP_SYM:
      yylloc->cpp.end= lip->get_cpp_ptr();
      yylloc->raw.end= lip->get_ptr();
      lip->add_digest_token(WITH_ROLLUP_SYM, yylval);
      return WITH_ROLLUP_SYM;
    default:
      /*
        Save the token following 'WITH'
      */
      lip->lookahead_yylval= lip->yylval;
      lip->yylval= NULL;
      lip->lookahead_token= token;
      yylloc->cpp.end= lip->get_cpp_ptr();
      yylloc->raw.end= lip->get_ptr();
      lip->add_digest_token(WITH, yylval);
      return WITH;
    }
    break;
  }

  yylloc->cpp.end= lip->get_cpp_ptr();
  yylloc->raw.end= lip->get_ptr();
  if (!lip->skip_digest)
    lip->add_digest_token(token, yylval);
  lip->skip_digest= false;
  return token;
}

static int lex_one_token(YYSTYPE *yylval, THD *thd)
{
  uchar c= 0;
  bool comment_closed;
  int tokval, result_state;
  uint length;
  enum my_lex_states state;
  Lex_input_stream *lip= & thd->m_parser_state->m_lip;
  const CHARSET_INFO *cs= thd->charset();
  const my_lex_states *state_map= cs->state_maps->main_map;
  const uchar *ident_map= cs->ident_map;

  lip->yylval=yylval;			// The global state

  lip->start_token();
  state=lip->next_state;
  lip->next_state=MY_LEX_OPERATOR_OR_IDENT;
  for (;;)
  {
    switch (state) {
    case MY_LEX_OPERATOR_OR_IDENT:	// Next is operator or keyword
    case MY_LEX_START:			// Start of token
      // Skip starting whitespace
      while(state_map[c= lip->yyPeek()] == MY_LEX_SKIP)
      {
	if (c == '\n')
	  lip->yylineno++;

        lip->yySkip();
      }

      /* Start of real token */
      lip->restart_token();
      c= lip->yyGet();
      state= state_map[c];
      break;
    case MY_LEX_ESCAPE:
      if (lip->yyGet() == 'N')
      {					// Allow \N as shortcut for NULL
	yylval->lex_str.str=(char*) "\\N";
	yylval->lex_str.length=2;
	return NULL_SYM;
      }
    case MY_LEX_CHAR:			// Unknown or single char token
    case MY_LEX_SKIP:			// This should not happen
      if (c == '-' && lip->yyPeek() == '-' &&
          (my_isspace(cs,lip->yyPeekn(1)) ||
           my_iscntrl(cs,lip->yyPeekn(1))))
      {
        state=MY_LEX_COMMENT;
        break;
      }

      if (c == '-' && lip->yyPeek() == '>')    // '->'
      {
        lip->yySkip();
        lip->next_state= MY_LEX_START;
        return JSON_SEPARATOR_SYM;
      }

      if (c != ')')
	lip->next_state= MY_LEX_START;	// Allow signed numbers

      if (c == ',')
      {
        /*
          Warning:
          This is a work around, to make the "remember_name" rule in
          sql/sql_yacc.yy work properly.
          The problem is that, when parsing "select expr1, expr2",
          the code generated by bison executes the *pre* action
          remember_name (see select_item) *before* actually parsing the
          first token of expr2.
        */
        lip->restart_token();
      }
      else
      {
        /*
          Check for a placeholder: it should not precede a possible identifier
          because of binlogging: when a placeholder is replaced with
          its value in a query for the binlog, the query must stay
          grammatically correct.
        */
        if (c == '?' && lip->stmt_prepare_mode && !ident_map[lip->yyPeek()])
        return(PARAM_MARKER);
      }

      return((int) c);

    case MY_LEX_IDENT_OR_NCHAR:
      if (lip->yyPeek() != '\'')
      {
	state= MY_LEX_IDENT;
	break;
      }
      /* Found N'string' */
      lip->yySkip();                         // Skip '
      if (!(yylval->lex_str.str = get_text(lip, 2, 1)))
      {
	state= MY_LEX_CHAR;             // Read char by char
	break;
      }
      yylval->lex_str.length= lip->yytoklen;
      return(NCHAR_STRING);

    case MY_LEX_IDENT_OR_HEX:
      if (lip->yyPeek() == '\'')
      {					// Found x'hex-number'
	state= MY_LEX_HEX_NUMBER;
	break;
      }
    case MY_LEX_IDENT_OR_BIN:
      if (lip->yyPeek() == '\'')
      {                                 // Found b'bin-number'
        state= MY_LEX_BIN_NUMBER;
        break;
      }
    case MY_LEX_IDENT:
      const char *start;
      if (use_mb(cs))
      {
	result_state= IDENT_QUOTED;
        switch (my_mbcharlen(cs, lip->yyGetLast()))
        {
        case 1:
          break;
        case 0:
          if (my_mbmaxlenlen(cs) < 2)
            break;
          /* else fall through */
        default:
          int l = my_ismbchar(cs,
                              lip->get_ptr() -1,
                              lip->get_end_of_query());
          if (l == 0) {
            state = MY_LEX_CHAR;
            continue;
          }
          lip->skip_binary(l - 1);
        }
        while (ident_map[c=lip->yyGet()])
        {
          switch (my_mbcharlen(cs, c))
          {
          case 1:
            break;
          case 0:
            if (my_mbmaxlenlen(cs) < 2)
              break;
            /* else fall through */
          default:
            int l;
            if ((l = my_ismbchar(cs,
                                 lip->get_ptr() -1,
                                 lip->get_end_of_query())) == 0)
              break;
            lip->skip_binary(l-1);
          }
        }
      }
      else
      {
        for (result_state= c; ident_map[c= lip->yyGet()]; result_state|= c) ;
        /* If there were non-ASCII characters, mark that we must convert */
        result_state= result_state & 0x80 ? IDENT_QUOTED : IDENT;
      }
      length= lip->yyLength();
      start= lip->get_ptr();
      if (lip->ignore_space)
      {
        /*
          If we find a space then this can't be an identifier. We notice this
          below by checking start != lex->ptr.
        */
        for (; state_map[c] == MY_LEX_SKIP ; c= lip->yyGet()) ;
      }
      if (start == lip->get_ptr() && c == '.' && ident_map[lip->yyPeek()])
	lip->next_state=MY_LEX_IDENT_SEP;
      else
      {					// '(' must follow directly if function
        lip->yyUnget();
	if ((tokval = find_keyword(lip, length, c == '(')))
	{
	  lip->next_state= MY_LEX_START;	// Allow signed numbers
	  return(tokval);		// Was keyword
	}
        lip->yySkip();                  // next state does a unget
      }
      yylval->lex_str=get_token(lip, 0, length);

      /*
         Note: "SELECT _bla AS 'alias'"
         _bla should be considered as a IDENT if charset haven't been found.
         So we don't use MYF(MY_WME) with get_charset_by_csname to avoid
         producing an error.
      */

      if (yylval->lex_str.str[0] == '_')
      {
        CHARSET_INFO *cs= get_charset_by_csname(yylval->lex_str.str + 1,
                                                MY_CS_PRIMARY, MYF(0));
        if (cs)
        {
          yylval->charset= cs;
          lip->m_underscore_cs= cs;

          lip->body_utf8_append(lip->m_cpp_text_start,
                                lip->get_cpp_tok_start() + length);
          return(UNDERSCORE_CHARSET);
        }
      }

      lip->body_utf8_append(lip->m_cpp_text_start);

      lip->body_utf8_append_literal(thd, &yylval->lex_str, cs,
                                    lip->m_cpp_text_end);

      return(result_state);			// IDENT or IDENT_QUOTED

    case MY_LEX_IDENT_SEP:		// Found ident and now '.'
      yylval->lex_str.str= (char*) lip->get_ptr();
      yylval->lex_str.length= 1;
      c= lip->yyGet();                  // should be '.'
      lip->next_state= MY_LEX_IDENT_START;// Next is an ident (not a keyword)
      if (!ident_map[lip->yyPeek()])            // Probably ` or "
	lip->next_state= MY_LEX_START;
      return((int) c);

    case MY_LEX_NUMBER_IDENT:		// number or ident which num-start
      if (lip->yyGetLast() == '0')
      {
        c= lip->yyGet();
        if (c == 'x')
        {
          while (my_isxdigit(cs,(c = lip->yyGet()))) ;
          if ((lip->yyLength() >= 3) && !ident_map[c])
          {
            /* skip '0x' */
            yylval->lex_str=get_token(lip, 2, lip->yyLength()-2);
            return (HEX_NUM);
          }
          lip->yyUnget();
          state= MY_LEX_IDENT_START;
          break;
        }
        else if (c == 'b')
        {
          while ((c= lip->yyGet()) == '0' || c == '1') ;
          if ((lip->yyLength() >= 3) && !ident_map[c])
          {
            /* Skip '0b' */
            yylval->lex_str= get_token(lip, 2, lip->yyLength()-2);
            return (BIN_NUM);
          }
          lip->yyUnget();
          state= MY_LEX_IDENT_START;
          break;
        }
        lip->yyUnget();
      }

      while (my_isdigit(cs, (c = lip->yyGet()))) ;
      if (!ident_map[c])
      {					// Can't be identifier
	state=MY_LEX_INT_OR_REAL;
	break;
      }
      if (c == 'e' || c == 'E')
      {
	// The following test is written this way to allow numbers of type 1e1
        if (my_isdigit(cs,lip->yyPeek()) ||
            (c=(lip->yyGet())) == '+' || c == '-')
	{				// Allow 1E+10
          if (my_isdigit(cs,lip->yyPeek()))     // Number must have digit after sign
	  {
            lip->yySkip();
            while (my_isdigit(cs,lip->yyGet())) ;
            yylval->lex_str=get_token(lip, 0, lip->yyLength());
	    return(FLOAT_NUM);
	  }
	}
        lip->yyUnget();
      }
      // fall through
    case MY_LEX_IDENT_START:			// We come here after '.'
      result_state= IDENT;
      if (use_mb(cs))
      {
	result_state= IDENT_QUOTED;
        while (ident_map[c=lip->yyGet()])
        {
          switch (my_mbcharlen(cs, c))
          {
          case 1:
            break;
          case 0:
            if (my_mbmaxlenlen(cs) < 2)
              break;
            /* else fall through */
          default:
            int l;
            if ((l = my_ismbchar(cs,
                                 lip->get_ptr() -1,
                                 lip->get_end_of_query())) == 0)
              break;
            lip->skip_binary(l-1);
          }
        }
      }
      else
      {
        for (result_state=0; ident_map[c= lip->yyGet()]; result_state|= c) ;
        /* If there were non-ASCII characters, mark that we must convert */
        result_state= result_state & 0x80 ? IDENT_QUOTED : IDENT;
      }
      if (c == '.' && ident_map[lip->yyPeek()])
	lip->next_state=MY_LEX_IDENT_SEP;// Next is '.'

      yylval->lex_str= get_token(lip, 0, lip->yyLength());

      lip->body_utf8_append(lip->m_cpp_text_start);

      lip->body_utf8_append_literal(thd, &yylval->lex_str, cs,
                                    lip->m_cpp_text_end);

      return(result_state);

    case MY_LEX_USER_VARIABLE_DELIMITER:	// Found quote char
    {
      uint double_quotes= 0;
      char quote_char= c;                       // Used char
      for(;;)
      {
        c= lip->yyGet();
        if (c == 0)
        {
          lip->yyUnget();
          return ABORT_SYM;                     // Unmatched quotes
        }

	int var_length;
	if ((var_length= my_mbcharlen(cs, c)) == 1)
	{
	  if (c == quote_char)
	  {
            if (lip->yyPeek() != quote_char)
	      break;
            c=lip->yyGet();
	    double_quotes++;
	    continue;
	  }
	}
        else if (use_mb(cs))
        {
          if ((var_length= my_ismbchar(cs, lip->get_ptr() - 1,
                                       lip->get_end_of_query())))
            lip->skip_binary(var_length-1);
        }
      }
      if (double_quotes)
	yylval->lex_str=get_quoted_token(lip, 1,
                                         lip->yyLength() - double_quotes -1,
					 quote_char);
      else
        yylval->lex_str=get_token(lip, 1, lip->yyLength() -1);
      if (c == quote_char)
        lip->yySkip();                  // Skip end `
      lip->next_state= MY_LEX_START;

      lip->body_utf8_append(lip->m_cpp_text_start);

      lip->body_utf8_append_literal(thd, &yylval->lex_str, cs,
                                    lip->m_cpp_text_end);

      return(IDENT_QUOTED);
    }
    case MY_LEX_INT_OR_REAL:		// Complete int or incomplete real
      if (c != '.')
      {					// Found complete integer number.
        yylval->lex_str=get_token(lip, 0, lip->yyLength());
	return int_token(yylval->lex_str.str, (uint) yylval->lex_str.length);
      }
      // fall through
    case MY_LEX_REAL:			// Incomplete real number
      while (my_isdigit(cs,c = lip->yyGet())) ;

      if (c == 'e' || c == 'E')
      {
        c = lip->yyGet();
	if (c == '-' || c == '+')
          c = lip->yyGet();                     // Skip sign
	if (!my_isdigit(cs,c))
	{				// No digit after sign
	  state= MY_LEX_CHAR;
	  break;
	}
        while (my_isdigit(cs,lip->yyGet())) ;
        yylval->lex_str=get_token(lip, 0, lip->yyLength());
	return(FLOAT_NUM);
      }
      yylval->lex_str=get_token(lip, 0, lip->yyLength());
      return(DECIMAL_NUM);

    case MY_LEX_HEX_NUMBER:		// Found x'hexstring'
      lip->yySkip();                    // Accept opening '
      while (my_isxdigit(cs, (c= lip->yyGet()))) ;
      if (c != '\'')
        return(ABORT_SYM);              // Illegal hex constant
      lip->yySkip();                    // Accept closing '
      length= lip->yyLength();          // Length of hexnum+3
      if ((length % 2) == 0)
        return(ABORT_SYM);              // odd number of hex digits
      yylval->lex_str=get_token(lip,
                                2,          // skip x'
                                length-3);  // don't count x' and last '
      return (HEX_NUM);

    case MY_LEX_BIN_NUMBER:           // Found b'bin-string'
      lip->yySkip();                  // Accept opening '
      while ((c= lip->yyGet()) == '0' || c == '1') ;
      if (c != '\'')
        return(ABORT_SYM);            // Illegal hex constant
      lip->yySkip();                  // Accept closing '
      length= lip->yyLength();        // Length of bin-num + 3
      yylval->lex_str= get_token(lip,
                                 2,         // skip b'
                                 length-3); // don't count b' and last '
      return (BIN_NUM);

    case MY_LEX_CMP_OP:			// Incomplete comparison operator
      if (state_map[lip->yyPeek()] == MY_LEX_CMP_OP ||
          state_map[lip->yyPeek()] == MY_LEX_LONG_CMP_OP)
        lip->yySkip();
      if ((tokval = find_keyword(lip, lip->yyLength() + 1, 0)))
      {
	lip->next_state= MY_LEX_START;	// Allow signed numbers
	return(tokval);
      }
      state = MY_LEX_CHAR;		// Something fishy found
      break;

    case MY_LEX_LONG_CMP_OP:		// Incomplete comparison operator
      if (state_map[lip->yyPeek()] == MY_LEX_CMP_OP ||
          state_map[lip->yyPeek()] == MY_LEX_LONG_CMP_OP)
      {
        lip->yySkip();
        if (state_map[lip->yyPeek()] == MY_LEX_CMP_OP)
          lip->yySkip();
      }
      if ((tokval = find_keyword(lip, lip->yyLength() + 1, 0)))
      {
	lip->next_state= MY_LEX_START;	// Found long op
	return(tokval);
      }
      state = MY_LEX_CHAR;		// Something fishy found
      break;

    case MY_LEX_BOOL:
      if (c != lip->yyPeek())
      {
	state=MY_LEX_CHAR;
	break;
      }
      lip->yySkip();
      tokval = find_keyword(lip,2,0);	// Is a bool operator
      lip->next_state= MY_LEX_START;	// Allow signed numbers
      return(tokval);

    case MY_LEX_STRING_OR_DELIMITER:
      if (thd->variables.sql_mode & MODE_ANSI_QUOTES)
      {
	state= MY_LEX_USER_VARIABLE_DELIMITER;
	break;
      }
      /* " used for strings */
    case MY_LEX_STRING:			// Incomplete text string
      if (!(yylval->lex_str.str = get_text(lip, 1, 1)))
      {
	state= MY_LEX_CHAR;		// Read char by char
	break;
      }
      yylval->lex_str.length=lip->yytoklen;

      lip->body_utf8_append(lip->m_cpp_text_start);

      lip->body_utf8_append_literal(thd, &yylval->lex_str,
        lip->m_underscore_cs ? lip->m_underscore_cs : cs,
        lip->m_cpp_text_end);

      lip->m_underscore_cs= NULL;

      return(TEXT_STRING);

    case MY_LEX_COMMENT:			//  Comment
      thd->m_parser_state->add_comment();
      while ((c = lip->yyGet()) != '\n' && c) ;
      lip->yyUnget();                   // Safety against eof
      state = MY_LEX_START;		// Try again
      break;
    case MY_LEX_LONG_COMMENT:		/* Long C comment? */
      if (lip->yyPeek() != '*')
      {
	state=MY_LEX_CHAR;		// Probable division
	break;
      }
      thd->m_parser_state->add_comment();
      /* Reject '/' '*', since we might need to turn off the echo */
      lip->yyUnget();

      lip->save_in_comment_state();

      if (lip->yyPeekn(2) == '!')
      {
        lip->in_comment= DISCARD_COMMENT;
        /* Accept '/' '*' '!', but do not keep this marker. */
        lip->set_echo(FALSE);
        lip->yySkip();
        lip->yySkip();
        lip->yySkip();

        /*
          The special comment format is very strict:
          '/' '*' '!', followed by exactly
          1 digit (major), 2 digits (minor), then 2 digits (dot).
          32302 -> 3.23.02
          50032 -> 5.0.32
          50114 -> 5.1.14
        */
        char version_str[6];
        if (  my_isdigit(cs, (version_str[0]= lip->yyPeekn(0)))
           && my_isdigit(cs, (version_str[1]= lip->yyPeekn(1)))
           && my_isdigit(cs, (version_str[2]= lip->yyPeekn(2)))
           && my_isdigit(cs, (version_str[3]= lip->yyPeekn(3)))
           && my_isdigit(cs, (version_str[4]= lip->yyPeekn(4)))
           )
        {
          version_str[5]= 0;
          ulong version;
          version=strtol(version_str, NULL, 10);

          if (version <= MYSQL_VERSION_ID)
          {
            /* Accept 'M' 'm' 'm' 'd' 'd' */
            lip->yySkipn(5);
            /* Expand the content of the special comment as real code */
            lip->set_echo(TRUE);
            state=MY_LEX_START;
            break;  /* Do not treat contents as a comment.  */
          }
          else
          {
            /*
              Patch and skip the conditional comment to avoid it
              being propagated infinitely (eg. to a slave).
            */
            char *pcom= lip->yyUnput(' ');
            comment_closed= ! consume_comment(lip, 1);
            if (! comment_closed)
            {
              *pcom= '!';
            }
            /* version allowed to have one level of comment inside. */
          }
        }
        else
        {
          /* Not a version comment. */
          state=MY_LEX_START;
          lip->set_echo(TRUE);
          break;
        }
      }
      else
      {
        lip->in_comment= PRESERVE_COMMENT;
        lip->yySkip();                  // Accept /
        lip->yySkip();                  // Accept *
        comment_closed= ! consume_comment(lip, 0);
        /* regular comments can have zero comments inside. */
      }
      /*
        Discard:
        - regular '/' '*' comments,
        - special comments '/' '*' '!' for a future version,
        by scanning until we find a closing '*' '/' marker.

        Nesting regular comments isn't allowed.  The first 
        '*' '/' returns the parser to the previous state.

        /#!VERSI oned containing /# regular #/ is allowed #/

		Inside one versioned comment, another versioned comment
		is treated as a regular discardable comment.  It gets
		no special parsing.
      */

      /* Unbalanced comments with a missing '*' '/' are a syntax error */
      if (! comment_closed)
        return (ABORT_SYM);
      state = MY_LEX_START;             // Try again
      lip->restore_in_comment_state();
      break;
    case MY_LEX_END_LONG_COMMENT:
      if ((lip->in_comment != NO_COMMENT) && lip->yyPeek() == '/')
      {
        /* Reject '*' '/' */
        lip->yyUnget();
        /* Accept '*' '/', with the proper echo */
        lip->set_echo(lip->in_comment == PRESERVE_COMMENT);
        lip->yySkipn(2);
        /* And start recording the tokens again */
        lip->set_echo(TRUE);
        
        /*
          C-style comments are replaced with a single space (as it
          is in C and C++).  If there is already a whitespace 
          character at this point in the stream, the space is
          not inserted.

          See also ISO/IEC 9899:1999 §5.1.1.2  
          ("Programming languages — C")
        */
        if (!my_isspace(cs, lip->yyPeek()) &&
            lip->get_cpp_ptr() != lip->get_cpp_buf() &&
            !my_isspace(cs, *(lip->get_cpp_ptr() - 1)))
          lip->cpp_inject(' ');

        lip->in_comment=NO_COMMENT;
        state=MY_LEX_START;
      }
      else
	state=MY_LEX_CHAR;		// Return '*'
      break;
    case MY_LEX_SET_VAR:		// Check if ':='
      if (lip->yyPeek() != '=')
      {
	state=MY_LEX_CHAR;		// Return ':'
	break;
      }
      lip->yySkip();
      return (SET_VAR);
    case MY_LEX_SEMICOLON:			// optional line terminator
      state= MY_LEX_CHAR;               // Return ';'
      break;
    case MY_LEX_EOL:
      if (lip->eof())
      {
        lip->yyUnget();                 // Reject the last '\0'
        lip->set_echo(FALSE);
        lip->yySkip();
        lip->set_echo(TRUE);
        /* Unbalanced comments with a missing '*' '/' are a syntax error */
        if (lip->in_comment != NO_COMMENT)
          return (ABORT_SYM);
        lip->next_state=MY_LEX_END;     // Mark for next loop
        return(END_OF_INPUT);
      }
      state=MY_LEX_CHAR;
      break;
    case MY_LEX_END:
      lip->next_state=MY_LEX_END;
      return(0);			// We found end of input last time

      /* Actually real shouldn't start with . but allow them anyhow */
    case MY_LEX_REAL_OR_POINT:
      if (my_isdigit(cs,lip->yyPeek()))
	state = MY_LEX_REAL;		// Real
      else
      {
	state= MY_LEX_IDENT_SEP;	// return '.'
        lip->yyUnget();                 // Put back '.'
      }
      break;
    case MY_LEX_USER_END:		// end '@' of user@hostname
      switch (state_map[lip->yyPeek()]) {
      case MY_LEX_STRING:
      case MY_LEX_USER_VARIABLE_DELIMITER:
      case MY_LEX_STRING_OR_DELIMITER:
        break;
      case MY_LEX_USER_END:
	lip->next_state=MY_LEX_SYSTEM_VAR;
	break;
      default:
	lip->next_state=MY_LEX_HOSTNAME;
	break;
      }
      yylval->lex_str.str=(char*) lip->get_ptr();
      yylval->lex_str.length=1;
      return((int) '@');
    case MY_LEX_HOSTNAME:		// end '@' of user@hostname
      for (c=lip->yyGet() ;
	   my_isalnum(cs,c) || c == '.' || c == '_' ||  c == '$';
           c= lip->yyGet()) ;
      yylval->lex_str=get_token(lip, 0, lip->yyLength());
      return(LEX_HOSTNAME);
    case MY_LEX_SYSTEM_VAR:
      yylval->lex_str.str=(char*) lip->get_ptr();
      yylval->lex_str.length=1;
      lip->yySkip();                                    // Skip '@'
      lip->next_state= (state_map[lip->yyPeek()] ==
			MY_LEX_USER_VARIABLE_DELIMITER ?
			MY_LEX_OPERATOR_OR_IDENT :
			MY_LEX_IDENT_OR_KEYWORD);
      return((int) '@');
    case MY_LEX_IDENT_OR_KEYWORD:
      /*
	We come here when we have found two '@' in a row.
	We should now be able to handle:
	[(global | local | session) .]variable_name
      */

      for (result_state= 0; ident_map[c= lip->yyGet()]; result_state|= c) ;
      /* If there were non-ASCII characters, mark that we must convert */
      result_state= result_state & 0x80 ? IDENT_QUOTED : IDENT;

      if (c == '.')
	lip->next_state=MY_LEX_IDENT_SEP;
      length= lip->yyLength();
      if (length == 0)
        return(ABORT_SYM);              // Names must be nonempty.
      if ((tokval= find_keyword(lip, length,0)))
      {
        lip->yyUnget();                         // Put back 'c'
	return(tokval);				// Was keyword
      }
      yylval->lex_str=get_token(lip, 0, length);

      lip->body_utf8_append(lip->m_cpp_text_start);

      lip->body_utf8_append_literal(thd, &yylval->lex_str, cs,
                                    lip->m_cpp_text_end);

      return(result_state);
    }
  }
}


void trim_whitespace(const CHARSET_INFO *cs, LEX_STRING *str)
{
  /*
    TODO:
    This code assumes that there are no multi-bytes characters
    that can be considered white-space.
  */

  while ((str->length > 0) && (my_isspace(cs, str->str[0])))
  {
    str->length --;
    str->str ++;
  }

  /*
    FIXME:
    Also, parsing backward is not safe with multi bytes characters
  */
  while ((str->length > 0) && (my_isspace(cs, str->str[str->length-1])))
  {
    str->length --;
    /* set trailing spaces to 0 as there're places that don't respect length */
    str->str[str->length]= 0;
  }
}


/**
  Construct and initialize st_select_lex_unit object.
*/

st_select_lex_unit::st_select_lex_unit(enum_parsing_context parsing_context) :
  next(NULL),
  prev(NULL),
  master(NULL),
  slave(NULL),
  explain_marker(CTX_NONE),
  prepared(false),
  optimized(false),
  executed(false),
  result_table_list(),
  union_result(NULL),
  table(NULL),
  m_query_result(NULL),
  uncacheable(0),
  cleaned(UC_DIRTY),
  item_list(),
  types(),
  select_limit_cnt(HA_POS_ERROR),
  offset_limit_cnt(0),
  item(NULL),
  thd(NULL),
  fake_select_lex(NULL),
  saved_fake_select_lex(NULL),
  union_distinct(NULL)
{
  switch (parsing_context)
  {
    case CTX_ORDER_BY:
      explain_marker= CTX_ORDER_BY_SQ; // A subquery in ORDER BY
      break;
    case CTX_GROUP_BY:
      explain_marker= CTX_GROUP_BY_SQ; // A subquery in GROUP BY
      break;
    case CTX_ON:
      explain_marker= CTX_WHERE;
      break;
    case CTX_HAVING:                         // A subquery elsewhere
    case CTX_SELECT_LIST:
    case CTX_UPDATE_VALUE_LIST:
    case CTX_WHERE:
    case CTX_DERIVED:
    case CTX_NONE:                           // A subquery in a non-select
      explain_marker= parsing_context;
      break;
    default:
      /* Subquery can't happen outside of those ^. */
      DBUG_ASSERT(false); /* purecov: inspected */
      break;
  }
}


/**
  Construct and initialize st_select_lex object.
*/

st_select_lex::st_select_lex
               (TABLE_LIST *table_list,
                List<Item> *item_list_arg,       // unused
                Item *where, Item *having, Item *limit, Item *offset)
                //SQL_I_LIST<ORDER> *group_by, SQL_I_LIST<ORDER> order_by)
  :
  next(NULL),
  prev(NULL),
  master(NULL),
  slave(NULL),
  link_next(NULL),
  link_prev(NULL),
  m_query_result(NULL),
  m_base_options(0),
  m_active_options(0),
  sql_cache(SQL_CACHE_UNSPECIFIED),
  uncacheable(0),
  linkage(UNSPECIFIED_TYPE),
  no_table_names_allowed(false),
  context(),
  first_context(&context),
  resolve_place(RESOLVE_NONE),
  resolve_nest(NULL),
  semijoin_disallowed(false),
  db(NULL),
  m_where_cond(where),
  m_having_cond(having),
  cond_value(Item::COND_UNDEF),
  having_value(Item::COND_UNDEF),
  parent_lex(NULL),
  olap(UNSPECIFIED_OLAP_TYPE),
  table_list(),
  group_list(),
  group_list_ptrs(NULL),
  item_list(),
  is_item_list_lookup(false),
  fields_list(item_list),
  all_fields(),
  ftfunc_list(&ftfunc_list_alloc),
  ftfunc_list_alloc(),
  join(NULL),
  top_join_list(),
  join_list(&top_join_list),
  embedding(NULL),
  sj_nests(),
  leaf_tables(NULL),
  leaf_table_count(0),
  derived_table_count(0),
  materialized_derived_table_count(0),
  has_sj_nests(false),
  partitioned_table_count(0),
  order_list(),
  order_list_ptrs(NULL),
  select_limit(NULL),
  offset_limit(NULL),
  ref_pointer_array(),
  select_n_having_items(0),
  cond_count(0),
  between_count(0),
  max_equal_elems(0),
  select_n_where_fields(0),
  parsing_place(CTX_NONE),
  in_sum_expr(0),
  with_sum_func(false),
  n_sum_items(0),
  n_child_sum_items(0),
  select_number(0),
  nest_level(0),
  inner_sum_func_list(NULL),
  with_wild(0),
  braces(false),
  having_fix_field(false),
  group_fix_field(false),
  inner_refs_list(),
  explicit_limit(false),
  subquery_in_having(false),
  first_execution(true),
  sj_pullout_done(false),
  exclude_from_table_unique_test(false),
  allow_merge_derived(true),
  prev_join_using(NULL),
  select_list_tables(0),
  outer_join(0),
  opt_hints_qb(NULL),
  m_agg_func_used(false),
  sj_candidates(NULL)
{
}


/**
  Set the name resolution context for the specified query block.

  @param outer_context Outer name resolution context.
                       NULL if none or it will be set later.
*/

bool st_select_lex::set_context(Name_resolution_context *outer_context)
{
  context.init();
  context.select_lex= this;
  context.outer_context= outer_context;
  /*
    Add the name resolution context of this query block to the
    stack of contexts for the whole query.
  */
  return parent_lex->push_context(&context);
}


/**
  Exclude this unit and its immediately contained select_lex objects
  from query expression / query block chain.

  @note
    Units that belong to the select_lex objects of the current unit will be
    brought up one level and will replace the current unit in the list of units.
*/
void st_select_lex_unit::exclude_level()
{
  /*
    Changing unit tree should be done only when LOCK_query_plan mutex is
    taken. This is needed to provide stable tree for EXPLAIN FOR CONNECTION.
  */
  thd->lock_query_plan();
  SELECT_LEX_UNIT *units= NULL;
  SELECT_LEX_UNIT **units_last= &units;
  SELECT_LEX *sl= first_select();
  while (sl)
  {
    SELECT_LEX *next_select= sl->next_select();

    // unlink current level from global SELECTs list
    if (sl->link_prev && (*sl->link_prev= sl->link_next))
      sl->link_next->link_prev= sl->link_prev;

    // bring up underlay levels
    SELECT_LEX_UNIT **last= NULL;
    for (SELECT_LEX_UNIT *u= sl->first_inner_unit(); u; u= u->next_unit())
    {
      /*
        We are excluding a SELECT_LEX from the hierarchy of
        SELECT_LEX_UNITs and SELECT_LEXes. Since this level is
        removed, we must also exclude the Name_resolution_context
        belonging to this level. Do this by looping through inner
        subqueries and changing their contexts' outer context pointers
        to point to the outer context of the removed SELECT_LEX.
      */
      for (SELECT_LEX *s= u->first_select(); s; s= s->next_select())
      {
        if (s->context.outer_context == &sl->context)
          s->context.outer_context= sl->context.outer_context;
      }
      if (u->fake_select_lex &&
          u->fake_select_lex->context.outer_context == &sl->context)
        u->fake_select_lex->context.outer_context= sl->context.outer_context;
      u->master= master;
      last= &(u->next);
    }
    if (last)
    {
      (*units_last)= sl->first_inner_unit();
      units_last= last;
    }

    sl->invalidate();
    sl= next_select;
  }
  if (units)
  {
    // include brought up levels in place of current
    (*prev)= units;
    (*units_last)= next;
    if (next)
      next->prev= units_last;
    units->prev= prev;
  }
  else
  {
    // exclude currect unit from list of nodes
    if (prev)
      (*prev)= next;
    if (next)
      next->prev= prev;
  }

  invalidate();
  thd->unlock_query_plan();
}


/**
  Exclude subtree of current unit from tree of SELECTs
*/
void st_select_lex_unit::exclude_tree()
{
  SELECT_LEX *sl= first_select();
  while (sl)
  {
    SELECT_LEX *next_select= sl->next_select();

    // unlink current level from global SELECTs list
    if (sl->link_prev && (*sl->link_prev= sl->link_next))
      sl->link_next->link_prev= sl->link_prev;

    // unlink underlay levels
    for (SELECT_LEX_UNIT *u= sl->first_inner_unit(); u; u= u->next_unit())
    {
      u->exclude_level();
    }

    sl->invalidate();
    sl= next_select;
  }
  // exclude currect unit from list of nodes
  if (prev)
    (*prev)= next;
  if (next)
    next->prev= prev;

  invalidate();
}


/**
  Invalidate by nulling out pointers to other st_select_lex_units and
  st_select_lexes.
*/
void st_select_lex_unit::invalidate()
{
  next= NULL;
  prev= NULL;
  master= NULL;
  slave= NULL;
}


/**
  Make active options from base options, supplied options and environment:

  @param added_options   Options that are added to the active options
  @param removed_options Options that are removed from the active options
*/

void st_select_lex::make_active_options(ulonglong added_options,
                                        ulonglong removed_options)
{
  m_active_options= (m_base_options | added_options |
                     parent_lex->thd->variables.option_bits) &
                    ~removed_options;
}

/**
  Mark all query blocks from this to 'last' as dependent

  @param last Pointer to last st_select_lex struct, before which all
              st_select_lex are marked as as dependent.

  @note
    last should be reachable from this st_select_lex
*/

void st_select_lex::mark_as_dependent(st_select_lex *last)
{
  /*
    Mark all selects from resolved to 1 before select where was
    found table as depended (of select where was found table)
  */
  for (SELECT_LEX *s= this;
       s && s != last;
       s= s->outer_select())
  {
    if (!(s->uncacheable & UNCACHEABLE_DEPENDENT))
    {
      // Select is dependent of outer select
      s->uncacheable= (s->uncacheable & ~UNCACHEABLE_UNITED) |
                       UNCACHEABLE_DEPENDENT;
      SELECT_LEX_UNIT *munit= s->master_unit();
      munit->uncacheable= (munit->uncacheable & ~UNCACHEABLE_UNITED) |
                       UNCACHEABLE_DEPENDENT;
      for (SELECT_LEX *sl= munit->first_select(); sl ; sl= sl->next_select())
      {
        if (sl != s &&
            !(sl->uncacheable & (UNCACHEABLE_DEPENDENT | UNCACHEABLE_UNITED)))
          sl->uncacheable|= UNCACHEABLE_UNITED;
      }
    }
  }
}


/*
  prohibit using LIMIT clause
*/
bool st_select_lex::test_limit()
{
  if (select_limit != 0)
  {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "LIMIT & IN/ALL/ANY/SOME subquery");
    return(1);
  }
  return(0);
}


enum_parsing_context st_select_lex_unit::get_explain_marker() const
{
  thd->query_plan.assert_plan_is_locked_if_other();
  return explain_marker;
}


void st_select_lex_unit::set_explain_marker(enum_parsing_context m)
{
  thd->lock_query_plan();
  explain_marker= m;
  thd->unlock_query_plan();
}


void st_select_lex_unit::
set_explain_marker_from(const st_select_lex_unit *u)
{
  thd->lock_query_plan();
  explain_marker= u->explain_marker;
  thd->unlock_query_plan();
}


ha_rows st_select_lex::get_offset()
{
  ulonglong val= 0;

  if (offset_limit)
  {
    // see comment for st_select_lex::get_limit()
    bool fix_fields_successful= true;
    if (!offset_limit->fixed)
    {
      fix_fields_successful= !offset_limit->fix_fields(master->thd, NULL);

      DBUG_ASSERT(fix_fields_successful);
    }
    val= fix_fields_successful ? offset_limit->val_uint() : HA_POS_ERROR;
  }

  return (ha_rows)val;
}


ha_rows st_select_lex::get_limit()
{
  ulonglong val= HA_POS_ERROR;

  if (select_limit)
  {
    /*
      fix_fields() has not been called for select_limit. That's due to the
      historical reasons -- this item could be only of type Item_int, and
      Item_int does not require fix_fields(). Thus, fix_fields() was never
      called for select_limit.

      Some time ago, Item_splocal was also allowed for LIMIT / OFFSET clauses.
      However, the fix_fields() behavior was not updated, which led to a crash
      in some cases.

      There is no single place where to call fix_fields() for LIMIT / OFFSET
      items during the fix-fields-phase. Thus, for the sake of readability,
      it was decided to do it here, on the evaluation phase (which is a
      violation of design, but we chose the lesser of two evils).

      We can call fix_fields() here, because select_limit can be of two
      types only: Item_int and Item_splocal. Item_int::fix_fields() is trivial,
      and Item_splocal::fix_fields() (or rather Item_sp_variable::fix_fields())
      has the following properties:
        1) it does not affect other items;
        2) it does not fail.

      Nevertheless DBUG_ASSERT was added to catch future changes in
      fix_fields() implementation. Also added runtime check against a result
      of fix_fields() in order to handle error condition in non-debug build.
    */
    bool fix_fields_successful= true;
    if (!select_limit->fixed)
    {
      fix_fields_successful= !select_limit->fix_fields(master->thd, NULL);

      DBUG_ASSERT(fix_fields_successful);
    }
    val= fix_fields_successful ? select_limit->val_uint() : HA_POS_ERROR;
  }

  return (ha_rows)val;
}


void st_select_lex::add_order_to_list(ORDER *order)
{
  add_to_list(order_list, order);
}


bool st_select_lex::add_item_to_list(THD *thd, Item *item)
{
  DBUG_ENTER("st_select_lex::add_item_to_list");
  DBUG_PRINT("info", ("Item: 0x%lx", (long) item));
  DBUG_RETURN(item_list.push_back(item));
}


void st_select_lex::add_group_to_list(ORDER *order)
{
  add_to_list(group_list, order);
}


bool st_select_lex::add_ftfunc_to_list(Item_func_match *func)
{
  return !func || ftfunc_list->push_back(func); // end of memory?
}


/**
  Invalidate by nulling out pointers to other st_select_lex_units and
  st_select_lexes.
*/
void st_select_lex::invalidate()
{
  next= NULL;
  prev= NULL;
  master= NULL;
  slave= NULL;
  link_next= NULL;
  link_prev= NULL;  
}


bool st_select_lex::set_braces(bool value)
{
  braces= value;
  return 0; 
}


bool st_select_lex::setup_ref_array(THD *thd)
{
  uint order_group_num= order_list.elements + group_list.elements;

  // find_order_in_list() may need some extra space, so multiply by two.
  order_group_num*= 2;

  // create_distinct_group() may need some extra space
  if (is_distinct())
  {
    uint bitcount= 0;
    Item *item;
    List_iterator<Item> li(item_list);
    while ((item= li++))
    {
      /*
        Same test as in create_distinct_group, when it pushes new items to the
        end of ref_pointer_array. An extra test for 'fixed' which, at this
        stage, will be true only for columns inserted for a '*' wildcard.
      */
      if (item->fixed &&
          item->type() == Item::FIELD_ITEM &&
          item->field_type() == MYSQL_TYPE_BIT)
        ++bitcount;
    }
    order_group_num+= bitcount;
  }

  /*
    We have to create array in prepared statement memory if it is
    prepared statement
  */
  Query_arena *arena= thd->stmt_arena;
  const uint n_elems= (n_sum_items +
                       n_child_sum_items +
                       item_list.elements +
                       select_n_having_items +
                       select_n_where_fields +
                       order_group_num) * 5;
  DBUG_PRINT("info", ("setup_ref_array this %p %4u : %4u %4u %4u %4u %4u %4u",
                      this,
                      n_elems, // :
                      n_sum_items,
                      n_child_sum_items,
                      item_list.elements,
                      select_n_having_items,
                      select_n_where_fields,
                      order_group_num));
  if (!ref_pointer_array.is_null())
  {
    /*
      We need to take 'n_sum_items' into account when allocating the array,
      and this may actually increase during the optimization phase due to
      MIN/MAX rewrite in Item_in_subselect::single_value_transformer.
      In the usual case we can reuse the array from the prepare phase.
      If we need a bigger array, we must allocate a new one.
     */
    if (ref_pointer_array.size() >= n_elems)
      return false;
  }
  /*
    ref_pointer_array could become bigger when a subquery gets transformed
    into a MIN/MAX subquery. Reallocate array in this case.
  */
  Item **array= static_cast<Item**>(arena->alloc(sizeof(Item*) * n_elems));
  if (array != NULL)
  {
    ref_pointer_array= Ref_ptr_array(array, n_elems);
    ref_ptrs= ref_ptr_array_slice(0);
  }
  return array == NULL;
}


void st_select_lex_unit::print(String *str, enum_query_type query_type)
{
  bool union_all= !union_distinct;
  for (SELECT_LEX *sl= first_select(); sl; sl= sl->next_select())
  {
    if (sl != first_select())
    {
      str->append(STRING_WITH_LEN(" union "));
      if (union_all)
	str->append(STRING_WITH_LEN("all "));
      else if (union_distinct == sl)
        union_all= TRUE;
    }
    if (sl->braces)
      str->append('(');
    sl->print(thd, str, query_type);
    if (sl->braces)
      str->append(')');
  }
  if (fake_select_lex)
  {
    if (fake_select_lex->order_list.elements)
    {
      str->append(STRING_WITH_LEN(" order by "));
      fake_select_lex->print_order(str,
        fake_select_lex->order_list.first,
        query_type);
    }
    fake_select_lex->print_limit(thd, str, query_type);
  }
  else if (saved_fake_select_lex)
    saved_fake_select_lex->print_limit(thd, str, query_type);
}


void st_select_lex::print_order(String *str,
                                ORDER *order,
                                enum_query_type query_type)
{
  for (; order; order= order->next)
  {
    (*order->item)->print_for_order(str, query_type, order->used_alias);
    if (order->direction == ORDER::ORDER_DESC)
      str->append(STRING_WITH_LEN(" desc"));
    if (order->next)
      str->append(',');
  }
}
 

void st_select_lex::print_limit(THD *thd,
                                String *str,
                                enum_query_type query_type)
{
  SELECT_LEX_UNIT *unit= master_unit();
  Item_subselect *item= unit->item;

  if (item && unit->global_parameters() == this)
  {
    Item_subselect::subs_type subs_type= item->substype();
    if (subs_type == Item_subselect::EXISTS_SUBS ||
        subs_type == Item_subselect::IN_SUBS ||
        subs_type == Item_subselect::ALL_SUBS)
      return;
  }
  if (explicit_limit)
  {
    str->append(STRING_WITH_LEN(" limit "));
    if (offset_limit)
    {
      offset_limit->print(str, query_type);
      str->append(',');
    }
    select_limit->print(str, query_type);
  }
}


/**
  @brief Print an index hint

  @details Prints out the USE|FORCE|IGNORE index hint.

  @param      thd         the current thread
  @param[out] str         appends the index hint here
  @param      hint        what the hint is (as string : "USE INDEX"|
                          "FORCE INDEX"|"IGNORE INDEX")
  @param      hint_length the length of the string in 'hint'
  @param      indexes     a list of index names for the hint
*/

void 
Index_hint::print(THD *thd, String *str)
{
  switch (type)
  {
    case INDEX_HINT_IGNORE: str->append(STRING_WITH_LEN("IGNORE INDEX")); break;
    case INDEX_HINT_USE:    str->append(STRING_WITH_LEN("USE INDEX")); break;
    case INDEX_HINT_FORCE:  str->append(STRING_WITH_LEN("FORCE INDEX")); break;
  }
  switch (clause)
  {
    case INDEX_HINT_MASK_ALL:
      break;
    case INDEX_HINT_MASK_JOIN:
      str->append(STRING_WITH_LEN(" FOR JOIN"));
      break;
    case INDEX_HINT_MASK_ORDER:
      str->append(STRING_WITH_LEN(" FOR ORDER BY"));
      break;
    case INDEX_HINT_MASK_GROUP:
      str->append(STRING_WITH_LEN(" FOR GROUP BY"));
      break;
  }

  str->append (STRING_WITH_LEN(" ("));
  if (key_name.length)
  {
    if (thd && !my_strnncoll(system_charset_info,
                             (const uchar *)key_name.str, key_name.length, 
                             (const uchar *)primary_key_name, 
                             strlen(primary_key_name)))
      str->append(primary_key_name);
    else
      append_identifier(thd, str, key_name.str, key_name.length);
  }
  str->append(')');
}


static void print_table_array(THD *thd, String *str, TABLE_LIST **table, 
                              TABLE_LIST **end, enum_query_type query_type)
{
  (*table)->print(thd, str, query_type);

  for (TABLE_LIST **tbl= table + 1; tbl < end; tbl++)
  {
    TABLE_LIST *curr= *tbl;
    // Print the join operator which relates this table to the previous one
    if (curr->outer_join)
    {
      /* MySQL converts right to left joins */
      str->append(STRING_WITH_LEN(" left join "));
    }
    else if (curr->straight)
      str->append(STRING_WITH_LEN(" straight_join "));
    else if (curr->sj_cond())
      str->append(STRING_WITH_LEN(" semi join "));
    else
      str->append(STRING_WITH_LEN(" join "));
    curr->print(thd, str, query_type);          // Print table

    // Print join condition
    Item *const cond=
      (curr->select_lex->join && curr->select_lex->join->is_optimized()) ?
      curr->join_cond_optim() : curr->join_cond();
    if (cond)
    {
      str->append(STRING_WITH_LEN(" on("));
      cond->print(str, query_type);
      str->append(')');
    }
  }
}


/**
  Print joins from the FROM clause.

  @param thd     thread handler
  @param str     string where table should be printed
  @param tables  list of tables in join
  @query_type    type of the query is being generated
*/

static void print_join(THD *thd,
                       String *str,
                       List<TABLE_LIST> *tables,
                       enum_query_type query_type)
{
  /* List is reversed => we should reverse it before using */
  List_iterator_fast<TABLE_LIST> ti(*tables);
  TABLE_LIST **table;

  /*
    If the QT_NO_DATA_EXPANSION flag is specified, we print the
    original table list, including constant tables that have been
    optimized away, as the constant tables may be referenced in the
    expression printed by Item_field::print() when this flag is given.
    Otherwise, only non-const tables are printed.

    Example:

    Original SQL:
    select * from (select 1) t

    Printed without QT_NO_DATA_EXPANSION:
    select '1' AS `1` from dual

    Printed with QT_NO_DATA_EXPANSION:
    select `t`.`1` from (select 1 AS `1`) `t`
  */
  const bool print_const_tables= (query_type & QT_NO_DATA_EXPANSION);
  size_t tables_to_print= 0;

  for (TABLE_LIST *t= ti++; t ; t= ti++)
    if (print_const_tables || !t->optimized_away)
      tables_to_print++;
  if (tables_to_print == 0)
  {
    str->append(STRING_WITH_LEN("dual"));
    return; // all tables were optimized away
  }
  ti.rewind();

  if (!(table= static_cast<TABLE_LIST **>(thd->alloc(sizeof(TABLE_LIST*) *
                                                     tables_to_print))))
    return;  // out of memory

  TABLE_LIST *tmp, **t= table + (tables_to_print - 1);
  while ((tmp= ti++))
  {
    if (tmp->optimized_away && !print_const_tables)
      continue;
    *t--= tmp;
  }

  /*
    If the first table is a semi-join nest, swap it with something that is
    not a semi-join nest. This is necessary because "A SEMIJOIN B" is not the
    same as "B SEMIJOIN A".
  */
  if ((*table)->sj_cond())
  {
    TABLE_LIST **end= table + tables_to_print;
    for (TABLE_LIST **t2= table; t2!=end; t2++)
    {
      if (!(*t2)->sj_cond())
      {
        TABLE_LIST *tmp= *t2;
        *t2= *table;
        *table= tmp;
        break;
      }
    }
  }
  DBUG_ASSERT(tables_to_print >= 1);
  print_table_array(thd, str, table, table + tables_to_print, query_type);
}


/**
  @returns whether a database is equal to the connection's default database
*/
bool db_is_default_db(const char *db, size_t db_len, const THD *thd)
{
  return thd != NULL && thd->db().str != NULL &&
    thd->db().length == db_len && !memcmp(db, thd->db().str, db_len);
}


/*.*
  Print table as it should be in join list.

  @param str   string where table should be printed
*/

void TABLE_LIST::print(THD *thd, String *str, enum_query_type query_type) const
{
  if (nested_join)
  {
    str->append('(');
    print_join(thd, str, &nested_join->join_list, query_type);
    str->append(')');
  }
  else
  {
    const char *cmp_name;                         // Name to compare with alias
    if (view_name.str)
    {
      // A view
      if (!(query_type & QT_NO_DB) &&
          !((query_type & QT_NO_DEFAULT_DB) &&
            db_is_default_db(view_db.str, view_db.length, thd)))
      {
        append_identifier(thd, str, view_db.str, view_db.length);
        str->append('.');
      }
      append_identifier(thd, str, view_name.str, view_name.length);
      cmp_name= view_name.str;
    }
    else if (is_derived() && !is_merged())
    {
      // A derived table that is materialized or without specified algorithm
      if (!(query_type & QT_DERIVED_TABLE_ONLY_ALIAS))
      {
        str->append('(');
        derived->print(str, query_type);
        str->append(')');
      }
      cmp_name= "";                               // Force printing of alias
    }
    else
    {
      // A normal table

      if (!(query_type & QT_NO_DB) &&
          !((query_type & QT_NO_DEFAULT_DB) &&
            db_is_default_db(db, db_length, thd)))
      {
        append_identifier(thd, str, db, db_length);
        str->append('.');
      }
      if (schema_table)
      {
        append_identifier(thd, str, schema_table_name,
                          strlen(schema_table_name));
        cmp_name= schema_table_name;
      }
      else
      {
        append_identifier(thd, str, table_name, table_name_length);
        cmp_name= table_name;
      }
      if (partition_names && partition_names->elements)
      {
        int i, num_parts= partition_names->elements;
        List_iterator<String> name_it(*(partition_names));
        str->append(STRING_WITH_LEN(" PARTITION ("));
        for (i= 1; i <= num_parts; i++)
        {
          String *name= name_it++;
          append_identifier(thd, str, name->c_ptr(), name->length());
          if (i != num_parts)
            str->append(',');
        }
        str->append(')');
      }
    }
    if (my_strcasecmp(table_alias_charset, cmp_name, alias))
    {
      char t_alias_buff[MAX_ALIAS_NAME];
      const char *t_alias= alias;

      str->append(' ');
      if (lower_case_table_names== 1)
      {
        if (alias && alias[0])
        {
          my_stpcpy(t_alias_buff, alias);
          my_casedn_str(files_charset_info, t_alias_buff);
          t_alias= t_alias_buff;
        }
      }

      append_identifier(thd, str, t_alias, strlen(t_alias));
    }

    if (index_hints)
    {
      List_iterator<Index_hint> it(*index_hints);
      Index_hint *hint;

      while ((hint= it++))
      {
        str->append (STRING_WITH_LEN(" "));
        hint->print (thd, str);
      }
    }
  }
}


void st_select_lex::print(THD *thd, String *str, enum_query_type query_type)
{
  /* QQ: thd may not be set for sub queries, but this should be fixed */
  if (!thd)
    thd= current_thd;

  if (query_type & QT_SHOW_SELECT_NUMBER)
  {
    /* it makes EXPLAIN's "id" column understandable */
    str->append("/* select#");
    if (unlikely(select_number >= INT_MAX))
      str->append("fake");
    else
      str->append_ulonglong(select_number);
    str->append(" */ select ");
  }
  else
    str->append(STRING_WITH_LEN("select "));

  if (thd->lex->opt_hints_global)
  {
    char buff[NAME_LEN];
    String hint_str(buff, sizeof(buff), system_charset_info);
    hint_str.length(0);

    if (select_number == 1)
    {
      if (opt_hints_qb)
        opt_hints_qb->append_qb_hint(thd, &hint_str);
      thd->lex->opt_hints_global->print(thd, &hint_str, query_type);
    }
    else if (opt_hints_qb)
      opt_hints_qb->append_qb_hint(thd, &hint_str);

    if (hint_str.length() > 0)
    {
      str->append(STRING_WITH_LEN("/*+ "));
      str->append(hint_str.ptr(), hint_str.length());
      str->append(STRING_WITH_LEN("*/ "));
    }
  }

  if (thd->is_error())
  {
    /*
      It is possible that this query block had an optimization error, but the
      caller didn't notice (caller evaluted this as a subquery and
      Item::val*() don't have an error status). In this case the query block
      may be broken and printing it may crash.
    */
    str->append(STRING_WITH_LEN("had some error"));
    return;
  }
  /*
   In order to provide info for EXPLAIN FOR CONNECTION units shouldn't
   be completely cleaned till the end of the query. This is valid only for
   explainable commands.
  */
  DBUG_ASSERT(!(master_unit()->cleaned == SELECT_LEX_UNIT::UC_CLEAN &&
                is_explainable_query(thd->lex->sql_command)));

  /* First add options */
  if (active_options() & SELECT_STRAIGHT_JOIN)
    str->append(STRING_WITH_LEN("straight_join "));
  if (active_options() & SELECT_HIGH_PRIORITY)
    str->append(STRING_WITH_LEN("high_priority "));
  if (active_options() & SELECT_DISTINCT)
    str->append(STRING_WITH_LEN("distinct "));
  if (active_options() & SELECT_SMALL_RESULT)
    str->append(STRING_WITH_LEN("sql_small_result "));
  if (active_options() & SELECT_BIG_RESULT)
    str->append(STRING_WITH_LEN("sql_big_result "));
  if (active_options() & OPTION_BUFFER_RESULT)
    str->append(STRING_WITH_LEN("sql_buffer_result "));
  if (active_options() & OPTION_FOUND_ROWS)
    str->append(STRING_WITH_LEN("sql_calc_found_rows "));
  switch (sql_cache)
  {
    case SQL_NO_CACHE:
      str->append(STRING_WITH_LEN("sql_no_cache "));
      break;
    case SQL_CACHE:
      str->append(STRING_WITH_LEN("sql_cache "));
      break;
    case SQL_CACHE_UNSPECIFIED:
      break;
    default:
      DBUG_ASSERT(0);
  }

  //Item List
  bool first= 1;
  List_iterator_fast<Item> it(item_list);
  Item *item;
  while ((item= it++))
  {
    if (first)
      first= 0;
    else
      str->append(',');

    if (master_unit()->item && item->item_name.is_autogenerated())
    {
      /*
        Do not print auto-generated aliases in subqueries. It has no purpose
        in a view definition or other contexts where the query is printed.
      */
      item->print(str, query_type);
    }
    else
      item->print_item_w_name(str, query_type);
    /** @note that 'INTO variable' clauses are not printed */
  }

  /*
    from clause
    TODO: support USING/FORCE/IGNORE index
  */
  if (table_list.elements)
  {
    str->append(STRING_WITH_LEN(" from "));
    /* go through join tree */
    print_join(thd, str, &top_join_list, query_type);
  }
  else if (m_where_cond)
  {
    /*
      "SELECT 1 FROM DUAL WHERE 2" should not be printed as 
      "SELECT 1 WHERE 2": the 1st syntax is valid, but the 2nd is not.
    */
    str->append(STRING_WITH_LEN(" from DUAL "));
  }

  // Where
  Item *const cur_where=
    (join && join->is_optimized()) ? join->where_cond : m_where_cond;

  if (cur_where || cond_value != Item::COND_UNDEF)
  {
    str->append(STRING_WITH_LEN(" where "));
    if (cur_where)
      cur_where->print(str, query_type);
    else
      str->append(cond_value != Item::COND_FALSE ? "1" : "0");
  }

  // group by & olap
  if (group_list.elements)
  {
    str->append(STRING_WITH_LEN(" group by "));
    print_order(str, group_list.first, query_type);
    switch (olap)
    {
      case CUBE_TYPE:
	str->append(STRING_WITH_LEN(" with cube"));
	break;
      case ROLLUP_TYPE:
	str->append(STRING_WITH_LEN(" with rollup"));
	break;
      default:
	;  //satisfy compiler
    }
  }

  // having
  Item *const cur_having= (join && join->having_for_explain != (Item*)1) ?
    join->having_for_explain : m_having_cond;

  if (cur_having || having_value != Item::COND_UNDEF)
  {
    str->append(STRING_WITH_LEN(" having "));
    if (cur_having)
      cur_having->print(str, query_type);
    else
      str->append(having_value != Item::COND_FALSE ? "1" : "0");
  }

  if (order_list.elements)
  {
    str->append(STRING_WITH_LEN(" order by "));
    print_order(str, order_list.first, query_type);
  }

  // limit
  print_limit(thd, str, query_type);

  // PROCEDURE unsupported here
}


Item::enum_walk get_walk_flags(const Select_lex_visitor *visitor)
{
  if (visitor->visits_in_prefix_order())
    return Item::WALK_SUBQUERY_PREFIX;
  else
    return Item::WALK_SUBQUERY_POSTFIX;
}


bool walk_item(Item *item, Select_lex_visitor *visitor)
{
  if (item == NULL)
    return false;
  return item->walk(&Item::visitor_processor, get_walk_flags(visitor),
                    pointer_cast<uchar*>(visitor));
}


static bool accept_for_order(SQL_I_List<ORDER> orders,
                             Select_lex_visitor *visitor)
{
  if (orders.elements == 0)
    return false;

  for (ORDER *order= orders.first; order != NULL; order= order->next)
    if (walk_item(*order->item, visitor))
      return true;
  return false;
}

bool st_select_lex_unit::accept(Select_lex_visitor *visitor)
{
  SELECT_LEX *end= NULL;
  for (SELECT_LEX *sl= first_select(); sl != end; sl= sl->next_select())
    if (sl->accept(visitor))
      return true;

  if (fake_select_lex &&
      accept_for_order(fake_select_lex->order_list, visitor))
    return true;

  return visitor->visit(this);
}


static bool accept_for_join(List<TABLE_LIST> *tables,
                            Select_lex_visitor *visitor)
{
  List_iterator<TABLE_LIST> ti(*tables);
  TABLE_LIST *end= NULL;
  for (TABLE_LIST *t= ti++; t != end; t= ti++)
  {
    if (t->nested_join && accept_for_join(&t->nested_join->join_list, visitor))
      return true;
    else if (t->is_derived())
      t->derived_unit()->accept(visitor);
    if (walk_item(t->join_cond(), visitor))
        return true;
  }
  return false;
}


bool st_select_lex::accept(Select_lex_visitor *visitor)
{
  // Select clause
  List_iterator<Item> it(item_list);
  Item *end= NULL;
  for (Item *item= it++; item != end; item= it++)
    if (walk_item(item, visitor))
      return true;

  // From clause
  if (table_list.elements != 0 && accept_for_join(join_list, visitor))
      return true;

  // Where clause
  Item *where_condition= join != NULL ? join->where_cond : m_where_cond;
  if (where_condition != NULL && walk_item(where_condition, visitor))
    return true;

  // Group by and olap clauses
  if (accept_for_order(group_list, visitor))
    return true;

  // Having clause
  Item *having_condition=
    join != NULL ? join->having_for_explain : m_having_cond;
  if (walk_item(having_condition, visitor))
    return true;

  // Order clause
  if (accept_for_order(order_list, visitor))
    return true;

  // Limit clause
  if (explicit_limit)
    if (walk_item(offset_limit, visitor) || walk_item(select_limit, visitor))
      return true;

  return visitor->visit(this);
}


/**
  @brief Restore the LEX and THD in case of a parse error.

  This is a clean up call that is invoked by the Bison generated
  parser before returning an error from MYSQLparse. If your
  semantic actions manipulate with the global thread state (which
  is a very bad practice and should not normally be employed) and
  need a clean-up in case of error, and you can not use %destructor
  rule in the grammar file itself, this function should be used
  to implement the clean up.
*/

void LEX::cleanup_lex_after_parse_error(THD *thd)
{
  sp_head *sp= thd->lex->sphead;

  if (sp)
  {
    sp->m_parser_data.finish_parsing_sp_body(thd);
    //  Do not delete sp_head if is invoked in the context of sp execution.
    if (thd->sp_runtime_ctx == NULL)
    {
      delete sp;
      thd->lex->sphead= NULL;
    }
  }
}

/*
  Initialize (or reset) Query_tables_list object.

  SYNOPSIS
    reset_query_tables_list()
      init  TRUE  - we should perform full initialization of object with
                    allocating needed memory
            FALSE - object is already initialized so we should only reset
                    its state so it can be used for parsing/processing
                    of new statement

  DESCRIPTION
    This method initializes Query_tables_list so it can be used as part
    of LEX object for parsing/processing of statement. One can also use
    this method to reset state of already initialized Query_tables_list
    so it can be used for processing of new statement.
*/

void Query_tables_list::reset_query_tables_list(bool init)
{
  sql_command= SQLCOM_END;
  if (!init && query_tables)
  {
    TABLE_LIST *table= query_tables;
    for (;;)
    {
      delete table->view_query();
      if (query_tables_last == &table->next_global ||
          !(table= table->next_global))
        break;
    }
  }
  query_tables= 0;
  query_tables_last= &query_tables;
  query_tables_own_last= 0;
  if (init)
  {
    /*
      We delay real initialization of hash (and therefore related
      memory allocation) until first insertion into this hash.
    */
    my_hash_clear(&sroutines);
  }
  else if (sroutines.records)
  {
    /* Non-zero sroutines.records means that hash was initialized. */
    my_hash_reset(&sroutines);
  }
  sroutines_list.empty();
  sroutines_list_own_last= sroutines_list.next;
  sroutines_list_own_elements= 0;
  binlog_stmt_flags= 0;
  stmt_accessed_table_flag= 0;
  lock_tables_state= LTS_NOT_LOCKED;
  table_count= 0;
  using_match= FALSE;
}


/*
  Destroy Query_tables_list object with freeing all resources used by it.

  SYNOPSIS
    destroy_query_tables_list()
*/

void Query_tables_list::destroy_query_tables_list()
{
  my_hash_free(&sroutines);
}


/*
  Initialize LEX object.

  SYNOPSIS
    LEX::LEX()

  NOTE
    LEX object initialized with this constructor can be used as part of
    THD object for which one can safely call open_tables(), lock_tables()
    and close_thread_tables() functions. But it is not yet ready for
    statement parsing. On should use lex_start() function to prepare LEX
    for this.
*/

LEX::LEX()
  :result(0), thd(NULL), opt_hints_global(NULL),
   // Quite unlikely to overflow initial allocation, so no instrumentation.
   plugins(PSI_NOT_INSTRUMENTED),
   insert_update_values_map(NULL),
   option_type(OPT_DEFAULT),
   sphead(NULL),
   is_set_password_sql(false),
   // Initialize here to avoid uninitialized variable warnings.
   contains_plaintext_password(false),
   keep_diagnostics(DA_KEEP_UNSPECIFIED),
   is_lex_started(0),
  in_update_value_clause(false)
{
  reset_query_tables_list(TRUE);
}


/**
  check if command can use VIEW with MERGE algorithm (for top VIEWs)

  @details
    Only listed here commands can use merge algorithm in top level
    SELECT_LEX (for subqueries will be used merge algorithm if
    LEX::can_not_use_merged() is not TRUE).

  @todo - Add SET as a command that can use merged views. Due to how
          all uses would be embedded in subqueries, this test is worthless
          for the SET command anyway.

  @returns true if command can use merged VIEWs, false otherwise
*/

bool LEX::can_use_merged()
{
  switch (sql_command)
  {
  case SQLCOM_SELECT:
  case SQLCOM_CREATE_TABLE:
  case SQLCOM_UPDATE:
  case SQLCOM_UPDATE_MULTI:
  case SQLCOM_DELETE:
  case SQLCOM_DELETE_MULTI:
  case SQLCOM_INSERT:
  case SQLCOM_INSERT_SELECT:
  case SQLCOM_REPLACE:
  case SQLCOM_REPLACE_SELECT:
  case SQLCOM_LOAD:
    return TRUE;
  default:
    return FALSE;
  }
}

/**
  Check if command can't use merged views in any part of command

  @details
    Temporary table algorithm will be used on all SELECT levels for queries
    listed here (see also LEX::can_use_merged()).

  @returns true if command cannot use merged view, false otherwise
*/

bool LEX::can_not_use_merged()
{
  switch (sql_command)
  {
  case SQLCOM_CREATE_VIEW:
  case SQLCOM_SHOW_CREATE:
  /*
    SQLCOM_SHOW_FIELDS is necessary to make 
    information schema tables working correctly with views.
    see get_schema_tables_result function
  */
  case SQLCOM_SHOW_FIELDS:
    return TRUE;
  default:
    return FALSE;
  }
}

/*
  Detect that we need only table structure of derived table/view

  SYNOPSIS
    only_view_structure()

  RETURN
    TRUE yes, we need only structure
    FALSE no, we need data
*/

bool LEX::only_view_structure()
{
  switch (sql_command) {
  case SQLCOM_SHOW_CREATE:
  case SQLCOM_SHOW_TABLES:
  case SQLCOM_SHOW_FIELDS:
  case SQLCOM_REVOKE_ALL:
  case SQLCOM_REVOKE:
  case SQLCOM_GRANT:
  case SQLCOM_CREATE_VIEW:
    return TRUE;
  default:
    return FALSE;
  }
}


/*
  Should Items_ident be printed correctly

  SYNOPSIS
    need_correct_ident()

  RETURN
    TRUE yes, we need only structure
    FALSE no, we need data
*/


bool LEX::need_correct_ident()
{
  switch(sql_command)
  {
  case SQLCOM_SHOW_CREATE:
  case SQLCOM_SHOW_TABLES:
  case SQLCOM_CREATE_VIEW:
    return TRUE;
  default:
    return FALSE;
  }
}


/**
  This method should be called only during parsing.
  It is aware of compound statements (stored routine bodies)
  and will initialize the destination with the default
  database of the stored routine, rather than the default
  database of the connection it is parsed in.
  E.g. if one has no current database selected, or current database 
  set to 'bar' and then issues:

  CREATE PROCEDURE foo.p1() BEGIN SELECT * FROM t1 END//

  t1 is meant to refer to foo.t1, not to bar.t1.

  This method is needed to support this rule.

  @return TRUE in case of error (parsing should be aborted, FALSE in
  case of success
*/

bool
LEX::copy_db_to(char **p_db, size_t *p_db_length) const
{
  if (sphead)
  {
    DBUG_ASSERT(sphead->m_db.str && sphead->m_db.length);
    /*
      It is safe to assign the string by-pointer, both sphead and
      its statements reside in the same memory root.
    */
    *p_db= sphead->m_db.str;
    if (p_db_length)
      *p_db_length= sphead->m_db.length;
    return FALSE;
  }
  return thd->copy_db_to(p_db, p_db_length);
}


/**
  Initialize offset and limit counters.

  @param sl SELECT_LEX to get offset and limit from.
*/
void st_select_lex_unit::set_limit(st_select_lex *sl)
{
  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());

  offset_limit_cnt= sl->get_offset();
  select_limit_cnt= sl->get_limit();
  if (select_limit_cnt + offset_limit_cnt >= select_limit_cnt)
    select_limit_cnt+= offset_limit_cnt;
  else
    select_limit_cnt= HA_POS_ERROR;
}


/**
  Decide if a temporary table is needed for the UNION.

  @retval true  A temporary table is needed.
  @retval false A temporary table is not needed.
 */
bool st_select_lex_unit::union_needs_tmp_table()
{
  return union_distinct != NULL ||
    global_parameters()->order_list.elements != 0 ||
    thd->lex->sql_command == SQLCOM_INSERT_SELECT ||
    thd->lex->sql_command == SQLCOM_REPLACE_SELECT;
}  


/**
  Include a query expression below a query block.

  @param lex:   Containing LEX object
  @param outer: The query block that this query expression is included below.
*/
void st_select_lex_unit::include_down(LEX *lex, st_select_lex *outer)
{
  if ((next= outer->slave))
    next->prev= &next;
  prev= &outer->slave;
  outer->slave= this;
  master= outer;

  renumber_selects(lex);
}


/**
  Include a complete chain of query expressions below a query block.

  @param lex:   Containing LEX object
  @param outer: The query block that the chain is included below.

  @note
    "this" is pointer to the first query expression in the chain.
*/
void st_select_lex_unit::include_chain(LEX *lex, st_select_lex *outer)
{
  st_select_lex_unit *last_unit= this; // Initialization needed for gcc
  for (st_select_lex_unit *unit= this; unit != NULL; unit= unit->next)
  {
    unit->master= outer; // Link to the outer query block
    unit->renumber_selects(lex);
    last_unit= unit;
  }

  if ((last_unit->next= outer->slave))
    last_unit->next->prev= &last_unit->next;
  prev= &outer->slave;
  outer->slave= this;
}


/**
  Return true if query expression can be merged into an outer query.
  Being mergeable also means that derived table/view is updatable.

  A view/derived table is not mergeable if it is one of the following:
   - A union (implementation restriction).
   - An aggregated query, or has HAVING, or has DISTINCT
     (A general aggregated query cannot be merged with a non-aggregated one).
   - A table-less query (unimportant special case).
   - A query with a LIMIT (limit applies to subquery, so the implementation
     strategy is to materialize this subquery, including row count constraint).
   - A query that modifies variables (When variables are modified, try to
     preserve the original structure of the query. This is less likely to cause
     changes in variable assignment order).

  A view/derived table will also not be merged if it contains subqueries
  in the SELECT list that depend on columns from itself.
  Merging such objects is possible, but we assume they are made derived
  tables because the user wants them to be materialized, for performance
  reasons.

  One possible case is a derived table with dependent subqueries in the select
  list, used as the inner table of a left outer join. Such tables will always
  be read as many times as there are qualifying rows in the outer table,
  and the select list subqueries are evaluated for each row combination.
  The select list subqueries are evaluated the same number of times also with
  join buffering enabled, even though the table then only will be read once.

  When materialization hints are implemented, this decision may be reconsidered.
*/

bool st_select_lex_unit::is_mergeable() const
{
  if (is_union())
    return false;

  SELECT_LEX *const select= first_select();
  Item *item;
  List_iterator<Item> it(select->fields_list);
  while ((item= it++))
  {
    if (item->has_subquery() && item->used_tables())
      return false;
  }
  return !select->is_grouped() &&
         !select->having_cond() &&
         !select->is_distinct() &&
         select->table_list.elements > 0 &&
         !select->has_limit() &&
         thd->lex->set_var_list.elements == 0;
}

/**
  Renumber contained select_lex objects.

  @param  lex   Containing LEX object
*/

void st_select_lex_unit::renumber_selects(LEX *lex)
{
  for (SELECT_LEX *select= first_select(); select; select= select->next_select())
    select->renumber(lex);
  if (fake_select_lex)
    fake_select_lex->renumber(lex);
}

/**
  @brief Set the initial purpose of this TABLE_LIST object in the list of used
    tables.

  We need to track this information on table-by-table basis, since when this
  table becomes an element of the pre-locked list, it's impossible to identify
  which SQL sub-statement it has been originally used in.

  E.g.:

  User request:                 SELECT * FROM t1 WHERE f1();
  FUNCTION f1():                DELETE FROM t2; RETURN 1;
  BEFORE DELETE trigger on t2:  INSERT INTO t3 VALUES (old.a);

  For this user request, the pre-locked list will contain t1, t2, t3
  table elements, each needed for different DML.

  The trigger event map is updated to reflect INSERT, UPDATE, DELETE,
  REPLACE, LOAD DATA, CREATE TABLE .. SELECT, CREATE TABLE ..
  REPLACE SELECT statements, and additionally ON DUPLICATE KEY UPDATE
  clause.
*/

void LEX::set_trg_event_type_for_tables()
{
  uint8 new_trg_event_map= 0;

  /*
    Some auxiliary operations
    (e.g. GRANT processing) create TABLE_LIST instances outside
    the parser. Additionally, some commands (e.g. OPTIMIZE) change
    the lock type for a table only after parsing is done. Luckily,
    these do not fire triggers and do not need to pre-load them.
    For these TABLE_LISTs set_trg_event_type is never called, and
    trg_event_map is always empty. That means that the pre-locking
    algorithm will ignore triggers defined on these tables, if
    any, and the execution will either fail with an assert in
    sql_trigger.cc or with an error that a used table was not
    pre-locked, in case of a production build.

    TODO: this usage pattern creates unnecessary module dependencies
    and should be rewritten to go through the parser.
    Table list instances created outside the parser in most cases
    refer to mysql.* system tables. It is not allowed to have
    a trigger on a system table, but keeping track of
    initialization provides extra safety in case this limitation
    is circumvented.
  */

  switch (sql_command) {
  case SQLCOM_LOCK_TABLES:
  /*
    On a LOCK TABLE, all triggers must be pre-loaded for this TABLE_LIST
    when opening an associated TABLE.
  */
    new_trg_event_map= static_cast<uint8>
                        (1 << static_cast<int>(TRG_EVENT_INSERT)) |
                      static_cast<uint8>
                        (1 << static_cast<int>(TRG_EVENT_UPDATE)) |
                      static_cast<uint8>
                        (1 << static_cast<int>(TRG_EVENT_DELETE));
    break;
  /*
    Basic INSERT. If there is an additional ON DUPLIATE KEY UPDATE
    clause, it will be handled later in this method.
  */
  case SQLCOM_INSERT:                           /* fall through */
  case SQLCOM_INSERT_SELECT:
  /*
    LOAD DATA ... INFILE is expected to fire BEFORE/AFTER INSERT
    triggers.
    If the statement also has REPLACE clause, it will be
    handled later in this method.
  */
  case SQLCOM_LOAD:                             /* fall through */
  /*
    REPLACE is semantically equivalent to INSERT. In case
    of a primary or unique key conflict, it deletes the old
    record and inserts a new one. So we also may need to
    fire ON DELETE triggers. This functionality is handled
    later in this method.
  */
  case SQLCOM_REPLACE:                          /* fall through */
  case SQLCOM_REPLACE_SELECT:
  /*
    CREATE TABLE ... SELECT defaults to INSERT if the table or
    view already exists. REPLACE option of CREATE TABLE ...
    REPLACE SELECT is handled later in this method.
  */
  case SQLCOM_CREATE_TABLE:
    new_trg_event_map|= static_cast<uint8>
                          (1 << static_cast<int>(TRG_EVENT_INSERT));
    break;
  /* Basic update and multi-update */
  case SQLCOM_UPDATE:                           /* fall through */
  case SQLCOM_UPDATE_MULTI:
    new_trg_event_map|= static_cast<uint8>
                          (1 << static_cast<int>(TRG_EVENT_UPDATE));
    break;
  /* Basic delete and multi-delete */
  case SQLCOM_DELETE:                           /* fall through */
  case SQLCOM_DELETE_MULTI:
    new_trg_event_map|= static_cast<uint8>
                          (1 << static_cast<int>(TRG_EVENT_DELETE));
    break;
  default:
    break;
  }

  switch (duplicates) {
  case DUP_UPDATE:
    new_trg_event_map|= static_cast<uint8>
                          (1 << static_cast<int>(TRG_EVENT_UPDATE));
    break;
  case DUP_REPLACE:
    new_trg_event_map|= static_cast<uint8>
                          (1 << static_cast<int>(TRG_EVENT_DELETE));
    break;
  case DUP_ERROR:
  default:
    break;
  }


  /*
    Do not iterate over sub-selects, only the tables in the outermost
    SELECT_LEX can be modified, if any.
  */
  TABLE_LIST *tables= select_lex ? select_lex->get_table_list() : NULL;
  while (tables)
  {
    /*
      This is a fast check to filter out statements that do
      not change data, or tables  on the right side, in case of
      INSERT .. SELECT, CREATE TABLE .. SELECT and so on.
      Here we also filter out OPTIMIZE statement and non-updateable
      views, for which lock_type is TL_UNLOCK or TL_READ after
      parsing.
    */
    if (static_cast<int>(tables->lock_type) >=
        static_cast<int>(TL_WRITE_ALLOW_WRITE))
      tables->trg_event_map= new_trg_event_map;
    tables= tables->next_local;
  }
}


/*
  Unlink the first table from the global table list and the first table from
  outer select (lex->select_lex) local list

  SYNOPSIS
    unlink_first_table()
    link_to_local	Set to 1 if caller should link this table to local list

  NOTES
    We assume that first tables in both lists is the same table or the local
    list is empty.

  RETURN
    0	If 'query_tables' == 0
    unlinked table
      In this case link_to_local is set.

*/
TABLE_LIST *LEX::unlink_first_table(bool *link_to_local)
{
  TABLE_LIST *first;
  if ((first= query_tables))
  {
    /*
      Exclude from global table list
    */
    if ((query_tables= query_tables->next_global))
      query_tables->prev_global= &query_tables;
    else
      query_tables_last= &query_tables;
    first->next_global= 0;

    if (query_tables_own_last == &first->next_global)
      query_tables_own_last= &query_tables;

    /*
      and from local list if it is not empty
    */
    if ((*link_to_local= MY_TEST(select_lex->get_table_list())))
    {
      select_lex->context.table_list= 
        select_lex->context.first_name_resolution_table= first->next_local;
      select_lex->table_list.first= first->next_local;
      select_lex->table_list.elements--; //safety
      first->next_local= 0;
      /*
        Ensure that the global list has the same first table as the local
        list.
      */
      first_lists_tables_same();
    }
  }
  return first;
}


/*
  Bring first local table of first most outer select to first place in global
  table list

  SYNOPSYS
     LEX::first_lists_tables_same()

  NOTES
    In many cases (for example, usual INSERT/DELETE/...) the first table of
    main SELECT_LEX have special meaning => check that it is the first table
    in global list and re-link to be first in the global list if it is
    necessary.  We need such re-linking only for queries with sub-queries in
    the select list, as only in this case tables of sub-queries will go to
    the global list first.
*/

void LEX::first_lists_tables_same()
{
  TABLE_LIST *first_table= select_lex->get_table_list();
  if (query_tables != first_table && first_table != 0)
  {
    TABLE_LIST *next;
    if (query_tables_last == &first_table->next_global)
      query_tables_last= first_table->prev_global;

    if ((next= *first_table->prev_global= first_table->next_global))
      next->prev_global= first_table->prev_global;
    /* include in new place */
    first_table->next_global= query_tables;
    /*
       We are sure that query_tables is not 0, because first_table was not
       first table in the global list => we can use
       query_tables->prev_global without check of query_tables
    */
    query_tables->prev_global= &first_table->next_global;
    first_table->prev_global= &query_tables;
    query_tables= first_table;
  }
}


/*
  Link table back that was unlinked with unlink_first_table()

  SYNOPSIS
    link_first_table_back()
    link_to_local	do we need link this table to local

  RETURN
    global list
*/

void LEX::link_first_table_back(TABLE_LIST *first,
				   bool link_to_local)
{
  if (first)
  {
    if ((first->next_global= query_tables))
      query_tables->prev_global= &first->next_global;
    else
      query_tables_last= &first->next_global;

    if (query_tables_own_last == &query_tables)
      query_tables_own_last= &first->next_global;

    query_tables= first;

    if (link_to_local)
    {
      first->next_local= select_lex->table_list.first;
      select_lex->context.table_list= first;
      select_lex->table_list.first= first;
      select_lex->table_list.elements++; //safety
    }
  }
}



/*
  cleanup lex for case when we open table by table for processing

  SYNOPSIS
    LEX::cleanup_after_one_table_open()

  NOTE
    This method is mostly responsible for cleaning up of selects lists and
    derived tables state. To rollback changes in Query_tables_list one has
    to call Query_tables_list::reset_query_tables_list(FALSE).
*/

void LEX::cleanup_after_one_table_open()
{
  /*
    thd->lex->derived_tables & additional units may be set if we open
    a view. It is necessary to clear thd->lex->derived_tables flag
    to prevent processing of derived tables during next open_and_lock_tables
    if next table is a real table and cleanup & remove underlying units
    NOTE: all units will be connected to thd->lex->select_lex, because we
    have not UNION on most upper level.
    */
  if (all_selects_list != select_lex)
  {
    derived_tables= 0;
    /* cleunup underlying units (units of VIEW) */
    for (SELECT_LEX_UNIT *un= select_lex->first_inner_unit();
         un;
         un= un->next_unit())
      un->cleanup(true);
    /* reduce all selects list to default state */
    all_selects_list= select_lex;
    /* remove underlying units (units of VIEW) subtree */
    select_lex->cut_subtree();
  }
}


/*
  Save current state of Query_tables_list for this LEX, and prepare it
  for processing of new statemnt.

  SYNOPSIS
    reset_n_backup_query_tables_list()
      backup  Pointer to Query_tables_list instance to be used for backup
*/

void LEX::reset_n_backup_query_tables_list(Query_tables_list *backup)
{
  backup->set_query_tables_list(this);
  /*
    We have to perform full initialization here since otherwise we
    will damage backed up state.
  */
  this->reset_query_tables_list(TRUE);
}


/*
  Restore state of Query_tables_list for this LEX from backup.

  SYNOPSIS
    restore_backup_query_tables_list()
      backup  Pointer to Query_tables_list instance used for backup
*/

void LEX::restore_backup_query_tables_list(Query_tables_list *backup)
{
  this->destroy_query_tables_list();
  this->set_query_tables_list(backup);
}


/*
  Checks for usage of routines and/or tables in a parsed statement

  SYNOPSIS
    LEX:table_or_sp_used()

  RETURN
    FALSE  No routines and tables used
    TRUE   Either or both routines and tables are used.
*/

bool LEX::table_or_sp_used()
{
  DBUG_ENTER("table_or_sp_used");

  if (sroutines.records || query_tables)
    DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}


void
st_select_lex::fix_prepare_information_for_order(THD *thd,
                                                 SQL_I_List<ORDER> *list,
                                                 Group_list_ptrs **list_ptrs)
{
  Group_list_ptrs *p= *list_ptrs;
  if (!p)
  {
    void *mem= thd->stmt_arena->alloc(sizeof(Group_list_ptrs));
    *list_ptrs= p= new (mem) Group_list_ptrs(thd->stmt_arena->mem_root);
  }
  p->reserve(list->elements);
  for (ORDER *order= list->first; order; order= order->next)
    p->push_back(order);
}


/*
  Saves the chain of ORDER::next in group_list and order_list, in
  case the list is modified by remove_const().

  @param thd          thread handler
*/

void st_select_lex::fix_prepare_information(THD *thd)
{
  if (!first_execution)
    return;
  first_execution= false;
  if (thd->stmt_arena->is_conventional())
    return;
  if (group_list.first)
    fix_prepare_information_for_order(thd, &group_list, &group_list_ptrs);
  if (order_list.first)
    fix_prepare_information_for_order(thd, &order_list, &order_list_ptrs);
}


/*
  There are st_select_lex::add_table_to_list &
  st_select_lex::set_lock_for_tables are in sql_parse.cc

  st_select_lex::print is in sql_select.cc

  st_select_lex_unit::prepare, st_select_lex_unit::exec,
  st_select_lex_unit::cleanup, st_select_lex_unit::reinit_exec_mechanism,
  st_select_lex_unit::change_query_result
  are in sql_union.cc
*/


st_select_lex::type_enum st_select_lex::type()
{
  if (master_unit()->fake_select_lex == this)
    return SLT_UNION_RESULT;
  else if (!master_unit()->outer_select() &&
           master_unit()->first_select() == this) 
  {
    if (first_inner_unit() || next_select())
      return SLT_PRIMARY;
    else
      return SLT_SIMPLE;
  }
  else if (this == master_unit()->first_select())
  {
    if (linkage == DERIVED_TABLE_TYPE) 
      return SLT_DERIVED;
    else
      return SLT_SUBQUERY;
  }
  else
    return SLT_UNION;
}



/**
  Add this query block below the specified query expression.

  @param lex   Containing LEX object
  @param outer Query expression that query block is added to.

  @note that this query block can never have any underlying query expressions,
        hence it is not necessary to e.g. renumber those, like e.g.
        st_select_lex_unit::include_down() does.
*/
void st_select_lex::include_down(LEX *lex, st_select_lex_unit *outer)
{
  DBUG_ASSERT(slave == NULL);

  if ((next= outer->slave))
    next->prev= &next;
  prev= &outer->slave;
  outer->slave= this;
  master= outer;

  select_number= ++lex->select_number;

  nest_level= outer_select() == NULL ? 0 : outer_select()->nest_level + 1;
}


/**
  Add this query block after the specified query block.

  @param lex    Containing LEX object
  @param before Query block that this object is added after.
*/
void st_select_lex::include_neighbour(LEX *lex, st_select_lex *before)
{
  if ((next= before->next))
    next->prev= &next;
  prev= &before->next;
  before->next= this;
  master= before->master;

  select_number= ++lex->select_number;
  nest_level= before->nest_level;
}


/**
  Include query block within the supplied unit.

  Do not link the query block into the global chain of query blocks.

  This function is exclusive for st_select_lex_unit::add_fake_select_lex() -
  use it with caution.

  @param  outer - Query expression this node is included below.
  @param  ref - Handle to the caller's pointer to this node.
*/
void st_select_lex::include_standalone(st_select_lex_unit *outer,
                                       st_select_lex **ref)
{
  next= NULL;
  prev= ref;
  master= outer;
  nest_level= master->first_select()->nest_level;
}


/**
  Renumber select_lex object, and apply renumbering recursively to
  contained objects.

  @param  lex   Containing LEX object
*/
void st_select_lex::renumber(LEX *lex)
{
  select_number= ++lex->select_number;

  nest_level= outer_select() == NULL ? 0 : outer_select()->nest_level + 1;

  for (SELECT_LEX_UNIT *u= first_inner_unit(); u; u= u->next_unit())
    u->renumber_selects(lex);
}

/**
  Include query block into global list.

  @param plink - Pointer to start of list
*/ 
void st_select_lex::include_in_global(st_select_lex **plink)
{
  if ((link_next= *plink))
    link_next->link_prev= &link_next;
  link_prev= plink;
  *plink= this;
}


/**
  Include chain of query blocks into global list.

  @param start - Pointer to start of list
*/
void st_select_lex::include_chain_in_global(st_select_lex **start)
{
  st_select_lex *last_select;
  for (last_select= this;
       last_select->link_next != NULL;
       last_select= last_select->link_next)
  {}
  last_select->link_next= *start;
  last_select->link_next->link_prev= &last_select->link_next;
  link_prev= start;
  *start= this;
}


void st_select_lex::set_join(JOIN *join_arg)
{
  master_unit()->thd->lock_query_plan();
  join= join_arg;
  master_unit()->thd->unlock_query_plan();
}


/**
   Helper function which handles the "ON conditions" part of
   SELECT_LEX::get_optimizable_conditions().
   @returns true if OOM
*/
static bool get_optimizable_join_conditions(THD *thd,
                                            List<TABLE_LIST> &join_list)
{
  TABLE_LIST *table;
  List_iterator<TABLE_LIST> li(join_list);
  while((table= li++))
  {
    NESTED_JOIN *const nested_join= table->nested_join;
    if (nested_join &&
        get_optimizable_join_conditions(thd, nested_join->join_list))
      return true;
    Item *const jc= table->join_cond();
    if (jc && !thd->stmt_arena->is_conventional())
    {
      table->set_join_cond_optim(jc->copy_andor_structure(thd));
      if (!table->join_cond_optim())
        return true;
    }
    else
      table->set_join_cond_optim(jc);
  }
  return false;
}


/**
   Returns disposable copies of WHERE/HAVING/ON conditions.

   This function returns a copy which can be thrashed during
   this execution of the statement. Only AND/OR items are trashable!
   If in conventional execution, no copy is created, the permanent clauses are
   returned instead, as trashing them is no problem.

   @param      thd        thread handle
   @param[out] new_where  copy of WHERE
   @param[out] new_having copy of HAVING (if passed pointer is not NULL)

   Copies of join (ON) conditions are placed in TABLE_LIST::m_join_cond_optim.

   @returns true if OOM
*/
bool SELECT_LEX::get_optimizable_conditions(THD *thd,
                                            Item **new_where,
                                            Item **new_having)
{
  /*
    We want to guarantee that
    join->optimized is true => conditions are ready for reading.
    So if we are here, this should hold:
  */
  DBUG_ASSERT(!(join && join->is_optimized()));
  if (m_where_cond && !thd->stmt_arena->is_conventional())
  {
    *new_where= m_where_cond->copy_andor_structure(thd);
    if (!*new_where)
      return true;
  }
  else
    *new_where= m_where_cond;
  if (new_having)
  {
    if (m_having_cond && !thd->stmt_arena->is_conventional())
    {
      *new_having= m_having_cond->copy_andor_structure(thd);
      if (!*new_having)
        return true;
    }
    else
      *new_having= m_having_cond;
  }
  return get_optimizable_join_conditions(thd, top_join_list);
}

Item_exists_subselect::enum_exec_method
st_select_lex::subquery_strategy(THD *thd) const
{
  if (opt_hints_qb)
  {
    Item_exists_subselect::enum_exec_method strategy=
      opt_hints_qb->subquery_strategy();
    if (strategy != Item_exists_subselect::EXEC_UNSPECIFIED)
      return strategy;
  }

  // No SUBQUERY hint given, base possible strategies on optimizer_switch
  if (thd->optimizer_switch_flag(OPTIMIZER_SWITCH_MATERIALIZATION))
    return thd->optimizer_switch_flag(OPTIMIZER_SWITCH_SUBQ_MAT_COST_BASED) ?
      Item_exists_subselect::EXEC_EXISTS_OR_MAT :
      Item_exists_subselect::EXEC_MATERIALIZATION;

  return Item_exists_subselect::EXEC_EXISTS;
}

bool st_select_lex::semijoin_enabled(THD *thd) const
{
  return opt_hints_qb ?
    opt_hints_qb->semijoin_enabled(thd) :
    thd->optimizer_switch_flag(OPTIMIZER_SWITCH_SEMIJOIN);
}

void st_select_lex::update_semijoin_strategies(THD *thd)
{
  uint sj_strategy_mask= OPTIMIZER_SWITCH_FIRSTMATCH |
    OPTIMIZER_SWITCH_LOOSE_SCAN | OPTIMIZER_SWITCH_MATERIALIZATION |
    OPTIMIZER_SWITCH_DUPSWEEDOUT;

  uint opt_switches= thd->variables.optimizer_switch & sj_strategy_mask;

  List_iterator<TABLE_LIST> sj_list_it(sj_nests);
  TABLE_LIST *sj_nest;
  while ((sj_nest= sj_list_it++))
  {
    /*
      After semi-join transformation, original SELECT_LEX with hints is lost.
      Fetch hints from first table in semijoin nest.
    */
    List_iterator<TABLE_LIST> table_list(sj_nest->nested_join->join_list);
    TABLE_LIST *table= table_list++;
    sj_nest->nested_join->sj_enabled_strategies= table->opt_hints_qb ?
      table->opt_hints_qb->sj_enabled_strategies(opt_switches) : opt_switches;
  }
}


/**
  Check if an option that can be used only for an outer-most query block is
  applicable to this query block.

  @param lex    LEX of current statement
  @param option option name to output within the error message 

  @returns      false if valid, true if invalid, error is sent to client
*/

bool st_select_lex::validate_outermost_option(LEX *lex,
                                              const char *option) const
{
  if (this != lex->select_lex)
  {
    my_error(ER_CANT_USE_OPTION_HERE, MYF(0), option);
    return true;
  }
  return false;
}


/**
  Validate base options for a query block.

  @param lex                LEX of current statement
  @param options_arg        base options for a SELECT statement.

  @returns false if success, true if validation failed

  These options are supported, per DML statement:

  SELECT: SELECT_STRAIGHT_JOIN
          SELECT_HIGH_PRIORITY
          SELECT_DISTINCT
          SELECT_ALL
          SELECT_SMALL_RESULT
          SELECT_BIG_RESULT
          OPTION_BUFFER_RESULT
          OPTION_FOUND_ROWS
          OPTION_TO_QUERY_CACHE
  DELETE: OPTION_QUICK
          LOW_PRIORITY
  INSERT: LOW_PRIORITY
          HIGH_PRIORITY
  UPDATE: LOW_PRIORITY

  Note that validation is only performed for SELECT statements.
*/

bool st_select_lex::validate_base_options(LEX *lex, ulonglong options_arg) const
{
  DBUG_ASSERT(!(options_arg & ~(SELECT_STRAIGHT_JOIN |
                                SELECT_HIGH_PRIORITY |
                                SELECT_DISTINCT |
                                SELECT_ALL |
                                SELECT_SMALL_RESULT |
                                SELECT_BIG_RESULT |
                                OPTION_BUFFER_RESULT |
                                OPTION_FOUND_ROWS |
                                OPTION_TO_QUERY_CACHE)));

  if (options_arg & SELECT_DISTINCT &&
      options_arg & SELECT_ALL)
  {
    my_error(ER_WRONG_USAGE, MYF(0), "ALL", "DISTINCT");
    return true;
  }
  if (options_arg & SELECT_HIGH_PRIORITY &&
      validate_outermost_option(lex, "HIGH_PRIORITY"))
    return true;
  if (options_arg & OPTION_BUFFER_RESULT &&
      validate_outermost_option(lex, "SQL_BUFFER_RESULT"))
    return true;
  if (options_arg & OPTION_FOUND_ROWS &&
      validate_outermost_option(lex, "SQL_CALC_FOUND_ROWS"))
    return true;

  return false;
}


bool Query_options::merge(const Query_options &a,
                          const Query_options &b)
{
  query_spec_options= a.query_spec_options | b.query_spec_options;

  if (b.sql_cache == SELECT_LEX::SQL_NO_CACHE)
  {
    if (a.sql_cache == SELECT_LEX::SQL_NO_CACHE)
    {
      my_error(ER_DUP_ARGUMENT, MYF(0), "SQL_NO_CACHE");
      return true;
    }
    else if (a.sql_cache == SELECT_LEX::SQL_CACHE)
    {
      my_error(ER_WRONG_USAGE, MYF(0), "SQL_CACHE", "SQL_NO_CACHE");
      return true;
    }
  }
  else if (b.sql_cache == SELECT_LEX::SQL_CACHE)
  {
    if (a.sql_cache == SELECT_LEX::SQL_CACHE)
    {
      my_error(ER_DUP_ARGUMENT, MYF(0), "SQL_CACHE");
      return true;
    }
    else if (a.sql_cache == SELECT_LEX::SQL_NO_CACHE)
    {
      my_error(ER_WRONG_USAGE, MYF(0), "SQL_NO_CACHE", "SQL_CACHE");
      return true;
    }
  }
  sql_cache= b.sql_cache;
  return false;
}


bool Query_options::save_to(Parse_context *pc)
{
  LEX *lex= pc->thd->lex;
  ulonglong options= query_spec_options;

  switch (sql_cache) {
  case SELECT_LEX::SQL_CACHE_UNSPECIFIED:
    break;
  case SELECT_LEX::SQL_NO_CACHE:
    if (pc->select != lex->select_lex)
    {
      my_error(ER_CANT_USE_OPTION_HERE, MYF(0), "SQL_NO_CACHE");
      return true;
    }
    DBUG_ASSERT(pc->select->sql_cache == SELECT_LEX::SQL_CACHE_UNSPECIFIED);
    lex->safe_to_cache_query= false;
    options&= ~OPTION_TO_QUERY_CACHE;
    pc->select->sql_cache= SELECT_LEX::SQL_NO_CACHE;
    break;
  case SELECT_LEX::SQL_CACHE:
    if (pc->select != lex->select_lex)
    {
      my_error(ER_CANT_USE_OPTION_HERE, MYF(0), "SQL_CACHE");
      return true;
    }
    DBUG_ASSERT(pc->select->sql_cache == SELECT_LEX::SQL_CACHE_UNSPECIFIED);
    lex->safe_to_cache_query= true;
    options|= OPTION_TO_QUERY_CACHE;
    pc->select->sql_cache= SELECT_LEX::SQL_CACHE;
    break;
  default:
    DBUG_ASSERT(!"Unexpected cache option!");
  }
  if (pc->select->validate_base_options(lex, options))
    return true;
  pc->select->set_base_options(options);

  return false;
}


/**
  A routine used by the parser to decide whether we are specifying a full
  partitioning or if only partitions to add or to split.

  @retval  TRUE    Yes, it is part of a management partition command
  @retval  FALSE          No, not a management partition command
*/

bool LEX::is_partition_management() const
{
  return (sql_command == SQLCOM_ALTER_TABLE &&
          (alter_info.flags == Alter_info::ALTER_ADD_PARTITION ||
           alter_info.flags == Alter_info::ALTER_REORGANIZE_PARTITION));
}


/**
  @todo This function is obviously incomplete. It does not walk update lists,
  for instance. At the time of writing, however, this function has only a
  single use, to walk all parts of select statements. If more functionality is
  needed, it should be added here, in the same fashion as for SQLCOM_INSERT
  below.
*/
bool LEX::accept(Select_lex_visitor *visitor)
{
  if (unit->accept(visitor))
    return true;
  if (sql_command == SQLCOM_INSERT)
  {
    List_iterator<List_item> row_it(
        static_cast<Sql_cmd_insert_base *>(m_sql_cmd)->insert_many_values);
    for (List_item *row= row_it++; row != NULL; row= row_it++)
    {
      List_iterator<Item> col_it(*row);
      for (Item *item= col_it++; item != NULL; item= col_it++)
        if (walk_item(item, visitor))
          return true;
    }
  }
  return false;
}


void st_lex_master_info::initialize()
{
  host= user= password= log_file_name= bind_addr = NULL;
  port= connect_retry= 0;
  heartbeat_period= 0;
  sql_delay= 0;
  pos= 0;
  server_id= retry_count= 0;
  gtid= NULL;
  gtid_until_condition= UNTIL_SQL_BEFORE_GTIDS;
  view_id= NULL;
  until_after_gaps= false;
  ssl= ssl_verify_server_cert= heartbeat_opt= repl_ignore_server_ids_opt= 
    retry_count_opt= auto_position= LEX_MI_UNCHANGED;
  ssl_key= ssl_cert= ssl_ca= ssl_capath= ssl_cipher= NULL;
  ssl_crl= ssl_crlpath= NULL;
  tls_version= NULL;
  relay_log_name= NULL;
  relay_log_pos= 0;
  repl_ignore_server_ids.clear();
  channel= NULL;
  for_channel= false;
}

void st_lex_master_info::set_unspecified()
{
  initialize();
  sql_delay= -1;
}

#ifdef MYSQL_SERVER
uint binlog_unsafe_map[256];

#define UNSAFE(a, b, c) \
  { \
  DBUG_PRINT("unsafe_mixed_statement", ("SETTING BASE VALUES: %s, %s, %02X\n", \
    LEX::stmt_accessed_table_string(a), \
    LEX::stmt_accessed_table_string(b), \
    c)); \
  unsafe_mixed_statement(a, b, c); \
  }

/*
  Sets the combination given by "a" and "b" and automatically combinations
  given by other types of access, i.e. 2^(8 - 2), as unsafe.

  It may happen a colision when automatically defining a combination as unsafe.
  For that reason, a combination has its unsafe condition redefined only when
  the new_condition is greater then the old. For instance,
  
     . (BINLOG_DIRECT_ON & TRX_CACHE_NOT_EMPTY) is never overwritten by 
     . (BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF).
*/
void unsafe_mixed_statement(LEX::enum_stmt_accessed_table a,
                            LEX::enum_stmt_accessed_table b, uint condition)
{
  int type= 0;
  int index= (1U << a) | (1U << b);
  
  
  for (type= 0; type < 256; type++)
  {
    if ((type & index) == index)
    {
      binlog_unsafe_map[type] |= condition;
    }
  }
}
/*
  The BINLOG_* AND TRX_CACHE_* values can be combined by using '&' or '|',
  which means that both conditions need to be satisfied or any of them is
  enough. For example, 
    
    . BINLOG_DIRECT_ON & TRX_CACHE_NOT_EMPTY means that the statment is
    unsafe when the option is on and trx-cache is not empty;

    . BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF means the statement is unsafe
    in all cases.

    . TRX_CACHE_EMPTY | TRX_CACHE_NOT_EMPTY means the statement is unsafe
    in all cases. Similar as above.
*/
void binlog_unsafe_map_init()
{
  memset((void*) binlog_unsafe_map, 0, sizeof(uint) * 256);

  /*
    Classify a statement as unsafe when there is a mixed statement and an
    on-going transaction at any point of the execution if:

      1. The mixed statement is about to update a transactional table and
      a non-transactional table.

      2. The mixed statement is about to update a transactional table and
      read from a non-transactional table.

      3. The mixed statement is about to update a non-transactional table
      and temporary transactional table.

      4. The mixed statement is about to update a temporary transactional
      table and read from a non-transactional table.

      5. The mixed statement is about to update a transactional table and
      a temporary non-transactional table.
      
      6. The mixed statement is about to update a transactional table and
      read from a temporary non-transactional table.

      7. The mixed statement is about to update a temporary transactional
      table and temporary non-transactional table.

      8. The mixed statement is about to update a temporary transactional
      table and read from a temporary non-transactional table.
    After updating a transactional table if:

      9. The mixed statement is about to update a non-transactional table
      and read from a transactional table.

      10. The mixed statement is about to update a non-transactional table
      and read from a temporary transactional table.

      11. The mixed statement is about to update a temporary non-transactional
      table and read from a transactional table.
      
      12. The mixed statement is about to update a temporary non-transactional
      table and read from a temporary transactional table.

      13. The mixed statement is about to update a temporary non-transactional
      table and read from a non-transactional table.

    The reason for this is that locks acquired may not protected a concurrent
    transaction of interfering in the current execution and by consequence in
    the result.
  */
  /* Case 1. */
  UNSAFE(LEX::STMT_WRITES_TRANS_TABLE, LEX::STMT_WRITES_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF);
  /* Case 2. */
  UNSAFE(LEX::STMT_WRITES_TRANS_TABLE, LEX::STMT_READS_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF);
  /* Case 3. */
  UNSAFE(LEX::STMT_WRITES_NON_TRANS_TABLE, LEX::STMT_WRITES_TEMP_TRANS_TABLE,
    BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF);
  /* Case 4. */
  UNSAFE(LEX::STMT_WRITES_TEMP_TRANS_TABLE, LEX::STMT_READS_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF);
  /* Case 5. */
  UNSAFE(LEX::STMT_WRITES_TRANS_TABLE, LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON);
  /* Case 6. */
  UNSAFE(LEX::STMT_WRITES_TRANS_TABLE, LEX::STMT_READS_TEMP_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON);
  /* Case 7. */
  UNSAFE(LEX::STMT_WRITES_TEMP_TRANS_TABLE, LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON);
  /* Case 8. */
  UNSAFE(LEX::STMT_WRITES_TEMP_TRANS_TABLE, LEX::STMT_READS_TEMP_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON);
  /* Case 9. */
  UNSAFE(LEX::STMT_WRITES_NON_TRANS_TABLE, LEX::STMT_READS_TRANS_TABLE,
    (BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF) & TRX_CACHE_NOT_EMPTY);
  /* Case 10 */
  UNSAFE(LEX::STMT_WRITES_NON_TRANS_TABLE, LEX::STMT_READS_TEMP_TRANS_TABLE,
    (BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF) & TRX_CACHE_NOT_EMPTY);
  /* Case 11. */
  UNSAFE(LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE, LEX::STMT_READS_TRANS_TABLE,
    BINLOG_DIRECT_ON & TRX_CACHE_NOT_EMPTY);
  /* Case 12. */
  UNSAFE(LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE, LEX::STMT_READS_TEMP_TRANS_TABLE,
    BINLOG_DIRECT_ON & TRX_CACHE_NOT_EMPTY);
  /* Case 13. */
  UNSAFE(LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE, LEX::STMT_READS_NON_TRANS_TABLE,
     BINLOG_DIRECT_OFF & TRX_CACHE_NOT_EMPTY);
}
#endif
