// Copyright 2021 Google LLC
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

#if defined(_WIN32)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "libreprl.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

// The Windows child receives inheritable HANDLE values through these variables.
// It can turn them into binary CRT file descriptors with _open_osfhandle.
#define REPRL_CHILD_CTRL_IN_ENV  L"FUZZILLI_REPRL_CRFD"
#define REPRL_CHILD_CTRL_OUT_ENV L"FUZZILLI_REPRL_CWFD"
#define REPRL_CHILD_DATA_IN_ENV  L"FUZZILLI_REPRL_DRFD"
#define REPRL_CHILD_DATA_OUT_ENV L"FUZZILLI_REPRL_DWFD"

#define REPRL_HANDSHAKE_TIMEOUT_US 30000000ULL
#define REPRL_MAX_TIMEOUT_IN_MICROSECONDS ((uint64_t)INT32_MAX * 1000)
#define REPRL_COMMAND_LINE_CAPACITY 32768

struct data_channel {
    // Both processes use file I/O. Their shared file position gives the
    // output length after the child writes to this handle.
    HANDLE file;
    char* content;
    size_t content_capacity;
};

struct reprl_context {
    bool initialized;
    HANDLE control_read;
    HANDLE control_write;
    HANDLE read_event;
    struct data_channel* data_in;     // Child -> parent (fuzzout).
    struct data_channel* data_out;    // Parent -> child (script).
    struct data_channel* child_stdout;
    struct data_channel* child_stderr;
    HANDLE child;
    char** argv;
    char** envp;
    char* last_error;
};

static void close_handle(HANDLE* handle)
{
    if (*handle && *handle != INVALID_HANDLE_VALUE)
        CloseHandle(*handle);
    *handle = NULL;
}

static uint64_t current_usecs(void)
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    // Divide first so the multiplication cannot overflow after a long uptime.
    return (uint64_t)(counter.QuadPart / frequency.QuadPart) * 1000000ULL
         + (uint64_t)(counter.QuadPart % frequency.QuadPart) * 1000000ULL / (uint64_t)frequency.QuadPart;
}

static int reprl_error(struct reprl_context* ctx, const char* format, ...)
{
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    int length = _vscprintf(format, copy);
    va_end(copy);
    char* message = length >= 0 ? malloc((size_t)length + 1) : NULL;
    if (message)
        vsprintf_s(message, (size_t)length + 1, format, args);
    va_end(args);
    free(ctx->last_error);
    ctx->last_error = message;
    return -1;
}

static char** copy_string_array(const char** original)
{
    size_t count = 0;
    if (original) {
        while (original[count]) count++;
    }
    char** copy = calloc(count + 1, sizeof(*copy));
    if (!copy) return NULL;
    for (size_t i = 0; i < count; i++) {
        copy[i] = _strdup(original[i]);
        if (!copy[i]) {
            for (size_t j = 0; j < i; j++) free(copy[j]);
            free(copy);
            return NULL;
        }
    }
    return copy;
}

static void free_string_array(char** array)
{
    if (!array) return;
    for (char** entry = array; *entry; entry++) free(*entry);
    free(array);
}

static struct data_channel* reprl_create_data_channel(struct reprl_context* ctx)
{
    WCHAR directory[MAX_PATH + 1], path[MAX_PATH + 1];
    DWORD length = GetTempPathW(MAX_PATH + 1, directory);
    if (!length || length > MAX_PATH || !GetTempFileNameW(directory, L"rpl", 0, path)) {
        reprl_error(ctx, "Failed to create a REPRL data file: Windows error %lu", GetLastError());
        return NULL;
    }

    SECURITY_ATTRIBUTES security = { sizeof(security), NULL, TRUE };
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              &security, OPEN_EXISTING,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        DeleteFileW(path);
        reprl_error(ctx, "Failed to open a REPRL data file: Windows error %lu", error);
        return NULL;
    }

    struct data_channel* channel = malloc(sizeof(*channel));
    if (!channel) {
        CloseHandle(file);
        reprl_error(ctx, "Out of memory creating a REPRL data channel");
        return NULL;
    }
    channel->file = file;
    channel->content = NULL;
    channel->content_capacity = 0;
    return channel;
}

