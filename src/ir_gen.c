#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "array.h"
#include "ir.h"
#include "parse.h"
#include "util.h"

typedef enum CTypeType
{
	INTEGER_TYPE,
	FUNCTION_TYPE,
	STRUCT_TYPE,
	POINTER_TYPE,
	ARRAY_TYPE,
} CTypeType;

typedef struct CType
{
	CTypeType type;

	struct CType *cached_pointer_type;

	union
	{
		struct
		{
			enum
			{
				CHAR,
				SHORT,
				INT,
				LONG,
				LONGLONG,
			} type;
			bool is_signed;
		} integer;
		struct
		{
			struct CType *return_type;
			struct CType **arg_type_array;
			u32 arity;
		} function;
		struct
		{
			Array(CDecl) fields;
			IrType *ir_type;
		} strukt;
		struct CType *pointee_type;
		struct
		{
			struct CType *elem_type;
			u64 size;
			IrType *ir_type;
		} array;
	} val;
} CType;

static IrType c_type_to_ir_type(CType *ctype)
{
	switch (ctype->type) {
	case INTEGER_TYPE: {
		u32 bit_width;
		switch (ctype->val.integer.type) {
		case INT:
			bit_width = 32;
			break;
		default:
			UNIMPLEMENTED;
		}

		return (IrType) {
			.kind = IR_INT,
			.val.bit_width = bit_width,
		};
	}
	case POINTER_TYPE:
		return (IrType) { .kind = IR_POINTER };
	case ARRAY_TYPE:
		return *ctype->val.array.ir_type;
	case FUNCTION_TYPE:
		return (IrType) { .kind = FUNCTION_TYPE };
	case STRUCT_TYPE:
		return *ctype->val.strukt.ir_type;
	}
}

typedef struct Term
{
	CType *ctype;
	IrValue value;
} Term;

typedef struct Binding
{
	char *name;
	Term term;
} Binding;

typedef struct Scope
{
	Array(Binding) bindings;
	struct Scope *parent_scope;
} Scope;

Binding *binding_for_name(Scope *scope, char *name)
{
	for (u32 i = 0; i < scope->bindings.size; i++) {
		Binding *binding = ARRAY_REF(&scope->bindings, Binding, i);
		if (streq(binding->name, name))
			return binding;
	}

	if (scope->parent_scope != NULL) {
		return binding_for_name(scope->parent_scope, name);
	} else {
		return NULL;
	}
}

typedef struct TypeEnvEntry
{
	char *name;
	CType type;
} TypeEnvEntry;

typedef struct TypeEnv
{
	Pool derived_types_pool;
	Array(TypeEnvEntry) struct_types;
	Array(TypeEnvEntry) union_types;
	Array(TypeEnvEntry) enum_types;
	Array(TypeEnvEntry) bare_types;
} TypeEnv;

static void init_type_env(TypeEnv *type_env)
{
	pool_init(&type_env->derived_types_pool, 512);
	ARRAY_INIT(&type_env->struct_types, TypeEnvEntry, 10);
	ARRAY_INIT(&type_env->union_types, TypeEnvEntry, 10);
	ARRAY_INIT(&type_env->enum_types, TypeEnvEntry, 10);

	ARRAY_INIT(&type_env->bare_types, TypeEnvEntry, 10);
	*ARRAY_APPEND(&type_env->bare_types, TypeEnvEntry) =
		(TypeEnvEntry) {
			.name = "int",
			.type = (CType) {
				.type = INTEGER_TYPE,
				.cached_pointer_type = NULL,
				.val.integer.type = INT,
				.val.integer.is_signed = true,
			},
		};
}

static CType *search(Array(CDecl) *types, char *name)
{
	for (u32 i = 0; i < types->size; i++) {
		TypeEnvEntry *entry = ARRAY_REF(types, TypeEnvEntry, i);
		if (streq(entry->name, name)) {
			return &entry->type;
		}
	}

	assert(false);
}

typedef struct CDecl
{
	char *name;
	CType *type;
} CDecl;

static void decl_to_cdecl(IrBuilder *builder, TypeEnv *type_env,
		ASTDeclSpecifier *decl_specifier_list, ASTDeclarator *declarator,
		CDecl *cdecl);

