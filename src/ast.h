#pragma once
#include "zion.h"
#include "ast_decls.h"
#include "token.h"
#include <vector>
#include <set>
#include <string>
#include "dbg.h"
#include <memory>
#include "scopes.h"
#include "match.h"
#include "type_checker.h"
#include "callable.h"
#include "life.h"

struct parse_state_t;
struct delegate_t;

namespace ast {
	struct render_state_t;

	struct parsed_type_t {
		parsed_type_t() {}
		parsed_type_t(types::type_t::ref type) : type(type) {}
		types::type_t::ref get_type(delegate_t &delegate, scope_t::ref scope) const;
		types::type_t::ref get_type(llvm::IRBuilder<> &builder, scope_t::ref scope) const;
		std::string str() const;
		location_t get_location() const;
		bool exists() const;

	private:
		types::type_t::ref type;
	};

	struct item_t : std::enable_shared_from_this<item_t> {
		typedef std::shared_ptr<const item_t> ref;

		virtual ~item_t() throw() = 0;
		std::string str() const;
		virtual void render(render_state_t &rs) const = 0;
		location_t get_location() const { return token.location; }

		token_t token;
	};

	void log_named_item_create(const char *type, const std::string &name);

	template <typename T>
	std::shared_ptr<T> create(const token_t &token) {
		auto item = std::make_shared<T>();
		item->token = token;
		return item;
	}

	template <typename T, typename... Args>
	std::shared_ptr<T> create(const token_t &token, Args... args) {
		auto item = std::make_shared<T>(args...);
		item->token = token;
		return item;
	}

	struct statement_t : public virtual item_t {
		typedef std::shared_ptr<const statement_t> ref;

		virtual ~statement_t() {}
		static std::shared_ptr<ast::statement_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const = 0;
	};

	struct module_t;
	struct expression_t;
	struct var_decl_t;
	struct tuple_expr_t;

	struct param_list_decl_t : public item_t {
		typedef std::shared_ptr<const param_list_decl_t> ref;

		static std::shared_ptr<param_list_decl_t> parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		std::vector<std::shared_ptr<var_decl_t>> params;
	};

	struct condition_t : public virtual item_t {
		typedef std::shared_ptr<const condition_t> ref;
		virtual ~condition_t() {}
		virtual var_t::ref resolve_condition(
				delegate_t &delegate,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const = 0;
	};

	struct predicate_t : public virtual item_t {
		typedef std::shared_ptr<const predicate_t> ref;
		typedef std::vector<ref> refs;
		virtual ~predicate_t() {}

		virtual std::string repr() const = 0;
		static ref parse(parse_state_t &ps, bool allow_else, token_t *name_assignment);
		virtual match::Pattern::ref get_pattern(types::type_t::ref type, env_t::ref env) const = 0;
		virtual bool resolve_match(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref scope,
				life_t::ref life,
				location_t value_location,
				bound_var_t::ref input_value,
				llvm::BasicBlock *llvm_match_block,
				llvm::BasicBlock *llvm_no_match_block,
				runnable_scope_t::ref *scope_if_true) const = 0;
	};

	struct tuple_predicate_t : public predicate_t {
		typedef std::shared_ptr<const tuple_predicate_t> ref;

		static ast::predicate_t::ref parse(parse_state_t &ps, token_t *name_assignment);
		virtual bool resolve_match(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref scope,
				life_t::ref life,
				location_t value_location,
				bound_var_t::ref input_value,
				llvm::BasicBlock *llvm_match_block,
				llvm::BasicBlock *llvm_no_match_block,
				runnable_scope_t::ref *scope_if_true) const;
		virtual std::string repr() const;
		virtual void render(render_state_t &rs) const;
		virtual match::Pattern::ref get_pattern(types::type_t::ref type, env_t::ref env) const;

		std::vector<predicate_t::ref> params;
		token_t name_assignment;
	};