static void reprl_destroy_data_channel(struct data_channel* channel)
{
    if (!channel) return;
    CloseHandle(channel->file);
    free(channel->content);
    free(channel);
}

static void reprl_child_terminated(struct reprl_context* ctx)
{
    close_handle(&ctx->read_event);
    close_handle(&ctx->child);
    close_handle(&ctx->control_read);
    close_handle(&ctx->control_write);
}

static void reprl_terminate_child(struct reprl_context* ctx)
{
    if (ctx->child) {
        if (WaitForSingleObject(ctx->child, 0) == WAIT_TIMEOUT)
            TerminateProcess(ctx->child, 9);
        WaitForSingleObject(ctx->child, INFINITE);
    }
    reprl_child_terminated(ctx);
}

static WCHAR* utf8_to_wide(const char* value)
{
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (!length) return NULL;
    WCHAR* result = malloc((size_t)length * sizeof(*result));
    if (!result) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result, length)) {
        free(result);
        return NULL;
    }
    return result;
}

static bool append_command_char(WCHAR* command, size_t* length, WCHAR value)
{
    if (*length >= REPRL_COMMAND_LINE_CAPACITY - 1) return false;
    command[(*length)++] = value;
    command[*length] = L'\0';
    return true;
}

// Quote an argument using the Windows CRT command-line parsing rules.
static bool append_quoted_argument(WCHAR* command, size_t* length, const WCHAR* argument)
{
    if (!append_command_char(command, length, L'"')) return false;
    while (*argument) {
        size_t slashes = 0;
        while (argument[slashes] == L'\\') slashes++;
        argument += slashes;
        size_t count = slashes;
        if (*argument == L'"' || !*argument) count *= 2;
        while (count--) {
            if (!append_command_char(command, length, L'\\')) return false;
        }
        if (*argument) {
            if (*argument == L'"' && !append_command_char(command, length, L'\\')) return false;
            if (!append_command_char(command, length, *argument++)) return false;
        }
    }
    return append_command_char(command, length, L'"');
}

static WCHAR* build_command_line(char** argv)
{
    WCHAR* command = calloc(REPRL_COMMAND_LINE_CAPACITY, sizeof(*command));
    if (!command) return NULL;
    size_t length = 0;
    for (char** argument = argv; *argument; argument++) {
        WCHAR* wide = utf8_to_wide(*argument);
        if (!wide || (length && !append_command_char(command, &length, L' '))
                || !append_quoted_argument(command, &length, wide)) {
            free(wide);
            free(command);
            return NULL;
        }
        free(wide);
    }
    return command;
}

struct environment_entries {
    WCHAR** values;
    size_t count;
    size_t capacity;
};

static void free_environment_entries(struct environment_entries* entries)
{
    for (size_t i = 0; i < entries->count; i++) free(entries->values[i]);
    free(entries->values);
}

static const WCHAR* environment_name_end(const WCHAR* value)
{
    // Windows has special inherited entries such as =C:=C:\path.
    return wcschr(value + (value[0] == L'='), L'=');
}

