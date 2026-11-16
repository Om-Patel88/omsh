#ifndef OMSH_PARSER_H
#define OMSH_PARSER_H

#include <stdbool.h>
#include <stddef.h>

#include "lexer.h"

enum redir_type { R_IN, R_OUT, R_APPEND, R_ERR, R_ERR_APPEND, R_ERR_TO_OUT };

struct redir {
    enum redir_type type;
    struct word *target; /* NULL for R_ERR_TO_OUT */
};

enum node_kind { N_LIST, N_AND, N_OR, N_PIPELINE, N_SIMPLE, N_SUBSHELL };

struct list_item {
    struct node *node;
    bool background;
    char *text; /* source text of the item, used for job listings */
};

struct node {
    enum node_kind kind;
    union {
        struct {
            struct list_item *items;
            size_t n;
        } list;
        struct { /* N_AND, N_OR */
            struct node *left, *right;
        } bin;
        struct {
            struct node **cmds;
            size_t n;
            bool negate;
        } pipe;
        struct {
            struct word **words;
            size_t nwords;
            struct redir *redirs;
            size_t nredirs;
        } simple;
        struct {
            struct node *body;
            struct redir *redirs;
            size_t nredirs;
        } sub;
    };
};

/*
 * Build an AST from tokens. Takes ownership of the words it uses.
 * Returns NULL for empty input (with *err NULL) or on a syntax error
 * (with *err set to the offending token's text).
 */
struct node *parse(struct tokens *t, const char *line, const char **err);
void ast_free(struct node *n);

#endif
