%{

#	include "y.tab.h"

	int pragmaline = 0 ;

%}

D [0-9]
L [a-zA-Z\_]
H [a-fA-F0-9]
E [Ee][+-]?{D}+
FS (f|F|l|L)
IS (u|U|l|L)*

%%

^"#"[ \t]*"pragma"[ \t]+"omp"[ \t]+		{
					pragmaline = 1 ;
					return PRAGMA_OMP ;
				}
\n				{
					if ( pragmaline ) {
						pragmaline = 0 ;
						return yytext [ 0 ] ;
					}
				}

parallel		{ return OMP_PARALLEL ; }
sections		{ return OMP_SECTIONS ; }
nowait			{ return OMP_NOWAIT ; }
ordered			{ return OMP_ORDERED ; }
schedule		{ return OMP_SCHEDULE ; }
dynamic			{ return OMP_DYNAMIC ; }
guided			{ return OMP_GUIDED ; }
runtime			{ return OMP_RUNTIME ; }
section			{ return OMP_SECTION ; }
single			{ return OMP_SINGLE ; }
master			{ return OMP_MASTER ; }
critical		{ return OMP_CRITICAL ; }
barrier			{ return OMP_BARRIER ; }
atomic			{ return OMP_ATOMIC ; }
flush			{ return OMP_FLUSH ; }
threadprivate	{ return OMP_THREADPRIVATE ; }
private			{ return OMP_PRIVATE ; }
firstprivate	{ return OMP_FIRSTPRIVATE ; }
lastprivate		{ return OMP_LASTPRIVATE ; }
shared			{ return OMP_SHARED ; }
none			{ return OMP_NONE ; }
reduction		{ return OMP_REDUCTION ; }
copyin			{ return OMP_COPYIN ; }

auto			{ return AUTO ; }
break			{ return BREAK ; }
case			{ return CASE ; }
char			{ return CHAR ; }
const			{ return CONST ; }
continue		{ return CONTINUE ; }
default			{ return DEFAULT ; }
do				{ return DO ; }
double			{ return DOUBLE ; }
else			{ return ELSE ; }
enum			{ return ENUM ; }
extern			{ return EXTERN ; }
float			{ return FLOAT ; }
for				{ return FOR ; }
goto			{ return GOTO ; }
if				{ return IF ; }
int				{ return INT ; }
long			{ return LONG ; }
register		{ return REGISTER ; }
return			{ return RETURN ; }
short			{ return SHORT ; }
signed			{ return SIGNED ; }
sizeof			{ return SIZEOF ; }
static			{ return STATIC ; }
struct			{ return STRUCT ; }
switch			{ return SWITCH ; }
typedef			{ return TYPEDEF ; }
union			{ return UNION ; }
unsigned		{ return UNSIGNED ; }
void			{ return VOID ; }
volatile		{ return VOLATILE ; }
while			{ return WHILE ; }

{L}({L}|{D})*	{ return IDENTIFIER ; }

\"[^\"]*\"		{ return STRING_LITERAL ; }

0[xX]{H}+{IS}?			{ return CONSTANT ; }
0{D}+{IS}?				{ return CONSTANT ; }
{D}+{IS}?				{ return CONSTANT ; }
'(\\.|[^\\'])+'			{ return CONSTANT ; }
{D}+{E}{FS}?			{ return CONSTANT ; }
{D}*"."{D}+({E})?{FS}?	{ return CONSTANT ; }
{D}+"."{D}*({E})?{FS}?	{ return CONSTANT ; }

">>="	{ return RIGHT_ASSIGN ; }
"<<="	{ return LEFT_ASSIGN ; }
"+="	{ return ADD_ASSIGN ; }
"-="	{ return SUB_ASSIGN ; }
"*="	{ return MUL_ASSIGN ; }
"/="	{ return DIV_ASSIGN ; }
"%="	{ return MOD_ASSIGN ; }
"&="	{ return AND_ASSIGN ; }
"^="	{ return XOR_ASSIGN ; }
"|="	{ return OR_ASSIGN ; }
">>"	{ return RIGHT_OP ; }
"<<"	{ return LEFT_OP ; }
"++"	{ return INC_OP ; }
"--"	{ return DEC_OP ; }
"->"	{ return PTR_OP ; }
"<="	{ return LE_OP ; }
">="	{ return GE_OP ; }
"=="	{ return EQ_OP ; }
"!="	{ return NE_OP ; }
"&&"	{ return AND_OP ; }
"||"	{ return OR_OP ; }

"..."	{ return ELIPSIS ; }

"="		{ return yytext [ 0 ] ; }
"!"		{ return yytext [ 0 ] ; }
"~"		{ return yytext [ 0 ] ; }
"/"		{ return yytext [ 0 ] ; }
"%"		{ return yytext [ 0 ] ; }
"<"		{ return yytext [ 0 ] ; }
">"		{ return yytext [ 0 ] ; }
"?"		{ return yytext [ 0 ] ; }
";"		{ return yytext [ 0 ] ; }
"{"		{ return yytext [ 0 ] ; }
"}"		{ return yytext [ 0 ] ; }
","		{ return yytext [ 0 ] ; }
":"		{ return yytext [ 0 ] ; }
"("		{ return yytext [ 0 ] ; }
")"		{ return yytext [ 0 ] ; }
"["		{ return yytext [ 0 ] ; }
"]"		{ return yytext [ 0 ] ; }
"."		{ return yytext [ 0 ] ; }
"&"		{ return yytext [ 0 ] ; }
"+"		{ return yytext [ 0 ] ; }
"*"		{ return yytext [ 0 ] ; }
"^"		{ return yytext [ 0 ] ; }
"|"		{ return yytext [ 0 ] ; }

[ \t]	{ }

%%
