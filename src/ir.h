#ifndef NAIVE_IR_H_
#define NAIVE_IR_H_

#include "asm.h"
#include "array.h"
#include "pool.h"
#include "misc.h"

typedef struct TransUnit
{
	Array(IrGlobal) globals;

	Pool pool;
} TransUnit;

typedef struct IrBlock
{
	char *name;

	u32 arity;
	struct IrArg *args;
	Array(IrInstr *) instrs;
} IrBlock;

typedef struct IrType
{
	enum
	{
		IR_INT,
		IR_POINTER,
		IR_FUNCTION,
	} kind;

	union
	{
		u32 bit_width;
	} val;
} IrType;

typedef struct IrFunction
{
	IrBlock entry_block;
	IrBlock ret_block;
} IrFunction;

typedef struct IrGlobal
{
	char *name;
	IrType ir_type;
	u32 id;

	enum
	{
		IR_GLOBAL_SCALAR,
		IR_GLOBAL_FUNCTION,
	} kind;

	union
	{
		IrFunction function;
	} val;
} IrGlobal;

typedef struct IrBuilder
{
	TransUnit *trans_unit;
	IrFunction *current_function;
	IrBlock *current_block;
} IrBuilder;


typedef struct Value
{
	enum
	{
		VALUE_CONST,
		VALUE_ARG,
		VALUE_INSTR,
		VALUE_GLOBAL,
	} kind;
	// @TODO: Should this be removed? It's contained in most (all?) of the
	// union members below; we could just write a function which extracts it
	// instead of duplicating it.
	IrType type;

	union
	{
		u64 constant;
		struct IrInstr *instr;
		struct IrArg *arg;
		// @TODO: Make this into a pointer instead?
		u32 global_id;
	} val;
} Value;

typedef enum IrOp
{
	OP_BIT_XOR,
	OP_IMUL,

	OP_CALL,

	OP_LOAD,
	OP_STORE,
	OP_BRANCH,
	OP_LOCAL,
} IrOp;

typedef struct IrInstr
{
	u32 id;
	IrType type;
	IrOp op;
	i32 virtual_register; // used by asm_gen

	union
	{
		u64 constant;
		struct
		{
			IrBlock *target_block;
			Value argument;
		} branch;
		struct
		{
			Value arg1;
			Value arg2;
		} binary_op;
		struct
		{
			Value pointer;
			IrType type;
		} load;
		struct
		{
			Value pointer;
			Value value;
			IrType type;
		} store;
		struct
		{
			Value callee;
			u32 arity;
			Value *arg_array;
			IrType return_type;
		} call;
		IrType type;
	} val;
} IrInstr;

typedef struct IrArg
{
	u32 index;
	IrType type;

	i32 virtual_register; // used by asm_gen
} IrArg;

void trans_unit_init(TransUnit *tu);
void trans_unit_free(TransUnit *trans_unit);
IrGlobal *trans_unit_add_function(TransUnit *tu, char *name,
		IrType return_type, u32 arity, IrType *arg_types);

static inline IrType ir_function_return_type(IrFunction *f)
{
	return f->ret_block.args[0].type;
}

bool ir_type_eq(IrType a, IrType b);
void dump_ir_type(IrType type);

void dump_trans_unit(TransUnit *tu);

void builder_init(IrBuilder *builder, TransUnit *tu);
IrInstr *build_branch(IrBuilder *builder, IrBlock *block, Value value);

Value value_const(IrType type, u64 constant);
Value value_arg(IrArg *arg);
Value value_global(IrGlobal *global);

Value build_binary_instr(IrBuilder *builder, IrOp op, Value arg1, Value arg2);
Value build_local(IrBuilder *builder, IrType type);
Value build_load(IrBuilder *builder, Value pointer, IrType type);
Value build_store(IrBuilder *builder, Value pointer, Value value, IrType type);
Value build_call(IrBuilder *builder, Value callee, IrType return_type, u32 arity,
		Value *arg_array);

#endif
