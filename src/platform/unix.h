#include <dirent.h>

#define CMD_SHUTDOWN "systemctl poweroff"
#define CMD_RESTART "systemctl reboot"
#define CMD_SLEEP "systemctl suspend"
#define EXT_DESKTOP ".desktop"
#define DELIMITER_ACTION ";"
#define DESKTOP_SECTION_HEADER "Desktop Entry"
#define DESKTOP_SECTION_HEADER_ACTION "Desktop Action %s"
#define KEY_EXEC "Exec"
#define KEY_STARTUP_WM_CLASS "StartupWMClass"
#define MAX_INI_SECTION 100

// Bringing an already running application to the front (KDE Plasma, via kdotool)
// The KWin script prints one line per window, top of the stack last:
// id, class, name, desktop file name and pid, separated by tabs
#define CMD_LIST_WINDOWS "kdotool kwinscript --inline '" \
    "let o = []; " \
    "for (const w of workspace.stackingOrder) " \
    "if (w.normalWindow || w.dialog) " \
    "o.push([w.internalId, w.resourceClass, w.resourceName, w.desktopFileName, w.pid].join(\"\\t\")); " \
    "output_result(o.join(\"\\n\"));' 2>/dev/null"
#define CMD_ACTIVATE_WINDOW "kdotool windowactivate '%s' >/dev/null 2>&1"
#define FLATPAK_SCOPE_PREFIX "app-flatpak-"
#define MAX_WINDOW_CANDIDATES 8
#define MAX_WINDOW_LINE 1024
#define MAX_TRACKED_LAUNCHES 32

typedef struct {
    char section[MAX_INI_SECTION + 1];
    char *exec;
    char *startup_wm_class;
} Desktop;

// Names the windows of an application may carry: the desktop file ID, StartupWMClass,
// the Flatpak app ID and the executable name, all lowercase
typedef struct {
    char *names[MAX_WINDOW_CANDIDATES];
    int num_names;
} WindowCandidates;

// The process group of the last launch of a command, to find windows whose class
// doesn't resemble the command
typedef struct {
    char *cmd;
    pid_t pgid;
} TrackedLaunch;
