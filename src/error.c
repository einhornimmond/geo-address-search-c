#include "error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void printHeader(const char *title)
{
    fprintf(stderr,
        "\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        " %s\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n",
        title);
}

// why luna? because she as chatgpt character wrote most of the code

static void printUsage(void)
{
    printHeader("Luna is confused...");

    fputs(
"      /\\_/\\\\\n"
"     ( >_< )\n"
"     /  ^  \\\n"
"\n"
" I don't know what you want me to parse.\n"
" Please tell me which Photon dump to open.\n"
"\n",
    stderr);
}

static void printIo(void)
{
    printHeader("Luna searched everywhere...");

    fputs(
"       /\\   /\\\n"
"       { ;_; }\n"
"      /|     |\\\n"
"     /_|_____|_\\\n"
"\n"
" I couldn't find your file.\n"
" Maybe it's hiding... or maybe the path is wrong.\n"
"\n",
    stderr);
}

static void printJson(void)
{
    printHeader("Luna broke your JSON...");

    fputs(
"      /\\_/\\\\\n"
"    =( x.x )=\n"
"     /  |  \\\n"
"\n"
" ...okay, maybe you broke it.\n"
"\n",
    stderr);
}

static void printZstd(void)
{
    printHeader("Luna fell asleep inside the .zst stream...");

    fputs(
"      /\\___/\\\\\n"
"    =( -.- )= zZz\n"
"     /  |  \\\n"
"\n"
" The compressed stream doesn't look right.\n"
"\n",
    stderr);
}

static void printMemory(void)
{
    printHeader("Luna ate all your RAM...");

    fputs(
"      /\\_/\\\\\n"
"    =( @o@ )=\n"
"     /[RAM]\\\n"
"\n"
" She apologizes.\n"
" (No she doesn't.)\n"
"\n",
    stderr);
}

static void printAssert(void)
{
    printHeader("Luna is judging your code...");

    fputs(
"      /\\___/\\\\\n"
"    =( ಠ_ಠ )=\n"
"      /   \\\n"
"\n"
" Something impossible just happened.\n"
"\n",
    stderr);
}

static void printInfo(void)
{
    printHeader("Luna has an update...");

    fputs(
"      /\\_/\\\n"
"     ( ^_^ )\n"
"     /  !  \\\n"
"\n"
" Here's something you should know.\n"
"\n",
    stdout);
}

_Noreturn void fatal(ErrorArt art, const char *fmt, ...)
{
    switch (art) {
        case ERROR_USAGE:  printUsage();  break;
        case ERROR_IO:     printIo();     break;
        case ERROR_JSON:   printJson();   break;
        case ERROR_ZSTD:   printZstd();   break;
        case ERROR_MEMORY: printMemory(); break;
        case ERROR_ASSERT: printAssert(); break;
        case ERROR_INFO:   printInfo();   break;
    }

    fprintf(stderr, "Error: ");

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);

    exit(EXIT_FAILURE);
}

void info(const char* fmt, ...)
{
    printInfo();
    fprintf(stdout, "Info: ");

    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);

    fputc('\n', stdout);
}