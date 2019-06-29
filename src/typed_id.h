#include <string>

#include "identifier.h"
#include "types.h"

struct TypedId {
  TypedId(Identifier id, types::Type::ref type) : id(id), type(type) {
  }
  Identifier const id;
  types::Type::ref const type;

private:
  mutable std::string cached_repr;

public:
  std::string repr() const;
  bool operator<(const TypedId &rhs) const;
};

std::ostream &operator<<(std::ostream &os, const TypedId &typed_id);
