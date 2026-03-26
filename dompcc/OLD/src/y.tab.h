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
     PRAGMA_OMP = 258,
     OMP_PARALLEL = 259,
     OMP_SECTIONS = 260,
     OMP_NOWAIT = 261,
     OMP_ORDERED = 262,
     OMP_SCHEDULE = 263,
     OMP_DYNAMIC = 264,
     OMP_GUIDED = 265,
     OMP_RUNTIME = 266,
     OMP_SECTION = 267,
     OMP_SINGLE = 268,
     OMP_MASTER = 269,
     OMP_CRITICAL = 270,
     OMP_BARRIER = 271,
     OMP_ATOMIC = 272,
     OMP_FLUSH = 273,
     OMP_THREADPRIVATE = 274,
     OMP_PRIVATE = 275,
     OMP_FIRSTPRIVATE = 276,
     OMP_LASTPRIVATE = 277,
     OMP_SHARED = 278,
     OMP_NONE = 279,
     OMP_REDUCTION = 280,
     OMP_COPYIN = 281,
     AUTO = 282,
     BREAK = 283,
     CASE = 284,
     CHAR = 285,
     CONST = 286,
     CONTINUE = 287,
     DEFAULT = 288,
     DO = 289,
     DOUBLE = 290,
     ELSE = 291,
     ENUM = 292,
     EXTERN = 293,
     FLOAT = 294,
     FOR = 295,
     GOTO = 296,
     IF = 297,
     INT = 298,
     LONG = 299,
     REGISTER = 300,
     RETURN = 301,
     SHORT = 302,
     SIGNED = 303,
     SIZEOF = 304,
     STATIC = 305,
     STRUCT = 306,
     SWITCH = 307,
     TYPEDEF = 308,
     UNION = 309,
     UNSIGNED = 310,
     VOID = 311,
     VOLATILE = 312,
     WHILE = 313,
     CONSTANT = 314,
     RIGHT_ASSIGN = 315,
     LEFT_ASSIGN = 316,
     ADD_ASSIGN = 317,
     SUB_ASSIGN = 318,
     MUL_ASSIGN = 319,
     DIV_ASSIGN = 320,
     MOD_ASSIGN = 321,
     AND_ASSIGN = 322,
     XOR_ASSIGN = 323,
     OR_ASSIGN = 324,
     RIGHT_OP = 325,
     LEFT_OP = 326,
     INC_OP = 327,
     DEC_OP = 328,
     PTR_OP = 329,
     LE_OP = 330,
     GE_OP = 331,
     EQ_OP = 332,
     NE_OP = 333,
     AND_OP = 334,
     OR_OP = 335,
     ELIPSIS = 336,
     IDENTIFIER = 337,
     STRING_LITERAL = 338
   };
#endif
/* Tokens.  */
#define PRAGMA_OMP 258
#define OMP_PARALLEL 259
#define OMP_SECTIONS 260
#define OMP_NOWAIT 261
#define OMP_ORDERED 262
#define OMP_SCHEDULE 263
#define OMP_DYNAMIC 264
#define OMP_GUIDED 265
#define OMP_RUNTIME 266
#define OMP_SECTION 267
#define OMP_SINGLE 268
#define OMP_MASTER 269
#define OMP_CRITICAL 270
#define OMP_BARRIER 271
#define OMP_ATOMIC 272
#define OMP_FLUSH 273
#define OMP_THREADPRIVATE 274
#define OMP_PRIVATE 275
#define OMP_FIRSTPRIVATE 276
#define OMP_LASTPRIVATE 277
#define OMP_SHARED 278
#define OMP_NONE 279
#define OMP_REDUCTION 280
#define OMP_COPYIN 281
#define AUTO 282
#define BREAK 283
#define CASE 284
#define CHAR 285
#define CONST 286
#define CONTINUE 287
#define DEFAULT 288
#define DO 289
#define DOUBLE 290
#define ELSE 291
#define ENUM 292
#define EXTERN 293
#define FLOAT 294
#define FOR 295
#define GOTO 296
#define IF 297
#define INT 298
#define LONG 299
#define REGISTER 300
#define RETURN 301
#define SHORT 302
#define SIGNED 303
#define SIZEOF 304
#define STATIC 305
#define STRUCT 306
#define SWITCH 307
#define TYPEDEF 308
#define UNION 309
#define UNSIGNED 310
#define VOID 311
#define VOLATILE 312
#define WHILE 313
#define CONSTANT 314
#define RIGHT_ASSIGN 315
#define LEFT_ASSIGN 316
#define ADD_ASSIGN 317
#define SUB_ASSIGN 318
#define MUL_ASSIGN 319
#define DIV_ASSIGN 320
#define MOD_ASSIGN 321
#define AND_ASSIGN 322
#define XOR_ASSIGN 323
#define OR_ASSIGN 324
#define RIGHT_OP 325
#define LEFT_OP 326
#define INC_OP 327
#define DEC_OP 328
#define PTR_OP 329
#define LE_OP 330
#define GE_OP 331
#define EQ_OP 332
#define NE_OP 333
#define AND_OP 334
#define OR_OP 335
#define ELIPSIS 336
#define IDENTIFIER 337
#define STRING_LITERAL 338




#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef int YYSTYPE;
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif

extern YYSTYPE yylval;