	struct irrefutable_predicate_t : public predicate_t {
		typedef std::shared_ptr<const irrefutable_predicate_t> ref;

		virtual bool resolve_match(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref scope,
				life_t::ref life,
				location_t value_location,
				bound_var_t::ref input_value,
				llvm::BasicBlock *llvm_match_block,
				llvm::BasicBlock *llvm_no_match_block,
				runnable_scope_t::ref *scope_if_true) const;
		virtual std::string repr() const;
		virtual void render(render_state_t &rs) const;
		virtual match::Pattern::ref get_pattern(types::type_t::ref type, env_t::ref env) const;
	};

	struct ctor_predicate_t : public predicate_t {
		typedef std::shared_ptr<const ctor_predicate_t> ref;
		typedef std::vector<ref> refs;

		static ast::predicate_t::ref parse(parse_state_t &ps, token_t *name_assignment);
		virtual bool resolve_match(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref scope,
				life_t::ref life,
				location_t value_location,
				bound_var_t::ref input_value,
				llvm::BasicBlock *llvm_match_block,
				llvm::BasicBlock *llvm_no_match_block,
				runnable_scope_t::ref *scope_if_true) const;
		virtual std::string repr() const;
		virtual void render(render_state_t &rs) const;
		virtual match::Pattern::ref get_pattern(types::type_t::ref type, env_t::ref env) const;

		std::vector<predicate_t::ref> params;
		token_t name_assignment;
	};

	struct expression_t : public statement_t, public condition_t {
		typedef std::shared_ptr<const expression_t> ref;

		virtual ~expression_t() {}
		static std::shared_ptr<expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const = 0;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const = 0;

		/* when resolve_condition is not overriden, it just proxies through to resolve_expression */
		virtual var_t::ref resolve_condition(
				delegate_t &delegate,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
	};

	namespace postfix_expr {
		std::shared_ptr<expression_t> parse(parse_state_t &ps);
	}

	struct continue_flow_t : public statement_t {
		typedef std::shared_ptr<const continue_flow_t> ref;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
	};

	struct break_flow_t : public statement_t {
		typedef std::shared_ptr<const break_flow_t> ref;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
	};

	struct typeid_expr_t : public expression_t {
		typedef std::shared_ptr<const typeid_expr_t> ref;

		typeid_expr_t(std::shared_ptr<expression_t> expr);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
		static std::shared_ptr<typeid_expr_t> parse(parse_state_t &ps);

		std::shared_ptr<expression_t> expr;
	};

	struct sizeof_expr_t : public expression_t {
		typedef std::shared_ptr<const typeid_expr_t> ref;

		sizeof_expr_t(types::type_t::ref type);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
		static std::shared_ptr<sizeof_expr_t> parse(parse_state_t &ps);

		parsed_type_t parsed_type;
	};

	struct callsite_expr_t : public expression_t {
		typedef std::shared_ptr<const callsite_expr_t> ref;

		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<expression_t> function_expr;
		std::vector<std::shared_ptr<expression_t>> params;
	};

	struct return_statement_t : public statement_t {
		typedef std::shared_ptr<const return_statement_t> ref;

		static std::shared_ptr<return_statement_t> parse(parse_state_t &ps);
		std::shared_ptr<expression_t> expr;
		virtual void render(render_state_t &rs) const;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
	};

	struct unreachable_t : public statement_t {
		typedef std::shared_ptr<const unreachable_t> ref;

		static std::shared_ptr<unreachable_t> parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
	};

	struct type_decl_t : public item_t {
		typedef std::shared_ptr<const type_decl_t> ref;

		type_decl_t(identifier::refs type_variables);

		static ref parse(parse_state_t &ps, token_t name_token);
		virtual void render(render_state_t &rs) const;

		identifier::refs type_variables;
	};

	struct cast_expr_t : public expression_t {
		typedef std::shared_ptr<const cast_expr_t> ref;

		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		std::shared_ptr<expression_t> lhs;
		parsed_type_t parsed_type_cast;
		bool force_cast = false;
	};

