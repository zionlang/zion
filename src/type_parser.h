#include "types.h"

struct parse_state_t;

namespace types {
	type_t::ref parse_type(parse_state_t &ps, const identifier::set &generics);
	identifier::ref reduce_ids(const std::list<identifier::ref> &ids, location_t location);
}

bool token_is_illegal_in_type(const token_t &token);
types::type_t::ref parse_type_expr(std::string input, identifier::set generics, identifier::ref module_id);