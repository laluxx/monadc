#include "cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>

/// Internal helpers

static void make_dir(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error: cannot create directory '%s': %s\n", path, strerror(errno));
        exit(1);
    }
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "Error: cannot write '%s': %s\n", path, strerror(errno)); exit(1); }
    fputs(content, f);
    fclose(f);
}

static char *git_config(const char *key) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "git config --global %s 2>/dev/null", key);
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    char buf[256] = {0};
    if (!fgets(buf, sizeof(buf), p)) { pclose(p); return NULL; }
    pclose(p);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    return len > 0 ? strdup(buf) : NULL;
}

static char *current_year(void) {
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char *buf = malloc(8);
    snprintf(buf, 8, "%d", tm_info->tm_year + 1900);
    return buf;
}

static char *read_file_str(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f); buf[sz] = '\0'; fclose(f);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// YAML parsing
// ─────────────────────────────────────────────────────────────────────────────

static char *yaml_scalar(const char *content, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "%s:", key);
    const char *p = content;
    while (p && *p) {
        if (strncmp(p, search, strlen(search)) == 0) {
            p += strlen(search);
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0' || *p == '\n') break;
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (!end) return NULL;
                return strndup(p, end - p);
            }
            const char *end = p;
            while (*end && *end != '\n' && *end != '\r') end++;
            while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
            return strndup(p, end - p);
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

static char **yaml_list(const char *content, const char *key, int *count) {
    *count = 0;
    char search[128];
    snprintf(search, sizeof(search), "%s:", key);
    const char *p = content;
    bool found_key = false;
    while (p && *p) {
        if (strncmp(p, search, strlen(search)) == 0) { found_key = true; break; }
        p = strchr(p, '\n'); if (p) p++;
    }
    if (!found_key) return NULL;
    p = strchr(p, '\n'); if (!p) return NULL; p++;

    int cap = 8;
    char **items = malloc(sizeof(char *) * cap);
    while (*p) {
        const char *line = p;
        while (*line == ' ' || *line == '\t') line++;
        if (strncmp(line, "- ", 2) == 0) {
            line += 2;
            const char *end = line;
            while (*end && *end != '\n' && *end != '\r') end++;
            while (end > line && (end[-1] == ' ' || end[-1] == '\t')) end--;
            if (*count >= cap) { cap *= 2; items = realloc(items, sizeof(char *) * cap); }
            items[(*count)++] = strndup(line, end - line);
        } else if (*line != '\0' && *line != '\n' && *line != '\r') {
            break;
        }
        p = strchr(p, '\n'); if (!p) break; p++;
    }
    if (*count == 0) { free(items); return NULL; }
    return items;
}

static char *yaml_exe_field(const char *content, const char *exe_name, const char *field) {
    char needle[256];
    snprintf(needle, sizeof(needle), "  %s:", exe_name);
    const char *p = content;
    while (p && *p) {
        if (strncmp(p, needle, strlen(needle)) == 0) break;
        p = strchr(p, '\n'); if (p) p++;
    }
    if (!p || !*p) return NULL;
    p = strchr(p, '\n'); if (!p) return NULL; p++;

    char fn[128];
    snprintf(fn, sizeof(fn), "%s:", field);
    while (*p) {
        const char *line = p;
        if (*line != ' ' && *line != '\t') break;
        while (*line == ' ' || *line == '\t') line++;
        if (strncmp(line, fn, strlen(fn)) == 0) {
            const char *val = line + strlen(fn);
            while (*val == ' ' || *val == '\t') val++;
            const char *end = val;
            while (*end && *end != '\n' && *end != '\r') end++;
            while (end > val && (end[-1] == ' ' || end[-1] == '\t')) end--;
            return strndup(val, end - val);
        }
        p = strchr(p, '\n'); if (!p) break; p++;
    }
    return NULL;
}

static char *yaml_first_exe(const char *content) {
    const char *p = content;
    while (p && *p) {
        if (strncmp(p, "executables:", 12) == 0) break;
        p = strchr(p, '\n'); if (p) p++;
    }
    if (!p || !*p) return NULL;
    p = strchr(p, '\n'); if (!p) return NULL; p++;
    const char *line = p;
    while (*line == ' ' || *line == '\t') line++;
    const char *end = line;
    while (*end && *end != ':' && *end != '\n') end++;
    if (*end != ':') return NULL;
    return strndup(line, end - line);
}

// ─────────────────────────────────────────────────────────────────────────────
// print_usage / parse_flags / get_base_executable_name
// ─────────────────────────────────────────────────────────────────────────────

void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] [<file.mon>]\n", prog);
    fprintf(stderr, "       %s new <package-name>\n", prog);
    fprintf(stderr, "       %s build\n", prog);
    fprintf(stderr, "       %s run\n", prog);
    fprintf(stderr, "       %s clean\n", prog);
    fprintf(stderr, "       %s install\n", prog);
    fprintf(stderr, "       %s test <file.mon>\n", prog);
    fprintf(stderr, "       %s --test <file.mon>\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -i             Start interactive REPL\n");
    fprintf(stderr, "  -o <file>      Output file name\n");
    fprintf(stderr, "  --test         Compile and embed tests (keep binary)\n");
    fprintf(stderr, "  --emit-ir      Emit LLVM IR (.ll)\n");
    fprintf(stderr, "  --emit-bc      Emit LLVM bitcode (.bc)\n");
    fprintf(stderr, "  --emit-asm     Emit assembly (.s)\n");
    fprintf(stderr, "  --emit-obj     Emit object file (.o)\n");
    fprintf(stderr, "  -Wall          Enable all warnings (accepted, no-op)\n");
    fprintf(stderr, "  -Wextra        Enable extra warnings (accepted, no-op)\n");
}

