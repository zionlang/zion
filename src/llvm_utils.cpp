#include "llvm_utils.h"

#include <iostream>

#include "compiler.h"
#include "location.h"
#include "logger.h"
#include "utils.h"

const char *GC_STRATEGY = "zion";

llvm::Value *llvm_create_global_string(llvm::IRBuilder<> &builder,
                                       std::string value) {
  return builder.CreateGlobalStringPtr(value);
}

llvm::Constant *llvm_get_pointer_to_constant(llvm::IRBuilder<> &builder,
                                             llvm::Constant *llvm_constant) {
  assert(llvm::dyn_cast<llvm::PointerType>(llvm_constant->getType()));

  debug_above(9, log(log_info, "getting pointer to constant %s",
                     llvm_print(llvm_constant).c_str()));

  std::vector<llvm::Constant *> gep_indices = {builder.getInt32(0),
                                               builder.getInt32(0)};

  return llvm::ConstantExpr::getInBoundsGetElementPtr(nullptr, llvm_constant,
                                                      gep_indices);
}

llvm::Constant *llvm_create_global_string_constant(llvm::IRBuilder<> &builder,
                                                   llvm::Module &M,
                                                   std::string str) {
  llvm::LLVMContext &Context = builder.getContext();
  llvm::Constant *StrConstant = llvm::ConstantDataArray::getString(Context,
                                                                   str);
  std::string name = std::string("__global_string_") + str;
  llvm::GlobalVariable *llvm_value = llvm_get_global(&M, name, StrConstant,
                                                     true /*is_constant*/);
  return llvm_get_pointer_to_constant(builder, llvm_value);
}

llvm::Value *llvm_create_bool(llvm::IRBuilder<> &builder, bool value) {
  if (value) {
    return builder.getTrue();
  } else {
    return builder.getFalse();
  }
}

llvm::ConstantInt *llvm_create_int(llvm::IRBuilder<> &builder, int64_t value) {
  return builder.getZionInt(value);
}

llvm::ConstantInt *llvm_create_int16(llvm::IRBuilder<> &builder,
                                     int16_t value) {
  return builder.getInt16(value);
}

llvm::ConstantInt *llvm_create_int32(llvm::IRBuilder<> &builder,
                                     int32_t value) {
  return builder.getInt32(value);
}

llvm::Value *llvm_create_double(llvm::IRBuilder<> &builder, double value) {
  return llvm::ConstantFP::get(builder.getContext(), llvm::APFloat(value));
}

llvm::FunctionType *llvm_create_function_type(
    llvm::IRBuilder<> &builder,
    const std::vector<llvm::Type *> &llvm_type_args,
    llvm::Type *llvm_return_type) {
  return llvm::FunctionType::get(llvm_return_type,
                                 llvm::ArrayRef<llvm::Type *>(llvm_type_args),
                                 false /*isVarArg*/);
}

llvm::Type *llvm_resolve_type(llvm::Value *llvm_value) {
  if (llvm::AllocaInst *alloca = llvm::dyn_cast<llvm::AllocaInst>(llvm_value)) {
    assert(llvm_value->getType()->isPointerTy());
    return alloca->getAllocatedType();
  } else {
    return llvm_value->getType();
  }
}

llvm::Value *_llvm_resolve_alloca(llvm::IRBuilder<> &builder,
                                  llvm::Value *llvm_value) {
  if (llvm::AllocaInst *alloca = llvm::dyn_cast<llvm::AllocaInst>(llvm_value)) {
    return builder.CreateLoad(alloca);
  } else {
    return llvm_value;
  }
}

