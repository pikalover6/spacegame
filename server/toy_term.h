#ifndef TOY_TERM_H
#define TOY_TERM_H

#ifdef __cplusplus
extern "C" {
#endif

#define TERM_HISTORY_MAX 256
#define TERM_LINE_MAX    256

typedef struct ToyTerm ToyTerm;

ToyTerm* term_create(void);
void     term_destroy(ToyTerm* t);

// Runs a command; pushes ">>> cmd", outputs, and new prompt into history.
// Returns number of new lines added since the call began.
int      term_run(ToyTerm* t, const char* cmd);

// Access history lines
int      term_history_count(const ToyTerm* t);
const char* term_history_line(const ToyTerm* t, int idx);

// Clear typed buffer is client-side; server only holds history + vars.

#ifdef __cplusplus
}
#endif

#endif
