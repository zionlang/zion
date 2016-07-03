#pragma once
#include "zion.h"
#include "ast_decls.h"
#include "token.h"
#include <vector>
#include <unordered_set>
#include <string>
#include "dbg.h"
#include <memory>
#include "scopes.h"
#include "type_checker.h"
#include "callable.h"


struct parse_state_t;

enum syntax_kind_t {
	sk_nil=0,

#define declare_syntax_kind(x) \
	sk_##x,

#define OP declare_syntax_kind
#include "sk_ops.h"
#undef OP

	sk_base_expr,
	sk_expression,
	sk_statement,
};

const char *skstr(syntax_kind_t sk);

namespace ast {
	struct render_state_t;

	struct item : std::enable_shared_from_this<item> {
		typedef ptr<const item> ref;

		virtual ~item() throw() = 0;
		std::string str() const;
		virtual void render(render_state_t &rs) const = 0;

		syntax_kind_t sk;
		zion_token_t token;
	};

	void log_named_item_create(const char *type, const std::string &name);

	template <typename T>
	ptr<T> create(const zion_token_t &token) {
		auto item = ptr<T>(new T());
		item->sk = T::SK;
		item->token = token;
		debug_ex(log_named_item_create(skstr(T::SK), token.text));
		return item;
	}

	template <typename T, typename... Args>
	ptr<T> create(const zion_token_t &token, Args... args) {
		auto item = ptr<T>(new T(args...));
		item->sk = T::SK;
		item->token = token;
		debug_ex(log_named_item_create(skstr(T::SK), token.text));
		return item;
	}

	struct statement : public item {
		typedef ptr<const statement> ref;

		static const syntax_kind_t SK = sk_statement;
		virtual ~statement() {}
		static ptr<ast::statement> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const = 0;
	};

	struct module;
	struct expression;
	struct var_decl;

	struct param_list_decl : public item {
		typedef ptr<const param_list_decl> ref;

		static const syntax_kind_t SK = sk_param_list_decl;
		static ptr<param_list_decl> parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<var_decl>> params;
	};

	struct param_list : public item {
		typedef ptr<const param_list> ref;

		static const syntax_kind_t SK = sk_param_list;
		static ptr<param_list> parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<expression>> expressions;
	};

	struct expression : public statement {
		typedef ptr<const expression> ref;

		static const syntax_kind_t SK = sk_expression;
		virtual ~expression() {}
		static ptr<expression> parse(parse_state_t &ps);
	};

	namespace postfix_expr {
		ptr<expression> parse(parse_state_t &ps);
	}

	struct continue_flow : public statement {
		typedef ptr<const continue_flow> ref;

		static const syntax_kind_t SK = sk_continue_flow;
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;
	};

	struct break_flow : public statement {
		typedef ptr<const break_flow> ref;

		static const syntax_kind_t SK = sk_break_flow;
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;
	};

	struct pass_flow : public statement {
		typedef ptr<const pass_flow> ref;

		static const syntax_kind_t SK = sk_pass_flow;
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;
	};

	struct callsite_expr : public expression {
		typedef ptr<const callsite_expr> ref;

		static const syntax_kind_t SK = sk_callsite_expr;
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression> function_expr;
		ptr<param_list> params;
	};

	struct return_statement : public statement {
		typedef ptr<const return_statement> ref;

		static const syntax_kind_t SK = sk_return_statement;
		static ptr<return_statement> parse(parse_state_t &ps);
		ptr<expression> expr;
		virtual void render(render_state_t &rs) const;

		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
	};

	struct type_decl : public item {
		typedef ptr<const type_decl> ref;

		type_decl(atom name, atom::many type_variables);
		static const syntax_kind_t SK = sk_type_decl;

		static ref parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		atom name;
		atom::many type_variables;
	};

	struct type_ref : public item {
		typedef ptr<const type_ref> ref;
		static ref parse(parse_state_t &ps);
		virtual ~type_ref() throw() {}

		virtual types::term::ref get_type_term() const = 0;
	};

	struct type_ref_named : public type_ref {
		static const syntax_kind_t SK = sk_type_ref_named;

