#include "ccv_nnc.h"
#include "ccv_nnc_easy.h"
#include "ccv_nnc_internal.h"
#include "ccv_internal.h"
#include "_ccv_nnc_micro.h"
#include "3rdparty/khash/khash.h"

// MARK - Level-1 API

KHASH_MAP_INIT_STR(ccv_nnc_micro_bind_scalar, uint32_t)

static uint32_t _scalars_lookup(const void* const context, const char* const name)
{
	const khash_t(ccv_nnc_micro_bind_scalar)* const bind_scalars = (const khash_t(ccv_nnc_micro_bind_scalar)*)context;
	khiter_t k = kh_get(ccv_nnc_micro_bind_scalar, bind_scalars, name);
	assert(k != kh_end(bind_scalars));
	return kh_val(bind_scalars, k);
}

KHASH_SET_INIT_INT64(ccv_nnc_ids)

CCV_WARN_UNUSED(ccv_nnc_micro_combine_t*) ccv_nnc_micro_combine_new(const ccv_nnc_micro_io_t* const inputs, const int input_size, const char* const* const parameters, const int parameter_size, const ccv_nnc_micro_io_t* const outputs, const int output_size)
{
	assert(output_size > 0);
	assert(input_size > 0);
	int i, j, k;
	// First, do reverse topological sort (from output and then reverse the order).
	// We can do this simple thing because there is no overlaps of the outputs, thus, no cases where
	// output[0] is the input for output[1]. Otherwise we need to detect this, see ccv_cnnp_model_new
	// for more details on why.
	for (i = 0; i < output_size - 1; i++)
		for (j = i + 1; j < output_size; j++)
		{ assert(outputs[i] != outputs[j]); }
	uint64_t input_bitmask[((input_size - 1) >> 6) + 1];
	memset(input_bitmask, 0, sizeof(uint64_t) * (((input_size - 1) >> 6) + 1));
	ccv_array_t* const reverse_top = ccv_array_new(sizeof(ccv_nnc_micro_io_t), output_size + input_size, 0);
	ccv_array_resize(reverse_top, output_size);
	memcpy(ccv_array_get(reverse_top, 0), outputs, sizeof(ccv_nnc_micro_io_t) * output_size);
	khash_t(ccv_nnc_ids)* const ids = kh_init(ccv_nnc_ids);
	for (i = 0; i < reverse_top->rnum; i++)
	{
		const ccv_nnc_micro_io_t output = *(ccv_nnc_micro_io_t*)ccv_array_get(reverse_top, i);
		for (j = 0; j < output->input_size; j++)
			if (!CCV_NNC_IS_MICRO_IO_INPUT(output->inputs[j]))
			{
				int ret;
				kh_put(ccv_nnc_ids, ids, (int64_t)(intptr_t)output->inputs[j], &ret);
				if (ret != 0)
					ccv_array_push(reverse_top, &output->inputs[j]);
			} else {
				// This is an input, it must be represented in inputs, try to find it.
				for (k = 0; k < input_size; k++)
					if (inputs[k] == output->inputs[j])
						break;
				assert(k < input_size); // Cannot find the inputs, error!
				input_bitmask[k >> 6] |= ((uint64_t)1 << (k & 63));
			}
	}
	kh_destroy(ccv_nnc_ids, ids);
	for (i = 0; i < input_size; i++)
		{ assert((input_bitmask[i >> 6] & ((uint64_t)1 << (i & 63)))); } // Assuming they all match.
	// Second, binding parameters (bounded scalars).
	khash_t(ccv_nnc_micro_bind_scalar)* const bind_scalars = kh_init(ccv_nnc_micro_bind_scalar);
	for (i = 0; i < parameter_size; i++)
	{
		int ret;
		khiter_t k = kh_put(ccv_nnc_micro_bind_scalar, bind_scalars, parameters[i], &ret);
		assert(ret != 0);
		kh_val(bind_scalars, k) = i;
	}
	for (i = 0; i < reverse_top->rnum; i++)
	{
		const ccv_nnc_micro_io_t output = *(ccv_nnc_micro_io_t*)ccv_array_get(reverse_top, reverse_top->rnum - 1 - i);
		ccv_nnc_micro_bind_scalars(output, _scalars_lookup, bind_scalars);
	}
	kh_destroy(ccv_nnc_micro_bind_scalar, bind_scalars);
	// Applying numbering for the inputs. Note that our variables are numbered in reverse topological order.
	for (i = 0; i < input_size; i++)
		ccv_nnc_micro_numbering(inputs[i], i + reverse_top->rnum);
	// Applying numbering for the outputs.
	for (i = reverse_top->rnum - 1; i >= 0; i--)
	{
		const ccv_nnc_micro_io_t output = *(ccv_nnc_micro_io_t*)ccv_array_get(reverse_top, i);
		ccv_nnc_micro_numbering(output, i);
	}
	// Third, lower each ccv_nnc_micro_io_t (except the input) op into nested loops such that we can
	// apply optimizations later.
	const int function_count = reverse_top->rnum;
	ccv_nnc_micro_function_t* const functions = (ccv_nnc_micro_function_t*)ccmalloc(sizeof(ccv_nnc_micro_function_t) * function_count);
	for (i = 0; i < function_count; i++)
	{
		const ccv_nnc_micro_io_t output = *(ccv_nnc_micro_io_t*)ccv_array_get(reverse_top, function_count - 1 - i);
		functions[i] = ccv_nnc_micro_emit(output);
	}
	ccv_nnc_micro_tensor_t* const vars = (ccv_nnc_micro_tensor_t*)cccalloc(reverse_top->rnum + input_size, sizeof(ccv_nnc_micro_tensor_t));
	for (i = 0; i < reverse_top->rnum; i++)
	{
		const ccv_nnc_micro_io_t output = *(ccv_nnc_micro_io_t*)ccv_array_get(reverse_top, i);
		vars[i] = ccv_nnc_micro_return_shape(output);
	}
	for (i = 0; i < input_size; i++)
	{
		vars[i + reverse_top->rnum].dimensions = inputs[i]->dimensions;
		vars[i + reverse_top->rnum].input = -1;
		vars[i + reverse_top->rnum].id = inputs[i]->id;
	}
	ccv_nnc_micro_combine_t* const combine = (ccv_nnc_micro_combine_t*)ccmalloc(sizeof(ccv_nnc_micro_combine_t));
	combine->input_size = input_size;
	combine->output_size = output_size;
	combine->parameter_size = parameter_size;
	combine->var_count = reverse_top->rnum + input_size;
	combine->vars = vars;
	combine->function_count = function_count;
	combine->functions = functions;
	for (i = 0; i < reverse_top->rnum; i++)
	{
		const ccv_nnc_micro_io_t output = *(ccv_nnc_micro_io_t*)ccv_array_get(reverse_top, i);
		ccfree(output);
	}
	ccv_array_free(reverse_top);
	for (i = 0; i < input_size; i++)
		ccfree(inputs[i]);
	return combine;
}