CompilerFlags parse_flags(int argc, char **argv) {
    CompilerFlags flags = {0};
    flags.mode = CMD_COMPILE;

    if (argc < 2) { print_usage(argv[0]); exit(1); }

    // Subcommands
    if (strcmp(argv[1], "new") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s new <name>\n", argv[0]); exit(1); }
        flags.mode         = CMD_NEW;
        flags.package_name = argv[2];
        return flags;
    }
    if (strcmp(argv[1], "run")     == 0) { flags.mode = CMD_RUN;     return flags; }
    if (strcmp(argv[1], "build")   == 0) { flags.mode = CMD_BUILD;   return flags; }
    if (strcmp(argv[1], "clean")   == 0) { flags.mode = CMD_CLEAN;   return flags; }
    if (strcmp(argv[1], "install") == 0) { flags.mode = CMD_INSTALL; return flags; }
    if (strcmp(argv[1], "-i")      == 0) { flags.mode = CMD_REPL; flags.start_repl = true; return flags; }

    // monad test <file.mon>  ->  compile with tests, run _test binary, delete it
    if (strcmp(argv[1], "test") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s test <file.mon>\n", argv[0]); exit(1); }
        flags.mode       = CMD_TEST;
        flags.test_mode  = true;
        flags.test_run   = true;
        flags.input_file = argv[2];
        return flags;
    }

    // monad --test <file.mon>  ->  compile with tests embedded, keep binary
    if (strcmp(argv[1], "--test") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s --test <file.mon>\n", argv[0]); exit(1); }
        flags.test_mode  = true;
        flags.input_file = argv[2];
        int start = 3;
        for (int i = start; i < argc; i++) {
            if      (!strcmp(argv[i], "--emit-ir"))  flags.emit_ir  = true;
            else if (!strcmp(argv[i], "--emit-bc"))  flags.emit_bc  = true;
            else if (!strcmp(argv[i], "--emit-asm")) flags.emit_asm = true;
            else if (!strcmp(argv[i], "--emit-obj")) flags.emit_obj = true;
            else if (!strcmp(argv[i], "-Wall"))   {}
            else if (!strcmp(argv[i], "-Wextra")) {}
            else if (!strcmp(argv[i], "-o")) {
                if (i + 1 >= argc) { fprintf(stderr, "-o requires an argument\n"); exit(1); }
                flags.output_name = argv[++i];
            } else {
                fprintf(stderr, "Unknown flag: %s\n", argv[i]);
                print_usage(argv[0]); exit(1);
            }
        }
        return flags;
    }

    // Internal flag used by cmd_test — compile with tests + _test suffix
    if (strcmp(argv[1], "--test-run") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s --test-run <file.mon>\n", argv[0]); exit(1); }
        flags.test_mode  = true;
        flags.test_run   = true;
        flags.input_file = argv[2];
        return flags;
    }

    // monad <file.mon> [flags...]  →  normal compile
    flags.input_file = argv[1];
    for (int i = 2; i < argc; i++) {
        if      (!strcmp(argv[i], "--emit-ir"))  flags.emit_ir  = true;
        else if (!strcmp(argv[i], "--emit-bc"))  flags.emit_bc  = true;
        else if (!strcmp(argv[i], "--emit-asm")) flags.emit_asm = true;
        else if (!strcmp(argv[i], "--emit-obj")) flags.emit_obj = true;
        else if (!strcmp(argv[i], "-Wall"))   {}
        else if (!strcmp(argv[i], "-Wextra")) {}
        else if (!strcmp(argv[i], "-o")) {
            if (i + 1 >= argc) { fprintf(stderr, "-o requires an argument\n"); exit(1); }
            flags.output_name = argv[++i];
        } else {
            fprintf(stderr, "Unknown flag: %s\n", argv[i]);
            print_usage(argv[0]); exit(1);
        }
    }
    return flags;
}