		type_ref_named(types::term::ref term);
		virtual types::term::ref get_type_term() const;
		static ref parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		types::term::ref term;
	};

	struct type_ref_list : public type_ref {
		static const syntax_kind_t SK = sk_type_ref_list;

		type_ref_list(type_ref::ref type_ref);
		virtual types::term::ref get_type_term() const;
		static ref parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		type_ref::ref type_ref;
	};

	struct type_ref_tuple : public type_ref {
		static const syntax_kind_t SK = sk_type_ref_tuple;

		type_ref_tuple(std::vector<type_ref::ref> type_ref);
		virtual types::term::ref get_type_term() const;
		static ref parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		std::vector<type_ref::ref> type_refs;
	};

	struct type_ref_generic : public type_ref {
		static const syntax_kind_t SK = sk_type_ref_generic;

		type_ref_generic(types::term::ref term);
		virtual types::term::ref get_type_term() const;
		static ref parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		types::term::ref term;
	};

	struct dimension : public item {
		typedef ptr<const dimension> ref;
		static const syntax_kind_t SK = sk_dimension;
		dimension(atom name, type_ref::ref type_ref);
		virtual ~dimension() throw() {}
		virtual void render(render_state_t &rs) const;

		static ref parse(parse_state_t &ps);

		atom name;
		type_ref::ref type_ref;
	};

	struct data_ctor : public item {
		typedef ptr<const data_ctor> ref;
		static const syntax_kind_t SK = sk_data_ctor;

		data_ctor(atom::set type_variables, std::vector<type_ref::ref> type_ref_params);
		static ref parse(parse_state_t &ps, atom::set type_variables);
		virtual void render(render_state_t &rs) const;
		
		atom::set type_variables;
		std::vector<type_ref::ref> type_ref_params;
	};

	struct type_algebra : public item {
		typedef ptr<const type_algebra> ref;

		virtual ~type_algebra() throw() {}

		/* instantiate_type is called from within the scope where the type's
		 * ctors should end up living. this function should create the
		 * unchecked ctors with the type. */
		virtual types::term::ref instantiate_type(
				status_t &status,
				scope_t::ref scope) const = 0;

		static ref parse(parse_state_t &ps, ast::type_decl::ref type_decl);
	};

	struct type_sum : public type_algebra {
		typedef ptr<const type_sum> ref;

		type_sum(std::vector<data_ctor::ref> data_ctors);
		virtual ~type_sum() throw() {}
		static const syntax_kind_t SK = sk_type_sum;
		static ref parse(parse_state_t &ps, atom::many type_variables);
		virtual types::term::ref instantiate_type(status_t &status, scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;

		std::vector<data_ctor::ref> data_ctors;
	};

	struct type_product : public type_algebra {
		typedef ptr<const type_product> ref;

		type_product(std::vector<dimension::ref> dimensions, atom::set type_variables);
		virtual ~type_product() throw() {}
		static const syntax_kind_t SK = sk_type_product;
		static ref parse(parse_state_t &ps, atom::many type_variables);
		virtual types::term::ref instantiate_type(status_t &status, scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;

		atom::set type_variables;
		std::vector<dimension::ref> dimensions;
	};

	struct type_alias : public type_algebra {
		typedef ptr<const type_alias> ref;

		type_alias(type_ref::ref type_ref, atom::set type_variables);
		virtual ~type_alias() throw() {}
		static const syntax_kind_t SK = sk_type_alias;
		static ref parse(parse_state_t &ps, atom::many type_variables);
		virtual types::term::ref instantiate_type(status_t &status, scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;

		atom::set type_variables;
		type_ref::ref type_ref;
	};

	struct type_def : public statement {
		typedef ptr<const type_def> ref;

		static const syntax_kind_t SK = sk_type_def;
		static ptr<type_def> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		type_decl::ref type_decl;
		type_algebra::ref type_algebra;
	};

	struct var_decl : public statement {
		typedef ptr<const var_decl> ref;

		static const syntax_kind_t SK = sk_var_decl;
		static ptr<var_decl> parse(parse_state_t &ps);
		static ptr<var_decl> parse_param(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		/* the inherited ast::item::token member contains the actual identifier
		 * name */
		type_ref::ref type_ref;
		ptr<expression> initializer;
	};