llvm::CallInst *llvm_create_call_inst(llvm::IRBuilder<> &builder,
                                      llvm::Value *llvm_callee_value,
                                      std::vector<llvm::Value *> llvm_values) {
  debug_above(9, log("found llvm_callee_value %s of type %s",
                     llvm_print(llvm_callee_value).c_str(),
                     llvm_print(llvm_callee_value->getType()).c_str()));

  llvm::Value *llvm_function = nullptr;
  llvm::FunctionType *llvm_function_type = nullptr;
  llvm::Function *llvm_func_decl = nullptr;

  if (llvm::Function *llvm_callee_fn = llvm::dyn_cast<llvm::Function>(
          llvm_callee_value)) {
    /* see if we have an exact function we want to call */

    /* get the current module we're inserting code into */
    llvm::Module *llvm_module = llvm_get_module(builder);

    debug_above(3,
                log(log_info,
                    "looking for function in LLVM " c_id("%s") " with type %s",
                    llvm_callee_fn->getName().str().c_str(),
                    llvm_print(llvm_callee_fn->getFunctionType()).c_str()));

    /* before we can call a function, we must make sure it either exists in
     * this module, or a declaration exists */
    llvm_func_decl = llvm::cast<llvm::Function>(
        llvm_module->getOrInsertFunction(llvm_callee_fn->getName(),
                                         llvm_callee_fn->getFunctionType(),
                                         llvm_callee_fn->getAttributes()));

    llvm_function_type = llvm::dyn_cast<llvm::FunctionType>(
        llvm_func_decl->getType()->getElementType());
    llvm_function = llvm_func_decl;
  } else {
    llvm_function = llvm_callee_value;

    llvm::PointerType *llvm_ptr_type = llvm::dyn_cast<llvm::PointerType>(
        llvm_callee_value->getType());
    assert(llvm_ptr_type != nullptr);

    debug_above(8,
                log("llvm_ptr_type is %s", llvm_print(llvm_ptr_type).c_str()));
    llvm_function_type = llvm::dyn_cast<llvm::FunctionType>(
        llvm_ptr_type->getElementType());
    assert(llvm_function_type != nullptr);
  }

  assert(llvm_function != nullptr);
  assert(llvm_function_type != nullptr);
  debug_above(3, log(log_info, "creating call to %s",
                     llvm_print(llvm_function_type).c_str()));

  if (llvm_function_type->getNumParams() - 1 == llvm_values.size()) {
    /* no closure, but we need to pad the inputs in this case. */
    llvm_values.push_back(
        llvm::Constant::getNullValue(builder.getInt8Ty()->getPointerTo()));
  }

  llvm::ArrayRef<llvm::Value *> llvm_args_array(llvm_values);

  debug_above(3, log(log_info, "creating call to " c_id("%s") " %s with [%s]",
                     llvm_func_decl ? llvm_func_decl->getName().str().c_str()
                                    : "a function",
                     llvm_print(llvm_function_type).c_str(),
                     join_with(llvm_values, ", ", llvm_print_value).c_str()));

  return builder.CreateCall(llvm_function, llvm_args_array);
}

llvm::Module *llvm_get_module(llvm::IRBuilder<> &builder) {
  return builder.GetInsertBlock()->getParent()->getParent();
}

llvm::Function *llvm_get_function(llvm::IRBuilder<> &builder) {
  return builder.GetInsertBlock()->getParent();
}

std::string llvm_print_module(llvm::Module &llvm_module) {
  std::stringstream ss;
  llvm::raw_os_ostream os(ss);
  llvm_module.print(os, nullptr /*AssemblyAnnotationWriter*/);
  os.flush();
  return ss.str();
}

std::string llvm_print_function(llvm::Function *llvm_function) {
  std::stringstream ss;
  llvm::raw_os_ostream os(ss);
  llvm_function->print(os, nullptr /*AssemblyAnnotationWriter*/);
  os.flush();
  return ss.str();
}

std::string llvm_print_type(llvm::Type *llvm_type) {
  assert(llvm_type != nullptr);
  return llvm_print(llvm_type);
}

std::string llvm_print_value(llvm::Value *llvm_value) {
  assert(llvm_value != nullptr);
  return llvm_print(*llvm_value);
}

std::string llvm_print(llvm::Value *llvm_value) {
  assert(llvm_value != nullptr);
  return llvm_print(*llvm_value);
}

std::string llvm_print(llvm::Value &llvm_value) {
  std::stringstream ss;
  llvm::raw_os_ostream os(ss);
  llvm_value.print(os);
  os.flush();
  ss << " : " << C_IR;
  llvm_value.getType()->print(os);
  os.flush();
  ss << C_RESET;
  return ss.str();
}

std::string llvm_print(llvm::Type *llvm_type) {
  std::stringstream ss;
  llvm::raw_os_ostream os(ss);
  ss << C_IR;
  if (llvm_type->isPointerTy()) {
    llvm_type = llvm::cast<llvm::PointerType>(llvm_type)->getElementType();
    ss << " {";
    llvm_type->print(os);
    os.flush();
    ss << "}*";
  } else {
    llvm_type->print(os);
    os.flush();
  }
  ss << C_RESET;
  return ss.str();
}

