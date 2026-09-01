#include "liteos/libc.h"

#include <unistd.h>

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;

static const char *g_option_cursor;
static int g_option_token_index = -1;

static void reset_option_cursor(void) {
    g_option_cursor = 0;
    g_option_token_index = -1;
}

static void prepare_option_parse(void) {
    optarg = 0;
    if (optind == 0) {
        optind = 1;
        reset_option_cursor();
    }
    if (optind < 1) optind = 1;
    if (g_option_cursor != 0 && optind != g_option_token_index) {
        reset_option_cursor();
    }
}

static int option_error(int option, const char *message) {
    optopt = option;
    if (opterr != 0 && message != 0) (void)fputs(message, stderr);
    return '?';
}

int getopt(int argc, char *const argv[], const char *options) {
    char option_char;
    const char *option;
    bool requires_argument;
    bool has_optional_argument;
    if (options == 0) return -1;
    prepare_option_parse();
    if (g_option_cursor == 0 || *g_option_cursor == '\0') {
        if (optind >= argc || argv == 0 || argv[optind] == 0 ||
            argv[optind][0] != '-' || argv[optind][1] == '\0') return -1;
        if (strcmp(argv[optind], "--") == 0) {
            ++optind;
            return -1;
        }
        g_option_cursor = argv[optind] + 1;
        g_option_token_index = optind;
    }
    option_char = *g_option_cursor++;
    option = strchr(options, (unsigned char)option_char);
    if (option == 0 || option_char == ':') {
        if (*g_option_cursor == '\0') {
            ++optind;
            g_option_cursor = 0;
        }
        return option_error((unsigned char)option_char, "unknown option\n");
    }
    requires_argument = option[1] == ':';
    has_optional_argument = requires_argument && option[2] == ':';
    if (requires_argument) {
        if (*g_option_cursor != '\0') {
            optarg = (char *)g_option_cursor;
            ++optind;
            g_option_cursor = 0;
        } else if (!has_optional_argument && optind + 1 < argc) {
            optarg = argv[++optind];
            ++optind;
            g_option_cursor = 0;
        } else if (has_optional_argument) {
            ++optind;
            g_option_cursor = 0;
        } else {
            int value = (unsigned char)option[0];
            ++optind;
            g_option_cursor = 0;
            if (options[0] == ':') return ':';
            return option_error(value, "option requires an argument\n");
        }
    } else if (*g_option_cursor == '\0') {
        ++optind;
        g_option_cursor = 0;
    }
    return (unsigned char)option[0];
}

static int long_option_match(const char *name, size_t length,
                             const struct option *options, bool *ambiguous) {
    int match = -1;
    size_t index;
    if (ambiguous != 0) *ambiguous = false;
    if (name == 0 || options == 0) return -1;
    for (index = 0U; options[index].name != 0; ++index) {
        size_t option_length = strlen(options[index].name);
        if (length == option_length &&
            strncmp(name, options[index].name, length) == 0) {
            return (int)index;
        }
        if (length < option_length &&
            strncmp(name, options[index].name, length) == 0) {
            if (match >= 0) {
                if (ambiguous != 0) *ambiguous = true;
            } else {
                match = (int)index;
            }
        }
    }
    return match;
}

static int parse_long_option(int argc, char *const argv[],
                             const char *short_options,
                             const struct option *long_options,
                             int *long_index, bool long_only) {
    const char *token;
    const char *name;
    const char *equals;
    size_t name_length;
    bool ambiguous = false;
    int option_index;
    const struct option *option;

    prepare_option_parse();
    if (argv == 0 || long_options == 0 || optind < 1 || optind >= argc ||
        argv[optind] == 0) return getopt(argc, argv, short_options);
    token = argv[optind];
    if (token[0] != '-' || token[1] == '\0' ||
        (!long_only && token[1] != '-') ||
        (long_only && token[1] == '-' && token[2] == '\0')) {
        return getopt(argc, argv, short_options);
    }
    name = token + (token[1] == '-' ? 2 : 1);
    if (*name == '\0') return getopt(argc, argv, short_options);
    equals = strchr(name, '=');
    name_length = equals == 0 ? strlen(name) : (size_t)(equals - name);
    option_index = long_option_match(name, name_length, long_options, &ambiguous);
    if (ambiguous || option_index < 0) {
        ++optind;
        reset_option_cursor();
        return option_error(name[0], ambiguous ? "ambiguous option\n" :
                            "unrecognized option\n");
    }
    option = &long_options[option_index];
    if (option->has_arg < no_argument || option->has_arg > optional_argument) {
        ++optind;
        reset_option_cursor();
        return option_error(option->val, "invalid long option\n");
    }
    optarg = 0;
    if (option->has_arg == required_argument) {
        if (equals != 0) {
            optarg = (char *)(equals + 1);
        } else if (optind + 1 < argc && argv[optind + 1] != 0) {
            optarg = argv[++optind];
        } else {
            ++optind;
            reset_option_cursor();
            if (short_options != 0 && short_options[0] == ':') return ':';
            return option_error(option->val, "option requires an argument\n");
        }
    } else if (option->has_arg == optional_argument && equals != 0) {
        optarg = (char *)(equals + 1);
    } else if (option->has_arg == no_argument && equals != 0) {
        ++optind;
        reset_option_cursor();
        return option_error(option->val, "option does not take an argument\n");
    }
    ++optind;
    reset_option_cursor();
    if (long_index != 0) *long_index = option_index;
    if (option->flag != 0) {
        *option->flag = option->val;
        return 0;
    }
    return option->val;
}

int getopt_long(int argc, char *const argv[], const char *short_options,
                const struct option *long_options, int *long_index) {
    return parse_long_option(argc, argv, short_options, long_options,
                             long_index, false);
}

int getopt_long_only(int argc, char *const argv[], const char *short_options,
                     const struct option *long_options, int *long_index) {
    return parse_long_option(argc, argv, short_options, long_options,
                             long_index, true);
}
