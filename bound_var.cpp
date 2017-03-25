#include "bound_var.h"
#include "llvm_utils.h"
#include "ast.h"
#include "parser.h"

std::string bound_var_t::str() const {
	std::stringstream ss;
	ss << C_VAR << name << C_RESET;
	ss << " : " << id->str();
	ss << " : " << *type;

	assert(llvm_value != nullptr);

	if (debug_level() >= 10) {
		ss << " IR: ";
		std::string llir = llvm_print(*llvm_value);
		trim(llir);
		ss << C_IR << llvm_value;
		ss << " : " << llir << C_RESET;
		ss << " " << internal_location.str();
	}
    return ss.str();
}

location_t bound_var_t::get_location() const {
	return id->get_location();
}

bool bound_var_t::is_lhs() const {
	assert(!_is_global);
	return _is_lhs;
}

bool bound_var_t::is_global() const {
	return _is_global;
}

bool bound_var_t::is_int() const {
	/* anything that is an integer value is a bool under the covers */
	return llvm_resolve_type(llvm_value)->isIntegerTy();
}

bool bound_var_t::is_pointer() const {
	/* anything that is an pointer */
	return llvm_resolve_type(llvm_value)->isPointerTy();
}

llvm::Value *bound_var_t::get_llvm_value() const {
	return llvm_value;
}

llvm::Value *bound_var_t::resolve_value(llvm::IRBuilder<> &builder) const {
	if (_is_lhs) {
		return _llvm_resolve_alloca(builder, llvm_value);
	} else if (_is_global) {
		// maybe...
		assert(llvm_value->getType() == type->get_llvm_type()->getPointerTo());
		return builder.CreateLoad(llvm_value);
	} else {
		return llvm_value;
	}
}

bound_var_t::ref bound_var_t::resolve_bound_value(llvm::IRBuilder<> &builder) const {
	return bound_var_t::create(
			INTERNAL_LOC(),
			this->name,
			type,
			resolve_value(builder),
			this->id,
			false /*is_lhs*/,
			false /*is_global*/);
}

std::ostream &operator <<(std::ostream &os, const bound_var_t &var) {
	return os << var.str();
}

std::string str(const bound_var_t::overloads &overloads) {
	std::stringstream ss;
	const char *indent = "\t";
	for (auto &var_overload : overloads) {
		ss << indent << var_overload.first.str() << ": ";
	   	ss << var_overload.second->str() << std::endl;
	}
	return ss.str();
}

std::string str(const bound_var_t::refs &args) {
	std::stringstream ss;
	ss << "[";
	ss << join_str(args, ", ");
	ss << "]";
	return ss.str();
}

bound_module_t::bound_module_t(
		location_t internal_location,
		atom name,
		identifier::ref id,
		module_scope_t::ref module_scope) :
	bound_var_t(internal_location,
			name, 
			module_scope->get_bound_type({"module"}),
			module_scope->get_program_scope()->get_singleton("nil")->get_llvm_value(),
			id,
			false /*is_lhs*/,
			false /*is_global*/),
	module_scope(module_scope)
{
	assert(module_scope != nullptr);
}

std::vector<llvm::Value *> get_llvm_values(
		llvm::IRBuilder<> &builder,
	   	const bound_var_t::refs &vars)
{
	std::vector<llvm::Value *> llvm_values;
	llvm_values.reserve(vars.size());

	for (auto var : vars) {
		llvm_values.push_back(var->resolve_value(builder));
	}

	return llvm_values;
}

bound_type_t::refs get_bound_types(bound_var_t::refs values) {
	bound_type_t::refs types;
	types.reserve(values.size());

	for (auto value : values) {
		types.push_back(value->type);
	}

	return types;
}

types::signature bound_var_t::get_signature() const {
	return type->get_signature();
}

types::type_t::ref bound_var_t::get_type(ptr<scope_t> scope) const {
	return type->get_type();
}

types::type_t::ref bound_var_t::get_type() const {
	return type->get_type();
}

#if 0
bound_var_t::ref resolve_alloca(llvm::IRBuilder<> &builder, bound_var_t::ref var) {
	if (llvm::AllocaInst *llvm_alloca = llvm::dyn_cast<llvm::AllocaInst>(
				var->get_llvm_value()))
   	{
		assert(var->is_lhs());
		assert(!var->is_global());
		return bound_var_t::create(
				INTERNAL_LOC(),
				string_format("%s.snapshot", var->name.c_str()),
				var->type,
				llvm_resolve_alloca(builder, llvm_alloca),
				var->id,
			   	false /*is_lhs*/,
				false /*is_global*/);
	} else {
		assert(!var->is_lhs());
		return var;
	}
}
#endif