static CType *type_spec_to_c_type(IrBuilder *builder, TypeEnv *type_env,
		ASTTypeSpecifier *type_spec)
{
	switch (type_spec->type) {
	case NAMED_TYPE_SPECIFIER: {
		return search(&type_env->bare_types, type_spec->val.name);
	}
	case STRUCT_TYPE_SPECIFIER: {
		ASTFieldDecl *field_list = type_spec->val.struct_or_union_specifier.field_list;
		char *name = type_spec->val.struct_or_union_specifier.name;

		if (field_list == NULL) {
			assert(name != NULL);
			return search(&type_env->struct_types, name);
		}

		TypeEnvEntry *struct_type =
			ARRAY_APPEND(&type_env->struct_types, TypeEnvEntry);
		if (name == NULL)
			name = "<anonymous struct>";
		struct_type->name = name;

		CType *type = &struct_type->type;
		type->type = STRUCT_TYPE;
		type->cached_pointer_type = NULL;

		Array(CDecl) *fields = &type->val.strukt.fields;
		ARRAY_INIT(fields, CDecl, 5);
		while (field_list != NULL) {
			ASTDeclSpecifier *decl_specs = field_list->decl_specifier_list;
			ASTFieldDeclarator *field_declarator = field_list->field_declarator_list;
			while (field_declarator != NULL) {
				assert(field_declarator->type == NORMAL_FIELD_DECLARATOR);
				ASTDeclarator *declarator = field_declarator->val.declarator;

				CDecl *cdecl = ARRAY_APPEND(fields, CDecl);
				decl_to_cdecl(builder, type_env, decl_specs, declarator, cdecl);

				field_declarator = field_declarator->next;
			}

			field_list = field_list->next;
		}

		IrType *ir_struct =
			trans_unit_add_struct(builder->trans_unit, name, fields->size);
		u32 current_offset = 0;
		for (u32 i = 0; i < fields->size; i++) {
			CDecl *field = ARRAY_REF(fields, CDecl, i);
			IrType field_type = c_type_to_ir_type(field->type);
			u32 field_size = size_of_ir_type(field_type);

			// @TODO: This is stricter than necessary for structs. We should
			// track not only the size of a type, but its alignment
			// requirements.
			current_offset = align_to(current_offset, field_size);

			ir_struct->val.strukt.fields[i].type = field_type;
			ir_struct->val.strukt.fields[i].offset = current_offset;

			current_offset += field_size;
		}
		ir_struct->val.strukt.total_size = current_offset;
		type->val.strukt.ir_type = ir_struct;

		return &struct_type->type;

		break;
	}
	default: UNIMPLEMENTED;
	}
}

typedef struct Env
{
	Scope *scope;
	TypeEnv type_env;
	IrBlock *break_target;
} Env;

static CType *look_up_type(Env *env, char *name)
{
	return search(&env->type_env.bare_types, name);
}

static CType *pointer_type(TypeEnv *type_env, CType *type)
{
	if (type->cached_pointer_type != NULL)
		return type->cached_pointer_type;

	CType *pointer_type =
		pool_alloc(&type_env->derived_types_pool, sizeof *pointer_type);
	pointer_type->type = POINTER_TYPE;
	pointer_type->cached_pointer_type = NULL;
	pointer_type->val.pointee_type = type;

	type->cached_pointer_type = pointer_type;

	return pointer_type;
}

static CType *array_type(IrBuilder *builder, TypeEnv *type_env, CType *type,
		u64 size)
{
	CType *array_type =
		pool_alloc(&type_env->derived_types_pool, sizeof *array_type);
	array_type->type = ARRAY_TYPE;
	array_type->cached_pointer_type = NULL;
	array_type->val.array.elem_type = type;
	array_type->val.array.size = size;

	IrType *ir_array_type =
		pool_alloc(&builder->trans_unit->pool, sizeof *ir_array_type);
	ir_array_type->kind = IR_ARRAY;

	IrType elem_type = c_type_to_ir_type(type);
	IrType *ir_elem_type;

	if (elem_type.kind == IR_ARRAY) {
		size *= elem_type.val.array.size;
		ir_elem_type = elem_type.val.array.elem_type;
	} else {
		ir_elem_type =
			pool_alloc(&builder->trans_unit->pool, sizeof *ir_elem_type);
		*ir_elem_type = c_type_to_ir_type(type);
	}
	ir_array_type->val.array.elem_type = ir_elem_type;
	ir_array_type->val.array.size = size;

	array_type->val.array.ir_type = ir_array_type;

	return array_type;
}

static u64 eval_constant_expr(ASTExpr *constant_expr)
{
	switch (constant_expr->type) {
	case INT_LITERAL_EXPR:
		return constant_expr->val.int_literal;
	default:
		UNIMPLEMENTED;
	}
}

