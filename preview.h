#ifndef PREVIEW_H
#define PREVIEW_H

#include <stdlib.h>

typedef struct {
    char *name, *ext, *type, *subtype, *script;
    int priority;
    size_t script_len;
} Preview;

typedef struct {
    char *ctpv, *f, *w, *h, *x, *y, *id;
} PreviewArgs;

void init_previews(Preview *ps, size_t len);
void cleanup_previews(void);
int run_preview(const char *ext, const char *mimetype, PreviewArgs *pa);
Preview **get_previews_list(size_t *len);

#endif
