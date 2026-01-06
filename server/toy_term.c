#include "toy_term.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

typedef struct {
    char name[32];
    double value;
    int used;
} Var;

#define VAR_MAX 128

struct ToyTerm {
    char history[TERM_HISTORY_MAX][TERM_LINE_MAX];
    int  histCount;
    Var  vars[VAR_MAX];
};

static void hist_push(ToyTerm* t, const char* line) {
    if (!line) return;
    if (t->histCount < TERM_HISTORY_MAX) {
        strncpy(t->history[t->histCount], line, TERM_LINE_MAX-1);
        t->history[t->histCount][TERM_LINE_MAX-1] = '\0';
        t->histCount++;
        return;
    }
    for (int i=1;i<TERM_HISTORY_MAX;i++) strcpy(t->history[i-1], t->history[i]);
    strncpy(t->history[TERM_HISTORY_MAX-1], line, TERM_LINE_MAX-1);
    t->history[TERM_HISTORY_MAX-1][TERM_LINE_MAX-1] = '\0';
}

static void replace_last(ToyTerm* t, const char* line) {
    if (t->histCount <= 0) return;
    strncpy(t->history[t->histCount-1], line, TERM_LINE_MAX-1);
    t->history[t->histCount-1][TERM_LINE_MAX-1] = '\0';
}

static int var_find(ToyTerm* t, const char* name) {
    for (int i=0;i<VAR_MAX;i++) {
        if (t->vars[i].used && strcmp(t->vars[i].name, name)==0) return i;
    }
    return -1;
}

static int var_set(ToyTerm* t, const char* name, double value) {
    int idx = var_find(t, name);
    if (idx>=0) { t->vars[idx].value=value; return idx; }
    for (int i=0;i<VAR_MAX;i++) {
        if (!t->vars[i].used) {
            t->vars[i].used = 1;
            strncpy(t->vars[i].name, name, sizeof(t->vars[i].name)-1);
            t->vars[i].name[sizeof(t->vars[i].name)-1] = '\0';
            t->vars[i].value = value;
            return i;
        }
    }
    return -1;
}

static int var_get(ToyTerm* t, const char* name, double* out) {
    int idx = var_find(t, name);
    if (idx<0) return 0;
    *out = t->vars[idx].value;
    return 1;
}

// --- expression parser ---
typedef struct {
    const char* s;
    int pos;
    char err[128];
    ToyTerm* t;
} Parser;

static void skip_ws(Parser* p) { while (isspace((unsigned char)p->s[p->pos])) p->pos++; }
static int match(Parser* p, char c) { skip_ws(p); if (p->s[p->pos]==c){p->pos++;return 1;} return 0; }
static int peek(Parser* p){ return p->s[p->pos]; }

static double parse_expr(Parser* p);

static double parse_number(Parser* p, int* ok) {
    skip_ws(p);
    int start = p->pos;
    int saw = 0;

    if (p->s[p->pos]=='.') p->pos++;
    while (isdigit((unsigned char)p->s[p->pos])) { p->pos++; saw=1; }
    if (p->s[p->pos]=='.') {
        p->pos++;
        while (isdigit((unsigned char)p->s[p->pos])) { p->pos++; saw=1; }
    }
    if (!saw) { *ok=0; return 0.0; }

    char buf[64];
    int len = p->pos - start;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf)-1;
    memcpy(buf, p->s+start, len);
    buf[len]='\0';
    *ok=1;
    return strtod(buf, NULL);
}

static int parse_ident(Parser* p, char out[32]) {
    skip_ws(p);
    int c = p->s[p->pos];
    if (!(isalpha((unsigned char)c) || c=='_')) return 0;

    int start = p->pos;
    p->pos++;
    while (isalnum((unsigned char)p->s[p->pos]) || p->s[p->pos]=='_') p->pos++;

    int len = p->pos - start;
    if (len>31) len=31;
    memcpy(out, p->s+start, len);
    out[len]='\0';
    return 1;
}

static double parse_factor(Parser* p) {
    skip_ws(p);
    if (match(p,'+')) return parse_factor(p);
    if (match(p,'-')) return -parse_factor(p);

    if (match(p,'(')) {
        double v = parse_expr(p);
        if (!match(p,')')) snprintf(p->err,sizeof(p->err),"Expected ')'");
        return v;
    }

    char ident[32];
    if (parse_ident(p, ident)) {
        double v=0;
        if (!var_get(p->t, ident, &v)) {
            snprintf(p->err,sizeof(p->err),"Undefined variable: %s", ident);
            return 0.0;
        }
        return v;
    }

    int ok=0;
    double num = parse_number(p,&ok);
    if (ok) return num;

    snprintf(p->err,sizeof(p->err),"Unexpected token near '%c'", peek(p)?peek(p):'?');
    return 0.0;
}

static double parse_term(Parser* p) {
    double v = parse_factor(p);
    for (;;) {
        skip_ws(p);
        if (match(p,'*')) v *= parse_factor(p);
        else if (match(p,'/')) {
            double d = parse_factor(p);
            if (fabs(d) < 1e-12) { snprintf(p->err,sizeof(p->err),"Division by zero"); return 0.0; }
            v /= d;
        } else break;
    }
    return v;
}

static double parse_expr(Parser* p) {
    double v = parse_term(p);
    for (;;) {
        skip_ws(p);
        if (match(p,'+')) v += parse_term(p);
        else if (match(p,'-')) v -= parse_term(p);
        else break;
    }
    return v;
}