static bool set_environment_entry(struct environment_entries* entries, const WCHAR* value)
{
    const WCHAR* end = environment_name_end(value);
    if (!end || end == value) return false;
    size_t name_length = (size_t)(end - value);
    size_t value_length = wcslen(value) + 1;
    WCHAR* copy = malloc(value_length * sizeof(*copy));
    if (!copy) return false;
    memcpy(copy, value, value_length * sizeof(*copy));

    for (size_t i = 0; i < entries->count; i++) {
        const WCHAR* old_end = environment_name_end(entries->values[i]);
        if (old_end && (size_t)(old_end - entries->values[i]) == name_length
                && _wcsnicmp(entries->values[i], value, name_length) == 0) {
            free(entries->values[i]);
            entries->values[i] = copy;
            return true;
        }
    }
    if (entries->count == entries->capacity) {
        size_t capacity = entries->capacity ? entries->capacity * 2 : 32;
        WCHAR** values = realloc(entries->values, capacity * sizeof(*values));
        if (!values) {
            free(copy);
            return false;
        }
        entries->values = values;
        entries->capacity = capacity;
    }
    entries->values[entries->count++] = copy;
    return true;
}

static int compare_environment_entries(const void* left, const void* right)
{
    const WCHAR* a = *(const WCHAR* const*)left;
    const WCHAR* b = *(const WCHAR* const*)right;
    size_t a_length = (size_t)(environment_name_end(a) - a);
    size_t b_length = (size_t)(environment_name_end(b) - b);
    size_t common = a_length < b_length ? a_length : b_length;
    int order = _wcsnicmp(a, b, common);
    if (order) return order;
    return a_length < b_length ? -1 : (a_length > b_length ? 1 : 0);
}

static WCHAR* build_environment(char** envp, HANDLE control_in, HANDLE control_out,
                                 HANDLE data_in, HANDLE data_out)
{
    struct environment_entries entries = { 0 };
    WCHAR* result = NULL;
    LPWCH inherited = GetEnvironmentStringsW();
    if (!inherited) return NULL;
    bool ok = true;
    for (const WCHAR* entry = inherited; *entry; entry += wcslen(entry) + 1) {
        if (!set_environment_entry(&entries, entry)) { ok = false; break; }
    }
    FreeEnvironmentStringsW(inherited);

    if (ok) {
        for (char** entry = envp; *entry; entry++) {
            WCHAR* wide = utf8_to_wide(*entry);
            if (!wide || !set_environment_entry(&entries, wide)) ok = false;
            free(wide);
            if (!ok) break;
        }
    }

    const WCHAR* names[] = {
        REPRL_CHILD_CTRL_IN_ENV, REPRL_CHILD_CTRL_OUT_ENV,
        REPRL_CHILD_DATA_IN_ENV, REPRL_CHILD_DATA_OUT_ENV
    };
    HANDLE handles[] = { control_in, control_out, data_in, data_out };
    for (size_t i = 0; ok && i < 4; i++) {
        WCHAR assignment[96];
        int n = swprintf_s(assignment, sizeof(assignment) / sizeof(*assignment),
                           L"%ls=%llu", names[i], (unsigned long long)(uintptr_t)handles[i]);
        ok = n > 0 && set_environment_entry(&entries, assignment);
    }

    if (ok) {
        qsort(entries.values, entries.count, sizeof(*entries.values), compare_environment_entries);
        size_t total = 1;
        for (size_t i = 0; i < entries.count; i++) {
            size_t length = wcslen(entries.values[i]) + 1;
            if (total > SIZE_MAX - length) { ok = false; break; }
            total += length;
        }
        if (ok) {
            result = calloc(total, sizeof(*result));
            if (result) {
                WCHAR* cursor = result;
                for (size_t i = 0; i < entries.count; i++) {
                    size_t length = wcslen(entries.values[i]) + 1;
                    memcpy(cursor, entries.values[i], length * sizeof(*cursor));
                    cursor += length;
                }
            }
        }
    }
    free_environment_entries(&entries);
    return result;
}

static void add_inherited_handle(HANDLE* handles, DWORD* count, HANDLE handle)
{
    for (DWORD i = 0; i < *count; i++) {
        if (handles[i] == handle) return;
    }
    handles[(*count)++] = handle;
}

enum control_result { CONTROL_OK, CONTROL_TIMEOUT, CONTROL_CHILD_EXITED, CONTROL_ERROR };

static volatile LONG next_pipe_number;

