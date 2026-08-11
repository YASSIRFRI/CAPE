# include <stdio.h>

extern int yydebug ;

int main ( int argc, char * arg [ ] )
{
	yydebug = 1 ;
	yyparse ( ) ;
}
