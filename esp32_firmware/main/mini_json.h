#pragma once

// parser JSON minimo para los comandos (sin memoria dinamica)

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MJ_UNDEF = 0,
    MJ_OBJECT,
    MJ_ARRAY,
    MJ_STRING,
    MJ_PRIMITIVE,
} mj_type_t;

typedef struct {
    mj_type_t type;
    int start;   // en strings no incluye las comillas
    int end;
    int size;    // hijos directos (objeto: claves, arreglo: elementos)
    int parent;
} mj_tok_t;

#define MJ_ERR_NOMEM (-1)
#define MJ_ERR_INVAL (-2)

// retorna la cantidad de tokens o un error negativo
int mj_parse(const char *js, size_t len, mj_tok_t *toks, int max_toks);

// indice del valor asociado a key dentro del objeto obj, o -1
int mj_find(const char *js, const mj_tok_t *t, int n, int obj, const char *key);

// siguiente hijo directo de parent a partir de from (exclusivo), o -1
int mj_next_child(const mj_tok_t *t, int n, int parent, int from);

bool mj_eq(const char *js, const mj_tok_t *t, const char *s);
bool mj_number(const char *js, const mj_tok_t *t, double *out);
bool mj_bool(const char *js, const mj_tok_t *t, bool *out);
