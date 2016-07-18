#include "zion.h"
#include "logger.h"
#include "type_checker.h"
#include "utils.h"
#include "compiler.h"
#include "llvm_zion.h"
#include "llvm_utils.h"
#include "json.h"
#include "ast.h"
#include "llvm_types.h"
#include "parser.h"
#include "unification.h"
#include "code_id.h"

/*
 * The basic idea here is that type checking is a graph operation which can be
 * ordered topologically based on dependencies between callers and callees.
 * Luckily our AST has exactly that structure.  We will perform a topological
 * sort by resolving types as we return from our depth first traversal.
 */


/************************************************************************/

bound_type_t::ref get_fully_bound_param_info(
		status_t &status,
        llvm::IRBuilder<> &builder,
        const ast::var_decl &obj,
        scope_t::ref scope,
        atom &var_name,
        atom::set &generics,
		int &generic_index)
{
	/* get the name of this parameter */
	var_name = obj.token.text;

	if (obj.type_ref != nullptr) {
		/* the user specified a type */
		auto term = obj.type_ref->get_type_term();
		return upsert_bound_type(status, builder, scope, term);
	} else {
		/* the user specified no type */
		return null_impl(); // ast::type_ref::resolve_null_param_type(status, builder, obj, scope, generic_index);
	}
}

bound_var_t::ref type_check_bound_var_decl(
        status_t &status,
        llvm::IRBuilder<> &builder,
        const ast::var_decl &obj,
        scope_t::ref scope)
{
	const atom symbol = obj.token.text;

	debug_above(4, log(log_info, "type_check_var_decl is looking for a type for variable " c_var("%s") " : %s",
				symbol.c_str(), obj.str().c_str()));

	if (!scope->has_bound_variable(symbol, rc_capture_level)) {
		bound_var_t::ref init_var;
		bound_type_t::ref type;

		if (obj.initializer || obj.type_ref) {
			/* we have an initializer or a type decl or both */
			if (obj.initializer) {
				init_var = obj.initializer->resolve_instantiation(status,
						builder, scope, nullptr, nullptr);

				if (!!status) {
					type = init_var->type;
					assert(type != nullptr);
				}
			}
			if (!!status) {
				if (obj.type_ref) {
					type = upsert_bound_type(status, builder,
							scope, obj.type_ref->get_type_term());
				}

				if (!!status) {
					if (init_var) {
						assert(type != nullptr);
						unification_t unification = unify(
								type->get_term(),
								init_var->type->get_term(),
								scope->get_type_env());

						if (!unification.result) {
							/* report that the variable type does not match the initializer type */
							user_error(status, obj, "type of " c_var("%s") " does not match type of initializer",
									obj.token.text.c_str());
							user_error(status, obj, c_type("%s") " != " c_type("%s"),
									type->str().c_str(),
									init_var->type->str().c_str());

							/* try to continue without the initializer just to
							 * get more feedback for the user */
							init_var.reset();
						}
					} 

					/* generate the mutable stack-based variable for this var */
					llvm::Function *llvm_function = llvm_get_function(builder);
					llvm::AllocaInst *llvm_alloca = llvm_create_entry_block_alloca(llvm_function, type, symbol);

					if (init_var) {
						debug_above(6, log(log_info, "creating a store instruction %s := %s",
									llvm_print_value_ptr(llvm_alloca).c_str(),
									llvm_print_value_ptr(init_var->llvm_value).c_str()));
						builder.CreateStore(init_var->llvm_value, llvm_alloca);	
					}

					/* the reference_expr that looks at this llvm_value
					 * will need to know to use store/load semantics, not
					 * just pass-by-value */
					return bound_var_t::create(INTERNAL_LOC(), symbol,
							type, llvm_alloca, make_code_id(obj.token));
				}
			}
		} else {
			/* we've got to have one of these to initialize this variable */
			user_error(status, obj, "variables must have an initializer or a type expression");
		}
	} else {
		// TODO: get a pointer to the prior var
		user_error(status, obj, "variables cannot be redeclared");
		debug_above(2, log(log_info, "%s", scope->str().c_str()));
	}

	assert(!status);
	return nullptr;
}

atom::many get_param_list_decl_variable_names(ast::param_list_decl::ref obj) {
	atom::many names;
	for (auto param : obj->params) {
		names.push_back({param->token.text});
    }
    return names;
}

bound_type_t::named_pairs zip_named_pairs(
		atom::many names,
	   	bound_type_t::refs args)
{
	bound_type_t::named_pairs named_args;
	assert(names.size() == args.size());
	for (int i = 0; i < args.size(); ++i) {
		named_args.push_back({names[i], args[i]});
	}
	return named_args;
}

status_t get_fully_bound_param_list_decl_variables(
		llvm::IRBuilder<> &builder,
		ast::param_list_decl &obj,
		scope_t::ref scope,
		bound_type_t::named_pairs &params)
{
	status_t status;

	/* we keep track of the generic parameters to ensure equivalence */
	atom::set generics;
	int generic_index = 1;

	for (auto param : obj.params) {
		atom var_name;
		bound_type_t::ref param_type = get_fully_bound_param_info(status,
				builder, *param, scope, var_name, generics, generic_index);

		if (!!status) {
			params.push_back({var_name, param_type});
        }
    }
    return status;
}

