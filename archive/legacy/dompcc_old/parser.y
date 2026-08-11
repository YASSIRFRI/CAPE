%%

pragma-list
	: pragma-element
	| pragma-list pragma-element
	;

pragma-element
	: pragma openmp-construct
	;

pragma
	: '#' TK_PRAGMA TK_OMP
	;

openmp-construct
	: parallel-construct
	| for-construct
	| sections-construct
	| single-construct
	| parallel-for-construct
	| parallel-sections-construct
	| master-construct
	| critical-construct
	| atomic-construct
	| ordered-construct
	;

structured-block
	: statement
	;

parallel-construct
	: parallel-directive structured-block
	;

parallel-directive
	: TK_PARALLEL '\n'
	| TK_PARALLEL parallel-clause-seq '\n'
	;

parallel-clause-seq
	: parallel-clause
	| parallel-clause-seq parallel-clause
	;

parallel-clause
	: unique-parallel-clause
	| data-clause
	;

unique-parallel-clause
	: TK_IF '(' expression ')'
	;

for-construct
	: for-directive iteration-statement
	;

for-directive
	: TK_FOR '\n'
	| TK_FOR for-clause-seq '\n'
	;

for-clause-seq
	: for-clause
	| for-clause-seq for-clause
	;

for-clause
	: unique-for-clause
	| data-clause
	| TK_NOWAIT
	;

unique-for-clause
	: TK_ORDERED
	| TK_SCHEDULE '(' schedule-kind ')'
	| TK_SCHEDULE '(' schedule-kind ',' expression ')'
	;

schedule-kind
	: TK_STATIC
	| TK_DYNAMIC
	| TK_GUIDED
	| TK_RUNTIME
	;

sections-construct
	: sections-directive section-scope
	;

sections-directive
	: TK_SECTIONS '\n'
	| TK_SECTIONS sections-clause-seq '\n'
	;

sections-clause-seq
	: sections-clause
	| sections-clause-seq sections-clause
	;

sections-clause
	: data-clause
	| TK_NOWAIT
	;

section-scope
	: '{' section-sequence '}'
	;

single-construct
	: single-directive structured-block
	;

single-directive
	: TK_SINGLE '\n'
	| TK_SINGLE single-clause-seq '\n'
	;

single-clause-seq
	: single-clause
	| single-clause-seq single-clause
	;

single-clause
	: data-clause
	| TK_NOWAIT
	;

parallel-for-construct
	: parallel-for-directive iteration-statement
	;

parallel-for-directive
	: TK_PARALLEL TK_FOR '\n'
	| TK_PARALLEL TK_FOR parallel-for-clause-seq '\n'
	;

parallel-for-clause-seq
	: parallel-for-clause
	| parallel-for-clause-seq parallel-for-clause
	;

parallel-for-clause
	: unique-parallel-clause
	| unique-for-clause
	| data-clause
	;

parallel-sections-construct
	: parallel-sections-directive section-scope
	;

parallel-sections-directive
	: TK_PARALLEL TK_SECTIONS '\n'
	| TK_PARALLEL TK_SECTIONS parallel-sections-clause-seq '\n'
	;

parallel-sections-clause-seq
	: parallel-sections-clause
	| parallel-sections-clause-seq parallel-sections-clause
	;

parallel-sections-clause
	: unique-parallel-clause
	| data-clause
	;

master-construct
	: master-directive structured-block
	;

master-directive
	: TK_MASTER '\n'
	;

critical-construct
	: critical-directive structured-block
	;

critical-directive
	: TK_CRITICAL '\n'
	| TK_CRITICAL region-phrase '\n'
	;

region-phrase
	: '(' identifier ')'
	;

atomic-construct
	: atomic-directive expression-statement
	;

atomic-directive
	: TK_ATOMIC '\n'
	;

ordered-construct
	: ordered-directive structured-block
	;

ordered-directive
	: TK_ORDERED '\n'
	;

data-clause
	: TK_PRIVATE '(' variable-list ')'
	| TK_FIRSTPRIVATE '(' variable-list ')'
	| TK_LASTPRIVATE '(' variable-list ')'
	| TK_SHARED '(' variable-list ')'
	| TK_DEFAULT '(' TK_SHARED ')'
	| TK_DEFAULT '(' TK_NONE ')'
	| TK_REDUCTION '(' reduction-operator ':' variable-list ')'
	| TK_COPYIN '(' variable-list ')'
	;

reduction-operator
	: '+' | '*' | '-' | '&' | '^' | '|' | TK_AND | TK_OR
	;

variable-list
	: identifier
	| variable-list ',' identifier
	;

%%