char *get_base_executable_name(const char *path) {
    char *copy = strdup(path);
    char *base = basename(copy);
    char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
    char *result = strdup(base); free(copy);
    return result;
}

void cmd_new(const char *package_name) {
    char *author = git_config("user.name");
    if (!author) { char *u = getenv("USER"); author = u ? strdup(u) : strdup("unknown"); }
    char *email = git_config("user.email");
    if (!email) email = strdup("");
    char *year = current_year();

    make_dir(package_name);
    char src_path[512]; snprintf(src_path, sizeof(src_path), "%s/src", package_name);
    make_dir(src_path);

    {
        char path[512]; snprintf(path, sizeof(path), "%s/package.yaml", package_name);
        char content[2048];
        snprintf(content, sizeof(content),
            "name:                %s\n"
            "version:             0.1.0.0\n"
            "github:              \"%s/%s\"\n"
            "license:             MIT\n"
            "author:              \"%s\"\n"
            "maintainer:          \"%s\"\n"
            "copyright:           \"%s %s\"\n"
            "\nextra-source-files:\n"
            "\ndescription:         A Monad package\n"
            "\ndependencies:\n- core >= 0.1 && < 1.0\n"
            "\nmonad-options:\n- -Wall\n- -Wextra\n"
            "\nexecutables:\n  %s:\n    main: Main.mon\n    source-dirs: src\n",
            package_name, author, package_name, author, email, year, author, package_name);
        write_file(path, content);
    }
    {
        char path[512]; snprintf(path, sizeof(path), "%s/src/Main.mon", package_name);
        char content[256];
        snprintf(content, sizeof(content),
            "(module Main)\n\n(show \"Say Hello to %s!\")\n", package_name);
        write_file(path, content);
    }
    {
        char path[512]; snprintf(path, sizeof(path), "%s/.gitignore", package_name);
        write_file(path, "build/\n*.o\n*.ll\n*.s\n");
    }
    {
        char path[512]; snprintf(path, sizeof(path), "%s/README.md", package_name);
        char content[512];
        snprintf(content, sizeof(content),
            "# %s\n\nA [Monad](https://github.com/nytrix-lang) project.\n\n"
            "## Build\n\n```sh\nmonad run\n```\n", package_name);
        write_file(path, content);
    }

    printf("\n");
    printf("  ╭─ %s/\n",     package_name);
    printf("  │  ├── src/\n");
    printf("  │  │   ╰── Main.mon\n");
    printf("  │  ├── package.yaml\n");
    printf("  │  ├── README.md\n");
    printf("  │  ╰── .gitignore\n");
    printf("  │\n");
    printf("  ╰─ \x1b[32m✓\x1b[0m Ready!\n\n");
    printf("  cd %s && monad run\n\n", package_name);

    free(author); free(email); free(year);
}

