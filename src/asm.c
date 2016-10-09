#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "array.h"
#include "asm.h"
#include "asm_gen.h"
#include "file.h"
#include "util.h"

void init_asm_module(AsmModule *asm_module, char *input_file_name)
{
	asm_module->input_file_name = input_file_name;

	pool_init(&asm_module->pool, 1024);

	ARRAY_INIT(&asm_module->globals, AsmGlobal *, 10);
	ARRAY_INIT(&asm_module->fixups, Fixup *, 10);
}

void free_asm_module(AsmModule *asm_module)
{
	for (u32 i = 0; i < asm_module->globals.size; i++) {
		AsmGlobal *global = *ARRAY_REF(&asm_module->globals, AsmGlobal *, i);

		if (global->type == ASM_GLOBAL_FUNCTION) {
			AsmFunction *function = &global->val.function;
			array_free(&function->prologue);
			array_free(&function->body);
			array_free(&function->epilogue);
			array_free(&function->labels);
		}
	}
	array_free(&asm_module->globals);
	array_free(&asm_module->fixups);

	pool_free(&asm_module->pool);
}

void init_asm_function(AsmFunction *function, char *name)
{
	function->name = name;

	ARRAY_INIT(&function->prologue, AsmInstr, 10);
	ARRAY_INIT(&function->body, AsmInstr, 20);
	ARRAY_INIT(&function->epilogue, AsmInstr, 10);
	ARRAY_INIT(&function->labels, AsmLabel *, 10);
	function->ret_label = NULL;
}

AsmArg asm_vreg(u32 vreg_number, u8 width)
{
	AsmArg asm_arg = {
		.is_deref = false,
		.type = ASM_ARG_REGISTER,
		.val.reg.width = width,
		.val.reg.type = V_REG,
		.val.reg.val.vreg_number = vreg_number,
	};

	return asm_arg;
}

AsmArg asm_phys_reg(RegClass reg, u8 width)
{
	AsmArg asm_arg = {
		.is_deref = false,
		.type = ASM_ARG_REGISTER,
		.val.reg.width = width,
		.val.reg.type = PHYS_REG,
		.val.reg.val.class = reg,
	};

	return asm_arg;
}

AsmArg asm_deref(AsmArg asm_arg)
{
	asm_arg.is_deref = true;
	return asm_arg;
}

AsmArg asm_offset_reg(RegClass reg, u8 width, AsmConst offset)
{
	if (offset.type == ASM_CONST_IMMEDIATE && offset.val.immediate == 0) {
		return asm_deref(asm_phys_reg(reg, width));
	} else {
		AsmArg asm_arg = {
			.is_deref = false,
			.type = ASM_ARG_OFFSET_REGISTER,
			.val.offset_register.reg.width = width,
			.val.offset_register.reg.type = PHYS_REG,
			.val.offset_register.reg.val.class = reg,
			.val.offset_register.offset = offset,
		};

		return asm_arg;
	}
}

AsmArg asm_const(u64 constant)
{
	AsmArg asm_arg = {
		.is_deref = false,
		.type = ASM_ARG_CONST,
		.val.constant = (AsmConst) {
			.type = ASM_CONST_IMMEDIATE,
			.val.immediate = constant,
		},
	};

	return asm_arg;
}

AsmArg asm_label(AsmLabel *label)
{
	AsmArg asm_arg = {
		.is_deref = false,
		.type = ASM_ARG_LABEL,
		.val.label = label,
	};

	return asm_arg;
}

AsmArg asm_global(AsmGlobal *global)
{
	AsmArg asm_arg = {
		.is_deref = false,
		.type = ASM_ARG_CONST,
		.val.constant = (AsmConst) {
			.type = ASM_CONST_GLOBAL,
			.val.global = global,
		},
	};

	return asm_arg;
}

// @TODO: Negative numbers
static bool is_const_and_fits(AsmArg asm_arg, u32 bits)
{
	if (asm_arg.type != ASM_ARG_CONST) {
		return false;
	}

	AsmConst constant = asm_arg.val.constant;
	switch (constant.type) {
	case ASM_CONST_IMMEDIATE: {
		u64 imm = constant.val.immediate;

		u64 truncated;

		// Handle this case specially, as 1 << 64 - 1 doesn't work to get a 64-bit
		// mask.
		if (bits == 64)
			truncated = imm;
		else
			truncated = imm & ((1ull << bits) - 1);
		return truncated == imm;
	}
	case ASM_CONST_GLOBAL:
		return bits == 64;
	}
}

