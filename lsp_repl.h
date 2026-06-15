#ifndef LSP_REPL_H
#define LSP_REPL_H

/* Start the interactive LSP REPL.
   Forks a child running the LSP server and connects via pipes.
   Prints the menu, then drops into a command loop until the user
   types "exit" or presses Ctrl-D.                                 */
int lsp_repl_run(void);

#endif
