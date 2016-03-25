#ifndef NAIVE_PARSE_H_
#define NAIVE_PARSE_H_

#include "array.h"
#include "misc.h"
#include "tokenise.h"
#include "pool.h"

typedef struct ParseError
{
	Token *encountered_token;
	const char *expected;
} ParseError;

typedef struct ASTType
{
	const char *name;
} ASTType;

typedef struct ASTVar
{
	ASTType *type;
	char *name;
} ASTVar;

#define AST_EXPR_TYPES \
		X(AST_INT_LITERAL), \
\
		X(AST_IDENTIFIER), \
\
		X(AST_STRUCT_DOT_FIELD), \
		X(AST_STRUCT_ARROW_FIELD), \
\
		X(AST_INDEX), \
		X(AST_POST_INCREMENT), \
		X(AST_POST_DECREMENT), \
\
		X(AST_PRE_INCREMENT), \
		X(AST_PRE_DECREMENT), \
		X(AST_ADDRESS_OF), \
		X(AST_DEREF), \
		X(AST_UNARY_PLUS), \
		X(AST_UNARY_MINUS), \
		X(AST_BIT_NOT), \
		X(AST_LOGICAL_NOT), \
\
		X(AST_CAST), \
		X(AST_SIZEOF_EXPR), \
		X(AST_SIZEOF_TYPE), \
\
		X(AST_MULTIPLY), \
		X(AST_DIVIDE), \
		X(AST_MODULO), \
		X(AST_ADD), \
		X(AST_MINUS), \
		X(AST_LEFT_SHIFT), \
		X(AST_RIGHT_SHIFT), \
\
		X(AST_LESS_THAN), \
		X(AST_GREATER_THAN), \
		X(AST_LESS_THAN_OR_EQUAL), \
		X(AST_GREATER_THAN_OR_EQUAL), \
		X(AST_EQUAL), \
		X(AST_NOT_EQUAL), \
\
		X(AST_BIT_AND), \
		X(AST_BIT_XOR), \
		X(AST_BIT_OR), \
\
		X(AST_LOGICAL_AND), \
		X(AST_LOGICAL_OR), \
\
		X(AST_CONDITIONAL), \
\
		X(AST_ASSIGN), \
		X(AST_MULT_ASSIGN), \
		X(AST_DIVIDE_ASSIGN), \
		X(AST_MODULO_ASSIGN), \
		X(AST_PLUS_ASSIGN), \
		X(AST_MINUS_ASSIGN), \
		X(AST_LEFT_SHIFT_ASSIGN), \
		X(AST_RIGHT_SHIFT_ASSIGN), \
		X(AST_BIT_AND_ASSIGN), \
		X(AST_BIT_XOR_ASSIGN), \
		X(AST_BIT_OR_ASSIGN),

#define X(x) x
typedef enum ASTExprType
{
	AST_EXPR_TYPES
} ASTExprType;
#undef X


typedef struct ASTExpr
{
	ASTExprType type;

	union
	{
		i64 int_literal;
		const char *identifier;
		struct ASTExpr *unary_arg;
		ASTType *type;
		struct
		{
			struct ASTExpr *arg1;
			struct ASTExpr *arg2;
		} binary_op;
		struct
		{
			struct ASTExpr *arg1;
			struct ASTExpr *arg2;
			struct ASTExpr *arg3;
		} ternary_op;
		struct
		{
			struct ASTExpr *struct_value;
			const char *field_name;
		} struct_field;
		struct
		{
			ASTType *cast_type;
			struct ASTExpr *arg;
		} cast;
	} val;
} ASTExpr;

#define AST_STATEMENT_TYPES \
		X(AST_EMPTY_STATEMENT), \
		X(AST_LABELED_STATEMENT), \
		X(AST_CASE_STATEMENT), \
		X(AST_COMPOUND_STATEMENT), \
		X(AST_EXPR_STATEMENT), \
		X(AST_IF_STATEMENT), \
		X(AST_SWITCH_STATEMENT), \
		X(AST_WHILE_STATEMENT), \
		X(AST_DO_WHILE_STATEMENT), \
		X(AST_FOR_STATEMENT), \
		X(AST_GOTO_STATEMENT), \
		X(AST_CONTINUE_STATEMENT), \
		X(AST_BREAK_STATEMENT), \
		X(AST_RETURN_STATEMENT)

#define X(x) x
typedef enum ASTStatementType
{
	AST_STATEMENT_TYPES
} ASTStatementType;
#undef X

typedef struct ASTStatement
{
	ASTStatementType type;

	union
	{
		struct
		{
			const char *label_name;
			struct ASTStatement *statement;
		} labeled_statement;
		struct
		{
			ASTExpr *expr;
			struct ASTStatement *statement;
		} expr_and_statement;
		struct
		{
			struct ASTStatement **statements;
			u32 num_statements;
		} compound_statement;
		struct
		{
			ASTExpr *condition;
			struct ASTStatement *then_statement;
			struct ASTStatement *else_statement;
		} if_statement;
		// @TODO: For loops with declarations.
		struct
		{
			ASTExpr *init_expr;
			ASTExpr *condition;
			ASTExpr *update_expr;
			struct ASTStatement *body;
		} for_statement;
		const char *goto_label;
		ASTExpr *expr;
	} val;
} ASTStatement;

typedef struct ASTToplevel
{
	enum
	{
		AST_FUNCTION_DEF,
	} type;

	union
	{
		struct
		{
			ASTType *return_type;
			char *name;
			Array(ASTVar *) arguments;
			ASTStatement *body;
		} function_def;
	} val;
} ASTToplevel;

void dump_toplevel(ASTToplevel *ast);
ASTToplevel *parse_toplevel(Array(SourceToken) *tokens, Pool *ast_pool);

#endif
