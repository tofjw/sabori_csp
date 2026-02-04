%code requires {
#include <string>
#include <vector>
#include <memory>
#include "sabori_csp/fzn/model.hpp"

typedef void* yyscan_t;

struct ParserContext {
    std::unique_ptr<sabori_csp::fzn::Model> model;
    std::string error_message;
    bool has_error = false;

    ParserContext() : model(std::make_unique<sabori_csp::fzn::Model>()) {}
};
}

%code provides {
int yylex(YYSTYPE* yylval, YYLTYPE* yylloc, yyscan_t scanner);
void yyerror(YYLTYPE* loc, yyscan_t scanner, ParserContext* ctx, const char* msg);
}

%{
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include "sabori_csp/fzn/model.hpp"

using namespace sabori_csp;
using namespace sabori_csp::fzn;
%}

%define api.pure full
%define parse.error verbose
%locations

%lex-param { yyscan_t scanner }
%parse-param { yyscan_t scanner }
%parse-param { ParserContext* ctx }

%union {
    int64_t int_val;
    double float_val;
    std::string* str_val;
    std::vector<int64_t>* int_list;
    std::vector<std::string>* str_list;
    sabori_csp::fzn::ConstraintArg* constraint_arg;
    std::vector<sabori_csp::fzn::ConstraintArg>* constraint_args;
}

%token ARRAY BOOL CONSTRAINT FALSE_LITERAL FLOAT INT
%token MAXIMIZE MINIMIZE OF PAR PREDICATE
%token SATISFY SET SOLVE TRUE_LITERAL VAR
%token DOTDOT COLONCOLON

%token <int_val> INT_LITERAL
%token <float_val> FLOAT_LITERAL
%token <str_val> IDENTIFIER STRING_LITERAL

%type <int_val> int_literal bool_literal
%type <str_val> identifier
%type <int_list> int_list int_list_inner
%type <str_list> id_list id_list_inner annotations annotation_list annotation
%type <constraint_arg> constraint_arg
%type <constraint_args> constraint_args

%destructor { delete $$; } <str_val>
%destructor { delete $$; } <int_list>
%destructor { delete $$; } <str_list>
%destructor { delete $$; } <constraint_arg>
%destructor { delete $$; } <constraint_args>

%%

model:
    items
    ;

items:
    /* empty */
    | items item
    ;

item:
    var_decl
    | constraint_decl
    | solve_decl
    | predicate_decl
    ;

predicate_decl:
    PREDICATE identifier '(' predicate_params ')' ';'
        { delete $2; /* Ignore predicate declarations */ }
    ;

predicate_params:
    /* empty */
    | predicate_param
    | predicate_params ',' predicate_param
    ;

predicate_param:
    type_inst ':' identifier
        { delete $3; }
    ;

