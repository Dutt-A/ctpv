#include <ctype.h>
#include <assert.h>

#include "error.h"
#include "lexer.h"
#include "vector.h"

#define PARSEERROR(c, format, ...)                                     \
    print_errorf("config parse error:%u:%u " format, (c).line, (c).col \
                 __VA_OPT__(, ) __VA_ARGS__)

#define TOK_TYPE(t) ((Token){ .type = t })

#define NULL_TOK TOK_TYPE(TOK_NULL)
#define EOF_TOK  TOK_TYPE(TOK_EOF)
#define END_TOK  TOK_TYPE(TOK_END)
#define ERR_TOK  TOK_TYPE(TOK_ERR)

#define READ_PUNCT(c, t, s) read_punct((c), (t), (s), LEN(s) - 1)

#define EOF_CHAR (-1)

typedef int (*Predicate)(int);

typedef struct {
    unsigned int pos, len, eof;
    FILE *f;
    char buf[1024];
} InputBuffer;

typedef struct {
    unsigned int back, front;
    Token toks[16];
} TokenQueue;

struct Lexer {
    unsigned int line, col;
    InputBuffer input_buf;
    TokenQueue tok_queue;
    VectorChar *text_buf;
};

static char block_open[] = "{{{",
            block_close[] = "}}}",
            slash[] = "/",
            star[] = "*",
            dot[] = ".";

static void add_token_queue(Lexer *ctx, Token tok)
{
    ctx->tok_queue.toks[ctx->tok_queue.back] = tok;
    ctx->tok_queue.back = (ctx->tok_queue.back + 1) % LEN(ctx->tok_queue.toks);
}

static Token remove_token_queue(Lexer *ctx)
{
    Token tok = ctx->tok_queue.toks[ctx->tok_queue.front];
    ctx->tok_queue.front = (ctx->tok_queue.front + 1) % LEN(ctx->tok_queue.toks);
    return tok;
}

static inline int is_empty_token_queue(Lexer *ctx)
{
    return ctx->tok_queue.back == ctx->tok_queue.front;
}

static void init_input_buf(InputBuffer *b, FILE *f)
{
    b->pos = 0;
    b->len = 0;
    b->eof = 0;
    b->f = f;
}

static int peekn_char(Lexer *ctx, unsigned int i)
{
    InputBuffer *b = &ctx->input_buf;

    if (b->pos + i < b->len)
        goto exit;

    if (b->eof || (i > 0 && i >= b->len))
        return EOF_CHAR;

    if (i > 0) {
        assert(i < LEN(b->buf));
        memmove(b->buf, b->buf + (b->len - i) * sizeof(*b->buf),
                i * sizeof(*b->buf));
    }

    b->pos = 0;
    b->len = fread(b->buf + i * sizeof(*b->buf), sizeof(*b->buf),
                   LEN(b->buf) - i, b->f);

    if (b->len != LEN(b->buf)) {
        if (feof(b->f))
            b->eof = 1;
        else if (ferror(b->f))
            PRINTINTERR("fread() failed");

        if (b->len == 0)
            return EOF_CHAR;
    }

exit:
    return b->buf[b->pos + i];
}

static inline char peek_char(Lexer *ctx)
{
    return peekn_char(ctx, 0);
}

static char nextn_char(Lexer *ctx, unsigned int i)
{
    char c = peekn_char(ctx, i);

    ctx->col++;

    if (c == '\n') {
        ctx->col = 1;
        ctx->line++;
    }

    ctx->input_buf.pos++;

    return c;
}

static inline char next_char(Lexer *ctx)
{
    return nextn_char(ctx, 0);
}

static void skipn_char(Lexer *ctx, int n)
{
    for (int i = 0; i < n; i++)
        next_char(ctx);
}

static inline void add_text_buf(Lexer *ctx, char c)
{
    vectorChar_append(ctx->text_buf, c);
}

static inline char *get_text_buf_at(Lexer *ctx, size_t i)
{
    return vector_get((Vector *)ctx->text_buf, i);
}

static inline size_t get_text_buf_len(Lexer *ctx)
{
    return ctx->text_buf->len;
}

static inline void set_text_buf_len(Lexer *ctx, size_t len)
{
    vectorChar_resize(ctx->text_buf, len);
}

Lexer *lexer_init(FILE *f)
{
    Lexer *ctx;

    if (!(ctx = malloc(sizeof(*ctx)))) {
        PRINTINTERR(FUNCFAILED("malloc"), ERRNOS);
        abort();
    }

    init_input_buf(&ctx->input_buf, f);
    ctx->text_buf = vectorChar_new(1024);
    ctx->line = 1;
    ctx->col = 1;
    ctx->tok_queue.back = 0;
    ctx->tok_queue.front = 0;

    return ctx;
}

void lexer_free(Lexer *ctx)
{
    vectorChar_free(ctx->text_buf);
    free(ctx);
}

static int cmp_nextn(Lexer *ctx, int n, char *s)
{
    int i = 0;
    char c;

    while (1) {
        c = peekn_char(ctx, i);
        if (i >= n || *s == '\0' || c != *s)
            break;

        s += sizeof(*s);
        i++;
    }

    if (i == n)
        return 0;
    else
        return ((unsigned char)c - *(unsigned char *)s);
}

