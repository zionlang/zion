#include <stdlib.h>
#include <string>
#include <iostream>
#include "logger.h"
#include "ast.h"
#include "token.h"
#include "logger_decls.h"
#include "compiler.h"
#include <csignal>
#include "parse_state.h"
#include "parser.h"
#include "type_parser.h"

using namespace bitter;

expr_t *parse_expr(parse_state_t &ps);
expr_t *parse_tuple_expr(parse_state_t &ps);
expr_t *parse_block(parse_state_t &ps, bool expression_means_return);

bool token_begins_type(const token_t &token) {
	switch (token.tk) {
	case tk_integer:
	case tk_string:
	case tk_times:
	case tk_lsquare:
	case tk_lparen:
	case tk_identifier:
		return true;
	default:
		return false;
	};
}

struct scoping_t {
	scoping_t(parse_state_t &ps) : ps(ps) {
		expect_token(tk_identifier);
		token = ps.token;
		for (auto token : ps.scopes) {
			if (token.text == token.text) {
				auto error = user_error(token.location, "duplicate name " c_id("%s") " found", token.text.c_str());
				error.add_info(token.location, "see prior declaration here");
				throw error;
			}
		}

		ps.scopes.push_back(token);
		ps.advance();
	}

	~scoping_t() {
		assert(ps.scopes.back().text == token.text);
		ps.scopes.pop_back();
	}
	parse_state_t &ps;
	token_t token;
};

std::vector<std::pair<int, identifier_t>> extract_ids(const std::vector<expr_t*> &dims) {
	std::vector<std::pair<int, identifier_t>> refs;
	int i = 0;
	for (auto dim: dims) {
		if (var_t *var = dcast<var_t*>(dim)) {
			if (var->id.name != "_") {
				refs.push_back({i, var->id});
			}
		} else {
			throw user_error(dim->get_location(), "only reference expressions are allowed here");
		}
		++i;
	}
	assert(refs.size() != 0);
	return refs;
}

expr_t *parse_var_decl(parse_state_t &ps, bool is_let, bool allow_tuple_destructuring) {
	if (ps.token.tk == tk_lparen) {
		if (!allow_tuple_destructuring) {
			throw user_error(ps.token.location, "tuple destructuring is not allowed here");
		}

		auto prior_token = ps.token;
		auto tuple = dcast<tuple_t*>(parse_tuple_expr(ps));
		if (tuple == nullptr) {
			throw user_error(prior_token.location, "tuple destructuring detected an invalid expression on the lhs");
		}

		if (ps.token.tk == tk_assign) {
			eat_token();
			auto rhs = parse_expr(ps);
			auto rhs_var = new var_t({fresh(), rhs->get_location()});

			std::vector<std::pair<int, identifier_t>> refs = extract_ids(tuple->dims);
			if (refs.size() == 0) {
				throw user_error(ps.token.location, "nothing to destructure");
			}


			expr_t *body = parse_block(ps, false/*expression_means_return*/);
			for (int i=refs.size()-1; i >= 0; --i) {
				body = new let_t(
						refs[i].second,
						new application_t(
							new var_t(make_iid(string_format("__[%d]__", i))),
							rhs_var),
						body);
			}

			return new let_t(rhs_var->id, rhs, body);
		} else {
			throw user_error(ps.token.location, "destructured tuples must be assigned to an rhs");
		}
	} else {
		expect_token(tk_identifier);

		auto var_id = make_code_id(ps.token);

		eat_token();
		expr_t *initializer = nullptr;

		if (ps.token.tk == tk_assign) {
			eat_token();
			initializer = parse_expr(ps);
		} else {
			initializer = new application_t(
					new var_t(make_iid("__init__")),
				   	new var_t(make_iid("unit")));
		}

		if (!is_let) {
			initializer = new application_t(new var_t(make_iid("Ref")), initializer);
		}

		// TODO: consider whether to try making var_id get implicit loads/stores within this scope
		return new let_t(var_id, initializer, parse_block(ps, false /*expression_means_return*/));
	}
}

expr_t *parse_return_statement(parse_state_t &ps) {
	auto return_token = ps.token;
	chomp_ident(K(return));
	return new return_statement_t(
			(!ps.line_broke() && ps.token.tk != tk_rcurly)
			? parse_expr(ps)
			: new var_t(make_iid("unit")));
}

maybe<identifier_t> parse_with_param(parse_state_t &ps, expr_t *&expr) {
	expr = parse_expr(ps);
	if (auto var = dcast<var_t *>(expr)) {
		if (ps.token.tk == tk_becomes) {
			auto param_id = var->id;
			ps.advance();
			expr = parse_expr(ps);
			return maybe<identifier_t>(param_id);
		}
	}
	return maybe<identifier_t>();
}

expr_t *parse_with_block(parse_state_t &ps) {
	auto with_token = ps.token;
	ps.advance();

	expr_t *expr = nullptr;
	maybe<identifier_t> maybe_param_id = parse_with_param(ps, expr);
	assert(expr != nullptr);

	identifier_t param_id = (maybe_param_id.valid
	   	? maybe_param_id.t
	   	: identifier_t{fresh(), with_token.location});

	auto block = parse_block(ps, false /*expression_means_return*/);

	auto else_token = ps.token;
	chomp_ident(K(else));

	identifier_t error_var_id = (ps.token.tk == tk_identifier)
		? identifier_t::from_token(ps.token_and_advance())
		: identifier_t{fresh(), with_token.location};

	auto error_block = parse_block(ps, false /* expression_means_return */);

	auto cleanup_token = identifier_t{"__cleanup", with_token.location};
	auto match = create<match_expr_t>(with_token);
	match->value = expr;

	auto with_pattern = create<pattern_block_t>(with_token);
	with_pattern->block = block;

	auto with_predicate = create<ctor_predicate_t>(token_t{with_token.location, tk_identifier, "Acquired"});
	with_predicate->params.push_back(create<irrefutable_predicate_t>(param_id));
	with_predicate->params.push_back(create<irrefutable_predicate_t>(cleanup_token));
	with_pattern->predicate = with_predicate;

	auto cleanup_defer = create<defer_t>(block->token);
	cleanup_defer->callable = create<reference_expr_t>(cleanup_token);
	block->statements.insert(block->statements.begin(), cleanup_defer);

	auto else_pattern = create<pattern_block_t>(else_token);
	else_pattern->block = error_block;

	auto else_predicate = create<ctor_predicate_t>(token_t{else_token.location, tk_identifier, "Failed"});
	else_predicate->params.push_back(create<irrefutable_predicate_t>(error_var_id));
	else_pattern->predicate = else_predicate;

	match->pattern_blocks.push_back(with_pattern);
	match->pattern_blocks.push_back(else_pattern);
	return match;
}

expr_t *wrap_with_iter(expr_t *expr) {
	auto iter_token = token_t{expr->get_location(), tk_identifier, "iter"};
	auto iter_ref = create<reference_expr_t>(iter_token);
	auto iter_callsite = create<callsite_expr_t>(iter_token);
	iter_callsite->function_expr = iter_ref;
	iter_callsite->params.push_back(expr);
	return iter_callsite;
}