static bool create_child_output_pipe(HANDLE* parent_read, HANDLE* child_write)
{
    WCHAR name[128];
    swprintf_s(name, sizeof(name) / sizeof(*name),
               L"\\\\.\\pipe\\fuzzilli-reprl-%lu-%ld-%llu", GetCurrentProcessId(),
               InterlockedIncrement(&next_pipe_number), (unsigned long long)GetTickCount64());
    SECURITY_ATTRIBUTES security = { sizeof(security), NULL, TRUE };
    HANDLE read_end = CreateNamedPipeW(name,
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, &security);
    if (read_end == INVALID_HANDLE_VALUE) return false;
    if (!SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
        DWORD error = GetLastError();
        CloseHandle(read_end);
        SetLastError(error);
        return false;
    }
    HANDLE write_end = CreateFileW(name, GENERIC_WRITE, 0, &security,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (write_end == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        CloseHandle(read_end);
        SetLastError(error);
        return false;
    }

    OVERLAPPED connection = { 0 };
    connection.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!connection.hEvent) {
        DWORD error = GetLastError();
        CloseHandle(write_end);
        CloseHandle(read_end);
        SetLastError(error);
        return false;
    }
    BOOL connected = ConnectNamedPipe(read_end, &connection);
    if (!connected) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            DWORD wait = WaitForSingleObject(connection.hEvent, 5000);
            DWORD ignored = 0;
            if (wait != WAIT_OBJECT_0) CancelIoEx(read_end, &connection);
            connected = GetOverlappedResult(read_end, &connection, &ignored, TRUE);
            if (!connected) error = GetLastError();
            if (wait != WAIT_OBJECT_0) {
                connected = FALSE;
                error = wait == WAIT_TIMEOUT ? ERROR_SEM_TIMEOUT : error;
            }
        } else if (error == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }
        if (!connected) {
            CloseHandle(connection.hEvent);
            CloseHandle(write_end);
            CloseHandle(read_end);
            SetLastError(error);
            return false;
        }
    }
    CloseHandle(connection.hEvent);
    *parent_read = read_end;
    *child_write = write_end;
    return true;
}

static enum control_result closed_control_pipe(struct reprl_context* ctx,
                                                uint64_t deadline, DWORD* error)
{
    // The pipe can close just before the process handle becomes signaled.
    // Give process teardown until the original deadline so a crash is reported
    // as a child exit rather than as a control-pipe failure.
    uint64_t now = current_usecs();
    uint64_t remaining = now < deadline ? deadline - now : 0;
    DWORD milliseconds = (DWORD)((remaining + 999) / 1000);
    DWORD wait = WaitForSingleObject(ctx->child, milliseconds);
    if (wait == WAIT_OBJECT_0) return CONTROL_CHILD_EXITED;
    if (wait == WAIT_FAILED) *error = GetLastError();
    return CONTROL_ERROR;
}

