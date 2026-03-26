include "CopenMP.Grm"        % déscription de c et openMP
include "CGnuOverrides.Grm"  % déscription des ajouts de gnu



		%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
		%%     Ajoute les includes et defines necessaire      %%
		%%     Et envoie le programme à realMain              %%
		%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


function main
	replace [program]
		P [program]

	construct NewP [program]
		P [realMain]  

	construct Inc [repeat declaration_or_function_definition]
		'#include '<sys/param.h> 
		'#include '<sys/stat.h> 
		'#include '<'assert.h'> 
		'#include '<sched.h> 
		'#include '<stdio.h> 
		'#include '<stdlib.h> 
		'#include '<unistd.h> 
		'#include "dispatch.h" 
		'#include "file.h" 
		'#include "snapshot.h"
		 

	construct Def [repeat declaration_or_function_definition]
		'#define '__BASE__ "/tmp/saspfl-%s:%d" 
		'#define '__AFTER__ "%s-after:%d" 
		'#define '__BEFORE__ "%s-before:%d" 
		'#define '__END__ "" 
		'#define '__FINAL__ "%s-final"

	construct Def2 [repeat declaration_or_function_definition]
		'#define '__ORIGINAL__ "%s-original" 
		'#define '__TARGET__ "%s-target"

	deconstruct NewP
		NewPP [repeat declaration_or_function_definition]

	by
		Inc          % Serie de include pour " checkpoint methode "
		[. Def]      % Serie de define pour " checkpoint methode "
		[. Def2]     % Serie de define pour " checkpoint methode "
		[. NewPP]    % Le program aprés transformation par les sous-régles
	
end function



		%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
		%%      Applique les differentes sous regles tant     %%
		%%           qu'il n'y a pas de point fixe            %%
		%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%



rule realMain
	replace [program]
		P [program]
	construct NewP [program]
		P [changeSections] [changeFor]
		% Une fois les sections_stat transformées en for_stat il faut les retransformer
		% C'est pourquoi changeFor est en deuxième
	where not
		P [= NewP]
	by 
		NewP
end rule





		%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
		%%             Ensemble des sub-rules                 %%
		%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%








%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%% Parallele FOR : ne prend pas en compte while et do comme openmp %%
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


