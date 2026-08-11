%{

#	include <stdio.h>

#	define YYDEBUG	1
#	define yyerror	perror

%}

%token	PRAGMA_OMP
%token	OMP_PARALLEL OMP_SECTIONS OMP_NOWAIT OMP_ORDERED OMP_SCHEDULE
%token	OMP_DYNAMIC OMP_GUIDED OMP_RUNTIME OMP_SECTION OMP_SINGLE OMP_MASTER
%token	OMP_CRITICAL OMP_BARRIER OMP_ATOMIC OMP_FLUSH OMP_THREADPRIVATE
%token	OMP_PRIVATE OMP_FIRSTPRIVATE OMP_LASTPRIVATE OMP_SHARED OMP_NONE
%token	OMP_REDUCTION OMP_COPYIN
%token	AUTO BREAK CASE CHAR CONST CONTINUE DEFAULT DO DOUBLE ELSE ENUM EXTERN
%token	FLOAT FOR GOTO IF INT LONG REGISTER RETURN SHORT SIGNED SIZEOF STATIC
%token	STRUCT SWITCH TYPEDEF UNION UNSIGNED VOID VOLATILE WHILE CONSTANT
%token	RIGHT_ASSIGN LEFT_ASSIGN ADD_ASSIGN SUB_ASSIGN MUL_ASSIGN DIV_ASSIGN
%token	MOD_ASSIGN AND_ASSIGN XOR_ASSIGN OR_ASSIGN RIGHT_OP LEFT_OP INC_OP
%token	DEC_OP PTR_OP LE_OP GE_OP EQ_OP NE_OP AND_OP OR_OP ELIPSIS
%token	IDENTIFIER STRING_LITERAL

%start file

%%

string_literal_seq
	: STRING_LITERAL
	| STRING_LITERAL string_literal_seq
	;

primary_expr
	: identifier
	| CONSTANT
	| string_literal_seq
	| '(' expr ')'
	;

postfix_expr
	: primary_expr
	| postfix_expr '[' expr ']'
	| postfix_expr '(' ')'
	| postfix_expr '(' argument_expr_list ')'
	| postfix_expr '.' identifier
	| postfix_expr PTR_OP identifier
	| postfix_expr INC_OP
	| postfix_expr DEC_OP
	;

argument_expr_list
	: assignment_expr
	| argument_expr_list ',' assignment_expr
	;

unary_expr
	: postfix_expr
	| INC_OP unary_expr
	| DEC_OP unary_expr
	| unary_operator cast_expr
	| SIZEOF unary_expr
	| SIZEOF '(' type_name ')'
	;

unary_operator
	: '&' | '*' | '+' | '-' | '~' | '!'
	;

cast_expr
	: unary_expr
	| '(' type_name ')' cast_expr
	;

multiplicative_expr
	: cast_expr
	| multiplicative_expr '*' cast_expr
	| multiplicative_expr '/' cast_expr
	| multiplicative_expr '%' cast_expr
	;

additive_expr
	: multiplicative_expr
	| additive_expr '+' multiplicative_expr
	| additive_expr '-' multiplicative_expr
	;

shift_expr
	: additive_expr
	| shift_expr LEFT_OP additive_expr
	| shift_expr RIGHT_OP additive_expr
	;

relational_expr
	: shift_expr
	| relational_expr '<' shift_expr
	| relational_expr '>' shift_expr
	| relational_expr LE_OP shift_expr
	| relational_expr GE_OP shift_expr
	;

equality_expr
	: relational_expr
	| equality_expr EQ_OP relational_expr
	| equality_expr NE_OP relational_expr
	;

and_expr
	: equality_expr
	| and_expr '&' equality_expr
	;

exclusive_or_expr
	: and_expr
	| exclusive_or_expr '^' and_expr
	;

inclusive_or_expr
	: exclusive_or_expr
	| inclusive_or_expr '|' exclusive_or_expr
	;

logical_and_expr
	: inclusive_or_expr
	| logical_and_expr AND_OP inclusive_or_expr
	;