static void direct_declarator_to_cdecl(IrBuilder *builder, TypeEnv *type_env,
		ASTDeclSpecifier *decl_specifier_list,
		ASTDirectDeclarator *direct_declarator, CDecl *cdecl) {
	switch (direct_declarator->type) {
	case FUNCTION_DECLARATOR: {
		ASTDirectDeclarator *function_declarator =
			direct_declarator->val.function_declarator.declarator;
		assert(function_declarator->type == IDENTIFIER_DECLARATOR);

		cdecl->name = function_declarator->val.name;

		assert(decl_specifier_list->next == NULL);
		assert(decl_specifier_list->type == TYPE_SPECIFIER);
		CType *return_c_type =
			type_spec_to_c_type(builder, type_env, decl_specifier_list->val.type_specifier);

		ASTParameterDecl *first_param =
			direct_declarator->val.function_declarator.parameters;
		ASTParameterDecl *params = first_param;

		u32 arity = 0;
		while (params != NULL) {
			arity++;
			params = params->next;
		}

		params = first_param;

		CType **arg_c_types = pool_alloc(
				&builder->trans_unit->pool,
				sizeof(*arg_c_types) * arity);

		for (u32 i = 0; i < arity; i++) {
			CDecl cdecl;
			decl_to_cdecl(builder, type_env, params->decl_specifier_list,
					params->declarator, &cdecl);
			arg_c_types[i] = cdecl.type;

			params = params->next;
		}

		CType *ctype = pool_alloc(&builder->trans_unit->pool, sizeof *ctype);
		ctype->type = FUNCTION_TYPE;
		ctype->val.function.arity = arity;
		ctype->val.function.arg_type_array = arg_c_types;
		ctype->val.function.return_type = return_c_type;

		cdecl->type = ctype;
		break;
	}
	case IDENTIFIER_DECLARATOR: {
		cdecl->name = direct_declarator->val.name;
		assert(decl_specifier_list->next == NULL);
		assert(decl_specifier_list->type == TYPE_SPECIFIER);

		ASTTypeSpecifier *type_spec = decl_specifier_list->val.type_specifier;
		cdecl->type = type_spec_to_c_type(builder, type_env, type_spec);
		break;
	}
	case ARRAY_DECLARATOR: {
		ASTDirectDeclarator *elem_declarator =
			direct_declarator->val.array_declarator.element_declarator;
		ASTExpr *array_length_expr =
			direct_declarator->val.array_declarator.array_length;

		CDecl elem_cdecl;
		direct_declarator_to_cdecl(builder, type_env, decl_specifier_list,
				elem_declarator, &elem_cdecl);
		cdecl->name = elem_cdecl.name;
		cdecl->type = array_type(builder, type_env, elem_cdecl.type,
				eval_constant_expr(array_length_expr));
		break;
	} 
	default:
		UNIMPLEMENTED;
	}
}

static void decl_to_cdecl(IrBuilder *builder, TypeEnv *type_env,
		ASTDeclSpecifier *decl_specifier_list, ASTDeclarator *declarator,
		CDecl *cdecl)
{
	if (declarator->type == POINTER_DECLARATOR) {
		CDecl pointee_cdecl;
		assert(declarator->val.pointer_declarator.decl_specifier_list == NULL);
		decl_to_cdecl(builder, type_env, decl_specifier_list,
				declarator->val.pointer_declarator.pointee, &pointee_cdecl);
		cdecl->name = pointee_cdecl.name;
		cdecl->type = pointer_type(type_env, pointee_cdecl.type);
	} else {
		assert(declarator->type == DIRECT_DECLARATOR);
		ASTDirectDeclarator *direct_declarator = declarator->val.direct_declarator;

		direct_declarator_to_cdecl(builder, type_env, decl_specifier_list,
				direct_declarator, cdecl);

	}
}

static void cdecl_to_binding(IrBuilder *builder, CDecl *cdecl, Binding *binding)
{
	IrType ir_type = c_type_to_ir_type(cdecl->type);

	binding->name = cdecl->name;
	binding->term.ctype = cdecl->type;
	binding->term.value = build_local(builder, ir_type);
}

static inline IrBlock *add_block(IrBuilder *builder, char *name)
{
	return add_block_to_function(builder->trans_unit, builder->current_function, name);
}

