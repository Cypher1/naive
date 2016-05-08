#ifndef NAIVE_IR_H_
#define NAIVE_IR_H_

#include "asm.h"
#include "array.h"
#include "misc.h"

typedef struct TransUnit
{
	Array(IrFunction) functions;
} TransUnit;

typedef struct Block
{
	char *name;

	u32 arity;
	struct Arg *args;
	Array(IrInstr) instrs;
} Block;

typedef struct IrFunction
{
	char *name;

	Block entry_block;
	Block ret_block;
} IrFunction;

typedef struct Builder
{
	IrFunction *function;
	Block *current_block;
} Builder;


typedef struct IrType
{
	u32 bit_width;
} IrType;

typedef enum IrOp
{
	OP_BRANCH,
} IrOp;

typedef struct Value
{
	enum
	{
		VALUE_CONST,
		VALUE_ARG,
		VALUE_INSTR,
	} kind;

	union
	{
		u64 constant;
		struct IrInstr *instr;
		struct Arg *arg;
	} val;
} Value;

typedef struct IrInstr
{
	u32 id;
	IrType type;
	IrOp op;

	union
	{
		u64 constant;
		struct
		{
			Block *target_block;
			struct Value argument;
		} branch;
	} val;
} IrInstr;

typedef struct Arg
{
	u32 index;
	IrType type;
} Arg;

void trans_unit_init(TransUnit *tu);
IrFunction *trans_unit_add_function(TransUnit *tu, char *name,
		IrType return_type, u32 arity, IrType *arg_types);

static inline IrType function_return_type(IrFunction *f)
{
	return f->ret_block.args[0].type;
}

void dump_trans_unit(TransUnit *tu);

void builder_init(Builder *builder);
IrInstr *build_branch(Builder *builder, Block *block, Value value);

Value value_const(u64 value);

void generate_asm_module(TransUnit *trans_unit, AsmModule *asm_module);

#endif
