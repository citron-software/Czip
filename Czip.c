/*
 * Czip native Windows edition
 * Build with MinGW-w64:
 *   gcc Czip.c -std=c11 -O2 -municode -mwindows -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lmsimg32 -o Czip.exe
 * Requires 7-Zip (7z.exe) for archive operations.
 */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#define APP_TITLE L"Czip — Archive Studio"
#define MAX_ENTRIES 12000
#define MAX_OUTPUT (64u * 1024u * 1024u)
#define ID_OPEN 101
#define ID_NEW 102
#define ID_ADD 103
#define ID_EXTRACT 104
#define ID_TEST 105
#define ID_DELETE 106
#define ID_LIST 200

typedef struct {
    wchar_t name[4096];
    wchar_t modified[80];
    ULONGLONG size, packed;
    BOOL directory, encrypted;
} ArchiveEntry;

typedef struct {
    HWND edit;
    wchar_t *output;
    size_t capacity;
    BOOL done, accepted;
    BOOL password;
} PromptState;

static HINSTANCE g_instance;
static HWND g_window, g_list, g_archive_label;
static HFONT g_font, g_title_font;
static wchar_t g_archive[MAX_PATH * 4];
static wchar_t g_7zip[MAX_PATH * 4];
static wchar_t g_password[512];
static BOOL g_archive_encrypted, g_header_encrypted;
static ArchiveEntry g_entries[MAX_ENTRIES];
static size_t g_entry_count;
static PromptState *g_prompt;

static void set_status(const wchar_t *message) {
    if (g_archive_label) SetWindowTextW(g_archive_label, message);
}