	struct dimension_t : public item_t {
		typedef std::shared_ptr<const dimension_t> ref;
		dimension_t(std::string name, types::type_t::ref type);
		virtual ~dimension_t() throw() {}
		virtual void render(render_state_t &rs) const;

		static ref parse(parse_state_t &ps, identifier::set generics);

		std::string name;
		parsed_type_t parsed_type;
	};

	struct type_algebra_t : public item_t {
		typedef std::shared_ptr<const type_algebra_t> ref;

		virtual ~type_algebra_t() throw() {}

		/* register_type is called from within the scope where the type's
		 * ctors should end up living. this function should create the
		 * unchecked ctors with the type. */
		virtual void register_type(
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const = 0;

		static ref parse(parse_state_t &ps, ast::type_decl_t::ref type_decl);
	};

	struct data_type_t : public type_algebra_t {
		typedef std::shared_ptr<const data_type_t> ref;

		virtual ~data_type_t() throw() {}
		static ref parse(parse_state_t &ps, type_decl_t::ref type_decl, identifier::refs type_variables);
		virtual void register_type(
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;

		std::vector<std::pair<token_t, types::type_args_t::ref>> ctor_pairs;
	};

	struct type_product_t : public type_algebra_t {
		typedef std::shared_ptr<const type_product_t> ref;

		type_product_t(bool native, types::type_t::ref type, identifier::set type_variables);
		virtual ~type_product_t() throw() {}
		static ref parse(parse_state_t &ps, type_decl_t::ref type_decl, identifier::refs type_variables, bool native);
		virtual void register_type(
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;

		bool native;
		parsed_type_t parsed_type;
	};

	struct type_alias_t : public type_algebra_t {
		typedef std::shared_ptr<const type_alias_t> ref;

		virtual ~type_alias_t() throw() {}
		static ref parse(parse_state_t &ps, type_decl_t::ref type_decl, identifier::refs type_variables);
		virtual void register_type(
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;

		parsed_type_t parsed_type;
		identifier::set type_variables;
	};

	struct type_link_t : public type_algebra_t {
		typedef std::shared_ptr<const type_link_t> ref;

		virtual ~type_link_t() throw() {}
		static ref parse(parse_state_t &ps, type_decl_t::ref type_decl, identifier::refs type_variables);
		virtual void register_type(
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;
	};

	struct type_def_t : public statement_t {
		typedef std::shared_ptr<const type_def_t> ref;

		static std::shared_ptr<type_def_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		type_decl_t::ref type_decl;
		type_algebra_t::ref type_algebra;
	};

	struct var_decl_t : public virtual statement_t, public condition_t {
		typedef std::shared_ptr<const var_decl_t> ref;

		static std::shared_ptr<statement_t> parse(parse_state_t &ps, bool is_let, bool allow_tuple_destructuring);
		static std::shared_ptr<var_decl_t> parse_param(parse_state_t &ps);
		bound_var_t::ref resolve_as_link(
				llvm::IRBuilder<> &builder,
				module_scope_t::ref module_scope);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual var_t::ref resolve_condition(
				delegate_t &delegate,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual void render(render_state_t &rs) const;

		virtual std::string get_symbol() const;
		types::type_t::ref get_type(delegate_t &delegate, scope_t::ref scope) const;
		types::type_t::ref get_type(llvm::IRBuilder<> &builder, scope_t::ref scope) const;
		location_t get_location() const;
		virtual bool is_let() const { return is_let_var; }

		/* the inherited ast::item::token member contains the actual identifier
		 * name */

		/* is_let defines whether the name of this variable can be repointed at some other value */
		bool is_let_var = false;

		/* type describes the type of the value this name refers to */
		parsed_type_t parsed_type;

		/* how should this variable be initialized? */
		std::shared_ptr<expression_t> initializer;

		/* for module variables, extends_module describes the module that this variable should be injected into */
		identifier::ref extends_module;
	};

