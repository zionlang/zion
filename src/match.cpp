#include "zion.h"
#include <typeinfo>
#include "match.h"
#include "ast.h"
#include "types.h"
#include <numeric>
#include <algorithm>
#include "user_error.h"
#include "unification.h"
#include "env.h"

namespace match {
	using namespace ::types;

	struct Nothing : std::enable_shared_from_this<Nothing>, Pattern {
		Nothing() : Pattern(INTERNAL_LOC()) {}

		virtual std::shared_ptr<const Nothing> asNothing() const { return shared_from_this(); }
		virtual std::string str() const;
	};

	struct CtorPatternValue {
		std::string type_name;
		std::string name;
		std::vector<Pattern::ref> args;

		std::string str() const;
	};

	struct CtorPattern : std::enable_shared_from_this<CtorPattern>, Pattern {
		CtorPatternValue cpv;
		CtorPattern(location_t location, CtorPatternValue cpv) :
			Pattern(location),
			cpv(cpv)
		{
		}

		virtual std::string str() const;
	};

	struct CtorPatterns : std::enable_shared_from_this<CtorPatterns>, Pattern {
		std::vector<CtorPatternValue> cpvs;
		CtorPatterns(location_t location, std::vector<CtorPatternValue> cpvs) :
			Pattern(location),
			cpvs(cpvs)
		{
		}

		virtual std::string str() const;
	};

	struct AllOf : std::enable_shared_from_this<AllOf>, Pattern {
		maybe<identifier_t> name;
		env_t::ref env;
		types::type_t::ref type;

		AllOf(location_t location, maybe<identifier_t> name, env_t::ref env, types::type_t::ref type) :
			Pattern(location),
			name(name),
			env(env),
			type(type)
		{
		}

		virtual std::string str() const;
	};

	template <typename T>
	struct Scalars : std::enable_shared_from_this<Scalars<T>>, Pattern {
		enum Kind { Include, Exclude } kind;
		std::set<T> collection;

		Scalars(location_t location, Kind kind, std::set<T> collection) :
			Pattern(location),
			kind(kind),
			collection(collection)
		{
			assert_implies(kind == Include, collection.size() != 0);
		}

		static std::string scalar_name();

		virtual std::string str() const {
			switch (kind) {
			case Include:
				{
					std::string coll_str = "[" + ::join(collection, ", ") + "]";
					return coll_str;
				}
			case Exclude:
				if (collection.size() == 0) {
					return "all " + scalar_name();
				} else {
					std::string coll_str = "[" + ::join(collection, ", ") + "]";
					return "all " + scalar_name() + " except " + coll_str;
				}
			}
			assert(false);
			return "";
		}
	};

	template <>
	std::string Scalars<int64_t>::scalar_name() {
		return "integers";
	}

	template <>
	std::string Scalars<std::string>::scalar_name() {
		return "strings";
	}

	std::shared_ptr<const CtorPattern> asCtorPattern(Pattern::ref pattern) {
		return dyncast<const CtorPattern>(pattern);
	}

	std::shared_ptr<const CtorPatterns> asCtorPatterns(Pattern::ref pattern) {
		return dyncast<const CtorPatterns>(pattern);
	}

	std::shared_ptr<const AllOf> asAllOf(Pattern::ref pattern) {
		return dyncast<const AllOf>(pattern);
	}

	template <typename T>
	std::shared_ptr<const Scalars<T>> asScalars(Pattern::ref pattern) {
		return dyncast<const Scalars<T>>(pattern);
	}

	std::shared_ptr<Nothing> theNothing = std::make_shared<Nothing>();
	std::shared_ptr<Scalars<int64_t>> allIntegers = std::make_shared<Scalars<int64_t>>(INTERNAL_LOC(), Scalars<int64_t>::Exclude, std::set<int64_t>{});
	std::shared_ptr<Scalars<std::string>> allStrings = std::make_shared<Scalars<std::string>>(INTERNAL_LOC(), Scalars<std::string>::Exclude, std::set<std::string>{});