static BOOL locate_7zip(void) {
    if (SearchPathW(NULL, L"7z.exe", NULL, (DWORD)(sizeof(g_7zip) / sizeof(g_7zip[0])), g_7zip, NULL)) return TRUE;
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramFiles", base, MAX_PATH);
    if (n && n < MAX_PATH) {
        swprintf(g_7zip, sizeof(g_7zip) / sizeof(g_7zip[0]), L"%ls\\7-Zip\\7z.exe", base);
        if (GetFileAttributesW(g_7zip) != INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    n = GetEnvironmentVariableW(L"ProgramFiles(x86)", base, MAX_PATH);
    if (n && n < MAX_PATH) {
        swprintf(g_7zip, sizeof(g_7zip) / sizeof(g_7zip[0]), L"%ls\\7-Zip\\7z.exe", base);
        if (GetFileAttributesW(g_7zip) != INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    MessageBoxW(g_window, L"7-Zip est requis. Installez-le ou ajoutez 7z.exe au PATH, puis relancez Czip.", APP_TITLE, MB_ICONWARNING | MB_OK);
    return FALSE;
}

static BOOL append_text(wchar_t **buffer, size_t *used, size_t *capacity, const wchar_t *text) {
    size_t length = wcslen(text);
    if (*used + length + 1 > *capacity) {
        size_t next = *capacity ? *capacity : 512;
        while (next < *used + length + 1) next *= 2;
        wchar_t *grown = (wchar_t *)realloc(*buffer, next * sizeof(wchar_t));
        if (!grown) return FALSE;
        *buffer = grown;
        *capacity = next;
    }
    memcpy(*buffer + *used, text, length * sizeof(wchar_t));
    *used += length;
    (*buffer)[*used] = L'\0';
    return TRUE;
}

static BOOL append_char(wchar_t **buffer, size_t *used, size_t *capacity, wchar_t c) {
    wchar_t one[2] = {c, L'\0'};
    return append_text(buffer, used, capacity, one);
}

static BOOL append_arg(wchar_t **buffer, size_t *used, size_t *capacity, const wchar_t *arg) {
    if (*used && !append_text(buffer, used, capacity, L" ")) return FALSE;
    if (!append_text(buffer, used, capacity, L"\"")) return FALSE;
    const wchar_t *p = arg;
    while (*p) {
        size_t slashes = 0;
        while (*p == L'\\') { ++slashes; ++p; }
        if (*p == L'"') {
            for (size_t i = 0; i < slashes * 2 + 1; ++i) if (!append_char(buffer, used, capacity, L'\\')) return FALSE;
            if (!append_char(buffer, used, capacity, L'"')) return FALSE;
            ++p;
        } else if (!*p) {
            for (size_t i = 0; i < slashes * 2; ++i) if (!append_char(buffer, used, capacity, L'\\')) return FALSE;
        } else {
            for (size_t i = 0; i < slashes; ++i) if (!append_char(buffer, used, capacity, L'\\')) return FALSE;
            if (!append_char(buffer, used, capacity, *p++)) return FALSE;
        }
    }
    return append_text(buffer, used, capacity, L"\"");
}

static void format_size(ULONGLONG size, wchar_t *out, size_t capacity) {
    static const wchar_t *units[] = {L"o", L"Ko", L"Mo", L"Go", L"To"};
    double value = (double)size; unsigned unit = 0;
    while (value >= 1024.0 && unit < 4) { value /= 1024.0; ++unit; }
    if (!unit) swprintf(out, capacity, L"%.0f %ls", value, units[unit]);
    else swprintf(out, capacity, L"%.1f %ls", value, units[unit]);
}

static wchar_t *utf8_to_wide(const char *bytes, size_t length) {
    int count = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)length, NULL, 0);
    UINT page = CP_UTF8;
    if (!count) {
        page = CP_OEMCP;
        count = MultiByteToWideChar(page, 0, bytes, (int)length, NULL, 0);
    }
    if (!count) return NULL;
    wchar_t *wide = (wchar_t *)calloc((size_t)count + 1, sizeof(wchar_t));
    if (!wide) return NULL;
    MultiByteToWideChar(page, 0, bytes, (int)length, wide, count);
    return wide;
}

static BOOL run_7zip(const wchar_t *working_dir, const wchar_t *const *args, size_t count, wchar_t **output, DWORD *exit_code) {
    if (!locate_7zip()) return FALSE;
    wchar_t *line = NULL;
    size_t used = 0, capacity = 0;
    if (!append_arg(&line, &used, &capacity, g_7zip)) return FALSE;
    for (size_t i = 0; i < count; ++i) if (!append_arg(&line, &used, &capacity, args[i])) { free(line); return FALSE; }
    SECURITY_ATTRIBUTES security = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    HANDLE read_pipe = NULL, write_pipe = NULL;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) { free(line); return FALSE; }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process;
    ZeroMemory(&process, sizeof(process));
    BOOL started = CreateProcessW(g_7zip, line, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                                  NULL, working_dir, &startup, &process);
    free(line);
    CloseHandle(write_pipe);
    if (!started) { CloseHandle(read_pipe); return FALSE; }
    char *bytes = (char *)malloc(8192);
    size_t bytes_used = 0, bytes_capacity = 8192;
    DWORD got = 0;
    while (ReadFile(read_pipe, bytes + bytes_used, (DWORD)(bytes_capacity - bytes_used - 1), &got, NULL) && got) {
        bytes_used += got;
        if (bytes_used > MAX_OUTPUT) break;
        if (bytes_capacity - bytes_used < 2048) {
            bytes_capacity *= 2;
            char *grown = (char *)realloc(bytes, bytes_capacity);
            if (!grown) break;
            bytes = grown;
        }
    }
    CloseHandle(read_pipe);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (bytes) {
        bytes[bytes_used] = '\0';
        *output = utf8_to_wide(bytes, bytes_used);
        free(bytes);
    } else *output = NULL;
    if (exit_code) *exit_code = code;
    return TRUE;
}

static void flush_entry(ArchiveEntry *entry, BOOL has_path, BOOL has_size) {
    if (has_path && has_size && g_entry_count < MAX_ENTRIES) g_entries[g_entry_count++] = *entry;
    ZeroMemory(entry, sizeof(*entry));
}

static void parse_listing(const wchar_t *output) {
    g_entry_count = 0;
    ArchiveEntry entry;
    ZeroMemory(&entry, sizeof(entry));
    BOOL has_path = FALSE, has_size = FALSE;
    const wchar_t *cursor = output;
    while (cursor && *cursor) {
        const wchar_t *end = wcschr(cursor, L'\n');
        size_t length = end ? (size_t)(end - cursor) : wcslen(cursor);
        while (length && (cursor[length - 1] == L'\r' || cursor[length - 1] == L' ')) --length;
        if (!length) {
            flush_entry(&entry, has_path, has_size);
            has_path = has_size = FALSE;
        } else {
            wchar_t line[8192];
            if (length >= sizeof(line) / sizeof(line[0])) length = sizeof(line) / sizeof(line[0]) - 1;
            wmemcpy(line, cursor, length); line[length] = L'\0';
            wchar_t *value = wcsstr(line, L" = ");
            if (value) {
                *value = L'\0'; value += 3;
                if (!wcscmp(line, L"Path")) { wcsncpy(entry.name, value, 4095); has_path = TRUE; }
                else if (!wcscmp(line, L"Size")) { entry.size = _wcstoui64(value, NULL, 10); has_size = TRUE; }
                else if (!wcscmp(line, L"Packed Size")) entry.packed = _wcstoui64(value, NULL, 10);
                else if (!wcscmp(line, L"Modified")) wcsncpy(entry.modified, value, 79);
                else if (!wcscmp(line, L"Folder")) entry.directory = value[0] == L'+';
                else if (!wcscmp(line, L"Attributes")) entry.directory = value[0] == L'D';
                else if (!wcscmp(line, L"Encrypted")) entry.encrypted = value[0] == L'+';
            }
        }
        if (!end) break;
        cursor = end + 1;
    }
    flush_entry(&entry, has_path, has_size);
}

static BOOL ask_text(HWND parent, const wchar_t *title, const wchar_t *label, wchar_t *output, size_t capacity, BOOL password);

static BOOL list_archive(void) {
    const wchar_t *args[] = {L"l", L"-slt", L"-sccUTF-8", g_archive};
    wchar_t *output = NULL;
    DWORD code = 1;
    if (!run_7zip(NULL, args, 4, &output, &code)) return FALSE;
    if (code != 0 && output && (wcsstr(output, L"password") || wcsstr(output, L"encrypted") || wcsstr(output, L"mot de passe"))) {
        wchar_t pass[512] = L"";
        if (g_password[0]) wcsncpy(pass, g_password, 511);
        else if (!ask_text(g_window, L"Archive protégée", L"Mot de passe de l’archive :", pass, 512, TRUE)) { free(output); return FALSE; }
        wcsncpy(g_password, pass, 511); g_header_encrypted = TRUE;
        const wchar_t *retry[] = {L"l", L"-slt", L"-sccUTF-8", pass[0] ? L"-p" : L"-p", g_archive};
        wchar_t *actual_args[5];
        actual_args[0] = (wchar_t *)retry[0]; actual_args[1] = (wchar_t *)retry[1]; actual_args[2] = (wchar_t *)retry[2];
        wchar_t pass_switch[520]; swprintf(pass_switch, 520, L"-p%ls", pass);
        actual_args[3] = pass_switch; actual_args[4] = (wchar_t *)retry[4];
        free(output); output = NULL;
        if (!run_7zip(NULL, (const wchar_t *const *)actual_args, 5, &output, &code)) return FALSE;
    }
    if (code != 0 || !output) {
        MessageBoxW(g_window, output ? output : L"Impossible de lire l’archive.", APP_TITLE, MB_ICONERROR | MB_OK);
        free(output); return FALSE;
    }
    parse_listing(output);
    free(output);
    g_archive_encrypted = g_header_encrypted;
    ListView_DeleteAllItems(g_list);
    for (size_t i = 0; i < g_entry_count; ++i) {
        ArchiveEntry *entry = &g_entries[i];
        LVITEMW row; ZeroMemory(&row, sizeof(row));
        row.mask = LVIF_TEXT; row.iItem = (int)i;
        wchar_t display[4200]; swprintf(display, 4200, L"%ls %ls", entry->directory ? L"▰" : L"▧", entry->name);
        row.pszText = display;
        int item = ListView_InsertItem(g_list, &row);
        wchar_t size[40], packed[40], type[32];
        if (entry->directory) { wcscpy(size, L"—"); wcscpy(packed, L"—"); wcscpy(type, L"Dossier"); }
        else { format_size(entry->size, size, 40); format_size(entry->packed, packed, 40); swprintf(type, 32, L"%ls", wcsrchr(entry->name, L'.') ? wcsrchr(entry->name, L'.') + 1 : L"Fichier"); }
        ListView_SetItemText(g_list, item, 1, size);
        ListView_SetItemText(g_list, item, 2, packed);
        ListView_SetItemText(g_list, item, 3, type);
        ListView_SetItemText(g_list, item, 4, entry->modified[0] ? entry->modified : L"—");
        if (entry->encrypted) g_archive_encrypted = TRUE;
    }
    wchar_t label[MAX_PATH * 4 + 32]; swprintf(label, sizeof(label) / sizeof(label[0]), L"  %ls", g_archive);
    SetWindowTextW(g_archive_label, label);
    return TRUE;
}

static BOOL choose_files(wchar_t *buffer, size_t capacity) {
    OPENFILENAMEW dialog; ZeroMemory(&dialog, sizeof(dialog));
    buffer[0] = L'\0';
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = g_window;
    dialog.lpstrFilter = L"Tous les fichiers\0*.*\0\0";
    dialog.lpstrFile = buffer; dialog.nMaxFile = (DWORD)capacity;
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
    return GetOpenFileNameW(&dialog);
}

static BOOL choose_folder(wchar_t *folder, size_t capacity) {
    BROWSEINFOW browse; ZeroMemory(&browse, sizeof(browse));
    browse.hwndOwner = g_window; browse.lpszTitle = L"Choisir le dossier de destination";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) return FALSE;
    BOOL ok = SHGetPathFromIDListW(item, folder);
    CoTaskMemFree(item);
    return ok;
}

