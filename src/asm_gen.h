#ifndef NAIVE_ASM_GEN_H_
#define NAIVE_ASM_GEN_H_

#include "asm.h"
#include "ir.h"

typedef struct VRegInfo
{
	RegClass assigned_register;
	i32 live_range_start;
	i32 live_range_end;
} VRegInfo;

typedef struct AsmBuilder
{
	AsmModule asm_module;
	IrGlobal *current_function;
	Array(AsmInstr) *current_block;
	AsmSymbol *ret_label;

	Array(VRegInfo) virtual_registers;
	u32 local_stack_usage;
	u32 register_save_area_size;
	u32 curr_sp_diff;

	Array(Fixup *) fixups;
} AsmBuilder;

void init_asm_builder(AsmBuilder *builder, char *input_file_name);
void free_asm_builder(AsmBuilder *builder);
void generate_asm_module(AsmBuilder *builder, TransUnit *trans_unit);

#endif
