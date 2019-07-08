#include "parse_state.h"

#include <cstdarg>

#include "builtins.h"
#include "compiler.h"
#include "dbg.h"
#include "disk.h"
#include "logger_decls.h"
#include "parser.h"
#include "types.h"
#include "zion.h"

namespace zion {

ParseState::ParseState(std::string filename,
                       std::string module_name,
                       Lexer &lexer,
                       std::vector<Token> &comments,
                       std::set<LinkIn> &link_ins,
                       const std::map<std::string, int> &builtin_arities)
    : filename(filename),
      module_name(module_name.size() != 0
                      ? module_name
                      : strip_zion_extension(leaf_from_file_path(filename))),
      lexer(lexer), comments(comments), link_ins(link_ins),
      builtin_arities(builtin_arities) {
  advance();
}

bool ParseState::advance() {
  debug_lexer(log(log_info, "advanced from %s %s", tkstr(token.tk),
                  token.text.c_str()[0] != '\n' ? token.text.c_str() : ""));
  prior_token = token;
  return lexer.get_token(token, newline, &comments);
}

Token ParseState::token_and_advance() {
  advance();
  return prior_token;
}

Identifier ParseState::identifier_and_advance() {
  assert(token.tk == tk_identifier);
  advance();
  assert(prior_token.tk == tk_identifier);
  return id_mapped(Identifier{prior_token.text, prior_token.location});
}

Identifier ParseState::id_mapped(Identifier id) {
  auto iter = term_map.find(id.name);
  if (iter != term_map.end()) {
    return Identifier{iter->second, id.location};
  } else {
    return id;
  }
}

bool ParseState::is_mutable_var(std::string name) {
  for (auto iter = scopes.rbegin(); iter != scopes.rend(); ++iter) {
    if ((*iter).id.name == name) {
      return (*iter).is_let;
    }
  }
  return false;
}

void ParseState::error(const char *format, ...) {
  va_list args;
  va_start(args, format);
  auto error = user_error(token.location, format, args);
  va_end(args);
  if (lexer.eof()) {
    error.add_info(token.location, "encountered end-of-file");
  }
  throw error;
}

void ParseState::add_term_map(Location location,
                              std::string key,
                              std::string value) {
  // log("adding %s to term map => %s", key.c_str(), value.c_str());
  if (in(key, term_map)) {
    throw user_error(location, "symbol imported twice");
  }
  term_map[key] = value;
}

} // namespace zion