var_decl:
    VAR INT ':' identifier annotations '=' int_literal ';'
        {
            VarDecl decl;
            decl.name = *$4;
            decl.lb = $7;
            decl.ub = $7;
            decl.fixed_value = $7;
            if ($5) {
                for (const auto& ann : *$5) {
                    if (ann == "output_var") decl.is_output = true;
                }
                delete $5;
            }
            ctx->model->add_var_decl(std::move(decl));
            delete $4;
        }
    | VAR int_literal DOTDOT int_literal ':' identifier annotations ';'
        {
            VarDecl decl;
            decl.name = *$6;
            decl.lb = $2;
            decl.ub = $4;
            if ($7) {
                for (const auto& ann : *$7) {
                    if (ann == "output_var") decl.is_output = true;
                }
                delete $7;
            }
            ctx->model->add_var_decl(std::move(decl));
            delete $6;
        }
    | VAR int_literal DOTDOT int_literal ':' identifier annotations '=' int_literal ';'
        {
            VarDecl decl;
            decl.name = *$6;
            decl.lb = $2;
            decl.ub = $4;
            decl.fixed_value = $9;
            if ($7) {
                for (const auto& ann : *$7) {
                    if (ann == "output_var") decl.is_output = true;
                }
                delete $7;
            }
            ctx->model->add_var_decl(std::move(decl));
            delete $6;
        }
    | VAR INT ':' identifier annotations ';'
        {
            // var int with no explicit bounds - use default range
            VarDecl decl;
            decl.name = *$4;
            decl.lb = -1000000000;  // Default lower bound for unbounded int
            decl.ub = 1000000000;   // Default upper bound for unbounded int
            if ($5) {
                for (const auto& ann : *$5) {
                    if (ann == "output_var") decl.is_output = true;
                }
                delete $5;
            }
            ctx->model->add_var_decl(std::move(decl));
            delete $4;
        }
    | VAR BOOL ':' identifier annotations ';'
        {
            VarDecl decl;
            decl.name = *$4;
            decl.lb = 0;
            decl.ub = 1;
            decl.is_bool = true;
            if ($5) {
                for (const auto& ann : *$5) {
                    if (ann == "output_var") decl.is_output = true;
                }
                delete $5;
            }
            ctx->model->add_var_decl(std::move(decl));
            delete $4;
        }
    | VAR BOOL ':' identifier annotations '=' bool_literal ';'
        {
            VarDecl decl;
            decl.name = *$4;
            decl.lb = 0;
            decl.ub = 1;
            decl.is_bool = true;
            decl.fixed_value = $7;
            if ($5) {
                for (const auto& ann : *$5) {
                    if (ann == "output_var") decl.is_output = true;
                }
                delete $5;
            }
            ctx->model->add_var_decl(std::move(decl));
            delete $4;
        }
    | VAR SET OF int_literal DOTDOT int_literal ':' identifier annotations ';'
        {
            // Set variables - for now treat as integer variable
            VarDecl decl;
            decl.name = *$8;
            decl.lb = $4;
            decl.ub = $6;
            if ($9) {
                for (const auto& ann : *$9) {
                    if (ann == "output_var") decl.is_output = true;
                }
                delete $9;
            }
            ctx->model->add_var_decl(std::move(decl));
            delete $8;
        }
    | ARRAY '[' int_literal DOTDOT int_literal ']' OF VAR int_literal DOTDOT int_literal ':' identifier annotations ';'
        {
            ArrayDecl decl;
            decl.name = *$13;
            decl.size = $5 - $3 + 1;
            if ($14) {
                for (const auto& ann : *$14) {
                    if (ann == "output_array") decl.is_output = true;
                }
                delete $14;
            }
            // Create individual variables for array elements
            for (size_t i = 0; i < decl.size; ++i) {
                VarDecl vdecl;
                vdecl.name = decl.name + "[" + std::to_string($3 + i) + "]";
                vdecl.lb = $9;
                vdecl.ub = $11;
                ctx->model->add_var_decl(std::move(vdecl));
                decl.elements.push_back(decl.name + "[" + std::to_string($3 + i) + "]");
            }
            ctx->model->add_array_decl(std::move(decl));
            delete $13;
        }
    | ARRAY '[' int_literal DOTDOT int_literal ']' OF VAR int_literal DOTDOT int_literal ':' identifier annotations '=' '[' id_list_inner ']' ';'
        {
            ArrayDecl decl;
            decl.name = *$13;
            decl.size = $5 - $3 + 1;
            if ($14) {
                for (const auto& ann : *$14) {
                    if (ann == "output_array") decl.is_output = true;
                }
                delete $14;
            }
            if ($17) {
                decl.elements = *$17;
                delete $17;
            }
            ctx->model->add_array_decl(std::move(decl));
            delete $13;
        }
    | ARRAY '[' int_literal DOTDOT int_literal ']' OF VAR BOOL ':' identifier annotations ';'
        {
            ArrayDecl decl;
            decl.name = *$11;
            decl.size = $5 - $3 + 1;
            decl.is_bool = true;
            if ($12) {
                for (const auto& ann : *$12) {
                    if (ann == "output_array") decl.is_output = true;
                }
                delete $12;
            }
            // Create individual bool variables for array elements
            for (size_t i = 0; i < decl.size; ++i) {
                VarDecl vdecl;
                vdecl.name = decl.name + "[" + std::to_string($3 + i) + "]";
                vdecl.lb = 0;
                vdecl.ub = 1;
                vdecl.is_bool = true;
                ctx->model->add_var_decl(std::move(vdecl));
                decl.elements.push_back(decl.name + "[" + std::to_string($3 + i) + "]");
            }
            ctx->model->add_array_decl(std::move(decl));
            delete $11;
        }
    | ARRAY '[' int_literal DOTDOT int_literal ']' OF VAR BOOL ':' identifier annotations '=' '[' id_list_inner ']' ';'
        {
            // Array of var bool with assignment (mixed identifiers and bool literals)
            ArrayDecl decl;
            decl.name = *$11;
            decl.size = $5 - $3 + 1;
            decl.is_bool = true;
            if ($12) {
                for (const auto& ann : *$12) {
                    if (ann == "output_array") decl.is_output = true;
                }
                delete $12;
            }
            if ($15) {
                decl.elements = *$15;
                delete $15;
            }
            ctx->model->add_array_decl(std::move(decl));
            delete $11;
        }
    | ARRAY '[' int_literal DOTDOT int_literal ']' OF VAR INT ':' identifier annotations ';'
        {
            // Array of var int (unbounded)
            ArrayDecl decl;
            decl.name = *$11;
            decl.size = $5 - $3 + 1;
            if ($12) {
                for (const auto& ann : *$12) {
                    if (ann == "output_array") decl.is_output = true;
                }
                delete $12;
            }
            // Create individual var int variables for array elements
            for (size_t i = 0; i < decl.size; ++i) {
                VarDecl vdecl;
                vdecl.name = decl.name + "[" + std::to_string($3 + i) + "]";
                vdecl.lb = -1000000000;
                vdecl.ub = 1000000000;
                ctx->model->add_var_decl(std::move(vdecl));
                decl.elements.push_back(decl.name + "[" + std::to_string($3 + i) + "]");
            }
            ctx->model->add_array_decl(std::move(decl));
            delete $11;
        }
    | ARRAY '[' int_literal DOTDOT int_literal ']' OF VAR INT ':' identifier annotations '=' '[' id_list_inner ']' ';'
        {
            // Array of var int (unbounded) with assignment
            ArrayDecl decl;
            decl.name = *$11;
            decl.size = $5 - $3 + 1;
            if ($12) {
                for (const auto& ann : *$12) {
                    if (ann == "output_array") decl.is_output = true;
                }
                delete $12;
            }
            if ($15) {
                decl.elements = *$15;
                delete $15;
            }
            ctx->model->add_array_decl(std::move(decl));
            delete $11;
        }
    | ARRAY '[' int_literal DOTDOT int_literal ']' OF INT ':' identifier annotations '=' '[' int_list_inner ']' ';'
        {
            // Array of par int (constants)
            ArrayDecl decl;
            decl.name = *$10;
            decl.size = $5 - $3 + 1;
            if ($11) {
                for (const auto& ann : *$11) {
                    if (ann == "output_array") decl.is_output = true;
                }
                delete $11;
            }
            if ($14) {
                for (size_t i = 0; i < $14->size(); ++i) {
                    std::string elem_name = decl.name + "[" + std::to_string($3 + i) + "]";
                    VarDecl vdecl;
                    vdecl.name = elem_name;
                    vdecl.lb = (*$14)[i];
                    vdecl.ub = (*$14)[i];
                    vdecl.fixed_value = (*$14)[i];
                    ctx->model->add_var_decl(std::move(vdecl));
                    decl.elements.push_back(elem_name);
                }
                delete $14;
            }
            ctx->model->add_array_decl(std::move(decl));
            delete $10;
        }
    | INT ':' identifier annotations '=' int_literal ';'
        {
            // par int constant
            VarDecl decl;
            decl.name = *$3;
            decl.lb = $6;
            decl.ub = $6;
            decl.fixed_value = $6;
            if ($4) delete $4;
            ctx->model->add_var_decl(std::move(decl));
            delete $3;
        }
    ;