	struct destructured_tuple_decl_t : public statement_t {
		static std::shared_ptr<statement_t> parse(parse_state_t &ps, std::shared_ptr<tuple_expr_t> lhs);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		bool is_let = true;
		parsed_type_t parsed_type;
		std::shared_ptr<tuple_expr_t> lhs;
		std::shared_ptr<expression_t> initializer;
	};

	struct defer_t : public statement_t {
		typedef std::shared_ptr<const defer_t> ref;

		static std::shared_ptr<statement_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<expression_t> callable;
	};

	struct assignment_t : public statement_t {
		typedef std::shared_ptr<const assignment_t> ref;

		static std::shared_ptr<statement_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<expression_t> lhs, rhs;
	};

	struct plus_assignment_t : public statement_t {
		typedef std::shared_ptr<const plus_assignment_t> ref;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<expression_t> lhs, rhs;
	};

	struct times_assignment_t : public statement_t {
		typedef std::shared_ptr<const times_assignment_t> ref;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<expression_t> lhs, rhs;
	};

	struct divide_assignment_t : public statement_t {
		typedef std::shared_ptr<const divide_assignment_t> ref;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<expression_t> lhs, rhs;
	};

	struct minus_assignment_t : public statement_t {
		typedef std::shared_ptr<const minus_assignment_t> ref;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<expression_t> lhs, rhs;
	};

	struct mod_assignment_t : public statement_t {
		typedef std::shared_ptr<const mod_assignment_t> ref;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<expression_t> lhs, rhs;
	};

	struct block_t : public expression_t {
		typedef std::shared_ptr<const block_t> ref;

		static std::shared_ptr<block_t> parse(parse_state_t &ps, bool expression_means_return=false);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		bound_var_t::ref resolve_block_expr(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				bool *returns,
				types::type_t::ref expected_type) const;
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		std::vector<std::shared_ptr<statement_t>> statements;
	};

	struct function_decl_t : public item_t {
		typedef std::shared_ptr<const function_decl_t> ref;

		static std::shared_ptr<function_decl_t> parse(parse_state_t &ps, bool within_expression, types::type_t::ref default_return_type);

		virtual void render(render_state_t &rs) const;

		types::type_t::ref function_type;
		identifier::ref extends_module;
		token_t link_to_name;
		bool no_closure = false;

		std::string get_function_name() const;
	};

	struct function_defn_t : public expression_t, public can_reference_overloads_t {
		typedef std::shared_ptr<const function_defn_t> ref;

		static std::shared_ptr<function_defn_t> parse(parse_state_t &ps, bool within_expression);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual bound_var_t::ref resolve_function(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_closure,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual var_t::ref resolve_overrides(
				delegate_t &delegate,
				scope_t::ref scope,
				life_t::ref,
				const std::shared_ptr<const ast::item_t> &obj,
				const bound_type_t::refs &args,
				types::type_t::ref return_type,
				bool *returns) const;
		virtual types::type_function_t::ref resolve_arg_types_from_overrides(
				scope_t::ref scope,
				location_t location,
				types::type_t::refs args,
				types::type_t::ref return_type) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<function_decl_t> decl;
		std::shared_ptr<block_t> block;
	};

	struct if_block_t : public statement_t {
		typedef std::shared_ptr<const if_block_t> ref;

		static std::shared_ptr<if_block_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<const condition_t> condition;
		std::shared_ptr<const block_t> block;
		std::shared_ptr<const statement_t> else_;
	};

	struct while_block_t : public statement_t {
		typedef std::shared_ptr<const while_block_t> ref;

		static std::shared_ptr<while_block_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<const condition_t> condition;
		std::shared_ptr<block_t> block;
	};

	struct pattern_block_t : public item_t {
		typedef std::shared_ptr<const pattern_block_t> ref;
		typedef std::vector<ref> refs;

