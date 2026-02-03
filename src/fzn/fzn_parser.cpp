#include "fzn_parser.hpp"
#include "parser.hpp"
#include <stdexcept>
#include <cstdio>

namespace sabori_csp {
namespace fzn {

std::unique_ptr<Model> parse_file(const std::string& filename) {
    FILE* file = fopen(filename.c_str(), "r");
    if (!file) {
        throw std::runtime_error("Cannot open file: " + filename);
    }

    yyscan_t scanner;
    yylex_init(&scanner);
    yyset_in(file, scanner);

    ParserContext ctx;
    int result = yyparse(scanner, &ctx);

    yylex_destroy(scanner);
    fclose(file);

    if (result != 0 || ctx.has_error) {
        throw std::runtime_error("Parse error: " + ctx.error_message);
    }

    return std::move(ctx.model);
}

std::unique_ptr<Model> parse_string(const std::string& input) {
    yyscan_t scanner;
    yylex_init(&scanner);

    YY_BUFFER_STATE buffer = yy_scan_string(input.c_str(), scanner);

    ParserContext ctx;
    int result = yyparse(scanner, &ctx);

    yy_delete_buffer(buffer, scanner);
    yylex_destroy(scanner);

    if (result != 0 || ctx.has_error) {
        throw std::runtime_error("Parse error: " + ctx.error_message);
    }

    return std::move(ctx.model);
}

} // namespace fzn
} // namespace sabori_csp