static IrGlobal *ir_global_for_decl(IrBuilder *builder, TypeEnv *type_env,
		ASTDeclSpecifier *decl_specifier_list, ASTDeclarator *declarator,
		CType **result_c_type)
{
	CDecl cdecl;
	decl_to_cdecl(builder, type_env, decl_specifier_list, declarator, &cdecl);
	CType *ctype = cdecl.type;
	if (ctype->type == FUNCTION_TYPE) {
		u32 arity = ctype->val.function.arity;
		IrType *arg_ir_types = malloc(sizeof(*arg_ir_types) * arity);
		
		for (u32 i = 0; i < arity; i++) {
			CType *arg_c_type = cdecl.type->val.function.arg_type_array[i];
			arg_ir_types[i] = c_type_to_ir_type(arg_c_type);
		}

		IrGlobal *global = NULL;
		Array(IrGlobal *) *globals = &builder->trans_unit->globals;
		for (u32 i = 0; i < globals->size; i++) {
			IrGlobal *curr_global = *ARRAY_REF(globals, IrGlobal *, i);
			if (streq(curr_global->name, cdecl.name)) {
				// @TODO: Check C type matches
				global = curr_global;
				break;
			}
		}
		
		if (global == NULL) {
			global = trans_unit_add_function(
					builder->trans_unit, cdecl.name,
					c_type_to_ir_type(ctype->val.function.return_type),
					arity, arg_ir_types);
		}

		assert(global->kind == IR_GLOBAL_FUNCTION);
		*result_c_type = ctype;

		return global;
	} else {
		CDecl cdecl;
		decl_to_cdecl(builder, type_env, decl_specifier_list, declarator, &cdecl);

		IrGlobal *global = NULL;
		Array(IrGlobal *) *globals = &builder->trans_unit->globals;
		for (u32 i = 0; i < globals->size; i++) {
			IrGlobal *curr_global = *ARRAY_REF(globals, IrGlobal *, i);
			if (streq(curr_global->name, cdecl.name)) {
				// @TODO: Check C type matches
				global = curr_global;
				break;
			}
		}

		if (global == NULL) {
			global = trans_unit_add_var(
					builder->trans_unit,
					cdecl.name,
					c_type_to_ir_type(ctype));
		}

		*result_c_type = cdecl.type;

		return global;
	}
}

static void ir_gen_statement(IrBuilder *builder, Env *env, ASTStatement *statement);

typedef enum ExprContext
{
	LVALUE_CONTEXT,
	RVALUE_CONTEXT,
} ExprContext;
static Term ir_gen_expression(IrBuilder *builder, Env *env, ASTExpr *expr,
		ExprContext context);

void ir_gen_toplevel(IrBuilder *builder, ASTToplevel *toplevel)
{
	Scope global_scope;
	global_scope.parent_scope = NULL;
	Array(Binding)* global_bindings = &global_scope.bindings;
	ARRAY_INIT(global_bindings, Binding, 10);

	Env env;
	init_type_env(&env.type_env);
	env.scope = &global_scope;
	env.break_target = NULL;

	while (toplevel != NULL) {
		IrGlobal *global = NULL;
		CType *global_type;
		ZERO_STRUCT(&global_type);

		switch (toplevel->type) {
		case FUNCTION_DEF: {
			ASTFunctionDef *func = toplevel->val.function_def;
			ASTDeclSpecifier *decl_specifier_list = func->decl_specifier_list;
			ASTDeclarator *declarator = func->declarator;

			global = ir_global_for_decl(builder, &env.type_env,
					decl_specifier_list, declarator, &global_type);
			assert(!global->defined);
			IrFunction *function = &global->val.function;

			builder->current_function = function;
			builder->current_block = *ARRAY_REF(&function->blocks, IrBlock *, 0);

			Scope scope;
			scope.parent_scope = &global_scope;
			Array(Binding) *param_bindings = &scope.bindings;
			ARRAY_INIT(param_bindings, Binding, 5);
			env.scope = &scope;

			ASTDirectDeclarator *direct_declarator =
				declarator->val.direct_declarator;
			assert(direct_declarator->type == FUNCTION_DECLARATOR);

			ASTParameterDecl *param =
				direct_declarator->val.function_declarator.parameters;
			for (u32 i = 0; param != NULL; i++, param = param->next) {
				Binding *binding = ARRAY_APPEND(param_bindings, Binding);

				CDecl cdecl;
				decl_to_cdecl(builder, &env.type_env, param->decl_specifier_list,
						param->declarator, &cdecl);
				cdecl_to_binding(builder, &cdecl, binding);

				build_store(builder,
						binding->term.value,
						value_arg(i, function->arg_types[i]),
						c_type_to_ir_type(binding->term.ctype));
			}

			ir_gen_statement(builder, &env, func->body);

			env.scope = env.scope->parent_scope;
			array_free(param_bindings);
			global->defined = true;

			break;
		}
		case DECL: {
			ASTDecl *decl = toplevel->val.decl;
			ASTDeclSpecifier *decl_specifier_list = decl->decl_specifier_list;
			ASTInitDeclarator *init_declarator = decl->init_declarators;
			assert(decl_specifier_list != NULL);

			Array(TypeEnvEntry) *bare_types = &env.type_env.bare_types;

			if (decl_specifier_list->type == STORAGE_CLASS_SPECIFIER &&
					decl_specifier_list->val.storage_class_specifier == TYPEDEF_SPECIFIER) {
				assert(init_declarator != NULL);
				decl_specifier_list = decl_specifier_list->next;

				while (init_declarator != NULL) {
					assert(init_declarator->initializer == NULL);
					CDecl cdecl;
					decl_to_cdecl(builder, &env.type_env, decl_specifier_list,
							init_declarator->declarator, &cdecl);

					TypeEnvEntry *new_type_alias = ARRAY_APPEND(bare_types, TypeEnvEntry);
					new_type_alias->name = cdecl.name;
					new_type_alias->type = *cdecl.type;

					init_declarator = init_declarator->next;
				}
			} else if (init_declarator == NULL) {
				assert(decl_specifier_list->next == NULL);
				assert(decl_specifier_list->type == TYPE_SPECIFIER);
				ASTTypeSpecifier *type_spec = decl_specifier_list->val.type_specifier;
				type_spec_to_c_type(builder, &env.type_env, type_spec);
			} else {
				assert(init_declarator->initializer == NULL);
				assert(init_declarator->next == NULL);
				ASTDeclarator *declarator = init_declarator->declarator;

				// @TODO: Multiple declarators in one global decl.
				global = ir_global_for_decl(builder, &env.type_env,
						decl_specifier_list, declarator, &global_type);
				global->defined = global_type->type != FUNCTION_TYPE;
			}

			break;
		}
		}

		if (global != NULL) {
			Binding *binding = ARRAY_APPEND(global_bindings, Binding);
			binding->name = global->name;
			binding->term.ctype = global_type;
			binding->term.value = value_global(global);
		}

		toplevel = toplevel->next;
	}

	pool_free(&env.type_env.derived_types_pool);
	array_free(&env.type_env.struct_types);
	array_free(&env.type_env.union_types);
	array_free(&env.type_env.enum_types);
	array_free(&env.type_env.bare_types);
	array_free(global_bindings);
}