static enum control_result read_control(struct reprl_context* ctx, void* data, DWORD length,
                                         uint64_t timeout, DWORD* error)
{
    uint64_t deadline = current_usecs() + timeout;
    DWORD received = 0;
    while (received < length) {
        OVERLAPPED read = { 0 };
        read.hEvent = ctx->read_event;
        ResetEvent(ctx->read_event);
        DWORD amount = 0;
        if (!ReadFile(ctx->control_read, (char*)data + received, length - received,
                      &amount, &read)) {
            DWORD code = GetLastError();
            if (code == ERROR_IO_PENDING) {
                uint64_t now = current_usecs();
                uint64_t remaining = now < deadline ? deadline - now : 0;
                DWORD milliseconds = (DWORD)((remaining + 999) / 1000);
                HANDLE waits[2] = { ctx->read_event, ctx->child };
                DWORD wait = WaitForMultipleObjects(2, waits, FALSE, milliseconds);
                if (wait == WAIT_OBJECT_0 + 1) {
                    // A final status can already be buffered when the child exits.
                    DWORD grace = milliseconds < 10 ? milliseconds : 10;
                    if (WaitForSingleObject(ctx->read_event, grace) == WAIT_OBJECT_0)
                        wait = WAIT_OBJECT_0;
                }
                if (wait != WAIT_OBJECT_0) {
                    DWORD wait_error = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
                    CancelIoEx(ctx->control_read, &read);
                    DWORD completed = 0;
                    BOOL completed_ok = GetOverlappedResult(ctx->control_read, &read,
                                                             &completed, TRUE);
                    if (completed_ok && completed == length - received) {
                        received += completed;
                        return CONTROL_OK;
                    }
                    if (wait == WAIT_OBJECT_0 + 1
                            || WaitForSingleObject(ctx->child, 0) == WAIT_OBJECT_0)
                        return CONTROL_CHILD_EXITED;
                    if (wait == WAIT_TIMEOUT) return CONTROL_TIMEOUT;
                    *error = wait_error;
                    return CONTROL_ERROR;
                }
                if (!GetOverlappedResult(ctx->control_read, &read, &amount, FALSE))
                    code = GetLastError();
                else
                    code = ERROR_SUCCESS;
            }
            if (code != ERROR_SUCCESS) {
                *error = code;
                if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED)
                    return closed_control_pipe(ctx, deadline, error);
                return CONTROL_ERROR;
            }
        }
        if (!amount) {
            *error = ERROR_BROKEN_PIPE;
            return closed_control_pipe(ctx, deadline, error);
        }
        received += amount;
    }
    return CONTROL_OK;
}

