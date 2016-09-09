#include <assert.h>

#include "array.h"
#include "asm.h"
#include "asm_gen.h"
#include "flags.h"
#include "ir.h"
#include "util.h"

void init_asm_builder(AsmBuilder *builder)
{
	init_asm_module(&builder->asm_module);

	builder->local_stack_usage = 0;
	builder->stack_slots = ARRAY_ZEROED;
	builder->virtual_registers = ARRAY_ZEROED;
}

void free_asm_builder(AsmBuilder *builder)
{
	free_asm_module(&builder->asm_module);

	if (ARRAY_IS_VALID(&builder->stack_slots))
		array_free(&builder->stack_slots);
	if (ARRAY_IS_VALID(&builder->virtual_registers))
		array_free(&builder->virtual_registers);
}

static AsmGlobal *append_function(AsmBuilder *builder, char *name)
{
	AsmGlobal *new_global = pool_alloc(&builder->asm_module.pool, sizeof *new_global);
	*ARRAY_APPEND(&builder->asm_module.globals, AsmGlobal *) = new_global;
	AsmFunction *new_function = &new_global->val.function;

	new_global->type = ASM_GLOBAL_FUNCTION;
	init_asm_function(new_function, name);
	builder->current_function = new_function;
	builder->current_block = &new_function->body;

	builder->local_stack_usage = 0;
	if (ARRAY_IS_VALID(&builder->stack_slots))
		array_free(&builder->stack_slots);
	ARRAY_INIT(&builder->stack_slots, StackSlot, 5);

	return new_global;
}

AsmInstr *emit_instr0(AsmBuilder *builder, AsmOp op)
{
	AsmInstr *instr = ARRAY_APPEND(builder->current_block, AsmInstr);
	instr->op = op;
	instr->num_args = 0;
	instr->label = NULL;

	return instr;
}

AsmInstr *emit_instr1(AsmBuilder *builder, AsmOp op, AsmArg arg1)
{
	AsmInstr *instr = ARRAY_APPEND(builder->current_block, AsmInstr);
	instr->op = op;
	instr->num_args = 1;
	instr->args[0] = arg1;
	instr->label = NULL;

	return instr;
}

AsmInstr *emit_instr2(AsmBuilder *builder, AsmOp op, AsmArg arg1, AsmArg arg2)
{
	AsmInstr *instr = ARRAY_APPEND(builder->current_block, AsmInstr);
	instr->op = op;
	instr->num_args = 2;
	instr->args[0] = arg1;
	instr->args[1] = arg2;
	instr->label = NULL;

	return instr;
}

AsmInstr *emit_instr3(AsmBuilder *builder, AsmOp op,
		AsmArg arg1, AsmArg arg2, AsmArg arg3)
{
	AsmInstr *instr = ARRAY_APPEND(builder->current_block, AsmInstr);
	instr->op = op;
	instr->num_args = 3;
	instr->args[0] = arg1;
	instr->args[1] = arg2;
	instr->args[2] = arg3;
	instr->label = NULL;

	return instr;
}

static u32 size_of_ir_type(IrType type)
{
	switch (type.kind) {
	case IR_INT:
		return type.val.bit_width;
	case IR_POINTER: case IR_FUNCTION:
		return 8;
	}
}

static StackSlot *stack_slot_for_id(AsmBuilder *builder, u32 id)
{
	for (u32 i = 0; i < builder->stack_slots.size; i++) {
		StackSlot *stack_slot = ARRAY_REF(&builder->stack_slots, StackSlot, i);
		if (stack_slot->ir_instr_id == id)
			return stack_slot;
	}

	return NULL;
}

// @TODO: Rethink name? "next" kinda suggests side effects, i.e. "move to the
// next vreg number".
static inline u32 next_vreg(AsmBuilder *builder)
{
	return builder->virtual_registers.size;
}

static PhysicalRegister argument_registers[] = {
	RDI, RSI, RDX, RCX, R8, R9,
};

static AsmArg asm_value(IrValue value)
{
	switch (value.kind) {
	case VALUE_CONST:
		assert((value.val.constant & 0xFFFFFFFF) == value.val.constant);
		return asm_const32(value.val.constant & 0xFFFFFFFF);
	case VALUE_INSTR: {
		i32 vreg = value.val.instr->virtual_register;
		assert(vreg != -1);
		return asm_virtual_register(vreg);
	}
	case VALUE_ARG: {
		assert(value.type.kind == IR_INT);
		assert(value.val.arg_index < STATIC_ARRAY_LENGTH(argument_registers));

		// We always allocate virtual registers to arguments first, so arg at
		// index i = virtual register i.
		return asm_virtual_register(value.val.arg_index);
	}
	case VALUE_GLOBAL:
		return asm_global(value.val.global->asm_global);
	}
}