constraint_decl:
    CONSTRAINT identifier '(' constraint_args ')' annotations ';'
        {
            ConstraintDecl decl;
            decl.name = *$2;
            if ($4) {
                decl.args = std::move(*$4);
                delete $4;
            }
            ctx->model->add_constraint_decl(std::move(decl));
            delete $2;
            if ($6) delete $6;
        }
    ;

constraint_args:
    /* empty */
        { $$ = new std::vector<ConstraintArg>(); }
    | constraint_arg
        {
            $$ = new std::vector<ConstraintArg>();
            $$->push_back(std::move(*$1));
            delete $1;
        }
    | constraint_args ',' constraint_arg
        {
            $$ = $1;
            $$->push_back(std::move(*$3));
            delete $3;
        }
    ;

constraint_arg:
    int_literal
        { $$ = new ConstraintArg($1); }
    | bool_literal
        { $$ = new ConstraintArg($1); }
    | identifier
        {
            $$ = new ConstraintArg(*$1);
            delete $1;
        }
    | '[' int_list_inner ']'
        {
            $$ = new ConstraintArg(std::move(*$2));
            delete $2;
        }
    | '[' id_list_inner ']'
        {
            $$ = new ConstraintArg(std::move(*$2));
            delete $2;
        }
    | '[' ']'
        { $$ = new ConstraintArg(std::vector<int64_t>()); }
    ;