static BOOL choose_archive(BOOL save, wchar_t *path, size_t capacity) {
    OPENFILENAMEW dialog; ZeroMemory(&dialog, sizeof(dialog));
    path[0] = L'\0'; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = g_window;
    dialog.lpstrFilter = L"Archives prises en charge\0*.zip;*.7z;*.tar;*.wim;*.gz;*.lz4;*.iso;*.dll\0Tous les fichiers\0*.*\0\0";
    dialog.lpstrFile = path; dialog.nMaxFile = (DWORD)capacity;
    dialog.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save) { dialog.lpstrDefExt = L"zip"; dialog.lpstrFilter = L"ZIP\0*.zip\0" L"7Z\0*.7z\0\0"; }
    return save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
}

static size_t selected_indices(int *indices, size_t max) {
    size_t count = 0; int row = -1;
    while ((row = ListView_GetNextItem(g_list, row, LVNI_SELECTED)) >= 0 && count < max) indices[count++] = row;
    return count;
}

static BOOL invoke_extract(void) {
    wchar_t folder[MAX_PATH * 4]; if (!choose_folder(folder, MAX_PATH * 4)) return FALSE;
    int rows[2048]; size_t count = selected_indices(rows, 2048);
    wchar_t pass_switch[520]; const wchar_t *args[2055]; size_t n = 0;
    args[n++] = L"x"; args[n++] = L"-sccUTF-8";
    if (g_archive_encrypted) {
        if (!g_password[0] && !ask_text(g_window, L"Archive protégée", L"Mot de passe de l’archive :", g_password, 512, TRUE)) return FALSE;
        swprintf(pass_switch, 520, L"-p%ls", g_password); args[n++] = pass_switch;
    }
    wchar_t out_switch[MAX_PATH * 4 + 8]; swprintf(out_switch, MAX_PATH * 4 + 8, L"-o%ls", folder); args[n++] = out_switch;
    args[n++] = L"-y"; args[n++] = g_archive;
    if (count) {
        args[n++] = L"--";
        for (size_t i = 0; i < count; ++i) args[n++] = g_entries[rows[i]].name;
    }
    wchar_t *output = NULL; DWORD code = 1;
    BOOL ok = run_7zip(NULL, args, n, &output, &code) && code == 0;
    if (!ok) MessageBoxW(g_window, output ? output : L"Échec de l’extraction.", APP_TITLE, MB_ICONERROR | MB_OK);
    else MessageBoxW(g_window, L"Extraction terminée.", APP_TITLE, MB_ICONINFORMATION | MB_OK);
    free(output); return ok;
}