// ─────────────────────────────────────────────────────────────────────────────
// Build helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool file_has_main_module(const char *path) {
    char *src = read_file_str(path);
    if (!src) return false;
    const char *p = src;
    bool found = false;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ';') { while (*p && *p != '\n') p++; continue; }
        if (*p != '(') break;
        if (strncmp(p, "(module", 7) == 0) {
            const char *q = p + 7;
            while (*q == ' ' || *q == '\t') q++;
            if (strncmp(q, "Main", 4) == 0 &&
                (q[4] == ' ' || q[4] == '\t' || q[4] == '\n' || q[4] == '\r' || q[4] == ')'))
                { found = true; break; }
        }
        break;
    }
    free(src); return found;
}

static char *find_main_recursive(const char *dir) {
    char candidate[1024];
    snprintf(candidate, sizeof(candidate), "%s/Main.mon", dir);
    if (access(candidate, F_OK) == 0 && file_has_main_module(candidate))
        return strdup(candidate);

    DIR *d = opendir(dir); if (!d) return NULL;
    char *result = NULL;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && !result) {
        if (ent->d_name[0] == '.') continue;
        char full[1024]; snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (strcmp(full, candidate) == 0) continue;
        struct stat st; if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            result = find_main_recursive(full);
        } else if (S_ISREG(st.st_mode)) {
            size_t len = strlen(ent->d_name);
            if (len > 4 && strcmp(ent->d_name + len - 4, ".mon") == 0)
                if (file_has_main_module(full)) result = strdup(full);
        }
    }
    closedir(d); return result;
}

typedef struct {
    char *pkg_name;
    char *exe_name;
    char *main_file;
    char *build_dir;
    char *out_path;
    char *project_root;
    char *src_dirs;       /* preserved for clean */
    char *monad_options;
} BuildInfo;

static BuildInfo resolve_build_info(void) {
    BuildInfo bi = {0};

    char yaml_path[1024] = "package.yaml";
    bool found = false;
    for (int lvl = 0; lvl < 5; lvl++) {
        if (access(yaml_path, F_OK) == 0) { found = true; break; }
        char tmp[1024]; snprintf(tmp, sizeof(tmp), "../%s", yaml_path);
        strncpy(yaml_path, tmp, sizeof(yaml_path) - 1);
    }
    if (!found) {
        fprintf(stderr, "Error: package.yaml not found\n"); exit(1);
    }

    char proj_root[1024] = ".";
    {
        char tmp[1024]; strncpy(tmp, yaml_path, sizeof(tmp) - 1);
        char *sl = strrchr(tmp, '/');
        if (sl) { *sl = '\0'; strncpy(proj_root, tmp, sizeof(proj_root) - 1); }
    }
    bi.project_root = strdup(proj_root);

    char *yaml = read_file_str(yaml_path);
    if (!yaml) { fprintf(stderr, "Error: cannot read %s\n", yaml_path); exit(1); }

    bi.pkg_name = yaml_scalar(yaml, "name");
    if (!bi.pkg_name) { fprintf(stderr, "Error: 'name' not in package.yaml\n"); exit(1); }

    bi.exe_name = yaml_first_exe(yaml);
    if (!bi.exe_name) bi.exe_name = strdup(bi.pkg_name);

    char *src_dirs = yaml_exe_field(yaml, bi.exe_name, "source-dirs");
    if (!src_dirs) src_dirs = yaml_scalar(yaml, "source-dirs");
    if (!src_dirs) src_dirs = strdup("src");
    bi.src_dirs = src_dirs;  /* owned by bi */

    char *main_val = yaml_exe_field(yaml, bi.exe_name, "main");
    if (!main_val) main_val = strdup("Main.mon");

    char try_path[1024];
    snprintf(try_path, sizeof(try_path), "%s/%s/%s", proj_root, src_dirs, main_val);
    if (access(try_path, F_OK) == 0 && file_has_main_module(try_path)) {
        bi.main_file = strdup(try_path);
    } else {
        char search_dir[1024];
        snprintf(search_dir, sizeof(search_dir), "%s/%s", proj_root, src_dirs);
        bi.main_file = find_main_recursive(search_dir);
        if (!bi.main_file) {
            fprintf(stderr, "Error: cannot find main file '%s' in '%s'\n",
                    main_val, search_dir);
            free(yaml); free(main_val); exit(1);
        }
    }
    free(main_val);

    char build_dir[1024];
    snprintf(build_dir, sizeof(build_dir), "%s/build", proj_root);
    make_dir(build_dir);
    bi.build_dir = strdup(build_dir);

    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/%s", build_dir, bi.exe_name);
    bi.out_path = strdup(out_path);

    int opt_count = 0;
    char **opts = yaml_list(yaml, "monad-options", &opt_count);
    if (opts && opt_count > 0) {
        size_t total = 1;
        for (int i = 0; i < opt_count; i++) total += strlen(opts[i]) + 1;
        bi.monad_options = malloc(total); bi.monad_options[0] = '\0';
        for (int i = 0; i < opt_count; i++) {
            if (i > 0) strcat(bi.monad_options, " ");
            strcat(bi.monad_options, opts[i]);
            free(opts[i]);
        }
        free(opts);
    } else {
        bi.monad_options = strdup("");
    }

    free(yaml);
    return bi;
}