static void read_while(Lexer *ctx, Predicate p, int add)
{
    char c;

    while (1) {
        c = peek_char(ctx);

        if (c < 0 || !p(c))
            break;

        if (add)
            add_text_buf(ctx, c);

        next_char(ctx);
    }

    if (add)
        add_text_buf(ctx, '\0');
}

static inline Token read_end(Lexer *ctx)
{
    Token tok = NULL_TOK;

    while (peek_char(ctx) == '\n') {
        char c = peek_char(ctx);
        if (c != '\n')
            break;

        next_char(ctx);
        tok = END_TOK;
    }

    return tok;
}

static inline Token read_symbol(Lexer *ctx)
{
    char c = peek_char(ctx);

    if (!isalpha(c))
        return NULL_TOK;

    size_t p = get_text_buf_len(ctx);
    read_while(ctx, isalnum, 1);

    return (Token){ TOK_STR, { .sp = p } };
}

static inline Token read_digit(Lexer *ctx)
{
    char c = peek_char(ctx);

    if (!isdigit(c))
        return NULL_TOK;

    size_t len = get_text_buf_len(ctx);
    read_while(ctx, isdigit, 1);

    int i = atoi(get_text_buf_at(ctx, len));
    set_text_buf_len(ctx, len);

    return (Token){ TOK_INT, { .i = i } };
}

static Token read_punct(Lexer *ctx, int type, char *s, int n)
{
    Token tok;

    if (peek_char(ctx) == EOF_CHAR)
        return EOF_TOK;

    int ret = cmp_nextn(ctx, n, s);

    if (ret == 0)
        tok.type = type;
    else
        return NULL_TOK;

    skipn_char(ctx, n);

    return tok;
}

static inline Token read_block_open(Lexer *ctx)
{
    return READ_PUNCT(ctx, TOK_BLK_OPEN, block_open);
}

static inline Token read_block_close(Lexer *ctx)
{
    return READ_PUNCT(ctx, TOK_BLK_CLS, block_close);
}

static Token read_block(Lexer *ctx)
{
    Token open_tok, body_tok, close_tok;

    if ((open_tok = read_block_open(ctx)).type == TOK_NULL)
        return NULL_TOK;

    body_tok = (Token){ TOK_STR, { .sp = get_text_buf_len(ctx) } };

    while (1) {
        close_tok = read_block_close(ctx);

        if (close_tok.type == TOK_EOF) {
            PARSEERROR(*ctx, "unclosed block");
            return ERR_TOK;
        } else if (close_tok.type != TOK_NULL) {
            break;
        }

        add_text_buf(ctx, next_char(ctx));
    }

    add_text_buf(ctx, '\0');
    add_token_queue(ctx, body_tok);

    if (close_tok.type != TOK_NULL)
        add_token_queue(ctx, close_tok);

    return open_tok;
}

#define ATTEMPT_READ(c, func)   \
    do {                        \
        Token t = (func)(c);    \
        if (t.type != TOK_NULL) \
            return t;           \
    } while (0)

#define ATTEMPT_READ_CHAR(ctx, ch, type) \
    do {                                 \
        char c = peek_char(ctx);         \
        if (c == (ch)) {                 \
            next_char(ctx);              \
            return (type);               \
        }                                \
    } while (0)

Token lexer_get_token(Lexer *ctx)
{
    if (!is_empty_token_queue(ctx))
        return remove_token_queue(ctx);

    read_while(ctx, isblank, 0);

    ATTEMPT_READ_CHAR(ctx, EOF_CHAR, EOF_TOK);
    ATTEMPT_READ_CHAR(ctx, '/', TOK_TYPE(TOK_SLASH));
    ATTEMPT_READ_CHAR(ctx, '*', TOK_TYPE(TOK_STAR));
    ATTEMPT_READ_CHAR(ctx, '.', TOK_TYPE(TOK_DOT));

    ATTEMPT_READ(ctx, read_end);
    ATTEMPT_READ(ctx, read_symbol);
    ATTEMPT_READ(ctx, read_digit);
    ATTEMPT_READ(ctx, read_block);

    PARSEERROR((*ctx), "cannot handle character: %c", peek_char(ctx));
    return ERR_TOK;
}

char *lexer_get_string(Lexer *ctx, Token tok)
{
    if (tok.type != TOK_STR)
        return NULL;

    return get_text_buf_at(ctx, tok.val.sp);
}

char *lexer_token_type_str(enum TokenType type)
{
    switch (type) {
    case TOK_NULL:
        return "<null>";
    case TOK_EOF:
        return "<end of file>";
    case TOK_ERR:
        return "<TOKEN ERROR>";
    case TOK_END:
        return "<end>";
    case TOK_BLK_OPEN:
        return block_open;
    case TOK_BLK_CLS:
        return block_close;
    case TOK_SLASH:
        return slash;
    case TOK_STAR:
        return star;
    case TOK_DOT:
        return dot;
    case TOK_INT:
        return "<integer>";
    case TOK_STR:
        return "<string>";
    }

    PRINTINTERR("unknown type: %d", type);
    abort();
}