static void add_decl_to_scope(IrBuilder *builder, Env *env, ASTDecl *decl)
{
	ASTInitDeclarator *init_declarator = decl->init_declarators;
	while (init_declarator != NULL) {
		CDecl cdecl;
		decl_to_cdecl(builder, &env->type_env, decl->decl_specifier_list,
				init_declarator->declarator,
				&cdecl);

		Binding *binding = ARRAY_APPEND(&env->scope->bindings, Binding);
		cdecl_to_binding(builder, &cdecl, binding);

		ASTInitializer *initializer = init_declarator->initializer;
		if (initializer != NULL) {
			assert(initializer->type == EXPR_INITIALIZER);
			Term init_term = ir_gen_expression(builder, env,
					initializer->val.expr, RVALUE_CONTEXT);
			build_store(
				builder,
				binding->term.value,
				init_term.value,
				c_type_to_ir_type(binding->term.ctype));
		}

		init_declarator = init_declarator->next;
	}
}

static void ir_gen_statement(IrBuilder *builder, Env *env, ASTStatement *statement)
{
	switch (statement->type) {
	case COMPOUND_STATEMENT: {
		Scope block_scope;
		block_scope.parent_scope = env->scope;
		ARRAY_INIT(&block_scope.bindings, Binding, 5);
		env->scope = &block_scope;

		ASTBlockItem *block_item_list = statement->val.block_item_list;
		while (block_item_list != NULL) {
			switch (block_item_list->type) {
			case BLOCK_ITEM_DECL: {
				add_decl_to_scope(builder, env, block_item_list->val.decl);
				break;
			}
			case BLOCK_ITEM_STATEMENT:
				ir_gen_statement(builder, env, block_item_list->val.statement);
				break;
			}

			block_item_list = block_item_list->next;
		}

		env->scope = env->scope->parent_scope;

		break;
	}
	case EXPR_STATEMENT: {
		ir_gen_expression(builder, env, statement->val.expr, RVALUE_CONTEXT);
		break;
	}
	case RETURN_STATEMENT: {
		Term term = ir_gen_expression(builder, env,
				statement->val.expr, RVALUE_CONTEXT);
		build_unary_instr(builder, OP_RET, term.value);
		break;
	}
	case IF_STATEMENT: {
		IrBlock *initial_block = builder->current_block;
		IrBlock *then_block = add_block(builder, "if.then");
		IrBlock *after_block = add_block(builder, "if.after");

		ASTStatement *then_statement = statement->val.if_statement.then_statement;
		builder->current_block = then_block;
		ir_gen_statement(builder, env, then_statement);
		build_branch(builder, after_block);

		ASTStatement *else_statement = statement->val.if_statement.else_statement;
		IrBlock *else_block = NULL;
		if (else_statement != NULL) {
			else_block = add_block(builder, "if.else");
			builder->current_block = else_block;
			ir_gen_statement(builder, env, else_statement);
			build_branch(builder, after_block);
		}

		builder->current_block = initial_block;
		ASTExpr *condition_expr = statement->val.if_statement.condition;
		Term condition_term =
			ir_gen_expression(builder, env, condition_expr, RVALUE_CONTEXT);
		assert(condition_term.ctype->type == INTEGER_TYPE);

		if (else_statement == NULL) {
			build_cond(builder, condition_term.value, then_block, after_block);
		} else {
			build_cond(builder, condition_term.value, then_block, else_block);
		}

		builder->current_block = after_block;
		break;
	}
	case WHILE_STATEMENT: {
		IrBlock *pre_header = add_block(builder, "while.ph");
		IrBlock *body = add_block(builder, "while.body");
		IrBlock *after = add_block(builder, "while.after");

		ASTExpr *condition_expr = statement->val.expr_and_statement.expr;
		ASTStatement *body_statement = statement->val.expr_and_statement.statement;

		build_branch(builder, pre_header);
		builder->current_block = pre_header;
		Term condition_term =
			ir_gen_expression(builder, env, condition_expr, RVALUE_CONTEXT);

		assert(condition_term.ctype->type == INTEGER_TYPE);
		build_cond(builder, condition_term.value, body, after);

		IrBlock *prev_break_target = env->break_target;
		env->break_target = after;
		builder->current_block = body;

		ir_gen_statement(builder, env, body_statement);

		build_branch(builder, pre_header);
		env->break_target = prev_break_target;

		builder->current_block = after;

		break;
	}
	case FOR_STATEMENT: {
		IrBlock *pre_header = add_block(builder, "for.ph");
		IrBlock *body = add_block(builder, "for.body");
		IrBlock *after = add_block(builder, "for.after");

		Scope init_scope;
		Scope *prev_scope = env->scope;

		ASTForStatement *f = &statement->val.for_statement;
		if (f->init_type == FOR_INIT_DECL) {
			init_scope.parent_scope = env->scope;
			ARRAY_INIT(&init_scope.bindings, Binding, 1);
			env->scope = &init_scope;
			add_decl_to_scope(builder, env, f->init.decl);
			env->scope = env->scope->parent_scope;

			env->scope = &init_scope;
		} else {
			assert(f->init_type == FOR_INIT_EXPR);
			if (f->init.expr != NULL)
				ir_gen_expression(builder, env, f->init.expr, RVALUE_CONTEXT);
		}

		build_branch(builder, pre_header);
		builder->current_block = pre_header;
		Term condition_term =
			ir_gen_expression(builder, env, f->condition, RVALUE_CONTEXT);

		assert(condition_term.ctype->type == INTEGER_TYPE);
		build_cond(builder, condition_term.value, body, after);

		builder->current_block = body;
		IrBlock *prev_break_target = env->break_target;
		env->break_target = after;

		ir_gen_statement(builder, env, f->body);
		if (f->update_expr != NULL)
			ir_gen_expression(builder, env, f->update_expr, RVALUE_CONTEXT);

		env->scope = prev_scope;
		env->break_target = prev_break_target;
		build_branch(builder, pre_header);

		builder->current_block = after;

		break;
	}
	case BREAK_STATEMENT:
		assert(env->break_target != NULL);
		build_branch(builder, env->break_target);
		break;
	default:
		UNIMPLEMENTED;
	}
}

