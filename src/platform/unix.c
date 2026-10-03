#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <ctype.h>
#include <stdio.h>
#include <SDL.h>
#include <ini.h>
#include "../launcher.h"
#include <launcher_config.h>
#include "unix.h"
#include "../util.h"
#include "../debug.h"
#include "platform.h"
#include "slideshow.h"

static int desktop_handler(void *user, const char *section, const char *name, const char *value);
static void strip_field_codes(char *cmd);
static bool ends_with(const char *string, const char *phrase);
static int image_filter(const struct dirent *file);
static void add_window_candidate(WindowCandidates *candidates, const char *name, size_t length);
static void add_exec_candidates(WindowCandidates *candidates, const char *exec);
static void free_window_candidates(WindowCandidates *candidates);
static bool in_flatpak_scope(pid_t pid, WindowCandidates *candidates);
static pid_t tracked_pgid(const char *cmd);
static void track_launch(const char *cmd, pid_t pgid);
static bool activate_running_application(const char *entry_cmd, WindowCandidates *candidates);

static TrackedLaunch tracked_launches[MAX_TRACKED_LAUNCHES];
static int num_tracked_launches = 0;

// A function to handle .desktop lines
static int desktop_handler(void *user, const char *section, const char *name, const char *value)
{
    Desktop *pdesktop = (Desktop*) user;
    if (!strcmp(pdesktop->section, section) && !strcmp(name, KEY_EXEC))
        pdesktop->exec = strdup(value);
    else if (!strcmp(section, DESKTOP_SECTION_HEADER) && !strcmp(name, KEY_STARTUP_WM_CLASS))
        pdesktop->startup_wm_class = strdup(value);
    return 0;
}

// A function to determine if a file exists in the filesystem
bool file_exists(const char *path)
{
    return access(path, R_OK) ? false : true;
}

// A function to determine if a directory exists in the filesystem
bool directory_exists(const char *path)
{
    struct stat directory;
    return stat(path, &directory) == 0 && S_ISDIR(directory.st_mode) ? true : false;
}

// A function to remove field codes from .desktop file Exec line
static void strip_field_codes(char *cmd)
{
    size_t start = 0;
    for (size_t i = 0; i < strlen(cmd); i++) {
        if (cmd[i] == '%' && i > 0 && cmd[i - 1] == ' ')
            start = i;
        else if (start && i > start + 2 && cmd[i] != ' ') {
            memmove(cmd + start, cmd + i, strlen(cmd + i) + 1); // The strings overlap
            i = start - 1; // Look at the moved text again, which may start with another field code
            start = 0;
        }
    }
    if (start)
        cmd[start - 1] ='\0'; 
}

// A function to make a directory, including any intermediate
// directories if necessary
void make_directory(const char *directory) 
{
    char buffer[MAX_PATH_CHARS + 1];
    char *i = NULL;
    size_t length;
    snprintf(buffer, sizeof(buffer), "%s", directory);
    length = strlen(buffer);
    if (buffer[length - 1] == '/')
        buffer[length - 1] = '\0';
    for (i = buffer + 1; *i != '\0'; i++) {
        if (*i == '/') {
            *i = '\0';
            mkdir(buffer, S_IRWXU);
            *i = '/';
        }
    }
    mkdir(buffer, S_IRWXU);
}

// A function to determine if a string ends with a phrase
static bool ends_with(const char *string, const char *phrase)
{
    size_t len_string = strlen(string);
    size_t len_phrase = strlen(phrase);
    if (len_phrase > len_string)
        return false;
    char *p = (char*) string + len_string - len_phrase;
    return strcmp(p, phrase) ? false : true;
}

// A function to add a lowercase window name candidate, skipping duplicates and
// names that belong to wrappers rather than applications
static void add_window_candidate(WindowCandidates *candidates, const char *name, size_t length)
{
    static const char *const wrappers[] = {"flatpak", "env", "sh", "bash"};
    if (length == 0 || candidates->num_names >= MAX_WINDOW_CANDIDATES)
        return;
    char *candidate = malloc(length + 1);
    for (size_t i = 0; i < length; i++)
        candidate[i] = (char) tolower((unsigned char) name[i]);
    candidate[length] = '\0';
    bool skip = false;
    for (size_t i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); i++)
        skip |= !strcmp(candidate, wrappers[i]);
    for (int i = 0; i < candidates->num_names; i++)
        skip |= !strcmp(candidate, candidates->names[i]);
    if (skip)
        free(candidate);
    else
        candidates->names[candidates->num_names++] = candidate;
}