static VRegInfo *append_vreg(AsmBuilder *builder)
{
	if (next_vreg(builder) == 4) {
		volatile int x = 3;
		x = x;
	}
	VRegInfo *vreg_info = ARRAY_APPEND(&builder->virtual_registers, VRegInfo);
	vreg_info->assigned_register = INVALID_REGISTER;
	vreg_info->live_range_start = vreg_info->live_range_end = -1;

	return vreg_info;
}

static VRegInfo *assign_vreg(AsmBuilder *builder, IrInstr *instr)
{
	u32 vreg_number = next_vreg(builder);
	VRegInfo *vreg = append_vreg(builder);
	instr->virtual_register = vreg_number;

	return vreg;
}

static AsmArg pre_alloced_vreg(AsmBuilder *builder, PhysicalRegister reg)
{
	u32 vreg_number = next_vreg(builder);

	VRegInfo *vreg_info = append_vreg(builder);
	vreg_info->assigned_register = reg;

	return asm_virtual_register(vreg_number);
}

AsmLabel *append_label(AsmBuilder *builder, char *name)
{
	AsmLabel *label = pool_alloc(&builder->asm_module.pool, sizeof *label);
	label->name = name;
	*ARRAY_APPEND(&builder->current_function->labels, AsmLabel *) = label;

	return label;
}