static int eval_expr(ToyTerm* t, const char* expr, double* out, char err[128]) {
    Parser p = {0};
    p.s = expr;
    p.pos = 0;
    p.err[0] = '\0';
    p.t = t;

    double v = parse_expr(&p);
    skip_ws(&p);

    if (p.err[0]) { strncpy(err,p.err,127); err[127]='\0'; return 0; }
    if (p.s[p.pos] != '\0') { snprintf(err,128,"Unexpected trailing input near '%c'", p.s[p.pos]); return 0; }

    *out = v;
    err[0] = '\0';
    return 1;
}

ToyTerm* term_create(void) {
    ToyTerm* t = (ToyTerm*)calloc(1, sizeof(ToyTerm));
    if (!t) return NULL;
    hist_push(t, "> SYSTEM READY");
    hist_push(t, "> INIT TOY INTERPRETER 0.1");
    hist_push(t, ">>> ");
    return t;
}

void term_destroy(ToyTerm* t) { free(t); }

int term_history_count(const ToyTerm* t) { return t ? t->histCount : 0; }
const char* term_history_line(const ToyTerm* t, int idx) {
    if (!t || idx<0 || idx>=t->histCount) return NULL;
    return t->history[idx];
}

int term_run(ToyTerm* t, const char* cmdIn) {
    if (!t) return 0;
    int before = t->histCount;

    char cmd[256];
    strncpy(cmd, cmdIn ? cmdIn : "", sizeof(cmd)-1);
    cmd[sizeof(cmd)-1] = '\0';

    char inLine[TERM_LINE_MAX];
    snprintf(inLine, sizeof(inLine), ">>> %s", cmd);
    replace_last(t, inLine);

    // trim leading ws
    const char* cmdp = cmd;
    while (*cmdp && isspace((unsigned char)*cmdp)) cmdp++;

    if (*cmdp == '\0') {
        hist_push(t, ">>> ");
        return t->histCount - before;
    }

    // print(expr) or print("text")
    if (strncmp(cmdp, "print", 5)==0) {
        const char* p = cmdp + 5;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '(') { hist_push(t,"Error: print expects parentheses: print(expr)"); hist_push(t,">>> "); return t->histCount-before; }
        p++;
        const char* end = strrchr(p, ')');
        if (!end) { hist_push(t,"Error: Missing ')'"); hist_push(t,">>> "); return t->histCount-before; }

        char inside[256];
        int len = (int)(end - p);
        if (len > 255) len = 255;
        memcpy(inside, p, len);
        inside[len]='\0';

        const char* q = inside;
        while (*q && isspace((unsigned char)*q)) q++;

        size_t qlen = strlen(q);
        if (qlen>=2 && (q[0]=='"' || q[0]=='\'') && q[qlen-1]==q[0]) {
            char out[256];
            size_t inner = qlen - 2;
            if (inner > 255) inner = 255;
            memcpy(out, q+1, inner);
            out[inner]='\0';
            hist_push(t, out);
            hist_push(t, ">>> ");
            return t->histCount-before;
        }

        double val=0;
        char err[128];
        if (!eval_expr(t, inside, &val, err)) {
            char msg[TERM_LINE_MAX];
            snprintf(msg,sizeof(msg),"Error: %s", err);
            hist_push(t, msg);
        } else {
            char out[TERM_LINE_MAX];
            if (fabs(val - round(val)) < 1e-9) snprintf(out,sizeof(out),"%.0f", round(val));
            else snprintf(out,sizeof(out),"%.10g", val);
            hist_push(t, out);
        }
        hist_push(t, ">>> ");
        return t->histCount-before;
    }

    // assignment
    {
        char buf[256];
        strncpy(buf, cmdp, sizeof(buf)-1);
        buf[sizeof(buf)-1] = '\0';

        char* eq = strchr(buf,'=');
        if (eq) {
            *eq = '\0';
            char* lhs = buf;
            char* rhs = eq+1;

            while (*lhs && isspace((unsigned char)*lhs)) lhs++;
            char* lend = lhs + strlen(lhs);
            while (lend>lhs && isspace((unsigned char)lend[-1])) lend--;
            *lend = '\0';

            if (!(isalpha((unsigned char)lhs[0]) || lhs[0]=='_')) { hist_push(t,"Error: Invalid identifier on left-hand side"); hist_push(t,">>> "); return t->histCount-before; }
            for (int i=0; lhs[i]; i++) if (!(isalnum((unsigned char)lhs[i]) || lhs[i]=='_')) { hist_push(t,"Error: Invalid identifier on left-hand side"); hist_push(t,">>> "); return t->histCount-before; }

            double val=0;
            char err[128];
            if (!eval_expr(t, rhs, &val, err)) {
                char msg[TERM_LINE_MAX];
                snprintf(msg,sizeof(msg),"Error: %s", err);
                hist_push(t, msg);
                hist_push(t, ">>> ");
                return t->histCount-before;
            }
            if (var_set(t, lhs, val) < 0) { hist_push(t,"Error: Variable table full"); hist_push(t,">>> "); return t->histCount-before; }
            hist_push(t, ">>> ");
            return t->histCount-before;
        }
    }

    // expression-only
    {
        double val=0;
        char err[128];
        if (!eval_expr(t, cmdp, &val, err)) {
            char msg[TERM_LINE_MAX];
            snprintf(msg,sizeof(msg),"Error: %s", err);
            hist_push(t, msg);
            hist_push(t, ">>> ");
            return t->histCount-before;
        }
        char out[TERM_LINE_MAX];
        if (fabs(val - round(val)) < 1e-9) snprintf(out,sizeof(out),"%.0f", round(val));
        else snprintf(out,sizeof(out),"%.10g", val);
        hist_push(t, out);
        hist_push(t, ">>> ");
    }

    return t->histCount - before;
}