expr_t *parse_for_block(parse_state_t &ps) {
	auto for_token = ps.token;
	ps.advance();

	token_t param_id;
	std::shared_ptr<tuple_expr_t> tuple_expr;

	if (ps.token.tk == tk_lparen) {
		tuple_expr = dyncast<tuple_expr_t>(tuple_expr_t::parse(ps));
		if (tuple_expr == nullptr) {
			throw user_error(ps.token.location, "expected a tuple of variable names");
		}
	} else {
		expect_token(tk_identifier);
		param_id = ps.token;
		ps.advance();
	}

	token_t becomes_token;

	chomp_ident(K(in));

	auto expr = expression_t::parse(ps);
	auto block = block_t::parse(ps, false /*expression_means_return*/);

	/* create the iterator function by evaluating the `iterable` (for _ in `iterable` { ... }) */
	auto iter_func_decl = create<var_decl_t>(token_t{expr->get_location(), tk_identifier, types::gensym(INTERNAL_LOC())->get_name()});
	iter_func_decl->is_let_var = true;
	iter_func_decl->parsed_type = parsed_type_t(type_variable(expr->get_location()));
	iter_func_decl->initializer = wrap_with_iter(expr);

	/* call the iterator value (which is a function returned by the expression */
	auto iter_token = token_t{expr->get_location(), tk_identifier, iter_func_decl->token.text};
	auto iter_ref = create<reference_expr_t>(iter_token);
	auto iter_callsite = create<callsite_expr_t>(iter_token);
	iter_callsite->function_expr = iter_ref;

	auto just_pattern = create<pattern_block_t>(for_token);
	just_pattern->block = block;

	token_t just_value_token = token_t{for_token.location, tk_identifier, types::gensym(INTERNAL_LOC())->get_name()};
	if (tuple_expr != nullptr) {
		auto destructured_tuple_decl = ast::create<ast::destructured_tuple_decl_t>(tuple_expr->token);
		destructured_tuple_decl->is_let = false;
		destructured_tuple_decl->lhs = tuple_expr;
		assert(destructured_tuple_decl->lhs != nullptr);
		destructured_tuple_decl->parsed_type = parsed_type_t(type_variable(tuple_expr->token.location));
		destructured_tuple_decl->initializer = create<reference_expr_t>(just_value_token);

		just_pattern->block->statements.insert(just_pattern->block->statements.begin(), destructured_tuple_decl);
	} else {
		auto just_var_decl = create<var_decl_t>(param_id);
		just_var_decl->is_let_var = false;
		just_var_decl->parsed_type = parsed_type_t(type_variable(param_id.location));
		just_var_decl->initializer = create<reference_expr_t>(just_value_token);

		just_pattern->block->statements.insert(just_pattern->block->statements.begin(), just_var_decl);
	}

	auto just_predicate = create<ctor_predicate_t>(token_t{for_token.location, tk_identifier, "Just"});
	just_predicate->params.push_back(create<irrefutable_predicate_t>(just_value_token));
	just_pattern->predicate = just_predicate;

	auto break_block = create<block_t>(for_token);
	break_block->statements.push_back(create<break_flow_t>(for_token));

	auto nothing_pattern = create<pattern_block_t>(for_token);
	nothing_pattern->block = break_block;

	auto nothing_predicate = create<ctor_predicate_t>(token_t{for_token.location, tk_identifier, "Nothing"});
	nothing_pattern->predicate = nothing_predicate;

	auto match = create<match_expr_t>(for_token);
	match->value = iter_callsite;
	match->pattern_blocks.push_back(just_pattern);
	match->pattern_blocks.push_back(nothing_pattern);

	auto while_block = create<block_t>(block->token);
	while_block->statements.push_back(match);

	auto while_loop = create<while_block_t>(for_token);
	while_loop->block = while_block;
	while_loop->condition = create<reference_expr_t>(token_t{becomes_token.location, tk_identifier, "true"});

	std::shared_ptr<block_t> outer_block = create<block_t>(for_token);
	outer_block->statements.push_back(iter_func_decl);
	outer_block->statements.push_back(while_loop);
	// log_location(log_info, outer_block->get_location(), "created %s", outer_block->str().c_str());
	return outer_block;
}

expr_t *defer_t::parse(parse_state_t &ps) {
	auto defer = create<defer_t>(ps.token);
	ps.advance();
	defer->callable = expression_t::parse(ps);
	return defer;
}

expr_t *parse_new_expr(parse_state_t &ps) {
	auto callsite = create<callsite_expr_t>(ps.token);
	callsite->function_expr = create<reference_expr_t>({ps.token.location, tk_identifier, "__init__"});

	ps.advance();

	try {
		auto cast_expr = create<cast_expr_t>(ps.token);
		cast_expr->lhs = callsite;
		cast_expr->parsed_type_cast = parsed_type_t(types::parse_type(ps, {} /*generics*/));
		return cast_expr;
	} catch (user_error &e) {
		std::throw_with_nested(user_error(callsite->token.location, "while parsing unary operator new"));
	}
}

expr_t *statement_t::parse(parse_state_t &ps) {
	assert(ps.token.tk != tk_rcurly);

	if (ps.token.is_ident(K(var))) {
		ps.advance();
		return var_decl_t::parse(ps, false /*is_let*/, true /*allow_tuple_destructuring*/);
	} else if (ps.token.is_ident(K(let))) {
		ps.advance();
		return var_decl_t::parse(ps, true /*is_let*/, true /*allow_tuple_destructuring*/);
	} else if (ps.token.is_ident(K(if))) {
		return if_block_t::parse(ps);
	} else if (ps.token.is_ident(K(while))) {
		return while_block_t::parse(ps);
	} else if (ps.token.is_ident(K(for))) {
		return parse_for_block(ps);
	} else if (ps.token.is_ident(K(match))) {
		return match_expr_t::parse(ps);
	} else if (ps.token.is_ident(K(with))) {
		return parse_with_block(ps);
	} else if (ps.token.is_ident(K(new))) {
		return parse_new_expr(ps);
	} else if (ps.token.is_ident(K(fn))) {
		return function_defn_t::parse(ps, false /*within_expression*/);
	} else if (ps.token.is_ident(K(return))) {
		return return_statement_t::parse(ps);
	} else if (ps.token.is_ident(K(unreachable))) {
		return new var_t(make_code_id(ps.token));
	} else if (ps.token.is_ident(K(type))) {
		return type_def_t::parse(ps);
	} else if (ps.token.is_ident(K(defer))) {
		return defer_t::parse(ps);
	} else if (ps.token.is_ident(K(continue))) {
		auto continue_flow = create<ast::continue_flow_t>(ps.token);
		eat_token();
		return std::move(continue_flow);
	} else if (ps.token.is_ident(K(break))) {
		auto break_flow = create<ast::break_flow_t>(ps.token);
		eat_token();
		return std::move(break_flow);
	} else {
		return assignment_t::parse(ps);
	}
}

expr_t *reference_expr_t::parse(parse_state_t &ps) {
	if (ps.token.tk == tk_identifier) {
		if (ps.token.text == "null") {
			auto token = ps.token;
			ps.advance();
			return create<ast::literal_expr_t>(token);
		} else if (ps.token.text == "__filename__") {
			auto expr = create<literal_expr_t>(
					token_t{ps.token.location, tk_string, escape_json_quotes(ps.token.location.filename)});
			ps.advance();
			return expr;
		}

		auto reference_expr = create<ast::reference_expr_t>(ps.token);
		ps.advance();
		return std::move(reference_expr);
	} else {
		throw user_error(ps.token.location, "expected an identifier");
	}
}