static void dump_asm_args(AsmArg *args, u32 num_args);

#define X(x) #x
static char *asm_op_names[] = {
	ASM_OPS
};
#undef X

static void dump_asm_instr(AsmInstr *instr)
{
	if (instr->label != NULL)
		printf("%s:\n", instr->label->name);

	putchar('\t');
	char *op_name = asm_op_names[instr->op];
	for (u32 i = 0; op_name[i] != '\0'; i++)
		putchar(tolower(op_name[i]));

	putchar(' ');
	dump_asm_args(instr->args, instr->num_args);
	putchar('\n');
}

#define X(x, b, d, w, o) { b, d, w, o }
static char *physical_register_names[][4] = {
	REG_CLASSES
};
#undef X

static void dump_register(Register reg)
{
	switch (reg.type) {
	case PHYS_REG: {
		char *reg_name = physical_register_names
				[reg.val.class]
				[lowest_set_bit(reg.width) - 3];
		for (u32 i = 0; reg_name[i] != '\0'; i++)
			putchar(tolower(reg_name[i]));
		break;
	}
	case V_REG:
		printf("#%u", reg.val.vreg_number);
		break;
	}
}

static void dump_asm_const(AsmConst constant)
{
	switch (constant.type) {
	case ASM_CONST_IMMEDIATE: {
		u64 x = constant.val.immediate;
		if ((i64)x < 0)
			printf("%" PRId64, (i64)x);
		else
			printf("%" PRIu64, (i64)x);
		break;
	}
	case ASM_CONST_GLOBAL:
		printf("%s", constant.val.global->name);
		break;
	default:
		UNREACHABLE;
	}
}

static void dump_asm_args(AsmArg *args, u32 num_args)
{
	for (u32 i = 0; i < num_args; i++) {
		if (i != 0)
			fputs(", ", stdout);

		AsmArg *arg = &args[i];
		if (arg->is_deref)
			putchar('[');
		switch (arg->type) {
		case ASM_ARG_REGISTER:
			dump_register(arg->val.reg);
			break;
		case ASM_ARG_OFFSET_REGISTER:
			dump_register(arg->val.offset_register.reg);
			fputs(" + ", stdout);
			dump_asm_const(arg->val.offset_register.offset);
			break;
		case ASM_ARG_LABEL:
			printf("%s", arg->val.label->name);
			break;
		case ASM_ARG_CONST:
			dump_asm_const(arg->val.constant);
			break;
		}
		if (arg->is_deref)
			putchar(']');
	}
}

void dump_asm_function(AsmFunction *asm_function)
{
	Array(AsmInstr) *instr_blocks[] = {
		&asm_function->prologue,
		&asm_function->body,
		&asm_function->epilogue,
	};

	for (u32 i = 0; i < STATIC_ARRAY_LENGTH(instr_blocks); i++) {
		Array(AsmInstr) *instr_block = instr_blocks[i];
		for (u32 j = 0; j < instr_block->size; j++) {
			AsmInstr *instr = ARRAY_REF(instr_block, AsmInstr, j);
			dump_asm_instr(instr);
		}
	}
}

void dump_asm_module(AsmModule *asm_module)
{
	for (u32 i = 0; i < asm_module->globals.size; i++) {
		AsmGlobal *global = *ARRAY_REF(&asm_module->globals, AsmGlobal *, i);

		printf("global %s", global->name);
		if (global->defined) {
			switch (global->type) {
			case ASM_GLOBAL_FUNCTION:
				putchar('\n');
				dump_asm_function(&global->val.function);
				break;
			case ASM_GLOBAL_VAR: {
				if (global->val.var.value != NULL) {
					fputs(" = [", stdout);
					u32 size = global->val.var.size_bytes;
					for (u32 i = 0; i < size; i++) {
						printf("%u", global->val.var.value[i]);
						if (i != size - 1)
							fputs(", ", stdout);
					}
					putchar(']');
				}
			}
			}
		}

		puts("\n");
	}
}

static inline void write_u8(Array(u8) *output, u8 x)
{
	*ARRAY_APPEND(output, u8) = x;
}

