// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// From a VS developer command prompt at the repository root:
// set FUZZILLI_REPO=%CD%
// pushd %TEMP%
// cl /nologo /std:c11 /W4 /WX /utf-8 /I "%FUZZILLI_REPO%\Sources\libreprl\include" "%FUZZILLI_REPO%\Sources\libreprl\libreprl-windows.c" "%FUZZILLI_REPO%\Tests\WindowsREPRLSmoke.c" /Fe:WindowsREPRLSmoke.exe
// WindowsREPRLSmoke.exe
// popd

#if defined(_WIN32)

#include "libreprl.h"

#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>

static int read_exact(int descriptor, void* output, unsigned int count)
{
    unsigned int done = 0;
    while (done < count) {
        int amount = _read(descriptor, (char*)output + done, count - done);
        if (amount <= 0) return 0;
        done += (unsigned int)amount;
    }
    return 1;
}

static int write_exact(int descriptor, const void* input, unsigned int count)
{
    unsigned int done = 0;
    while (done < count) {
        int amount = _write(descriptor, (const char*)input + done, count - done);
        if (amount <= 0) return 0;
        done += (unsigned int)amount;
    }
    return 1;
}

static int descriptor_from_environment(const char* name)
{
    char value[32];
    DWORD size = GetEnvironmentVariableA(name, value, sizeof(value));
    if (!size || size >= sizeof(value)) return -1;
    char* end = NULL;
    unsigned long long handle = strtoull(value, &end, 10);
    if (!end || *end || handle == 0) return -1;
    return _open_osfhandle((intptr_t)handle, _O_BINARY);
}

static int spawn_inheriting_descendant(void)
{
    WCHAR executable[MAX_PATH];
    WCHAR command[MAX_PATH + 32];
    if (!GetModuleFileNameW(NULL, executable, MAX_PATH)
            || swprintf_s(command, sizeof(command) / sizeof(*command),
                          L"\"%ls\" --grandchild", executable) < 0) return 0;
    STARTUPINFOW startup = { 0 };
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = { 0 };
    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process)) return 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 1;
}

static int run_child(int argc, char** argv)
{
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    const char expected_argument[] = "spaces \"quotes\" and trailing\\\\";
    if (argc != 3 || strcmp(argv[2], expected_argument) != 0) return 10;
    WCHAR environment[16];
    if (GetEnvironmentVariableW(L"FUZZILLI_TEST_UNICODE", environment, 16) != 1
            || environment[0] != L'\u2603') return 11;

    int control_in = descriptor_from_environment("FUZZILLI_REPRL_CRFD");
    int control_out = descriptor_from_environment("FUZZILLI_REPRL_CWFD");
    int data_in = descriptor_from_environment("FUZZILLI_REPRL_DRFD");
    int data_out = descriptor_from_environment("FUZZILLI_REPRL_DWFD");
    if (control_in < 0 || control_out < 0 || data_in < 0 || data_out < 0) return 12;

    char helo[4];
    if (!write_exact(control_out, "HELO", 4) || !read_exact(control_in, helo, 4)
            || memcmp(helo, "HELO", 4) != 0) return 13;

    for (;;) {
        char command[4];
        uint64_t length;
        if (!read_exact(control_in, command, 4)) return 0;
        if (memcmp(command, "exec", 4) != 0 || !read_exact(control_in, &length, 8)
                || length > 31) return 14;
        if (_lseeki64(data_in, 0, SEEK_SET) != 0) return 15;
        char script[32];
        if (!read_exact(data_in, script, (unsigned int)length)) return 16;
        script[length] = '\0';

        if (strcmp(script, "sleep") == 0) {
            Sleep(250);
            continue;
        }
        if (strcmp(script, "die") == 0)
            RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, NULL);
        if (strcmp(script, "exit") == 0) ExitProcess(7);
        if (strcmp(script, "inherit") == 0) {
            SetHandleInformation((HANDLE)_get_osfhandle(control_out), HANDLE_FLAG_INHERIT,
                                 HANDLE_FLAG_INHERIT);
            if (!spawn_inheriting_descendant()) return 32;
            ExitProcess(7);
        }
        if (strcmp(script, "one") != 0 && strcmp(script, "two") != 0) return 17;

        char fuzzout[64];
        int fuzzout_length = sprintf_s(fuzzout, sizeof(fuzzout), "fuzz:%s", script);
        if (fuzzout_length < 0 || !write_exact(data_out, fuzzout, (unsigned int)fuzzout_length))
            return 18;
        printf("stdout:%s", script);
        fprintf(stderr, "stderr:%s", script);
        fflush(stdout);
        fflush(stderr);
        int32_t status = strcmp(script, "two") == 0 ? 2 << 8 : 0;
        if (!write_exact(control_out, &status, sizeof(status))) return 19;
    }
}

