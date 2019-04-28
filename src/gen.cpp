#include "gen.h"

#include "ast.h"
#include "logger.h"
#include "ptr.h"
#include "typed_id.h"
#include "types.h"
#include "user_error.h"

#define assert_not_impl()                                                      \
  do {                                                                         \
    std::cout << llvm_print_module(*llvm_get_module(builder)) << std::endl;    \
    assert(false);                                                             \
  } while (0)

namespace gen {

struct loop_guard_t {
  llvm::BasicBlock *old_break_to_block;
  llvm::BasicBlock *old_continue_to_block;
  llvm::BasicBlock **break_to_block;
  llvm::BasicBlock **continue_to_block;

  loop_guard_t(llvm::BasicBlock *new_break_to_block,
               llvm::BasicBlock *new_continue_to_block,
               llvm::BasicBlock **break_to_block_,
               llvm::BasicBlock **continue_to_block_)
      : old_break_to_block(*break_to_block_),
        old_continue_to_block(*continue_to_block_),
        break_to_block(break_to_block_), continue_to_block(continue_to_block_) {
    *break_to_block = new_break_to_block;
    *continue_to_block = new_continue_to_block;
  }

  ~loop_guard_t() {
    *break_to_block = old_break_to_block;
    *continue_to_block = old_continue_to_block;
  }
};

struct free_vars_t {
  std::set<typed_id_t> globals;
  std::set<typed_id_t> typed_ids;
  int count() const {
    return typed_ids.size();
  }
  void add(identifier_t id, types::type_t::ref type) {
    debug_above(5, log("adding free var %s", id.str().c_str()));
    assert(type != nullptr);
    typed_ids.insert({id, type});
  }
  bool contains(identifier_t id, types::type_t::ref type) {
    return in(typed_id_t{id, type}, typed_ids);
  }
  std::string str() const {
    return string_format("{%s}", join(typed_ids, ", ").c_str());
  }
};

types::type_t::ref get_nth_type_in_arrow(types::type_t::ref arrow_type, int n) {
  types::type_t::refs terms;
  unfold_binops_rassoc(ARROW_TYPE_OPERATOR, arrow_type, terms);
  assert(n < terms.size());
  return terms[n];
}

void get_free_vars(const bitter::expr_t *expr,
                   const tracked_types_t &typing,
                   const std::unordered_set<std::string> &globals,
                   const std::unordered_set<std::string> &locals,
                   free_vars_t &free_vars) {
  if (auto literal = dcast<const bitter::literal_t *>(expr)) {
  } else if (auto static_print = dcast<const bitter::static_print_t *>(expr)) {
  } else if (auto var = dcast<const bitter::var_t *>(expr)) {
    if (!in(var->id.name, globals) && !in(var->id.name, locals)) {
      /* we need to capture this variable in order to put it into our closure */
      free_vars.add(var->id, get(typing, expr, {}));
    }
  } else if (auto lambda = dcast<const bitter::lambda_t *>(expr)) {
    debug_above(5, log("checking lambda %s", lambda->str().c_str()));
    auto lambda_type = typing.at(lambda);
    bool already_has_lambda_var = free_vars.contains(
        lambda->var, get_nth_type_in_arrow(lambda_type, 0));
    auto new_globals = globals;
    new_globals.insert(lambda->var.name);
    get_free_vars(lambda->body, typing, new_globals, {}, free_vars);
    /* we should never be adding a bound variable name to the closure if it is
     * being overwritten at a lower scope prior to capture */
    assert_implies(!already_has_lambda_var,
                   !free_vars.contains(lambda->var,
                                       get_nth_type_in_arrow(lambda_type, 0)));
  } else if (auto application = dcast<const bitter::application_t *>(expr)) {
    get_free_vars(application->a, typing, globals, locals, free_vars);
    get_free_vars(application->b, typing, globals, locals, free_vars);
  } else if (auto let = dcast<const bitter::let_t *>(expr)) {
    // TODO: allow let-rec
    get_free_vars(let->value, typing, globals, locals, free_vars);
    auto new_locals = locals;
    new_locals.insert(let->var.name);
    get_free_vars(let->body, typing, globals, new_locals, free_vars);
  } else if (auto fix = dcast<const bitter::fix_t *>(expr)) {
    get_free_vars(fix->f, typing, globals, locals, free_vars);
  } else if (auto condition = dcast<const bitter::conditional_t *>(expr)) {
    get_free_vars(condition->cond, typing, globals, locals, free_vars);
    get_free_vars(condition->truthy, typing, globals, locals, free_vars);
    get_free_vars(condition->falsey, typing, globals, locals, free_vars);
  } else if (auto break_ = dcast<const bitter::break_t *>(expr)) {
  } else if (auto while_ = dcast<const bitter::while_t *>(expr)) {
    get_free_vars(while_->condition, typing, globals, locals, free_vars);
    get_free_vars(while_->block, typing, globals, locals, free_vars);
  } else if (auto block = dcast<const bitter::block_t *>(expr)) {
    for (auto statement : block->statements) {
      get_free_vars(statement, typing, globals, locals, free_vars);
    }
  } else if (auto return_ = dcast<const bitter::return_statement_t *>(expr)) {
    get_free_vars(return_->value, typing, globals, locals, free_vars);
  } else if (auto tuple = dcast<const bitter::tuple_t *>(expr)) {
    for (auto dim : tuple->dims) {
      get_free_vars(dim, typing, globals, locals, free_vars);
    }
  } else if (auto tuple_deref = dcast<const bitter::tuple_deref_t *>(expr)) {
    get_free_vars(tuple_deref->expr, typing, globals, locals, free_vars);
  } else if (auto as = dcast<const bitter::as_t *>(expr)) {
    get_free_vars(as->expr, typing, globals, locals, free_vars);
  } else if (auto sizeof_ = dcast<const bitter::sizeof_t *>(expr)) {
  } else if (auto builtin = dcast<const bitter::builtin_t *>(expr)) {
    for (auto expr : builtin->exprs) {
      get_free_vars(expr, typing, globals, locals, free_vars);
    }
  } else if (auto match = dcast<const bitter::match_t *>(expr)) {
    /* by this point, all match expressions should have been transformed into
     * conditionals */
    assert(false);
#if 0
    get_free_vars(match->scrutinee, typing, globals, locals, free_vars);
    for (auto pattern_block : match->pattern_blocks) {
      auto new_bindings = bindings;
      pattern_block->predicate->get_bound_vars(new_bindings);
      get_free_vars(pattern_block->result, typing, new_bindings, free_vars);
    }
#endif
  } else {
    assert(false);
  }
  debug_above(2, log("get_free_vars(..., {%s}, {%s}, %s)", expr->str().c_str(),
                     // join(globals, ", ").c_str(),
                     join(locals, ", ").c_str(), free_vars.str().c_str()));
}

llvm::Value *maybe_get_env_var(const gen_env_t &gen_env,
                               std::string name,
                               types::type_t::ref type) {
  return maybe_get_env_var(gen_env, make_iid(name), type);
}

llvm::Value *maybe_get_env_var(const gen_env_t &gen_env,
                               identifier_t id,
                               types::type_t::ref type) {
  auto iter_id = gen_env.find(id.name);
  if (iter_id != gen_env.end()) {
    type = types::unitize(type);
    auto iter_type = iter_id->second.find(type);
    if (iter_type != iter_id->second.end()) {
      resolver_t *resolver_ptr = iter_type->second.get();
      assert(resolver_ptr != nullptr);

      /* since this resolver exists, we can assume that we should be able to ask
       * for its value. */
      return resolver_ptr->resolve();
    } else {
      /* no symbol goes by that type in these parts, mister */
      return nullptr;
    }
  } else {
    /* we don't know anything about this symbol name */
    return nullptr;
  }
}

llvm::Value *get_env_var(llvm::IRBuilder<> &builder,
                         const gen_env_t &gen_env,
                         identifier_t id,
                         types::type_t::ref type) {
  llvm::IRBuilderBase::InsertPointGuard ipg(builder);
  llvm::Value *llvm_value = maybe_get_env_var(gen_env, id, type);
  if (llvm_value == nullptr) {
    type = types::unitize(type);
    auto error = user_error(id.location, "we need a definition for %s :: %s",
                            id.str().c_str(), type->str().c_str());
    for (auto pair : gen_env) {
      for (auto &overload : pair.second) {
        error.add_info(id.location, "%s :: %s = %s", pair.first.c_str(),
                       overload.first->str().c_str(),
                       overload.second->str().c_str());
      }
    }
    throw error;
  }
  debug_above(5, log("get_env_var(%s, %s) -> %s", id.str().c_str(),
                     type->str().c_str(), llvm_print(llvm_value).c_str()));
  if (auto arg = llvm::dyn_cast<llvm::Argument>(llvm_value)) {
    debug_above(5, log("%s is an argument %s to %s", id.str().c_str(),
                       llvm_print(llvm_value).c_str(),
                       arg->getParent()->getName().str().c_str()));
    auto cur_func = llvm_get_function(builder);
    debug_above(5, log("the current function is %s",
                       cur_func->getName().str().c_str()));
    assert(arg->getParent() == llvm_get_function(builder));
  }
  assert(llvm_value != nullptr);
  return llvm_value;
}

void set_env_var(gen_env_t &gen_env,
                 std::string name,
                 types::type_t::ref type,
                 llvm::Value *llvm_value,
                 bool allow_shadowing) {
  debug_above(5, log("gen::set_env_var(0x%08llx, %s, %s, %s)",
                     (unsigned long long)(&gen_env), name.c_str(),
                     type->str().c_str(), llvm_print(llvm_value).c_str(),
                     boolstr(allow_shadowing)));
  assert(name.size() != 0);
  type = types::unitize(type);
  llvm::Value *existing_value = maybe_get_env_var(gen_env, name, type);
  if (existing_value == nullptr) {
    debug_above(4, log("found no value in the gen_env 0x%08llx for %s :: %s, "
                       "adding a new value",
                       (unsigned long long)&gen_env, name.c_str(),
                       type->str().c_str()));
    gen_env[name].insert({type, strict_resolver(llvm_value)});
  } else if (allow_shadowing) {
    debug_above(
        4, log("overwriting any existing value in the gen_env for %s :: %s",
               name.c_str(), type->str().c_str()));
    gen_env[name].insert({type, strict_resolver(llvm_value)});
  } else {
    /* what now? probably shouldn't have happened */
    assert(false);
  }
  for (auto pair : gen_env[name]) {
    debug_above(5, log("gen_env[%s] .= {%s, %s}", name.c_str(),
                       type->str().c_str(), llvm_print(llvm_value).c_str()));
  }
  assert(maybe_get_env_var(gen_env, name, type) != nullptr);
}

llvm::Value *gen_builtin(llvm::IRBuilder<> &builder,
                         const std::string &name,
                         const std::vector<llvm::Value *> &params) {
  debug_above(7, log("lowering builtin %s(%s)...", name.c_str(),
                     join_with(params, ", ", [](llvm::Value *lv) {
                       return llvm_print(lv);
                     }).c_str()));

  if (name == "__builtin_word_size") {
    /* scheme({}, {}, Int) */
  } else if (name == "__builtin_min_int") {
    /* scheme({}, {}, Int) */
  } else if (name == "__builtin_max_int") {
    /* scheme({}, {}, Int) */
  } else if (name == "__builtin_multiply_int") {
    /* scheme({}, {}, type_arrows({Int, Int, Int})) */
  } else if (name == "__builtin_divide_int") {
    /* scheme({}, {}, type_arrows({Int, Int, Int})) */
  } else if (name == "__builtin_subtract_int") {
    /* scheme({}, {}, type_arrows({Int, Int, Int})) */
  } else if (name == "__builtin_add_int") {
    /* scheme({}, {}, type_arrows({Int, Int, Int})) */
    return builder.CreateAdd(params[0], params[1]);
  } else if (name == "__builtin_negate_int") {
    /* scheme({}, {}, type_arrows({Int, Int})) */
    return builder.CreateNeg(params[0]);
  } else if (name == "__builtin_abs_int") {
    /* scheme({}, {}, type_arrows({Int, Int})) */
  } else if (name == "__builtin_multiply_float") {
    /* scheme({}, {}, type_arrows({Float, Float, Float})) */
  } else if (name == "__builtin_divide_float") {
    /* scheme({}, {}, type_arrows({Float, Float, Float})) */
  } else if (name == "__builtin_subtract_float") {
    /* scheme({}, {}, type_arrows({Float, Float, Float})) */
  } else if (name == "__builtin_add_float") {
    /* scheme({}, {}, type_arrows({Float, Float, Float})) */
  } else if (name == "__builtin_abs_float") {
    /* scheme({}, {}, type_arrows({Float, Float})) */
  } else if (name == "__builtin_int_to_float") {
    /* scheme({}, {}, type_arrows({Int, Float})) */
  } else if (name == "__builtin_negate_float") {
    /* scheme({}, {}, type_arrows({Float, Float})) */
  } else if (name == "__builtin_add_ptr") {
    /* scheme({"a"}, {}, type_arrows({tp_a, Int, tp_a})) */
    return builder.CreateGEP(params[0], std::vector<llvm::Value *>{params[1]});
  } else if (name == "__builtin_ptr_eq") {
    /* scheme({"a"}, {}, type_arrows({tp_a, tp_a, Bool})) */
    assert(false);
  } else if (name == "__builtin_ptr_ne") {
    /* scheme({"a"}, {}, type_arrows({tp_a, tp_a, Bool})) */
    assert(false);
  } else if (name == "__builtin_ptr_load") {
    /* scheme({"a"}, {}, type_arrows({tp_a, tv_a})) */
    return builder.CreateLoad(params[0]);
  } else if (name == "__builtin_get_dim") {
    /* scheme({"a", "b"}, {}, type_arrows({tv_a, Int, tv_b})) */
  } else if (name == "__builtin_get_ctor_id") {
    /* scheme({"a"}, {}, type_arrows({tv_a, Int})) */
  } else if (name == "__builtin_int_eq") {
    /* scheme({}, {}, type_arrows({Int, Int, Bool})) */
  } else if (name == "__builtin_int_ne") {
    /* scheme({}, {}, type_arrows({Int, Int, Bool})) */
  } else if (name == "__builtin_int_lt") {
    /* scheme({}, {}, type_arrows({Int, Int, Bool})) */
  } else if (name == "__builtin_int_lte") {
    /* scheme({}, {}, type_arrows({Int, Int, Bool})) */
  } else if (name == "__builtin_int_gt") {
    /* scheme({}, {}, type_arrows({Int, Int, Bool})) */
  } else if (name == "__builtin_int_gte") {
    /* scheme({}, {}, type_arrows({Int, Int, Bool})) */
  } else if (name == "__builtin_float_eq") {
    /* scheme({}, {}, type_arrows({Float, Float, Bool})) */
  } else if (name == "__builtin_float_ne") {
    /* scheme({}, {}, type_arrows({Float, Float, Bool})) */
  } else if (name == "__builtin_float_lt") {
    /* scheme({}, {}, type_arrows({Float, Float, Bool})) */
  } else if (name == "__builtin_float_lte") {
    /* scheme({}, {}, type_arrows({Float, Float, Bool})) */
  } else if (name == "__builtin_float_gt") {
    /* scheme({}, {}, type_arrows({Float, Float, Bool})) */
  } else if (name == "__builtin_float_gte") {
    /* scheme({}, {}, type_arrows({Float, Float, Bool})) */
  } else if (name == "__builtin_print") {
    /* scheme({}, {}, type_arrows({*Char, type_unit(INTERNAL_LOC())})) */
    auto llvm_module = llvm_get_module(builder);
    llvm::Type *write_terms[] = {builder.getInt8Ty()->getPointerTo()};

    assert(params.size() == 1);

    // libc dependency
    auto llvm_write_func_decl = llvm::cast<llvm::Function>(
        llvm_module->getOrInsertFunction(
            "zion_puts",
            llvm::FunctionType::get(builder.getInt64Ty(),
                                    llvm::ArrayRef<llvm::Type *>(write_terms),
                                    false /*isVarArg*/)));
    return builder.CreateIntToPtr(
        builder.CreateCall(llvm_write_func_decl, params),
        builder.getInt8Ty()->getPointerTo());
  } else if (name == "__builtin_print_int") {
    /* scheme({}, {}, type_arrows({*Char, type_unit(INTERNAL_LOC())})) */
    auto llvm_module = llvm_get_module(builder);
    llvm::Type *print_int_terms[] = {builder.getInt64Ty()};

    assert(params.size() == 1);

    // libc dependency
    auto llvm_print_int_func_decl = llvm::cast<llvm::Function>(
        llvm_module->getOrInsertFunction(
            "zion_print_int64",
            llvm::FunctionType::get(
                builder.getInt8Ty()->getPointerTo(),
                llvm::ArrayRef<llvm::Type *>(print_int_terms),
                false /*isVarArg*/)));
    return builder.CreateCall(llvm_print_int_func_decl, params);
  } else if (name == "__builtin_itoa") {
    /* scheme({}, {}, type_arrows({Int, *Char})) */
    auto llvm_module = llvm_get_module(builder);
    llvm::Type *itoa_terms[] = {builder.getInt64Ty()};

    assert(params.size() == 1);

    auto llvm_itoa_func_decl = llvm::cast<llvm::Function>(
        llvm_module->getOrInsertFunction(
            "zion_itoa",
            llvm::FunctionType::get(builder.getInt8Ty()->getPointerTo(),
                                    llvm::ArrayRef<llvm::Type *>(itoa_terms),
                                    false /*isVarArg*/)));
    return builder.CreateCall(llvm_itoa_func_decl, params);
  } else if (name == "__builtin_strlen") {
    /* scheme({}, {}, type_arrows({*Char, Int})) */
    auto llvm_module = llvm_get_module(builder);
    llvm::Type *param_types[] = {builder.getInt8Ty()->getPointerTo()};

    assert(params.size() == 1);

    auto ffi_function = llvm::cast<llvm::Function>(
        llvm_module->getOrInsertFunction(
            "zion_strlen",
            llvm::FunctionType::get(builder.getInt64Ty(),
                                    llvm::ArrayRef<llvm::Type *>(param_types),
                                    false /*isVarArg*/)));
    return builder.CreateCall(ffi_function, params);
  } else if (name == "__builtin_exit") {
    /* scheme({}, {}, type_arrows({Int, type_bottom()})) */
  } else if (name == "__builtin_calloc") {
    /* scheme({"a"}, {}, type_arrows({Int, tp_a})) */
  } else if (name == "__builtin_store_ref") {
    /* scheme({"a"}, {}, type_arrows({
     * type_operator(type_id(make_iid(REF_TYPE_OPERATOR)), tv_a), tv_a,
     * type_unit(INTERNAL_LOC())})) */
  } else if (name == "__builtin_store_ptr") {
    /* scheme({"a"}, {}, type_arrows({
     * type_operator(type_id(make_iid(PTR_TYPE_OPERATOR)), tv_a), tv_a,
     * type_unit(INTERNAL_LOC())})) */
  } else if (name == "__builtin_hello" || name == "__builtin_goodbye") {
    /* scheme({}, {}, Unit) */
    auto llvm_module = llvm_get_module(builder);
    llvm::Type *write_terms[] = {builder.getInt8Ty()->getPointerTo()};

    std::vector<llvm::Value *> params = {llvm_create_global_string_constant(
        builder, *llvm_module,
        name == "__builtin_hello" ? "hello" : "goodbye")};

    // libc dependency
    auto llvm_write_func_decl = llvm::cast<llvm::Function>(
        llvm_module->getOrInsertFunction(
            "zion_puts",
            llvm::FunctionType::get(builder.getInt64Ty(),
                                    llvm::ArrayRef<llvm::Type *>(write_terms),
                                    false /*isVarArg*/)));
    return builder.CreateIntToPtr(
        builder.CreateCall(llvm_write_func_decl, params),
        builder.getInt8Ty()->getPointerTo());
  }

  log("Need an impl for " c_id("%s"), name.c_str());
  panic("quitting...");
  return nullptr;
}

#if 0
bool is_terminator(instruction_t::ref inst) {
  if (dyncast<return_t>(inst)) {
    return true;
  } else if (dyncast<goto_t>(inst)) {
    return true;
  } else if (dyncast<cond_branch_t>(inst)) {
    return true;
  } else {
    return false;
  }
}

bool has_terminator(instructions_t &instructions) {
  if (instructions.size() == 0) {
    return false;
  } else {
    return is_terminator(instructions.back());
  }
}

void builder_t::ensure_terminator(std::function<void(builder_t &)> callback) {
  assert(block != nullptr);
  if (!has_terminator(block->instructions)) {
    callback(*this);
  }
}

function_t::ref builder_t::create_function(std::string name,
                                           identifiers_t param_ids,
                                           location_t location,
                                           types::type_t::ref type) {
  if (name == "" && function) {
    name = function->name + bitter::fresh();
  }
  auto function = std::make_shared<function_t>(module, name, location, type);
  types::type_t::refs terms;
  unfold_binops_rassoc(ARROW_TYPE_OPERATOR, type, terms);
  assert(terms.size() > param_ids.size());
  for (int i = 0; i < param_ids.size(); ++i) {
    auto param_type = terms[0];
    terms.erase(terms.begin());

    debug_above(8, log("creating argument %s :: %s for %s",
                       param_ids[i].str().c_str(), param_type->str().c_str(),
                       function->get_name().c_str()));
    function->args.push_back(
        std::make_shared<argument_t>(param_ids[i], param_type, i, function));
  }

  set_env_var(module->gen_env, function->get_name(), function);
  return function;
}
#endif

void gen_lambda(std::string name,
                llvm::IRBuilder<> &builder,
                llvm::Module *llvm_module,
                const bitter::lambda_t *lambda,
                types::type_t::ref type,
                const tracked_types_t &typing,
                const gen_env_t &gen_env_globals,
                const gen_env_t &gen_env_locals,
                const std::unordered_set<std::string> &globals,
                publisher_t *publisher) {
  if (name == "") {
    name = "__anonymous";
  }

  INDENT(2, string_format("gen_lambda(%s, ..., %s, %s, ...)", name.c_str(),
                          lambda->str().c_str(), type->str().c_str()));

  /* see if we need to lift any free variables into a closure */
  free_vars_t free_vars;
  get_free_vars(lambda, typing, globals, {}, free_vars);

  types::type_t::refs type_terms;
  unfold_binops_rassoc(ARROW_TYPE_OPERATOR, type, type_terms);

  llvm::FunctionType *llvm_function_type = get_llvm_arrow_function_type(
      builder, type_terms);

  llvm::Type *llvm_return_type = llvm_function_type->getReturnType();
  llvm::ArrayRef<llvm::Type *> llvm_param_types = llvm_function_type->params();

  llvm::Function *llvm_function = llvm::Function::Create(
      llvm_function_type, llvm::Function::ExternalLinkage, name,
      llvm_module != nullptr ? llvm_module : llvm_get_module(builder));
  llvm_function->setDoesNotThrow();

  llvm::BasicBlock *block = llvm::BasicBlock::Create(builder.getContext(),
                                                     "entry", llvm_function);
  std::vector<llvm::Value *> llvm_dims;
  types::type_t::refs dim_types;

  /* the closure includes a reference to its code so that it can be run */
  llvm_dims.push_back(llvm_function);

  // CAPTURE
  /* this lambda requires closure over some variables from our environment,
   * and as such requires that we add code to capture the free_vars and place
   * them in our nested environment, but pointing to the closure, not to the
   * outer environment. */
  debug_above(8, log("for %s aka %s we need closure by value of %s",
                     llvm_function->getName().str().c_str(), name.c_str(),
                     free_vars.str().c_str()));

  for (auto typed_id : free_vars.typed_ids) {
    /* add a copy of each captured variable. If get_env_var fails here, then
     * it means that get_free_vars is talking about a variable that just
     * doesn't exist yet, and thus will need to be captured by a nested
     * closure. */
    llvm_dims.push_back(
        get_env_var(builder, gen_env_locals, typed_id.id, typed_id.type));
    dim_types.push_back(typed_id.type);
  }

  /* actually copy the available free variables into the closure. if a
   * variable does not exist at this scope, then we rely on a sub-scope that
   * declares that variable to add it to the eventual node that does require
   * the closure.
   *
   * (let f (lambda x (lambda y (lambda z y))))
   *
   * In the above example, the free vars at (lambda x... include y in the
   * innermost (lambda z y), however at this moment, y is not even declared.
   *
   * */
  llvm::StructType *llvm_closure_type = llvm::StructType::get(
      llvm_function->getType(), builder.getInt8Ty()->getPointerTo());
  auto _llvm_closure_type = get_llvm_closure_type(builder, type_terms);
  debug_above(
      5, log("llvm_closure_type = %s", llvm_print(llvm_closure_type).c_str()));
  debug_above(5, log("_llvm_closure_type = %s",
                     llvm_print(_llvm_closure_type).c_str()));
  assert(llvm_closure_type->getPointerTo() == _llvm_closure_type);

  debug_above(5, log("llvm_dims count is %d", int(llvm_dims.size())));

  llvm::Value *opaque_closure = nullptr;
  llvm::Value *closure = nullptr;
  if (llvm_dims.size() == 1 && llvm_dims[0] == llvm_function) {
    opaque_closure = llvm_get_global(
        llvm_module, name + ".closure",
        llvm::ConstantStruct::get(
            llvm_closure_type,
            std::vector<llvm::Constant *>(
                {llvm_function, llvm::Constant::getNullValue(
                                    builder.getInt8Ty()->getPointerTo())})),
        true /*is_constant*/);
  } else {
    closure = llvm_tuple_alloc(builder, llvm_dims);
    opaque_closure = builder.CreateBitCast(closure,
                                           llvm_closure_type->getPointerTo());
  }
  assert(opaque_closure->getType() == llvm_closure_type->getPointerTo());

  /* we should always be returning the same type, and it should be the closure
   * type */
  debug_above(5, log("created closure %s",
                     llvm_print(closure ? closure : opaque_closure).c_str()));
  debug_above(5, log("%s == llvm_closure_type->getPointerTo()",
                     llvm_print(llvm_closure_type->getPointerTo()).c_str()));

  /* we have a closure which is usable now in this scope */
  if (publisher != nullptr) {
    publisher->publish(opaque_closure);
  }

  // BLOCK
  {
    llvm::IRBuilderBase::InsertPointGuard ipg(builder);
    builder.SetInsertPoint(block);

    /* put the param in scope */
    gen_env_t new_env_locals;
    if (name != "") {
      /* inject the closure itself so that it can self refer */
      // set_env_var(new_env, name, type, opaque_closure, true
      // /*allow_shadowing*/);
    }
    set_env_var(new_env_locals, lambda->var.name, type_terms[0],
                &*llvm_function->args().begin(), true /*allow_shadowing*/);

    if (closure != nullptr) {
      assert(free_vars.typed_ids.size() != 0);
      llvm::Value *closure_env = builder.CreateBitCast(
          llvm_function->arg_end() - 1, closure->getType(), "closure_env");
      debug_above(5, log("closure_env in gen_lambda is %s",
                         llvm_print(closure_env).c_str()));

      int arg_index = 1;
      for (auto typed_id : free_vars.typed_ids) {
        // inject the closed over vars into the new environment within the
        // closure
        llvm::Value *gep_path[] = {builder.getInt32(0),
                                   builder.getInt32(arg_index)};
        llvm::Value *llvm_captured_value_in_lambda_scope = builder.CreateLoad(
            builder.CreateInBoundsGEP(closure_env, gep_path));
        llvm_captured_value_in_lambda_scope->setName(typed_id.id.name);

        debug_above(5,
                    log("adding closed over var %s to new_env as %s :: %s",
                        typed_id.id.name.c_str(),
                        llvm_print(llvm_captured_value_in_lambda_scope).c_str(),
                        dim_types[arg_index - 1]->str().c_str()));

        set_env_var(new_env_locals, typed_id.id.name, dim_types[arg_index - 1],
                    llvm_captured_value_in_lambda_scope,
                    true /*allow_shadowing*/);
        ++arg_index;
      }
    } else {
      assert(free_vars.typed_ids.size() == 0);
    }

    debug_above(3, log("generating body for %s = %s", name.c_str(),
                       lambda->body->str().c_str()));
    /* now build the body of the function */
    gen("", builder, llvm_module, nullptr /*break_to_block*/,
        nullptr /*continue_to_block*/, lambda->body, typing, gen_env_globals,
        new_env_locals, globals, nullptr /*publishable*/);

    if (builder.GetInsertBlock()->getTerminator() == nullptr) {
      /* ensure that we have a terminator */
      builder.CreateRet(
          llvm::Constant::getNullValue(builder.getInt8Ty()->getPointerTo()));
    }
    llvm_verify_function(INTERNAL_LOC(), llvm_function);
  }
}

llvm::Value *gen_literal(std::string name,
                         llvm::IRBuilder<> &builder,
                         const bitter::literal_t *literal,
                         types::type_t::ref type) {
  auto &token = literal->token;
  debug_above(6, log("emitting literal %s :: %s", token.str().c_str(),
                     type->str().c_str()));
  if (type_equality(type, type_id(make_iid(INT_TYPE)))) {
    return builder.getZionInt(atoll(token.text.c_str()));
  } else if (type_equality(type,
                           type_operator({type_id(make_iid(PTR_TYPE_OPERATOR)),
                                          type_id(make_iid(CHAR_TYPE))}))) {
    /* char * */
    auto llvm_literal = llvm_create_global_string_constant(
        builder, *llvm_get_module(builder), unescape_json_quotes(token.text));
    debug_above(
        6, log("emitting llvm literal %s", llvm_print(llvm_literal).c_str()));
    return llvm_literal;
  }

  assert_not_impl();
  return nullptr;
}

llvm::Value *gen(llvm::IRBuilder<> &builder,
                 llvm::Module *llvm_module,
                 llvm::BasicBlock *break_to_block,
                 llvm::BasicBlock *continue_to_block,
                 const bitter::expr_t *expr,
                 const tracked_types_t &typing,
                 const gen_env_t &gen_env_globals,
                 const gen_env_t &gen_env_locals,
                 const std::unordered_set<std::string> &globals) {
  llvm::Value *llvm_value = nullptr;
  publishable_t publishable(&llvm_value);
  gen("", builder, llvm_module, break_to_block, continue_to_block, expr, typing,
      gen_env_globals, gen_env_locals, globals, &publishable);
  return llvm_value;
}

void gen(std::string name,
         llvm::IRBuilder<> &builder,
         llvm::Module *llvm_module,
         llvm::BasicBlock *break_to_block,
         llvm::BasicBlock *continue_to_block,
         const bitter::expr_t *expr,
         const tracked_types_t &typing,
         const types::type_env_t &type_env,
         const gen_env_t &gen_env_globals,
         const gen_env_t &gen_env_locals,
         const std::unordered_set<std::string> &globals,
         publisher_t * const publisher) {
  auto publish = [publisher](llvm::Value *llvm_value) {
    if (publisher != nullptr) {
      publisher->publish(llvm_value);
    }
  };

  try {
    auto type = get(typing, expr, {});
    if (type == nullptr) {
      debug_above(5, log_location(log_error, expr->get_location(),
                                  "expression lacks typing %s",
                                  expr->str().c_str()));
      dbg();
    }

    debug_above(8, log("gen(..., %s, ..., ...)", expr->str().c_str()));
    if (auto literal = dcast<const bitter::literal_t *>(expr)) {
      publish(gen_literal(name, builder, literal, type));
    } else if (auto static_print = dcast<const bitter::static_print_t *>(
                   expr)) {
      assert(false);
    } else if (auto var = dcast<const bitter::var_t *>(expr)) {
      auto value = maybe_get_env_var(gen_env_locals, var->id, type);
      if (value == nullptr) {
        debug_above(5, log("falling back to globals to find %s :: %s",
                           var->id.str().c_str(), type->str().c_str()));
        publish(get_env_var(builder, gen_env_globals, var->id, type));
      } else {
        publish(value);
      }
    } else if (auto lambda = dcast<const bitter::lambda_t *>(expr)) {
      /* gen_lambda needs access to locals in order to capture closed over
       * variables */
      gen_lambda(name, builder, llvm_module, lambda, type, typing,
                 gen_env_globals, gen_env_locals, globals, publisher);
    } else if (auto application = dcast<const bitter::application_t *>(expr)) {
      llvm::Value *closure = gen(builder, llvm_module, break_to_block,
                                 continue_to_block, application->a, typing,
                                 gen_env_globals, gen_env_locals, globals);

      llvm::Value *lambda_arg = gen(builder, llvm_module, break_to_block,
                                    continue_to_block, application->b, typing,
                                    gen_env_globals, gen_env_locals, globals);

      llvm::Value *llvm_function_to_call = nullptr;
      destructure_closure(builder, closure, &llvm_function_to_call, nullptr);

      llvm::Value *args[] = {
          lambda_arg,
          builder.CreateBitCast(closure, builder.getInt8Ty()->getPointerTo())};

      publish(builder.CreateCall(llvm_function_to_call,
                                 llvm::ArrayRef<llvm::Value *>(args)));
    } else if (auto let = dcast<const bitter::let_t *>(expr)) {
      llvm::Value *let_value = nullptr;
      publishable_t publishable(&let_value);
      gen(let->var.name, builder, llvm_module, break_to_block,
          continue_to_block, let->value, typing, gen_env_globals,
          gen_env_locals, globals, &publishable);

      auto new_env_locals = gen_env_locals;
      set_env_var(new_env_locals, let->var.name,
                  get(typing, static_cast<const bitter::expr_t *>(let->value),
                      types::type_t::ref{}),
                  let_value, true /*allow_shadowing*/);

      publish(gen(builder, llvm_module, break_to_block, continue_to_block,
                  let->body, typing, gen_env_globals, new_env_locals, globals));
    } else if (auto fix = dcast<const bitter::fix_t *>(expr)) {
      assert(false);
    } else if (auto condition = dcast<const bitter::conditional_t *>(expr)) {
      llvm::Value *cond = gen(builder, llvm_module, break_to_block,
                              continue_to_block, condition->cond, typing,
                              gen_env_globals, gen_env_locals, globals);

      llvm::Function *llvm_function = llvm_get_function(builder);

      auto tag = bitter::fresh();
      llvm::BasicBlock *truthy_block = llvm::BasicBlock::Create(
          builder.getContext(), "truthy." + tag, llvm_function);
      llvm::BasicBlock *falsey_block = llvm::BasicBlock::Create(
          builder.getContext(), "falsey." + tag, llvm_function);
      llvm::BasicBlock *merge_block = nullptr;

      builder.CreateCondBr(cond, truthy_block, falsey_block);
      builder.SetInsertPoint(truthy_block);
      llvm::Value *truthy_value = gen(
          builder, llvm_module, break_to_block, continue_to_block,
          condition->truthy, typing, gen_env_globals, gen_env_locals, globals);

      llvm::PHINode *phi_node = nullptr;
      if (!builder.GetInsertBlock()->getTerminator()) {
        merge_block = llvm::BasicBlock::Create(builder.getContext(),
                                               "merge." + tag, llvm_function);
        phi_node = llvm::PHINode::Create(truthy_value->getType(), 1,
                                         "phi." + tag, merge_block);
        phi_node->addIncoming(truthy_value, builder.GetInsertBlock());
        builder.CreateBr(merge_block);
      }

      builder.SetInsertPoint(falsey_block);
      llvm::Value *falsey_value = gen(
          builder, llvm_module, break_to_block, continue_to_block,
          condition->falsey, typing, gen_env_globals, gen_env_locals, globals);
      if (!builder.GetInsertBlock()->getTerminator()) {
        if (phi_node == nullptr) {
          assert(merge_block == nullptr);
          merge_block = llvm::BasicBlock::Create(builder.getContext(),
                                                 "merge." + tag, llvm_function);
          phi_node = llvm::PHINode::Create(truthy_value->getType(), 1,
                                           "phi." + tag, merge_block);
        }
        phi_node->addIncoming(falsey_value, builder.GetInsertBlock());
        builder.CreateBr(merge_block);
      }

      if (merge_block != nullptr) {
        builder.SetInsertPoint(merge_block);
        publish(phi_node);
      }
    } else if (auto break_ = dcast<const bitter::break_t *>(expr)) {
      assert(break_to_block != nullptr);
      builder.CreateBr(break_to_block);
    } else if (auto continue_ = dcast<const bitter::continue_t *>(expr)) {
      assert(continue_to_block != nullptr);
      builder.CreateBr(continue_to_block);
    } else if (auto while_ = dcast<const bitter::while_t *>(expr)) {
      assert(publisher == nullptr);
      llvm::Function *llvm_function = llvm_get_function(builder);
      auto tag = bitter::fresh();
      auto cond_block = llvm::BasicBlock::Create(
          builder.getContext(), "while_cond." + tag, llvm_function);
      builder.CreateBr(cond_block);
      builder.SetInsertPoint(cond_block);

      llvm::Value *cond = gen(builder, llvm_module, break_to_block,
                              continue_to_block, while_->condition, typing,
                              gen_env_globals, gen_env_locals, globals);

      auto while_block = llvm::BasicBlock::Create(
          builder.getContext(), "while_block" + tag, llvm_function);
      auto else_block = llvm::BasicBlock::Create(
          builder.getContext(), "while_break" + tag, llvm_function);

      builder.CreateCondBr(cond, while_block, else_block);
      builder.SetInsertPoint(while_block);
      loop_guard_t loop_guard(else_block, cond_block, &break_to_block,
                              &continue_to_block);
      gen("", builder, llvm_module, break_to_block, continue_to_block,
          while_->block, typing, gen_env_globals, gen_env_locals, globals,
          nullptr /*publisher*/);

      if (builder.GetInsertBlock()->getTerminator() == nullptr) {
        builder.CreateBr(cond_block);
      }

      builder.SetInsertPoint(else_block);
    } else if (auto block = dcast<const bitter::block_t *>(expr)) {
      size_t inst_counter = block->statements.size() - 1;

      llvm::Value *value = nullptr;
      publishable_t publishable(&value);

      for (auto statement : block->statements) {
        gen("", builder, llvm_module, break_to_block, continue_to_block,
            statement, typing, gen_env_globals, gen_env_locals, globals,
            &publishable);
      }
      publish(value);
    } else if (auto return_ = dcast<const bitter::return_statement_t *>(expr)) {
      builder.CreateRet(gen(builder, llvm_module, break_to_block,
                            continue_to_block, return_->value, typing,
                            gen_env_globals, gen_env_locals, globals));
    } else if (auto tuple = dcast<const bitter::tuple_t *>(expr)) {
      std::vector<llvm::Value *> dim_values;
      for (auto dim : tuple->dims) {
        dim_values.push_back(gen(builder, llvm_module, break_to_block,
                                 continue_to_block, dim, typing,
                                 gen_env_globals, gen_env_locals, globals));
      }
      publish(llvm_tuple_alloc(builder, dim_values));
    } else if (auto tuple_deref = dcast<const bitter::tuple_deref_t *>(expr)) {
      auto td = gen(builder, llvm_module, break_to_block, continue_to_block,
                    tuple_deref->expr, typing, gen_env_globals, gen_env_locals,
                    globals);
      debug_above(10, log_location(tuple_deref->expr->get_location(),
                                   "created tuple deref %s from %s",
                                   llvm_print(td).c_str(),
                                   tuple_deref->expr->str().c_str()));
      llvm::Value *gep_path[] = {builder.getInt32(0),
                                 builder.getInt32(tuple_deref->index)};
      publish(builder.CreateLoad(builder.CreateInBoundsGEP(
          td->getType()->getPointerElementType(), td, gep_path)));
    } else if (auto as = dcast<const bitter::as_t *>(expr)) {
      assert(as->force_cast);
      auto expr_value = gen(builder, llvm_module, break_to_block,
                            continue_to_block, as->expr, typing,
                            gen_env_globals, gen_env_locals, globals);
      auto cast_type = get_llvm_type(builder,
                                     type_env,
                                     as->scheme->instantiate(INTERNAL_LOC()));
      log("casting %s (which is %s) to type %s (which is %s)",
          as->expr->str().c_str(), llvm_print(expr_value).c_str(),
          as->scheme->str().c_str(), llvm_print(cast_type).c_str());
      publish(builder.CreateBitCast(expr_value, cast_type));
    } else if (auto sizeof_ = dcast<const bitter::sizeof_t *>(expr)) {
      assert(false);
    } else if (auto match = dcast<const bitter::match_t *>(expr)) {
      assert(false);
    } else if (auto builtin = dcast<const bitter::builtin_t *>(expr)) {
      std::vector<llvm::Value *> llvm_values;
      for (auto expr : builtin->exprs) {
        llvm_values.push_back(gen(builder, llvm_module, break_to_block,
                                  continue_to_block, expr, typing,
                                  gen_env_globals, gen_env_locals, globals));
      }
      publish(gen_builtin(builder, builtin->var->id.name, llvm_values));
    } else {
      throw user_error(expr->get_location(), "unhandled gen for %s :: %s",
                       expr->str().c_str(), type->str().c_str());
    }
  } catch (user_error &e) {
    e.add_info(expr->get_location(), "while in gen phase for %s",
               expr->str().c_str());
    throw;
  }
}

} // namespace gen