static BOOL invoke_test(void) {
    wchar_t pass_switch[520]; const wchar_t *args[5] = {L"t", L"-sccUTF-8", NULL, NULL, NULL}; size_t n = 2;
    if (g_archive_encrypted) {
        if (!g_password[0] && !ask_text(g_window, L"Archive protégée", L"Mot de passe de l’archive :", g_password, 512, TRUE)) return FALSE;
        swprintf(pass_switch, 520, L"-p%ls", g_password); args[n++] = pass_switch;
    }
    args[n++] = g_archive;
    wchar_t *output = NULL; DWORD code = 1;
    BOOL ok = run_7zip(NULL, args, n, &output, &code) && code == 0;
    MessageBoxW(g_window, ok ? L"Tous les fichiers de l’archive sont intacts." : (output ? output : L"Échec de la vérification."), APP_TITLE, ok ? MB_ICONINFORMATION | MB_OK : MB_ICONERROR | MB_OK);
    free(output); return ok;
}

static BOOL add_one(const wchar_t *file, const wchar_t *archive, const wchar_t *password, BOOL create) {
    wchar_t working[MAX_PATH * 4], base[MAX_PATH * 4], type[16], pass_switch[520];
    wcsncpy(working, file, MAX_PATH * 4 - 1); working[MAX_PATH * 4 - 1] = L'\0';
    wchar_t *slash = wcsrchr(working, L'\\'); if (!slash) slash = wcsrchr(working, L'/');
    wchar_t *name = working;
    if (slash) { *slash = L'\0'; name = slash + 1; }
    wcsncpy(base, archive, MAX_PATH * 4 - 1); base[MAX_PATH * 4 - 1] = L'\0';
    const wchar_t *dot = wcsrchr(archive, L'.');
    if (dot && !_wcsicmp(dot, L".7z")) wcscpy(type, L"-t7z");
    else if (dot && !_wcsicmp(dot, L".tar")) wcscpy(type, L"-ttar");
    else if (dot && !_wcsicmp(dot, L".wim")) wcscpy(type, L"-twim");
    else wcscpy(type, L"-tzip");
    const wchar_t *args[12]; size_t n = 0;
    args[n++] = L"a"; args[n++] = type; args[n++] = base; args[n++] = L"-sccUTF-8";
    if (password && password[0]) {
        swprintf(pass_switch, 520, L"-p%ls", password); args[n++] = pass_switch;
        if (!_wcsicmp(type, L"-t7z")) args[n++] = L"-mhe=on";
        else if (!_wcsicmp(type, L"-tzip")) args[n++] = L"-mem=AES256";
    }
    args[n++] = L"-y"; args[n++] = L"--"; args[n++] = name;
    wchar_t *output = NULL; DWORD code = 1;
    BOOL ok = run_7zip(slash ? working : NULL, args, n, &output, &code) && code == 0;
    if (!ok && output) MessageBoxW(g_window, output, APP_TITLE, MB_ICONERROR | MB_OK);
    free(output); (void)create; return ok;
}

