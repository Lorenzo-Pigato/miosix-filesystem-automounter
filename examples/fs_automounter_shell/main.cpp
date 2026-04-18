#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filesystem/automounter/sd_automounter.h"

using namespace miosix;

static char cwd[128] = "/";
static char line[256];
static char arg[256];

static void cmd_cd(const char *path)
{
    if(chdir(path) != 0) iprintf("cd: %s\n", strerror(errno));
    getcwd(cwd, sizeof(cwd));
}

static void cmd_ls(const char *path)
{
    const char *target = path[0] ? path : ".";
    DIR *d = opendir(target);
    if(!d)
    {
        iprintf("ls: %s\n", strerror(errno));
        return;
    }

    dirent *e;
    while((e = readdir(d)) != nullptr)
    {
        struct stat st;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", target, e->d_name);
        bool isDir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        iprintf("  %s%s\n", e->d_name, isDir ? "/" : "");
    }
    closedir(d);
}

static void cmd_stat(const char *path)
{
    struct stat st;
    if(stat(path, &st) != 0)
    {
        iprintf("stat: %s\n", strerror(errno));
        return;
    }
    iprintf("  dev=%d size=%d mode=0%o\n",
            static_cast<int>(st.st_dev),
            static_cast<int>(st.st_size),
            static_cast<unsigned>(st.st_mode));
}

static void cmd_cat(const char *path)
{
    FILE *f = fopen(path, "r");
    if(!f)
    {
        iprintf("cat: %s\n", strerror(errno));
        return;
    }
    char buf[128];
    while(fgets(buf, sizeof(buf), f)) iprintf("%s", buf);
    fclose(f);
    iprintf("\n");
}

static void cmd_echo(const char *args)
{
    const char *sep = strstr(args, ">>");
    bool append = true;
    if(!sep)
    {
        sep = strstr(args, ">");
        append = false;
    }

    if(!sep)
    {
        iprintf("%s\n", args);
        return;
    }

    char text[256];
    size_t textLen = static_cast<size_t>(sep - args);
    while(textLen > 0 && args[textLen - 1] == ' ') textLen--;
    if(textLen >= sizeof(text)) textLen = sizeof(text) - 1;
    memcpy(text, args, textLen);
    text[textLen] = '\0';

    const char *path = sep + (append ? 2 : 1);
    while(*path == ' ') path++;
    if(*path == '\0')
    {
        iprintf("echo: missing filename\n");
        return;
    }

    FILE *f;
    if(append)
    {
        f = fopen(path, "r+");
        if(f) fseek(f, 0, SEEK_END);
        else f = fopen(path, "w");
    } else {
        f = fopen(path, "w");
    }
    if(!f)
    {
        iprintf("echo: %s\n", strerror(errno));
        return;
    }
    fprintf(f, "%s\n", text);
    fclose(f);
}

static void cmd_touch(const char *path)
{
    if(*path == '\0')
    {
        iprintf("touch: missing filename\n");
        return;
    }

    struct stat st;
    if(stat(path, &st) == 0) return;
    FILE *f = fopen(path, "w");
    if(!f)
    {
        iprintf("touch: %s\n", strerror(errno));
        return;
    }
    fclose(f);
}

static void cmd_rm(const char *path)
{
    if(*path == '\0')
    {
        iprintf("rm: missing filename\n");
        return;
    }
    if(remove(path) != 0) iprintf("rm: %s\n", strerror(errno));
}

static void cmd_enable()
{
    SdAutomounter::instance().enable();
    iprintf("automounter enabled\n");
}

static void cmd_disable()
{
    SdAutomounter::instance().disable();
    iprintf("automounter disabled\n");
}

int main()
{
    getcwd(cwd, sizeof(cwd));
    for(;;)
    {
        iprintf("stm32 |%s| >> ", cwd);

        if(!fgets(line, sizeof(line), stdin)) continue;

        char *nl = strchr(line, '\n');
        if(nl) *nl = '\0';
        if(line[0] == '\0') continue;

        arg[0] = '\0';
        char *sp = strchr(line, ' ');
        if(sp)
        {
            *sp = '\0';
            strncpy(arg, sp + 1, sizeof(arg) - 1);
            arg[sizeof(arg) - 1] = '\0';
        }

        if(strcmp(line, "cd") == 0) cmd_cd(arg[0] ? arg : "/");
        else if(strcmp(line, "ls") == 0) cmd_ls(arg);
        else if(strcmp(line, "stat") == 0) cmd_stat(arg);
        else if(strcmp(line, "cat") == 0) cmd_cat(arg);
        else if(strcmp(line, "echo") == 0) cmd_echo(arg);
        else if(strcmp(line, "touch") == 0) cmd_touch(arg);
        else if(strcmp(line, "rm") == 0) cmd_rm(arg);
        else if(strcmp(line, "enable") == 0) cmd_enable();
        else if(strcmp(line, "disable") == 0) cmd_disable();
        else iprintf("commands: cd ls stat cat echo touch rm enable disable\n");
    }
}