static void asm_gen_instr(
		IrFunction *ir_func, AsmBuilder *builder, IrInstr *instr)
{
	switch (instr->op) {
	case OP_LOCAL: {
		StackSlot *slot = ARRAY_APPEND(&builder->stack_slots, StackSlot);
		slot->ir_instr_id = instr->id;

		// @TODO: Alignment of stack slots. This could probably use similar
		// logic to that of struct layout.
		slot->stack_offset = builder->local_stack_usage;
		builder->local_stack_usage += size_of_ir_type(instr->type);

		break;
	}
	case OP_RET: {
		IrValue arg = instr->val.arg;
		assert(ir_type_eq(ir_func->return_type, arg.type));

		emit_instr2(builder, MOV, asm_physical_register(RAX), asm_value(arg));
		emit_instr1(builder, JMP, asm_label(builder->current_function->ret_label));

		break;
	}
	case OP_BRANCH:
		emit_instr1(builder, JMP, asm_label(instr->val.target_block->label));
		break;
	case OP_COND:
		emit_instr2(builder, CMP, asm_value(instr->val.cond.condition), asm_const32(0));
		emit_instr1(builder, JE, asm_label(instr->val.cond.else_block->label));
		emit_instr1(builder, JMP, asm_label(instr->val.cond.then_block->label));
		break;
	case OP_STORE: {
		IrValue pointer = instr->val.store.pointer;
		IrValue value = instr->val.store.value;
		IrType type = instr->val.store.type;

		assert(ir_type_eq(value.type, type));
		assert(type.kind == IR_INT);
		assert(type.val.bit_width == 32);

		assert(pointer.kind == VALUE_INSTR);
		IrInstr *pointer_instr = pointer.val.instr;
		assert(pointer_instr->op == OP_LOCAL);

		StackSlot *slot = stack_slot_for_id(builder, pointer_instr->id);
		assert(slot != NULL);

		emit_instr2(builder, MOV,
				asm_deref(asm_offset_register(RSP, slot->stack_offset)),
				asm_value(value));
		break;
	}
	case OP_LOAD: {
		IrValue pointer = instr->val.load.pointer;
		IrType type = instr->val.load.type;

		assert(type.kind == IR_INT);
		assert(type.val.bit_width == 32);

		assert(pointer.kind == VALUE_INSTR);
		IrInstr *pointer_instr = pointer.val.instr;
		assert(pointer_instr->op == OP_LOCAL);

		StackSlot *slot = stack_slot_for_id(builder, pointer_instr->id);
		assert(slot != NULL);

		emit_instr2(builder, MOV,
				asm_virtual_register(next_vreg(builder)),
				asm_deref(asm_offset_register(RSP, slot->stack_offset)));
		assign_vreg(builder, instr);
		break;
	}
	case OP_CALL: {
		// @TODO: Save caller save registers. We could just push and pop
		// everything and rely on some later pass to eliminate saves of
		// registers that aren't live across the call, but that seems a little
		// messy.
		u32 arity = instr->val.call.arity;
		assert(arity <= STATIC_ARRAY_LENGTH(argument_registers));
		for (u32 i = 0; i < arity; i++) {
			AsmArg arg_current_reg = asm_value(instr->val.call.arg_array[i]);
			AsmArg arg_target_reg = pre_alloced_vreg(builder, argument_registers[i]);
			emit_instr2(builder, MOV, arg_target_reg, arg_current_reg);
		}

		emit_instr1(builder, CALL, asm_value(instr->val.call.callee));
		assign_vreg(builder, instr)->assigned_register = RAX;
		break;
	}
	case OP_BIT_XOR: {
		AsmArg arg1 = asm_value(instr->val.binary_op.arg1);
		AsmArg arg2 = asm_value(instr->val.binary_op.arg2);
		emit_instr2(builder, MOV, asm_virtual_register(next_vreg(builder)), arg1);
		emit_instr2(builder, XOR, asm_virtual_register(next_vreg(builder)), arg2);

		assign_vreg(builder, instr);
		break;
	}
	case OP_ADD: {
		AsmArg arg1 = asm_value(instr->val.binary_op.arg1);
		AsmArg arg2 = asm_value(instr->val.binary_op.arg2);
		emit_instr2(builder, MOV, asm_virtual_register(next_vreg(builder)), arg1);
		emit_instr2(builder, ADD, asm_virtual_register(next_vreg(builder)), arg2);

		assign_vreg(builder, instr);
		break;
	}
	case OP_IMUL: {
		AsmArg arg1 = asm_value(instr->val.binary_op.arg1);
		AsmArg arg2 = asm_value(instr->val.binary_op.arg2);

		if (!asm_arg_is_const(arg1) && !asm_arg_is_const(arg2)) {
			emit_instr2(builder, MOV, asm_virtual_register(next_vreg(builder)), arg1);
			emit_instr2(builder, IMUL, asm_virtual_register(next_vreg(builder)), arg2);
		} else {
			AsmArg const_arg;
			AsmArg non_const_arg;
			if (asm_arg_is_const(arg1)) {
				const_arg = arg1;
				non_const_arg = arg2;
			} else {
				const_arg = arg2;
				non_const_arg = arg1;
			}

			assert(!asm_arg_is_const(non_const_arg));

			emit_instr3(builder, IMUL,
					asm_virtual_register(next_vreg(builder)),
					non_const_arg,
					const_arg);
		}

		assign_vreg(builder, instr);
		break;
	}
	case OP_EQ: {
		AsmArg arg1 = asm_value(instr->val.binary_op.arg1);
		AsmArg arg2 = asm_value(instr->val.binary_op.arg2);
		emit_instr2(builder, CMP, arg1, arg2);
		// @TODO: We should use SETE instead, once we have 1-byte registers
		// working.
		AsmLabel *new_label = append_label(builder, "OP_EQ_label");
		emit_instr2(builder, MOV, asm_virtual_register(next_vreg(builder)), asm_const32(1));
		emit_instr1(builder, JE, asm_label(new_label));
		emit_instr2(builder, MOV, asm_virtual_register(next_vreg(builder)), asm_const32(0));
		emit_instr0(builder, NOP)->label = new_label;

		assign_vreg(builder, instr);
		break;
	}
	}
}

static Register *arg_reg(AsmArg *arg)
{
	if (arg->type == ASM_ARG_REGISTER)
		return &arg->val.reg;
	if (arg->type == ASM_ARG_OFFSET_REGISTER)
		return &arg->val.offset_register.reg;
	return NULL;
}

#define ALLOCATION_ORDER \
	X(0, R11), \
	X(1, R10), \
	X(2, R9), \
	X(3, R8), \
	X(4, RBX), \
	X(5, R12), \
	X(6, R13), \
	X(7, R14), \
	X(8, R15), \
	X(9, RCX), \
	X(10, RDX), \
	X(11, RSI), \
	X(12, RDI), \
	X(13, RAX),

#define X(i, x) [i] = x
static PhysicalRegister alloc_index_to_reg[] = {
	ALLOCATION_ORDER
};
#undef X

#define X(i, x) [x] = i
static u32 reg_to_alloc_index[] = {
	ALLOCATION_ORDER
};
#undef X

// We deliberately exclude RBP from this list, as we don't support omitting the
// frame pointer and so we never use it for register allocation.
static PhysicalRegister callee_save_registers[] = {
	R15, R14, R13, R12, RBX,
};

