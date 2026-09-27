/* goed - minimalist GUI editor for Go (Win32 + RichEdit, C99, TCC).
 * Build: tcc -DUNICODE -Wall goed.c -luser32 -lgdi32 -L. -lcomdlg32 -Wl,-subsystem=windows -o goed.exe
 * Keys:  ^N new  ^O open  ^S save+gofmt  ^Shift+S save as  ^R go run  ^B go vet
 *        ^F find  F3 next  ^G goto line  Esc stop/cancel  ^Q quit  F1 help
 */
#include <windows.h>
#include <commdlg.h>
#include <richedit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---------- pure logic (covered by test_goed.c) ---------- */

enum { C_PLAIN, C_KEY, C_TYPE, C_STR, C_COM, C_NUM };
typedef struct { int start, len, cls; } Span;

static const wchar_t *KEYS[] = {
    L"break", L"case", L"chan", L"const", L"continue", L"default", L"defer", L"else",
    L"fallthrough", L"for", L"func", L"go", L"goto", L"if", L"import", L"interface",
    L"map", L"package", L"range", L"return", L"select", L"struct", L"switch", L"type", L"var", 0};
static const wchar_t *TYPES[] = {
    L"bool", L"byte", L"complex64", L"complex128", L"error", L"float32", L"float64",
    L"int", L"int8", L"int16", L"int32", L"int64", L"rune", L"string", L"uint", L"uint8",
    L"uint16", L"uint32", L"uint64", L"uintptr", L"any", L"comparable",
    L"true", L"false", L"nil", L"iota",
    L"append", L"cap", L"clear", L"close", L"complex", L"copy", L"delete", L"imag", L"len",
    L"make", L"max", L"min", L"new", L"panic", L"print", L"println", L"real", L"recover", 0};

static int in_list(const wchar_t **l, const wchar_t *s, int n) {
    for (; *l; l++)
        if ((int)wcslen(*l) == n && !wcsncmp(*l, s, n)) return 1;
    return 0;
}
static int is_dig(wchar_t c) { return c >= '0' && c <= '9'; }
static int is_id(wchar_t c) {
    return c == '_' || is_dig(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c > 127;
}
static int is_nl(wchar_t c) { return c == '\r' || c == '\n'; }

/* Writes coloured spans (plain text is skipped) to out; out needs room for n spans. */
static int go_lex(const wchar_t *s, int n, Span *out) {
    int i = 0, k = 0;
    while (i < n) {
        wchar_t c = s[i];
        int st = i, cls = C_PLAIN;
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && !is_nl(s[i])) i++;
            cls = C_COM;
        } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) i++;
            i = i < n ? i + 2 : n;
            cls = C_COM;
        } else if (c == '"' || c == '\'') {
            i++;
            while (i < n && s[i] != c && !is_nl(s[i])) {
                if (s[i] == '\\' && i + 1 < n && !is_nl(s[i + 1])) i++;
                i++;
            }
            if (i < n && s[i] == c) i++;
            cls = C_STR;
        } else if (c == '`') {
            i++;
            while (i < n && s[i] != '`') i++;
            if (i < n) i++;
            cls = C_STR;
        } else if (is_dig(c) || (c == '.' && i + 1 < n && is_dig(s[i + 1]))) {
            int hex = c == '0' && i + 1 < n && (s[i + 1] | 32) == 'x';
            i++;
            while (i < n) {
                wchar_t p = s[i - 1] | 32;
                if (is_id(s[i]) || s[i] == '.') i++;
                else if ((s[i] == '+' || s[i] == '-') && (p == 'p' || (p == 'e' && !hex))) i++;
                else break;
            }
            cls = C_NUM;
        } else if (is_id(c)) {
            while (i < n && is_id(s[i])) i++;
            cls = in_list(KEYS, s + st, i - st) ? C_KEY : in_list(TYPES, s + st, i - st) ? C_TYPE : C_PLAIN;
        } else {
            i++;
        }
        if (cls) { out[k].start = st; out[k].len = i - st; out[k].cls = cls; k++; }
    }
    return k;
}

static int has_crlf(const char *b, int n) {
    for (int i = 0; i + 1 < n; i++)
        if (b[i] == '\r' && b[i + 1] == '\n') return 1;
    return 0;
}

