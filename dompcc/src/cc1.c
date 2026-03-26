# include <sys/wait.h>
# include <assert.h>
# include <libgen.h>
# include <stdarg.h>
# include <stdio.h>
# include <stdlib.h>
# include <string.h>

char * mybasename ( char path [ ] )
{
	return basename ( strdup ( path ) ) ;
}

char * mydirname ( char path [ ] )
{
	return dirname ( strdup ( path ) ) ;
}

char * realname ( char prog [ ] )
{
	static char * ret = NULL ;

	if ( ! ret ) {
		FILE * fd ;
		char command [ 256 ] ;

		sprintf ( command, "gcc -print-prog-name=%s", prog ) ;
		ret = ( char * ) malloc ( 256 * sizeof ( char ) ) ;
		fd = popen ( command, "r" ) ;
		fscanf ( fd, "%s", ret ) ;
		pclose ( fd ) ;
	}
	return ret ;
}

int normalexec ( char * args [ ] )
{
	int status ;
	pid_t pid ;

	switch ( pid = fork ( ) ) {
	case 0 :
		args [ 0 ] = realname ( mybasename ( args [ 0 ] ) ) ;
		execv ( args [ 0 ], args ) ;
	case -1 :
		perror ( args [ 0 ] ) ;
		exit ( 1 ) ;
	default :
		waitpid ( pid, & status, 0 ) ;
	}
	return status ;
}

int option_rank ( int argc, char * argv [ ], char option )
{
	int i ;

	for ( i = 0 ; i < argc ; i ++ )
		if ( argv [ i ] [ 0 ] == '-' )
			if ( argv [ i ] [ 1 ] == option )
				if ( argv [ i ] [ 2 ] == '\0' )
					return i ;
	return -1 ;
}

int divert ( int argc, char * argv [ ] )
{
	char * base ;

	base = mybasename ( argv [ 0 ] ) ;

	if ( ! strcmp ( base, "cpp0" ) )
		return 1 ;

	if ( ! strcmp ( base, "cc1" ) )
		return option_rank ( argc, argv, 'E' ) >= 0 ;

	return 0 ;
}

int vsystem ( char * format, ... )
{
	char cmd [ 256 ] ;
	int ret ;
	va_list ptr ;

	va_start ( ptr, format ) ;
	vsprintf ( cmd, format, ptr ) ;
	va_end ( ptr ) ;
	ret = system ( cmd ) ;
	if ( ret < 0 || WEXITSTATUS ( ret ) )
		return ret < 0 ? ret : WEXITSTATUS ( ret ) ;
	return 0 ;
}

int do_job ( int argc, char * argv [ ] )
{
	char * dir, * tmpdir, tmp1 [ 32 ] ;
	int i ;

	dir = mydirname ( argv [ 0 ] ) ;

	for ( i = argc - 1 ; i ; i -- )
		if ( strstr ( argv [ i ], ".c\0" ) )
			break ;
	assert ( i ) ;

	tmpdir = ( char * ) malloc ( 32 * sizeof ( char ) ) ;
	sprintf ( tmpdir, "/tmp/%s.XXXXXX", mybasename ( argv [ 0 ] ) ) ;
	tmpdir = mkdtemp ( tmpdir ) ;
	sprintf ( tmp1, "%s/tmp1.c", tmpdir ) ;

#	define RET( X )			{ int ret ; if ( ret = ( X ) ) return ret ; }
#	define VSYSTEM( ... )	RET ( vsystem ( __VA_ARGS__ ) )

	VSYSTEM (
		"%s/txl %s %s/transform.txl > %s", // 2>/dev/null",
		dir, argv [ i ], dir, tmp1
	) ;

	argv [ i ] = tmp1 ;
	argv [ argc ] = NULL ;
	RET ( normalexec ( argv ) ) ;

	VSYSTEM ( "rm -rf %s", tmpdir ) ;

	return 0 ;
}

int main ( int argc, char * argv [ ] )
{
	return divert ( argc, argv ) ? do_job ( argc, argv ) : normalexec ( argv ) ;
}
