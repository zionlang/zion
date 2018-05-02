#include "zion.h"
#include "match.h"
#include "ast.h"
#include "types.h"
#include <numeric>

namespace match {
	using namespace ::types;

	ptr<Nothing> theNothing = make_ptr<Nothing>();

	CtorPattern::CtorPattern(location_t location, CtorPatternValue cpv) :
	   	Pattern(location),
	   	cpv(cpv)
   	{
	}

	CtorPatterns::CtorPatterns(location_t location, std::vector<CtorPatternValue> cpvs) :
	   	Pattern(location),
	   	cpvs(cpvs)
   	{
	}

	AllOf::AllOf(location_t location, env_t::ref env, types::type_t::ref type) :
	   	Pattern(location),
	   	env(env),
	   	type(type)
   	{
	}

	Pattern::ref reduce_all_datatype(
			location_t location,
			std::string type_name,
			Pattern::ref rhs,
			const std::vector<CtorPatternValue> &cpvs)
	{
		for (auto cpv : cpvs) {
			if (cpv.type_name != type_name) {
				auto error = user_error(location,
						"invalid typed ctor pattern found. expected %s but ctor_pattern indicates it is a %s",
						type_name.c_str(),
						cpv.type_name.c_str());
				error.add_info(location, "comparing %s and %s", cpv.type_name.c_str(), type_name.c_str());
				throw error;
			}
		}

		assert(cpvs.size() != 0);
		if (cpvs.size() == 1) {
			return make_ptr<CtorPattern>(location, cpvs[0]);
		} else {
			return make_ptr<CtorPatterns>(location, cpvs);
		}
	}

	Pattern::ref intersect(location_t location, const CtorPatternValue &lhs, const CtorPatternValue &rhs) {
		assert(lhs.type_name == rhs.type_name);
		if (lhs.name != rhs.name) {
			return theNothing;
		}
		assert(lhs.args.size() == rhs.args.size());

		std::vector<Pattern::ref> reduced_args;
		reduced_args.reserve(lhs.args.size());

		for (int i = 0; i < lhs.args.size(); ++i) {
			auto new_arg = intersect(lhs.args[i], rhs.args[i]);
			if (dyncast<const Nothing>(new_arg) != nullptr) {
				return theNothing;
			} else {
				reduced_args.push_back(new_arg);
			}
		}
		assert(reduced_args.size() == lhs.args.size());
		return make_ptr<CtorPattern>(location, CtorPatternValue{lhs.type_name, lhs.name, reduced_args});
	}

	Pattern::ref cpv_intersect(Pattern::ref lhs, const CtorPatternValue &rhs) {
		auto ctor_pattern = dyncast<const CtorPattern>(lhs);
		assert(ctor_pattern != nullptr);
		return intersect(lhs->location, ctor_pattern->cpv, rhs);
	}

	Pattern::ref intersect(location_t location, const std::vector<CtorPatternValue> &lhs, const std::vector<CtorPatternValue> &rhs) {
		Pattern::ref intersection = theNothing;
		for (auto &cpv : lhs) {
			Pattern::ref init = make_ptr<CtorPattern>(location, cpv);
			intersection = pattern_union(
					std::accumulate(
						rhs.begin(),
					   	rhs.end(),
					   	init,
						cpv_intersect),
					intersection);
		}
		return intersection;
	}