/* Normalise \r, \r\n and \n to the chosen line ending. Caller frees. */
static char *to_eol(const char *s, int n, int crlf, int *outn) {
    char *o = malloc(2 * (size_t)n + 1);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\r' || s[i] == '\n') {
            if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') i++;
            if (crlf) o[k++] = '\r';
            o[k++] = '\n';
        } else {
            o[k++] = s[i];
        }
    }
    o[k] = 0;
    *outn = k;
    return o;
}

/* Finds "x.go:LINE[:COL]" in a compiler/vet/panic line. */
static int parse_goerr(const wchar_t *l, int *ln, int *col) {
    for (const wchar_t *p = wcsstr(l, L".go:"); p; p = wcsstr(p + 1, L".go:")) {
        p += 4;
        if (!is_dig(*p)) continue;
        *ln = (int)wcstol(p, (wchar_t **)&p, 10);
        *col = (*p == ':' && is_dig(p[1])) ? (int)wcstol(p + 1, NULL, 10) : 1;
        return 1;
    }
    return 0;
}

/* First argument after argv[0], quotes stripped. */
static wchar_t *cmdline_arg(const wchar_t *cl, wchar_t *buf) {
    int k = 0;
    if (*cl == '"') { cl++; while (*cl && *cl != '"') cl++; if (*cl) cl++; }
    else while (*cl && *cl != ' ' && *cl != '\t') cl++;
    while (*cl == ' ' || *cl == '\t') cl++;
    if (*cl == '"') { cl++; while (*cl && *cl != '"' && k < MAX_PATH - 1) buf[k++] = *cl++; }
    else while (*cl && *cl != ' ' && *cl != '\t' && k < MAX_PATH - 1) buf[k++] = *cl++;
    buf[k] = 0;
    return buf;
}

/* ---------- GUI ---------- */
#ifndef GOED_TEST

enum { ID_NEW = 100, ID_OPEN, ID_SAVE, ID_SAVEAS, ID_RUN, ID_VET, ID_FIND, ID_NEXT,
       ID_GOTO, ID_QUIT, ID_ESC, ID_HELP, ID_NOP };
#define TIMER_HL 1
#define WM_OUT  (WM_APP + 1)
#define WM_DONE (WM_APP + 2)

static const COLORREF COL[] = {RGB(30, 30, 30), RGB(0, 0, 170), RGB(0, 120, 120),
                               RGB(170, 50, 0), RGB(0, 128, 0), RGB(140, 0, 140)};
static HWND g_main, g_ed, g_out, g_stat, g_input;
static HFONT g_font;
static WNDPROC g_edproc, g_outproc, g_inproc;
static wchar_t g_path[MAX_PATH], g_find[256];
static int g_crlf, g_dirty, g_prompt; /* g_prompt: 0, 'f' find, 'g' goto */
static void *g_doc;                    /* TOM ITextDocument, may be NULL */
static HANDLE g_proc, g_job;