llvm::AllocaInst *llvm_create_entry_block_alloca(llvm::Function *llvm_function,
                                                 types::type_t::ref type,
                                                 std::string var_name) {
  /* we'll need to place the alloca instance in the entry block, so let's
   * make a builder that points there */
  llvm::IRBuilder<> builder(&llvm_function->getEntryBlock(),
                            llvm_function->getEntryBlock().begin());

  /* create the local variable */
  return builder.CreateAlloca(get_llvm_type(builder, type), nullptr,
                              var_name.c_str());
}

llvm::Value *llvm_zion_bool_to_i1(llvm::IRBuilder<> &builder,
                                  llvm::Value *llvm_value) {
  if (llvm_value->getType()->isIntegerTy(1)) {
    return llvm_value;
  }

  llvm::Type *llvm_type = llvm_value->getType();
  assert(llvm_type->isIntegerTy());
  if (!llvm_type->isIntegerTy(1)) {
    llvm::Constant *zero = llvm::ConstantInt::get(llvm_type, 0);
    llvm_value = builder.CreateICmpNE(llvm_value, zero);
  }
  assert(llvm_value->getType()->isIntegerTy(1));
  return llvm_value;
}

void llvm_create_if_branch(llvm::IRBuilder<> &builder,
                           llvm::Value *llvm_value,
                           llvm::BasicBlock *then_bb,
                           llvm::BasicBlock *else_bb) {
  /* the job of this function is to derive a value from the input value that is
   * a valid input to a branch instruction */
  builder.CreateCondBr(llvm_zion_bool_to_i1(builder, llvm_value), then_bb,
                       else_bb);
}

#if 0
bound_var_t::ref create_global_str(
		llvm::IRBuilder<> &builder,
	   	scope_t::ref scope,
	   	location_t location,
	   	std::string value)
{
	auto program_scope = scope->get_program_scope();

	bound_type_t::ref str_type = upsert_bound_type(builder, scope, type_id(make_iid_impl(MANAGED_STR, location)));
	bound_type_t::ref str_literal_type = program_scope->get_runtime_type(builder, "str_literal_t", true /*get_ptr*/);
	bound_type_t::ref owning_buffer_literal_type = program_scope->get_runtime_type(builder, "owning_buffer_literal_t", true /*get_ptr*/);

	llvm::Module *llvm_module = scope->get_llvm_module(builder);

	debug_above(8, log("creating owning buffer for string literal \"%s\"", value.c_str()));

	std::string owning_buffer_literal_name = string_format("__internal.owning_buffer_literal_%d", atomize(value));
	bound_var_t::ref owning_buffer_literal = program_scope->get_bound_variable(builder, location, owning_buffer_literal_name);
	llvm::Constant *llvm_owning_buffer_literal;

	if (owning_buffer_literal == nullptr) {
		llvm_owning_buffer_literal = llvm_get_global(
				llvm_module,
				owning_buffer_literal_name,
				llvm_create_constant_struct_instance(
					llvm::dyn_cast<llvm::StructType>(owning_buffer_literal_type->get_llvm_type()->getPointerElementType()),
					{
					builder.getInt32(atomize("OwningBuffer")),
                    llvm_create_global_string_constant(builder, *llvm_module, value),
					llvm_create_int(builder, value.size()),
					}),
				true /*isConstant*/);
		program_scope->put_bound_variable(owning_buffer_literal_name, make_bound_var(
					INTERNAL_LOC(),
					owning_buffer_literal_name,
					owning_buffer_literal_type,
					llvm_owning_buffer_literal,
					make_iid_impl(owning_buffer_literal_name, location)));
	} else {
		llvm_owning_buffer_literal = (llvm::Constant *)owning_buffer_literal->get_llvm_value(nullptr);
	}

	debug_above(8, log("creating str literal \"%s\"", value.c_str()));
	std::string str_literal_name = string_format("__internal.str_literal_%d", atomize(value));
	bound_var_t::ref str_literal = program_scope->get_bound_variable(builder, location, str_literal_name);
	llvm::Constant *llvm_str_literal;

	if (str_literal == nullptr) {
		llvm_str_literal = llvm_get_global(
				llvm_module,
				"str_literal",
				llvm_create_constant_struct_instance(
					llvm::dyn_cast<llvm::StructType>(str_literal_type->get_llvm_type()->getPointerElementType()),
					{
					builder.getInt32(atomize(MANAGED_STR)),
					llvm_owning_buffer_literal,
					llvm_create_int(builder, 0),
					llvm_create_int(builder, value.size()),
					}),
				true /*isConstant*/);
		str_literal = make_bound_var(
				INTERNAL_LOC(),
				str_literal_name,
				str_type,
				builder.CreateBitCast(llvm_str_literal, str_type->get_llvm_type()),
				make_iid_impl(str_literal_name, location));
		program_scope->put_bound_variable(str_literal_name, str_literal);
	}

	return str_literal;
}
#endif