static bool is_callee_save(PhysicalRegister reg)
{
	for (u32 i = 0; i < STATIC_ARRAY_LENGTH(callee_save_registers); i++) {
		if (callee_save_registers[i] == reg) {
			return true;
		}
	}

	return false;
}

#define CALLER_SAVE_REGS_BITMASK \
	((1 << RAX) | (1 << RDI) | (1 << RSI) | (1 << RDX) | (1 << RCX) | \
	 (1 << R8) | (1 << R9) | (1 << R10) | (1 << R11))

typedef struct CallSite
{
	u32 instr_index;
	u32 active_caller_save_regs_bitset;
} CallSite;

// @TODO: Save all caller save registers that are live across calls.
static void allocate_registers(AsmBuilder *builder)
{
	Array(AsmInstr) *body = &builder->current_function->body;
	for (u32 i = 0; i < body->size; i++) {
		AsmInstr *instr = ARRAY_REF(body, AsmInstr, i);
		for (u32 j = 0; j < instr->num_args; j++) {
			AsmArg *arg = instr->args + j;
			Register *reg = arg_reg(arg);
			if (reg != NULL && reg->type == VIRTUAL_REGISTER) {
				u32 reg_num = reg->val.register_number;
				VRegInfo *vreg = ARRAY_REF(&builder->virtual_registers,
						VRegInfo, reg_num);
				if (vreg->live_range_start == -1) {
					vreg->live_range_start = vreg->live_range_end = i;
				} else {
					vreg->live_range_end = i;
				}
			}
		}
	}

	if (flag_dump_live_ranges) {
		dump_asm_function(builder->current_function);
		for (u32 i = 0; i < builder->virtual_registers.size; i++) {
			VRegInfo *vreg = ARRAY_REF(&builder->virtual_registers, VRegInfo, i);
			printf("#%u: [%d, %d]\n", i, vreg->live_range_start, vreg->live_range_end);
		}
		putchar('\n');
	}

	u32 free_regs_bitset;
	assert(STATIC_ARRAY_LENGTH(alloc_index_to_reg) < 8 * sizeof free_regs_bitset);

	// Start with all regs free
	free_regs_bitset = (1 << STATIC_ARRAY_LENGTH(alloc_index_to_reg)) - 1;

	Array(VRegInfo *) active_vregs;
	ARRAY_INIT(&active_vregs, VRegInfo *, 16);
	for (u32 i = 0; i < builder->virtual_registers.size; i++) {
		VRegInfo *vreg = ARRAY_REF(&builder->virtual_registers, VRegInfo, i);

		// This indicates that we assigned a vreg to something that wasn't
		// used, e.g. a pre-alloced RAX for the return value of a function.
		if (vreg->live_range_start == -1) {
			assert(vreg->live_range_end == -1);
		}

		while (active_vregs.size != 0) {
			VRegInfo *active_vreg = *ARRAY_REF(&active_vregs, VRegInfo *, 0);
			if (active_vreg->live_range_end >= vreg->live_range_start)
				break;
			u32 alloc_index = reg_to_alloc_index[active_vreg->assigned_register];
			free_regs_bitset |= 1 << alloc_index;

			// @TODO: Remove all the invalidated vregs at once instead of
			// repeatedly shifting down.
			ARRAY_REMOVE(&active_vregs, VRegInfo *, 0);
		}

		if (vreg->assigned_register  == INVALID_REGISTER) {
			// @TODO: Insert spills when there are no free registers to assign.
			assert(free_regs_bitset != 0);
			u32 first_free_alloc_index = lowest_set_bit(free_regs_bitset);
			vreg->assigned_register = alloc_index_to_reg[first_free_alloc_index];
			free_regs_bitset &= ~(1 << first_free_alloc_index);
		} else {
			// This register has already been assigned, e.g. part of a call
			// sequence. We don't need to allocate it, but we do need to keep
			// track of it so it doesn't get clobbered.
			u32 alloc_index = reg_to_alloc_index[vreg->assigned_register];
			assert((free_regs_bitset & (1 << alloc_index)) != 0);
			free_regs_bitset &= ~(1 << alloc_index);
		}

		u32 insertion_point = active_vregs.size;
		for (u32 j = 0; j < active_vregs.size; j++) {
			VRegInfo *active_vreg = *ARRAY_REF(&active_vregs, VRegInfo *, j);
			if (active_vreg->live_range_end < vreg->live_range_end) {
				insertion_point = j;
				break;
			}
		}
		*ARRAY_INSERT(&active_vregs, VRegInfo *, insertion_point) = vreg;
	}

	array_clear(&active_vregs);
	Array(CallSite) callsites;
	ARRAY_INIT(&callsites, CallSite, 5);
	u32 live_regs_bitset = 0;
	u32 total_regs_to_save = 0;
	u32 vreg_index = 0;

	for (u32 i = 0; i < body->size; i++) {
		while (active_vregs.size != 0) {
			VRegInfo *active_vreg = *ARRAY_REF(&active_vregs, VRegInfo *, 0);
			if (active_vreg->live_range_end == -1)
				continue;
			if ((u32)active_vreg->live_range_end >= i)
				break;
			assert(active_vreg->assigned_register != INVALID_REGISTER);
			live_regs_bitset &= ~(1 << active_vreg->assigned_register);

			// @TODO: Remove all the invalidated vregs at once instead of
			// repeatedly shifting down.
			ARRAY_REMOVE(&active_vregs, VRegInfo *, 0);
		}
		if (vreg_index != builder->virtual_registers.size) {
			VRegInfo *next_vreg =
				ARRAY_REF(&builder->virtual_registers, VRegInfo, vreg_index);
			if ((u32)next_vreg->live_range_start == i) {
				assert(next_vreg->assigned_register != INVALID_REGISTER);
				live_regs_bitset |= 1 << next_vreg->assigned_register;

				u32 insertion_point = active_vregs.size;
				for (u32 j = 0; j < active_vregs.size; j++) {
					VRegInfo *active_vreg = *ARRAY_REF(&active_vregs, VRegInfo *, j);
					if (active_vreg->live_range_end == -1)
						continue;
					if ((u32)active_vreg->live_range_end < i) {
						insertion_point = j;
						break;
					}
				}
				*ARRAY_INSERT(&active_vregs, VRegInfo *, insertion_point) = next_vreg;
			}
		}

		AsmInstr *instr = ARRAY_REF(body, AsmInstr, i);
		if (instr->op == OP_CALL) {
			CallSite *callsite = ARRAY_APPEND(&callsites, CallSite);
			callsite->instr_index = i;
			callsite->active_caller_save_regs_bitset =
				live_regs_bitset & CALLER_SAVE_REGS_BITMASK;
			total_regs_to_save += bit_count(live_regs_bitset);
		}
	}
	array_free(&active_vregs);

#if 0
	printf("Total regs to save: %u\n", total_regs_to_save);
	puts("Callsites:");
	for (u32 i = 0; i < callsites.size; i++) {
		CallSite *callsite = ARRAY_REF(&callsites, CallSite, i);
		printf("%d: [", callsite->instr_index);
		u32 active_regs = callsite->active_caller_save_regs_bitset;
		while (active_regs != 0) {
			u32 lowest_bit = lowest_set_bit(active_regs);
			printf("%d ", lowest_bit);
			active_regs &= ~(1 << lowest_bit);
		}
		puts("]");
	}
#endif
	// @TODO: Actually write the code to save registers across callsites.
	assert(total_regs_to_save == 0);

	array_free(&callsites);

	for (u32 i = 0; i < body->size; i++) {
		AsmInstr *instr = ARRAY_REF(body, AsmInstr, i);

		for (u32 j = 0; j < instr->num_args; j++) {
			AsmArg *arg = instr->args + j;
			Register *reg = arg_reg(arg);
			if (reg == NULL)
				continue;

			if (reg->type == VIRTUAL_REGISTER) {
				u32 reg_num = reg->val.register_number;
				VRegInfo *vreg = ARRAY_REF(&builder->virtual_registers,
						VRegInfo, reg_num);
				PhysicalRegister phys_reg = vreg->assigned_register;

				reg->type = PHYSICAL_REGISTER;
				reg->val.physical_register = phys_reg;
			}
		}
	}
}