static inline void write_int_at(Array(u8) *output, u32 offset, u64 x, u32 size)
{
	array_ensure_room(output, 1, offset + size);
	for (u32 n = 0; n < size; n ++) {
		u8 byte = (x >> (n * 8)) & 0xFF;
		*ARRAY_REF(output, u8, offset + n) = byte;
	}
	if (output->size < offset + size)
		output->size = offset + size;
}

static inline void write_int(Array(u8) *output, u64 x, u32 size)
{
	write_int_at(output, output->size, x, size);
}

static inline RegClass get_reg_class(AsmArg *arg)
{
	Register reg;
	if (arg->type == ASM_ARG_REGISTER) {
		reg = arg->val.reg;
	} else {
		assert(arg->type == ASM_ARG_OFFSET_REGISTER);
		reg = arg->val.offset_register.reg;
	}

	assert(reg.type == PHYS_REG);
	return reg.val.class;
}

static u32 encoded_register_number(RegClass reg)
{
	switch (reg) {
	case INVALID_REG_CLASS: UNREACHABLE;
	case REG_CLASS_A: return 0;
	case REG_CLASS_C: return 1;
	case REG_CLASS_D: return 2;
	case REG_CLASS_B: return 3;
	case REG_CLASS_SP: return 4;
	case REG_CLASS_BP: return 5;
	case REG_CLASS_SI: return 6;
	case REG_CLASS_DI: return 7;
	case REG_CLASS_R8:  return 8;
	case REG_CLASS_R9:  return 9;
	case REG_CLASS_R10: return 10;
	case REG_CLASS_R11: return 11;
	case REG_CLASS_R12: return 12;
	case REG_CLASS_R13: return 13;
	case REG_CLASS_R14: return 14;
	case REG_CLASS_R15: return 15;
	case REG_CLASS_IP: UNIMPLEMENTED;
	}
}

#define MAX_OPCODE_SIZE 2

typedef struct EncodedInstr
{
	u8 rex_prefix;
	u8 opcode_size;
	u8 opcode[MAX_OPCODE_SIZE];
	u8 opcode_extension;
	bool has_modrm;
	u8 mod;
	u8 reg;
	u8 rm;
	bool has_sib;
	u8 scale;
	u8 index;
	u8 base;
	i8 displacement_size;
	u64 displacement;
	Fixup *disp_fixup;
	i8 immediate_size;
	u64 immediate;
	Fixup *imm_fixup;
} EncodedInstr;

static void add_mod_rm_arg(AsmModule *asm_module, EncodedInstr *encoded_instr,
		AsmArg *arg, FixupType fixup_type)
{
	encoded_instr->has_modrm = true;

	if (arg->type == ASM_ARG_REGISTER) {
		RegClass class = get_reg_class(arg);

		if (arg->is_deref) {
			switch (class) {
			default: {
				encoded_instr->mod = 0;
				encoded_instr->rm = encoded_register_number(class);
				return;
			}
			case REG_CLASS_SP: case REG_CLASS_R12: {
				// Mod = 0, R/M = 4 means SIB addressing
				encoded_instr->mod = 0;
				encoded_instr->rm = 4;
				// no index/scale
				encoded_instr->has_sib = true;
				encoded_instr->scale = 0;
				encoded_instr->index = 4;
				encoded_instr->base = encoded_register_number(class);

				return;
			}
			case REG_CLASS_BP: case REG_CLASS_R13: {
				// Mod = 1 means 8-bit displacement
				encoded_instr->mod = 1;
				encoded_instr->rm = encoded_register_number(class);
				encoded_instr->displacement_size = 1;
				encoded_instr->displacement = 0;

				return;
			}
			}
		} else {
			encoded_instr->mod = 3;
			encoded_instr->rm = encoded_register_number(class);
			return;
		}
	} else if (arg->type == ASM_ARG_OFFSET_REGISTER) {
		assert(arg->is_deref);

		AsmConst asm_const = arg->val.offset_register.offset;
		RegClass reg = get_reg_class(arg);

		u64 offset;
		if (asm_const.type == ASM_CONST_GLOBAL) {
			encoded_instr->mod = 2;

			// Dummy value, gets patched later.
			offset = 0;

			if (reg == REG_CLASS_IP)
				fixup_type = FIXUP_RELATIVE;

			Fixup *fixup = pool_alloc(&asm_module->pool, sizeof *fixup);
			*ARRAY_APPEND(&asm_module->fixups, Fixup *) = fixup;
			encoded_instr->disp_fixup = fixup;
			fixup->type = fixup_type;
			fixup->size_bytes = 4;
			fixup->source = FIXUP_GLOBAL;
			fixup->val.global = asm_const.val.global;
		} else {
			assert(asm_const.type == ASM_CONST_IMMEDIATE);
			offset = asm_const.val.immediate;
			// @TODO: Negative numbers
			if ((offset & 0xFF) == offset)
				encoded_instr->mod = 1;
			else if ((offset & 0xFFFFFFFF) == offset)
				encoded_instr->mod = 2;
			else
				assert(!"Offset too large!");
		}

		switch (reg) {
		case REG_CLASS_A: case REG_CLASS_C: case REG_CLASS_D: case REG_CLASS_B:
		case REG_CLASS_BP: case REG_CLASS_SI: case REG_CLASS_DI: {
			encoded_instr->rm = encoded_register_number(reg);
			break;
		}
		case REG_CLASS_SP:
			// Same as above: SIB addressing
			encoded_instr->rm = 4;
			encoded_instr->has_sib = true;
			encoded_instr->scale = 0;
			encoded_instr->index = 4;
			encoded_instr->base = 4;

			if (encoded_instr->mod == 1)
				encoded_instr->displacement_size = 1;
			else
				encoded_instr->displacement_size = 4;
			encoded_instr->displacement = offset;

			break;
		case REG_CLASS_IP:
			encoded_instr->mod = 0;
			encoded_instr->rm = 5;
			encoded_instr->displacement_size = 4;
			encoded_instr->displacement = 0;
			break;
		default:
			UNIMPLEMENTED;
		}
	} else {
		UNIMPLEMENTED;
	}
}