static void build_info_free(BuildInfo *bi) {
    free(bi->pkg_name);  free(bi->exe_name);  free(bi->main_file);
    free(bi->build_dir); free(bi->out_path);  free(bi->project_root);
    free(bi->src_dirs);  free(bi->monad_options);
}

static int do_build(const BuildInfo *bi) {
    char self[1024] = "monad";
    {
        char buf[1024] = {0};
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) strncpy(self, buf, sizeof(self) - 1);
    }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s %s -o %s%s%s",
             self, bi->main_file, bi->out_path,
             bi->monad_options[0] ? " " : "",
             bi->monad_options[0] ? bi->monad_options : "");
    return system(cmd);
}

void cmd_build(void) {
    BuildInfo bi = resolve_build_info();
    printf("╭─ Build %s\n╰", bi.pkg_name);
    fflush(stdout);

    int rc = do_build(&bi);

    if (rc == 0)
        printf("─ \x1b[32m✓\x1b[0m build/%s\n", bi.exe_name);
    else
        printf("─ failed\n");

    build_info_free(&bi);
    exit(rc == 0 ? 0 : 1);
}

void cmd_run(void) {
    BuildInfo bi = resolve_build_info();
    printf("╭─ Run %s\n╰", bi.pkg_name);
    fflush(stdout);

    int rc = do_build(&bi);
    if (rc != 0) {
        printf("─ failed\n");
        build_info_free(&bi); exit(1);
    }

    /* compile() already printed [done] ./build/exe with no newline.
       We print the connecting line immediately after on the same line,
       then the program output follows on the next line.             */
    printf("\n│\n├─ \x1b[32m✓\x1b[0m Build %s\n╰─▶ ", bi.exe_name);
    fflush(stdout);

    rc = system(bi.out_path);
    printf("\n");
    build_info_free(&bi);
    exit(rc == 0 ? 0 : 1);
}

static void clean_ext_in_dir(const char *dir, const char *ext) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t nlen = strlen(ent->d_name);
        size_t elen = strlen(ext);
        if (nlen > elen && strcmp(ent->d_name + nlen - elen, ext) == 0) {
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
            remove(full);
        }
    }
    closedir(d);
}

static void rmdir_recursive(const char *path) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) rmdir_recursive(full);
        else remove(full);
    }
    closedir(d);
    rmdir(path);
}

void cmd_clean(void) {
    BuildInfo bi = resolve_build_info();

    printf("╭─ Clean %s\n", bi.pkg_name);

    /* Remove build/ */
    char build_dir[1024];
    snprintf(build_dir, sizeof(build_dir), "%s/build", bi.project_root);
    if (access(build_dir, F_OK) == 0) {
        rmdir_recursive(build_dir);
        printf("│  removed build/\n");
    }

    /* Remove .o, .ll, .s from source-dirs
       source-dirs is a single path for now; split on spaces if ever needed */
    char src_full[1024];
    snprintf(src_full, sizeof(src_full), "%s/%s", bi.project_root, bi.src_dirs);
    clean_ext_in_dir(src_full, ".o");
    clean_ext_in_dir(src_full, ".ll");
    clean_ext_in_dir(src_full, ".s");
    printf("│  removed *.o *.ll *.s from %s/\n", bi.src_dirs);

    printf("╰─ \x1b[32m✓\x1b[0m done\n");

    build_info_free(&bi);
}