static void _ccv_nnc_micro_loop_index_free(ccv_nnc_micro_loop_index_term_t* const term)
{
	if (term->type == CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY)
	{
		_ccv_nnc_micro_loop_index_free(&term->binary->left);
		_ccv_nnc_micro_loop_index_free(&term->binary->right);
		ccfree(term->binary);
	}
}

static void _ccv_nnc_micro_loop_variable_free(ccv_nnc_micro_loop_variable_t* const var)
{
	int i;
	for (i = 0; i < var->index_count; i++)
		_ccv_nnc_micro_loop_index_free(&var->index[i]);
}

static void _ccv_nnc_micro_loop_expression_free(ccv_nnc_micro_loop_expression_t* const expr)
{
	switch (expr->type) {
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_VAR: {
			_ccv_nnc_micro_loop_variable_free(&expr->variable);
			break;
		}
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_UNARY: {
			_ccv_nnc_micro_loop_expression_free(expr->unary.x);
			ccfree(expr->unary.x);
		}
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_BINARY: {
			_ccv_nnc_micro_loop_expression_free(expr->binary.left);
			ccfree(expr->binary.left);
			_ccv_nnc_micro_loop_expression_free(expr->binary.right);
			ccfree(expr->binary.right);
		}
	}
}

void ccv_nnc_micro_combine_free(ccv_nnc_micro_combine_t* const combine)
{
	int i, j, k, l;
	const int var_count = combine->var_count;
	for (i = 0; i < var_count; i++)
	{
		if (combine->vars[i].shape)
		{
			for (j = 0; j < combine->vars[i].dimensions; j++)
				_ccv_nnc_micro_loop_index_free(&combine->vars[i].shape[j]);
			ccfree(combine->vars[i].shape);
		}
	}
	ccfree(combine->vars);
	const int function_count = combine->function_count;
	for (i = 0; i < function_count; i++)
	{
		const int block_count = combine->functions[i].block_count;
		ccv_nnc_micro_loop_block_t* const blocks = (block_count == 1) ? &combine->functions[i].one_block : combine->functions[i].blocks;
		for (j = 0; j < block_count; j++)
		{
			ccv_nnc_micro_loop_block_t block = blocks[j];
			for (k = 0; k < block.loop_count; k++)
			{
				for (l = 0; l < block.loops[k].statement_count; l++)
				{
					ccv_nnc_micro_loop_statement_t statement = block.loops[k].statements[l];
					switch (statement.type) {
						case CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_COMPOUND_ASSIGNMENT: {
							_ccv_nnc_micro_loop_expression_free(&statement.compound_assignment.rvalue);
							break;
						}
						case CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_ASSIGNMENT: {
							_ccv_nnc_micro_loop_variable_free(&statement.assignment.lvalue);
							_ccv_nnc_micro_loop_expression_free(&statement.assignment.rvalue);
							break;
						}
					}
				}
				if (block.loops[k].statements)
					ccfree(block.loops[k].statements);
				if (block.loops[k].carrieds)
					ccfree(block.loops[k].carrieds);
			}
			ccfree(block.loops);
		}
		if (block_count != 1)
			ccfree(combine->functions[i].blocks);
	}
	ccfree(combine->functions);
	ccfree(combine);
}