/* --- small helpers --- */
static wchar_t *u8_to_w(const char *s, int n, int *wn) {
    int m = n ? MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0) : 0;
    wchar_t *w = malloc((m + 1) * sizeof(wchar_t));
    if (m) MultiByteToWideChar(CP_UTF8, 0, s, n, w, m);
    w[m] = 0;
    if (wn) *wn = m;
    return w;
}
static char *w_to_u8(const wchar_t *w, int n, int *un) {
    int m = n ? WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL) : 0;
    char *s = malloc(m + 1);
    if (m) WideCharToMultiByte(CP_UTF8, 0, w, n, s, m, NULL, NULL);
    s[m] = 0;
    if (un) *un = m;
    return s;
}
static void **vt(void *o) { return *(void ***)o; }
static void tom_undo(long mode) { /* tomSuspend / tomResume: keep colouring out of undo */
    if (g_doc) ((HRESULT(__stdcall *)(void *, long, long *))vt(g_doc)[22])(g_doc, mode, NULL);
}
static void msg(const wchar_t *m) { SetWindowTextW(g_stat, m); }
static const wchar_t *base_name(const wchar_t *p) {
    const wchar_t *b = p;
    for (; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

static wchar_t *get_text(HWND h, int *n) {
    GETTEXTLENGTHEX gl = {GTL_NUMCHARS | GTL_PRECISE, 1200};
    int len = (int)SendMessageW(h, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    wchar_t *s = malloc((len + 1) * sizeof(wchar_t));
    GETTEXTEX gt = {0};
    gt.cb = (len + 1) * sizeof(wchar_t);
    gt.flags = GT_DEFAULT;
    gt.codepage = 1200;
    *n = (int)SendMessageW(h, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)s);
    s[*n] = 0;
    return s;
}
static void set_text(const wchar_t *w, int keep_undo) {
    SETTEXTEX st = {keep_undo ? ST_KEEPUNDO : ST_DEFAULT, 1200};
    SendMessageW(g_ed, EM_SETEVENTMASK, 0, 0);
    SendMessageW(g_ed, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)w);
    SendMessageW(g_ed, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
}
static void append_out(const char *u8, int n) {
    wchar_t *w = u8_to_w(u8, n, NULL);
    SendMessageW(g_out, EM_SETSEL, -1, -1);
    SendMessageW(g_out, EM_REPLACESEL, FALSE, (LPARAM)w);
    SendMessageW(g_out, EM_SCROLLCARET, 0, 0);
    free(w);
}

static void update_title(void) {
    wchar_t t[MAX_PATH + 16];
    _snwprintf(t, MAX_PATH + 15, L"goed - %s%s", g_path[0] ? base_name(g_path) : L"untitled", g_dirty ? L"*" : L"");
    t[MAX_PATH + 15] = 0;
    SetWindowTextW(g_main, t);
}
static void update_status(void) {
    if (g_prompt) return;
    CHARRANGE r;
    SendMessageW(g_ed, EM_EXGETSEL, 0, (LPARAM)&r);
    int ln = (int)SendMessageW(g_ed, EM_EXLINEFROMCHAR, 0, r.cpMax);
    int col = r.cpMax - (int)SendMessageW(g_ed, EM_LINEINDEX, ln, 0);
    wchar_t s[128];
    _snwprintf(s, 127, L" Ln %d, Col %d    %s    F1 help%s", ln + 1, col + 1, g_crlf ? L"CRLF" : L"LF",
               g_proc ? L"    [running - Esc stops]" : L"");
    s[127] = 0;
    msg(s);
}

/* ponytail: recolours the whole document on each (debounced) edit; fine to a few
 * thousand lines, switch to visible-range lexing with saved line state if it lags. */
static void highlight(void) {
    int n;
    wchar_t *s = get_text(g_ed, &n);
    Span *sp = malloc(sizeof(Span) * (n + 1));
    int k = go_lex(s, n, sp);
    CHARRANGE sel;
    POINT scr;
    SendMessageW(g_ed, EM_EXGETSEL, 0, (LPARAM)&sel);
    SendMessageW(g_ed, EM_GETSCROLLPOS, 0, (LPARAM)&scr);
    SendMessageW(g_ed, EM_SETEVENTMASK, 0, 0);
    SendMessageW(g_ed, WM_SETREDRAW, FALSE, 0);
    tom_undo(-9999995); /* tomSuspend */

    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT;
    cf.crTextColor = COL[C_PLAIN];
    cf.yHeight = 200; /* twips: 10pt */
    wcscpy(cf.szFaceName, L"Consolas");
    SendMessageW(g_ed, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    cf.dwMask = CFM_COLOR;
    for (int i = 0; i < k; i++) {
        CHARRANGE r = {sp[i].start, sp[i].start + sp[i].len};
        SendMessageW(g_ed, EM_EXSETSEL, 0, (LPARAM)&r);
        cf.crTextColor = COL[sp[i].cls];
        SendMessageW(g_ed, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }

    SendMessageW(g_ed, EM_EXSETSEL, 0, (LPARAM)&sel);
    SendMessageW(g_ed, EM_SETSCROLLPOS, 0, (LPARAM)&scr);
    tom_undo(-9999994); /* tomResume */
    SendMessageW(g_ed, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_ed, NULL, TRUE);
    SendMessageW(g_ed, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
    free(sp);
    free(s);
}

/* --- files --- */
static char *read_file(const wchar_t *path, int *n) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(sz + 1);
    *n = (int)fread(b, 1, sz, f);
    b[*n] = 0;
    fclose(f);
    return b;
}
/* Write to a temp file then rename, so a failed write never truncates the original. */
static int write_file(const wchar_t *path, const char *b, int n) {
    wchar_t tmp[MAX_PATH + 8];
    _snwprintf(tmp, MAX_PATH + 7, L"%s.goed~", path);
    tmp[MAX_PATH + 7] = 0;
    FILE *f = _wfopen(tmp, L"wb");
    int ok = f && (int)fwrite(b, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) {
        DeleteFileW(tmp);
        MessageBoxW(g_main, path, L"goed: could not write file", MB_ICONERROR);
    }
    return ok;
}

static void load_file(const wchar_t *path) {
    int n = 0;
    char *b = path[0] ? read_file(path, &n) : NULL;
    char *p = b ? b : "";
    if (n >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3, n -= 3;
    g_crlf = b ? has_crlf(p, n) : 0;
    wchar_t *w = u8_to_w(p, n, NULL);
    set_text(w, 0);
    SendMessageW(g_ed, EM_SETSEL, 0, 0);
    free(w);
    free(b);
    wcsncpy(g_path, path, MAX_PATH - 1);
    g_dirty = 0;
    highlight();
    update_title();
    update_status();
    if (!b && path[0]) msg(L" New file");
}

static int ask_path(int save) {
    wchar_t buf[MAX_PATH];
    wcscpy(buf, save ? g_path : L"");
    OPENFILENAMEW of;
    memset(&of, 0, sizeof of);
    of.lStructSize = sizeof of;
    of.hwndOwner = g_main;
    of.lpstrFilter = L"Go files (*.go)\0*.go\0All files\0*.*\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = L"go";
    of.Flags = save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST;
    if (!(save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of))) return 0;
    wcscpy(g_path, buf);
    return 1;
}

/* Runs cmd, collecting stdout and stderr. Returns exit code, or -1 if it could not start. */
static int run_sync(wchar_t *cmd, char **out, int *outn, char **err, int *errn) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE orl, owr, erl, ewr;
    CreatePipe(&orl, &owr, &sa, 1 << 20);
    CreatePipe(&erl, &ewr, &sa, 1 << 20);
    SetHandleInformation(orl, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(erl, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = owr;
    si.hStdError = ewr;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(owr);
    CloseHandle(ewr);
    HANDLE rd[2] = {orl, erl};
    char **dst[2] = {out, err};
    int *dn[2] = {outn, errn};
    for (int j = 0; j < 2; j++) {
        int cap = 4096, n = 0;
        char *b = malloc(cap);
        DWORD r;
        while (ok && ReadFile(rd[j], b + n, cap - n - 1, &r, NULL) && r) {
            n += r;
            if (cap - n < 1024) b = realloc(b, cap *= 2);
        }
        b[n] = 0;
        *dst[j] = b;
        *dn[j] = n;
        CloseHandle(rd[j]);
    }
    if (!ok) return -1;
    DWORD code = 1;
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

/* Saves as-is, then runs gofmt; on success the formatted text is written and shown. */
static int save_file(int as) {
    if ((as || !g_path[0]) && !ask_path(1)) return 0;
    int n, un, en, lfn;
    wchar_t *w = get_text(g_ed, &n);
    char *u = w_to_u8(w, n, &un);
    char *lf = to_eol(u, un, 0, &lfn);
    char *disk = to_eol(u, un, g_crlf, &en);
    int ok = write_file(g_path, disk, en);
    free(disk);
    free(u);
    free(w);
    if (!ok) { free(lf); return 0; }
    g_dirty = 0;
    update_title();
    update_status();

    int len = (int)wcslen(g_path);
    if (len > 3 && !_wcsicmp(g_path + len - 3, L".go")) {
        wchar_t cmd[MAX_PATH + 16];
        _snwprintf(cmd, MAX_PATH + 15, L"gofmt \"%s\"", g_path);
        cmd[MAX_PATH + 15] = 0;
        char *fo, *fe;
        int fon, fen;
        int code = run_sync(cmd, &fo, &fon, &fe, &fen);
        if (code == -1) {
            msg(L" Saved (gofmt not found on PATH)");
        } else if (code != 0) {
            SetWindowTextW(g_out, L"");
            append_out("gofmt:\n", 7);
            append_out(fe, fen);
            msg(L" Saved - gofmt reported errors");
        } else if (fon != lfn || memcmp(fo, lf, fon)) {
            int dn;
            char *d = to_eol(fo, fon, g_crlf, &dn);
            if (write_file(g_path, d, dn)) {
                CHARRANGE sel;
                POINT scr;
                SendMessageW(g_ed, EM_EXGETSEL, 0, (LPARAM)&sel);
                SendMessageW(g_ed, EM_GETSCROLLPOS, 0, (LPARAM)&scr);
                wchar_t *fw = u8_to_w(fo, fon, NULL);
                set_text(fw, 1);
                free(fw);
                /* ponytail: caret restored by offset; can drift by the indent gofmt changed */
                SendMessageW(g_ed, EM_EXSETSEL, 0, (LPARAM)&sel);
                SendMessageW(g_ed, EM_SETSCROLLPOS, 0, (LPARAM)&scr);
                highlight();
            }
            free(d);
            g_dirty = 0;
            update_title();
        }
        free(fo);
        free(fe);
    }
    free(lf);
    return 1;
}

static int confirm_discard(void) {
    if (!g_dirty) return 1;
    wchar_t q[MAX_PATH + 32];
    _snwprintf(q, MAX_PATH + 31, L"Save changes to %s?", g_path[0] ? base_name(g_path) : L"untitled");
    q[MAX_PATH + 31] = 0;
    int r = MessageBoxW(g_main, q, L"goed", MB_YESNOCANCEL | MB_ICONQUESTION);
    return r == IDYES ? save_file(0) : r == IDNO;
}

/* --- go run / go vet (async, killable) --- */
static DWORD WINAPI reader(void *h) {
    char buf[4096];
    DWORD r;
    /* ponytail: a UTF-8 char split across two reads shows as '?'; buffer the tail if it matters */
    while (ReadFile(h, buf, sizeof buf, &r, NULL) && r) {
        char *m = malloc(r);
        memcpy(m, buf, r);
        PostMessageW(g_main, WM_OUT, r, (LPARAM)m);
    }
    CloseHandle(h);
    PostMessageW(g_main, WM_DONE, 0, 0);
    return 0;
}

static void stop_proc(void) {
    if (g_proc) TerminateProcess(g_proc, 1);        /* first, so [exit 1] shows it was stopped */
    if (g_job) { CloseHandle(g_job); g_job = NULL; } /* KILL_ON_JOB_CLOSE takes the go-built child too */
}

static void go_cmd(const wchar_t *verb) {
    if (g_proc) { msg(L" Already running - Esc stops it"); return; }
    if ((g_dirty || !g_path[0]) && !save_file(0)) return;
    wchar_t cmd[MAX_PATH + 32], dir[MAX_PATH];
    _snwprintf(cmd, MAX_PATH + 31, L"go %s \"%s\"", verb, g_path);
    cmd[MAX_PATH + 31] = 0;
    wcscpy(dir, g_path);
    *(wchar_t *)base_name(dir) = 0;

    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi;
    SetWindowTextW(g_out, L"");
    char *hdr = w_to_u8(cmd, (int)wcslen(cmd), NULL);
    append_out("> ", 2);
    append_out(hdr, (int)strlen(hdr));
    append_out("\n", 1);
    free(hdr);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL,
                        dir[0] ? dir : NULL, &si, &pi)) {
        append_out("could not start go (is it on PATH?)\n", 36);
        CloseHandle(rd);
        CloseHandle(wr);
        return;
    }
    CloseHandle(wr);
    /* Job objects aren't in TCC's kernel32.def, so resolve them at runtime. */
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HANDLE(WINAPI *mkjob)(void *, const wchar_t *) = (void *)GetProcAddress(k32, "CreateJobObjectW");
    BOOL(WINAPI *setjob)(HANDLE, int, void *, DWORD) = (void *)GetProcAddress(k32, "SetInformationJobObject");
    BOOL(WINAPI *assign)(HANDLE, HANDLE) = (void *)GetProcAddress(k32, "AssignProcessToJobObject");
    if (mkjob && setjob && assign && (g_job = mkjob(NULL, NULL))) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        setjob(g_job, 9 /* JobObjectExtendedLimitInformation */, &li, sizeof li);
        assign(g_job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    CloseHandle(CreateThread(NULL, 0, reader, rd, 0, NULL));
    update_status();
}

/* --- navigation --- */
static void goto_line(int ln, int col) {
    int idx = (int)SendMessageW(g_ed, EM_LINEINDEX, ln - 1, 0);
    if (ln < 1 || idx < 0) { msg(L" No such line"); return; }
    int len = (int)SendMessageW(g_ed, EM_LINELENGTH, idx, 0);
    if (col > len + 1) col = len + 1;
    CHARRANGE r = {idx + col - 1, idx + col - 1};
    SendMessageW(g_ed, EM_EXSETSEL, 0, (LPARAM)&r);
    SendMessageW(g_ed, EM_SCROLLCARET, 0, 0);
    SetFocus(g_ed);
}

static void find_next(void) {
    if (!g_find[0]) return;
    CHARRANGE sel;
    SendMessageW(g_ed, EM_EXGETSEL, 0, (LPARAM)&sel);
    FINDTEXTEXW ft;
    ft.chrg.cpMin = sel.cpMax;
    ft.chrg.cpMax = -1;
    ft.lpstrText = g_find;
    if (SendMessageW(g_ed, EM_FINDTEXTEXW, FR_DOWN, (LPARAM)&ft) < 0) {
        ft.chrg.cpMin = 0; /* wrap */
        if (SendMessageW(g_ed, EM_FINDTEXTEXW, FR_DOWN, (LPARAM)&ft) < 0) { msg(L" Not found"); return; }
    }
    SendMessageW(g_ed, EM_EXSETSEL, 0, (LPARAM)&ft.chrgText);
    SendMessageW(g_ed, EM_SCROLLCARET, 0, 0);
}

static void layout(void) {
    RECT rc;
    GetClientRect(g_main, &rc);
    int w = rc.right, h = rc.bottom, bar = 22, out = h / 4;
    MoveWindow(g_ed, 0, 0, w, h - out - bar, TRUE);
    MoveWindow(g_out, 0, h - out - bar, w, out, TRUE);
    MoveWindow(g_stat, 0, h - bar, g_prompt ? 50 : w, bar, TRUE);
    MoveWindow(g_input, 50, h - bar, w - 50, bar, TRUE);
}

static void prompt(int kind) {
    g_prompt = kind;
    msg(kind == 'f' ? L" Find:" : L" Line:");
    SetWindowTextW(g_input, kind == 'f' ? g_find : L"");
    SendMessageW(g_input, EM_SETSEL, 0, -1);
    layout();
    ShowWindow(g_input, SW_SHOW);
    SetFocus(g_input);
}
static void end_prompt(void) {
    g_prompt = 0;
    ShowWindow(g_input, SW_HIDE);
    layout();
    update_status();
    SetFocus(g_ed);
}

static LRESULT CALLBACK input_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CHAR && (wp == '\r' || wp == 27)) return 0; /* no beep */
    if (m == WM_KEYDOWN && wp == VK_RETURN) {
        wchar_t t[256];
        GetWindowTextW(h, t, 256);
        int kind = g_prompt;
        end_prompt();
        if (kind == 'f') { wcscpy(g_find, t); find_next(); }
        else goto_line(_wtoi(t), 1);
        return 0;
    }
    return CallWindowProcW(g_inproc, h, m, wp, lp);
}

/* Enter keeps the current line's indent, plus one tab after '{', '(' or ':'.
 * RichEdit inserts the newline on WM_KEYDOWN, so hook that and drop the '\r' char. */
static LRESULT CALLBACK ed_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CHAR && wp == '\r') return 0;
    if (m == WM_KEYDOWN && wp == VK_RETURN) {
        CHARRANGE r;
        SendMessageW(h, EM_EXGETSEL, 0, (LPARAM)&r);
        int ln = (int)SendMessageW(h, EM_EXLINEFROMCHAR, 0, r.cpMin);
        int col = r.cpMin - (int)SendMessageW(h, EM_LINEINDEX, ln, 0);
        wchar_t line[1024], ins[256] = L"\r";
        *(WORD *)line = 1023;
        int n = (int)SendMessageW(h, EM_GETLINE, ln, (LPARAM)line);
        if (col < n) n = col;
        int k = 1;
        for (int i = 0; i < n && (line[i] == '\t' || line[i] == ' ') && k < 250; i++) ins[k++] = line[i];
        if (n > 0 && (line[n - 1] == '{' || line[n - 1] == '(' || line[n - 1] == ':')) ins[k++] = '\t';
        ins[k] = 0;
        SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)ins);
        return 0;
    }
    return CallWindowProcW(g_edproc, h, m, wp, lp);
}

