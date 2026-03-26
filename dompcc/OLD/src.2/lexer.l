%{

#	include "y.tab.h"

%}

D [0-9]
L [a-zA-Z\_]
H [a-fA-F0-9]
E [Ee][+-]?{D}+
FS (f|F|l|L)
IS (u|U|l|L)*

%%

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

{L}({L}|{D})*	{ return is_user_defined ( yytext ) ? TYPE_NAME : IDENTIFIER ; }

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

[ \t\n]+	{ }

.		{ return yytext [ 0 ] ; }

%%