static int expect_output(struct reprl_context* context, const char* script)
{
    char expected[64];
    sprintf_s(expected, sizeof(expected), "fuzz:%s", script);
    if (strcmp(reprl_fetch_fuzzout(context), expected) != 0) return 0;
    sprintf_s(expected, sizeof(expected), "stdout:%s", script);
    if (strcmp(reprl_fetch_stdout(context), expected) != 0) return 0;
    sprintf_s(expected, sizeof(expected), "stderr:%s", script);
    return strcmp(reprl_fetch_stderr(context), expected) == 0;
}

static int run_parent(void)
{
    WCHAR executable_wide[MAX_PATH];
    char executable_utf8[MAX_PATH * 4];
    if (!GetModuleFileNameW(NULL, executable_wide, MAX_PATH)
            || !WideCharToMultiByte(CP_UTF8, 0, executable_wide, -1,
                                    executable_utf8, sizeof(executable_utf8), NULL, NULL)) return 20;

    const char* argv[] = { executable_utf8, "--child", "spaces \"quotes\" and trailing\\\\", NULL };
    const char* envp[] = { "FUZZILLI_TEST_UNICODE=\xE2\x98\x83", NULL };
    struct reprl_context* context = reprl_create_context();
    if (!context) return 21;
    int result = 0;
    if (reprl_initialize_context(context, argv, envp, 1, 1) != 0) {
        fprintf(stderr, "initialize: %s\n", reprl_get_last_error(context));
        result = 22;
        goto done;
    }

    uint64_t duration = 0;
    int status = reprl_execute(context, "one", 3, 1000000, &duration, 0);
    if (status != 0 || !expect_output(context, "one")) { result = 23; goto done; }
    status = reprl_execute(context, "two", 3, 1000000, &duration, 0);
    if (status != 2 << 8 || !expect_output(context, "two")) { result = 24; goto done; }
    status = reprl_execute(context, "sleep", 5, 10000, &duration, 0);
    if (status != 1 << 16) { result = 25; goto done; }
    status = reprl_execute(context, "one", 3, 1000000, &duration, 0);
    if (status != 0 || !expect_output(context, "one")) { result = 26; goto done; }
    status = reprl_execute(context, "die", 3, 1000000, &duration, 0);
    if (!RIFSIGNALED(status) || RTERMSIG(status) != 11) { result = 27; goto done; }
    status = reprl_execute(context, "two", 3, 1000000, &duration, 1);
    if (status != 2 << 8 || !expect_output(context, "two")) { result = 28; goto done; }
    status = reprl_execute(context, "one", 3, 1000000, &duration, 1);
    if (status != 0 || !expect_output(context, "one")) { result = 29; goto done; }
    status = reprl_execute(context, "exit", 4, 1000000, &duration, 0);
    if (!RIFEXITED(status) || REXITSTATUS(status) != 7) { result = 30; goto done; }
    status = reprl_execute(context, "two", 3, 1000000, &duration, 0);
    if (status != 2 << 8 || !expect_output(context, "two")) { result = 31; goto done; }
    ULONGLONG started = GetTickCount64();
    status = reprl_execute(context, "inherit", 7, 100000, &duration, 0);
    ULONGLONG elapsed = GetTickCount64() - started;
    // The descendant keeps the control writer open for two seconds. The call
    // must still report the child's exit promptly and must not hang in cleanup.
    if (status != 7 << 8 || elapsed >= 1000) { result = 33; goto done; }
    status = reprl_execute(context, "one", 3, 1000000, &duration, 0);
    if (status != 0 || !expect_output(context, "one")) { result = 34; goto done; }

done:
    if (result && result != 22) {
        const char* error = reprl_get_last_error(context);
        fprintf(stderr, "Windows REPRL smoke failed at step %d (last error: %s)\n",
                result, error ? error : "none");
    }
    reprl_destroy_context(context);
    return result;
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--grandchild") == 0) {
        Sleep(2000);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--child") == 0) return run_child(argc, argv);
    int result = run_parent();
    if (!result) puts("Windows REPRL smoke passed");
    return result;
}

#endif
