#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace types {

struct ClassPredicate;
typedef std::shared_ptr<const ClassPredicate> ClassPredicateRef;

struct ClassPredicateRefHasher {
  size_t operator()(const ClassPredicateRef &) const;
};

struct ClassPredicateRefEqualTo {
  bool operator()(const ClassPredicateRef &lhs,
                  const ClassPredicateRef &rhs) const;
};

typedef std::unordered_set<ClassPredicateRef,
                           ClassPredicateRefHasher,
                           ClassPredicateRefEqualTo>
    ClassPredicates;

} // namespace types

#include "identifier.h"
#include "types.h"

namespace types {

struct ClassPredicate final
    : public std::enable_shared_from_this<ClassPredicate> {
  typedef std::shared_ptr<const ClassPredicate> Ref;

  ClassPredicate() = delete;
  ClassPredicate(const ClassPredicate &) = delete;
  ClassPredicate(Identifier classname, const Refs &params);
  ClassPredicate(Identifier classname, const Identifiers &params);

  std::string repr() const;
  std::string str() const;

  Location get_location() const;
  Ref rebind(const types::Map &bindings) const;
  Ref remap_vars(const std::map<std::string, std::string> &remapping) const;
  const Ftvs &get_ftvs() const;

  Identifier const classname;
  Refs const params;

  bool operator==(const ClassPredicate &rhs) const;

private:
  mutable bool has_repr_ = false;
  mutable std::string repr_;
  mutable bool has_ftvs_ = false;
  mutable Ftvs ftvs_;
};

ClassPredicates rebind(const ClassPredicates &class_predicates,
                       const Map &bindings);
ClassPredicates remap_vars(const ClassPredicates &class_predicates,
                           const std::map<std::string, std::string> &remapping);
Ftvs get_ftvs(const ClassPredicates &class_predicates);

} // namespace types

std::string str(const types::ClassPredicates &pm);