/* Double-click an error line in the output pane to jump there. */
static LRESULT CALLBACK out_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    LRESULT res = CallWindowProcW(g_outproc, h, m, wp, lp);
    if (m == WM_LBUTTONDBLCLK) {
        CHARRANGE r;
        SendMessageW(h, EM_EXGETSEL, 0, (LPARAM)&r);
        int ln = (int)SendMessageW(h, EM_EXLINEFROMCHAR, 0, r.cpMin), el, ec;
        wchar_t line[1024];
        *(WORD *)line = 1023;
        line[SendMessageW(h, EM_GETLINE, ln, (LPARAM)line)] = 0;
        if (parse_goerr(line, &el, &ec)) goto_line(el, ec);
    }
    return res;
}

static const char HELP[] =
    "goed - minimalist Go editor\n"
    "  Ctrl+N new        Ctrl+O open       Ctrl+S save (+gofmt)   Ctrl+Shift+S save as\n"
    "  Ctrl+R go run     Ctrl+B go vet     Esc stop / cancel\n"
    "  Ctrl+F find       F3 find next      Ctrl+G go to line      Ctrl+Q quit\n"
    "  Double-click an error line here to jump to it.\n";

static HWND mk_edit(DWORD extra) {
    HWND h = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                 ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL | extra,
                             0, 0, 0, 0, g_main, NULL, GetModuleHandleW(NULL), NULL);
    SendMessageW(h, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
    SendMessageW(h, EM_SETTARGETDEVICE, 0, 1); /* no word wrap */
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
    cf.yHeight = 200;
    cf.crTextColor = COL[C_PLAIN];
    wcscpy(cf.szFaceName, L"Consolas");
    SendMessageW(h, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    return h;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_main = h;
        g_ed = mk_edit(ES_WANTRETURN);
        g_out = mk_edit(ES_READONLY);
        SendMessageW(g_out, EM_SETBKGNDCOLOR, 0, RGB(245, 245, 245));
        g_stat = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 0, 0, 0, 0, h, NULL, NULL, NULL);
        g_input = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, h, NULL, NULL, NULL);
        HDC dc = GetDC(h);
        g_font = CreateFontW(-MulDiv(10, GetDeviceCaps(dc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        ReleaseDC(h, dc);
        SendMessageW(g_stat, WM_SETFONT, (WPARAM)g_font, 0);
        SendMessageW(g_input, WM_SETFONT, (WPARAM)g_font, 0);
        g_edproc = (WNDPROC)SetWindowLongPtrW(g_ed, GWLP_WNDPROC, (LONG_PTR)ed_proc);
        g_outproc = (WNDPROC)SetWindowLongPtrW(g_out, GWLP_WNDPROC, (LONG_PTR)out_proc);
        g_inproc = (WNDPROC)SetWindowLongPtrW(g_input, GWLP_WNDPROC, (LONG_PTR)input_proc);

        static const GUID IID_ITextDocument = {0x8CC497C0, 0xA1DF, 0x11CE, {0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D}};
        void *ole = NULL;
        SendMessageW(g_ed, EM_GETOLEINTERFACE, 0, (LPARAM)&ole);
        if (ole) {
            ((HRESULT(__stdcall *)(void *, const GUID *, void **))vt(ole)[0])(ole, &IID_ITextDocument, &g_doc);
            ((ULONG(__stdcall *)(void *))vt(ole)[2])(ole);
        }
        if (g_doc) ((HRESULT(__stdcall *)(void *, float))vt(g_doc)[14])(g_doc, 22.0f); /* tab = 4 cols */
        SendMessageW(g_ed, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
        append_out(HELP, sizeof HELP - 1);
        return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_SETFOCUS: SetFocus(g_prompt ? g_input : g_ed); return 0;
    case WM_TIMER: KillTimer(h, TIMER_HL); highlight(); return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == EN_SELCHANGE) update_status();
        return 0;
    case WM_OUT: append_out((char *)lp, (int)wp); free((void *)lp); return 0;
    case WM_DONE: {
        DWORD code = 0;
        char s[40];
        GetExitCodeProcess(g_proc, &code);
        sprintf(s, "\n[exit %ld]\n", (long)code);
        append_out(s, (int)strlen(s));
        CloseHandle(g_proc);
        g_proc = NULL;
        if (g_job) { CloseHandle(g_job); g_job = NULL; }
        update_status();
        return 0;
    }
    case WM_COMMAND:
        if ((HWND)lp == g_ed && HIWORD(wp) == EN_CHANGE) {
            if (!g_dirty) { g_dirty = 1; update_title(); }
            SetTimer(h, TIMER_HL, 250, NULL);
            return 0;
        }
        switch (LOWORD(wp)) {
        case ID_NEW: if (confirm_discard()) load_file(L""); break;
        case ID_OPEN: {
            wchar_t old[MAX_PATH];
            wcscpy(old, g_path);
            if (confirm_discard()) {
                if (ask_path(0)) load_file(g_path);
                else wcscpy(g_path, old);
            }
            break;
        }
        case ID_SAVE: save_file(0); break;
        case ID_SAVEAS: save_file(1); break;
        case ID_RUN: go_cmd(L"run"); break;
        case ID_VET: go_cmd(L"vet"); break;
        case ID_FIND: prompt('f'); break;
        case ID_NEXT: find_next(); break;
        case ID_GOTO: prompt('g'); break;
        case ID_QUIT: PostMessageW(h, WM_CLOSE, 0, 0); break;
        case ID_ESC: if (g_prompt) end_prompt(); else stop_proc(); break;
        case ID_HELP: SetWindowTextW(g_out, L""); append_out(HELP, sizeof HELP - 1); break;
        }
        return 0;
    case WM_CLOSE:
        if (confirm_discard()) { stop_proc(); DestroyWindow(h); }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cl, int show) {
    (void)prev; (void)cl;
    BOOL(WINAPI *dpi)(void) = (void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware");
    if (dpi) dpi();
    if (!LoadLibraryW(L"Msftedit.dll")) {
        MessageBoxW(NULL, L"Msftedit.dll not found", L"goed", MB_ICONERROR);
        return 1;
    }
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    /* icon group 1 is embedded after the build by icon.ps1 */
    wc.hIcon = LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"goed";
    RegisterClassExW(&wc);
    CreateWindowExW(0, L"goed", L"goed", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 750,
                    NULL, NULL, hi, NULL);
    ShowWindow(g_main, show);

    wchar_t arg[MAX_PATH];
    cmdline_arg(GetCommandLineW(), arg);
    if (arg[0]) {
        wchar_t full[MAX_PATH];
        GetFullPathNameW(arg, MAX_PATH, full, NULL);
        load_file(full);
    } else {
        load_file(L"");
    }

    /* Accelerators run before RichEdit sees the key, so they also mask its
     * formatting shortcuts (Ctrl+E/J/L/R align, Ctrl+1/2/5 spacing). */
    ACCEL acc[] = {
        {FCONTROL | FVIRTKEY, 'N', ID_NEW},   {FCONTROL | FVIRTKEY, 'O', ID_OPEN},
        {FCONTROL | FVIRTKEY, 'S', ID_SAVE},  {FCONTROL | FSHIFT | FVIRTKEY, 'S', ID_SAVEAS},
        {FCONTROL | FVIRTKEY, 'R', ID_RUN},   {FCONTROL | FVIRTKEY, 'B', ID_VET},
        {FCONTROL | FVIRTKEY, 'F', ID_FIND},  {FVIRTKEY, VK_F3, ID_NEXT},
        {FCONTROL | FVIRTKEY, 'G', ID_GOTO},  {FCONTROL | FVIRTKEY, 'Q', ID_QUIT},
        {FVIRTKEY, VK_ESCAPE, ID_ESC},        {FVIRTKEY, VK_F1, ID_HELP},
        {FCONTROL | FVIRTKEY, 'E', ID_NOP},   {FCONTROL | FVIRTKEY, 'J', ID_NOP},
        {FCONTROL | FVIRTKEY, 'L', ID_NOP},   {FCONTROL | FVIRTKEY, '1', ID_NOP},
        {FCONTROL | FVIRTKEY, '2', ID_NOP},   {FCONTROL | FVIRTKEY, '5', ID_NOP},
    };
    HACCEL ha = CreateAcceleratorTableW(acc, sizeof acc / sizeof acc[0]);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (TranslateAcceleratorW(g_main, ha, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
#endif
