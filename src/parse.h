#ifndef NAIVE_PARSE_H_
#define NAIVE_PARSE_H_

#include "array.h"
#include "misc.h"
#include "tokenise.h"
#include "pool.h"

typedef struct ParseError
{
	Token *encountered_token;
	char *expected;
} ParseError;

#define AST_EXPR_TYPES \
		X(INT_LITERAL_EXPR), \
		X(STRING_LITERAL_EXPR), \
\
		X(IDENTIFIER_EXPR), \
\
		X(STRUCT_DOT_FIELD_EXPR), \
		X(STRUCT_ARROW_FIELD_EXPR), \
\
		X(INDEX_EXPR), \
		X(FUNCTION_CALL), \
		X(POST_INCREMENT_EXPR), \
		X(POST_DECREMENT_EXPR), \
\
		X(PRE_INCREMENT_EXPR), \
		X(PRE_DECREMENT_EXPR), \
		X(ADDRESS_OF_EXPR), \
		X(DEREF_EXPR), \
		X(UNARY_PLUS_EXPR), \
		X(UNARY_MINUS_EXPR), \
		X(BIT_NOT_EXPR), \
		X(LOGICAL_NOT_EXPR), \
\
		X(CAST_EXPR), \
		X(SIZEOF_EXPR_EXPR), \
		X(SIZEOF_TYPE_EXPR), \
\
		X(MULTIPLY_EXPR), \
		X(DIVIDE_EXPR), \
		X(MODULO_EXPR), \
		X(ADD_EXPR), \
		X(MINUS_EXPR), \
		X(LEFT_SHIFT_EXPR), \
		X(RIGHT_SHIFT_EXPR), \
\
		X(LESS_THAN_EXPR), \
		X(GREATER_THAN_EXPR), \
		X(LESS_THAN_OR_EQUAL_EXPR), \
		X(GREATER_THAN_OR_EQUAL_EXPR), \
		X(EQUAL_EXPR), \
		X(NOT_EQUAL_EXPR), \
\
		X(BIT_AND_EXPR), \
		X(BIT_XOR_EXPR), \
		X(BIT_OR_EXPR), \
\
		X(LOGICAL_AND_EXPR), \
		X(LOGICAL_OR_EXPR), \
\
		X(CONDITIONAL_EXPR), \
\
		X(ASSIGN_EXPR), \
		X(MULT_ASSIGN_EXPR), \
		X(DIVIDE_ASSIGN_EXPR), \
		X(MODULO_ASSIGN_EXPR), \
		X(PLUS_ASSIGN_EXPR), \
		X(MINUS_ASSIGN_EXPR), \
		X(LEFT_SHIFT_ASSIGN_EXPR), \
		X(RIGHT_SHIFT_ASSIGN_EXPR), \
		X(BIT_AND_ASSIGN_EXPR), \
		X(BIT_XOR_ASSIGN_EXPR), \
		X(BIT_OR_ASSIGN_EXPR), \
\
		X(COMMA_EXPR),

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
		u64 int_literal;
		char *string_literal;
		char *identifier;
		struct ASTExpr *unary_arg;
		struct ASTTypeName *type;
		struct
		{
			struct ASTExpr *callee;
			struct ASTArgument *arg_list;
		} function_call;
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
			char *field_name;
		} struct_field;
		struct
		{
			struct ASTTypeName *cast_type;
			struct ASTExpr *arg;
		} cast;
	} val;
} ASTExpr;

typedef struct ASTArgument
{
	ASTExpr *expr;
	struct ASTArgument *next;
} ASTArgument;

#define AST_STATEMENT_TYPES \
		X(EMPTY_STATEMENT), \
		X(LABELED_STATEMENT), \
		X(CASE_STATEMENT), \
		X(COMPOUND_STATEMENT), \
		X(EXPR_STATEMENT), \
		X(IF_STATEMENT), \
		X(SWITCH_STATEMENT), \
		X(WHILE_STATEMENT), \
		X(DO_WHILE_STATEMENT), \
		X(FOR_STATEMENT), \
		X(GOTO_STATEMENT), \
		X(CONTINUE_STATEMENT), \
		X(BREAK_STATEMENT), \
		X(RETURN_STATEMENT)

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
			char *label_name;
			struct ASTStatement *statement;
		} labeled_statement;
		struct
		{
			ASTExpr *expr;
			struct ASTStatement *statement;
		} expr_and_statement;
		struct ASTBlockItem *block_item_list;
		struct
		{
			ASTExpr *condition;
			struct ASTStatement *then_statement;
			struct ASTStatement *else_statement;
		} if_statement;
		// @TODO: For loops with declarations.
		struct
		{
			enum
			{
				FOR_INIT_DECL,
				FOR_INIT_EXPR,
			} init_type;
			union
			{
				struct ASTDecl *decl;
				ASTExpr *expr;
			} init;
			ASTExpr *condition;
			ASTExpr *update_expr;
			struct ASTStatement *body;
		} for_statement;
		char *goto_label;
		ASTExpr *expr;
	} val;
} ASTStatement;

typedef struct ASTBlockItem
{
	struct ASTBlockItem *next;

	enum
	{
		BLOCK_ITEM_DECL,
		BLOCK_ITEM_STATEMENT,
	} type;

	union 
	{
		struct ASTDecl *decl;
		ASTStatement *statement;
	} val;
} ASTBlockItem;


typedef struct ASTDesignator
{
	struct ASTDesignator *next;

	enum
	{
		INDEX_DESIGNATOR,
		FIELD_DESIGNATOR,
	} type;

	union
	{
		ASTExpr *index_expr;
		char *field_name;
	} val;
} ASTDesignator;