// A function to add the window name candidates of a shell command: the executable's
// name, and the app ID of a 'flatpak run' command
static void add_exec_candidates(WindowCandidates *candidates, const char *exec)
{
    char *tmp = strdup(exec);
    bool flatpak = false;
    bool flatpak_run = false;
    for (char *token = strtok(tmp, " \t"); token != NULL; token = strtok(NULL, " \t")) {
        // Remove quotes around the token
        size_t length = strlen(token);
        if (length >= 2 && (token[0] == '"' || token[0] == '\'') && token[length - 1] == token[0]) {
            token[length - 1] = '\0';
            token++;
        }

        if (flatpak_run) {
            if (token[0] != '-' && strchr(token, '.') != NULL) {
                add_window_candidate(candidates, token, strlen(token));
                break;
            }
        }
        else if (flatpak) {
            if (!strcmp(token, "run"))
                flatpak_run = true;
            else if (token[0] != '-')
                break;
        }

        // Skip 'env' and environment variable assignments in front of the executable
        else if (!strcmp(token, "env") || (token[0] != '-' && strchr(token, '=') != NULL))
            continue;
        else {
            // The executable. Flatpak's exported launchers are named after the app ID
            const char *base = strrchr(token, '/');
            base = base == NULL ? token : base + 1;
            if (!strcmp(base, "flatpak"))
                flatpak = true;
            else {
                add_window_candidate(candidates, base, strlen(base));
                break;
            }
        }
    }
    free(tmp);
}

static void free_window_candidates(WindowCandidates *candidates)
{
    for (int i = 0; i < candidates->num_names; i++)
        free(candidates->names[i]);
    candidates->num_names = 0;
}

// A function to determine if a process runs in the systemd scope Flatpak creates for
// one of the candidate app IDs. This finds Flatpak apps whose windows are named
// differently from the app ID, e.g. RetroDECK
static bool in_flatpak_scope(pid_t pid, WindowCandidates *candidates)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cgroup", (int) pid);
    FILE *file = fopen(path, "r");
    if (file == NULL)
        return false;
    char cgroup[MAX_WINDOW_LINE];
    size_t length = fread(cgroup, 1, sizeof(cgroup) - 1, file);
    fclose(file);
    cgroup[length] = '\0';
    for (size_t i = 0; i < length; i++)
        cgroup[i] = (char) tolower((unsigned char) cgroup[i]);

    char scope[MAX_WINDOW_LINE];
    for (int i = 0; i < candidates->num_names; i++) {
        snprintf(scope, sizeof(scope), "/" FLATPAK_SCOPE_PREFIX "%s-", candidates->names[i]);
        if (strstr(cgroup, scope) != NULL)
            return true;
    }
    return false;
}

// A function to get the process group of the last launch of a command
static pid_t tracked_pgid(const char *cmd)
{
    for (int i = 0; i < num_tracked_launches; i++) {
        if (!strcmp(tracked_launches[i].cmd, cmd))
            return tracked_launches[i].pgid;
    }
    return 0;
}

// A function to remember the process group of a launched command
static void track_launch(const char *cmd, pid_t pgid)
{
    for (int i = 0; i < num_tracked_launches; i++) {
        if (!strcmp(tracked_launches[i].cmd, cmd)) {
            tracked_launches[i].pgid = pgid;
            return;
        }
    }
    if (num_tracked_launches == MAX_TRACKED_LAUNCHES) {
        free(tracked_launches[0].cmd);
        memmove(tracked_launches, tracked_launches + 1, (MAX_TRACKED_LAUNCHES - 1) * sizeof(TrackedLaunch));
        num_tracked_launches--;
    }
    tracked_launches[num_tracked_launches++] = (TrackedLaunch) {.cmd = strdup(cmd), .pgid = pgid};
}