rule changeFor
	replace [statement]
		'#pragma 'omp 'parallel 'for RFC[repeat parallel_for_clause] IT[for_statement]

	deconstruct IT
		% Deconstruction de l'iteration
		% Afin de la replacer morceau par morceau le long du code
   		'for ( A [expression] '; B [expression] '; C [expression] ) 
				 D [statement] 
	
	construct decInit1 [repeat compound_statement_body]
		'char '__host__ '[ 'FILENAME_MAX '], '__base__ '[ 'FILENAME_MAX '] ';
		'char '__original__ '[ 'FILENAME_MAX '], '__target__ '[ 'FILENAME_MAX '] ';
		'char '__after__ '[ 'FILENAME_MAX '], '__before__ '[ 'FILENAME_MAX '] ';
		'char '__end__ '[ 'FILENAME_MAX '], '__final__ '[ 'FILENAME_MAX '] ';

	construct decInit2 [repeat compound_statement_body]
		'int '__pipe_in__ '[ 2 '], '__pipe_out__ '[ 2 '] ';
		'int '__ack__ ', '__i__, '__nb__, '__parent__, '__err__ ';
		'struct 'stat '__stat__ ;

	construct baseName [repeat compound_statement_body]
		'gethostname '( '__host__ ', 'MAXHOSTNAMELEN ') ';
		'sprintf '( '__base__ ', '__BASE__ ', '__host__ ', 'getpid '( ') ') ';

	construct createPipe [repeat compound_statement_body]
		'__err__ '= 'pipe '( '__pipe_in__ ') ';
		'assert '( '! '__err__ ') ';
		'__err__ '= 'pipe '( '__pipe_out__ ') ';
		'assert '( '! '__err__ ') ';

	construct startDispacher [repeat compound_statement_body]
		'switch '( '__parent__ '= 'fork '(') ') '{
		'case '-1 ': 
		'perror '( "dispach" ') ';
		'exit '( '1 ') ';
		'case '0 ':
		'close '( '__pipe_in__ '[ '0 '] ') ';
		'close '( '__pipe_out__ '[ '1 '] ') ';
		'dispach_body '( '__pipe_out__ '[ 0 '], '__pipe_in__ '[ '1 '] ') ';
		'close '( '__pipe_in__ '[ '1 '] ') ';
		'close '( '__pipe_out__ '[ '0 '] ') ';
		'exit '( '0 ') ';
		'default ':
		'close '( '__pipe_in__ '[ '1 '] ') ';
		'close '( '__pipe_out__ '[ '0 '] ') ';
		'}

	construct originalCkpt [repeat compound_statement_body]
		'sprintf '( '__original__ ', '__ORIGINAL__ ', '__base__ ') ';
		'sprintf '( '__target__ ', '__TARGET__ ', '__base__ ') ';
		'switch '( 'snapshot_create '( '__original__ ') ') '{
		'case '-1 ': 
		'perror '( '__original__ ') ';
		'exit '( '1 ') ';
		'case '0 ':
		'fprintf '( 'stderr ', "this ckpt is not intented to be restarted. \n " ') ';
		'exit '( '1 ') ';
		'default ':
		'bwrite '( '__pipe_out__ '[ '1 '] ', '__original__ ', 'FILENAME_MAX ') ';
		'bwrite '( '__pipe_out__ '[ '1 '] ', '__target__ ', 'FILENAME_MAX ') ';
		'}

	construct switchST [switch_statement]
		'switch '( '__parent__ '= 'snapshot_create '( '__before__ ') ') '{
		'case '-1 ': 
		'perror '( '__before__ ') ';
		'exit '( '1 ') ';
		'case '0 ':
		'close '( '__pipe_in__ '[ '0 '] ') ';
		'close '( '__pipe_out__ '[ '1 '] ') ';
		D
		'switch '( 'snapshot_create '( '__after__ ') ') '{
			'case '-1 ':
			'perror '( '__after__ ') ';
			'exit '( '1 ') ';
			'case '0 ':
			'fprintf '( 'stderr ', "This ckpt is't intended to be restarted. \n" ') ';
			'exit '( '1 ') ';
			'default ':
			'exit '( '0 ') ';
			}
		'default ':
		'bwrite '( '__pipe_out__ '[ '1 '] ', '__before__ ', 'FILENAME_MAX ') ';
		'bwrite '( '__pipe_out__ '[ '1 '] ', '__after__ ', 'FILENAME_MAX ') ';
		'}

	construct createCkpt [repeat compound_statement_body]
		'__nb__ '= '0 ';
		'for '( A '; B '; C ') '{
			'if '( '! '__parent__ ')
				'break ';
			'sprintf '( '__after__ ', '__AFTER__ ', '__base__ ', '__nb__ ') ';
			'sprintf '( '__before__ ', '__BEFORE__ ', '__base__ ', '__nb__ '++ ') ';		
			switchST
			'}

	construct createFinalCkpt [repeat compound_statement_body]
		'sprintf '( '__end__ ', '__END__ ') ';
		'sprintf '( '__final__ ', '__FINAL__ ', '__base__ ') ';
		'switch '( '__parent__ '= 'snapshot_create '( '__final__ ') ') '{
			'case '-1 ':
			'perror '( '__final__ ') ';
			'exit '( '1 ') ';
			'case '0 ':
			'close '( '__pipe_in__ '[ '0 '] ') ';
			'close '( '__pipe_out__ '[ '1 '] ') ';
			'unlink '( '__target__ ') ';
			'break ';
			'default ':
			'bwrite '( '__pipe_out__ '[ '1 '] ', '__end__ ', 'FILENAME_MAX ') ';
			'bwrite '( '__pipe_out__ '[ '1 '] ', '__final__ ', 'FILENAME_MAX ') ';			
			'bread '( '__pipe_in__ '[ '0 '] ', '& '__ack__ ', 'sizeof '( 'int ') ') ';
			'snapshot_restart '( '__target__ ') ';
			'perror '( '__target__ ') ';
			'exit '( '1 ') ';
			'}

	by
			'{
				% Référence aux travaux d'Eric Renault
				 decInit1 
				[. decInit2] 
				[. baseName] 
				[. createPipe]
				[. startDispacher] 
				[. originalCkpt] 
				[. createCkpt]
				[. createFinalCkpt]
			'} 

end rule




%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%% Parallele SECTION : transformer en FOR_STAT pour être retransformé par changeFor %%
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


rule changeSections

	replace  [statement]
		% Attention les clauses prises en compte ici sont des sections clauses
		% Elles ne peuvent être remise tel quel pour le for final que nous construisons
		'#pragma 'omp 'parallel 'sections RSC [repeat parallel_sections_clause]
		'{ 
			inutile [opt section_directive] 
			firstS [compound_statement] 
			SectionSeq [repeat section_sequences] 
		'}


	construct vide1 [repeat compound_statement]      
	construct vide2 [repeat compound_statement_body]  

	construct TteSection [repeat compound_statement]
		% extraction de tous les compound_statement inclus dans la sequence de section
		% Resultat sous forme de sequence de compound
		firstS vide1 [^ SectionSeq]

	construct nbrSection [number]
		% donne le nombre de section
		_ [length TteSection]

	export compte [number]
		% preparation de la global pour la sous-regle creeCaseSeq
		1

	construct CaseSuite [repeat compound_statement_body]
		% creation de sequence de case qui sera dans le switch
		% voir sous regle creeCaseSeq 
		vide2 [creeCaseSeq each TteSection]


	by
		'#pragma 'omp 'parallel 'for 
		'for '( 'I_karrrlito_ '= 0 '; 'I_karrrlito_ '<= nbrSection '; 'I_karrrlito_ '++ ')
			'{
			'switch '( 'I_karrrlito_ ')
				'{ 
					CaseSuite 
				'}
			'}

end rule



% Construit une sequence de case statement grâce à la sequence de compound_statement
% Fournit en argument

function creeCaseSeq CS [compound_statement]
	
	replace [repeat compound_statement_body]
		Ancien [repeat compound_statement_body]

	construct caseSt [compound_statement]
		% construit un case stat à chaque etape
		% sachant que les compound lui sont donné un par un
		'{ CS 'break '; '}

	import compte [number]
	construct NewCase [compound_statement_body]
		'case compte ': caseSt
	export compte
		compte [+ 1]

	by
		Ancien [. NewCase]

end function 