static int _ccv_nnc_micro_shape_interpret(const ccv_nnc_micro_loop_index_term_t shape, const int* const shapes, const ccv_nnc_micro_scalar_t* const values, const int parameter_size)
{
	switch (shape.type) {
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL:
			return shape.immediate_value;
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_ID:
			switch (shape.id.type) {
				case CCV_NNC_MICRO_AXIS_SIZE_ID:
					return shapes[shape.id.id * CCV_NNC_MAX_DIM_ALLOC + shape.id.d];
				case CCV_NNC_MICRO_SCALAR_ID:
					switch (values[shape.id.id].type) {
						case CCV_8U:
							return values[shape.id.id].u8;
						case CCV_32S:
							return values[shape.id.id].i32;
						case CCV_64S:
							return (int)values[shape.id.id].i64;
					}
					break;
			}
			break;
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY: {
			const int left = _ccv_nnc_micro_shape_interpret(shape.binary->left, shapes, values, parameter_size);
			const int right = _ccv_nnc_micro_shape_interpret(shape.binary->right, shapes, values, parameter_size);
			switch (shape.binary->op) {
				case CCV_NNC_MICRO_BINARY_OP_PLUS:
					return left + right;
				case CCV_NNC_MICRO_BINARY_OP_MINUS:
					return left - right;
				case CCV_NNC_MICRO_BINARY_OP_MUL:
					return left * right;
				case CCV_NNC_MICRO_BINARY_OP_DIV:
					return left / right;
				case CCV_NNC_MICRO_BINARY_OP_MAX:
					return ccv_max(left, right);
				case CCV_NNC_MICRO_BINARY_OP_MIN:
					return ccv_min(left, right);
			}
			break;
		}
	}
	return 0;
}

void ccv_nnc_micro_combine_interpret(ccv_nnc_micro_combine_t* const combine, const uint32_t cmd, ccv_nnc_tensor_t* const* const inputs, const int input_size, const ccv_nnc_micro_scalar_t* const values, const int parameter_size, ccv_nnc_tensor_t* const* const outputs, const int output_size)
{
	// We haven't optimized for emit_grad at the moment yet.
	assert(cmd == CCV_NNC_CUSTOM_FORWARD);
	int i, j;
	const int var_count = combine->var_count;
	assert(input_size == combine->input_size);
	assert(output_size == combine->output_size);
	assert(parameter_size == combine->parameter_size);
	int* const shapes = (int*)cccalloc(var_count, sizeof(int) * CCV_NNC_MAX_DIM_ALLOC);
	ccv_nnc_micro_tensor_t* const vars = combine->vars;
	for (i = 0; i < input_size; i++)
		memcpy(shapes + (var_count - input_size + i) * CCV_NNC_MAX_DIM_ALLOC, &inputs[i]->info.dim, sizeof(int) * CCV_NNC_MAX_DIM_ALLOC);
	for (i = var_count - input_size - 1; i >= 0; i--)
	{
		if (vars[i].shape)
		{
			for (j = 0; j < vars[i].dimensions; j++)
				shapes[i * CCV_NNC_MAX_DIM_ALLOC + j] = _ccv_nnc_micro_shape_interpret(vars[i].shape[j], shapes, values, parameter_size);
		} else
			memcpy(shapes + i * CCV_NNC_MAX_DIM_ALLOC, shapes + vars[i].input * CCV_NNC_MAX_DIM_ALLOC, sizeof(int) * CCV_NNC_MAX_DIM_ALLOC);
	}
	const int function_count = combine->function_count;
	for (i = 0; i < function_count; i++)
	{
	}
	ccfree(shapes);
}

char* ccv_nnc_micro_combine_c(ccv_nnc_micro_combine_t* const combine)
{
	return 0;
}