solve_decl:
    SOLVE annotations SATISFY ';'
        {
            SolveDecl decl;
            decl.kind = SolveKind::Satisfy;
            ctx->model->set_solve_decl(std::move(decl));
            if ($2) delete $2;
        }
    | SOLVE annotations MINIMIZE identifier ';'
        {
            SolveDecl decl;
            decl.kind = SolveKind::Minimize;
            decl.objective_var = *$4;
            ctx->model->set_solve_decl(std::move(decl));
            delete $4;
            if ($2) delete $2;
        }
    | SOLVE annotations MAXIMIZE identifier ';'
        {
            SolveDecl decl;
            decl.kind = SolveKind::Maximize;
            decl.objective_var = *$4;
            ctx->model->set_solve_decl(std::move(decl));
            delete $4;
            if ($2) delete $2;
        }
    ;

annotations:
    /* empty */
        { $$ = nullptr; }
    | COLONCOLON annotation_list
        { $$ = $2; }
    ;

annotation_list:
    annotation
        { $$ = $1; }
    | annotation_list COLONCOLON annotation
        {
            $$ = $1;
            if ($3) {
                for (auto& s : *$3) {
                    $$->push_back(std::move(s));
                }
                delete $3;
            }
        }
    ;

annotation:
    identifier
        {
            $$ = new std::vector<std::string>();
            $$->push_back(*$1);
            delete $1;
        }
    | identifier '(' annotation_args ')'
        {
            $$ = new std::vector<std::string>();
            $$->push_back(*$1);
            delete $1;
        }
    ;

annotation_args:
    annotation_arg
    | annotation_args ',' annotation_arg
    ;

annotation_arg:
    int_literal
    | identifier
        { delete $1; }
    | '[' ']'
    | '[' int_list_inner ']'
        { delete $2; }
    | '[' id_list_inner ']'
        { delete $2; }
    | '[' int_literal DOTDOT int_literal ']'
        { /* range like [1..4] - ignore */ }
    | STRING_LITERAL
        { delete $1; }
    ;

type_inst:
    VAR INT
    | VAR BOOL
    | VAR int_literal DOTDOT int_literal
    | VAR SET OF INT
    | VAR SET OF int_literal DOTDOT int_literal
    | INT
    | BOOL
    | ARRAY '[' INT ']' OF VAR INT
    | ARRAY '[' INT ']' OF VAR BOOL
    | ARRAY '[' INT ']' OF VAR int_literal DOTDOT int_literal
    | ARRAY '[' INT ']' OF INT
    | ARRAY '[' INT ']' OF BOOL
    ;

int_list:
    '[' int_list_inner ']'
        { $$ = $2; }
    | '[' ']'
        { $$ = new std::vector<int64_t>(); }
    ;

int_list_inner:
    int_literal
        {
            $$ = new std::vector<int64_t>();
            $$->push_back($1);
        }
    | int_list_inner ',' int_literal
        {
            $$ = $1;
            $$->push_back($3);
        }
    ;

id_list:
    '[' id_list_inner ']'
        { $$ = $2; }
    | '[' ']'
        { $$ = new std::vector<std::string>(); }
    ;

id_list_inner:
    identifier
        {
            $$ = new std::vector<std::string>();
            $$->push_back(*$1);
            delete $1;
        }
    | id_list_inner ',' identifier
        {
            $$ = $1;
            $$->push_back(*$3);
            delete $3;
        }
    | id_list_inner ',' int_literal
        {
            // Support mixed lists with inline integer literals
            // Convert the literal to a constant variable name
            $$ = $1;
            $$->push_back("__inline_" + std::to_string($3));
        }
    | int_literal
        {
            // Support starting with an integer literal
            $$ = new std::vector<std::string>();
            $$->push_back("__inline_" + std::to_string($1));
        }
    | id_list_inner ',' bool_literal
        {
            // Support mixed lists with inline bool literals (true/false)
            $$ = $1;
            $$->push_back("__inline_" + std::to_string($3));
        }
    | bool_literal
        {
            // Support starting with a bool literal
            $$ = new std::vector<std::string>();
            $$->push_back("__inline_" + std::to_string($1));
        }
    ;

int_literal:
    INT_LITERAL
        { $$ = $1; }
    ;

bool_literal:
    TRUE_LITERAL
        { $$ = 1; }
    | FALSE_LITERAL
        { $$ = 0; }
    ;

identifier:
    IDENTIFIER
        { $$ = $1; }
    ;

%%

void yyerror(YYLTYPE* loc, yyscan_t scanner, ParserContext* ctx, const char* msg) {
    (void)scanner;
    ctx->has_error = true;
    ctx->error_message = std::string(msg) + " at line " + std::to_string(loc->first_line);
}