std::shared_ptr<typeid_expr_t> typeid_expr_t::parse(parse_state_t &ps) {
	auto token = ps.token;
	chomp_ident(K(typeid));
	chomp_token(tk_lparen);

	auto value = expression_t::parse(ps);
	assert(value != nullptr);
	auto expr = ast::create<typeid_expr_t>(token, value);
	chomp_token(tk_rparen);
	return expr;
}

std::shared_ptr<sizeof_expr_t> sizeof_expr_t::parse(parse_state_t &ps) {
	auto token = ps.token;
	chomp_ident(K(sizeof));
	chomp_token(tk_lparen);

	auto type = types::parse_type(ps, {});
	assert(type != nullptr);
	auto expr = ast::create<sizeof_expr_t>(token, type);
	chomp_token(tk_rparen);
	return expr;
}

expr_t *parse_cast_wrap(parse_state_t &ps, expr_t *expr) {
    if (ps.token.is_ident(K(as))) {
		auto token = ps.token;
		ps.advance();
		auto cast = ast::create<ast::cast_expr_t>(token);
		cast->lhs = expr;

		if (ps.token.tk == tk_bang) {
			if (token.location.line != ps.token.location.line || token.location.col + int(strlen(K(as))) != ps.token.location.col) {
				throw user_error(ps.token.location,
					   	"unsafe casts must not have any whitespace between \"as\" and \"!\". "
						"They must appear exactly like this: \"as!\". This is to enable "
						"searching over codebases for any misused unsafe casts.");
			}
			ps.advance();
			cast->force_cast = true;
		}
		cast->parsed_type_cast = parsed_type_t(types::parse_type(ps, {}));
        return cast;
    } else {
        return expr;
    }
}

expr_t *base_expr::parse(parse_state_t &ps) {
	if (ps.token.tk == tk_lparen) {
		return parse_tuple_expr(ps);
	} else if (ps.token.is_ident(K(new))) {
		return parse_new_expr(ps);
	} else if (ps.token.is_ident(K(fn))) {
		return parse_function_defn(ps, true /*within_expression*/);
	} else if (ps.token.is_ident(K(match))) {
		return parse_match_expr(ps);
	} else if (ps.token.tk == tk_identifier) {
		// NB: this is last to ensure "special" builtins are in play above
		return reference_expr_t::parse(ps);
	} else {
		return literal_expr_t::parse(ps);
	}
}

expr_t *array_literal_expr_t::parse(parse_state_t &ps) {
	chomp_token(tk_lsquare);
	auto array = create<array_literal_expr_t>(ps.token);
	auto &items = array->items;

	int i = 0;
	while (ps.token.tk != tk_rsquare && ps.token.tk != tk_none) {
		++i;
		items.push_back(expression_t::parse(ps));
		if (ps.token.tk == tk_comma) {
			ps.advance();
		} else if (ps.token.tk != tk_rsquare) {
			throw user_error(ps.token.location, "found something that does not make sense in an array literal");
		}
	}
	chomp_token(tk_rsquare);
	return array;
}

expr_t *literal_expr_t::parse(parse_state_t &ps) {
	switch (ps.token.tk) {
	case tk_integer:
	case tk_string:
	case tk_char:
	case tk_float:
		{
			auto literal_expr = create<ast::literal_expr_t>(ps.token);
			ps.advance();
			return std::move(literal_expr);
		}
	case tk_lsquare:
		return array_literal_expr_t::parse(ps);
	// case tk_lcurly:
	//	return assoc_array_expr_t::parse(ps);

	case tk_identifier:
		throw user_error(ps.token.location, "unexpected token found when parsing literal expression. '" c_error("%s") "'", ps.token.text.c_str());

	default:
		if (ps.token.tk == tk_lcurly) {
			throw user_error(ps.token.location, "this squiggly brace is a surprise");
		} else if (ps.lexer.eof()) {
			auto error = user_error(ps.token.location, "unexpected end-of-file.");
			for (auto pair : ps.lexer.nested_tks) {
				error.add_info(pair.first, "unclosed %s here", tkstr(pair.second));
			}
			throw error;
		} else {
			throw user_error(ps.token.location, "out of place token found when parsing literal expression. '" c_error("%s") "' (%s)",
					ps.token.text.c_str(),
					tkstr(ps.token.tk));
		}
	}
}

std::vector<std::shared_ptr<expression_t>> parse_param_list(parse_state_t &ps) {
	std::vector<std::shared_ptr<expression_t>> params;
	chomp_token(tk_lparen);
	int i = 0;
	while (ps.token.tk != tk_rparen) {
		++i;
		params.push_back(expression_t::parse(ps));
		if (ps.token.tk == tk_comma) {
			eat_token();
		} else if (ps.token.tk != tk_rparen) {
			throw user_error(ps.token.location, "unexpected %s in parameter list (" c_id("%s") ")", tkstr(ps.token.tk), ps.token.text.c_str());
		}
		// continue and read the next parameter
	}
	chomp_token(tk_rparen);

	return params;
}

namespace ast {
	namespace postfix_expr {
		expr_t *parse(parse_state_t &ps) {
			expr_t *expr = base_expr::parse(ps);

			while (!ps.line_broke() &&
					(ps.token.tk == tk_lsquare ||
					 ps.token.tk == tk_lparen ||
					 ps.token.tk == tk_dot ||
					 ps.token.tk == tk_bang))
			{
				switch (ps.token.tk) {
				case tk_lparen:
					{
						/* function call */
						auto callsite = create<callsite_expr_t>(ps.token);
						callsite->params = parse_param_list(ps);
						callsite->function_expr.swap(expr);
						assert(expr == nullptr);
						expr = callsite;
						break;
					}
				case tk_dot:
					{
						auto dot_expr = create<ast::dot_expr_t>(ps.token);
						eat_token();
						expect_token(tk_identifier);
						dot_expr->rhs = ps.token;
						ps.advance();
						dot_expr->lhs.swap(expr);
						assert(expr == nullptr);
						expr = dot_expr;
						break;
					}
				case tk_lsquare:
					{
						eat_token();
						auto array_index_expr = create<ast::array_index_expr_t>(ps.token);

						array_index_expr->lhs = expr;
						array_index_expr->start = expression_t::parse(ps);
						if (ps.token.tk == tk_colon) {
							array_index_expr->is_slice = true;
							ps.advance();
							if (ps.token.tk != tk_rsquare) {
								array_index_expr->stop = expression_t::parse(ps);
							}
						}
						expr = array_index_expr;
						chomp_token(tk_rsquare);
						break;
					}
				case tk_bang:
					{
						auto bang = ast::create<ast::bang_expr_t>(ps.token);
						bang->lhs = expr;
						expr = bang;
						ps.advance();
						break;
					}
				default:
					break;
				}
			}

			return expr;
		}
	}
}

expr_t *prefix_expr_t::parse(parse_state_t &ps) {
	std::shared_ptr<prefix_expr_t> prefix_expr;
	if (ps.token.tk == tk_ampersand
			|| ps.token.tk == tk_minus
			|| ps.token.tk == tk_plus
			|| ps.token.is_ident(K(not)))
	{
		prefix_expr = create<ast::prefix_expr_t>(ps.token);
		eat_token();
	}

	expr_t *rhs;
	if (ps.token.is_ident(K(not))
			|| ps.token.tk == tk_minus
			|| ps.token.tk == tk_plus) {
		/* recurse to find more prefix expressions */
		rhs = prefix_expr_t::parse(ps);
	} else {
		/* ok, we're done with prefix operators */
		rhs = postfix_expr::parse(ps);
	}

	if (prefix_expr) {
		prefix_expr->rhs = std::move(rhs);
		return parse_cast_wrap(ps, prefix_expr);
	} else {
		return parse_cast_wrap(ps, rhs);
	}
}