logical_or_expr
	: logical_and_expr
	| logical_or_expr OR_OP logical_and_expr
	;

conditional_expr
	: logical_or_expr
	| logical_or_expr '?' logical_or_expr ':' conditional_expr
	;

assignment_expr
	: conditional_expr
	| unary_expr assignment_operator assignment_expr
	;

assignment_operator
	: '='
	| MUL_ASSIGN
	| DIV_ASSIGN
	| MOD_ASSIGN
	| ADD_ASSIGN
	| SUB_ASSIGN
	| LEFT_ASSIGN
	| RIGHT_ASSIGN
	| AND_ASSIGN
	| XOR_ASSIGN
	| OR_ASSIGN
	;

expr
	: assignment_expr
	| expr ',' assignment_expr
	;

constant_expr
	: conditional_expr
	;

declaration
	: declaration_specifiers ';'
	| declaration_specifiers init_declarator_list ';'
	| threadprivate_directive
	;

declaration_specifiers
	: storage_class_specifier
	| storage_class_specifier declaration_specifiers
	| type_specifier
	| type_specifier declaration_specifiers
	;

init_declarator_list
	: init_declarator
	| init_declarator_list ',' init_declarator
	;

init_declarator
	: declarator
	| declarator '=' initializer
	;

storage_class_specifier
	: TYPEDEF
	| EXTERN
	| STATIC
	| AUTO
	| REGISTER
	;

type_specifier
	: CHAR
	| SHORT
	| INT
	| LONG
	| SIGNED
	| UNSIGNED
	| FLOAT
	| DOUBLE
	| CONST
	| VOLATILE
	| VOID
	| struct_or_union_specifier
	| enum_specifier
	| TYPE_NAME
	;

struct_or_union_specifier
	: struct_or_union identifier '{' struct_declaration_list '}'
	| struct_or_union '{' struct_declaration_list '}'
	| struct_or_union identifier
	;

struct_or_union
	: STRUCT
	| UNION
	;

struct_declaration_list
	: struct_declaration
	| struct_declaration_list struct_declaration
	;

struct_declaration
	: type_specifier_list struct_declarator_list ';'
	;

struct_declarator_list
	: struct_declarator
	| struct_declarator_list ',' struct_declarator
	;

struct_declarator
	: declarator
	| ':' constant_expr
	| declarator ':' constant_expr
	;

enum_specifier
	: ENUM '{' enumerator_list '}'
	| ENUM identifier '{' enumerator_list '}'
	| ENUM identifier
	;

enumerator_list
	: enumerator
	| enumerator_list ',' enumerator
	;

enumerator
	: identifier
	| identifier '=' constant_expr
	;

declarator
	: declarator2
	| pointer declarator2
	;

declarator2
	: identifier
	| '(' declarator ')'
	| declarator2 '[' ']'
	| declarator2 '[' constant_expr ']'
	| declarator2 '(' ')'
	| declarator2 '(' parameter_type_list ')'
	| declarator2 '(' parameter_identifier_list ')'
	;

pointer
	: '*'
	| '*' type_specifier_list
	| '*' pointer
	| '*' type_specifier_list pointer
	;

type_specifier_list
	: type_specifier
	| type_specifier_list type_specifier
	;

parameter_identifier_list
	: identifier_list
	| identifier_list ',' ELIPSIS
	;

identifier_list
	: identifier
	| identifier_list ',' identifier
	;

parameter_type_list
	: parameter_list
	| parameter_list ',' ELIPSIS
	;

parameter_list
	: parameter_declaration
	| parameter_list ',' parameter_declaration
	;

parameter_declaration
	: type_specifier_list declarator
	| type_name
	;

type_name
	: type_specifier_list
	| type_specifier_list abstract_declarator
	;

abstract_declarator
	: pointer
	| abstract_declarator2
	| pointer abstract_declarator2
	;