llvm::Constant *llvm_create_struct_instance(
    std::string var_name,
    llvm::Module *llvm_module,
    llvm::StructType *llvm_struct_type,
    std::vector<llvm::Constant *> llvm_struct_data) {
  debug_above(5, log("creating struct %s with %s",
                     llvm_print(llvm_struct_type).c_str(),
                     join_with(llvm_struct_data, ", ",
                               [](llvm::Constant *c) -> std::string {
                                 return llvm_print(c);
                               })
                         .c_str()));

  return llvm_get_global(
      llvm_module, var_name,
      llvm_create_constant_struct_instance(llvm_struct_type, llvm_struct_data),
      true /*is_constant*/);
}

llvm::Constant *llvm_create_constant_struct_instance(
    llvm::StructType *llvm_struct_type,
    std::vector<llvm::Constant *> llvm_struct_data) {
  assert(llvm_struct_type != nullptr);
  llvm::ArrayRef<llvm::Constant *> llvm_struct_initializer{llvm_struct_data};
  check_struct_initialization(llvm_struct_initializer, llvm_struct_type);

  return llvm::ConstantStruct::get(llvm_struct_type, llvm_struct_data);
}

llvm::StructType *llvm_create_struct_type(
    llvm::IRBuilder<> &builder,
    std::string name,
    const std::vector<llvm::Type *> &llvm_types) {
  llvm::ArrayRef<llvm::Type *> llvm_dims{llvm_types};

  auto llvm_struct_type = llvm::StructType::create(builder.getContext(),
                                                   llvm_dims);

  /* give the struct a helpful name internally */
  llvm_struct_type->setName(name);

  debug_above(3, log(log_info, "created struct type " c_id("%s") " %s",
                     name.c_str(), llvm_print(llvm_struct_type).c_str()));

  return llvm_struct_type;
}

llvm::StructType *llvm_create_struct_type(
    llvm::IRBuilder<> &builder,
    std::string name,
    const types::type_t::refs &dimensions) {
  return llvm_create_struct_type(builder, name,
                                 get_llvm_types(builder, dimensions));
}

void llvm_verify_function(location_t location, llvm::Function *llvm_function) {
  debug_above(5, log("writing to function-verification-failure.llir..."));
  std::string llir_filename = "function-verification-failure.llir";
#if 1
	FILE *fp = fopen(llir_filename.c_str(), "wt");
	fprintf(fp, "%s\n", llvm_print_module(*llvm_function->getParent()).c_str());
	fclose(fp);
#endif

  std::stringstream ss;
  llvm::raw_os_ostream os(ss);
  if (llvm::verifyFunction(*llvm_function, &os)) {
    os.flush();
    ss << llvm_print_function(llvm_function);
    auto error = user_error(location, "LLVM function verification failed: %s",
                            ss.str().c_str());
    error.add_info(location_t{llir_filename, 1, 1}, "consult LLVM module dump");
    throw error;
  }
}

void llvm_verify_module(llvm::Module &llvm_module) {
  std::stringstream ss;
  llvm::raw_os_ostream os(ss);
  if (llvm::verifyModule(llvm_module, &os)) {
    os.flush();
    throw user_error(location_t{},
                     "module %s: failed verification. %s\nModule listing:\n%s",
                     llvm_module.getName().str().c_str(), ss.str().c_str(),
                     llvm_print_module(llvm_module).c_str());
  }
}

