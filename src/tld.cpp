#include "tld.h"

#include "builtins.h"

namespace zion {
namespace tld {

#define SCOPE_SEP "::"
#define SCOPE_SEP_LEN 2

bool is_fqn(std::string name) {
  return name.find(SCOPE_SEP) != std::string::npos;
}

std::vector<std::string> split_fqn(std::string fqn) {
  auto ns = split(fqn, SCOPE_SEP);
  assert(ns.size() < 3);
  return ns;
}

std::string mktld(std::string module, std::string name) {
  if (starts_with(name, ARROW_TYPE_OPERATOR)) {
    return name;
  } else if (starts_with(name, PTR_TYPE_OPERATOR)) {
    return name;
  } else {
    return tld(module + SCOPE_SEP + name);
  }
}

bool is_tld(std::string name) {
  if (starts_with(name, SCOPE_SEP)) {
    assert(name.find(SCOPE_SEP) != std::string::npos);
    return true;
  } else {
    return false;
  }
}

std::string tld(std::string name) {
  if (is_tld(name)) {
    return name;
  } else {
    return SCOPE_SEP + name;
  }
}

Identifier tld(Identifier id) {
  if (is_tld(id.name)) {
    return id;
  } else {
    return Identifier(SCOPE_SEP + id.name, id.location);
  }
}

bool test_first_char_of_leaf(std::string _name, int (*char_predicate)(int)) {
  auto names = split_fqn(_name);
  const auto &name = names.back();
  if (starts_with(name, SCOPE_SEP)) {
    if (name.size() > SCOPE_SEP_LEN) {
      return char_predicate(name[SCOPE_SEP_LEN]);
    } else {
      assert(false);
      return false;
    }
  } else {
    if (name.size() > 0) {
      return char_predicate(name[0]);
    } else {
      assert(false);
      return false;
    }
  }
}

bool is_lowercase_leaf(std::string name) {
  return test_first_char_of_leaf(name, islower);
}

bool is_tld_type(std::string name) {
  if (name == ARROW_TYPE_OPERATOR || name == PTR_TYPE_OPERATOR) {
    // HACK for now.
    return true;
  }
  return test_first_char_of_leaf(name, isupper);
}

bool is_in_module(std::string module, std::string name) {
  return starts_with(name, std::string(SCOPE_SEP) + module + SCOPE_SEP);
}

} // namespace tld
} // namespace zion