abstract_declarator2
	: '(' abstract_declarator ')'
	| '[' ']'
	| '[' constant_expr ']'
	| abstract_declarator2 '[' ']'
	| abstract_declarator2 '[' constant_expr ']'
	| '(' ')'
	| '(' parameter_type_list ')'
	| abstract_declarator2 '(' ')'
	| abstract_declarator2 '(' parameter_type_list ')'
	;

initializer
	: assignment_expr
	| '{' open_brace initializer_list '}'
	| '{' open_brace initializer_list ',' '}'
	;

initializer_list
	: initializer
	| initializer_list ',' initializer
	;

statement
	: labeled_statement
	| compound_statement
	| expression_statement
	| selection_statement
	| iteration_statement
	| jump_statement
	| openmp_construct
	;

openmp_construct
	: parallel_construct
	| for_construct
	| sections_construct
	| single_construct
	| parallel_for_construct
	| parallel_sections_construct
	| master_construct
	| critical_construct
	| atomic_construct
	| ordered_construct
	;

openmp_directive
	: barrier_directive
	| flush_directive
	;

structured_block
	: statement
	;

parallel_construct
	: parallel_directive structured_block
	;

parallel_clause_optseq
	: /* empty */
	| parallel_clause_optseq parallel_clause
	;

parallel_directive
	: PRAGMA_OMP OMP_PARALLEL parallel_clause_optseq '\n'
	;

parallel_clause
	: unique_parallel_clause
	| data_clause
	;

unique_parallel_clause
	: OMP_IF '(' expr ')'
	;

for_construct
	: for_directive iteration_statement ;

for_clause_optseq
	: /* empty */
	| for_clause_optseq for_clause
	;

for_directive
	: PRAGMA_OMP OMP_FOR for_clause_optseq '\n' ;

for_clause
	: unique_for_clause
	| data_clause
	| OMP_NOWAIT
	;

unique_for_clause
	: OMP_ORDERED
	| OMP_SCHEDULE '(' schedule_kind ')'
	| OMP_SCHEDULE '(' schedule_kind ',' expr ')'
	;

schedule_kind
	: OMP_STATIC
	| OMP_DYNAMIC
	| OMP_GUIDED
	| OMP_RUNTIME
	;

sections_construct
	: sections_directive section_scope
	;

sections_clause_optseq
	: /* empty */
	| sections_clause_optseq sections_clause
	;

sections_directive
	: PRAGMA_OMP OMP_SECTIONS sections_clause_optseq '\n'
	;

sections_clause
	: data_clause
	| OMP_NOWAIT
	;

section_scope
	: '{' section_sequence '}'
	;

section_sequence
	: structured_block
	| section_directive structured_block
	| section_sequence section_directive structured_block
	;

section_directive
	: PRAGMA_OMP OMP_SECTION '\n'
	;

single_construct
	: single_directive structured_block
	;

single_clause_optseq
	: /* empty */
	| single_clause_optseq single_clause
	;

single_directive
	: PRAGMA_OMP OMP_SINGLE single_clause_optseq '\n'
	;

single_clause
	: data_clause
	| OMP_NOWAIT
	;

parallel_for_construct
	: parallel_for_directive iteration_statement
	;

parallel_for_clause_optseq
	: /* empty */
	| parallel_for_clause_optseq parallel_for_clause
	;

parallel_for_directive
	: PRAGMA_OMP OMP_PARALLEL OMP_FOR parallel_for_clause_optseq '\n'
	;

parallel_for_clause
	: unique_parallel_clause
	| unique_for_clause
	| data_clause
	;

parallel_sections_construct
	: parallel_sections_directive section_scope
	;

parallel_sections_clause_optseq
	: /* empty */
	| parallel_sections_clause_optseq parallel_sections_clause
	;

parallel_sections_directive
	: PRAGMA_OMP OMP_PARALLEL OMP_SECTIONS parallel_sections_clause_optseq '\n'
	;

parallel_sections_clause
	: unique_parallel_clause
	| data_clause
	;

master_construct
	: master_directive structured_block
	;

master_directive
	: PRAGMA_OMP OMP_MASTER '\n'
	;

critical_construct
	: critical_directive structured_block
	;

