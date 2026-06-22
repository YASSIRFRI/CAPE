% Transformation from OpenMP to CAPE DICKPT.
% Based on omptocape.Txl by Patricia REINOSO, 29 09 2017
%
% DICKPT is the two-executable model where the application
% communicates with an external monitor process via int3
% breakpoints and signal codes.
%
% Key differences from timestamped (cape.c) version:
%   - No CAPE_Init()/CAPE_Finalize() - monitor handles lifecycle
%   - No CAPE_Begin()/CAPE_End() - uses dickpt_start_ckpt()/
%     dickpt_generate_ckpt()/dickpt_stop_ckpt()
%   - Clauses use __cape_signal_val() with D_* codes
%   - Environment routines use dickpt_read_node()/dickpt_read_num_nodes()
%   - Variables registered via dickpt_register_region()

% TXL translation. OMP to DICKPT.

% Null transform - format output according to grammar
include "C.Grm"

% Comment out this line to disallow Gnu gcc extensions
include "CGnuOverrides.Grm"

% Comment out this line to disallow parsing of comments
include "CCommentOverrides.Grm"

% DICKPT and OpenMP grammar definition
include "CAPE_DICKPT.Grm"
include "OMP.Grm"

% Translation rule set
include "clausesRules_dickpt.Rul"
include "constructRules_dickpt.Rul"
include "operatorRules_dickpt.Rul"
include "environmentRoutRules_dickpt.Rul"
include "varDeclarationRules_dickpt.Rul"

% Main translation rule set
function main
	replace [program]
		CompilationUnit [repeat declaration_or_function_definition]

	% Creation of a global variable that contains all the
	% declaration of global variables in the C program.
	construct GlobalVars [repeat declaration]
		_

	export GlobalVars

	construct ThreadDecls [dickpt_threadprivate]
	export ThreadDecls

	% Section counter for assigning sections to nodes
	construct SectionCounter [number]
		0
	export SectionCounter

	% Dynamic-task mode: set to 1 when any plain function contains a
	% task construct (recursive task graph). Outlined task functions
	% and their forward declarations are collected program-wide.
	construct DynFlag [number]
		0
	export DynFlag
	construct DynTaskFns [repeat declaration_or_function_definition]
		_
	export DynTaskFns
	construct DynFwdDecls [repeat declaration]
		_
	export DynFwdDecls
	construct DynProbe [repeat declaration_or_function_definition]
		CompilationUnit [setDynFlagIfNested]

	% New includes for DICKPT application.
	% No MPI needed - monitor handles communication.
		construct NewIncludes [repeat declaration]
			'#include <stdio.h>
			'#include <stdlib.h>
			'#include <'string.h>
			'#include "../../../cape_ucx/include/cape_dickpt.h"

	construct NewCProgram [repeat declaration_or_function_definition]
		_ [changeUnit each CompilationUnit]

	import DynTaskFns
	import DynFwdDecls

	construct HeaderDecls [repeat declaration]
		NewIncludes [. each DynFwdDecls]
	construct ProgramUnits [repeat declaration_or_function_definition]
		NewCProgram [. each DynTaskFns]

	by
		HeaderDecls
		ProgramUnits
end function

% Function that starts dealing with the global variable declarations,
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

% Check whether the omp library was included and
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

	import GlobalVars [repeat declaration]

	export GlobalVars
		GlobalVars [. Decl]

	by
		Decl [alignGlobalIfDyn]
end function

% In dynamic-task mode the outlined task functions run outside main and
% must see the real file-scope globals: page-align them in place.
function alignGlobalIfDyn
	replace [declaration]
		Decl [declaration]
	import DynFlag [number]
	deconstruct DynFlag
		1
	where not
		Decl [isTypedefDecl]
	where not
		Decl [isExternDecl]
	by
		Decl [alignSharedDeclarators]
end function

function isTypedefDecl
	match * [sc_specifier]
		'typedef
end function

function isExternDecl
	match * [sc_specifier]
		'extern
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

	construct DickptDirective [directives]
		OMPSDirective [replaceThreadPrivate]

	import ThreadDecls [dickpt_threadprivate]

	deconstruct DickptDirective
		NewThreadList [repeat dickpt_threadprivate_variables]

	deconstruct ThreadDecls
		OldThreadList [repeat dickpt_threadprivate_variables]

	export ThreadDecls
		OldThreadList [. NewThreadList]
	by
		_
end function

% Identify a function and:
% - Add the global variable declaration to the main function.
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

	% DICKPT: keep variable declarations intact. Stack-local variables
	% must NOT be replaced by dickpt_register_region (the runtime explicitly
	% does not track the stack), and globals are still tracked as real
	% C declarations. So we skip [replaceDeclaration] here.
	construct NewCompilationUnit [function_definition]
		Function [addGlobalVarsToMain]
				 [replaceNestedTasksInFunction]
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
% routines in functions that are not the main
% function and that do not possess an OpenMP construct.
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
% In DICKPT: no CAPE_Init/CAPE_Finalize - the monitor
% manages the application lifecycle externally.
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

    import ThreadDecls [dickpt_threadprivate]

    % Page-align the globals that get registered for checkpoint tracking.
    % userfaultfd write-protect registration only accepts anonymous VMAs, but
    % the linker packs small statics onto the .data/.got.plt/.bss boundary
    % page, which is file-backed -> registration fails (EINVAL) and the region
    % is untracked. aligned(4096) puts each on its own clean anonymous .bss
    % page so dickpt_register_region's whole-range UFFDIO_REGISTER succeeds.
    construct AlignedGlobalVars [repeat declaration]
        _ [keepShadowGlobalsIfStatic GlobalVars]

    construct NewMainFunction [function_definition]
    	Specifiers MainName
    	'{
    		% Add the declarations to the main function.
    		AlignedGlobalVars
    		% Add the omp threadprivate statements that used to be global.
    		ThreadDecls
    		InnerStatement
    	'}

	by
		NewMainFunction
end function

% Append __attribute__((aligned(4096))) to every registered global's
% declarator so it lands on its own anonymous page (see addGlobalVarsToMain).
rule alignSharedDeclarators
	replace [init_declarator]
		ID [init_declarator]
	% the attribute can parse into the declarator or the trailing
	% attribute list depending on position: search the whole thing
	where not
		ID [hasAlignedAttribute]
	deconstruct ID
		Dtor [declarator] Init [opt initialization]
		Attrs [repeat attributes_or_asm]
	construct PageAlign [attributes_or_asm]
		'__attribute__ '(( 'aligned '( 4096 ') '))
	by
		Dtor Init Attrs [. PageAlign]
end rule

function hasAlignedAttribute
	match * [attribute_aligned]
		_ [attribute_aligned]
end function

% Static-task mode shadows aligned globals into main; dynamic-task mode
% must not (task functions run outside main, on the file-scope globals).
function keepShadowGlobalsIfStatic GlobalVars [repeat declaration]
	replace [repeat declaration]
		_ [repeat declaration]
	import DynFlag [number]
	deconstruct DynFlag
		0
	by
		GlobalVars [alignSharedDeclarators]
end function