		static ref parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;
		
		predicate_t::ref predicate;
		std::shared_ptr<block_t> block;
	};

	struct match_expr_t : public expression_t {
		typedef std::shared_ptr<const match_expr_t> ref;

		static std::shared_ptr<match_expr_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		bound_var_t::ref resolve_match_expr(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				bool *returns,
				types::type_t::ref expected_type) const;
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<expression_t> value;
		pattern_block_t::refs pattern_blocks;
	};

	struct module_decl_t : public item_t {
		typedef std::shared_ptr<const module_decl_t> ref;

		static std::shared_ptr<module_decl_t> parse(parse_state_t &ps, bool skip_module_token=false);
		virtual void render(render_state_t &rs) const;

		std::string get_canonical_name() const;
		token_t get_name() const;

		token_t name;
		bool global = false;
	};

	struct link_module_statement_t : public statement_t {
		typedef std::shared_ptr<const link_module_statement_t> ref;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		token_t link_as_name;
		std::shared_ptr<module_decl_t> extern_module;
		identifier::refs symbols;
	};

	struct link_name_t : public statement_t {
		typedef std::shared_ptr<const link_name_t> ref;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		token_t local_name;
		std::shared_ptr<module_decl_t> extern_module;
		token_t remote_name;
	};

	struct link_function_statement_t : public std::enable_shared_from_this<link_function_statement_t>, public expression_t {
		typedef std::shared_ptr<const link_function_statement_t> ref;

		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<function_decl_t> extern_function;
	};

	struct link_var_statement_t : public expression_t {
		typedef std::shared_ptr<const link_var_statement_t> ref;

		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<var_decl_t> var_decl;
	};

	struct module_t : public std::enable_shared_from_this<module_t>, public item_t {
		typedef std::shared_ptr<const module_t> ref;

		module_t(const std::string filename, bool global=false);
		static std::shared_ptr<module_t> parse(parse_state_t &ps);
		std::string get_canonical_name() const;
		virtual void render(render_state_t &rs) const;

		bool global;
		std::string filename;
		std::string module_key;

		std::shared_ptr<module_decl_t> decl;
		std::vector<std::shared_ptr<var_decl_t>> var_decls;
		std::vector<std::shared_ptr<type_def_t>> type_defs;
		std::vector<std::shared_ptr<function_defn_t>> functions;
		std::vector<std::shared_ptr<link_module_statement_t>> linked_modules;
		std::vector<std::shared_ptr<link_function_statement_t>> linked_functions;
		std::vector<std::shared_ptr<link_var_statement_t>> linked_vars;
	};

	struct program_t : public item_t {
		typedef std::shared_ptr<const program_t> ref;

		virtual ~program_t() {}
		virtual void render(render_state_t &rs) const;

		std::vector<std::shared_ptr<const module_t>> modules;
	};

	struct dot_expr_t : public expression_t, public can_reference_overloads_t {
		typedef std::shared_ptr<const dot_expr_t> ref;

		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual var_t::ref resolve_overrides(
				delegate_t &delegate,
				scope_t::ref scope,
				life_t::ref,
				const std::shared_ptr<const ast::item_t> &obj,
				const bound_type_t::refs &args,
				types::type_t::ref return_type,
				bool *returns) const;
		virtual types::type_function_t::ref resolve_arg_types_from_overrides(
				scope_t::ref scope,
				location_t location,
				types::type_t::refs args,
				types::type_t::ref return_type) const;
		var_t::ref resolve_with_lhs(
				delegate_t &delegate,
				scope_t::ref scope,
				life_t::ref life,
				var_t::ref lhs_val,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<ast::expression_t> lhs;
		token_t rhs;
	};

	struct tuple_expr_t : public expression_t {
		typedef std::shared_ptr<const tuple_expr_t> ref;

		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		static std::shared_ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::vector<std::shared_ptr<ast::expression_t>> values;
	};