static BOOL invoke_add(void) {
    if (!g_archive[0]) { MessageBoxW(g_window, L"Ouvrez ou créez une archive d’abord.", APP_TITLE, MB_ICONINFORMATION | MB_OK); return FALSE; }
    const wchar_t *ext = wcsrchr(g_archive, L'.');
    if (!ext || (_wcsicmp(ext, L".zip") && _wcsicmp(ext, L".7z") && _wcsicmp(ext, L".tar") && _wcsicmp(ext, L".wim"))) {
        MessageBoxW(g_window, L"Ce format est en lecture seule. L’ajout est disponible pour ZIP, 7Z, TAR et WIM.", APP_TITLE, MB_ICONINFORMATION | MB_OK); return FALSE;
    }
    if (g_archive_encrypted && !g_password[0] && !ask_text(g_window, L"Archive protégée", L"Mot de passe de l’archive :", g_password, 512, TRUE)) return FALSE;
    wchar_t files[32768]; if (!choose_files(files, 32768)) return FALSE;
    wchar_t *cursor = files; wchar_t directory[MAX_PATH * 4]; wcsncpy(directory, cursor, MAX_PATH * 4 - 1); directory[MAX_PATH * 4 - 1] = L'\0';
    cursor += wcslen(cursor) + 1;
    BOOL multi = *cursor != L'\0', ok = TRUE;
    if (!multi) ok = add_one(directory, g_archive, g_password, FALSE);
    else while (*cursor) {
        wchar_t full[MAX_PATH * 4]; swprintf(full, MAX_PATH * 4, L"%ls\\%ls", directory, cursor);
        if (!add_one(full, g_archive, g_password, FALSE)) { ok = FALSE; break; }
        cursor += wcslen(cursor) + 1;
    }
    if (ok) list_archive();
    return ok;
}

