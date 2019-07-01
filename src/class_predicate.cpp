#include "class_predicate.h"

#include <ctype.h>
#include <cstring>
#include <sstream>

#include "colors.h"
#include "types.h"
#include "utils.h"

namespace types {

size_t ClassPredicateRefHasher::operator()(const ClassPredicateRef &rhs) const {
  return std::hash<std::string>()(rhs->repr());
}

bool ClassPredicateRefEqualTo::operator()(const ClassPredicateRef &lhs,
                                          const ClassPredicateRef &rhs) const {
  return !(*lhs < *rhs || *rhs < *lhs);
}

ClassPredicate::ClassPredicate(Identifier classname, const types::Refs &params)
    : classname(classname), params(params) {
#ifdef ZION_DEBUG
  if (std::strchr(classname.name.c_str(), '.') != nullptr) {
    assert(isupper(split(classname.name, ".")[1][0]));
  } else {
    assert(isupper(classname.name[0]));
  }
#endif
}

ClassPredicate::ClassPredicate(Identifier classname, const Identifiers &params)
    : ClassPredicate(classname, type_variables(params)) {
}

Location ClassPredicate::get_location() const {
  return classname.location;
}

bool ClassPredicate::operator<(const ClassPredicate &rhs) const {
  assert(false);
  if (classname < rhs.classname) {
    return true;
  }

  for (int i = 0; i < params.size(); ++i) {
    if (i >= rhs.params.size()) {
      return false;
    } else if (params[i]->repr() < rhs.params[i]->repr()) {
      return true;
    }
  }
  return false;
}

std::string ClassPredicate::repr() const {
  if (!has_repr_) {
    std::stringstream ss;
    ss << classname;
    for (auto &param : params) {
      ss << " " << param->repr();
    }
    repr_ = ss.str();
  }

  return repr_;
}

std::string ClassPredicate::str() const {
  std::stringstream ss;
  ss << C_GOOD << repr() << C_RESET;
  return ss.str();
}

ClassPredicates rebind(const ClassPredicates &class_predicates,
                       const Map &bindings) {
  assert(false);
  return {};
}

ClassPredicate::Ref ClassPredicate::remap_vars(
    const std::map<std::string, std::string> &remapping) const {
  /* remap all the type vars in a ClassPredicate. This is basically just an
   * optimization over rebind. */
  Refs new_params;
  new_params.reserve(params.size());

  for (const types::Ref &param : params) {
    new_params.push_back(param->remap_vars(remapping));
  }

  return std::make_shared<types::ClassPredicate>(classname, new_params);
}

const Ftvs &ClassPredicate::get_ftvs() const {
  if (!has_ftvs_) {
    for (auto &param : params) {
      set_merge(ftvs_, param->get_ftvs());
    }
    has_ftvs_ = true;
  }

  return ftvs_;
}

ClassPredicates remap_vars(
    const ClassPredicates &class_predicates,
    const std::map<std::string, std::string> &remapping) {
  /* remap all the type variables referenced in a set of ClassPredicates */
  ClassPredicates new_class_predicates;

  for (const ClassPredicateRef &cp : class_predicates) {
    new_class_predicates.insert(cp->remap_vars(remapping));
  }
  return new_class_predicates;
}

} // namespace types