static int reprl_spawn_child(struct reprl_context* ctx)
{
    SECURITY_ATTRIBUTES security = { sizeof(security), NULL, TRUE };
    HANDLE child_to_parent[2] = { NULL, NULL };
    HANDLE parent_to_child[2] = { NULL, NULL };
    HANDLE null_device = NULL;
    WCHAR* command = NULL;
    WCHAR* environment = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    bool attributes_initialized = false;
    PROCESS_INFORMATION process = { 0 };
    int result = -1;

    if (!create_child_output_pipe(&child_to_parent[0], &child_to_parent[1])
            || !CreatePipe(&parent_to_child[0], &parent_to_child[1], &security, 0)) {
        reprl_error(ctx, "Failed to create REPRL control pipes: Windows error %lu", GetLastError());
        goto cleanup;
    }
    if (!SetHandleInformation(parent_to_child[1], HANDLE_FLAG_INHERIT, 0)) {
        reprl_error(ctx, "Failed to configure REPRL control pipes: Windows error %lu", GetLastError());
        goto cleanup;
    }
    null_device = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (null_device == INVALID_HANDLE_VALUE) {
        null_device = NULL;
        reprl_error(ctx, "Failed to open NUL for child stdio: Windows error %lu", GetLastError());
        goto cleanup;
    }

    command = build_command_line(ctx->argv);
    environment = build_environment(ctx->envp, parent_to_child[0], child_to_parent[1],
                                    ctx->data_out->file, ctx->data_in->file);
    if (!command || !environment) {
        reprl_error(ctx, "Failed to build the Windows child command line or environment");
        goto cleanup;
    }

    HANDLE inherited[7];
    DWORD count = 0;
    add_inherited_handle(inherited, &count, parent_to_child[0]);
    add_inherited_handle(inherited, &count, child_to_parent[1]);
    add_inherited_handle(inherited, &count, ctx->data_out->file);
    add_inherited_handle(inherited, &count, ctx->data_in->file);
    add_inherited_handle(inherited, &count, null_device);
    add_inherited_handle(inherited, &count, ctx->child_stdout ? ctx->child_stdout->file : null_device);
    add_inherited_handle(inherited, &count, ctx->child_stderr ? ctx->child_stderr->file : null_device);

    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_bytes);
    attributes = malloc(attribute_bytes);
    if (!attributes || !InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes)) {
        reprl_error(ctx, "Failed to initialize child handle inheritance: Windows error %lu", GetLastError());
        goto cleanup;
    }
    attributes_initialized = true;
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, count * sizeof(*inherited), NULL, NULL)) {
        reprl_error(ctx, "Failed to select child handles: Windows error %lu", GetLastError());
        goto cleanup;
    }

    STARTUPINFOEXW startup = { 0 };
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_device;
    startup.StartupInfo.hStdOutput = ctx->child_stdout ? ctx->child_stdout->file : null_device;
    startup.StartupInfo.hStdError = ctx->child_stderr ? ctx->child_stderr->file : null_device;
    startup.lpAttributeList = attributes;

    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                        environment, NULL, &startup.StartupInfo, &process)) {
        reprl_error(ctx, "Failed to start child process %s: Windows error %lu",
                    ctx->argv[0], GetLastError());
        goto cleanup;
    }
    close_handle(&process.hThread);
    ctx->child = process.hProcess;
    process.hProcess = NULL;
    ctx->control_read = child_to_parent[0];
    child_to_parent[0] = NULL;
    ctx->control_write = parent_to_child[1];
    parent_to_child[1] = NULL;
    close_handle(&child_to_parent[1]);
    close_handle(&parent_to_child[0]);

    ctx->read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ctx->read_event) {
        reprl_error(ctx, "Failed to create REPRL read event: Windows error %lu", GetLastError());
        goto cleanup;
    }

    DWORD error = 0;
    char helo[4];
    switch (read_control(ctx, helo, sizeof(helo), REPRL_HANDSHAKE_TIMEOUT_US, &error)) {
    case CONTROL_OK:
        break;
    case CONTROL_TIMEOUT:
        reprl_error(ctx, "Timed out waiting for the child's REPRL HELO");
        goto cleanup;
    case CONTROL_CHILD_EXITED: {
        DWORD exit_code = 0;
        GetExitCodeProcess(ctx->child, &exit_code);
        reprl_error(ctx, "Child exited before REPRL HELO with status %lu", exit_code);
        goto cleanup;
    }
    case CONTROL_ERROR:
        reprl_error(ctx, "Failed to read the child's REPRL HELO: Windows error %lu", error);
        goto cleanup;
    }
    if (memcmp(helo, "HELO", sizeof(helo)) != 0) {
        reprl_error(ctx, "Child sent an invalid REPRL HELO");
        goto cleanup;
    }
    DWORD written = 0;
    if (!WriteFile(ctx->control_write, helo, sizeof(helo), &written, NULL)
            || written != sizeof(helo)) {
        reprl_error(ctx, "Failed to reply to the child's REPRL HELO: Windows error %lu", GetLastError());
        goto cleanup;
    }
    result = 0;

cleanup:
    if (result != 0 && ctx->child) reprl_terminate_child(ctx);
    close_handle(&process.hThread);
    close_handle(&process.hProcess);
    close_handle(&child_to_parent[0]);
    close_handle(&child_to_parent[1]);
    close_handle(&parent_to_child[0]);
    close_handle(&parent_to_child[1]);
    close_handle(&null_device);
    if (attributes_initialized) DeleteProcThreadAttributeList(attributes);
    free(attributes);
    free(command);
    free(environment);
    return result;
}

struct reprl_context* reprl_create_context(void)
{
    return calloc(1, sizeof(struct reprl_context));
}

