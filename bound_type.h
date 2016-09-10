#pragma once
#include "zion.h"
#include "dbg.h"
#include "status.h"
#include "utils.h"
#include <string>
#include <map>
#include "ast_decls.h"
#include "types.h"
#include "signature.h"

struct bound_var_t;

struct bound_type_t {
	typedef ptr<const bound_type_t> ref;
	typedef std::weak_ptr<const bound_type_t> weak_ref;
	typedef std::vector<std::pair<atom, ref>> named_pairs;
	typedef std::vector<ref> refs;
	typedef std::map<types::signature, ref> map;
	typedef atom::map<int> name_index;

protected:
	bound_type_t() {}
	virtual ~bound_type_t() {}

public:
	std::string str() const;
	bool is_function() const;
	bool is_void() const;
	bool is_obj() const;
	bool is_struct() const;
	types::signature get_signature() const;

	virtual types::type::ref get_type() const = 0;
	virtual struct location const get_location() const = 0;
	virtual llvm::Type * const get_llvm_type() const = 0;
	virtual llvm::Type * const get_llvm_specific_type() const = 0;
	virtual refs const get_dimensions() const = 0;
	virtual name_index const get_member_index() const = 0;

	static refs refs_from_vars(const std::vector<ptr<const bound_var_t>> &vars);

	types::term::ref get_term() const;

	static ref create(
			types::type::ref type,
			struct location location,
			llvm::Type *llvm_type,
			llvm::Type *llvm_specific_type = nullptr,
			refs dimensions = {},
			name_index member_index = {});

	static ref create_handle(ref handle);
};

struct bound_type_impl_t : public bound_type_t {
	bound_type_impl_t(
			types::type::ref type,
			location location,
			llvm::Type *llvm_type,
			llvm::Type *llvm_specific_type,
			refs dimensions,
			name_index member_index);
	virtual ~bound_type_impl_t() {}

	bound_type_impl_t(const bound_type_impl_t &) = delete;
	bound_type_impl_t(const bound_type_impl_t &&) = delete;
	bound_type_impl_t &operator =(const bound_type_impl_t &) = delete;

	virtual types::type::ref get_type() const;
	virtual struct location const get_location() const;
	virtual llvm::Type * const get_llvm_type() const;
	virtual llvm::Type * const get_llvm_specific_type() const;
	virtual refs const get_dimensions() const;
	virtual name_index const get_member_index() const;

	types::type::ref type;
	struct location location;
	llvm::Type * const llvm_type;
	llvm::Type * const llvm_specific_type;
	refs const dimensions;
	name_index const member_index;
};

struct bound_type_handle_t : public bound_type_t {
	bound_type_handle_t(bound_type_t::ref actual);
	virtual ~bound_type_handle_t() {}

	bound_type_handle_t(const bound_type_handle_t &) = delete;
	bound_type_handle_t(const bound_type_handle_t &&) = delete;
	bound_type_handle_t &operator =(const bound_type_handle_t &) = delete;

	virtual types::type::ref get_type() const;
	virtual struct location const get_location() const;
	virtual llvm::Type * const get_llvm_type() const;
	virtual llvm::Type * const get_llvm_specific_type() const;
	virtual refs const get_dimensions() const;
	virtual name_index const get_member_index() const;

	ref actual;
};

std::string str(const bound_type_t::refs &args);
std::string str(const bound_type_t::named_pairs &named_pairs);
std::ostream &operator <<(std::ostream &os, const bound_type_t &type);

types::term::ref get_tuple_term(types::term::refs dimensions);
types::term::ref get_tuple_term(const bound_type_t::refs &items_types);
types::term::ref get_function_term(bound_type_t::named_pairs args, bound_type_t::ref ret);
types::term::ref get_function_term(bound_type_t::refs args, bound_type_t::ref return_value);
types::term::ref get_function_term(bound_type_t::refs args, types::term::ref return_value);
types::term::ref get_function_term(bound_type_t::refs args, types::term::ref return_value);
types::term::ref get_args_term(bound_type_t::refs args);
types::term::refs get_terms(const bound_type_t::refs &types);

types::type::ref get_function_type(bound_type_t::refs args, bound_type_t::ref return_type);
