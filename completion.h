#ifndef COMPLETION_H
#define COMPLETION_H

/* Print the top-level usage menu (replaces old print_usage). */
void print_usage(const char *prog);

/* Print a per-subcommand detailed menu. */
void print_subcommand_menu(const char *subcmd);

#endif