	Pattern::ref intersect(Pattern::ref lhs, Pattern::ref rhs) {
		/* ironically, this is where pattern matching would really help... */
		auto lhs_nothing = lhs->asNothing();
		auto rhs_nothing = rhs->asNothing();
		auto lhs_allof = lhs->asAllOf();
		auto rhs_allof = rhs->asAllOf();
		auto lhs_ctor_patterns = lhs->asCtorPatterns();
		auto rhs_ctor_patterns = rhs->asCtorPatterns();
		auto lhs_ctor_pattern = lhs->asCtorPattern();
		auto rhs_ctor_pattern = rhs->asCtorPattern();

		if (lhs_nothing) {
			return lhs;
		}

		if (rhs_nothing) {
			return rhs;
		}

		if (lhs_allof) {
			if (rhs_allof) {
				if (lhs_allof->type->repr() == rhs_allof->type->repr()) {
					return lhs;
				}

				auto error = user_error(lhs->location, "type mismatch when comparing ctors for pattern intersection");
				error.add_info(rhs->location, "comparing this type");
				throw error;
			} else {
				auto lhs_data_type = dyncast<const type_data_t>(lhs_allof->type->eval(lhs_allof->env));
				if (lhs_data_type) {
					if (rhs_ctor_patterns) {
						return reduce_all_datatype(lhs->location, lhs_data_type->repr(), rhs, rhs_ctor_patterns->cpvs);
					} else if (rhs_ctor_pattern) {
						std::vector<CtorPatternValue> cpvs{rhs_ctor_pattern->cpv};
						return reduce_all_datatype(lhs->location, lhs_data_type->repr(), rhs, cpvs);
					}
				}
				throw user_error(INTERNAL_LOC(), "not implemented");
			}
		}

		if (rhs_allof) {
			if (auto rhs_data_type = dyncast<const type_data_t>(rhs_allof->type->eval(rhs_allof->env))) {
				if (lhs_ctor_patterns) {
					return reduce_all_datatype(rhs->location, rhs_data_type->repr(), lhs, lhs_ctor_patterns->cpvs);
				} else if (lhs_ctor_pattern) {
					std::vector<CtorPatternValue> cpvs{lhs_ctor_pattern->cpv};
					return reduce_all_datatype(rhs->location, rhs_data_type->repr(), lhs, cpvs);
				}
			}
			throw user_error(INTERNAL_LOC(), "not implemented");
		}

		if (lhs_ctor_pattern && rhs_ctor_pattern) {
			std::vector<CtorPatternValue> lhs_cpvs({lhs_ctor_pattern->cpv});
			std::vector<CtorPatternValue> rhs_cpvs({rhs_ctor_pattern->cpv});
			return intersect(rhs->location, lhs_cpvs, rhs_cpvs);
		}

		if (lhs_ctor_patterns && rhs_ctor_pattern) {
			std::vector<CtorPatternValue> rhs_cpvs({rhs_ctor_pattern->cpv});
			return intersect(rhs->location, lhs_ctor_patterns->cpvs, rhs_cpvs);
		}

		if (lhs_ctor_pattern && rhs_ctor_patterns) {
			std::vector<CtorPatternValue> lhs_cpvs({lhs_ctor_pattern->cpv});
			return intersect(rhs->location, lhs_cpvs, rhs_ctor_patterns->cpvs);
		}

		log_location(log_error, lhs->location,
				"intersect is not implemented yet (%s vs. %s)",
				lhs->str().c_str(),
				rhs->str().c_str());

		throw user_error(INTERNAL_LOC(), "not implemented");
		return nullptr;
	}

	Pattern::ref pattern_union(Pattern::ref lhs, Pattern::ref rhs) {
		auto lhs_nothing = lhs->asNothing();
		auto rhs_nothing = rhs->asNothing();
		auto lhs_allof = lhs->asAllOf();
		auto rhs_allof = rhs->asAllOf();
		auto lhs_ctor_patterns = lhs->asCtorPatterns();
		auto rhs_ctor_patterns = rhs->asCtorPatterns();
		auto lhs_ctor_pattern = lhs->asCtorPattern();
		auto rhs_ctor_pattern = rhs->asCtorPattern();

		if (lhs_nothing) {
			return rhs;
		}

		if (rhs_nothing) {
			return lhs;
		}

		if (lhs_ctor_patterns && rhs_ctor_patterns) {
			std::vector<CtorPatternValue> cpvs = lhs_ctor_patterns->cpvs;
			for (auto &cpv : rhs_ctor_patterns->cpvs) {
				cpvs.push_back(cpv);
			}
			return make_ptr<CtorPatterns>(lhs->location, cpvs);
		}

		if (lhs_ctor_patterns && rhs_ctor_pattern) {
			std::vector<CtorPatternValue> cpvs = lhs_ctor_patterns->cpvs;
			cpvs.push_back(rhs_ctor_pattern->cpv);
			return make_ptr<CtorPatterns>(lhs->location, cpvs);
		}

		if (lhs_ctor_pattern && rhs_ctor_patterns) {
			std::vector<CtorPatternValue> cpvs = rhs_ctor_patterns->cpvs;
			cpvs.push_back(lhs_ctor_pattern->cpv);
			return make_ptr<CtorPatterns>(lhs->location, cpvs);
		}

		if (lhs_ctor_pattern && rhs_ctor_pattern) {
			std::vector<CtorPatternValue> cpvs{lhs_ctor_pattern->cpv, rhs_ctor_pattern->cpv};
			return make_ptr<CtorPatterns>(lhs->location, cpvs);
		}

		log_location(log_error, lhs->location, "unhandled pattern_union (%s ∪ %s)",
				lhs->str().c_str(),
				rhs->str().c_str());
		throw user_error(INTERNAL_LOC(), "not implemented");
		return nullptr;
	}