	Pattern::ref all_of(location_t location, maybe<identifier_t> expr, env_t::ref env, types::type_t::ref type) {
		return std::make_shared<match::AllOf>(location, expr, env, type);
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
			return std::make_shared<CtorPattern>(location, cpvs[0]);
		} else {
			return std::make_shared<CtorPatterns>(location, cpvs);
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

		for (size_t i = 0; i < lhs.args.size(); ++i) {
			auto new_arg = intersect(lhs.args[i], rhs.args[i]);
			if (dyncast<const Nothing>(new_arg) != nullptr) {
				return theNothing;
			} else {
				reduced_args.push_back(new_arg);
			}
		}
		assert(reduced_args.size() == lhs.args.size());
		return std::make_shared<CtorPattern>(location, CtorPatternValue{lhs.type_name, lhs.name, reduced_args});
	}

	Pattern::ref cpv_intersect(Pattern::ref lhs, const CtorPatternValue &rhs) {
		auto ctor_pattern = dyncast<const CtorPattern>(lhs);
		assert(ctor_pattern != nullptr);
		return intersect(lhs->location, ctor_pattern->cpv, rhs);
	}

	Pattern::ref intersect(location_t location, const std::vector<CtorPatternValue> &lhs, const std::vector<CtorPatternValue> &rhs) {
		Pattern::ref intersection = theNothing;
		for (auto &cpv : lhs) {
			Pattern::ref init = std::make_shared<CtorPattern>(location, cpv);
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

	template <typename T>
	Pattern::ref intersect(
			const Scalars<T> &lhs,
			const Scalars<T> &rhs)
	{
		typename Scalars<T>::Kind new_kind;
		std::set<T> new_collection;

		if (lhs.kind == Scalars<T>::Exclude && rhs.kind == Scalars<T>::Exclude) {
			new_kind = Scalars<T>::Exclude;
			std::set_union(
					lhs.collection.begin(), lhs.collection.end(),
					rhs.collection.begin(), rhs.collection.end(),
					std::insert_iterator<std::set<T>>(new_collection, new_collection.begin()));
		} else if (lhs.kind == Scalars<T>::Exclude && rhs.kind == Scalars<T>::Include) {
			new_kind = Scalars<T>::Include;
			std::set_difference(
					rhs.collection.begin(), rhs.collection.end(),
					lhs.collection.begin(), lhs.collection.end(),
					std::insert_iterator<std::set<T>>(new_collection, new_collection.begin()));
		} else if (lhs.kind == Scalars<T>::Include && rhs.kind == Scalars<T>::Exclude) {
			new_kind = Scalars<T>::Include;
			std::set_difference(
					lhs.collection.begin(), lhs.collection.end(),
					rhs.collection.begin(), rhs.collection.end(),
					std::insert_iterator<std::set<T>>(new_collection, new_collection.begin()));
		} else if (lhs.kind == Scalars<T>::Include && rhs.kind == Scalars<T>::Include) {
			new_kind = Scalars<T>::Include;
			std::set_intersection(
					lhs.collection.begin(), lhs.collection.end(),
					rhs.collection.begin(), rhs.collection.end(),
					std::insert_iterator<std::set<T>>(new_collection, new_collection.begin()));
		} else {
			return null_impl();
		}

		if (new_kind == Scalars<T>::Include && new_collection.size() == 0) {
			return theNothing;
		}
		return std::make_shared<Scalars<T>>(lhs.location, new_kind, new_collection);
	}

	Pattern::ref intersect(Pattern::ref lhs, Pattern::ref rhs) {
		/* ironically, this is where pattern matching would really help... */
		auto lhs_nothing = lhs->asNothing();
		auto rhs_nothing = rhs->asNothing();
		auto lhs_allof = asAllOf(lhs);
		auto rhs_allof = asAllOf(rhs);
		auto lhs_ctor_patterns = asCtorPatterns(lhs);
		auto rhs_ctor_patterns = asCtorPatterns(rhs);
		auto lhs_ctor_pattern = asCtorPattern(lhs);
		auto rhs_ctor_pattern = asCtorPattern(rhs);
		auto lhs_integers = asScalars<int64_t>(lhs);
		auto rhs_integers = asScalars<int64_t>(rhs);
		auto lhs_strings = asScalars<std::string>(lhs);
		auto rhs_strings = asScalars<std::string>(rhs);

		if (lhs_nothing || rhs_nothing) {
			/* intersection of nothing and anything is nothing */
			return theNothing;
		}

		if (lhs_allof) {
			/* intersection of everything and x is x */
			return rhs;
		}

		if (rhs_allof) {
			/* intersection of everything and x is x */
			return lhs;
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

		if (lhs_integers && rhs_integers) {
			return intersect(*lhs_integers, *rhs_integers);
		}

		if (lhs_strings && rhs_strings) {
			return intersect(*lhs_strings, *rhs_strings);
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
		auto lhs_allof = asAllOf(lhs);
		auto rhs_allof = asAllOf(rhs);
		auto lhs_ctor_patterns = asCtorPatterns(lhs);
		auto rhs_ctor_patterns = asCtorPatterns(rhs);
		auto lhs_ctor_pattern = asCtorPattern(lhs);
		auto rhs_ctor_pattern = asCtorPattern(rhs);

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
			return std::make_shared<CtorPatterns>(lhs->location, cpvs);
		}

		if (lhs_ctor_patterns && rhs_ctor_pattern) {
			std::vector<CtorPatternValue> cpvs = lhs_ctor_patterns->cpvs;
			cpvs.push_back(rhs_ctor_pattern->cpv);
			return std::make_shared<CtorPatterns>(lhs->location, cpvs);
		}

		if (lhs_ctor_pattern && rhs_ctor_patterns) {
			std::vector<CtorPatternValue> cpvs = rhs_ctor_patterns->cpvs;
			cpvs.push_back(lhs_ctor_pattern->cpv);
			return std::make_shared<CtorPatterns>(lhs->location, cpvs);
		}

		if (lhs_ctor_pattern && rhs_ctor_pattern) {
			std::vector<CtorPatternValue> cpvs{lhs_ctor_pattern->cpv, rhs_ctor_pattern->cpv};
			return std::make_shared<CtorPatterns>(lhs->location, cpvs);
		}

		log_location(log_error, lhs->location, "unhandled pattern_union (%s ∪ %s)",
				lhs->str().c_str(),
				rhs->str().c_str());
		throw user_error(INTERNAL_LOC(), "not implemented");
		return nullptr;
	}

	Pattern::ref from_type(location_t location, env_t::ref env, type_t::ref type) {
		assert(false);
		// type = type->eval(env);
		if (auto data_type = dyncast<const type_data_t>(type)) {
			std::vector<CtorPatternValue> cpvs;

			for (auto ctor_pair : data_type->ctor_pairs) {
				std::vector<Pattern::ref> args;
				args.reserve(ctor_pair.second->args.size());

				for (size_t i = 0; i < ctor_pair.second->args.size(); ++i) {
					args.push_back(
							std::make_shared<AllOf>(location,
								(ctor_pair.second->names.size() > i)
							   	? maybe<identifier_t>(ctor_pair.second->names[i])
							   	: maybe<identifier_t>(),
								env,
								ctor_pair.second->args[i]));
				}

				/* add a ctor */
				cpvs.push_back(CtorPatternValue{type->repr(), ctor_pair.first.text, args});
			}

			if (cpvs.size() == 1) {
				return std::make_shared<CtorPattern>(location, cpvs[0]);
			} else if (cpvs.size() > 1) {
				return std::make_shared<CtorPatterns>(location, cpvs);
			} else {
				throw user_error(INTERNAL_LOC(), "not implemented");
			}
		} else if (auto tuple_type = dyncast<const types::type_tuple_t>(type)) {
			std::vector<Pattern::ref> args;
			for (auto dim : tuple_type->dimensions) {
				args.push_back(from_type(location, env, dim));
			}
			CtorPatternValue cpv{type->repr(), "tuple", args};
			return std::make_shared<CtorPattern>(location, cpv);
		} else if (type_equality(type, type_string(INTERNAL_LOC()))) {
			return allStrings;
		} else if (type_equality(type, type_int(INTERNAL_LOC()))) {
			return allIntegers;
		} else {
			/* just accept all of whatever this is */
			return std::make_shared<AllOf>(
					type->get_location(),
					maybe<identifier_t>(identifier_t{string_format("AllOf(%s)", type->str().c_str()), type->get_location()}),
					env,
					type);
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
			send(std::make_shared<CtorPattern>(location, lhs));
		} else if (lhs.args.size() == 0) {
			send(theNothing);
		} else {
			assert(lhs.args.size() == rhs.args.size());
			size_t i = 0;
			auto send_ctor_pattern = [location, &i, &lhs, &send] (Pattern::ref arg) {
				if (dyncast<const Nothing>(arg)) {
					send(theNothing);
				} else {
					std::vector<Pattern::ref> args = lhs.args;
					args[i] = arg;
					send(std::make_shared<CtorPattern>(location, CtorPatternValue{lhs.type_name, lhs.name, args}));
				}
			};

			for (; i < lhs.args.size(); ++i) {
				difference(lhs.args[i], rhs.args[i], send_ctor_pattern);
			}
		}
	}

	template <typename T>
	Pattern::ref difference(
			const Scalars<T> &lhs,
			const Scalars<T> &rhs)
	{
		typename Scalars<T>::Kind new_kind;
		std::set<T> new_collection;

		if (lhs.kind == Scalars<T>::Exclude && rhs.kind == Scalars<T>::Exclude) {
			new_kind = Scalars<T>::Include;
			std::set_difference(
					rhs.collection.begin(), rhs.collection.end(),
					lhs.collection.begin(), lhs.collection.end(),
					std::insert_iterator<std::set<T>>(new_collection, new_collection.begin()));
		} else if (lhs.kind == Scalars<T>::Exclude && rhs.kind == Scalars<T>::Include) {
			new_kind = Scalars<T>::Exclude;
			std::set_union(
					rhs.collection.begin(), rhs.collection.end(),
					lhs.collection.begin(), lhs.collection.end(),
					std::insert_iterator<std::set<T>>(new_collection, new_collection.begin()));
		} else if (lhs.kind == Scalars<T>::Include && rhs.kind == Scalars<T>::Exclude) {
			new_kind = Scalars<T>::Include;
			std::set_intersection(
					rhs.collection.begin(), rhs.collection.end(),
					lhs.collection.begin(), lhs.collection.end(),
					std::insert_iterator<std::set<T>>(new_collection, new_collection.begin()));
		} else if (lhs.kind == Scalars<T>::Include && rhs.kind == Scalars<T>::Include) {
			new_kind = Scalars<T>::Include;
			std::set_difference(
					lhs.collection.begin(), lhs.collection.end(),
					rhs.collection.begin(), rhs.collection.end(),
					std::insert_iterator<std::set<T>>(new_collection, new_collection.begin()));
		} else {
			return null_impl();
		}

		if (new_kind == Scalars<T>::Include && new_collection.size() == 0) {
			return theNothing;
		}
		return std::make_shared<Scalars<T>>(lhs.location, new_kind, new_collection);
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
		auto lhs_allof = asAllOf(lhs);
		auto rhs_allof = asAllOf(rhs);
		auto lhs_ctor_patterns = asCtorPatterns(lhs);
		auto rhs_ctor_patterns = asCtorPatterns(rhs);
		auto lhs_ctor_pattern = asCtorPattern(lhs);
		auto rhs_ctor_pattern = asCtorPattern(rhs);
		auto lhs_integers = asScalars<int64_t>(lhs);
		auto rhs_integers = asScalars<int64_t>(rhs);
		auto lhs_strings = asScalars<std::string>(lhs);
		auto rhs_strings = asScalars<std::string>(rhs);

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
					difference(std::make_shared<CtorPattern>(lhs_ctor_patterns->location, cpv), rhs, send);
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
				Pattern::ref new_a = std::make_shared<CtorPattern>(lhs->location, lhs_ctor_pattern->cpv);
				auto new_a_send = [&new_a] (Pattern::ref pattern) {
					new_a = pattern_union(new_a, pattern);
				};

				for (auto &b : rhs_ctor_patterns->cpvs) {
					auto current_a = new_a;
					new_a = theNothing;
					difference(current_a, std::make_shared<CtorPattern>(rhs->location, b), new_a_send);
				}

				send(new_a);
				return;
			} else if (rhs_ctor_pattern) {
				difference(lhs->location, lhs_ctor_pattern->cpv, rhs_ctor_pattern->cpv, send);
				return;
			}
		}

		if (lhs_integers) {
			if (rhs_integers) {
				send(difference(*lhs_integers, *rhs_integers));
				return;
			}
		}

		if (lhs_strings) {
			if (rhs_strings) {
				send(difference(*lhs_strings, *rhs_strings));
				return;
			}
		}

		log_location(log_error, lhs->location, "unhandled difference - %s \\ %s",
				lhs->str().c_str(),
				rhs->str().c_str());
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
		ss << name;
		return ss.str();
	}

	std::string Nothing::str() const {
		return "∅";
	}

	std::string CtorPattern::str() const {
		return cpv.str();
	}

	std::string CtorPatterns::str() const {
		return ::join_with(cpvs, " and ", [](const CtorPatternValue &cpv) -> std::string { return cpv.str(); });
	}

	std::string CtorPatternValue::str() const {
		std::stringstream ss;
		ss << name;
		if (args.size() != 0) {
			ss << "(" << ::join_str(args, ", ") << ")";
		}
		return ss.str();
	}

}

namespace bitter {
	using namespace ::match;
	using namespace ::types;

