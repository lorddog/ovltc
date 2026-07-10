/**
 * test_harness.c — Development test tool for ovlTC.
 *
 * Part of the ovlTC project (see ovl_wcx.c for license/copyright).
 *
 * Loads ovl_wcx.wcx64 dynamically (the same way Total Commander does) and
 * exercises OpenArchiveW / ReadHeaderExW / ProcessFileW / CloseArchive
 * against a real .ovl file. For manual testing only, not part of the plugin.
 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdint.h>
#include "wcxhead.h"

typedef HANDLE (__stdcall *OpenArchiveW_t)(tOpenArchiveDataW *);
typedef int (__stdcall *ReadHeaderExW_t)(HANDLE, tHeaderDataExW *);
typedef int (__stdcall *ProcessFileW_t)(HANDLE, int, wchar_t *, wchar_t *);
typedef int (__stdcall *CloseArchive_t)(HANDLE);
typedef void (__stdcall *SetProcessDataProcW_t)(HANDLE, tProcessDataProcW);

static int __stdcall progress_cb(wchar_t *fname, int size) {
    (void)fname; (void)size;
    return 1;
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) {
        wprintf(L"Usage: test_harness.exe <plugin.wcx64> <file.ovl> [extract_dir]\n");
        return 1;
    }

    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) {
        wprintf(L"LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    OpenArchiveW_t OpenArchiveW_ = (OpenArchiveW_t)GetProcAddress(dll, "OpenArchiveW");
    ReadHeaderExW_t ReadHeaderExW_ = (ReadHeaderExW_t)GetProcAddress(dll, "ReadHeaderExW");
    ProcessFileW_t ProcessFileW_ = (ProcessFileW_t)GetProcAddress(dll, "ProcessFileW");
    CloseArchive_t CloseArchive_ = (CloseArchive_t)GetProcAddress(dll, "CloseArchive");
    SetProcessDataProcW_t SetProcessDataProcW_ = (SetProcessDataProcW_t)GetProcAddress(dll, "SetProcessDataProcW");

    if (!OpenArchiveW_ || !ReadHeaderExW_ || !ProcessFileW_ || !CloseArchive_) {
        wprintf(L"Missing exports in plugin!\n");
        return 1;
    }

    tOpenArchiveDataW oad;
    memset(&oad, 0, sizeof(oad));
    oad.ArcName = argv[2];
    oad.OpenMode = PK_OM_LIST;

    HANDLE h = OpenArchiveW_(&oad);
    if (!h) {
        wprintf(L"OpenArchiveW failed, OpenResult=%d\n", oad.OpenResult);
        return 1;
    }
    wprintf(L"OpenArchiveW OK.\n");

    if (SetProcessDataProcW_) SetProcessDataProcW_(h, progress_cb);

    int count = 0;
    tHeaderDataExW hd;
    while (ReadHeaderExW_(h, &hd) == 0) {
        uint64_t size = ((uint64_t)hd.UnpSizeHigh << 32) | hd.UnpSize;
        wprintf(L"  [%3d] %-60s  %10llu bytes\n", count, hd.FileName, (unsigned long long)size);
        count++;

        if (argc >= 4) {
            int r = ProcessFileW_(h, PK_EXTRACT, argv[3], hd.FileName);
            if (r != 0) wprintf(L"        -> extraction failed, code=%d\n", r);
        } else {
            ProcessFileW_(h, PK_SKIP, NULL, NULL);
        }
    }
    wprintf(L"\nTotal: %d entries.\n", count);

    CloseArchive_(h);
    FreeLibrary(dll);
    return 0;
}