	Pattern::ref from_type(location_t location, env_t::ref env, type_t::ref type) {
		type = type->eval(env);
		if (auto data_type = dyncast<const type_data_t>(type)) {
			std::vector<CtorPatternValue> cpvs;

			for (auto ctor_pair : data_type->ctor_pairs) {
				std::vector<Pattern::ref> args;
				args.reserve(ctor_pair.second->args.size());

				for (auto arg_type : ctor_pair.second->args) {
					args.push_back(make_ptr<AllOf>(location, env, arg_type));
				}

				/* add a ctor */
				cpvs.push_back(CtorPatternValue{type->repr(), ctor_pair.first.text, args});
			}

			if (cpvs.size() == 1) {
				return make_ptr<CtorPattern>(location, cpvs[0]);
			} else if (cpvs.size() > 1) {
				return make_ptr<CtorPatterns>(location, cpvs);
			} else {
				throw user_error(INTERNAL_LOC(), "not implemented");
			}
		} else {
			throw user_error(type->get_location(), "don't know how to create a pattern from a %s",
					type->str().c_str());
		}

		return nullptr;
	}

	void difference(
			Pattern::ref lhs,
		   	Pattern::ref rhs,
		   	const std::function<void (Pattern::ref)> &send);

	void difference(
			location_t location,
			const CtorPatternValue &lhs,
			const CtorPatternValue &rhs,
			const std::function<void (Pattern::ref)> &send)
	{
		assert(lhs.type_name == rhs.type_name);

		if (lhs.name != rhs.name) {
			send(make_ptr<CtorPattern>(location, lhs));
		} else if (lhs.args.size() == 0) {
			send(theNothing);
		} else {
			assert(lhs.args.size() == rhs.args.size());
			int i = 0;
			auto send_ctor_pattern = [location, &i, &lhs, &send] (Pattern::ref arg) {
				if (dyncast<const Nothing>(arg)) {
					send(theNothing);
				} else {
					std::vector<Pattern::ref> args = lhs.args;
					args[i] = arg;
					send(make_ptr<CtorPattern>(location, CtorPatternValue{lhs.type_name, lhs.name, args}));
				}
			};

			for (; i < lhs.args.size(); ++i) {
				difference(lhs.args[i], rhs.args[i], send_ctor_pattern);
			}
		}
	}

	void difference(
			Pattern::ref lhs,
			Pattern::ref rhs,
			const std::function<void (Pattern::ref)> &send)
	{
		debug_above(8, log_location(log_info, rhs->location, "computing %s \\ %s",
				lhs->str().c_str(),
				rhs->str().c_str()));

		auto lhs_nothing = lhs->asNothing();
		auto rhs_nothing = rhs->asNothing();
		auto lhs_allof = lhs->asAllOf();
		auto rhs_allof = rhs->asAllOf();
		auto lhs_ctor_patterns = lhs->asCtorPatterns();
		auto rhs_ctor_patterns = rhs->asCtorPatterns();
		auto lhs_ctor_pattern = lhs->asCtorPattern();
		auto rhs_ctor_pattern = rhs->asCtorPattern();

		if (lhs_nothing || rhs_nothing) {
			send(lhs);
			return;
		}

		if (lhs_allof) {
			if (rhs_allof) {
				if (lhs_allof->type->repr() == rhs_allof->type->repr()) {
					/* subtracting an entire type from itself */
					send(theNothing);
					return;
				}

				auto error = user_error(lhs->location, "type mismatch when comparing ctors for pattern intersection");
				error.add_info(rhs->location, "comparing this type");
				throw error;
			}

			difference(from_type(lhs->location, lhs_allof->env, lhs_allof->type), rhs, send);
			return;
		}

		if (rhs_allof) {
			difference(lhs, from_type(rhs->location, rhs_allof->env, rhs_allof->type), send);
			return;
		}

		assert(lhs_allof == nullptr);
		assert(rhs_allof == nullptr);

		if (lhs_ctor_patterns) {
			if (rhs_ctor_patterns) {
				for (auto &cpv : lhs_ctor_patterns->cpvs) {
					difference(make_ptr<CtorPattern>(lhs_ctor_patterns->location, cpv), rhs, send);
				}
				return;
			} else if (rhs_ctor_pattern) {
				for (auto &cpv : lhs_ctor_patterns->cpvs) {
					difference(lhs->location, cpv, rhs_ctor_pattern->cpv, send);
				}
				return;
			} else {
				throw user_error(rhs->location, "type mismatch");
			}
		}

		if (lhs_ctor_pattern) {
			if (rhs_ctor_patterns) {
				Pattern::ref new_a = make_ptr<CtorPattern>(lhs->location, lhs_ctor_pattern->cpv);
				auto new_a_send = [&new_a] (Pattern::ref pattern) {
					new_a = pattern_union(new_a, pattern);
				};

				for (auto &b : rhs_ctor_patterns->cpvs) {
					auto current_a = new_a;
					new_a = theNothing;
					difference(current_a, make_ptr<CtorPattern>(rhs->location, b), new_a_send);
				}

				send(new_a);
				return;
			} else if (rhs_ctor_pattern) {
				difference(lhs->location, lhs_ctor_pattern->cpv, rhs_ctor_pattern->cpv, send);
				return;
			}
		}

		debug_above(8, log_location(log_error, lhs->location, "unhandled difference - %s \\ %s",
				lhs->str().c_str(),
				rhs->str().c_str()));
		throw user_error(INTERNAL_LOC(), "not implemented");
	}