int reprl_initialize_context(struct reprl_context* ctx, const char** argv, const char** envp,
                             int capture_stdout, int capture_stderr)
{
    if (ctx->initialized) return reprl_error(ctx, "Context is already initialized");
    if (!argv || !argv[0] || !argv[0][0]) return reprl_error(ctx, "Missing child executable");

    ctx->argv = copy_string_array(argv);
    ctx->envp = copy_string_array(envp);
    if (!ctx->argv || !ctx->envp)
        return reprl_error(ctx, "Out of memory copying child arguments or environment");

    ctx->data_in = reprl_create_data_channel(ctx);
    if (!ctx->data_in) return -1;
    ctx->data_out = reprl_create_data_channel(ctx);
    if (!ctx->data_out) return -1;
    if (capture_stdout) {
        ctx->child_stdout = reprl_create_data_channel(ctx);
        if (!ctx->child_stdout) return -1;
    }
    if (capture_stderr) {
        ctx->child_stderr = reprl_create_data_channel(ctx);
        if (!ctx->child_stderr) return -1;
    }
    ctx->initialized = true;
    return 0;
}

void reprl_destroy_context(struct reprl_context* ctx)
{
    if (!ctx) return;
    reprl_terminate_child(ctx);
    free_string_array(ctx->argv);
    free_string_array(ctx->envp);
    reprl_destroy_data_channel(ctx->data_in);
    reprl_destroy_data_channel(ctx->data_out);
    reprl_destroy_data_channel(ctx->child_stdout);
    reprl_destroy_data_channel(ctx->child_stderr);
    free(ctx->last_error);
    free(ctx);
}

static bool reset_channel(struct data_channel* channel)
{
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    return !channel || (SetFilePointerEx(channel->file, zero, NULL, FILE_BEGIN)
                        && SetEndOfFile(channel->file));
}

static int child_exit_status(DWORD exit_code)
{
    // Exception exit codes are NTSTATUS values. Report the common exceptions as
    // signals so RIFSIGNALED still identifies crashes on Windows.
    switch (exit_code) {
    case 0x80000001: // STATUS_GUARD_PAGE_VIOLATION
    case 0x80000002: // STATUS_DATATYPE_MISALIGNMENT
    case 0xC0000005: // STATUS_ACCESS_VIOLATION
    case 0xC00000FD: // STATUS_STACK_OVERFLOW
        return 11;
    case 0xC000001D: // STATUS_ILLEGAL_INSTRUCTION
    case 0xC0000096: // STATUS_PRIVILEGED_INSTRUCTION
        return 4;
    case 0xC0000094: // STATUS_INTEGER_DIVIDE_BY_ZERO
    case 0xC0000095: // STATUS_INTEGER_OVERFLOW
    case 0xC000008E: // STATUS_FLOAT_DIVIDE_BY_ZERO
        return 8;
    case 0x80000003: // STATUS_BREAKPOINT
    case 0x80000004: // STATUS_SINGLE_STEP
        return 5;
    default:
        if ((exit_code & 0xC0000000U) == 0xC0000000U) return 6;
        return (int)((exit_code & 0xffU) << 8);
    }
}

