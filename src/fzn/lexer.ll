%{
#include <string>
#include <cstdlib>
#include "parser.hpp"

// Track location for error messages
#define YY_USER_ACTION yylloc->first_line = yylloc->last_line = yylineno;
%}

%option noyywrap
%option yylineno
%option nounput
%option noinput
%option reentrant
%option bison-bridge
%option bison-locations

DIGIT       [0-9]
LETTER      [a-zA-Z_]
ID          {LETTER}({LETTER}|{DIGIT})*

%%

[ \t\r]+        { /* skip whitespace */ }
\n              { /* skip newline */ }
"%"[^\n]*       { /* skip comments */ }

"array"         { return ARRAY; }
"bool"          { return BOOL; }
"constraint"    { return CONSTRAINT; }
"false"         { return FALSE_LITERAL; }
"float"         { return FLOAT; }
"int"           { return INT; }
"maximize"      { return MAXIMIZE; }
"minimize"      { return MINIMIZE; }
"of"            { return OF; }
"par"           { return PAR; }
"predicate"     { return PREDICATE; }
"satisfy"       { return SATISFY; }
"set"           { return SET; }
"solve"         { return SOLVE; }
"true"          { return TRUE_LITERAL; }
"var"           { return VAR; }

".."            { return DOTDOT; }
"::"            { return COLONCOLON; }

[,;:\[\]()={}]  { return yytext[0]; }

"-"?{DIGIT}+    {
                    yylval->int_val = std::stoll(yytext);
                    return INT_LITERAL;
                }

"-"?{DIGIT}+"."{DIGIT}+([eE][+-]?{DIGIT}+)? {
                    yylval->float_val = std::stod(yytext);
                    return FLOAT_LITERAL;
                }

{ID}            {
                    yylval->str_val = new std::string(yytext);
                    return IDENTIFIER;
                }

\"[^\"]*\"      {
                    // String literal - remove quotes
                    std::string s(yytext + 1);
                    s.pop_back();
                    yylval->str_val = new std::string(s);
                    return STRING_LITERAL;
                }

.               { return yytext[0]; }

%%
