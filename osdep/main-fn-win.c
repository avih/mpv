#include <windows.h>

#ifndef BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE
#define BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE (0x0001)
#endif

#include "config.h"

#include "common/common.h"
#include "osdep/io.h"
#include "osdep/terminal.h"
#include "osdep/main-fn.h"

static bool is_valid_handle(HANDLE h)
{
    return h != INVALID_HANDLE_VALUE && h != NULL &&
           GetFileType(h) != FILE_TYPE_UNKNOWN;
}

static bool has_redirected_stdio(void)
{
    return is_valid_handle(GetStdHandle(STD_INPUT_HANDLE)) ||
           is_valid_handle(GetStdHandle(STD_OUTPUT_HANDLE)) ||
           is_valid_handle(GetStdHandle(STD_ERROR_HANDLE));
}

static void microsoft_nonsense(void)
{
    // stop Windows from showing all kinds of annoying error dialogs
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    // Enable heap corruption detection
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    WINBOOL (WINAPI *pSetSearchPathMode)(DWORD Flags) =
        (WINBOOL (WINAPI *)(DWORD))GetProcAddress(kernel32, "SetSearchPathMode");

    // Always use safe search paths for DLLs and other files, ie. never use the
    // current directory
    SetDllDirectoryW(L"");
    if (pSetSearchPathMode)
        pSetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE);
}

// Cygwin/msys2 pty is a pipe with the following format (H: hex-digit, N: 0-9):
// '\{cygwin,msys}-HHHHHHHHHHHHHHHH-ptyN-{from,to}-master'
static bool cyg_isatty(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if ((h == INVALID_HANDLE_VALUE) || (GetFileType(h) != FILE_TYPE_PIPE))
        return false;

    UINT infosize = sizeof(FILE_NAME_INFO) + sizeof(WCHAR) * MAX_PATH;
    FILE_NAME_INFO *info = malloc(infosize + sizeof(WCHAR));  // +1 for '\0'
    if (!info)
        return false;

    bool rv = false;
    if (GetFileInformationByHandleEx(h, FileNameInfo, info, infosize)) {
        WCHAR *n = info->FileName;
        // not terminated https://msdn.microsoft.com/en-us/library/ee791145.aspx
        n[info->FileNameLength] = 0;  // we allocated for +1 WCHAR even if full

        rv = ((wcsstr(n, L"\\msys-") == n) || (wcsstr(n, L"\\cygwin-") == n)) &&
             wcsstr(n, L"-pty") &&
             (wcsstr(n, L"-from-master") || wcsstr(n, L"-to-master"));
    }
    free(info);
    return rv;
}

// returns false on failure, needs both stdin and stdout connected to the pty.
// also needs the terminal in non line mode, which has to be set in advance,
// e.g. by `stty -icanon` (and restore it later with `stty icanon`), otherwise
// scanf will hang till the user presses enter (and then continue correctly).
// based on resize.c of the xterm package: http://invisible-island.net/xterm/
static bool vt_getsize(int *rows, int *cols) {
    *rows = 24; *cols = 80;
    return 1;
    // disabled for now. too annoyignn to press enter if the term is not set up.

    // save cursor, disable margin(?), move cursor, request pos, restore cursor
    printf("\e7\e[r\e[9999;9999H\e[6n\e8"); fflush(stdout);
    return 2 == scanf("\e[%d;%dR", rows, cols);  // expected reply format
}

int main(int argc_, char **argv_)
{
    microsoft_nonsense();

    // doesn't print anything for mpv.{com,exe} in cmd.exe, but it's NO, NO, NO
    printf("cyg_isatty stdin:%d  stdout:%d  stderr:%d\n",
            cyg_isatty(0), cyg_isatty(1), cyg_isatty(2));

    int rows = -1, cols = -1;
    if (cyg_isatty(0) && cyg_isatty(1) && vt_getsize(&rows, &cols)) {
        printf("VT terminal size: rows: %d, cols: %d.\n", rows, cols);
    } else {
        printf("Not VT or cannot read terminal size.\n");
    }

    // If started from the console wrapper (see osdep/win32-console-wrapper.c),
    // attach to the console and set up the standard IO handles
    bool has_console = terminal_try_attach();

    // If mpv is started from Explorer, the Run dialog or the Start Menu, it
    // will have no console and no standard IO handles. In this case, the user
    // is expecting mpv to show some UI, so enable the pseudo-GUI profile.
    bool gui = !has_console && !has_redirected_stdio();

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    int argv_len = 0;
    char **argv_u8 = NULL;

    // Build mpv's UTF-8 argv, and add the pseudo-GUI profile if necessary
    if (argc > 0 && argv[0])
        MP_TARRAY_APPEND(NULL, argv_u8, argv_len, mp_to_utf8(argv_u8, argv[0]));
    if (gui) {
        MP_TARRAY_APPEND(NULL, argv_u8, argv_len,
                         "--player-operation-mode=pseudo-gui");
    }
    for (int i = 1; i < argc; i++)
        MP_TARRAY_APPEND(NULL, argv_u8, argv_len, mp_to_utf8(argv_u8, argv[i]));
    MP_TARRAY_APPEND(NULL, argv_u8, argv_len, NULL);

    int ret = mpv_main(argv_len - 1, argv_u8);

    talloc_free(argv_u8);
    return ret;
}
