#ifndef NAIVE_ASM_H_
#define NAIVE_ASM_H_

#include <stdio.h>

#include "array.h"
#include "pool.h"

// @TODO: We need to unify registers that overlap like rax, eax, al, ah.
#define PHYSICAL_REGISTERS \
	X(INVALID_REGISTER), \
	X(RAX), \
	X(RBX), \
	X(RCX), \
	X(RDX), \
	X(RDI), \
	X(RSI), \
	X(RBP), \
	X(RSP), \
	X(R8), \
	X(R9), \
	X(R10), \
	X(R11), \
	X(R12), \
	X(R13), \
	X(R14), \
	X(R15),

#define X(x) x
typedef enum PhysicalRegister
{
	PHYSICAL_REGISTERS
} PhysicalRegister;
#undef X

typedef struct Register
{
	enum
	{
		PHYSICAL_REGISTER,
		VIRTUAL_REGISTER,
	} type;

	union
	{
		u32 register_number;
		PhysicalRegister physical_register;
	} val;
} Register;

// @TODO: Merge this and AsmFunction, just like in the IR?
typedef struct AsmGlobal
{
	char *name;
	i32 offset;
	struct AsmSymbol *symbol;
} AsmGlobal;

typedef struct AsmArg
{
	// @TODO: This is kinda messy considering that OFFSET_REGISTER doesn't make
	// sense if this isn't set.
	bool is_deref;

	enum
	{
		REGISTER,
		OFFSET_REGISTER,

		// @TODO: Do we want to encode the size of the immediate here rather
		// than just having one type for all constants? Seems unnecessary.
		CONST8,
		CONST16,
		CONST32,
		CONST64,
		GLOBAL,
	} type;

	union
	{
		Register reg;
		struct
		{
			Register reg;
			u64 offset;
		} offset_register;
		u64 constant;
		AsmGlobal *global;
	} val;
} AsmArg;

#define ASM_OPS \
	X(MOV), \
	X(RET), \
	X(CALL), \
	X(XOR), \
	X(ADD), \
	X(SUB), \
	X(PUSH), \
	X(POP), \
	X(IMUL)

#define X(x) x
typedef enum AsmOp
{
	ASM_OPS
} AsmOp;
#undef X

typedef struct AsmInstr
{
	AsmOp op;
	u32 num_args;
	AsmArg args[2];
} AsmInstr;

typedef struct AsmFunction
{
	Array(AsmInstr) instrs;
	char *name;
} AsmFunction;

typedef struct GlobalReference
{
	AsmGlobal *global;
	u32 file_location;
	u32 size_bytes;
} GlobalReference;

typedef struct AsmModule
{
	Array(AsmFunction) functions;
	Array(AsmGlobal *) globals;

	Array(GlobalReference) global_references;

	Pool pool;
} AsmModule;

typedef struct AsmSymbol
{
	char *name;
	u32 string_table_offset_for_name;
	u32 offset;
	u32 size;
} AsmSymbol;

void init_asm_function(AsmFunction *func, char *name);
void init_asm_module(AsmModule *asm_module);

void free_asm_module(AsmModule *asm_module);

AsmArg asm_virtual_register(u32 n);
AsmArg asm_physical_register(PhysicalRegister reg);
AsmArg asm_offset_register(PhysicalRegister reg, u64 offset);
AsmArg asm_const32(i32 constant);
AsmArg asm_deref(AsmArg asm_arg);
AsmArg asm_global(AsmGlobal *global);

void dump_asm_module(AsmModule *asm_module);

void assemble(AsmModule *asm_module, FILE *output_file, Array(AsmSymbol) *symbols);

#endif
