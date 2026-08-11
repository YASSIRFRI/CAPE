# include <libgen.h>
# include <stdio.h>
# include <stdlib.h>
# include <string.h>

int main ( int argc, char * argv [ ] )
{
	char * args [ argc + 3 ], bindir [ 256 ] ;
	int i = 0, j ;

	sprintf ( bindir, "-B%s", dirname ( strdup ( argv [ 0 ] ) ) ) ;

	args [ i ++ ] = "gcc" ;
	args [ i ++ ] = "-no-integrated-cpp" ;
	args [ i ++ ] = bindir ;
	for ( j = 1 ; j < argc ; j ++ )
		args [ i ++ ] = argv [ j ] ;
	args [ i ++ ] = NULL ;

	execvp ( args [ 0 ], args ) ;
	perror ( args [ 0 ] ) ;
	exit ( 1 ) ;
}