bound_type_t::ref get_return_type_from_return_type_expr(
        status_t &status,
        llvm::IRBuilder<> &builder,
        ast::type_ref::ref type_ref,
        scope_t::ref scope)
{
    /* lookup the alias, default to void */
    if (type_ref != nullptr) {
		return upsert_bound_type(status, builder, scope,
				type_ref->get_type_term());
    } else {
		/* user specified no return type, default to void */
		return scope->get_program_scope()->get_bound_type({"void"});
    }
}

void type_check_fully_bound_function_decl(
        status_t &status,
        llvm::IRBuilder<> &builder,
        const ast::function_decl &obj,
        scope_t::ref scope,
        bound_type_t::named_pairs &params,
        bound_type_t::ref &return_value)
{
    /* returns the parameters and the return value types fully resolved */
    debug_above(4, log(log_info, "type checking function decl %s", obj.token.str().c_str()));

    if (obj.param_list_decl) {
        /* the parameter types as per the decl */
        status |= get_fully_bound_param_list_decl_variables(builder,
                *obj.param_list_decl, scope, params);

        if (!!status) {
            return_value = get_return_type_from_return_type_expr(status, builder,
                    obj.return_type_ref, scope);

            /* we got the params, and the return value */
            return;
        }
    } else {
        user_error(status, obj, "no param_list_decl was present");
    }

    assert(!status);
}

bool is_function_defn_generic_impl(scope_t::ref scope, const ast::function_defn &obj) {
    if (obj.decl->param_list_decl) {
		/* check the parameters' genericity */
		auto &params = obj.decl->param_list_decl->params;
		for (auto &param : params) {
			if (!param->type_ref) {
				debug_above(3, log(log_info, "found a missing parameter type on %s, defaulting it to an unnamed generic",
							param->str().c_str()));
				return true;
			}
			types::term::ref term = param->type_ref->get_type_term();

			if (term->is_generic(scope->get_type_env())) {
				debug_above(3, log(log_info, "found a generic parameter type on %s",
							param->str().c_str()));
				return true;
			}
		}
	} else {
		panic("function declaration has no parameter list");
	}

	if (obj.decl->return_type_ref) {
		/* check the return type's genericity */
		types::term::ref term = obj.decl->return_type_ref->get_type_term();
		return term->is_generic(scope->get_type_env());
	} else {
		/* default to void, which is fully bound */
		return false;
	}
}

bool is_function_defn_generic(scope_t::ref scope, const ast::function_defn &obj) {
	auto generic = is_function_defn_generic_impl(scope, obj);
    log(log_info, "%s is %s", obj.token.str().c_str(),
		   	generic ? c_type("generic") : c_var("fully bound"));
	return generic;
}


function_scope_t::ref make_param_list_scope(
        status_t &status,
        const ast::function_decl &obj,
        scope_t::ref &scope,
		bound_var_t::ref function_var,
		bound_type_t::named_pairs params)
{
    /* this function is coupled to the sbk_generic_substitution mechanism.
     * when we're not dealing with generics, it simply looks up the types in
     * the decl parameter's type expressions. when we're dealing with generics
     * after using the incoming scope to find the bound parameter types (based
     * on upstream callsite arguments), we drop the sbk_generic_substitution in
     * order to prevent those type names from being visible within the function.
     */
    assert(!!status);

    if (!!status) {
        auto new_scope = scope->new_function_scope(
                string_format("function-%s", function_var->name.c_str()));

        assert(obj.param_list_decl->params.size() == params.size());

        llvm::Function::arg_iterator args = llvm::cast<llvm::Function>(function_var->llvm_value)->arg_begin();

        int i = 0;

        for (auto &param : params) {
            llvm::Value *llvm_param = args++;
            llvm_param->setName(param.first.str());

            /* add the parameter argument to the current scope */
            new_scope->put_bound_variable(param.first,
                    bound_var_t::create(INTERNAL_LOC(), param.first, param.second,
                        llvm_param, make_code_id(obj.param_list_decl->params[i++]->token)));
        }

        return new_scope;
    } else {
        return nullptr;
    }
}