	struct ternary_expr_t : public expression_t {
		typedef std::shared_ptr<const ternary_expr_t> ref;

		static std::shared_ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual var_t::ref resolve_condition(
				delegate_t &delegate,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<ast::expression_t> condition, when_true, when_false;
	};

	struct or_expr_t : public expression_t {
		typedef std::shared_ptr<const or_expr_t> ref;

		static std::shared_ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual var_t::ref resolve_condition(
				delegate_t &delegate,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<ast::expression_t> lhs, rhs;
	};

	struct and_expr_t : public expression_t {
		typedef std::shared_ptr<const and_expr_t> ref;

		static std::shared_ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual var_t::ref resolve_condition(
				delegate_t &delegate,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<ast::expression_t> lhs, rhs;
	};

	struct binary_operator_t : public expression_t {
		typedef std::shared_ptr<const binary_operator_t> ref;

		static std::shared_ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual var_t::ref resolve_condition(
				delegate_t &delegate,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual void render(render_state_t &rs) const;

		std::string function_name;
		std::shared_ptr<ast::expression_t> lhs, rhs;
	};

	struct prefix_expr_t : public expression_t {
		typedef std::shared_ptr<const prefix_expr_t> ref;

		static std::shared_ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual var_t::ref resolve_condition(
				delegate_t &delegate,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<ast::expression_t> rhs;

	private:
		virtual var_t::ref resolve_prefix_expr(
				delegate_t &delegate,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
	};

	struct reference_expr_t : public expression_t, public can_reference_overloads_t {
		typedef std::shared_ptr<const reference_expr_t> ref;

		static std::shared_ptr<expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual var_t::ref resolve_overrides(
				delegate_t &delegate,
				scope_t::ref scope,
				life_t::ref,
				const std::shared_ptr<const ast::item_t> &obj,
				const bound_type_t::refs &args,
				types::type_t::ref return_type,
				bool *returns) const;
		virtual var_t::ref resolve_condition(
				delegate_t &delegate,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		var_t::ref resolve_reference(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual types::type_function_t::ref resolve_arg_types_from_overrides(
				scope_t::ref scope,
				location_t location,
				types::type_t::refs args,
				types::type_t::ref return_type) const;
		virtual void render(render_state_t &rs) const;
	};

	struct literal_expr_t : public expression_t, public predicate_t {
		typedef std::shared_ptr<const literal_expr_t> ref;

		static std::shared_ptr<expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual bool resolve_match(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref scope,
				life_t::ref life,
				location_t value_location,
				bound_var_t::ref input_value,
				llvm::BasicBlock *llvm_match_block,
				llvm::BasicBlock *llvm_no_match_block,
				runnable_scope_t::ref *scope_if_true) const;
		virtual std::string repr() const;
		virtual void render(render_state_t &rs) const;

		virtual match::Pattern::ref get_pattern(types::type_t::ref type, env_t::ref env) const;
	};

	struct array_literal_expr_t : public expression_t {
		typedef std::shared_ptr<const array_literal_expr_t> ref;

		static std::shared_ptr<expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::vector<std::shared_ptr<expression_t>> items;
	};

    struct bang_expr_t : public expression_t {
		typedef std::shared_ptr<const bang_expr_t> ref;

		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<expression_t> lhs;
    };

	struct array_index_expr_t : public expression_t {
		typedef std::shared_ptr<const array_index_expr_t> ref;

		static std::shared_ptr<expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual var_t::ref resolve_expression(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				bool *returns) const;
		var_t::ref resolve_assignment(
				delegate_t &delegate,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				const ast::expression_t::ref &rhs,
				types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		std::shared_ptr<expression_t> lhs;
		std::shared_ptr<expression_t> start, stop;
		bool is_slice = false;
	};

	namespace base_expr {
		std::shared_ptr<expression_t> parse(parse_state_t &ps);
	}
}