typedef struct ASTInitializerElement
{
	struct ASTInitializerElement *next;

	ASTDesignator *designator_list;
	struct ASTInitializer *initializer;
} ASTInitializerElement;

typedef struct ASTInitializer
{
	enum
	{
		EXPR_INITIALIZER,
		BRACE_INITIALIZER,
	} type;

	union
	{
		ASTExpr *expr;
		ASTInitializerElement *initializer_element_list;
	} val;
} ASTInitializer;

typedef struct ASTInitDeclarator
{
	struct ASTInitDeclarator *next;

	struct ASTDeclarator *declarator;
	ASTInitializer *initializer;
} ASTInitDeclarator;

typedef struct ASTDecl
{
	struct ASTDecl *next;

	struct ASTDeclSpecifier *decl_specifier_list;
	ASTInitDeclarator *init_declarators;
} ASTDecl;

typedef struct ASTTypeName
{
	struct ASTDeclSpecifier *decl_specifier_list;
	struct ASTDeclarator *declarator;
} ASTTypeName;

typedef struct ASTParameterDecl
{
	struct ASTParameterDecl *next;

	struct ASTDeclSpecifier *decl_specifier_list;
	struct ASTDeclarator *declarator;
} ASTParameterDecl;

typedef struct ASTDirectDeclarator
{
	enum
	{
		DECLARATOR,
		IDENTIFIER_DECLARATOR,
		ARRAY_DECLARATOR,
		FUNCTION_DECLARATOR,
	} type;

	union
	{
		char *name;
		struct ASTDeclarator *declarator;
		struct
		{
			struct ASTDirectDeclarator *element_declarator;
			ASTExpr *array_length;
		} array_declarator;
		struct
		{
			struct ASTDirectDeclarator *declarator;
			ASTParameterDecl *parameters;
		} function_declarator;
	} val;
} ASTDirectDeclarator;

typedef struct ASTDeclarator
{
	enum
	{
		POINTER_DECLARATOR,
		DIRECT_DECLARATOR,
	} type;

	union
	{
		struct
		{
			struct ASTDeclSpecifier *decl_specifier_list;
			struct ASTDeclarator *pointee;
		} pointer_declarator;
		ASTDirectDeclarator *direct_declarator;
	} val;
} ASTDeclarator;

typedef enum ASTStorageClassSpecifier
{
	TYPEDEF_SPECIFIER,
	EXTERN_SPECIFIER,
	STATIC_SPECIFIER,
	AUTO_SPECIFIER,
	REGISTER_SPECIFIER,
} ASTStorageClassSpecifier;

typedef enum ASTTypeQualifier
{
	CONST_QUALIFIER,
	RESTRICT_QUALIFIER,
	VOLATILE_QUALIFIER,
} ASTTypeQualifier;

typedef enum ASTFunctionSpecifier
{
	INLINE_SPECIFIER,
} ASTFunctionSpecifier;

typedef struct ASTEnumerator
{
	struct ASTEnumerator *next;
	char *name;
	ASTExpr *value;
} ASTEnumerator;

typedef struct ASTFieldDeclarator
{
	struct ASTFieldDeclarator *next;

	enum
	{
		BITFIELD_FIELD_DECLARATOR,
		NORMAL_FIELD_DECLARATOR,
	} type;

	union
	{
		struct
		{
			ASTDeclarator *declarator;
			ASTExpr *width;
		} bitfield;
		ASTDeclarator *declarator;
	} val;
} ASTFieldDeclarator;

typedef struct ASTFieldDecl
{
	struct ASTFieldDecl *next;

	struct ASTDeclSpecifier *decl_specifier_list;
	ASTFieldDeclarator *field_declarator_list;
} ASTFieldDecl;

typedef struct ASTTypeSpecifier
{
	enum
	{
		NAMED_TYPE_SPECIFIER,
		STRUCT_TYPE_SPECIFIER,
		UNION_TYPE_SPECIFIER,
		ENUM_TYPE_SPECIFIER,
	} type;

	union
	{
		char *name;
		struct
		{
			char *name;
			ASTFieldDecl *field_list;
		} struct_or_union_specifier;
		struct
		{
			char *name;
			ASTEnumerator *enumerator_list;
		} enum_specifier;
	} val;
} ASTTypeSpecifier;

typedef struct ASTDeclSpecifier
{
	struct ASTDeclSpecifier *next;
	enum
	{
		STORAGE_CLASS_SPECIFIER,
		TYPE_SPECIFIER,
		TYPE_QUALIFIER,
		FUNCTION_SPECIFIER,
	} type;

	union
	{
		ASTStorageClassSpecifier storage_class_specifier;
		ASTTypeSpecifier *type_specifier;
		ASTTypeQualifier type_qualifier;
		ASTFunctionSpecifier function_specifier;
	} val;
} ASTDeclSpecifier;

typedef struct ASTFunctionDef
{
	ASTDeclSpecifier *decl_specifier_list;
	ASTDeclarator *declarator;
	ASTDecl *old_style_param_decl_list;
	ASTStatement *body;
} ASTFunctionDef;

typedef struct ASTToplevel
{
	struct ASTToplevel *next;
	enum
	{
		FUNCTION_DEF,
		DECL,
	} type;

	union
	{
		ASTFunctionDef *function_def;
		ASTDecl *decl;
	} val;
} ASTToplevel;

void dump_toplevel(ASTToplevel *ast);
ASTToplevel *parse_toplevel(Array(SourceToken) *tokens, Pool *ast_pool);

#endif
