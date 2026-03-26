% Transformation from OpenMP to CAPE.
% Patricia REINOSO, 29 09 2017
% 
% Revision history:
% 1.0 Initial revision - 29 09 2017
%
% Main program that executes all the transformations.
% from OpenMP to CAPE.

% TXL translation. OMP to CAPE.

% Null transform - format output according to grammar
include "C.Grm"

% Comment out this line to disallow Gnu gcc extensions
include "CGnuOverrides.Grm"

% Comment out this line to disallow parsing of comments
include "CCommentOverrides.Grm"

% CAPE and OpenMP grammar definition
include "CAPE.Grm"
include "OMP.Grm"

% Translation rule set
include "clausesRules.Rul"
include "constructRules.Rul"
include "datatypeRules.Rul"
include "environmentRoutRules.Rul"
include "operatorRules.Rul"
include "varDeclarationRules.Rul"

% Main translation rule set
function main 
	replace [program]
		CompilationUnit [repeat declaration_or_function_definition]

	% Creation of a global variable that contains all the 
	% declaration of global variables in the C program.
	% The list is initially empty.
	construct GlobalVars [repeat declaration]
		_

	% Export of the list of global variable varDeclarationRules
	% So it can be accessed by other functions.
	export GlobalVars 

	construct ThreadDecls [cape_threadprivate]
	export ThreadDecls

	% New includes into the C program.
	construct NewIncludes [repeat declaration]
		'#include <unistd.h>
		'#include <sys/types.h>
		'#include <sys/stat.h>
		'#include <fcntl.h>
		'#include <signal.h>
		'#include <malloc.h>
		'#include <errno.h>
		'#include "../../include/cape.h"
		'#include "mpi.h"

	construct NewCProgram [repeat declaration_or_function_definition]
		_ [changeUnit each CompilationUnit]

	by 
		NewIncludes
		NewCProgram
end function

% Function that start dealing with the global variable declarations,
% threadprivate declarations and transformations into functions.
function changeUnit CompilationUnit [declaration_or_function_definition]
	replace [repeat declaration_or_function_definition]
		SoFar [repeat declaration_or_function_definition]

	construct NewCompilationUnit [declaration_or_function_definition]
		CompilationUnit [checkOMPInclude]
						[treatGlobalDeclaration]
						[treatGlobalThreadPrivate]
						[treatFunction]
						[replaceEnvRoutInFunctions]

	by 
		SoFar [.NewCompilationUnit]
end function

% Check whereter the omp library was included and 
% deletes it from the includes section.
function checkOMPInclude
	replace [declaration_or_function_definition]
		Include [preprocessor]

	deconstruct Include
		'#include <omp.h>

	by 
		_
end function

% Identify the global variable declaration and add them
% to a global variable to be added to the main function.
function treatGlobalDeclaration
	replace [declaration_or_function_definition]
		Decl [declaration]
	deconstruct Decl
		VarDecl [declaration_body] Semi[semi]
	
	% Import of the global variable containing all the 
	% declarations of global variables in the C Program.
	% So it can be used as a local variable.
	import GlobalVars [repeat declaration]

	% The new declaration is added to the list.
	% And the variable is updated.
	export GlobalVars
		GlobalVars [. Decl]

	% The global variable declaration in the C program stays the same.
	by 
		Decl
end function 

% Identify the threadprivate declaration and add them
% to a global variable to be added to the main function.
function treatGlobalThreadPrivate
	replace [declaration_or_function_definition]
		Decl [declaration]

	deconstruct Decl
		Directive [preprocessor]

	deconstruct Directive 
		OMPSDirective [directives]

	deconstruct OMPSDirective
		'#pragma omp threadprivate(Variables [list decl_specifiers])
	
	construct CAPEDirective [directives]
		OMPSDirective [replaceThreadPrivate]

	import ThreadDecls [cape_threadprivate]

	deconstruct CAPEDirective
		NewThreadList [repeat cape_threadprivate_variables]

	deconstruct ThreadDecls
		OldThreadList [repeat cape_threadprivate_variables]

	export ThreadDecls
		OldThreadList [. NewThreadList]
	by 
		_
end function

% Identify a function and:
% - Add the globlal variable declaration to the main function.
% - Transform the C variable declarations.
% - Transform the omp construct.
% - Transform the execution environment Routines.
function treatFunction
	replace [declaration_or_function_definition]
		Function [function_definition]

	deconstruct Function 
		Specifiers [decl_specifiers] MainName [declarator]
    	Statements [compound_statement] 

	deconstruct Statements
    	'{	
    		InnerStatement [repeat compound_statement_body]
    	'}

    where 
    	InnerStatement [checkDirective]
    		
	construct NewCompilationUnit [function_definition]
		Function [addGlobalVarsToMain]
				 [replaceDeclaration]
				 [replaceConstruct]
				 [replaceEnvRoutine]
				 [replaceEnvRoutSetNumStatement]
	by 
		NewCompilationUnit
end function 

% Function that checks whether there exists a
% omp construct inside the function.
function checkDirective
    match * [compound_statement_body]
		ReductionC [directives]

end function

% Handles the replacement of execution environment 
% routines in fuctions that are not the main 
% function and that not possess as OpenMP construct.
function replaceEnvRoutInFunctions
replace [declaration_or_function_definition]
		Function [function_definition]

	deconstruct Function 
		Specifiers [decl_specifiers] MainName [declarator]
    	Statements [compound_statement] 
    		
	construct NewCompilationUnit [function_definition]
		Function [replaceEnvRoutine]
				 [replaceEnvRoutSetNumStatement]
	by 
		NewCompilationUnit
end function 

% Functions that identify the main function and 
% add the global variable declarations.
% Add the omp threadprivate directives to be
% tranform later as a regular omp construct.
function addGlobalVarsToMain
	replace [function_definition] 
		MainFunction [function_definition]

	deconstruct MainFunction
		Specifiers [decl_specifiers] MainName [declarator]
    	Statements [compound_statement] 

    % This is actually the main function.
    deconstruct MainName
    	main Args [repeat declarator_extension]

    deconstruct Statements
    	'{	
    		InnerStatement [repeat compound_statement_body]
    	'}
    
    % Get the list of global variable declarations of the C program.
    import GlobalVars [repeat declaration]

    import ThreadDecls [cape_threadprivate]

    construct NewInit [cape_statement]
    	CAPE_Init();

    construct NewFinalize [compound_statement_body]
    	CAPE_Finalize();
      
    construct NewMainFunction [function_definition]
    	Specifiers MainName
    	'{
    		% Add the declarations to the main function.
    		GlobalVars
    		NewInit
    		% Add the omp threadprivate statements that used to be global.
    		ThreadDecls
    		InnerStatement [. NewFinalize]
    	'}

	by 
		NewMainFunction
end function