expr_t *times_expr_parse(parse_state_t &ps) {
	auto expr = prefix_expr_t::parse(ps);

	while (!ps.line_broke() && (ps.token.tk == tk_times
				|| ps.token.tk == tk_divide_by
				|| ps.token.tk == tk_mod)) {
		auto binary_operator = create<ast::binary_operator_t>(ps.token);
		switch (ps.token.tk) {
		case tk_times:
			binary_operator->function_name = "__times__";
			break;
		case tk_divide_by:
			binary_operator->function_name = "__divide__";
			break;
		case tk_mod:
			binary_operator->function_name = "__mod__";
			break;
		default:
			assert(false);
			break;
		}

		eat_token();

		auto rhs = prefix_expr_t::parse(ps);
		binary_operator->lhs = expr;
		binary_operator->rhs = rhs;
		expr = binary_operator;
	}

	return expr;
}

expr_t *plus_expr_parse(parse_state_t &ps) {
	auto expr = times_expr_parse(ps);

	while (!ps.line_broke() &&
			(ps.token.tk == tk_plus || ps.token.tk == tk_minus || ps.token.tk == tk_backslash))
	{
		auto binary_operator = create<ast::binary_operator_t>(ps.token);
		switch (ps.token.tk) {
		case tk_plus:
			binary_operator->function_name = "__plus__";
			break;
		case tk_minus:
			binary_operator->function_name = "__minus__";
			break;
		case tk_backslash:
			binary_operator->function_name = "__backslash__";
			break;
		default:
			assert(false);
			break;
		}

		eat_token();

		auto rhs = times_expr_parse(ps);
		binary_operator->lhs = expr;
		binary_operator->rhs = rhs;
		expr = binary_operator;
	}

	return expr;
}

expr_t *shift_expr_parse(parse_state_t &ps) {
	auto expr = plus_expr_parse(ps);

	while (!ps.line_broke() &&
		   	(ps.token.tk == tk_shift_left || ps.token.tk == tk_shift_right))
	{
		auto binary_operator = create<ast::binary_operator_t>(ps.token);
		switch (ps.token.tk) {
		case tk_shift_left:
			binary_operator->function_name = "__shl__";
			break;
		case tk_shift_right:
			binary_operator->function_name = "__shr__";
			break;
		default:
			assert(false);
			break;
		}

		eat_token();

		auto rhs = plus_expr_parse(ps);
		binary_operator->lhs = expr;
		binary_operator->rhs = rhs;
		expr = binary_operator;
	}

	return expr;
}

expr_t *binary_eq_expr_parse(parse_state_t &ps) {
	auto lhs = shift_expr_parse(ps);
	if (ps.line_broke()
			|| !(ps.token.tk == tk_binary_equal
				|| ps.token.tk == tk_binary_inequal))
	{
		/* there is no rhs */
		return lhs;
	}

	auto binary_operator = create<ast::binary_operator_t>(ps.token);
	switch (ps.token.tk) {
	case tk_binary_equal:
		binary_operator->function_name = "__binary_eq__";
		break;
	case tk_binary_inequal:
		binary_operator->function_name = "__binary_ineq__";
		break;
	default:
		assert(false);
		break;
	}

	eat_token();

	auto rhs = shift_expr_parse(ps);
	binary_operator->lhs = std::move(lhs);
	binary_operator->rhs = std::move(rhs);
	return std::move(binary_operator);
}

expr_t *ineq_expr_parse(parse_state_t &ps) {
	auto lhs = binary_eq_expr_parse(ps);
	if (ps.line_broke()
			|| !(ps.token.tk == tk_gt
				|| ps.token.tk == tk_gte
				|| ps.token.tk == tk_lt
				|| ps.token.tk == tk_lte)) {
		/* there is no rhs */
		return lhs;
	}

	auto binary_operator = create<ast::binary_operator_t>(ps.token);
	switch (ps.token.tk) {
	case tk_gt:
		binary_operator->function_name = "__gt__";
		break;
	case tk_gte:
		binary_operator->function_name = "__gte__";
		break;
	case tk_lt:
		binary_operator->function_name = "__lt__";
		break;
	case tk_lte:
		binary_operator->function_name = "__lte__";
		break;
	default:
		assert(false);
		break;
	}

	eat_token();

	auto rhs = binary_eq_expr_parse(ps);
	binary_operator->lhs = std::move(lhs);
	binary_operator->rhs = std::move(rhs);
	return std::move(binary_operator);
}

expr_t *eq_expr_parse(parse_state_t &ps) {
	auto lhs = ineq_expr_parse(ps);
	bool not_in = false;
	if (ps.token.is_ident(K(not))) {
		eat_token();
		expect_ident(K(in));
		not_in = true;
	}

	if (ps.line_broke() ||
			!(ps.token.is_ident(K(in))
				|| ps.token.tk == tk_equal
				|| ps.token.tk == tk_inequal)) {
		/* there is no rhs */
		return lhs;
	}

	auto binary_operator = create<ast::binary_operator_t>(ps.token);
	if (ps.token.is_ident(K(in))) {
		binary_operator->function_name = "__in__";
	} else if (ps.token.tk == tk_equal) {
		binary_operator->function_name = "__eq__";
	} else if (ps.token.tk == tk_inequal) {
		binary_operator->function_name = "__ineq__";
	} else {
		assert(false);
	}

	eat_token();

	if (not_in) {
		binary_operator->function_name = "__not_in__";
	}

	auto rhs = ineq_expr_parse(ps);
	binary_operator->lhs = lhs;
	binary_operator->rhs = rhs;
	return binary_operator;
}

expr_t *bitwise_and_parse(parse_state_t &ps) {
	auto expr = eq_expr_parse(ps);

	while (!ps.line_broke() && ps.token.tk == tk_ampersand) {
		auto binary_operator = create<ast::binary_operator_t>(ps.token);
		binary_operator->function_name = "__bitwise_and__";

		eat_token();

		auto rhs = eq_expr_parse(ps);
		binary_operator->lhs = expr;
		binary_operator->rhs = rhs;
		expr = binary_operator;
	}

	return expr;
}

expr_t *bitwise_xor_parse(parse_state_t &ps) {
	auto expr = bitwise_and_parse(ps);

	while (!ps.line_broke() && ps.token.tk == tk_hat) {
		auto binary_operator = create<ast::binary_operator_t>(ps.token);
		binary_operator->function_name = "__xor__";

		eat_token();

		auto rhs = bitwise_and_parse(ps);
		binary_operator->lhs = expr;
		binary_operator->rhs = rhs;
		expr = binary_operator;
	}
	return expr;
}

expr_t *bitwise_or_parse(parse_state_t &ps) {
	auto expr = bitwise_xor_parse(ps);

	while (!ps.line_broke() && ps.token.tk == tk_pipe) {
		auto binary_operator = create<ast::binary_operator_t>(ps.token);
		binary_operator->function_name = "__bitwise_or__";

		eat_token();

		auto rhs = bitwise_xor_parse(ps);
		binary_operator->lhs = expr;
		binary_operator->rhs = rhs;
		expr = binary_operator;
	}

	return expr;
}