	struct assignment : public expression {
		typedef ptr<const assignment> ref;

		static const syntax_kind_t SK = sk_assignment;
		static ptr<statement> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression> lhs, rhs;
	};

	struct plus_assignment : public statement {
		typedef ptr<const plus_assignment> ref;

		static const syntax_kind_t SK = sk_plus_assignment;
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression> lhs, rhs;
	};

	struct times_assignment : public statement {
		typedef ptr<const times_assignment> ref;

		static const syntax_kind_t SK = sk_times_assignment;
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression> lhs, rhs;
	};

	struct divide_assignment : public statement {
		typedef ptr<const divide_assignment> ref;

		static const syntax_kind_t SK = sk_divide_assignment;
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression> lhs, rhs;
	};

	struct minus_assignment : public statement {
		typedef ptr<const minus_assignment> ref;

		static const syntax_kind_t SK = sk_minus_assignment;
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression> lhs, rhs;
	};

	struct mod_assignment : public statement {
		typedef ptr<const mod_assignment> ref;

		static const syntax_kind_t SK = sk_mod_assignment;
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression> lhs, rhs;
	};

	struct block : public statement {
		typedef ptr<const block> ref;

		static const syntax_kind_t SK = sk_block;

		static ptr<block> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<statement>> statements;
	};

	struct function_decl : public item {
		typedef ptr<const function_decl> ref;

		static const syntax_kind_t SK = sk_function_decl;
		static ptr<function_decl> parse(parse_state_t &ps);

		virtual void render(render_state_t &rs) const;

		type_ref::ref return_type_ref;
		ptr<param_list_decl> param_list_decl;
	};

	struct function_defn : public expression {
		typedef ptr<const function_defn> ref;

		static const syntax_kind_t SK = sk_function_defn;

		static ptr<function_defn> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		bound_var_t::ref instantiate_with_args_and_return_type(
				status_t &status,
			   	llvm::IRBuilder<> &builder,
			   	scope_t::ref block_scope,
				local_scope_t::ref *new_scope,
				bound_type_t::named_pairs args,
				bound_type_t::ref return_type) const;
		virtual void render(render_state_t &rs) const;

		ptr<function_decl> decl;
		ptr<block> block;
	};

	struct if_block : public statement {
		typedef ptr<const if_block> ref;

		static const syntax_kind_t SK = sk_if_block;

		static ptr<if_block> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression> condition;
		ptr<block> block;
		ptr<statement> else_;
	};

	struct while_block : public statement {
		typedef ptr<const while_block> ref;

		static const syntax_kind_t SK = sk_while_block;

		static ptr<while_block> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression> condition;
		ptr<block> block;
	};

	struct semver : public item {
		typedef ptr<const semver> ref;
		virtual void render(render_state_t &rs) const;

		static const syntax_kind_t SK = sk_semver;
		static ptr<semver> parse(parse_state_t &ps);
	};

	struct module_decl : public item {
		typedef ptr<const module_decl> ref;

		static const syntax_kind_t SK = sk_module_decl;

		static ptr<module_decl> parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		ptr<semver> semver;
		std::string get_canonical_name() const;
		zion_token_t get_name() const;
	private:
		zion_token_t name;
	};

	struct link_module_statement : public statement {
		typedef ptr<const link_module_statement> ref;

		static const syntax_kind_t SK = sk_link_module_statement;

		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		zion_token_t link_as_name;
		ptr<module_decl> extern_module;
	};

	struct link_function_statement : public statement {
		typedef ptr<const link_function_statement> ref;

		static const syntax_kind_t SK = sk_link_function_statement;

		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref block_scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		zion_token_t link_as_name;
		ptr<function_decl> extern_function;
	};

	struct module : public std::enable_shared_from_this<module>, public item {
		typedef ptr<const module> ref;

		static const syntax_kind_t SK = sk_module;

		module(const atom filename);
		static ptr<module> parse(parse_state_t &ps);
		std::string get_canonical_name() const;
		virtual void render(render_state_t &rs) const;