llvm::Constant *llvm_sizeof_type(llvm::IRBuilder<> &builder,
                                 llvm::Type *llvm_type) {
  llvm::StructType *llvm_struct_type = llvm::dyn_cast<llvm::StructType>(
      llvm_type);
  if (llvm_struct_type != nullptr) {
    if (llvm_struct_type->isOpaque()) {
      debug_above(1, log("llvm_struct_type is opaque when we're trying to get "
                         "its size: %s",
                         llvm_print(llvm_struct_type).c_str()));
      assert(false);
    }
    assert(llvm_struct_type->elements().size() != 0);
  }

  llvm::Constant *alloc_size_const = llvm::ConstantExpr::getSizeOf(llvm_type);
  llvm::Constant *size_value = llvm::ConstantExpr::getTruncOrBitCast(
      alloc_size_const, builder.getInt64Ty());
  debug_above(3,
              log(log_info, "size of %s is: %s", llvm_print(llvm_type).c_str(),
                  llvm_print(*size_value).c_str()));
  return size_value;
}

llvm::Type *llvm_deref_type(llvm::Type *llvm_type) {
  if (llvm_type->isPointerTy()) {
    return llvm::cast<llvm::PointerType>(llvm_type)->getElementType();
  } else {
    return llvm_type;
  }
}

llvm::Function *llvm_start_function(llvm::IRBuilder<> &builder,
                                    llvm::Module *llvm_module,
                                    const types::type_t::refs &terms,
                                    std::string name) {
  log("llvm_start_function(..., ..., {%s}, %s)...", join_str(terms).c_str(),
      name.c_str());
  assert(terms.size() == 3);
  std::vector<llvm::Type *> llvm_type_terms = get_llvm_types(builder, terms);

  /* get the llvm function type for the data ctor */
  llvm::FunctionType *llvm_fn_type = llvm_create_function_type(
      builder, vec_slice(llvm_type_terms, 0, llvm_type_terms.size() - 1),
      llvm_type_terms.back());

  /* now let's generate our actual data ctor fn */
  auto llvm_function = llvm::Function::Create(
      llvm_fn_type, llvm::Function::ExternalLinkage, name,
      llvm_module != nullptr ? llvm_module : llvm_get_module(builder));

  llvm_function->setDoesNotThrow();

#if 0
	/* start emitting code into the new function. caller should have an
	 * insert point guard */
	llvm::BasicBlock *llvm_entry_block = llvm::BasicBlock::Create(builder.getContext(),
			"entry", llvm_function);
	llvm::BasicBlock *llvm_body_block = llvm::BasicBlock::Create(builder.getContext(),
			"body", llvm_function);

	builder.SetInsertPoint(llvm_entry_block);
	/* leave an empty entry block so that we can insert GC stuff in there, but be able to
	 * seek to the end of it and not get into business logic */
	assert(!builder.GetInsertBlock()->getTerminator());
	builder.CreateBr(llvm_body_block);

	builder.SetInsertPoint(llvm_body_block);
#endif

  return llvm_function;
}

void check_struct_initialization(
    llvm::ArrayRef<llvm::Constant *> llvm_struct_initialization,
    llvm::StructType *llvm_struct_type) {
  if (llvm_struct_type->elements().size() !=
      llvm_struct_initialization.size()) {
    debug_above(7, log(log_error,
                       "mismatch in number of elements for %s (%d != %d)",
                       llvm_print(llvm_struct_type).c_str(),
                       (int)llvm_struct_type->elements().size(),
                       (int)llvm_struct_initialization.size()));
    assert(false);
  }

  for (unsigned i = 0, e = llvm_struct_initialization.size(); i != e; ++i) {
    if (llvm_struct_initialization[i]->getType() ==
        llvm_struct_type->getElementType(i)) {
      continue;
    } else {
      debug_above(
          7, log(log_error,
                 "llvm_struct_initialization[%d] mismatch is %s should be %s",
                 i, llvm_print(*llvm_struct_initialization[i]).c_str(),
                 llvm_print(llvm_struct_type->getElementType(i)).c_str()));
      assert(false);
    }
  }
}

llvm::GlobalVariable *llvm_get_global(llvm::Module *llvm_module,
                                      std::string name,
                                      llvm::Constant *llvm_constant,
                                      bool is_constant) {
  auto llvm_global_variable = new llvm::GlobalVariable(
      *llvm_module, llvm_constant->getType(), is_constant,
      llvm::GlobalValue::PrivateLinkage, llvm_constant, name, nullptr,
      llvm::GlobalVariable::NotThreadLocal);

  // llvm_global_variable->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  return llvm_global_variable;
}