bound_var_t::ref ast::link_module_statement::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
	   	bool *returns) const
{
	module_scope_t::ref module_scope = dyncast<module_scope_t>(scope);
	assert(module_scope != nullptr);

    auto linked_module_name = extern_module->get_canonical_name();
	assert(linked_module_name.size() != 0);

	program_scope_t::ref program_scope = scope->get_program_scope();
	module_scope_t::ref linked_module_scope = program_scope->lookup_module(linked_module_name);

	if (linked_module_scope != nullptr) {
		/* put the module into program scope as a named variable. this is to
		 * enable dot-expressions to resolve module scope lookups. note that
		 * the module variables are not reified into the actual generated LLVM
		 * IR.  they are resolved entirely at compile time.  perhaps in a
		 * future version they can be used as run-time variables, so that we
		 * can pass modules around for another level of polymorphism. */
		bound_module_t::ref module_variable = bound_module_t::create(INTERNAL_LOC(),
				link_as_name.text, make_code_id(token), linked_module_scope);

		module_scope->put_bound_variable(module_variable->name, module_variable);

		return module_variable;
	} else {
		user_error(status, *this, "can't find module %s", linked_module_name.c_str());
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::link_function_statement::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
	   	bool *returns) const
{
    module_scope_t::ref module_scope = dyncast<module_scope_t>(scope);
    assert(module_scope);

    if (!scope->has_bound_variable(link_as_name.text, rc_just_current_scope)) {
        bound_type_t::named_pairs named_args;
        bound_type_t::ref return_value;

        type_check_fully_bound_function_decl(status, builder,
                *extern_function, scope, named_args, return_value);

        if (!!status) {
			bound_type_t::refs args;
			for (auto &named_arg_pair : named_args) {
				args.push_back(named_arg_pair.second);
			}

			// TODO: rearrange this, and get the pointer type from the term
            llvm::FunctionType *llvm_func_type = llvm_create_function_type(
                    status, builder, args, return_value);

            /* get the full function term */
            types::term::ref function_sig = get_function_term(args, return_value);
			debug_above(3, log(log_info, "%s has term %s",
						link_as_name.str().c_str(),
						function_sig->str().c_str()));

            llvm::Value *llvm_value = llvm::Function::Create(llvm_func_type,
                    llvm::Function::ExternalLinkage, link_as_name.text,
                    module_scope->llvm_module);

            /* actually create or find the finalized bound type for this function */
			bound_type_t::ref bound_function_type = upsert_bound_type(
					status, builder, scope, function_sig);

			return bound_var_t::create(
					INTERNAL_LOC(),
					scope->make_fqn(link_as_name.text),
                    bound_function_type,
                    llvm_value,
                    make_code_id(extern_function->token));
        }
    } else {
        user_error(status, *this, "name conflict with %s", link_as_name.text.c_str());
    }

    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::dot_expr::resolve_overrides(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        const ptr<const ast::item> &callsite,
        const bound_type_t::refs &args) const
{
	indent_logger indent;
	log(log_info, "dot_expr::resolve_overrides for %s", callsite->str().c_str());

	/* check the left-hand side first, it should be a type_namespace */
	bound_var_t::ref lhs_var = lhs->resolve_instantiation(
			status, builder, scope, nullptr, nullptr);

	if (!!status) {
		if (lhs_var->type->is_struct()) {
			/* we've got a struct on the lhs, let's lookup the member by name */
			not_impl();
		} else if (auto bound_module = dyncast<const bound_module_t>(lhs_var)) {
			assert(bound_module->module_scope != nullptr);

			/* let's see if the associated module has a method that can handle this callsite */
			return get_callable(status, builder, bound_module->module_scope,
					rhs.text, callsite, get_args_term(args));
		} else {
			user_error(status, *lhs, "left of a dot (\".\") must be a struct or module. this is not a struct or module. %s",
					lhs_var->str().c_str());
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::callsite_expr::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    /* get the value of calling a function */
    bound_type_t::refs param_types;
    bound_var_t::refs arguments;

    if (params && params->expressions.size() != 0) {
        /* iterate through the parameters and add their types to a vector */
        for (auto &param : params->expressions) {
            bound_var_t::ref param_var = param->resolve_instantiation(
                    status, builder, scope, nullptr, nullptr);

            if (!status) {
                break;
            }

            arguments.push_back(param_var);
            param_types.push_back(param_var->type);
        }
    } else {
        /* the callsite has no parameters */
    }

	if (!!status) {
		if (auto can_reference_overloads = dyncast<can_reference_overloads_t>(function_expr)) {
			/* we need to figure out which overload to call, if there are any */
			bound_var_t::ref function = can_reference_overloads->resolve_overrides(
					status, builder, scope, shared_from_this(),
					bound_type_t::refs_from_vars(arguments));

			if (!!status) {
				log(log_info, "function chosen is %s", function->str().c_str());

				return make_call_value(status, builder, shared_from_this(), scope,
						function, arguments);
			}
		} else {
			user_error(status, *function_expr,
					"%s being called like a function. arguments are %s",
					function_expr->str().c_str(),
					::str(arguments).c_str());
			return nullptr;
		}
	}

    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::reference_expr::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	/* we wouldn't be referencing a variable name here unless it was unique
	 * override resolution only happens on callsites, and we don't allow
	 * passing around unresolved overload references */
    bound_var_t::ref var = scope->get_bound_variable(status, shared_from_this(), token.text);

    if (!var) {
        user_error(status, *this, "undefined symbol " c_id("%s"), token.text.c_str());
    }

    return var;
}

bound_var_t::ref ast::array_index_expr::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	/* this expression looks like this
	 *
	 *   lhs[index]
	 *
	 */

	if (!!status) {
		bound_var_t::ref lhs_val = lhs->resolve_instantiation(status, builder,
				scope, nullptr, nullptr);

		if (!!status) {
			/* check to see if we have a literal index */
			if (auto literal_expr = dyncast<ast::literal_expr>(index)) {
				/* check to see if it is an integer */
				if (literal_expr->token.tk == tk_integer) {
					int64_t value = atoll(literal_expr->token.text.c_str());

					/* see if we have a deref operator function for the lhs type */
					return call_const_subscript_operator(status, builder,
							scope, shared_from_this(), lhs_val, value);
				} else {
					user_error(status, *this,
							"tuple dereferencing with " c_internal("%s") " is not yet impl",
							tkstr(literal_expr->token.tk));
				}
			} else {
				user_error(status, *this, "not impl");
			}
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::array_literal_expr::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    user_error(status, *this, "not impl");
    return nullptr;
}

bound_var_t::ref type_check_binary_operator(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		ptr<const ast::expression> lhs,
		ptr<const ast::expression> rhs,
		const ast::expression &obj)
{
	auto function_name = obj.token.text;
	assert(function_name.size() != 0);

	bound_var_t::ref lhs_var, rhs_var;
	lhs_var = lhs->resolve_instantiation(status, builder, scope, nullptr, nullptr);
	if (!!status) {
		rhs_var = rhs->resolve_instantiation(status, builder, scope, nullptr, nullptr);

		if (!!status) {
			/* get or instantiate a function we can call on these arguments */
			return call_program_function(
					status, builder, scope, function_name,
					obj.shared_from_this(), {lhs_var, rhs_var});
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::eq_expr::resolve_instantiation(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		local_scope_t::ref *new_scope,
		bool *returns) const
{
    return type_check_binary_operator(status, builder, scope, lhs, rhs, *this);
}

bound_var_t::ref ast::tuple_expr::resolve_instantiation(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
	   	scope_t::ref scope,
	   	local_scope_t::ref *new_scope,
	   	bool *returns) const
{
	/* let's get the actual values in our tuple. */
	bound_var_t::refs vars;
	vars.reserve(values.size());

	for (auto &value: values) {
		bound_var_t::ref var = value->resolve_instantiation(status, builder,
				scope, nullptr, nullptr);
		if (!!status) {
			vars.push_back(var);
		}
	}

	if (!!status) {
		bound_type_t::refs args = get_bound_types(vars);

		/* let's get the term for this tuple wrapped as an object */
		types::term::ref tuple_term = types::term_product(pk_obj,
				{get_tuple_term(args)});

		/* now, let's see if we already have a ctor for this tuple type, if not
		 * we'll need to create a data ctor for this unnamed tuple type */
		auto program_scope = scope->get_program_scope();

		std::pair<bound_var_t::ref, bound_type_t::ref> tuple = instantiate_tuple_ctor(
				status, 
				builder,
				scope,
				args,
				tuple_term->repr(),
				token.location,
				shared_from_this());

		if (!!status) {
			assert(get_function_return_type(status,
						builder, *shared_from_this(),
						scope, tuple.first->type)->get_term()->repr() == tuple_term->repr());

			/* now, let's call our unnamed tuple ctor and return that value */
			return create_callsite(status, builder, scope, shared_from_this(), tuple.first,
					tuple_term->repr(), token.location,
					vars);
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::or_expr::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    return type_check_binary_operator(status, builder, scope, lhs, rhs, *this);
}

bound_var_t::ref ast::and_expr::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    return type_check_binary_operator(status, builder, scope, lhs, rhs, *this);
}

bound_var_t::ref ast::dot_expr::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	bound_var_t::ref lhs_val = lhs->resolve_instantiation(status,
			builder, scope, nullptr, nullptr);

	int index = -1;
	types::type::ref member_type;

	if (get_obj_struct_name_info(lhs_val->type->type, rhs.text, index, member_type)) {
		/* get the type of the dimension being reference */
		bound_type_t::ref type = upsert_bound_type(status, builder,
				scope, member_type);

		llvm::Value *llvm_gep = builder.CreateInBoundsGEP(
				llvm_resolve_alloca(builder, lhs_val->llvm_value),
				{builder.getInt32(0), builder.getInt32(1), builder.getInt32(index)});

		llvm::Value *llvm_item = builder.CreateLoad(llvm_gep);
		return bound_var_t::create(
				INTERNAL_LOC(), string_format(".%s", rhs.text.c_str()),
				type, llvm_item, make_code_id(token));
	} else {
		user_error(status, *this, "%s has no dimension called " c_id("%s"),
				lhs_val->type->str().c_str(),
				rhs.text.c_str());
	}

    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::ineq_expr::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    return type_check_binary_operator(status, builder, scope, lhs, rhs, *this);
}

bound_var_t::ref ast::plus_expr::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    return type_check_binary_operator(status, builder, scope, lhs, rhs, *this);
}

bound_var_t::ref ast::function_defn::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *) const
{
	llvm::IRBuilderBase::InsertPointGuard ipg(builder);
	assert(!!status);

	/* function definitions are type checked at instantiation points. callsites
	 * are instantiation points.
	 *
	 * The main job of this function is to:
	 * 0. type check the function given the scope.
	 * 1. generate code for this function.
	 * 2. bind the function name to the generated code within the given scope.
	 * */
	indent_logger indent;
	log(log_info, "type checking %s in %s", token.str().c_str(), scope->get_name().c_str());

	/* see if we can get a monotype from the function declaration */
	bound_type_t::named_pairs args;
	bound_type_t::ref return_type;
	type_check_fully_bound_function_decl(status, builder, *decl, scope, args, return_type);

	if (!!status) {
		return instantiate_with_args_and_return_type(status, builder, scope,
				new_scope, args, return_type);
	} else {
		user_error(status, *this, "unable to get function declaration");
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::function_defn::instantiate_with_args_and_return_type(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		local_scope_t::ref *new_scope,
		bound_type_t::named_pairs args,
		bound_type_t::ref return_type) const
{
	llvm::IRBuilderBase::InsertPointGuard ipg(builder);
	assert(!!status);

	std::string function_name = token.text;

	assert(scope->get_llvm_module() != nullptr);

	auto function_term = get_function_term(args, return_type);
	bound_type_t::ref function_type = upsert_bound_type(status,
			builder, scope, function_term);

	if (!!status) {
		assert(function_type->llvm_type != nullptr);

		llvm::Type *llvm_type = function_type->llvm_type;
		if (llvm_type->isPointerTy()) {
			llvm_type = llvm_type->getPointerElementType();
		}
		log(log_info, "creating function %s with LLVM type %s",
				function_name.c_str(),
				llvm_print_type(*llvm_type).c_str());
		assert(llvm_type->isFunctionTy());

		llvm::Function *llvm_function = llvm::Function::Create(
				(llvm::FunctionType *)llvm_type,
				llvm::Function::ExternalLinkage, function_name,
				scope->get_llvm_module());

		llvm::BasicBlock *llvm_block = llvm::BasicBlock::Create(builder.getContext(), "entry", llvm_function);
		builder.SetInsertPoint(llvm_block);

		/* set up the mapping to this function for use in recursion */
		bound_var_t::ref function_var = bound_var_t::create(
				INTERNAL_LOC(), token.text, function_type, llvm_function,
				make_code_id(token));

		/* we should be able to check its block as a callsite. note that this
		 * code will also run for generics but only after the
		 * sbk_generic_substitution mechanism has run its course. */
		auto params_scope = make_param_list_scope(status, *decl, scope,
				function_var, args);

		/* now put this function declaration into the containing scope in case
		 * of indirect recursion */
		if (function_var->name.size() != 0) {
			/* inline function definitions are scoped to the virtual block in which
			 * they appear */
			if (auto local_scope = dyncast<local_scope_t>(scope)) {
				*new_scope = local_scope->new_local_scope(
						string_format("function-%s", function_name.c_str()));

				(*new_scope)->put_bound_variable(function_var->name, function_var);
			} else {
				module_scope_t::ref module_scope = dyncast<module_scope_t>(scope);

				if (module_scope == nullptr) {
					if (auto subst_scope = dyncast<generic_substitution_scope_t>(scope)) {
						module_scope = dyncast<module_scope_t>(subst_scope->get_parent_scope());
					}
				}

				if (module_scope != nullptr) {
					/* before recursing directly or indirectly, let's just add
					 * this function to the module scope we're in */
					module_scope->put_bound_variable(function_var->name, function_var);
					module_scope->mark_checked(shared_from_this());
				}
			}
		} else {
			user_error(status, *this, "function definitions need names");
		}

		/* keep track of whether this function returns */
		bool all_paths_return = false;
		params_scope->return_type_constraint = return_type;
		block->resolve_instantiation(status, builder, params_scope,
				nullptr, &all_paths_return);

		if (!!status) {
			debug_above(4, log(log_info, "module dump from %s\n%s", __PRETTY_FUNCTION__,
						llvm_print_module(*llvm_get_module(builder)).c_str()));

			if (all_paths_return) {
				return function_var;
			} else {
				/* not all control paths return */
				if (return_type->is_void()) {
					/* if this is a void let's give the user a break and insert
					 * a default void return */
					builder.CreateRetVoid();
					return function_var;
				} else {
					/* no breaks here, we don't know what to return */
					user_error(status, *this, "not all control paths return a value");
				}
			}

			llvm_verify_function(status, llvm_function);
		}
	}

    assert(!status);
    return nullptr;
}

status_t type_check_module_links(
        compiler &compiler,
        llvm::IRBuilder<> &builder,
        const ast::module &obj,
        scope_t::ref program_scope)
{
    status_t status;

    /* get module level scope variable */
	module_scope_t::ref scope = compiler.get_module_scope(obj.module_key);

	for (auto &link : obj.linked_modules) {
		bound_var_t::ref link_value = link->resolve_instantiation(status, builder, scope, nullptr, nullptr);
		// REVIEW: this is dumb
		assert_implies(!status, link_value == nullptr);
		assert_implies(!!status, link_value != nullptr);
	}

    for (auto &link : obj.linked_functions) {
        bound_var_t::ref link_value = link->resolve_instantiation(
                status, builder, scope, nullptr, nullptr);

        if (!!status) {
            if (link->link_as_name.text.size() != 0) {
                scope->put_bound_variable(link->link_as_name.text, link_value);
            } else {
                user_error(status, *link, "module level link definitions need names");
            }
        }
    }
    return status;
}

status_t type_check_module_types(
        compiler &compiler,
        llvm::IRBuilder<> &builder,
        const ast::module &obj,
        scope_t::ref program_scope)
{
	status_t final_status;

    /* get module level scope types */
    module_scope_t::ref module_scope = compiler.get_module_scope(obj.module_key);

    for (int i = 0; i < module_scope->unchecked_types_ordered.size(); ++i) {
		auto unchecked_type = module_scope->unchecked_types_ordered[i];
		auto node = unchecked_type->node;
		if (!module_scope->has_checked(node)) {
			assert(!dyncast<const ast::function_defn>(node));

			/* prevent recurring checks */
			log(log_info, "checking module level type %s", node->token.str().c_str());

			if (auto type_def = dyncast<const ast::type_def>(node)) {
				/* NB: this next line creates type definitions, regardless of
				 * their genericity.  type expressions will be added as macros
				 * in the type system.  this step is MUTATING the type
				 * environment of the module, and the program. */
				status_t status;
				type_def->resolve_instantiation(
						status, builder, module_scope, nullptr, nullptr);

				/* take note of whether this failed or not */
				final_status |= status;
			} else {
				assert(!"unhandled unchecked type node at module scope");
			}
		} else {
			log(log_info, "skipping %s because it's already been checked", node->token.str().c_str());
        }
    }

    return final_status;
}

status_t type_check_module_variables(
        compiler &compiler,
        llvm::IRBuilder<> &builder,
        const ast::module &obj,
        scope_t::ref program_scope)
{
	status_t final_status;

    /* get module level scope variable */
    module_scope_t::ref module_scope = compiler.get_module_scope(obj.module_key);

    for (int i = 0; i < module_scope->unchecked_vars_ordered.size(); ++i) {
		auto unchecked_var = module_scope->unchecked_vars_ordered[i];
		auto node = unchecked_var->node;

		if (!module_scope->has_checked(node)) {
			/* prevent recurring checks */
			log(log_info, "checking module level variable %s", node->token.str().c_str());
			if (auto function_defn = dyncast<const ast::function_defn>(node)) {
				// TODO: decide whether we need treatment here
				if (is_function_defn_generic(module_scope, *function_defn)) {
					/* this is a generic function, or we've already checked
					 * it so let's skip checking it */
					continue;
				}
			}

			if (auto stmt = dyncast<const ast::statement>(node)) {
				status_t status;
				bound_var_t::ref variable = stmt->resolve_instantiation(
						status, builder, module_scope, nullptr, nullptr);

				/* take note of whether this failed or not */
				final_status |= status;
			} else if (auto data_ctor = dyncast<const ast::data_ctor>(node)) {
				/* ignore until instantiation at a callsite */
			} else {
				assert(!"unhandled unchecked node at module scope");
			}
		} else {
			log(log_info, "skipping %s because it's already been checked", node->token.str().c_str());
		}
    }

	debug_above(3, log(log_info, "module after its own variable pass is:\n" c_ir("%s"),
				llvm_print_module(*module_scope->get_llvm_module()).c_str()));
    return final_status;
}

status_t type_check_program(
        llvm::IRBuilder<> &builder,
        const ast::program &obj,
        compiler &compiler)
{
    indent_logger indent;

    /* we track type-checking success or failure in this status value object */
    status_t status;

    ptr<scope_t> program_scope = compiler.get_program_scope();
    debug_above(10, log(log_info, "type_check_program program scope:\n%s", program_scope->str().c_str()));

    /* second pass is to resolve all module-level links */
    for (auto &module : obj.modules) {
        debug_above(3, log(log_info, "resolving links in %s", module->module_key.c_str()));

        status |= type_check_module_links(compiler, builder, *module, program_scope);
    }

    /* third pass is to resolve all module-level types */
    for (auto &module : obj.modules) {
        log(log_info, "resolving types in %s", module->module_key.c_str());

		status |= type_check_module_types(compiler, builder, *module, program_scope);
    }

    /* fourth pass is to resolve all module-level variables */
    for (auto &module : obj.modules) {
        log(log_info, "resolving variables in %s", module->module_key.c_str());

        status |= type_check_module_variables(compiler, builder, *module, program_scope);
    }
    return status;
}

bound_var_t::ref ast::type_def::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool * /*returns*/) const
{
	/* the goal of this function is to
	 * construct a type, and its requisite parts - not limited to type
	 * definition - such as ctors, accessors, etc, and instantiate those
	 * components into the eligible scopes.  the current type we're defining
	 * should provide a definition that is defined in terms of fully qualified
	 * names.  the type will eventually be able to be referenced by its
	 * name. types can be imported across module boundaries, and type
	 * definitions can be generic in declaration, but concrete in resolution.
	 * this function is the resolution step, so we assume all types referenced
	 * within are bound to some scope and have concrete definitions provided
	 * already. for those that have not been provided we will need to recurse
	 * instantiation. */

	/*  create a def_macro term and add it to the current type environment.
	 *  Use type_decl->name, and bind the type algebra expression to a lambda
	 *  of the type_decl->type_variables. */
	
	/* as a test, let's see if the preexisting term is already bound. */
	auto already_bound_type = scope->get_bound_type({type_decl->token.text});
	if (already_bound_type != nullptr) {
		log(log_warning, "found predefined bound type for %s -> %s",
			   	type_decl->token.str().c_str(),
				already_bound_type->str().c_str());
		
		// this is probably fine in practice, but maybe we should check whether
		// the already existing type was created in this scope
		dbg();
	}

	if (auto runnable_scope = dyncast<runnable_scope_t>(scope)) {
		assert(new_scope != nullptr);

		/* type definitions begin new scopes */
		local_scope_t::ref fresh_scope = runnable_scope->new_local_scope(
				string_format("type-%s", token.text.c_str()));

		/* update current scope for writing */
		scope = fresh_scope;

		/* have the caller update their current scope */
		*new_scope = fresh_scope;
	}

	// TODO: consider type namespacing here, or 
	auto type_term = type_algebra->instantiate_type(status, builder,
			type_decl->type_variables, scope);

	if (!!status) {
		scope->put_type_term(type_decl->token.text, type_term);
		return nullptr;
	}
	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::assignment::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    return type_check_binary_operator(status, builder, scope, lhs, rhs, *this);
}

bound_var_t::ref ast::break_flow::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    not_impl();
    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::continue_flow::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    not_impl();
    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::mod_assignment::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    not_impl();
    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::plus_assignment::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    not_impl();
    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::minus_assignment::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    not_impl();
    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::return_statement::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	/* obviously... */
	*returns = true;

	/* let's figure out if we have a return value, and what it's type is */
    bound_var_t::ref return_value;
    bound_type_t::ref return_type;

    if (expr) {
        /* if there is a return expression resolve it into a value */
        return_value = expr->resolve_instantiation(status, builder, scope, nullptr, nullptr);
        if (!!status) {
            /* get the type suggested by this return value */
            return_type = return_value->type;
        }
    } else {
        /* we have an empty return, let's just use void */
		// TODO: figure out how to find void
		assert(false);
        return_type = nullptr ; // scope->get_program_scope()->get_bound_type({"void"});
    }

    if (!!status) {
        runnable_scope_t::ref runnable_scope = dyncast<runnable_scope_t>(scope);
        assert(runnable_scope != nullptr);

		/* make sure this return type makes sense, or keep track of it if we
		 * didn't yet know the return type for this function */
		runnable_scope->check_or_update_return_type_constraint(status,
				shared_from_this(), return_type);

		if (return_value != nullptr) {
			builder.CreateRet(llvm_resolve_alloca(builder, return_value->llvm_value));
		} else {
			assert(types::is_type_id(return_type->type, "void"));
			builder.CreateRetVoid();
		}

        return return_value;
    }
    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::times_assignment::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    not_impl();
    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::divide_assignment::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    not_impl();
    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::block::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns_) const
{
	/* it's important that we keep track of returns */
	bool placeholder_returns = false;
	bool *returns = returns_;
	if (returns == nullptr) {
		returns = &placeholder_returns;
	}

    scope_t::ref current_scope = scope;

    assert(builder.GetInsertBlock() != nullptr);

	for (auto &statement : statements) {
		if (*returns) {
			user_error(status, *statement, "this statement will never run");
		}

		local_scope_t::ref next_scope;

		log(log_info, "type checking statement %s", statement->str().c_str());

		statement->resolve_instantiation(status, builder, current_scope,
				&next_scope, returns);

		if (!!status) {
			if (next_scope != nullptr) {
				/* the statement just executed wants to create a new nested scope.
				 * let's allow this by just keeping track of the current scope. */
				current_scope = next_scope;
				next_scope = nullptr;
				debug_above(8, log(log_info, "got a new scope %s", current_scope->str().c_str()));
			}
		}
    }

    /* blocks don't really have values */
    return nullptr;
}

bound_var_t::ref ast::while_block::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    not_impl();
    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::if_block::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	/* if scope allows us to set up new variables inside if conditions */
	local_scope_t::ref if_scope;

	bool if_block_returns = false, else_block_returns = false;

	if (condition != nullptr) {
		assert(token.text == "if" || token.text == "elif");

		/* evaluate the condition for branching */
		bound_var_t::ref bool_condition_value;
		bound_var_t::ref condition_value = condition->resolve_instantiation(
				status, builder, scope, &if_scope, nullptr);

		if (!!status) {
			llvm::Value *llvm_condition_value = nullptr;
			if (condition_value->is_int()) {
				llvm_condition_value = llvm_resolve_alloca(builder, condition_value->llvm_value);
				assert(llvm_condition_value->getType()->isIntegerTy());
			} else {
				log(log_info, "if condition %s appears to not be a bool or int, attempting to resolve a " c_var("bool") " override",
						condition_value->str().c_str());

				/* convert condition to an integer */
				bound_var_t::ref bool_fn = get_callable(
						status, builder, scope, "bool", condition,
					   	get_args_term({condition_value->type}));

				if (!!status) {
					/* we've found a bool function that will take our condition as input */
				   	assert(bool_fn != nullptr);

					log(log_info, "generating a call to " c_var("bool") "(%s) for if condition evaluation (term %s)",
						   	condition->str().c_str(), bool_fn->type->str().c_str());

					/* let's call this bool function */
					llvm_condition_value = llvm_create_call_inst(
							status, builder, *condition, bool_fn,
							{condition_value->llvm_value});

					assert(llvm_condition_value->getType()->isIntegerTy());
				} else {
					// TODO: maybe want a better explanation for why we're
					// trying to call bool.
				}
			}

			if (!!status && llvm_condition_value != nullptr) {
				/* test that the if statement doesn't return */
				llvm::Function *llvm_function_current = llvm_get_function(builder);

				/* generate some new blocks */
				llvm::BasicBlock *then_bb = llvm::BasicBlock::Create(builder.getContext(), "then", llvm_function_current);
				llvm::BasicBlock *merge_bb = nullptr;

				/* we have to keep track of whether we need a merge block
				 * because our nested branches could all return */
				bool insert_merge_bb = false;

				if (else_ != nullptr) {
					/* we've got an else block, so let's create an "else" basic block. */
					llvm::BasicBlock *else_bb = llvm::BasicBlock::Create(builder.getContext(), "else", llvm_function_current);

					/* put the merge block after the else block */
					merge_bb = llvm::BasicBlock::Create(builder.getContext(), "ifcont");

					/* create the actual branch instruction */
					llvm_create_if_branch(builder, llvm_condition_value, then_bb, else_bb);

					builder.SetInsertPoint(else_bb);
					else_->resolve_instantiation(status, builder, scope, nullptr, &else_block_returns);

					if (!else_block_returns) {
						/* keep track of the fact that we have to have a
						 * merged block to land in after the else block */
						insert_merge_bb = true;

						/* go ahead and jump there */
						builder.CreateBr(merge_bb);
					}
				} else {
					/* since there is no else block it cannot return */
					else_block_returns = false;

					/* keep track of the fact that we have to have a merged
					 * block to land in after the if block */
					insert_merge_bb = true;

					/* put the merge block after the if block */
					merge_bb = llvm::BasicBlock::Create(builder.getContext(), "ifcont");

					/* we don't have an else block, so we can just continue on */
					llvm_create_if_branch(builder, llvm_condition_value, then_bb, merge_bb);
				}

				if (!!status) {
					/* let's generate code for the "then" block */
					builder.SetInsertPoint(then_bb);
					block->resolve_instantiation(status, builder, if_scope ? if_scope : scope, nullptr, &if_block_returns);
					if (!!status) {
						if (!if_block_returns) {
							insert_merge_bb = true;
							builder.CreateBr(merge_bb);
							builder.SetInsertPoint(merge_bb);
						}
						
						if (insert_merge_bb) {
							/* we know we'll need to fall through to the merge
							 * block, let's add it to the end of the function
							 * and let's set it as the next insert point. */
							llvm_function_current->getBasicBlockList().push_back(merge_bb);
							builder.SetInsertPoint(merge_bb);
						}

						/* track whether the branches return */
						*returns |= (if_block_returns && else_block_returns);

						assert(!!status);
						return nullptr;
					}
				}
			}
		}
	} else {
		/* this should never happen */
		not_impl();
	}

	assert(!status);
    return nullptr;
}

bound_var_t::ref ast::var_decl::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    runnable_scope_t::ref runnable_scope = dyncast<runnable_scope_t>(scope);
    assert(runnable_scope);

    /* variable declarations begin new scopes */
    local_scope_t::ref fresh_scope = runnable_scope->new_local_scope(
            string_format("variable-%s", token.text.c_str()));

    scope = fresh_scope;

    /* check to make sure this var decl is sound */
    bound_var_t::ref var_decl_variable = type_check_bound_var_decl(
            status, builder, *this, fresh_scope);

    if (!!status) {
        /* on our way out, stash the variable in the current scope */
        fresh_scope->put_bound_variable(var_decl_variable->name, var_decl_variable);
        *new_scope = fresh_scope;
        return var_decl_variable;
    }

    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::pass_flow::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    return nullptr;
}

bound_var_t::ref ast::times_expr::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    return type_check_binary_operator(status, builder, scope, lhs, rhs, *this);
}

bound_var_t::ref ast::prefix_expr::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    /* first solve the right hand side */
    bound_var_t::ref rhs_var = rhs->resolve_instantiation(status, builder, scope, nullptr, nullptr);

    if (!!status) {
        return call_program_function(status, builder, scope, token.text,
                shared_from_this(), {rhs_var});
    }
    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::literal_expr::resolve_instantiation(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    scope_t::ref program_scope = scope->get_program_scope();

    switch (token.tk) {
    case tk_integer:
        {
            int64_t value = atoll(token.text.c_str());
            bound_type_t::ref type = program_scope->get_bound_type({"int"});
            if (!!status) {
                return bound_var_t::create(
						INTERNAL_LOC(), "temp_int_literal", type,
						llvm_create_int(builder, value), make_code_id(token));
            }
        }
		break;
    case tk_true:
    case tk_false:
        {
            assert(token.text == "true" || token.text == "false");
            return scope->get_program_scope()->get_singleton(token.text);
        }
		break;
    case tk_string:
        {
            std::string value = unescape_json_quotes(token.text);
            bound_type_t::ref type = program_scope->get_bound_type({"str"});
            if (!!status) {
                return bound_var_t::create(INTERNAL_LOC(), "temp_str_literal", 
                        type, llvm_create_global_string(builder, value), make_code_id(token));
            }
        }
		break;
    case tk_float:
        {
            float value = atof(token.text.c_str());
            bound_type_t::ref type = program_scope->get_bound_type({"float"});
            if (!!status) {
                return bound_var_t::create(INTERNAL_LOC(), "temp_float_literal", 
                        type, llvm_create_float(builder, value), make_code_id(token));
            }
        }
		break;
    default:
        assert(false);
    };

    assert(!status);
    return nullptr;
}

atom ast::binary_expr::get_function_name() const {
	/* this implements the function name lookup for function overloads */
	return token.text;
}

bound_var_t::ref ast::reference_expr::resolve_overrides(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		const ptr<const ast::item> &callsite,
		const bound_type_t::refs &args) const
{
	indent_logger indent;
	log(log_info, "reference_expr::resolve_overrides for %s", callsite->str().c_str());

	/* ok, we know we've got some variable here */
	return get_callable(status, builder, scope, token.text, shared_from_this(),
			get_args_term(args));
}