void cmd_install(void) {
    BuildInfo bi = resolve_build_info();

    /* Build if binary doesn't exist yet */
    if (access(bi.out_path, F_OK) != 0) {
        printf("╭─ Install %s\n│  (building first...)\n╰", bi.pkg_name);
        fflush(stdout);
        int rc = do_build(&bi);
        if (rc != 0) {
            printf("─ build failed\n");
            build_info_free(&bi); exit(1);
        }
        printf("\n");
    } else {
        printf("╭─ Install %s\n", bi.pkg_name);
    }

    const char *home = getenv("HOME");
    if (!home) { fprintf(stderr, "Error: $HOME not set\n"); exit(1); }

    /* Ensure ~/.local/bin exists */
    char local[512], local_bin[512];
    snprintf(local,     sizeof(local),     "%s/.local",     home);
    snprintf(local_bin, sizeof(local_bin), "%s/.local/bin", home);
    make_dir(local);
    make_dir(local_bin);

    char dest[512];
    snprintf(dest, sizeof(dest), "%s/%s", local_bin, bi.exe_name);

    /* Copy with install(1) to set correct permissions */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "install -m 755 %s %s", bi.out_path, dest);
    int rc = system(cmd);

    if (rc == 0) {
        printf("│  → %s\n", dest);
        printf("╰─ \x1b[32m✓\x1b[0m installed\n");

        /* Warn if ~/.local/bin not in PATH */
        const char *path_env = getenv("PATH");
        if (!path_env || !strstr(path_env, local_bin)) {
            printf("\n  hint: ~/.local/bin is not in your PATH\n");
            printf("  add to your shell rc:\n");
            printf("    export PATH=\"$HOME/.local/bin:$PATH\"\n\n");
        }
    } else {
        printf("╰─ failed\n");
    }

    build_info_free(&bi);
    exit(rc == 0 ? 0 : 1);
}


void cmd_test(const char *input_file) {
    char self[1024] = "monad";
    {
        char buf[1024] = {0};
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) strncpy(self, buf, sizeof(self) - 1);
    }

    // Use --test-run so compile() appends _test to the binary name
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s --test-run %s", self, input_file);
    int rc = system(cmd);
    if (rc != 0) { fprintf(stderr, "test build failed\n"); exit(1); }

    // Derive and run the _test binary
    char *base = get_base_executable_name(input_file);
    char test_bin[1024];
    snprintf(test_bin, sizeof(test_bin), "./%s_test", base);

    printf("\n");
    rc = system(test_bin);

    // Clean up
    remove(test_bin + 2);
    char obj[1024];
    snprintf(obj, sizeof(obj), "%s_test.o", base);
    remove(obj);

    free(base);
    exit(rc == 0 ? 0 : 1);
}

/* void cmd_test(const char *input_file) { */
/*     // Build the _test binary by invoking the compiler with --test */
/*     char self[1024] = "monad"; */
/*     { */
/*         char buf[1024] = {0}; */
/*         ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1); */
/*         if (n > 0) strncpy(self, buf, sizeof(self) - 1); */
/*     } */

/*     char cmd[4096]; */
/*     snprintf(cmd, sizeof(cmd), "%s --test %s", self, input_file); */
/*     int rc = system(cmd); */
/*     if (rc != 0) { fprintf(stderr, "test build failed\n"); exit(1); } */

/*     // Derive the _test binary name */
/*     char *base = get_base_executable_name(input_file); */
/*     char test_bin[1024]; */
/*     snprintf(test_bin, sizeof(test_bin), "./%s_test", base); */

/*     // Run it */
/*     printf("\n"); */
/*     rc = system(test_bin); */

/*     // Clean up — remove _test binary and its .o */
/*     remove(test_bin + 2);   // strip "./" */
/*     char obj[1024]; */
/*     snprintf(obj, sizeof(obj), "%s_test.o", base); */
/*     remove(obj); */

/*     free(base); */
/*     exit(rc == 0 ? 0 : 1); */
/* } */