static Term ir_gen_struct_field(IrBuilder *builder, Term struct_term,
		char *field_name, ExprContext context)
{
		assert(struct_term.value.type.kind == IR_POINTER);

		CType *ctype = struct_term.ctype;
		if (struct_term.ctype->type == POINTER_TYPE) {
			ctype = ctype->val.pointee_type;
		}

		assert(ctype->type == STRUCT_TYPE);
		Array(CDecl) *fields = &ctype->val.strukt.fields;
		CDecl *selected_field = NULL;
		u32 field_number;
		for (u32 i = 0; i < fields->size; i++) {
			CDecl *field = ARRAY_REF(fields, CDecl, i);
			if (streq(field->name, field_name)) {
				selected_field = field;
				field_number = i;
				break;
			}
		}
		assert(selected_field != NULL);

		IrValue value = build_field(builder, struct_term.value,
				*ctype->val.strukt.ir_type, field_number);
		IrType *struct_ir_type = ctype->val.strukt.ir_type;
		assert(struct_ir_type->kind == IR_STRUCT);
		IrType field_type = struct_ir_type->val.strukt.fields[field_number].type;

		if (context == RVALUE_CONTEXT
				&& selected_field->type->type != STRUCT_TYPE
				&& selected_field->type->type != ARRAY_TYPE) {
			value = build_load(builder, value, field_type);
		}

		return (Term) { .ctype = selected_field->type, .value = value };
}