static BOOL invoke_delete(void) {
    const wchar_t *ext = wcsrchr(g_archive, L'.');
    if (!ext || (_wcsicmp(ext, L".zip") && _wcsicmp(ext, L".7z") && _wcsicmp(ext, L".tar") && _wcsicmp(ext, L".wim"))) {
        MessageBoxW(g_window, L"Ce format est en lecture seule. La suppression est disponible pour ZIP, 7Z, TAR et WIM.", APP_TITLE, MB_ICONINFORMATION | MB_OK); return FALSE;
    }
    int rows[2048]; size_t count = selected_indices(rows, 2048);
    if (!count) { MessageBoxW(g_window, L"Sélectionnez les éléments à supprimer.", APP_TITLE, MB_ICONINFORMATION | MB_OK); return FALSE; }
    if (MessageBoxW(g_window, L"Supprimer la sélection de l’archive ?", L"Confirmer", MB_YESNO | MB_ICONWARNING) != IDYES) return FALSE;
    wchar_t pass_switch[520]; const wchar_t *args[2055]; size_t n = 0;
    args[n++] = L"d"; args[n++] = L"-sccUTF-8"; args[n++] = g_archive;
    if (g_archive_encrypted) {
        if (!g_password[0] && !ask_text(g_window, L"Archive protégée", L"Mot de passe de l’archive :", g_password, 512, TRUE)) return FALSE;
        swprintf(pass_switch, 520, L"-p%ls", g_password); args[n++] = pass_switch;
    }
    args[n++] = L"--";
    for (size_t i = 0; i < count; ++i) args[n++] = g_entries[rows[i]].name;
    wchar_t *output = NULL; DWORD code = 1;
    BOOL ok = run_7zip(NULL, args, n, &output, &code) && code == 0;
    if (!ok) MessageBoxW(g_window, output ? output : L"Échec de la suppression.", APP_TITLE, MB_ICONERROR | MB_OK);
    free(output); if (ok) list_archive(); return ok;
}

static BOOL invoke_new(void) {
    wchar_t archive[MAX_PATH * 4]; if (!choose_archive(TRUE, archive, MAX_PATH * 4)) return FALSE;
    wchar_t files[32768]; if (!choose_files(files, 32768)) return FALSE;
    BOOL encrypted = MessageBoxW(g_window, L"Protéger la nouvelle archive par mot de passe ?", L"Chiffrement", MB_YESNO | MB_ICONQUESTION) == IDYES;
    wchar_t pass[512] = L"", confirm[512] = L"";
    if (encrypted) {
        if (!ask_text(g_window, L"Nouveau mot de passe", L"Choisissez un mot de passe :", pass, 512, TRUE) ||
            !ask_text(g_window, L"Confirmer le mot de passe", L"Saisissez-le une seconde fois :", confirm, 512, TRUE)) return FALSE;
        if (wcscmp(pass, confirm)) { MessageBoxW(g_window, L"Les mots de passe ne correspondent pas.", APP_TITLE, MB_ICONERROR | MB_OK); return FALSE; }
    }
    wchar_t *cursor = files; wchar_t directory[MAX_PATH * 4]; wcsncpy(directory, cursor, MAX_PATH * 4 - 1); directory[MAX_PATH * 4 - 1] = L'\0';
    cursor += wcslen(cursor) + 1;
    BOOL multi = *cursor != L'\0', ok = TRUE;
    if (!multi) ok = add_one(directory, archive, encrypted ? pass : L"", TRUE);
    else while (*cursor) {
        wchar_t full[MAX_PATH * 4]; swprintf(full, MAX_PATH * 4, L"%ls\\%ls", directory, cursor);
        if (!add_one(full, archive, encrypted ? pass : L"", TRUE)) { ok = FALSE; break; }
        cursor += wcslen(cursor) + 1;
    }
    if (ok) { wcsncpy(g_archive, archive, MAX_PATH * 4 - 1); wcsncpy(g_password, pass, 511); g_header_encrypted = encrypted && !_wcsicmp(wcsrchr(archive, L'.') ? wcsrchr(archive, L'.') : L"", L".7z"); list_archive(); }
    return ok;
}

static BOOL invoke_open(void) {
    wchar_t archive[MAX_PATH * 4]; if (!choose_archive(FALSE, archive, MAX_PATH * 4)) return FALSE;
    wcsncpy(g_archive, archive, MAX_PATH * 4 - 1); g_archive[MAX_PATH * 4 - 1] = L'\0';
    g_password[0] = L'\0'; g_header_encrypted = g_archive_encrypted = FALSE;
    return list_archive();
}

static void draw_button(const DRAWITEMSTRUCT *draw) {
    HDC dc = draw->hDC; RECT r = draw->rcItem;
    BOOL accent = draw->CtlID == ID_EXTRACT;
    COLORREF color = accent ? RGB(80, 101, 228) : RGB(246, 249, 255);
    if (draw->itemState & ODS_SELECTED) color = accent ? RGB(63, 81, 200) : RGB(229, 237, 253);
    HBRUSH brush = CreateSolidBrush(color); HPEN pen = CreatePen(PS_SOLID, 1, accent ? RGB(121, 139, 255) : RGB(255, 255, 255));
    HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 18, 18);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, accent ? RGB(255, 255, 255) : RGB(39, 56, 86));
    HFONT old_font = (HFONT)SelectObject(dc, g_font);
    wchar_t label[80]; GetWindowTextW(draw->hwndItem, label, 80);
    DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font); SelectObject(dc, old_brush); SelectObject(dc, old_pen); DeleteObject(brush); DeleteObject(pen);
}

