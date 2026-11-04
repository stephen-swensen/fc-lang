#pragma once
#include <stdbool.h>

/* A line editor for the interactive session's prompt (fcc --interactive): a
 * terminal in raw mode, with cursor movement, in-memory history, and
 * bracketed paste. fcc depends on nothing but the C library, so this stands
 * in for readline, with the editing keys most shells share:
 *
 *   Left/Right, Ctrl+B/F     move           Home/End, Ctrl+A/E  line start/end
 *   Backspace, Delete        erase          Ctrl+K/U/W          erase to end/start, a word
 *   Up/Down, Ctrl+P/N        history        Tab                 four spaces
 *   Ctrl+L                   clear screen   Ctrl+C              discard the input
 *   Ctrl+D                   end the session on an empty line, else Delete
 *
 * A paste arrives as one piece (the terminal brackets it), newlines and
 * all, so it is never mistaken for lines typed one by one. */

typedef enum {
    LE_LINE,        /* a line, without its newline */
    LE_PASTE,       /* a paste that ended in a newline: one input, every line of it */
    LE_INTERRUPT,   /* Ctrl+C: discard what has been entered */
    LE_EOF,         /* Ctrl+D on an empty line, or the end of the input */
} LineResult;

/* Read a line at `prompt` from the terminal on stdin. *out receives the
 * text (malloc'd; NULL for LE_INTERRUPT and LE_EOF). */
LineResult lineedit_read(const char *prompt, char **out);

/* Add an input to the history that Up and Down walk. */
void lineedit_history_add(const char *entry);

void lineedit_free(void);