		atom filename;
		atom module_key, filename_key;

		ptr<module_decl> decl;
		std::vector<ptr<type_def>> type_defs;
		std::vector<ptr<function_defn>> functions;
		std::vector<ptr<link_module_statement>> linked_modules;
		std::vector<ptr<link_function_statement>> linked_functions;
	};

	struct program : public item {
		typedef ptr<const program> ref;

		static const syntax_kind_t SK = sk_program;
		virtual ~program() {}
		virtual void render(render_state_t &rs) const;

		std::unordered_set<ptr<const module>> modules;
	};

	struct dot_expr : public expression, public can_reference_overloads_t {
		typedef ptr<const dot_expr> ref;

		static const syntax_kind_t SK = sk_dot_expr;
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
        virtual bound_var_t::ref resolve_overrides(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, const ptr<const ast::item> &obj, const bound_type_t::refs &args) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression> lhs;
		zion_token_t rhs;
	};

	struct binary_expr : public expression {
		/* this is a helper class to consolidate overload resolution logic */
		virtual atom get_function_name() const;
	};

	struct tuple_expr : public expression {
		typedef ptr<const tuple_expr> ref;

		static const syntax_kind_t SK = sk_tuple_expr;
		static ptr<ast::expression> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<ast::expression>> values;
	};

	struct or_expr : public binary_expr {
		typedef ptr<const or_expr> ref;

		static const syntax_kind_t SK = sk_or_expr;
		static ptr<ast::expression> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression> lhs, rhs;
	};

	struct and_expr : public binary_expr {
		typedef ptr<const and_expr> ref;

		static const syntax_kind_t SK = sk_and_expr;
		static ptr<ast::expression> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression> lhs, rhs;
	};

	struct eq_expr : public binary_expr {
		typedef ptr<const eq_expr> ref;

		static const syntax_kind_t SK = sk_eq_expr;
		static ptr<ast::expression> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression> lhs, rhs;
		bool not_in = false;
	};

	struct ineq_expr : public binary_expr {
		typedef ptr<const ineq_expr> ref;

		static const syntax_kind_t SK = sk_ineq_expr;
		static ptr<ast::expression> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression> lhs, rhs;
	};

	struct plus_expr : public binary_expr {
		typedef ptr<const plus_expr> ref;

		static const syntax_kind_t SK = sk_plus_expr;
		static ptr<ast::expression> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression> lhs, rhs;
	};

	struct times_expr : public binary_expr {
		typedef ptr<const times_expr> ref;

		static const syntax_kind_t SK = sk_times_expr;
		static ptr<ast::expression> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression> lhs, rhs;
	};

	struct prefix_expr : public expression {
		typedef ptr<const prefix_expr> ref;

		static const syntax_kind_t SK = sk_prefix_expr;
		static ptr<ast::expression> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression> rhs;
	};

	struct reference_expr : public expression, public can_reference_overloads_t {
		typedef ptr<const reference_expr> ref;

		static const syntax_kind_t SK = sk_reference_expr;
		static ptr<expression> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
        virtual bound_var_t::ref resolve_overrides(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, const ptr<const ast::item> &obj, const bound_type_t::refs &args) const;
		virtual void render(render_state_t &rs) const;
	};

	struct literal_expr : public expression {
		typedef ptr<const literal_expr> ref;

		static const syntax_kind_t SK = sk_literal_expr;
		static ptr<expression> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;
	};

	struct array_literal_expr : public expression {
		typedef ptr<const array_literal_expr> ref;

		static const syntax_kind_t SK = sk_array_literal_expr;
		static ptr<expression> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<expression>> items;
	};

	struct array_index_expr : public expression {
		typedef ptr<const array_index_expr> ref;

		static const syntax_kind_t SK = sk_array_index_expr;
		static ptr<expression> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(status_t &status, llvm::IRBuilder<> &builder, scope_t::ref scope, local_scope_t::ref *new_scope, bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression> lhs;
		ptr<expression> index;
	};

	struct base_expr : public expression {
		typedef ptr<const base_expr> ref;

		virtual ~base_expr() {}
		static ptr<expression> parse(parse_state_t &ps);
	};
}