expr_t *and_expr_t::parse(parse_state_t &ps) {
	auto expr = bitwise_or_parse(ps);

	while (!ps.line_broke() && (ps.token.is_ident(K(and)))) {
		auto and_expr = create<ast::and_expr_t>(ps.token);

		eat_token();

		auto rhs = bitwise_or_parse(ps);
		and_expr->lhs = expr;
		and_expr->rhs = rhs;
		expr = and_expr;
	}

	return expr;
}

expr_t *parse_tuple_expr(parse_state_t &ps) {
	auto start_token = ps.token;
	chomp_token(tk_lparen);
    if (ps.token.tk == tk_rparen) {
		/* we've got a reference to sole value of unit type */
		auto unit = create<ast::tuple_expr_t>(ps.token);
        ps.advance();
        return unit;
    }
	auto expr = expression_t::parse(ps);
	if (ps.token.tk != tk_comma) {
		chomp_token(tk_rparen);
		return expr;
	} else {
		ps.advance();

		/* we've got a tuple */
		auto tuple_expr = create<ast::tuple_expr_t>(start_token);

		/* add the first value */
		tuple_expr->values.push_back(expr);

		/* now let's find the rest of the values */
		while (ps.token.tk != tk_rparen) {
			expr = expression_t::parse(ps);
			tuple_expr->values.push_back(expr);
			if (ps.token.tk == tk_comma) {
				eat_token();
			} else if (ps.token.tk != tk_rparen) {
				throw user_error(ps.token.location, 
						"unexpected token " c_id("%s") " in tuple. expected comma or right-paren",
						ps.token.text.c_str());
			}
			// continue and read the next parameter
		}
		chomp_token(tk_rparen);
		return tuple_expr;
	}
}

expr_t *or_expr_t::parse(parse_state_t &ps) {
	auto expr = and_expr_t::parse(ps);

	while (!ps.line_broke() && (ps.token.is_ident(K(or)))) {
		auto or_expr = create<ast::or_expr_t>(ps.token);

		eat_token();

		auto rhs = and_expr_t::parse(ps);
		or_expr->lhs = std::move(expr);
		or_expr->rhs = std::move(rhs);
		expr = std::move(or_expr);
	}

	return expr;
}

expr_t *ternary_expr_t::parse(parse_state_t &ps) {
	auto condition = or_expr_t::parse(ps);
	if (ps.token.tk == tk_maybe) {
		ps.advance();
		// TODO: handle a ?? b form

		auto truthy_expr = or_expr_t::parse(ps);
		expect_token(tk_colon);
		ps.advance();
		auto falsey_expr = expression_t::parse(ps);
		auto ternary = ast::create<ternary_expr_t>(condition->token);
		ternary->condition = condition;
		ternary->when_true = truthy_expr;
		ternary->when_false = falsey_expr;
		return ternary;
	} else {
		return condition;
	}
}

expr_t *parse_expr(parse_state_t &ps) {
	return parse_ternary_expr(ps);
}

expr_t *assignment_t::parse(parse_state_t &ps) {
	auto lhs = expression_t::parse(ps);

#define handle_assign(tk_, tk_binop_) \
	if (!ps.line_broke() && ps.token.tk == tk_) { \
		auto token = ps.token; \
		chomp_token(tk_); \
		auto assigment = create<assignment_t>(token); \
		auto rhs = expression_t::parse(ps); \
		assignment->lhs = std::move(lhs); \
		assignment->rhs = std::move(rhs); \
		return std::move(assignment); \
	}

	handle_assign(tk_assign, ast::assignment_t);
	handle_assign(tk_plus_eq, tk_plus);
	// handle_assign(tk_maybe_eq, ast::maybe_assignment_t);
	handle_assign(tk_minus_eq, tk_minus);
	handle_assign(tk_divide_by_eq, tk_divide_by);
	handle_assign(tk_times_eq, tk_times);
	handle_assign(tk_mod_eq, tk_mod);

	if (!ps.line_broke() && ps.token.tk == tk_becomes) {
		if (dyncast<reference_expr_t>(lhs) != nullptr) {
			auto var_decl = create<ast::var_decl_t>(lhs->token);
			var_decl->is_let_var = true;
			var_decl->parsed_type = parsed_type_t(type_variable(lhs->token.location));
			chomp_token(tk_becomes);
			auto initializer = expression_t::parse(ps);
			var_decl->initializer.swap(initializer);
			return var_decl;
		} else if (auto tuple_expr = dyncast<tuple_expr_t>(lhs)) {
			std::shared_ptr<destructured_tuple_decl_t> destructured_tuple_decl = create<destructured_tuple_decl_t>(lhs->token);
			destructured_tuple_decl->is_let = true;
			destructured_tuple_decl->lhs = tuple_expr;
			destructured_tuple_decl->parsed_type = parsed_type_t(type_variable(lhs->token.location));
			chomp_token(tk_becomes);
			destructured_tuple_decl->initializer = expression_t::parse(ps);
			return destructured_tuple_decl;
		} else {
			throw user_error(ps.token.location, ":= may only come after a new symbol name");
		}
	} else {
		return lhs;
	}
}

std::shared_ptr<param_list_decl_t> param_list_decl_t::parse(parse_state_t &ps) {
	/* reset the argument index */
	ps.argument_index = 0;

	auto param_list_decl = create<ast::param_list_decl_t>(ps.token);
	while (ps.token.tk != tk_rparen) {
		param_list_decl->params.push_back(var_decl_t::parse_param(ps));
		if (ps.token.tk == tk_comma) {
			eat_token();
		} else if (ps.token.tk != tk_rparen) {
			throw user_error(ps.token.location, "unexpected token in param_list_decl");
		}
	}
	return param_list_decl;
}

expr_t *parse_block(parse_state_t &ps, bool expression_means_return) {
	bool expression_block_syntax = false;
	token_t expression_block_assign_token;
	bool finish_block = false;
	if (ps.token.tk == tk_lcurly) {
		finish_block = true;
		ps.advance();
	} else if (ps.token.tk == tk_expr_block) {
		expression_block_syntax = true;
		expression_block_assign_token = ps.token;
		ps.advance();
	}

	if (expression_block_syntax) {
		if (!ps.line_broke()) {
			auto statement = parse_statement(ps);
			if (expression_means_return) {
				if (auto expression = dcast<expr_t*>(statement)) {
					auto return_statement = new return_statement_t(expression);
					statement = return_statement;
				}
			}

			if (ps.token.tk != tk_rparen
					&& ps.token.tk != tk_rcurly
					&& ps.token.tk != tk_rsquare
					&& ps.token.tk != tk_comma
					&& !ps.line_broke())
			{
				throw user_error(ps.token.location, "this looks hard to read. you should have a line break after = blocks, unless they are immediately followed by one of these: )]}");
			}
			return statement;
		} else {
			throw user_error(ps.token.location, "empty expression blocks are not allowed");
		}
	} else {
		std::vector<expr_t*> stmts;
		while (ps.token.tk != tk_rcurly) {
			while (ps.token.tk == tk_semicolon) {
				ps.advance();
			}
			stmts.push_back(parse_statement(ps));
			while (ps.token.tk == tk_semicolon) {
				ps.advance();
			}
		}
		if (finish_block) {
			chomp_token(tk_rcurly);
		}
		return new block_t(stmts);
	}
}

