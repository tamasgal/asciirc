#ifndef ASCIIRC_JSON_H
#define ASCIIRC_JSON_H

#include <stddef.h>

typedef enum {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT
} JsonType;

typedef struct Json Json;

struct Json {
	JsonType type;
	double num;
	int boolean;
	char *str;
	Json **items;
	char **keys;
	size_t len;
};

Json *json_parse(const char *s, size_t n);
void json_free(Json *v);

Json *json_get(const Json *obj, const char *key);
Json *json_path(const Json *v, const char *path);
Json *json_at(const Json *arr, size_t i);
size_t json_len(const Json *v);
const char *json_str(const Json *v);
const char *json_get_str(const Json *obj, const char *key);
double json_num(const Json *v, double def);
int json_bool(const Json *v);

#endif