static Term ir_gen_binary_operator(IrBuilder *builder, Env *env, ASTExpr *expr,
		IrOp ir_op)
{
	// @TODO: Determine type correctly.
	CType *result_type = look_up_type(env, "int");

	IrValue value = build_binary_instr(
			builder,
			ir_op,
			ir_gen_expression(builder, env, expr->val.binary_op.arg1, RVALUE_CONTEXT).value,
			ir_gen_expression(builder, env, expr->val.binary_op.arg2, RVALUE_CONTEXT).value);

	return (Term) { .ctype = result_type, .value = value };
}

static inline bool converts_to_pointer(CType *type)
{
	return type->type == POINTER_TYPE || type->type == ARRAY_TYPE;
}

static Term ir_gen_add(IrBuilder *builder, Env *env, ASTExpr *expr)
{
	Term arg1 =
		ir_gen_expression(builder, env, expr->val.binary_op.arg1, RVALUE_CONTEXT);
	Term arg2 =
		ir_gen_expression(builder, env, expr->val.binary_op.arg2, RVALUE_CONTEXT);

	if (arg1.ctype->type == INTEGER_TYPE && arg2.ctype->type == INTEGER_TYPE) {
		IrValue value = build_binary_instr(builder, OP_ADD, arg1.value, arg2.value);

		// @TODO: Determine type correctly
		return (Term) {
			.ctype = arg1.ctype,
			.value = value,
		};
	} else if (converts_to_pointer(arg1.ctype)
			^ converts_to_pointer(arg2.ctype)) {
		Term pointee = converts_to_pointer(arg1.ctype) ? arg1 : arg2;
		Term other = converts_to_pointer(arg1.ctype) ? arg2 : arg1;
		assert(other.ctype->type == INTEGER_TYPE);

		CType *result_type = pointee.ctype;
		if (result_type->type == ARRAY_TYPE) {
			result_type =
				pointer_type(&env->type_env, result_type->val.array.elem_type);
		}

		// @TODO: Determine type correctly
		// @TODO: Don't hardcode in the size of a pointer!
		IrType pointer_int_type = (IrType) { .kind = IR_INT, .val.bit_width = 64 };

		IrValue zext =
			build_type_instr(builder, OP_ZEXT, other.value, pointer_int_type);
		IrValue ptr_to_int =
			build_type_instr(builder, OP_CAST, pointee.value, pointer_int_type);
		IrValue addend = build_binary_instr(
				builder,
				OP_MUL,
				zext,
				value_const(pointer_int_type,
					size_of_ir_type(c_type_to_ir_type(result_type))));

		IrValue sum = build_binary_instr(builder, OP_ADD, ptr_to_int, addend);
		IrValue int_to_ptr = build_type_instr(builder, OP_CAST, sum,
				c_type_to_ir_type(result_type));

		return (Term) {
			.ctype = result_type,
			.value = int_to_ptr,
		};
	} else {
		UNIMPLEMENTED;
	}
}

static Term ir_gen_deref(IrBuilder *builder, Term pointer, ExprContext context)
{
	// @TODO: Function which does this. Use for IDENTIFIER_EXPR as well.
	assert(pointer.ctype->type == POINTER_TYPE
			|| pointer.ctype->type == ARRAY_TYPE);
	CType *pointee_type = pointer.ctype->val.pointee_type;

	IrValue value;
	if (context == LVALUE_CONTEXT) {
		value = pointer.value;
	} else {
		assert(context == RVALUE_CONTEXT);

		value = build_load(
				builder,
				pointer.value,
				c_type_to_ir_type(pointee_type));
	}

	return (Term) { .ctype = pointee_type, .value = value };
}

