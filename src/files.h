#ifndef ASCIIRC_FILES_H
#define ASCIIRC_FILES_H

#include <stddef.h>

#include "util.h"

const char *path_token(const char *s, char **out);
size_t path_files(const char *s, char ***paths);
void path_escape(Buf *b, const char *path);
size_t path_complete(const char *typed, char ***out);
const char *mime_type(const char *name);
const char *path_base(const char *path);
void path_list_free(char **list, size_t n);

#endif