#if 0
bound_var_t::ref llvm_create_global_tag(
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		bound_type_t::ref tag_type,
		std::string tag,
		identifier::ref id)
{
	auto program_scope = scope->get_program_scope();

	bound_type_t::ref var_ptr_type = program_scope->get_runtime_type(builder, STD_MANAGED_TYPE, true /*get_ptr*/);
	llvm::Type *llvm_var_ptr_type = var_ptr_type->get_llvm_type();

	llvm::StructType *llvm_tag_struct_type = llvm::dyn_cast<llvm::StructType>(llvm_var_ptr_type->getPointerElementType());
	debug_above(10, log(log_info, "var_ptr_type is %s", llvm_print(var_ptr_type->get_llvm_type()).c_str()));
	debug_above(10, log(log_info, "tag_struct_type is %s", llvm_print(llvm_tag_struct_type).c_str()));
	assert(llvm_tag_struct_type != nullptr);

	llvm::Module *llvm_module = scope->get_llvm_module(builder);
	assert(llvm_module != nullptr);

	llvm::Constant *llvm_name = llvm_create_global_string_constant(builder, *llvm_module, tag);
	debug_above(10, log(log_info, "llvm_name is %s", llvm_print(*llvm_name).c_str()));

	std::vector<llvm::Constant *> llvm_struct_data_tag = {
		builder.getInt32(atomize(tag)),
	};

	/* create the actual tag singleton */
	llvm::Constant *llvm_tag_constant = llvm_create_struct_instance(
			std::string("__tag_") + tag,
			llvm_module,
			llvm_tag_struct_type,
			llvm_struct_data_tag);

	return make_bound_var(INTERNAL_LOC(), tag, tag_type, llvm_tag_constant, id);
}
#endif

llvm::Value *llvm_maybe_pointer_cast(llvm::IRBuilder<> &builder,
                                     llvm::Value *llvm_value,
                                     llvm::Type *llvm_type) {
  if (llvm_value->getType() == llvm_type) {
    return llvm_value;
  }

  if (llvm_type->isPointerTy()) {
    debug_above(6, log("attempting to cast %s to a %s",
                       llvm_print(llvm_value).c_str(),
                       llvm_print(llvm_type).c_str()));
    assert(llvm_value->getType()->isPointerTy() ||
           llvm_value->getType()->isIntegerTy());
    assert(llvm_value->getType() != llvm_type->getPointerTo());

    if (llvm_type != llvm_value->getType()) {
      return builder.CreateBitCast(llvm_value, llvm_type);
    }
  }

  return llvm_value;
}

llvm::Value *llvm_int_cast(llvm::IRBuilder<> &builder,
                           llvm::Value *llvm_value,
                           llvm::Type *llvm_type) {
  return builder.CreateIntCast(llvm_value, llvm_type, false /*isSigned*/);
}

#if 0
llvm::Value *llvm_maybe_pointer_cast(
		llvm::IRBuilder<> &builder,
		llvm::Value *llvm_value,
		const bound_type_t::ref &bound_type)
{
	return llvm_maybe_pointer_cast(builder, llvm_value, bound_type->get_llvm_specific_type());
}
#endif

void explain(llvm::Type *llvm_type) {
  INDENT(6, string_format("explain %s", llvm_print(llvm_type).c_str()));

  if (auto llvm_struct_type = llvm::dyn_cast<llvm::StructType>(llvm_type)) {
    for (auto element : llvm_struct_type->elements()) {
      explain(element);
    }
  } else if (auto lp = llvm::dyn_cast<llvm::PointerType>(llvm_type)) {
    explain(lp->getElementType());
  }
}

bool llvm_value_is_handle(llvm::Value *llvm_value) {
  llvm::Type *llvm_type = llvm_value->getType();
  return llvm_type->isPointerTy() && llvm::cast<llvm::PointerType>(llvm_type)
                                         ->getElementType()
                                         ->isPointerTy();
}

bool llvm_value_is_pointer(llvm::Value *llvm_value) {
  llvm::Type *llvm_type = llvm_value->getType();
  return llvm_type->isPointerTy();
}

llvm::StructType *llvm_find_struct(llvm::Type *llvm_type) {
  if (auto llvm_struct_type = llvm::dyn_cast<llvm::StructType>(llvm_type)) {
    return llvm_struct_type;
  } else if (auto llvm_ptr_type = llvm::dyn_cast<llvm::PointerType>(
                 llvm_type)) {
    return llvm_find_struct(llvm_ptr_type->getElementType());
  } else {
    return nullptr;
  }
}

