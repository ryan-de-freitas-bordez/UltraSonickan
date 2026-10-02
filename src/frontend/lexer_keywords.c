#include "usk/lexer.h"

#include <string.h>

static const UskKeywordMetadata keywords[] = {
    {"use", USK_KW_USE, USK_KEYWORD_CATEGORY_MODULE, false},
    {"fn", USK_KW_FN, USK_KEYWORD_CATEGORY_DECLARATION, false},
    {"var", USK_KW_VAR, USK_KEYWORD_CATEGORY_DECLARATION, false},
    {"const", USK_KW_CONST, USK_KEYWORD_CATEGORY_DECLARATION, false},
    {"if", USK_KW_IF, USK_KEYWORD_CATEGORY_CONTROL_FLOW, false},
    {"else", USK_KW_ELSE, USK_KEYWORD_CATEGORY_CONTROL_FLOW, false},
    {"elif", USK_KW_ELIF, USK_KEYWORD_CATEGORY_CONTROL_FLOW, false},
    {"enum", USK_KW_ENUM, USK_KEYWORD_CATEGORY_DECLARATION, false},
    {"class", USK_KW_CLASS, USK_KEYWORD_CATEGORY_DECLARATION, false},
    {"struct", USK_KW_STRUCT, USK_KEYWORD_CATEGORY_DECLARATION, false},
    {"USKInt", USK_KW_INT, USK_KEYWORD_CATEGORY_TYPE, false},
    {"USKDouble", USK_KW_DOUBLE, USK_KEYWORD_CATEGORY_TYPE, false},
    {"USKString", USK_KW_STRING, USK_KEYWORD_CATEGORY_TYPE, false},
    {"USKChar", USK_KW_CHAR, USK_KEYWORD_CATEGORY_TYPE, false},
    {"USKFloat", USK_KW_FLOAT, USK_KEYWORD_CATEGORY_TYPE, false},
    {"USKBool", USK_KW_BOOL, USK_KEYWORD_CATEGORY_TYPE, false},
    {"USKNull", USK_KW_NULL_TYPE, USK_KEYWORD_CATEGORY_TYPE, false},
    {"typedef", USK_KW_TYPEDEF, USK_KEYWORD_CATEGORY_DECLARATION, false},
    {"switch", USK_KW_SWITCH, USK_KEYWORD_CATEGORY_CONTROL_FLOW, false},
    {"case", USK_KW_CASE, USK_KEYWORD_CATEGORY_CONTROL_FLOW, false},
    {"break", USK_KW_BREAK, USK_KEYWORD_CATEGORY_CONTROL_FLOW, false},
    {"continue", USK_KW_CONTINUE, USK_KEYWORD_CATEGORY_CONTROL_FLOW, false},
    {"default", USK_KW_DEFAULT, USK_KEYWORD_CATEGORY_CONTROL_FLOW, false},
    {"define", USK_KW_DEFINE, USK_KEYWORD_CATEGORY_PREPROCESSOR, false},
    {"ifndef", USK_KW_IFNDEF, USK_KEYWORD_CATEGORY_PREPROCESSOR, false},
    {"endif", USK_KW_ENDIF, USK_KEYWORD_CATEGORY_PREPROCESSOR, false},
    {"extern", USK_KW_EXTERN, USK_KEYWORD_CATEGORY_DECLARATION, false},
    {"public", USK_KW_PUBLIC, USK_KEYWORD_CATEGORY_MODIFIER, false},
    {"private", USK_KW_PRIVATE, USK_KEYWORD_CATEGORY_MODIFIER, false},
    {"for", USK_KW_FOR, USK_KEYWORD_CATEGORY_CONTROL_FLOW, false},
    {"in", USK_KW_IN, USK_KEYWORD_CATEGORY_CONTROL_FLOW, false},
    {"USKAuto", USK_KW_AUTO, USK_KEYWORD_CATEGORY_TYPE, false},
    {"USKLong", USK_KW_LONG, USK_KEYWORD_CATEGORY_MODIFIER, false},
    {"USKShort", USK_KW_SHORT, USK_KEYWORD_CATEGORY_MODIFIER, false},
    {"inline", USK_KW_INLINE, USK_KEYWORD_CATEGORY_MODIFIER, false},
    {"static", USK_KW_STATIC, USK_KEYWORD_CATEGORY_MODIFIER, false},
    {"signed", USK_KW_SIGNED, USK_KEYWORD_CATEGORY_MODIFIER, false},
    {"unsigned", USK_KW_UNSIGNED, USK_KEYWORD_CATEGORY_MODIFIER, false},
    {"import", USK_KW_IMPORT, USK_KEYWORD_CATEGORY_MODULE, false},
    {"typeof", USK_KW_TYPEOF, USK_KEYWORD_CATEGORY_TYPE, false},
    {"while", USK_KW_WHILE, USK_KEYWORD_CATEGORY_CONTROL_FLOW, false},
    {"volatile", USK_KW_VOLATILE, USK_KEYWORD_CATEGORY_MODIFIER, false},
    {"true", USK_KW_TRUE, USK_KEYWORD_CATEGORY_LITERAL, false},
    {"false", USK_KW_FALSE, USK_KEYWORD_CATEGORY_LITERAL, false},
    {"pub", USK_KW_PUB_ALIAS, USK_KEYWORD_CATEGORY_MODIFIER, true}
};

