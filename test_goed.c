/* tcc -DUNICODE -luser32 -lgdi32 -L. -lcomdlg32 -run test_goed.c */
#define GOED_TEST
#include "goed.c"
#include <assert.h>

static int lex(const wchar_t *s, Span *sp) { return go_lex(s, (int)wcslen(s), sp); }

static void test_lex(void) {
    Span sp[64];
    int n = lex(L"package main", sp);
    assert(n == 1 && sp[0].start == 0 && sp[0].len == 7 && sp[0].cls == C_KEY);

    n = lex(L"x := \"a\\\"b\" // hi\ry", sp);  /* escaped quote, comment ends at \r */
    assert(n == 2);
    assert(sp[0].cls == C_STR && sp[0].start == 5 && sp[0].len == 6);
    assert(sp[1].cls == C_COM && sp[1].start == 12 && sp[1].len == 5);

    n = lex(L"s := `a\rb` + 'x'", sp);     /* raw string spans lines */
    assert(n == 2 && sp[0].cls == C_STR && sp[0].len == 5 && sp[1].cls == C_STR && sp[1].len == 3);

    n = lex(L"/* a\r b */ int 0x1F 1.5e-3 foo9", sp);
    assert(n == 4);
    assert(sp[0].cls == C_COM && sp[0].len == 10);
    assert(sp[1].cls == C_TYPE && sp[1].len == 3);
    assert(sp[2].cls == C_NUM && sp[2].len == 4);
    assert(sp[3].cls == C_NUM && sp[3].len == 6);   /* foo9: ident, no span */

    n = lex(L"\"unterminated\rfunc", sp);      /* string stops at newline */
    assert(n == 2 && sp[0].len == 13 && sp[1].cls == C_KEY);

    n = lex(L"funcs forx", sp);                /* keywords must be whole words */
    assert(n == 0);
    n = lex(L"/* open", sp);
    assert(n == 1 && sp[0].len == 7);
}

static void test_eol(void) {
    assert(has_crlf("a\r\nb", 4) == 1);
    assert(has_crlf("a\nb", 3) == 0);
    int n; char *o = to_eol("a\rb\r\nc\nd", 8, 1, &n);
    assert(n == 10 && memcmp(o, "a\r\nb\r\nc\r\nd", 10) == 0); free(o);
    o = to_eol("a\rb\r\nc", 6, 0, &n);
    assert(n == 5 && memcmp(o, "a\nb\nc", 5) == 0); free(o);
}

static void test_goerr(void) {
    int ln, col;
    assert(parse_goerr(L".\\main.go:12:5: undefined: x", &ln, &col) && ln == 12 && col == 5);
    assert(parse_goerr(L"\tC:/p/main.go:7 +0x1d", &ln, &col) && ln == 7 && col == 1);
    assert(!parse_goerr(L"ok  done", &ln, &col));
    assert(!parse_goerr(L"x.go: no line", &ln, &col));
}

static void test_arg(void) {
    wchar_t b[MAX_PATH];
    assert(!wcscmp(cmdline_arg(L"\"C:\\a b\\goed.exe\" \"x y.go\"", b), L"x y.go"));
    assert(!wcscmp(cmdline_arg(L"goed.exe   main.go ", b), L"main.go"));
    assert(!wcscmp(cmdline_arg(L"goed.exe", b), L""));
}

int main(void) {
    test_lex(); test_eol(); test_goerr(); test_arg();
    puts("all tests passed");
    return 0;
}