std::shared_ptr<if_block_t> if_block_t::parse(parse_state_t &ps) {
	auto if_block = create<ast::if_block_t>(ps.token);
	if (ps.token.is_ident(K(if))) {
		ps.advance();
	} else {
		throw user_error(ps.token.location, "expected if");
	}

	token_t condition_token = ps.token;
	auto expression = expression_t::parse(ps);
	if (auto condition = dyncast<const expression_t>(expression)) {
		if_block->condition = condition;
	} else if (auto var_decl = dyncast<const var_decl_t>(expression)) {
		if_block->condition = var_decl;
	} else {
		throw user_error(condition_token.location,
				"if conditions are limited to expressions or variable definitions");
	}

	if_block->block = parse_block(ps, false /*expression_means_return*/);

	/* check the successive instructions for "else if" or else */
	if (ps.token.is_ident(K(else))) {
		if (ps.line_broke() && if_block->block->token.tk != tk_expr_block) {
			throw user_error(ps.token.location, "else must be on the same line as the prior closing squiggly");
		}
		ps.advance();
		if (ps.token.is_ident(K(if))) {
			if (ps.line_broke()) {
				throw user_error(ps.token.location, "else if must be on the same line");
			}
			if_block->else_ = if_block_t::parse(ps);
		} else {
			if_block->else_ = parse_block(ps, false /*expression_means_return*/);
		}
	}

	return if_block;
}

std::shared_ptr<while_block_t> while_block_t::parse(parse_state_t &ps) {
	auto while_block = create<ast::while_block_t>(ps.token);
	auto while_token = ps.token;
	chomp_ident(K(while));
	token_t condition_token = ps.token;
	if (condition_token.is_ident(K(match))) {
		/* sugar for while match ... which becomes while true { match ... } */
		auto true_token = token_t{while_token.location, tk_identifier, "true"};
		while_block->condition = create<reference_expr_t>(true_token);
		while_block->block = create<block_t>(while_token);
		while_block->block->statements.push_back(match_expr_t::parse(ps));
	} else {
		auto expr = expression_t::parse(ps);
		if (auto condition = dyncast<const expression_t>(expr)) {
			while_block->condition = condition;
		} else if (auto var_decl = dyncast<const var_decl_t>(expr)) {
			while_block->condition = var_decl;
		} else {
			throw user_error(condition_token.location,
					"while conditions are limited to expressions or variable definitions");
		}

		auto block = parse_block(ps, false /*expression_means_return*/);
		while_block->block = block;
	}
	return while_block;
}

ast::predicate_t::ref ctor_predicate_t::parse(parse_state_t &ps, token_t *name_assignment) {
	assert(ps.token.tk == tk_identifier && isupper(ps.token.text[0]));
	auto value_name = ps.token;

	ps.advance();
	
	std::vector<predicate_t::ref> params;
	if (ps.token.tk == tk_lparen) {
		ps.advance();
		bool expect_comma = false;
		while (ps.token.tk != tk_rparen) {
			if (expect_comma) {
				chomp_token(tk_comma);
			}

			auto predicate = predicate_t::parse(ps, false /*allow_else*/, nullptr /*name_assignment*/);
			params.push_back(predicate);
			expect_comma = true;
		}
		chomp_token(tk_rparen);
	}
	auto ctor = ast::create<ast::ctor_predicate_t>(value_name);
	if (name_assignment != nullptr) {
		ctor->name_assignment = *name_assignment;
	}
	std::swap(ctor->params, params);
	return ctor;
}

ast::predicate_t::ref tuple_predicate_t::parse(parse_state_t &ps, token_t *name_assignment) {
	assert(ps.token.tk == tk_lparen);
	auto paren_token = ps.token;

	ps.advance();

	std::vector<predicate_t::ref> params;
	bool expect_comma = false;
	while (ps.token.tk != tk_rparen) {
		if (expect_comma) {
			chomp_token(tk_comma);
		}

		auto predicate = predicate_t::parse(ps, false /*allow_else*/, nullptr /*name_assignment*/);
		params.push_back(predicate);
		expect_comma = true;
	}
	chomp_token(tk_rparen);
	auto tuple = ast::create<ast::tuple_predicate_t>(paren_token);
	if (name_assignment != nullptr) {
		tuple->name_assignment = *name_assignment;
	}
	std::swap(tuple->params, params);
	return tuple;
}

ast::predicate_t::ref predicate_t::parse(parse_state_t &ps, bool allow_else, token_t *name_assignment) {
	if (ps.token.is_ident(K(else))) {
		if (!allow_else) {
			throw user_error(ps.token.location, "illegal keyword " c_type("%s") " in a pattern match context",
					ps.token.text.c_str());
		}
	} else if (is_restricted_var_name(ps.token.text)) {
		throw user_error(ps.token.location, "irrefutable predicates are restricted to non-keyword symbols");
	}

	if (ps.token.tk == tk_lparen) {
		return tuple_predicate_t::parse(ps, name_assignment);
	} else if (ps.token.tk == tk_identifier) {
		if (isupper(ps.token.text[0])) {
			/* match a ctor */
			return ctor_predicate_t::parse(ps, name_assignment);
		} else {
			if (name_assignment != nullptr) {
				throw user_error(ps.token.location, "pattern name assignment is only allowed once per term");
			} else {
				/* match anything */
				auto symbol = ps.token;
				ps.advance();
				if (ps.token.tk == tk_about) {
					ps.advance();

					return predicate_t::parse(ps, allow_else, &symbol);
				} else {
					return ast::create<ast::irrefutable_predicate_t>(symbol);
				}
			}
		}
	} else {
		if (name_assignment != nullptr) {
			throw user_error(ps.token.location, "pattern name assignment is only allowed for data constructor matching");
		}

		std::string sign;
		switch (ps.token.tk) {
		case tk_minus:
		case tk_plus:
			sign = ps.token.text;
			ps.advance();
			if (ps.token.tk != tk_integer && ps.token.tk != tk_float) {
				throw user_error(ps.prior_token.location, "unary prefix %s is not allowed before %s in this context",
						ps.prior_token.text.c_str(),
						ps.token.text.c_str());
			}
			break;
		default:
			break;
		}

		switch (ps.token.tk) {
		case tk_string:
		case tk_char:
			{
				/* match a literal */
				auto literal = create<ast::literal_expr_t>(ps.token);
				ps.advance();
				return literal;
			}
		case tk_integer:
		case tk_float:
			{
				/* match a literal */
				auto literal = create<ast::literal_expr_t>(
						sign != ""
						? token_t(ps.token.location, ps.token.tk, sign + ps.token.text)
						: ps.token);
				ps.advance();
				return literal;
			}
		default:
			throw user_error(ps.token.location, "unexpected token for pattern " c_warn("%s"),
					ps.token.text.c_str());
		}
		return null_impl();
	}
}

pattern_block_t *parse_pattern_block(parse_state_t &ps) {
	return new pattern_block_t(
			parse_predicate(ps, true /*allow_else*/, nullptr /*name_assignment*/),
			parse_block(ps, false /*expression_means_return*/));
}