	Pattern::ref tuple_predicate_t::get_pattern(type_t::ref type, env_t::ref env) const {
		assert(false);
		// type = type->eval(env);

		std::vector<Pattern::ref> args;
		if (auto tuple_type = dyncast<const type_tuple_t>(type)) {
			if (tuple_type->dimensions.size() != params.size()) {
				throw user_error(location, "tuple predicate has an incorrect number of sub-patterns. there are %d, there should be %d",
						int(params.size()),
						int(tuple_type->dimensions.size()));
			}

			std::vector<Pattern::ref> args;
			for (size_t i = 0; i < params.size(); ++i) {
				args.push_back(params[i]->get_pattern(tuple_type->dimensions[i], env));
			}
			return std::make_shared<CtorPattern>(location, CtorPatternValue{tuple_type->repr(), "tuple", args});
		} else {
			throw user_error(location,
					"type mismatch on pattern. incoming type is %s. "
					"it is not a %d-tuple.", type->str().c_str(), (int)params.size());
			return nullptr;
		}
	}
	Pattern::ref ctor_predicate_t::get_pattern(type_t::ref type, env_t::ref env) const {
		assert(false);
		// type = type->eval(env);
		if (auto data_type = dyncast<const type_data_t>(type)) {
			for (auto ctor_pair : data_type->ctor_pairs) {
				if (ctor_pair.first.text == ctor_name.name) {
					std::vector<Pattern::ref> args;
					if (ctor_pair.second->args.size() != params.size()) {
						throw user_error(location, "%s has an incorrect number of sub-patterns. there are %d, there should be %d",
								ctor_name.name.c_str(),
								int(params.size()),
								int(ctor_pair.second->args.size()));
					}

					for (size_t i = 0; i < params.size(); ++i) {
						args.push_back(params[i]->get_pattern(ctor_pair.second->args[i], env));
					}

					/* found the ctor we're matching on */
					return std::make_shared<CtorPattern>(
							location,
						   	CtorPatternValue{type->repr(),
						   	ctor_name.name,
						   	args});
				}
			}

			throw user_error(location, "invalid ctor, " c_id("%s") " is not a member of %s",
					ctor_name.name.c_str(), type->str().c_str());
			return nullptr;
		} else {
			throw user_error(location, "type mismatch on pattern. incoming type is %s. "
				   	c_id("%s") " cannot match it.", type->str().c_str(), ctor_name.name.c_str());
			return nullptr;
		}
	}
	Pattern::ref irrefutable_predicate_t::get_pattern(type_t::ref type, env_t::ref env) const {
		return std::make_shared<AllOf>(location, name_assignment, env, type);
	}
	Pattern::ref literal_t::get_pattern(type_t::ref type, env_t::ref env) const {
		if (type_equality(type, type_int(INTERNAL_LOC()))) {
			if (token.tk == tk_integer) {
				int64_t value = parse_int_value(token);
				return std::make_shared<Scalars<int64_t>>(token.location, Scalars<int64_t>::Include, std::set<int64_t>{value});
			} else if (token.tk == tk_identifier) {
				return std::make_shared<Scalars<int64_t>>(token.location, Scalars<int64_t>::Exclude, std::set<int64_t>{});
			}
		} else if (type_equality(type, type_string(INTERNAL_LOC()))) {
			if (token.tk == tk_string) {
				std::string value = unescape_json_quotes(token.text);
				return std::make_shared<Scalars<std::string>>(token.location, Scalars<std::string>::Include, std::set<std::string>{value});
			} else if (token.tk == tk_identifier) {
				return std::make_shared<Scalars<std::string>>(token.location, Scalars<std::string>::Exclude, std::set<std::string>{});
			}
		}

		throw user_error(token.location, "invalid type for literal '%s'. should be a %s",
				token.text.c_str(),
				type->str().c_str());
		return nullptr;
	}
}
