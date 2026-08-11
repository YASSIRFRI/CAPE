/* A Bison parser, made by GNU Bison 2.3.  */

/* Skeleton interface for Bison's Yacc-like parsers in C

   Copyright (C) 1984, 1989, 1990, 2000, 2001, 2002, 2003, 2004, 2005, 2006
   Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
     IDENTIFIER = 258,
     CONSTANT = 259,
     STRING_LITERAL = 260,
     SIZEOF = 261,
     TYPEDEF = 262,
     EXTERN = 263,
     STATIC = 264,
     AUTO = 265,
     REGISTER = 266,
     CHAR = 267,
     SHORT = 268,
     INT = 269,
     LONG = 270,
     SIGNED = 271,
     UNSIGNED = 272,
     FLOAT = 273,
     DOUBLE = 274,
     CONST = 275,
     VOLATILE = 276,
     VOID = 277,
     STRUCT = 278,
     UNION = 279,
     ENUM = 280,
     CASE = 281,
     DEFAULT = 282,
     IF = 283,
     ELSE = 284,
     SWITCH = 285,
     WHILE = 286,
     DO = 287,
     FOR = 288,
     GOTO = 289,
     CONTINUE = 290,
     BREAK = 291,
     RETURN = 292,
     RIGHT_ASSIGN = 293,
     LEFT_ASSIGN = 294,
     ADD_ASSIGN = 295,
     SUB_ASSIGN = 296,
     MUL_ASSIGN = 297,
     DIV_ASSIGN = 298,
     MOD_ASSIGN = 299,
     AND_ASSIGN = 300,
     XOR_ASSIGN = 301,
     OR_ASSIGN = 302,
     RIGHT_OP = 303,
     LEFT_OP = 304,
     INC_OP = 305,
     DEC_OP = 306,
     PTR_OP = 307,
     LE_OP = 308,
     GE_OP = 309,
     EQ_OP = 310,
     NE_OP = 311,
     AND_OP = 312,
     OR_OP = 313,
     ELIPSIS = 314
   };
#endif
/* Tokens.  */
#define IDENTIFIER 258
#define CONSTANT 259
#define STRING_LITERAL 260
#define SIZEOF 261
#define TYPEDEF 262
#define EXTERN 263
#define STATIC 264
#define AUTO 265
#define REGISTER 266
#define CHAR 267
#define SHORT 268
#define INT 269
#define LONG 270
#define SIGNED 271
#define UNSIGNED 272
#define FLOAT 273
#define DOUBLE 274
#define CONST 275
#define VOLATILE 276
#define VOID 277
#define STRUCT 278
#define UNION 279
#define ENUM 280
#define CASE 281
#define DEFAULT 282
#define IF 283
#define ELSE 284
#define SWITCH 285
#define WHILE 286
#define DO 287
#define FOR 288
#define GOTO 289
#define CONTINUE 290
#define BREAK 291
#define RETURN 292
#define RIGHT_ASSIGN 293
#define LEFT_ASSIGN 294
#define ADD_ASSIGN 295
#define SUB_ASSIGN 296
#define MUL_ASSIGN 297
#define DIV_ASSIGN 298
#define MOD_ASSIGN 299
#define AND_ASSIGN 300
#define XOR_ASSIGN 301
#define OR_ASSIGN 302
#define RIGHT_OP 303
#define LEFT_OP 304
#define INC_OP 305
#define DEC_OP 306
#define PTR_OP 307
#define LE_OP 308
#define GE_OP 309
#define EQ_OP 310
#define NE_OP 311
#define AND_OP 312
#define OR_OP 313
#define ELIPSIS 314




#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef union YYSTYPE
#line 30 "parser.y"
{
	int storage ;
}
/* Line 1489 of yacc.c.  */
#line 171 "y.tab.h"
	YYSTYPE;
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif

extern YYSTYPE yylval;