static void layout_controls(HWND hwnd) {
    RECT r; GetClientRect(hwnd, &r); int width = r.right, height = r.bottom;
    MoveWindow(g_archive_label, width - 460, 28, 430, 30, TRUE);
    MoveWindow(g_list, 24, 180, width - 48, height - 232, TRUE);
    HWND b;
    int x = 24;
    const int widths[] = {112, 112, 105, 116, 158, 92};
    const int ids[] = {ID_ADD, ID_EXTRACT, ID_TEST, ID_DELETE, ID_NEW, ID_OPEN};
    for (int i = 0; i < 6; ++i) { b = GetDlgItem(hwnd, ids[i]); MoveWindow(b, x, 112, widths[i], 42, TRUE); x += widths[i] + 9; }
}

static LRESULT CALLBACK prompt_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    if (message == WM_CREATE) {
        PromptState *state = g_prompt;
        HWND label = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 18, 16, 340, 36, hwnd, NULL, g_instance, NULL);
        (void)label;
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (state->password ? ES_PASSWORD : 0), 18, 58, 340, 28, hwnd, (HMENU)1, g_instance, NULL);
        CreateWindowW(L"BUTTON", L"Annuler", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 176, 103, 84, 30, hwnd, (HMENU)(INT_PTR)IDCANCEL, g_instance, NULL);
        CreateWindowW(L"BUTTON", L"Valider", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 268, 103, 90, 30, hwnd, (HMENU)(INT_PTR)IDOK, g_instance, NULL);
        SetFocus(state->edit); return 0;
    }
    if (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
        PromptState *state = g_prompt;
        if (LOWORD(wparam) == IDOK) { GetWindowTextW(state->edit, state->output, (int)state->capacity); state->accepted = TRUE; }
        state->done = TRUE; DestroyWindow(hwnd); return 0;
    }
    if (message == WM_CLOSE) { g_prompt->done = TRUE; DestroyWindow(hwnd); return 0; }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static BOOL ask_text(HWND parent, const wchar_t *title, const wchar_t *label_text, wchar_t *output, size_t capacity, BOOL password) {
    WNDCLASSW cls; ZeroMemory(&cls, sizeof(cls)); cls.lpfnWndProc = prompt_proc; cls.hInstance = g_instance; cls.lpszClassName = L"CzipPromptWindow"; cls.hCursor = LoadCursor(NULL, IDC_ARROW); cls.hbrBackground = CreateSolidBrush(RGB(244, 247, 255));
    RegisterClassW(&cls);
    wchar_t title_copy[160]; wcsncpy(title_copy, title, 159); title_copy[159] = L'\0';
    PromptState state; ZeroMemory(&state, sizeof(state)); state.output = output; state.capacity = capacity; state.password = password; g_prompt = &state;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"CzipPromptWindow", title_copy, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 390, 180, parent, NULL, g_instance, NULL);
    if (!dialog) return FALSE;
    HWND label = FindWindowExW(dialog, NULL, L"STATIC", NULL);
    if (label) SetWindowTextW(label, label_text);
    RECT pr; GetWindowRect(parent, &pr); SetWindowPos(dialog, HWND_TOP, pr.left + (pr.right-pr.left-390)/2, pr.top + (pr.bottom-pr.top-180)/2, 390, 180, SWP_SHOWWINDOW);
    EnableWindow(parent, FALSE); ShowWindow(dialog, SW_SHOW); UpdateWindow(dialog);
    MSG msg;
    while (!state.done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(parent, TRUE); SetForegroundWindow(parent); g_prompt = NULL;
    return state.accepted;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_title_font = CreateFontW(-27, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_archive_label = CreateWindowW(L"STATIC", L"Aucune archive ouverte", WS_CHILD | WS_VISIBLE | SS_RIGHT, 0, 0, 400, 30, hwnd, NULL, g_instance, NULL);
        const wchar_t *labels[] = {L"+  Ajouter", L">  Extraire", L"✓  Vérifier", L"-  Supprimer", L"+  Nouvelle archive", L"Ouvrir"};
        const int ids[] = {ID_ADD, ID_EXTRACT, ID_TEST, ID_DELETE, ID_NEW, ID_OPEN};
        for (int i = 0; i < 6; ++i) CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ids[i], g_instance, NULL);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                 24, 180, 900, 450, hwnd, (HMENU)ID_LIST, g_instance, NULL);
        SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(g_list, RGB(248, 250, 255));
        ListView_SetTextBkColor(g_list, RGB(248, 250, 255));
        ListView_SetTextColor(g_list, RGB(39, 56, 86));
        const wchar_t *headers[] = {L"Nom", L"Taille", L"Compressé", L"Type", L"Modifié le"};
        const int widths[] = {430, 120, 125, 120, 170};
        for (int i = 0; i < 5; ++i) { LVCOLUMNW col; ZeroMemory(&col, sizeof(col)); col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM; col.pszText = (LPWSTR)headers[i]; col.cx = widths[i]; col.iSubItem = i; ListView_InsertColumn(g_list, i, &col); }
        layout_controls(hwnd); return 0;
    }
    case WM_SIZE: layout_controls(hwnd); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *limits = (MINMAXINFO *)lparam;
        limits->ptMinTrackSize.x = 900;
        limits->ptMinTrackSize.y = 560;
        return 0;
    }
    case WM_DRAWITEM: draw_button((const DRAWITEMSTRUCT *)lparam); return TRUE;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wparam; SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(37, 53, 82));
        HWND control = (HWND)lparam;
        if (control == g_archive_label) SetTextColor(dc, RGB(65, 84, 123));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); RECT r; GetClientRect(hwnd, &r);
        TRIVERTEX vertices[2] = {{0, 0, 0xDD00, 0xEE00, 0xFF00, 0xFFFF}, {r.right, r.bottom, 0xF100, 0xE800, 0xFF00, 0xFFFF}};
        GRADIENT_RECT grad = {0, 1}; GradientFill(dc, vertices, 2, &grad, 1, GRADIENT_FILL_RECT_V);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(36, 169, 94)); SelectObject(dc, g_title_font); TextOutW(dc, 27, 17, L"C", 1);
        SelectObject(dc, g_font); SetTextColor(dc, RGB(27, 43, 73)); TextOutW(dc, 70, 22, L"Czip", 4);
        const wchar_t *subtitle = L"ARCHIVE STUDIO  ·  ZIP / 7Z / TAR / WIM";
        SetTextColor(dc, RGB(115, 132, 160)); TextOutW(dc, 72, 53, subtitle, lstrlenW(subtitle));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(45, 169, 96)); HGDIOBJ old = SelectObject(dc, pen); HBRUSH brush = CreateSolidBrush(RGB(221, 247, 230)); HGDIOBJ oldb = SelectObject(dc, brush);
        RoundRect(dc, 114, 28, 140, 57, 8, 8); MoveToEx(dc, 119, 35, NULL); LineTo(dc, 135, 35); MoveToEx(dc, 119, 42, NULL); LineTo(dc, 135, 42); MoveToEx(dc, 119, 49, NULL); LineTo(dc, 135, 49);
        SelectObject(dc, old); SelectObject(dc, oldb); DeleteObject(pen); DeleteObject(brush);
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_OPEN: invoke_open(); return 0;
        case ID_NEW: invoke_new(); return 0;
        case ID_ADD: invoke_add(); return 0;
        case ID_EXTRACT: invoke_extract(); return 0;
        case ID_TEST: invoke_test(); return 0;
        case ID_DELETE: invoke_delete(); return 0;
        }
        break;
    case WM_NOTIFY:
        if (((LPNMHDR)lparam)->idFrom == ID_LIST && ((LPNMHDR)lparam)->code == NM_DBLCLK) invoke_extract();
        break;
    case WM_DESTROY:
        if (g_font) DeleteObject(g_font); if (g_title_font) DeleteObject(g_title_font); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous; (void)command_line;
    g_instance = instance;
    INITCOMMONCONTROLSEX controls = {sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc)); wc.lpfnWndProc = window_proc; wc.hInstance = instance; wc.lpszClassName = L"CzipNativeWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.style = CS_HREDRAW | CS_VREDRAW;
    if (!RegisterClassW(&wc)) return 1;
    g_window = CreateWindowExW(0, wc.lpszClassName, APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1120, 740,
                               NULL, NULL, instance, NULL);
    if (!g_window) return 1;
    ShowWindow(g_window, show); UpdateWindow(g_window);
    MSG msg; while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return (int)msg.wParam;
}