std::shared_ptr<match_expr_t> match_expr_t::parse(parse_state_t &ps) {
	auto when_block = create<ast::match_expr_t>(ps.token);
	chomp_ident(K(match));
	bool auto_else = false;
	token_t bang_token = ps.token;
	if (ps.token.tk == tk_bang) {
		auto_else = true;
		ps.advance();
	}
	when_block->value = expression_t::parse(ps);
	chomp_token(tk_lcurly);
	while (ps.token.tk != tk_rcurly) {
		if (ps.token.is_ident(K(else))) {
			throw user_error(ps.token.location, "place else patterns outside of the match block. (match ... { ... } else { ... })");
		}
		when_block->pattern_blocks.push_back(pattern_block_t::parse(ps));
	}
	chomp_token(tk_rcurly);
	if (auto_else) {
		auto pattern_block = ast::create<ast::pattern_block_t>(bang_token);
		pattern_block->predicate = ast::create<ast::irrefutable_predicate_t>(token_t{bang_token.location, tk_identifier, "else"});
		pattern_block->block = ast::create<ast::block_t>(bang_token);
		when_block->pattern_blocks.push_back(pattern_block);
	}
   	if (ps.token.is_ident(K(else))) {
		if (auto_else) {
			throw user_error(ps.token.location, "no need for else block when you are using \"match!\". either delete the ! or discard the else block");
		} else {
			when_block->pattern_blocks.push_back(pattern_block_t::parse(ps));
		}
	}

	if (when_block->pattern_blocks.size() == 0) {
		throw user_error(ps.token.location, "when block did not have subsequent patterns to match");
	}

	return when_block;
}

std::shared_ptr<function_decl_t> function_decl_t::parse(
        parse_state_t &ps,
        bool within_expression,
        types::type_t::ref default_return_type)
{
	location_t location = ps.token.location;
	chomp_ident(K(fn));

	bool no_closure = false;
	if (ps.token.tk == tk_lsquare) {
		ps.advance();
		chomp_token(tk_rsquare);
		no_closure = true;
	}
	identifier_t function_name;
	auto parsed_type = types::parse_function_type(ps, location, {}, function_name, default_return_type);
	debug_above(6, log("parsed function type %s at %s", parsed_type->str().c_str(), ps.token.location.str().c_str()));

	std::string name;
	if (function_name != nullptr) {
		name = function_name->get_name();
	} else if (!within_expression) {
		throw user_error(parsed_type->get_location(), "function is missing a name");
	}

	if (name == "main") {
		if (extends_module == nullptr) {
			extends_module = make_iid_impl(GLOBAL_SCOPE_NAME, ps.token.location);
		} else {
			throw user_error(attributes_location,
					"the main function may not specify a scope injection module");
		}
	}

	auto name_token = 
		function_name != nullptr
		? token_t(
				function_name->get_location(),
				tk_identifier,
				name)
		: token_t(
				parsed_type->get_location(),
				tk_identifier,
				"");

	auto function_decl = create<ast::function_decl_t>(name_token);
	function_decl->function_type = parsed_type;
	function_decl->extends_module = extends_module;
	function_decl->link_to_name = name_token;
	function_decl->no_closure = no_closure;
	return function_decl;
}

// TODO: put type mappings into the scope
type_t::ref parse_type(parse_state_t &ps, const std::set<identifier_t> &generics) {
	assert(ps.token.tk != tk_lcurly && ps.token.tk != tk_rcurly);
	return types::parse_or_type(ps, generics);
}

lambda_t *parse_lambda(parse_state_t &ps) {
	if (ps.token.tk == tk_lsquare) {
		throw user_error(ps.token.location, "not yet impl");
	}
	if (ps.token.tk == tk_comma || ps.token.tk == tk_lparen) {
		ps.advance();
	}
	scoping_t scoping(ps);

	if (ps.token.tk != tk_comma && ps.token.tk != tk_rparen) {
		auto type = parse_type(ps, {});
		log(type->get_location(), "discarding parsed param type %s", type->str().c_str());
	}

	if (ps.token.tk == tk_comma) {
		return new lambda_t(make_code_id(scoping.var), parse_lambda(ps));
	} else if (ps.token.tk == tk_rparen) {
		if (ps.token.tk != tk_lcurly && ps.token.tk != tk_expr_block) {
			auto return_type = parse_type(ps, {});
			log(type->get_location(), "discarding parsed return type %s", return_type->str().c_str());
		}

		return new lambda_t(make_code_id(scoping.var), parse_block(ps, true /*expression_means_return*/));
	}
}

void parse_maybe_type_decl(parse_state_t &ps, identifiers_t &type_variables) {
	while (!ps.line_broke() && ps.token.tk == tk_identifier) {
		if (token_is_illegal_in_type(ps.token)) {
			if (ps.token.is_ident(K(any))) {
				throw user_error(ps.token.location, "`any` is unnecessary within type parameters of type declarations");
			}

			break;
		}

		/* we found a type variable, let's stash it */
		type_variables.push_back(make_code_id(ps.token));
		ps.advance();
	}
}

identifier_t make_code_id(const token_t &token) {
	return std::make_shared<code_id>(token);
}

identifier_t make_type_id_code_id(const location_t location, std::string var_name) {
	return std::make_shared<type_id_code_id>(location, var_name);
}

type_decl_t::ref type_decl_t::parse(parse_state_t &ps, token_t name_token) {
	identifiers_t type_variables;
	parse_maybe_type_decl(ps, type_variables);
	return create<ast::type_decl_t>(name_token, type_variables);
}

std::shared_ptr<type_def_t> type_def_t::parse(parse_state_t &ps) {
	chomp_ident(K(type));
	expect_token(tk_identifier);
	auto type_name_token = ps.token;
	ps.advance();

	auto type_def = create<ast::type_def_t>(type_name_token);
	type_def->type_decl = type_decl_t::parse(ps, type_name_token);
	type_def->type_algebra = ast::type_algebra_t::parse(ps, type_def->type_decl);
	return type_def;
}

type_algebra_t::ref type_algebra_t::parse(
		parse_state_t &ps,
		ast::type_decl_t::ref type_decl)
{
    indent_logger indent(type_decl->token.location, 8, string_format("parsing type algebra for %s",
                type_decl->token.text.c_str()));

	if (ps.token.is_ident(K(is))) {
		return data_type_t::parse(ps, type_decl, type_decl->type_variables);
	} else if (ps.token.is_ident(K(has))) {
		return type_product_t::parse(ps, type_decl, type_decl->type_variables, false /*native*/);
	} else if (ps.token.is_ident(K(link))) {
		return type_link_t::parse(ps, type_decl, type_decl->type_variables);
	} else if (ps.token.tk == tk_assign) {
		return type_alias_t::parse(ps, type_decl, type_decl->type_variables);
	} else if (ps.token.is_ident(K(struct))) {
		return type_product_t::parse(ps, type_decl, type_decl->type_variables, true /*native*/);
	} else {
		throw user_error(ps.token.location, 
				"type descriptions must begin with "
			   	c_id("is") ", " c_id("has") ", or " c_id("=") ". (Found %s)",
				ps.token.str().c_str());
	}
}

std::pair<token_t, types::type_args_t::ref> parse_ctor(
		parse_state_t &ps,
	   	identifiers_t type_variables_list)
{
	expect_token(tk_identifier);
	auto name = ps.token;
	if (!isupper(name.text[0])) {
		throw user_error(name.location, "constructor names must begin with an uppercase letter");
	}

	ps.advance();
	return {name, types::parse_data_ctor_type(ps, to_set(type_variables_list))};
}