typedef enum ArgOrder { INVALID, RM, MR } ArgOrder;

static inline void write_bytes(Array(u8) *output, u32 size, u8 *bytes)
{
	ARRAY_APPEND_ELEMS(output, u8, size, bytes);
}

typedef enum RexPrefix
{
	REX_B = 1 << 0,
	REX_X = 1 << 1,
	REX_R = 1 << 2,
	REX_W = 1 << 3,
	REX_HIGH = 0x40,
} RexPrefix;

// Called by the generated function "assemble_instr".
static void encode_instr(Array(u8) *output, AsmModule *asm_module,
		AsmInstr *instr, ArgOrder arg_order, bool use_rex_w, u32 opcode_size,
		u8 opcode[], bool reg_and_rm, i32 opcode_extension, i32 immediate_size,
		bool reg_in_opcode, FixupType fixup_type)
{
	EncodedInstr encoded_instr;
	ZERO_STRUCT(&encoded_instr);
	encoded_instr.displacement_size = -1;
	encoded_instr.immediate_size = -1;

	if (use_rex_w)
		encoded_instr.rex_prefix |= REX_W;

	encoded_instr.opcode_size = opcode_size;
	memcpy(encoded_instr.opcode, opcode, opcode_size);
	if (reg_in_opcode) {
		encoded_instr.opcode_extension =
			encoded_register_number(get_reg_class(instr->args));
	}

	if (reg_and_rm) {
		AsmArg *register_operand;
		AsmArg *memory_operand;
		if (arg_order == RM) {
			register_operand = instr->args;
			memory_operand = instr->args + 1;
		} else if (arg_order == MR) {
			memory_operand = instr->args;
			register_operand = instr->args + 1;
		} else if (instr->num_args == 1) {
			memory_operand = instr->args;
			register_operand = NULL;
		} else {
			UNREACHABLE;
		}

		if (register_operand == NULL) {
			encoded_instr.reg = 0;
		} else {
			encoded_instr.reg =
				encoded_register_number(get_reg_class(register_operand));
		}
		add_mod_rm_arg(asm_module, &encoded_instr, memory_operand, fixup_type);
	} else if (opcode_extension != -1) {
		encoded_instr.reg = opcode_extension;
		add_mod_rm_arg(asm_module, &encoded_instr, instr->args, fixup_type);
	} else {
		// @NOTE: I'm not sure this is true in general, but it seems like there
		// are three cases:
		//   * The ModR/M byte contains a register and an r/m operand
		//   * The ModR/M byte contains an opcode extension and an r/m operand
		//   * The ModR/M byte is not present
		// This branch represents the third case. Since we already wrote the
		// opcode above there's nothing left to do.
	}

	// @TODO: This seems kinda redundant considering we already encode the
	// immediate size in AsmArg.
	if (immediate_size != -1) {
		encoded_instr.immediate_size = immediate_size;
		AsmArg* immediate_arg = NULL;
		for (u32 i = 0; i < instr->num_args; i++) {
			if (instr->args[i].type == ASM_ARG_CONST
					|| instr->args[i].type == ASM_ARG_LABEL) {
				// Check that we only have one immediate.
				assert(immediate_arg == NULL);
				immediate_arg = instr->args + i;
			}
		}
		assert(immediate_arg != NULL);

		if (immediate_arg->type == ASM_ARG_LABEL) {
			Fixup *fixup = pool_alloc(&asm_module->pool, sizeof *fixup);
			*ARRAY_APPEND(&asm_module->fixups, Fixup *) = fixup;
			encoded_instr.imm_fixup = fixup;
			fixup->type = fixup_type;
			fixup->size_bytes = 4;
			fixup->source = FIXUP_LABEL;
			fixup->val.label = immediate_arg->val.label;

			// Dummy value, gets patched later.
			encoded_instr.immediate = 0;
		} else if (immediate_arg->type == ASM_ARG_CONST) {
			AsmConst constant = immediate_arg->val.constant;
			switch (constant.type) {
			case ASM_CONST_GLOBAL: {
				Fixup *fixup = pool_alloc(&asm_module->pool, sizeof *fixup);
				*ARRAY_APPEND(&asm_module->fixups, Fixup *) = fixup;
				encoded_instr.imm_fixup = fixup;
				fixup->type = fixup_type;
				fixup->size_bytes = 4;
				fixup->source = FIXUP_GLOBAL;
				fixup->val.global = constant.val.global;

				// Dummy value, gets patched later.
				encoded_instr.immediate = 0;
				break;
			}
			case ASM_CONST_IMMEDIATE:
				encoded_instr.immediate = constant.val.immediate;
				break;
			}

		} else {
			UNREACHABLE;
		}
	}


	if (encoded_instr.has_modrm) {
		if (encoded_instr.reg >= 8)
			encoded_instr.rex_prefix |= REX_R;
		if (encoded_instr.rm >= 8)
			encoded_instr.rex_prefix |= REX_B;
	}
	if (encoded_instr.has_sib) {
		if (encoded_instr.index >= 8)
			encoded_instr.rex_prefix |= REX_X;
		if (encoded_instr.base >= 8) {
			// Make sure we didn't already use REX.B for the RM field.
			assert((encoded_instr.rex_prefix & REX_B) == 0);
			encoded_instr.rex_prefix |= REX_B;
		}
	}
	if (encoded_instr.opcode_extension >= 8) {
		// Make sure we didn't already use REX.B for the RM field or SIB.
		assert((encoded_instr.rex_prefix & REX_B) == 0);
		encoded_instr.rex_prefix |= REX_B;
	}
	if (encoded_instr.rex_prefix != 0) {
		encoded_instr.rex_prefix |= REX_HIGH;
		write_u8(output, encoded_instr.rex_prefix);
	}
	encoded_instr.opcode[0] |= encoded_instr.opcode_extension & 7;
	write_bytes(output, encoded_instr.opcode_size, encoded_instr.opcode);
	if (encoded_instr.has_modrm) {
		u8 mod = encoded_instr.mod;
		u8 reg = encoded_instr.reg;
		u8 rm = encoded_instr.rm;
		u8 mod_rm_byte = ((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7);

		write_u8(output, mod_rm_byte);
	}
	if (encoded_instr.has_sib) {
		u8 scale = encoded_instr.scale;
		u8 index = encoded_instr.index;
		u8 base = encoded_instr.base;
		u8 sib_byte = ((scale  & 3) << 6) | ((index & 7) << 3) | (base & 7);
		write_u8(output, sib_byte);
	}
	if (encoded_instr.disp_fixup != NULL)
		encoded_instr.disp_fixup->offset = output->size;
	if (encoded_instr.displacement_size != -1)
		write_int(output, encoded_instr.displacement, encoded_instr.displacement_size);
	if (encoded_instr.imm_fixup != NULL)
		encoded_instr.imm_fixup->offset = output->size;
	if (encoded_instr.immediate_size != -1)
		write_int(output, encoded_instr.immediate, encoded_instr.immediate_size);

	if (encoded_instr.disp_fixup != NULL)
		encoded_instr.disp_fixup->next_instr_offset = output->size;
	if (encoded_instr.imm_fixup != NULL)
		encoded_instr.imm_fixup->next_instr_offset = output->size;
}

// This is generated from "x64.enc", and defines the function "assemble_instr".
#include "x64.inc"

void init_binary(Binary *binary)
{
	Array(u8) *text = &binary->text;
	ARRAY_INIT(text, u8, 1024);
	Array(u8) *data = &binary->data;
	ARRAY_INIT(data, u8, 1024);
	binary->bss_size = 0;
	Array(AsmSymbol) *symbols = &binary->symbols;
	ARRAY_INIT(symbols, AsmSymbol, 10);
}

void free_binary(Binary *binary)
{
	array_free(&binary->text);
	array_free(&binary->data);
	array_free(&binary->symbols);
}

void assemble(AsmModule *asm_module, Binary *binary)
{
	init_binary(binary);

	for (u32 i = 0; i < asm_module->globals.size; i++) {
		AsmGlobal *global = *ARRAY_REF(&asm_module->globals, AsmGlobal *, i);
		AsmSymbol *symbol = pool_alloc(&asm_module->pool, sizeof *symbol);
		*ARRAY_APPEND(&binary->symbols, AsmSymbol *) = symbol;

		symbol->name = global->name;
		symbol->defined = global->defined;
		// Add one to account for 0 = undef symbol index
		symbol->symtab_index = i + 1;
		symbol->linkage = global->linkage;

		global->symbol = symbol;

		switch (global->type) {
		case ASM_GLOBAL_FUNCTION: {
			AsmFunction *function = &global->val.function;

			u32 function_start;
			u32 function_size;
			if (global->defined) {
				function_start = binary->text.size;

				Array(AsmInstr) *instr_blocks[] = {
					&function->prologue,
					&function->body,
					&function->epilogue,
				};
				for (u32 i = 0; i < STATIC_ARRAY_LENGTH(instr_blocks); i++) {
					Array(AsmInstr) *instr_block = instr_blocks[i];

					for (u32 j = 0; j < instr_block->size; j++) {
						AsmInstr *instr = ARRAY_REF(instr_block, AsmInstr, j);
						if (instr->label != NULL) {
							instr->label->offset = binary->text.size;
						}
						assemble_instr(&binary->text, asm_module, instr);
					}
				}

				function_size = binary->text.size - function_start;
			} else {
				function_start = function_size = 0;
			}

			symbol->section = TEXT_SECTION;
			symbol->offset = function_start;
			symbol->size = function_size;

			break;
		} 
		case ASM_GLOBAL_VAR:
			symbol->size = global->val.var.size_bytes;

			if (global->val.var.value == NULL) {
				symbol->section = BSS_SECTION;

				// @TODO: Alignment
				symbol->offset = binary->bss_size;
				binary->bss_size += symbol->size;
			} else {
				symbol->section = DATA_SECTION;

				// @TODO: Alignment
				symbol->offset = binary->data.size;

				u8 *value = global->val.var.value;
				assert(value != NULL);
				ARRAY_APPEND_ELEMS(&binary->data, u8, global->val.var.size_bytes, value);
			}

			break;
		}
	}

	// @TODO: Emit relocations here instead?
	for (u32 i = 0; i < asm_module->fixups.size; i++) {
		Fixup *fixup = *ARRAY_REF(&asm_module->fixups, Fixup *, i);
		if (fixup->type == FIXUP_ABSOLUTE)
			continue;

		u32 target_offset;
		if (fixup->source == FIXUP_GLOBAL) {
			AsmGlobal *global = fixup->val.global;
			if (global->defined && global->type == ASM_GLOBAL_FUNCTION)
				target_offset = global->offset;
			else
				continue;
		} else {
			target_offset = fixup->val.label->offset;
		}


		// Relative accesses are relative to the start of the next instruction,
		// so we add on the size of the reference itself first.
		i32 value = (i32)target_offset - (i32)fixup->next_instr_offset;
		write_int_at(&binary->text, fixup->offset, (u64)value, fixup->size_bytes);
	}
}