AsmGlobal *asm_gen_function(AsmBuilder *builder, IrGlobal *ir_global)
{
	assert(ir_global->kind == IR_GLOBAL_FUNCTION);
	IrFunction *ir_func = &ir_global->val.function;

	AsmGlobal *function = append_function(builder, ir_global->name);
	ir_global->asm_global = function;
	AsmLabel *ret_label = pool_alloc(&builder->asm_module.pool, sizeof *ret_label);
	ret_label->name = "ret";
	ret_label->file_location = 0;
	function->val.function.ret_label = ret_label;

	function->defined = ir_global->defined;
	if (!ir_global->defined) {
		return function;
	}

	if (ARRAY_IS_VALID(&builder->virtual_registers))
		array_free(&builder->virtual_registers);
	ARRAY_INIT(&builder->virtual_registers, VRegInfo, 20);
	IrType return_type = ir_func->return_type;
	assert(return_type.kind == IR_INT && return_type.val.bit_width == 32);

	assert(ir_func->arity <= STATIC_ARRAY_LENGTH(argument_registers));
	for (u32 i = 0; i < ir_func->arity; i++) {
		VRegInfo *vreg_info = ARRAY_APPEND(&builder->virtual_registers, VRegInfo);
		vreg_info->assigned_register = argument_registers[i];
		vreg_info->live_range_start = vreg_info->live_range_end = -1;
	}

	for (u32 block_index = 0; block_index < ir_func->blocks.size; block_index++) {
		IrBlock *block = *ARRAY_REF(&ir_func->blocks, IrBlock *, block_index);
		AsmLabel *label = append_label(builder, block->name);
		block->label = label;
	}

	for (u32 block_index = 0; block_index < ir_func->blocks.size; block_index++) {
		IrBlock *block = *ARRAY_REF(&ir_func->blocks, IrBlock *, block_index);

		u32 first_instr_of_block_index = builder->current_function->body.size;
		Array(IrInstr *) *instrs = &block->instrs;
		for (u32 i = 0; i < instrs->size; i++) {
			asm_gen_instr(ir_func, builder, *ARRAY_REF(instrs, IrInstr *, i));
		}

		AsmInstr *first_instr_of_block = ARRAY_REF(
				&builder->current_function->body,
				AsmInstr,
				first_instr_of_block_index);
		first_instr_of_block->label = block->label;
	}

	allocate_registers(builder);

	PhysicalRegister used_callee_save_regs[
		STATIC_ARRAY_LENGTH(callee_save_registers)] = { 0 };
	u32 used_callee_save_regs_size = 0;
	for (u32 i = 0; i < builder->current_function->body.size; i++) {
		AsmInstr *instr = ARRAY_REF(&builder->current_function->body, AsmInstr, i);
		for (u32 j = 0; j < instr->num_args; j++) {
			Register *reg = arg_reg(instr->args + j);
			if (reg != NULL &&
					reg->type == PHYSICAL_REGISTER &&
					is_callee_save(reg->val.physical_register)) {
				used_callee_save_regs[used_callee_save_regs_size++] =
					reg->val.physical_register;
			}
		}
	}

	builder->current_block = &builder->current_function->prologue;

	AsmLabel *entry_label = pool_alloc(&builder->asm_module.pool, sizeof *entry_label);
	entry_label->name = ir_global->name;
	entry_label->file_location = 0;
	ir_func->label = entry_label;

	AsmInstr *prologue_first_instr =
		emit_instr1(builder, PUSH, asm_physical_register(RBP));
	prologue_first_instr->label = entry_label;
	emit_instr2(builder, MOV, asm_physical_register(RBP), asm_physical_register(RSP));
	for (u32 i = 0; i < used_callee_save_regs_size; i++) {
		emit_instr1(builder, PUSH, asm_physical_register(used_callee_save_regs[i]));
	}
	emit_instr2(builder, SUB, asm_physical_register(RSP), asm_const32(builder->local_stack_usage));

	builder->current_block = &builder->current_function->epilogue;

	AsmInstr *epilogue_first_instr =
		emit_instr2(builder, ADD, asm_physical_register(RSP),
				asm_const32(builder->local_stack_usage));
	for (u32 i = 0; i < used_callee_save_regs_size; i++) {
		emit_instr1(builder, POP, asm_physical_register(used_callee_save_regs[i]));
	}
	epilogue_first_instr->label = ret_label;
	emit_instr1(builder, POP, asm_physical_register(RBP));
	emit_instr0(builder, RET);

	return function;
}

void generate_asm_module(AsmBuilder *builder, TransUnit *trans_unit)
{
	for (u32 i = 0; i < trans_unit->globals.size; i++) {
		IrGlobal *ir_global = *ARRAY_REF(&trans_unit->globals, IrGlobal *, i);
		AsmGlobal *asm_global = NULL;

		switch (ir_global->kind) {
		case IR_GLOBAL_FUNCTION:
			asm_global = asm_gen_function(builder, ir_global);
			break;
		case IR_GLOBAL_SCALAR: UNIMPLEMENTED;
		}

		asm_global->name = ir_global->name;
		asm_global->offset = 0;
	}
}