int reprl_execute(struct reprl_context* ctx, const char* script, uint64_t script_size,
                  uint64_t timeout, uint64_t* execution_time, int fresh_instance)
{
    if (!ctx->initialized) return reprl_error(ctx, "REPRL context is not initialized");
    if (script_size > REPRL_MAX_DATA_SIZE) return reprl_error(ctx, "Script too large");
    if (timeout > REPRL_MAX_TIMEOUT_IN_MICROSECONDS) return reprl_error(ctx, "Timeout too large");
    if (fresh_instance) reprl_terminate_child(ctx);

    if (!reset_channel(ctx->data_in) || !reset_channel(ctx->data_out)
            || !reset_channel(ctx->child_stdout) || !reset_channel(ctx->child_stderr)) {
        DWORD error = GetLastError();
        reprl_terminate_child(ctx);
        return reprl_error(ctx, "Failed to seek a REPRL data channel: Windows error %lu", error);
    }
    if (ctx->child && WaitForSingleObject(ctx->child, 0) == WAIT_OBJECT_0)
        reprl_child_terminated(ctx);
    if (!ctx->child && reprl_spawn_child(ctx) != 0) return -1;

    if (script_size) {
        DWORD script_written = 0;
        if (!WriteFile(ctx->data_out->file, script, (DWORD)script_size, &script_written, NULL)
                || script_written != script_size) {
            DWORD error = GetLastError();
            reprl_terminate_child(ctx);
            return reprl_error(ctx, "Failed to write REPRL script: Windows error %lu", error);
        }
    }
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(ctx->data_out->file, zero, NULL, FILE_BEGIN)) {
        DWORD error = GetLastError();
        reprl_terminate_child(ctx);
        return reprl_error(ctx, "Failed to rewind REPRL script: Windows error %lu", error);
    }
    DWORD written = 0;
    if (!WriteFile(ctx->control_write, "exec", 4, &written, NULL) || written != 4
            || !WriteFile(ctx->control_write, &script_size, sizeof(script_size), &written, NULL)
            || written != sizeof(script_size)) {
        DWORD error = GetLastError();
        DWORD exit_code = 0;
        bool exited = WaitForSingleObject(ctx->child, 0) == WAIT_OBJECT_0
                   && GetExitCodeProcess(ctx->child, &exit_code);
        reprl_terminate_child(ctx);
        if (exited) return reprl_error(ctx, "Child unexpectedly exited between executions with status %lu", exit_code);
        return reprl_error(ctx, "Failed to send REPRL command: Windows error %lu", error);
    }

    uint64_t start = current_usecs();
    DWORD error = 0;
    int32_t status = 0;
    enum control_result result = read_control(ctx, &status, sizeof(status), timeout, &error);
    *execution_time = current_usecs() - start;
    if (result == CONTROL_TIMEOUT) {
        reprl_terminate_child(ctx);
        return 1 << 16;
    }
    if (result == CONTROL_CHILD_EXITED) {
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(ctx->child, &exit_code)) {
            error = GetLastError();
            reprl_terminate_child(ctx);
            return reprl_error(ctx, "Failed to read child exit status: Windows error %lu", error);
        }
        reprl_child_terminated(ctx);
        return child_exit_status(exit_code);
    }
    if (result == CONTROL_ERROR) {
        reprl_terminate_child(ctx);
        return reprl_error(ctx, "Failed to read REPRL status: Windows error %lu", error);
    }
    return status & 0xffff;
}

static const char* fetch_data_channel_content(struct data_channel* channel)
{
    if (!channel) return "";
    LARGE_INTEGER zero, position;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(channel->file, zero, &position, FILE_CURRENT)) return "";
    size_t end = (size_t)position.QuadPart;
    if (end >= REPRL_MAX_DATA_SIZE) end = REPRL_MAX_DATA_SIZE - 1;
    if (channel->content_capacity <= end) {
        char* content = realloc(channel->content, end + 1);
        if (!content) return "";
        channel->content = content;
        channel->content_capacity = end + 1;
    }
    if (!SetFilePointerEx(channel->file, zero, NULL, FILE_BEGIN)) return "";
    size_t received = 0;
    while (received < end) {
        DWORD amount = 0;
        if (!ReadFile(channel->file, channel->content + received, (DWORD)(end - received),
                      &amount, NULL) || !amount) {
            channel->content[0] = '\0';
            SetFilePointerEx(channel->file, position, NULL, FILE_BEGIN);
            return channel->content;
        }
        received += amount;
    }
    channel->content[received] = '\0';
    SetFilePointerEx(channel->file, position, NULL, FILE_BEGIN);
    return channel->content;
}

const char* reprl_fetch_fuzzout(struct reprl_context* ctx)
{
    return fetch_data_channel_content(ctx->data_in);
}

const char* reprl_fetch_stdout(struct reprl_context* ctx)
{
    return fetch_data_channel_content(ctx->child_stdout);
}

const char* reprl_fetch_stderr(struct reprl_context* ctx)
{
    return fetch_data_channel_content(ctx->child_stderr);
}

const char* reprl_get_last_error(struct reprl_context* ctx)
{
    return ctx->last_error;
}

#endif
