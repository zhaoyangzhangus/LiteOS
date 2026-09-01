#pragma once

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

#define no_argument       0
#define required_argument 1
#define optional_argument 2

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char *const argv[], const char *options);
int getopt_long(int argc, char *const argv[], const char *short_options,
                const struct option *long_options, int *long_index);
int getopt_long_only(int argc, char *const argv[], const char *short_options,
                     const struct option *long_options, int *long_index);