// A function to bring an already running instance of an application to the front.
// A window matches when its class, name or desktop file name is one of the
// candidates, or its process runs in the candidate's Flatpak scope. Failing that,
// a window from the process group of the last launch of the command matches.
// Returns false if the application isn't running, or the windows can't be listed
// (not on KDE Plasma, or kdotool isn't installed)
static bool activate_running_application(const char *entry_cmd, WindowCandidates *candidates)
{
    FILE *pipe = popen(CMD_LIST_WINDOWS, "r");
    if (pipe == NULL)
        return false;

    pid_t launch_pgid = tracked_pgid(entry_cmd);
    pid_t launcher_pid = getpid();
    char best_id[MAX_WINDOW_LINE] = "";
    int best_score = 0;
    char line[MAX_WINDOW_LINE];
    while (fgets(line, sizeof(line), pipe) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        char *fields[5] = {line};
        int num_fields = 1;
        for (char *p = line; num_fields < 5 && (p = strchr(p, '\t')) != NULL; num_fields++) {
            *p++ = '\0';
            fields[num_fields] = p;
        }
        if (num_fields < 5)
            continue;
        const char *id = fields[0];
        pid_t pid = (pid_t) atoi(fields[4]);
        if (pid == launcher_pid)
            continue;

        // Window IDs are UUIDs in braces. Anything else won't go to the shell
        if (id[0] != '{' || strspn(id, "{}-0123456789abcdefABCDEF") != strlen(id))
            continue;

        int score = 0;
        for (int i = 0; i < candidates->num_names && score == 0; i++) {
            for (int j = 1; j <= 3; j++) {
                if (!strcasecmp(fields[j], candidates->names[i]))
                    score = 2;
            }
        }
        if (score == 0 && pid > 0 && in_flatpak_scope(pid, candidates))
            score = 2;
        if (score == 0 && pid > 0 && launch_pgid > 0 && getpgid(pid) == launch_pgid)
            score = 1;

        // Windows come bottom of the stack first, so the topmost match wins a tie
        if (score > 0 && score >= best_score) {
            best_score = score;
            copy_string(best_id, id, sizeof(best_id));
        }
    }
    pclose(pipe);
    if (best_score == 0)
        return false;

    char cmd[MAX_WINDOW_LINE + 64];
    snprintf(cmd, sizeof(cmd), CMD_ACTIVATE_WINDOW, best_id);
    if (system(cmd) != 0) {
        log_error("Could not bring the running application to the front");
        return false;
    }
    log_debug("Application already running, brought window %s to the front", best_id);
    return true;
}