size_t usk_keyword_count(void) {
    return sizeof(keywords) / sizeof(keywords[0]);
}

const UskKeywordMetadata *usk_keyword_metadata_at(size_t index) {
    return index < usk_keyword_count() ? &keywords[index] : NULL;
}

const UskKeywordMetadata *usk_keyword_metadata(TokenKeyword keyword) {
    for (size_t index = 0; index < usk_keyword_count(); ++index)
        if (keywords[index].keyword == keyword) return &keywords[index];
    return NULL;
}

TokenKeyword usk_keyword_from_text(const char *text) {
    if (!text) return USK_KW_NONE;
    for (size_t index = 0; index < usk_keyword_count(); ++index)
        if (strcmp(keywords[index].spelling, text) == 0)
            return keywords[index].keyword;
    return USK_KW_NONE;
}

const char *usk_keyword_name(TokenKeyword keyword) {
    const UskKeywordMetadata *metadata = usk_keyword_metadata(keyword);
    return metadata ? metadata->spelling :
           keyword == USK_KW_NONE ? "identifier" : "<invalid-keyword>";
}

UskKeywordCategory usk_keyword_category(TokenKeyword keyword) {
    const UskKeywordMetadata *metadata = usk_keyword_metadata(keyword);
    return metadata ? metadata->category : USK_KEYWORD_CATEGORY_NONE;
}

bool usk_token_is_keyword(const Token *token, TokenKeyword keyword) {
    return token && token->kind == TK_ID && token->keyword == keyword;
}

bool usk_keyword_is_reserved(TokenKeyword keyword) {
    return usk_keyword_metadata(keyword) != NULL;
}

bool usk_keyword_is_type(TokenKeyword keyword) {
    return usk_keyword_category(keyword) == USK_KEYWORD_CATEGORY_TYPE;
}

bool usk_keyword_is_control_flow(TokenKeyword keyword) {
    return usk_keyword_category(keyword) == USK_KEYWORD_CATEGORY_CONTROL_FLOW;
}

bool usk_keyword_is_modifier(TokenKeyword keyword) {
    return usk_keyword_category(keyword) == USK_KEYWORD_CATEGORY_MODIFIER;
}

bool usk_keyword_is_preprocessor(TokenKeyword keyword) {
    return usk_keyword_category(keyword) == USK_KEYWORD_CATEGORY_PREPROCESSOR;
}
