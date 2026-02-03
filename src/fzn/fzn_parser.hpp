/**
 * @file fzn_parser.hpp
 * @brief FlatZincパーサーのインターフェース
 */
#ifndef SABORI_CSP_FZN_PARSER_HPP
#define SABORI_CSP_FZN_PARSER_HPP

#include "sabori_csp/fzn/model.hpp"
#include <memory>
#include <string>

// Forward declarations for flex/bison
typedef void* yyscan_t;
struct ParserContext;

// Flex functions
int yylex_init(yyscan_t* scanner);
int yylex_destroy(yyscan_t scanner);
void yyset_in(FILE* in, yyscan_t scanner);
struct yy_buffer_state;
typedef struct yy_buffer_state* YY_BUFFER_STATE;
YY_BUFFER_STATE yy_scan_string(const char* str, yyscan_t scanner);
void yy_delete_buffer(YY_BUFFER_STATE buffer, yyscan_t scanner);

// Bison function
int yyparse(yyscan_t scanner, ParserContext* ctx);

namespace sabori_csp {
namespace fzn {

std::unique_ptr<Model> parse_file(const std::string& filename);
std::unique_ptr<Model> parse_string(const std::string& input);

} // namespace fzn
} // namespace sabori_csp

#endif // SABORI_CSP_FZN_PARSER_HPP