// A function to launch an external application. If the application is already
// running, its window is brought to the front instead
bool start_process(char *cmd, bool application)
{
    const char *entry_cmd = cmd;
    WindowCandidates candidates = {.num_names = 0};

    // Check if the command is an XDG .desktop file
    char *exec = NULL;
    char *tmp = strdup(cmd);
    char *file = strtok(tmp, DELIMITER_ACTION);
    if (ends_with(file, EXT_DESKTOP)) {
        Desktop desktop;
        desktop.exec = NULL;
        desktop.startup_wm_class = NULL;

        // Parse the desktop action from the command (if any)
        const char* const action = strtok(NULL, DELIMITER_ACTION);
        if (action == NULL)
            copy_string(desktop.section, DESKTOP_SECTION_HEADER, sizeof(desktop.section));
        else
            snprintf(desktop.section, sizeof(desktop.section), DESKTOP_SECTION_HEADER_ACTION, action);

        // Parse the .desktop file for the Exec line value
        int error = ini_parse(file, desktop_handler, &desktop);
        if (error < 0) {
            log_error("Desktop file '%s' not found", file);
            free(tmp);
            return false;
        }
        if (desktop.exec == NULL) {
            log_debug("No Exec line found in desktop file '%s'", cmd);
            free(desktop.startup_wm_class);
            free(tmp);
            return false;
        }
        exec = desktop.exec;
        strip_field_codes(exec);
        cmd = exec;

        // The desktop file ID, e.g. org.kde.konsole, is the app ID of Wayland windows
        if (application) {
            const char *desktop_id = strrchr(file, '/');
            desktop_id = desktop_id == NULL ? file : desktop_id + 1;
            add_window_candidate(&candidates, desktop_id, strlen(desktop_id) - strlen(EXT_DESKTOP));
            if (desktop.startup_wm_class != NULL)
                add_window_candidate(&candidates, desktop.startup_wm_class, strlen(desktop.startup_wm_class));
        }
        free(desktop.startup_wm_class);
    }
    free(tmp);

    if (application) {
        add_exec_candidates(&candidates, cmd);
        bool activated = activate_running_application(entry_cmd, &candidates);
        free_window_candidates(&candidates);
        if (activated) {
            free(exec);
            return true;
        }
    }

    // Fork new system shell process
    pid_t child_pid = fork();
    switch(child_pid) {
        case -1:
            log_error("Could not fork new process for application");
            free(exec);
            return false;

        // Child process
        case 0:
            setpgid(0, 0);
            const char *file = "/bin/sh";
            const char *args[] = {
                "sh",
                "-c", 
                cmd, 
                NULL
            };
            execvp(file, (char* const*) args);
            _exit(127); // Never return into the launcher's code in the child

        // Parent process
        default:
            if (!application) {
                free(exec);
                return true;
            }

            // Check to see if the shell successfully launched
            int status = 0;
            SDL_Delay(10);
            if (waitpid(child_pid, &status, WNOHANG) == child_pid && WIFEXITED(status) && WEXITSTATUS(status) > 126) {
                log_error("Application failed to launch");
                free(exec);
                return false;
            }
            track_launch(entry_cmd, child_pid);
            log_debug("Application launched successfully");
            break;
    }
    free(exec);
    return true;
}

// A function to reap child processes that have exited (launched applications and :fork
// commands), so they don't linger as zombie processes
void reap_children()
{
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

// A function to determine if a file is an image file
int image_filter(const struct dirent *file)
{
    size_t len_file = strlen(file->d_name);
    size_t len_extension;
    for (size_t i = 0; i < NUM_IMAGE_EXTENSIONS; i++) {
        len_extension = strlen(extensions[i]);
        if (len_file > len_extension && 
        !strcmp(file->d_name + len_file - len_extension, extensions[i]))
            return 1;
    }
    return 0;
}

// A function to scan a directory for images
void scan_slideshow_directory(Slideshow *slideshow, const char *directory)
{
    struct dirent **files;
    slideshow->num_images = scandir(directory, &files, image_filter, NULL);
    slideshow->images = malloc((size_t) slideshow->num_images * sizeof(char*));
    char file_path[MAX_PATH_CHARS + 1];
    for (int i = 0; i < slideshow->num_images; i++) {
        join_paths(file_path, sizeof(file_path), 2, directory, files[i]->d_name);
        slideshow->images[i] = strdup(file_path);
        free(files[i]);
    }
    free(files);
}

void get_region(char *buffer)
{
    // Work on a copy: strtok would otherwise cut up the LANG environment variable itself,
    // which launched applications inherit (e.g. "en_US.UTF-8" would become "en")
    const char *env = getenv("LANG");
    if (env == NULL)
        return;
    char lang[64];
    copy_string(lang, env, sizeof(lang));
    char *token = strtok(lang, "_");
    if (token == NULL)
        return;
    token = strtok(NULL, ".");
    if (token != NULL && strlen(token) == 2)
        copy_string(buffer, token, 3);
}

// A function to shutdown the computer
void scmd_shutdown()
{
    start_process(CMD_SHUTDOWN, false);
}

// A function to restart the computer
void scmd_restart()
{
    start_process(CMD_RESTART, false);
}

// A function to put the computer to sleep
void scmd_sleep()
{
    start_process(CMD_SLEEP, false);
}

// A function to print usage to the command line
void print_usage()
{
    printf("Usage: " EXECUTABLE_TITLE " [OPTIONS]\n");
    printf("  -c p, --config=p   Load config file from path p.\n");
    printf("  -d,   --debug      Enable debug messages.\n");
    printf("  -h,   --help       Show this help message.\n");
    printf("  -v,   --version    Print version information.\n");
}