critical_directive
	: PRAGMA_OMP OMP_CRITICAL region_phrase_opt '\n'
	;

region_phrase_opt
	: /* empty */
	| '(' identifier ')'
	;

barrier_directive
	: PRAGMA_OMP OMP_BARRIER '\n'
	;

atomic_construct
	: atomic_directive expression_statement
	;

atomic_directive
	: PRAGMA_OMP OMP_ATOMIC '\n'
	;

flush_directive
	: PRAGMA_OMP OMP_FLUSH flush_vars_opt '\n'
	;

flush_vars_opt
	: /* empty */
	| '(' variable_list ')'
	;

ordered_construct
	: ordered_directive structured_block
	;

ordered_directive
	: PRAGMA_OMP OMP_ORDERED '\n'
	;

threadprivate_directive
	: PRAGMA_OMP OMP_THREADPRIVATE '(' variable_list ')' '\n'
	;

data_clause
	: OMP_PRIVATE '(' variable_list ')'
	| OMP_FIRSTPRIVATE '(' variable_list ')'
	| OMP_LASTPRIVATE '(' variable_list ')'
	| OMP_SHARED '(' variable_list ')'
	| OMP_DEFAULT '(' OMP_SHARED ')'
	| OMP_DEFAULT '(' OMP_NONE ')'
	| OMP_REDUCTION '(' reduction_operator ':' variable_list ')'
	| OMP_COPYIN '(' variable_list ')'
	;

reduction_operator
	: OMP_PLUS
	| OMP_MULT
	| OMP_MINUS
	| OMP_BAND
	| OMP_XOR
	| OMP_BOR
	| OMP_LAND
	| OMP_LOR
	;

variable_list
	: identifier
	| variable_list ',' identifier
	;

labeled_statement
	: identifier ':' statement
	| CASE constant_expr ':' statement
	| DEFAULT ':' statement
	;

open_brace
	:
	;

compound_statement
	: '{' '}'
	| '{' statement_list '}'
	| '{' open_brace declaration_list '}'
	| '{' open_brace declaration_list statement_list '}'
	;

declaration_list
	: declaration
	| declaration_list declaration
	;

statement_list
	: statement
	| openmp_directive
	| statement_list statement
	| statement_list openmp_directive
	;

expression_statement
	: ';'
	| expr ';'
	;

else_statement
	: /* empty */
	| ELSE statement
	;

selection_statement
	: IF '(' expr ')' statement else_statement
	| SWITCH '(' expr ')' statement
	;

iteration_statement
	: WHILE '(' expr ')' statement
	| DO statement WHILE '(' expr ')' ';'
	| FOR '(' ';' ';' ')' statement
	| FOR '(' ';' ';' expr ')' statement
	| FOR '(' ';' expr ';' ')' statement
	| FOR '(' ';' expr ';' expr ')' statement
	| FOR '(' expr ';' ';' ')' statement
	| FOR '(' expr ';' ';' expr ')' statement
	| FOR '(' expr ';' expr ';' ')' statement
	| FOR '(' expr ';' expr ';' expr ')' statement
	;

jump_statement
	: GOTO identifier ';'
	| CONTINUE ';'
	| BREAK ';'
	| RETURN ';'
	| RETURN expr ';'
	;

file
	: external_definition
	| file external_definition
	;

external_definition
	: function_definition
	| declaration
	;

function_definition
	: declarator function_body
	| declaration_specifiers declarator function_body
	;

function_body
	: compound_statement
	| declaration_list compound_statement
	;

identifier
	: IDENTIFIER
	;

OMP_DEFAULT : DEFAULT ;
OMP_FOR : FOR ;
OMP_IF : IF ;
OMP_STATIC : STATIC ;
OMP_LAND : AND_OP ;
OMP_LOR : OR_OP ;
OMP_BAND : '&' ;
OMP_MINUS : '-' ;
OMP_PLUS : '+' ;
OMP_MULT : '*' ;
OMP_XOR : '^' ;
OMP_BOR : '|' ;

TYPE_NAME : IDENTIFIER ;

%%
