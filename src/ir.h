#ifndef NAIVE_IR_H_
#define NAIVE_IR_H_

#include "asm.h"
#include "array.h"
#include "pool.h"
#include "misc.h"

typedef struct TransUnit
{
	Array(IrGlobal *) globals;

	Pool pool;
} TransUnit;

typedef struct IrBlock
{
	char *name;
	Array(IrInstr *) instrs;

	// used by asm_gen
	AsmLabel *label;
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
		u8 bit_width;
	} val;
} IrType;

typedef struct IrFunction
{
	u32 arity;
	IrType *arg_types;
	IrType return_type;
	Array(IrBlock *) blocks;
	AsmLabel *label;
} IrFunction;

typedef struct IrGlobal
{
	char *name;
	IrType ir_type;
	bool defined;
	AsmGlobal *asm_global;

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


typedef struct IrValue
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
		u32 arg_index;
		IrGlobal *global;
	} val;
} IrValue;

#define IR_OPS \
	X(OP_BIT_XOR), \
	X(OP_MUL), \
	X(OP_ADD), \
	X(OP_EQ), \
	X(OP_CALL), \
	X(OP_LOAD), \
	X(OP_STORE), \
	X(OP_LOCAL), \
	X(OP_RET), \
	X(OP_BRANCH), \
	X(OP_COND),

#define X(x) x
typedef enum IrOp
{
	IR_OPS
} IrOp;
#undef X

typedef struct IrInstr
{
	u32 id;
	IrType type;
	IrOp op;
	i32 vreg_number; // used by asm_gen

	union
	{
		u64 constant;
		IrValue arg;
		struct
		{
			IrValue arg1;
			IrValue arg2;
		} binary_op;
		struct
		{
			IrValue pointer;
			IrType type;
		} load;
		struct
		{
			IrValue pointer;
			IrValue value;
			IrType type;
		} store;
		struct
		{
			IrValue callee;
			u32 arity;
			IrValue *arg_array;
			IrType return_type;
		} call;
		struct
		{
			IrValue condition;
			IrBlock *then_block;
			IrBlock *else_block;
		} cond;
		IrBlock *target_block;
		IrType type;
	} val;
} IrInstr;

void trans_unit_init(TransUnit *tu);
void trans_unit_free(TransUnit *trans_unit);
IrGlobal *trans_unit_add_function(TransUnit *tu, char *name,
		IrType return_type, u32 arity, IrType *arg_types);

IrBlock *add_block_to_function(
		TransUnit *trans_unit, IrFunction *function, char *name);

bool ir_type_eq(IrType a, IrType b);
void dump_ir_type(IrType type);

void dump_trans_unit(TransUnit *tu);

void builder_init(IrBuilder *builder, TransUnit *tu);
IrInstr *build_branch(IrBuilder *builder, IrBlock *block);
IrInstr *build_cond(IrBuilder *builder,
		IrValue condition, IrBlock *then_block, IrBlock *else_block);

IrValue value_const(IrType type, u64 constant);
IrValue value_arg(u32 arg_index, IrType type);
IrValue value_global(IrGlobal *global);

AsmLabel *global_label(IrGlobal *global);

IrValue build_unary_instr(IrBuilder *builder, IrOp op, IrValue arg);
IrValue build_binary_instr(IrBuilder *builder, IrOp op, IrValue arg1, IrValue arg2);
IrValue build_local(IrBuilder *builder, IrType type);
IrValue build_load(IrBuilder *builder, IrValue pointer, IrType type);
IrValue build_store(IrBuilder *builder, IrValue pointer, IrValue value, IrType type);
IrValue build_call(IrBuilder *builder, IrValue callee, IrType return_type, u32 arity,
		IrValue *arg_array);

#endif