void llvm_generate_dead_return(llvm::IRBuilder<> &builder) {
  llvm::Function *llvm_function_current = llvm_get_function(builder);
  llvm::Type *llvm_return_type = llvm_function_current->getReturnType();
  llvm_dead_return(builder, llvm_return_type);
}

llvm::Instruction *llvm_dead_return(llvm::IRBuilder<> &builder,
                                    llvm::Type *llvm_return_type) {
  if (llvm_return_type->isPointerTy()) {
    return builder.CreateRet(llvm::Constant::getNullValue(llvm_return_type));
  } else if (llvm_return_type->isIntegerTy()) {
    return builder.CreateRet(llvm::ConstantInt::get(llvm_return_type, 0));
  } else if (llvm_return_type->isVoidTy()) {
    return builder.CreateRetVoid();
  } else if (llvm_return_type->isFloatTy()) {
    return builder.CreateRet(llvm::ConstantFP::get(llvm_return_type, 0.0));
  } else if (llvm_return_type->isDoubleTy()) {
    return builder.CreateRet(llvm::ConstantFP::get(llvm_return_type, 0.0));
  } else {
    log(log_error, "unhandled return type for dead return %s",
        llvm_print(llvm_return_type).c_str());
    assert(false && "Unhandled return type.");
    return nullptr;
  }
}

llvm::Value *llvm_last_param(llvm::Function *llvm_function) {
  llvm::Value *last = nullptr;
  for (auto arg = llvm_function->arg_begin(); arg != llvm_function->arg_end();
       ++arg) {
    last = &*arg;
  }
  assert(last != nullptr);
  return last;
}

llvm::Type *get_llvm_type(llvm::IRBuilder<> &builder,
                          const types::type_t::ref &type) {
  debug_above(7, log("get_llvm_type(%s)...", type->str().c_str()));
  if (auto id = dyncast<const types::type_id_t>(type)) {
    const std::string &name = id->id.name;
    if (name == INT_TYPE) {
      return builder.getZionIntTy();
    } else if (name == FLOAT_TYPE) {
      return builder.getFloatTy();
    } else {
      return builder.getInt8Ty()->getPointerTo();
    }
  } else if (auto tuple_type = dyncast<const types::type_tuple_t>(type)) {
    if (tuple_type->dimensions.size() == 0) {
      return builder.getInt8Ty()->getPointerTo();
    }
    std::vector<llvm::Type *> llvm_types = get_llvm_types(
        builder, tuple_type->dimensions);
    llvm::StructType *llvm_struct_type = llvm_create_struct_type(
        builder, type->repr(), llvm_types);
    return llvm_struct_type->getPointerTo();
  } else if (auto operator_ = dyncast<const types::type_operator_t>(type)) {
    types::type_t::refs terms;
    unfold_binops_rassoc(ARROW_TYPE_OPERATOR, type, terms);
    if (terms.size() <= 1) {
      /* anything user-defined gets passed around as an i8* */
      return builder.getInt8Ty()->getPointerTo();
    } else {
      auto ft = llvm_create_function_type(
                    builder, {get_llvm_type(builder, terms[0])},
                    get_llvm_type(builder, type_arrows(terms, 1)))
                    ->getPointerTo();
      log("get_llvm_type(..., %s) -> %s", type->str().c_str(),
          llvm_print(ft).c_str());
      return ft;
    }
  } else if (auto variable = dyncast<const types::type_variable_t>(type)) {
    assert(false);
    return nullptr;
  } else if (auto lambda = dyncast<const types::type_lambda_t>(type)) {
    assert(false);
    return nullptr;
  } else {
    assert(false);
    return nullptr;
  }
}

std::vector<llvm::Type *> get_llvm_types(llvm::IRBuilder<> &builder,
                                         const types::type_t::refs &types) {
  debug_above(7, log("get_llvm_types([%s])...", join_str(types, ", ").c_str()));
  std::vector<llvm::Type *> llvm_types;
  for (auto type : types) {
    llvm_types.push_back(get_llvm_type(builder, type));
  }
  return llvm_types;
}

std::vector<llvm::Type *> llvm_get_types(
    const std::vector<llvm::Value *> &llvm_values) {
  std::vector<llvm::Type *> llvm_types;
  for (auto llvm_value : llvm_values) {
    llvm_types.push_back(llvm_value->getType());
  }
  return llvm_types;
}