data_type_t::ref data_type_t::parse(
		parse_state_t &ps,
		ast::type_decl_t::ref type_decl,
		identifiers_t type_variables_list)
{
	std::set<identifier_t> type_variables = to_set(type_variables_list);
	auto is_token = ps.token;
	chomp_ident(K(is));
	chomp_token(tk_lcurly);

	auto data_type = create<data_type_t>(type_decl->token);
	while (ps.token.tk == tk_identifier) {
		auto ctor_pair = parse_ctor(ps, type_variables_list);
		for (auto x : data_type->ctor_pairs) {
			if (x.first.text == ctor_pair.first.text) {
				auto error = user_error(ctor_pair.first.location, "duplicated data constructor name");
				error.add_info(x.first.location, "see initial declaration here");
				throw error;
			}
		}
		debug_above(8, log("parsed ctor %s for type " c_type("%s"), ctor_pair.first.str().c_str(), data_type->token.text.c_str()));
		data_type->ctor_pairs.push_back(ctor_pair);
	}

	chomp_token(tk_rcurly);

	return data_type;
}

type_product_t::ref type_product_t::parse(
		parse_state_t &ps,
		ast::type_decl_t::ref type_decl,
	   	identifiers_t type_variables,
		bool native)
{
	std::set<identifier_t> generics = to_identifier_set(type_variables);
	if (native) {
		expect_ident(K(struct));
	} else {
		expect_ident(K(has));
	}
	auto type = types::parse_product_type(ps, generics);
	return create<type_product_t>(type_decl->token, native, type, generics);
}

type_link_t::ref type_link_t::parse(
		parse_state_t &ps,
		ast::type_decl_t::ref type_decl,
		identifiers_t type_variables)
{
	std::set<identifier_t> generics = to_identifier_set(type_variables);
	chomp_ident(K(link));
	return create<type_link_t>(type_decl->token);

}

type_alias_t::ref type_alias_t::parse(
		parse_state_t &ps,
		ast::type_decl_t::ref type_decl,
	   	identifiers_t type_variables)
{
	chomp_token(tk_assign);

	std::set<identifier_t> generics = to_identifier_set(type_variables);
	types::type_t::ref type = types::parse_type(ps, generics);

	auto type_alias = ast::create<ast::type_alias_t>(type_decl->token);
	assert(type_alias->token.text != "");

	type_alias->parsed_type = parsed_type_t(type);
	type_alias->type_variables = generics;
	return type_alias;
}

dimension_t::ref dimension_t::parse(parse_state_t &ps, std::set<identifier_t> generics) {
	token_t primary_token;
	std::string name;
	if (ps.token.is_ident(K(var))) {
		ps.advance();
		expect_token(tk_identifier);
		primary_token = ps.token;
		name = primary_token.text;
		ps.advance();
	} else {
		throw user_error(ps.token.location, "not sure what's going on here");
	}

	types::type_t::ref type = types::parse_type(ps, generics);
	return ast::create<ast::dimension_t>(primary_token, name, type);
}

module_t *parse_module(parse_state_t &ps, identifiers_t &module_deps) {
	debug_above(6, log("about to parse %s with type_macros: [%s]",
				ps.filename.c_str(),
				join_with(ps.type_macros, ", ", [] (type_macros_t::value_type v) -> std::string {
					return v.first + ": " + v.second->str();
					}).c_str()));

	assert(ps.module_name.size() == 0);
	ps.module_name = strip_zion_extension(leaf_from_file_path(ps.filename));
	assert(ps.module_name.size() != 0);

	std::vector<decl_t *> decls;

	while (ps.token.is_ident(K(get))) {
		ps.advance();
		expect_token(tk_identifier);
		std::string module_name = ps.token.text;
		ps.advance();
		expect_token(tk_lcurly);
		while (ps.token.tk != tk_rcurly) {
			expect_token(tk_identifier);
			ps.add_term_map(ps.token.location, ps.token.text, module_name + "." + ps.token.text);
			ps.advance();
			if (ps.token.tk == tk_comma) {
				ps.advance();
			}
		}
	}

	while (true) {
		if (ps.token.is_ident(K(var)) || ps.token.is_ident(K(let))) {
			bool is_let = ps.token.is_ident(K(let));
			if (is_let) {
				throw user_error(ps.token.location, "let variables are not yet supported at the module level");
			} else {
				ps.advance();
				auto var = parse_expr(ps, is_let, false /*allow_tuple_destructuring*/);
				auto var_decl = dyncast<var_decl_t>(var);
				assert(var_decl != nullptr);
				module->var_decls.push_back(var_decl);
			}
		}
	}
	return new module_t(ps.module_name, decls);
}

void ff() {
	/* Get vars, functions or type defs */
	while (true) {
		if (ps.token.is_ident(K(link))) {
			auto link_statement = link_statement_parse(ps);
			if (auto linked_function = dyncast<link_function_statement_t>(link_statement)) {
				module->linked_functions.push_back(linked_function);
			} else if (auto linked_var = dyncast<link_var_statement_t>(link_statement)) {
				module->linked_vars.push_back(linked_var);
			}
		} else if (ps.token.is_ident(K(var)) || ps.token.is_ident(K(let))) {
			bool is_let = ps.token.is_ident(K(let));
			if (is_let) {
				throw user_error(ps.token.location, "let variables are not yet supported at the module level");
			} else {
				ps.advance();
				auto var = var_decl_t::parse(ps, is_let, false /*allow_tuple_destructuring*/);
				auto var_decl = dyncast<var_decl_t>(var);
				assert(var_decl != nullptr);
				module->var_decls.push_back(var_decl);
			}
		} else if (ps.token.tk == tk_lsquare || ps.token.is_ident(K(fn))) {
			/* function definitions */
			auto function = function_defn_t::parse(ps, false /*within_expression*/);
			if (function->token.text == "main") {
				bool have_linked_main = false;
				for (auto linked_module : module->linked_modules) {
					if (linked_module->token.text == "main") {
						have_linked_main = true;
						break;
					}
				}
				if (!have_linked_main && getenv("NO_STD_MAIN") == nullptr) {
					std::shared_ptr<link_module_statement_t> linked_module = create<link_module_statement_t>(ps.token);
					linked_module->link_as_name = token_t(
							function->decl->token.location,
							tk_identifier,
							types::gensym(INTERNAL_LOC())->get_name());
					linked_module->extern_module = create<ast::module_decl_t>(token_t(
								function->decl->token.location,
								tk_identifier,
								"main"));
					linked_module->extern_module->name = linked_module->extern_module->token;
					module->linked_modules.push_back(linked_module);
				}
			}
			module->functions.push_back(std::move(function));
		} else if (ps.token.is_ident(K(type))) {
			/* type definitions */
			auto type_def = type_def_t::parse(ps);
			module->type_defs.push_back(type_def);
			if (module->global) {
				auto id = make_code_id(type_def->token);
				ps.type_macros.insert({type_def->token.text, type_id(id)});
				ps.global_type_macros.insert({type_def->token.text, type_id(id)});
			}
		} else {
			break;
		}
	}

	if (ps.token.is_ident(K(link))) {
		throw user_error(ps.token.location, C_MODULE "link" C_RESET " directives must come before types, variables, and functions");
	} else if (ps.token.tk != tk_none) {
		throw user_error(ps.token.location, "unexpected '" c_id("%s") "' at top-level module scope (%s)",
				ps.token.text.c_str(), tkstr(ps.token.tk));
	}

	return module;
}