	Pattern::ref difference(Pattern::ref lhs, Pattern::ref rhs) {
		Pattern::ref computed = theNothing;

		auto send = [&computed] (Pattern::ref pattern) {
			computed = pattern_union(pattern, computed);
		};

		difference(lhs, rhs, send);

		return computed;
	}

	std::string AllOf::str() const {
		std::stringstream ss;
		ss << "AllOf(" << type->str() << ")";
		return ss.str();
	}

	std::string Nothing::str() const {
		return "Nothing";
	}

	std::string CtorPattern::str() const {
		std::stringstream ss;
		ss << "CtorPattern(" << cpv.str() << ")";
		return ss.str();
	}

	std::string CtorPatterns::str() const {
		std::stringstream ss;
		ss << "CtorPatterns(" << ::join_with(cpvs, ", ", [](const CtorPatternValue &cpv) -> std::string { return cpv.str(); }) << ")";
		return ss.str();
	}

	std::string CtorPatternValue::str() const {
		std::stringstream ss;
		ss << "CtorPatternValue(" << type_name << ", " << name << ", [";
		ss << ::join_str(args, ", ") << "])";
		return ss.str();
	}

}

namespace ast {
	using namespace ::match;
	using namespace ::types;

	Pattern::ref ctor_predicate_t::get_pattern(type_t::ref type, env_t::ref env) const {
		type = type->eval(env);
		if (auto data_type = dyncast<const type_data_t>(type->eval(env))) {
			for (auto ctor_pair : data_type->ctor_pairs) {
				if (ctor_pair.first.text == token.text) {
					std::vector<Pattern::ref> args;
					if (ctor_pair.second->args.size() != params.size()) {
						throw user_error(token.location, "%s has an incorrect number of sub-patterns. there are %d, there should be %d",
								token.text.c_str(),
								int(params.size()),
								int(ctor_pair.second->args.size()));
					}

					for (int i = 0; i < params.size(); ++i) {
						args.push_back(params[i]->get_pattern(ctor_pair.second->args[i], env));
					}

					/* found the ctor we're matching on */
					return make_ptr<CtorPattern>(token.location, CtorPatternValue{type->repr(), token.text, args});
				}
			}

			throw user_error(token.location, "invalid ctor, " c_id("%s") " is not a member of %s",
					token.text.c_str(), type->str().c_str());
			return nullptr;
		} else {
			throw user_error(token.location, "type mismatch on pattern. incoming type is %s. "
				   	c_id("%s") " cannot match it.", type->str().c_str(), token.text.c_str());
			return nullptr;
		}
	}
	Pattern::ref irrefutable_predicate_t::get_pattern(type_t::ref type, env_t::ref env) const {
		return make_ptr<AllOf>(token.location, env, type);
	}
	Pattern::ref literal_expr_t::get_pattern(type_t::ref type, env_t::ref env) const {
		throw user_error(INTERNAL_LOC(), "not implemented");
		return nullptr;
	}
}