static Term ir_gen_expression(IrBuilder *builder, Env *env, ASTExpr *expr,
		ExprContext context)
{
	IGNORE(builder);

	if (context == LVALUE_CONTEXT) {
		ASTExprType type = expr->type;
		assert(type == IDENTIFIER_EXPR
				|| type == STRUCT_DOT_FIELD_EXPR
				|| type == STRUCT_ARROW_FIELD_EXPR
				|| type == INDEX_EXPR
				|| type == DEREF_EXPR);
	}

	switch (expr->type) {
	case IDENTIFIER_EXPR: {
		Binding *binding = binding_for_name(env->scope, expr->val.identifier);
		assert(binding != NULL);
		IrValue value;
		IrType ir_type = c_type_to_ir_type(binding->term.ctype);

		// Functions, arrays, and structs implicitly have their address taken.
		if (context == LVALUE_CONTEXT
				|| binding->term.ctype->type == FUNCTION_TYPE
				|| binding->term.ctype->type == ARRAY_TYPE
				|| binding->term.ctype->type == STRUCT_TYPE) {
			value = binding->term.value;
		} else {
			assert(context == RVALUE_CONTEXT);
			value = build_load(
					builder,
					binding->term.value,
					ir_type);
		}

		return (Term) { .ctype = binding->term.ctype, .value = value };
	}
	case STRUCT_ARROW_FIELD_EXPR: {
		ASTExpr *struct_expr = expr->val.struct_field.struct_expr;
		Term struct_term =
			ir_gen_expression(builder, env, struct_expr, RVALUE_CONTEXT);
		assert(struct_term.ctype->type == POINTER_TYPE);
		assert(struct_term.ctype->val.pointee_type->type == STRUCT_TYPE);

		return ir_gen_struct_field(builder, struct_term,
				expr->val.struct_field.field_name, context);
	}
	case STRUCT_DOT_FIELD_EXPR: {
		ASTExpr *struct_expr = expr->val.struct_field.struct_expr;
		Term struct_term =
			ir_gen_expression(builder, env, struct_expr, RVALUE_CONTEXT);
		assert(struct_term.ctype->type == STRUCT_TYPE);

		return ir_gen_struct_field(builder, struct_term,
				expr->val.struct_field.field_name, context);
	}
	case ADDRESS_OF_EXPR: {
		ASTExpr *inner_expr = expr->val.unary_arg;
		return ir_gen_expression(builder, env, inner_expr, LVALUE_CONTEXT);
	}
	case DEREF_EXPR: {
		ASTExpr *inner_expr = expr->val.unary_arg;
		Term pointer = ir_gen_expression(builder, env, inner_expr, RVALUE_CONTEXT);
		return ir_gen_deref(builder, pointer, context);
	}
	case INDEX_EXPR: {
		Term pointer = ir_gen_add(builder, env, expr);
		assert(pointer.ctype->type == POINTER_TYPE);
		return ir_gen_deref(builder, pointer, context);
	}
	case INT_LITERAL_EXPR: {
		// @TODO: Determine types of constants correctly.
		CType *result_type = look_up_type(env, "int");

		IrValue value = value_const(
				(IrType) { .kind = IR_INT, .val.bit_width = 32 },
				expr->val.int_literal);

		return (Term) { .ctype = result_type, .value = value };
	}
	case ADD_EXPR:
		return ir_gen_add(builder, env, expr);
	case BIT_XOR_EXPR:
		return ir_gen_binary_operator(builder, env, expr, OP_BIT_XOR);
	case MULTIPLY_EXPR:
		return ir_gen_binary_operator(builder, env, expr, OP_MUL);
	case EQUAL_EXPR:
		return ir_gen_binary_operator(builder, env, expr, OP_EQ);
	case NOT_EQUAL_EXPR:
		return ir_gen_binary_operator(builder, env, expr, OP_NEQ);
	case FUNCTION_CALL_EXPR: {
		Term callee = ir_gen_expression(builder, env,
				expr->val.function_call.callee, RVALUE_CONTEXT);

		u32 arity = 0;
		ASTArgument *arg = expr->val.function_call.arg_list;
		while (arg != NULL) {
			arity++;
			arg = arg->next;
		}

		assert(callee.ctype->type == FUNCTION_TYPE);

		CType *return_type = callee.ctype->val.function.return_type;
		IrValue *arg_array = pool_alloc(&builder->trans_unit->pool,
				arity * sizeof(*arg_array));

		arg = expr->val.function_call.arg_list;
		for (u32 i = 0; arg != NULL; i++, arg = arg->next) {
			Term arg_term = ir_gen_expression(builder, env, arg->expr, RVALUE_CONTEXT);
			arg_array[i] = arg_term.value;
		}

		IrValue value = build_call(
				builder,
				callee.value,
				c_type_to_ir_type(return_type),
				arity,
				arg_array);

		return (Term) { .ctype = return_type, .value = value, };
	}
	case ASSIGN_EXPR: {
		ASTExpr *lhs = expr->val.binary_op.arg1;
		ASTExpr *rhs = expr->val.binary_op.arg2;

		Term lhs_ptr = ir_gen_expression(builder, env, lhs, LVALUE_CONTEXT);
		Term rhs_term = ir_gen_expression(builder, env, rhs, RVALUE_CONTEXT);

		build_store(
				builder,
				lhs_ptr.value,
				rhs_term.value,
				c_type_to_ir_type(lhs_ptr.ctype));
		return rhs_term;
	}
	case COMMA_EXPR:
		ir_gen_expression(builder, env, expr->val.binary_op.arg1, RVALUE_CONTEXT);
		return ir_gen_expression(builder, env, expr->val.binary_op.arg2, RVALUE_CONTEXT);
	default:
		printf("%d\n", expr->type);
		UNIMPLEMENTED;
	}
}
