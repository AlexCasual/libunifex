/*
 * Copyright 2020-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <unifex/win32/low_latency_iocp_context.hpp>
#include <unifex/scope_guard.hpp>

#include <atomic>
#include <system_error>
#include <random>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace unifex::win32 {

namespace {

static safe_handle create_iocp() {
    HANDLE h = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (h == NULL) {
        DWORD errorCode = ::GetLastError();
        throw std::system_error{static_cast<int>(errorCode), std::system_category(), "CreateIoCompletionPort()"};
    }

    return safe_handle{h};
}

} // namespace

low_latency_iocp_context::low_latency_iocp_context(std::size_t maxIoOperations)
: activeThreadId_(std::thread::id())
, iocp_(create_iocp())
, ioPoolSize_(maxIoOperations)
, ioPool_(std::make_unique<vectored_io_state[]>(maxIoOperations)) {

    // Build the I/O free-list in reverse so front of free-list
    // is first element in array.
    for (std::size_t i = 0; i < maxIoOperations; ++i) {
        ioFreeList_.push_front(&ioPool_[maxIoOperations - 1 - i]);
    }
}

low_latency_iocp_context::~low_latency_iocp_context() {
    // Wait until the completion-event for every io-state has been 
    // received before we free the memory for the io-states.
    std::size_t remaining = ioPoolSize_;
    while (!ioFreeList_.empty()) {
        (void)ioFreeList_.pop_front();
        --remaining;
    }

    if (remaining > 0) {
        constexpr std::uint32_t completionBufferSize = 128;
        OVERLAPPED_ENTRY completionBuffer[completionBufferSize];
        std::memset(&completionBuffer, 0, sizeof(completionBuffer));

        do {
            DWORD numEntriesRemoved = 0;
            BOOL ok = ::GetQueuedCompletionStatusEx(
                iocp_.get(),
                completionBuffer,
                completionBufferSize,
                &numEntriesRemoved,
                INFINITE,
                FALSE);
            if (!ok) {
                std::terminate();
            }

            for (std::uint32_t i = 0; i < numEntriesRemoved; ++i) {
                auto& entry = completionBuffer[i];
                if (entry.lpOverlapped != nullptr) {
                    win32::overlapped* o = reinterpret_cast<win32::overlapped*>(entry.lpOverlapped);
                    auto* state = to_io_state(o);
                    --state->pendingCompletionNotifications;
                    if (state->pendingCompletionNotifications == 0) {
                        --remaining;
                    }
                }
            }
        } while (remaining > 0);
    }
}

void low_latency_iocp_context::run_impl(bool& stopFlag) {

    [[maybe_unused]] auto prevId = activeThreadId_.exchange(std::this_thread::get_id(), std::memory_order_relaxed);
    assert(prevId == std::thread::id());

    scope_guard resetActiveThreadIdOnExit = [&]() noexcept {
        activeThreadId_.store(std::thread::id(), std::memory_order_relaxed);
    };

    constexpr std::uint32_t completionBufferSize = 128;
    OVERLAPPED_ENTRY completionBuffer[completionBufferSize];
    std::memset(&completionBuffer, 0, sizeof(completionBuffer));

    bool shouldCheckRemoteQueue = true;

    // Make sure that we leave the remote queue in a
    // state such that the  'shouldCheckRemoteQueue = true'
    // is a valid initial state for the next caller to run().
    scope_guard ensureRemoteQueueActiveOnExit = [&]() noexcept {
        if (!shouldCheckRemoteQueue) {
            (void)remoteQueue_.try_mark_active();
        }
    };

    while (!stopFlag) {
    process_ready_queue:
        // Process/drain all ready-to-run work.
        while (!readyQueue_.empty()) {
            operation_base* task = readyQueue_.pop_front();
            task->callback(task);
        }

        // Check whether the stop request was processed
        // when draining the ready queue.
        if (stopFlag) {
            break;
        }

        // Check the poll-queue for I/O operations that might have completed
        // already but where we have not yet seen the IOCP event.

        while (!pollQueue_.empty()) {
           io_operation* op = static_cast<io_operation*>(pollQueue_.pop_front());
           auto* state = op->ioState;
           assert(state->pendingCompletionNotifications > 0);
           assert(!state->completed);

           if (poll_is_complete(*state)) {
               // Completed before we received any notifications via IOCP.
               // Schedule it to resume now, before polling any other I/Os
               // to give those other I/Os time to complete.
               state->completed = true;
               schedule_local(state->parent);
               goto process_ready_queue;
           }
        }

    process_remote_queue:
        if (shouldCheckRemoteQueue) {
            if (try_dequeue_remote_work()) {
                goto process_ready_queue;
            }
        }

        // Now check if there are any IOCP events.
    get_iocp_entries:
        ULONG completionEntriesRetrieved = 0;
        BOOL ok = ::GetQueuedCompletionStatusEx(
            iocp_.get(),
            completionBuffer,
            completionBufferSize,
            &completionEntriesRetrieved,
            shouldCheckRemoteQueue ? 0 : INFINITE,
            FALSE);
        if (!ok) {
            DWORD errorCode = ::GetLastError();
            if (errorCode == WAIT_TIMEOUT) {
                assert(shouldCheckRemoteQueue);

                // Previous call was non-blocking.
                // About to transition to blocking-call, but first need to
                // mark remote queue inactive so that remote threads know
                // they need to post an event to wake this thread up.
                if (remoteQueue_.try_mark_inactive()) {
                    shouldCheckRemoteQueue = false;
                    goto get_iocp_entries;
                } else {
                    goto process_remote_queue;
                }
            }

            throw std::system_error{static_cast<int>(errorCode), std::system_category(), "GetQueuedCompletionStatusEx()"};
        }

        // Process completion-entries we received from the OS.
        for (ULONG i = 0; i < completionEntriesRetrieved; ++i) {
            auto& entry = completionBuffer[i];
            if (entry.lpOverlapped != nullptr) {
                win32::overlapped* o = reinterpret_cast<win32::overlapped*>(entry.lpOverlapped);
                // TODO: Do we need to store the error-code/bytes-transferred here?

                auto* state = to_io_state(o);

                assert(state->pendingCompletionNotifications > 0);
                if (--state->pendingCompletionNotifications == 0) {
                    // This was the last pending notification for this state. 
                    if (state->parent != nullptr) {
                        // An operation is still attached to this state.
                        // Notify it if we hadn't already done so.
                        if (!state->completed) {
                            state->completed = true;
                            schedule_local(state->parent);
                        }
                    } else {
                        // Otherwise the operation has already detached
                        // from this I/O state and so we can immediately
                        // return it to the pool.
                        release_io_state(state);
                    }
                }
            } else {
                // A remote-thread wake-up notification.
                shouldCheckRemoteQueue = true;
            }
        }
    }
}

bool low_latency_iocp_context::try_dequeue_remote_work() noexcept {
    auto items = remoteQueue_.dequeue_all_reversed();
    if (items.empty()) {
        return false;
    }

    do {
        schedule_local(items.pop_front());
    } while (!items.empty());

    return true;
}

bool low_latency_iocp_context::poll_is_complete(vectored_io_state& state) noexcept {
    // TODO: Double check the memory barriers/atomics we need to use
    // here to ensure we perform this read, that it doesn't tear and
    // that it has the appropriate memory semantics.

    for (std::size_t i = 0; i < state.operationCount; ++i) {
        overlapped* o = &state.operations[i];

        // Mark volatile to indicate to the compiler that it might change in the
        // background. The kernel might update it as the I/O completes.
        volatile io_status_block* iosb = reinterpret_cast<io_status_block*>(o);
        if (iosb->Status == STATUS_PENDING) {
            return false;
        }
    }

    // Make sure that we have visibility of the results of the I/O operations
    // (assuming that the OS used a "release" memory semantic when writing
    // to the 'Status' field).
    std::atomic_thread_fence(std::memory_order_acquire);

    return true;
}

low_latency_iocp_context::vectored_io_state* low_latency_iocp_context::to_io_state(overlapped* o) noexcept {
    vectored_io_state* const pool = ioPool_.get();
    
    std::ptrdiff_t offset = reinterpret_cast<char*>(o) - reinterpret_cast<char*>(pool);
    assert(offset >= 0);

    std::ptrdiff_t index = offset / sizeof(vectored_io_state);

    return &pool[index];
}

void low_latency_iocp_context::schedule(operation_base* op) noexcept {
    if (is_running_on_io_thread()) {
        schedule_local(op);
    } else {
        schedule_remote(op);
    }
}

void low_latency_iocp_context::schedule_local(operation_base* op) noexcept {
    assert(is_running_on_io_thread());
    readyQueue_.push_back(op);
}

void low_latency_iocp_context::schedule_remote(operation_base* op) noexcept {
    assert(!is_running_on_io_thread());
    if (remoteQueue_.enqueue(op)) {
        // I/O thread is potentially sleeping.
        // Post a wake-up NOP event to the queue.

        // BUGBUG: This could potentially fail and if it does then
        // the I/O thread might never respond to remote enqueues.
        // For now we just treat this as a (hopefully rare) unrecoverable error.
        BOOL ok = ::PostQueuedCompletionStatus(iocp_.get(), 0, 0, nullptr);
        if (!ok) {
            std::terminate();
        }
    }
}

bool low_latency_iocp_context::try_allocate_io_state_for(io_operation* op) noexcept {
    assert(is_running_on_io_thread());

    if (ioFreeList_.empty()) {
        return false;
    }

    assert(pendingIoQueue_.empty());

    // An operation is already available
    auto* state = ioFreeList_.pop_front();
    state->parent = op;
    state->completed = false;
    state->operationCount = 0;
    state->pendingCompletionNotifications = 0;
    op->ioState = state;

    return true;
}

void low_latency_iocp_context::schedule_when_io_state_available(io_operation* op) noexcept {
    assert(is_running_on_io_thread());
    assert(ioFreeList_.empty());
    pendingIoQueue_.push_back(op);
}

void low_latency_iocp_context::release_io_state(vectored_io_state* state) noexcept {
    assert(is_running_on_io_thread());

    if (state->pendingCompletionNotifications == 0) {
        // Can be freed immediately.

        if (pendingIoQueue_.empty()) {
            ioFreeList_.push_front(state);
        } else {
            assert(ioFreeList_.empty());

            // Another operation was waiting for an I/O state.
            // Give the I/O state directly to the operation instead
            // of adding it to the free-list. This prevents some other
            // operation from skipping the queue and acquiring it
            // before this I/O operation can run.
            auto* op = static_cast<io_operation*>(pendingIoQueue_.pop_front());

            state->parent = op;
            state->completed = false;
            state->operationCount = 0;
            state->pendingCompletionNotifications = 0;
            op->ioState = state;

            schedule_local(op);
        }
    } else {
        // Mark it to be freed once all of its I/O completion
        // notifications have been received.
        state->parent = nullptr;
    }
}

void low_latency_iocp_context::schedule_poll_io(io_operation* op) noexcept {
    assert(is_running_on_io_thread());
    assert(op != nullptr);
    assert(op->ioState != nullptr);

    if (op->ioState->pendingCompletionNotifications > 0) {
        pollQueue_.push_back(op);
    } else {
        // No need to poll, schedule straight to the front of the ready-to-run queue.
        readyQueue_.push_front(op);
    }
}

void low_latency_iocp_context::associate_file_handle(handle_t fileHandle) {
    const HANDLE result = ::CreateIoCompletionPort(
        fileHandle,
        iocp_.get(),
        0, 0);
    if (result == nullptr) {
        DWORD errorCode = ::GetLastError();
        throw std::system_error{
            static_cast<int>(errorCode), std::system_category(),
            "CreateIoCompletionPort"};
    }

    const BOOL ok = ::SetFileCompletionNotificationModes(
        fileHandle,
        FILE_SKIP_COMPLETION_PORT_ON_SUCCESS |
        FILE_SKIP_SET_EVENT_ON_HANDLE);
    if (!ok) {
        DWORD errorCode = ::GetLastError();
        throw std::system_error{
            static_cast<int>(errorCode), std::system_category(),
            "SetFileCompletionNotificationModes"};
    }
}

void low_latency_iocp_context::io_operation::cancel_io() noexcept {
    assert(ioState != nullptr);

    // Cancel operations in reverse order so that later operations
    // are cancelled first and don't accidentally end up with earlier
    // operations being cancelled and later ones completing due to
    // a race.
    for (std::uint16_t i = ioState->operationCount; i != 0; --i) {
        win32::overlapped* o = &ioState->operations[i-1];
        (void)::CancelIoEx(fileHandle, reinterpret_cast<OVERLAPPED*>(o));
    }
}

bool low_latency_iocp_context::io_operation::is_complete() noexcept {
    assert(context.is_running_on_io_thread());
    assert(ioState != nullptr);

    if (ioState->pendingCompletionNotifications == 0) {
        return true;
    }

    for (std::size_t i = 0; i < ioState->operationCount; ++i) {
        overlapped* o = &ioState->operations[i];

        // Mark volatile to indicate to the compiler that it might change in the
        // background. The kernel might update it as the I/O completes.
        volatile io_status_block* iosb = reinterpret_cast<io_status_block*>(o);
        if (iosb->Status == STATUS_PENDING) {
            return false;
        }
    }

    // Make sure that we have visibility of the results of all of the I/O
    // operations.
    // Assuming that the OS used a "release" memory semantic when writing
    // to the 'Status' field here.
    std::atomic_thread_fence(std::memory_order_acquire);

    return true;
}

bool low_latency_iocp_context::io_operation::start_read(
        span<std::byte> buffer) noexcept {
    assert(context.is_running_on_io_thread());
    assert(ioState != nullptr);
    assert(ioState->operationCount < max_vectored_io_size);

    bool allowStartingMore = false;

    std::size_t offset = 0;
    while (offset < buffer.size()) {
        auto& op = ioState->operations[ioState->operationCount];
        ++ioState->operationCount;

        op.Internal = 0;
        op.InternalHigh = 0;
        op.Offset = 0;
        op.OffsetHigh = 0;
        op.hEvent = nullptr;

        // Truncate over-large chunks to a number of bytes that will still
        // preserve alignment requirements of the underlying device if this
        // happens to be an unbuffered storage device.
        // In this case we truncate to the largest multiple of 64k less than
        // 2^32 to allow for up to 64k alignment.
        // TODO: Ideally we'd just use the underlying alignment of the file-handle.
        static constexpr std::size_t truncatedChunkSize = 0xFFFF0000u;
        static constexpr std::size_t maxChunkSize = 0xFFFFFFFFu;

        std::size_t chunkSize = buffer.size() - offset;
        if (chunkSize > maxChunkSize) {
            chunkSize = truncatedChunkSize;
        }

        BOOL ok = ::ReadFile(
            fileHandle,
            buffer.data() + offset,
            static_cast<DWORD>(chunkSize),
            nullptr,
            reinterpret_cast<OVERLAPPED*>(&op));
        if (!ok) {
            DWORD errorCode = ::GetLastError();
            if (errorCode == ERROR_IO_PENDING) {
                ++ioState->pendingCompletionNotifications;
            } else {
                // Immediate Failure.
                // Don't launch any more operations.
                // TODO: Should we cancel the other operations?
                return false;
            }
        } else {
            // Succceeded synchronously
            if (!skipNotificationOnSuccess) {
                ++ioState->pendingCompletionNotifications;
            }
        }

        if (ioState->operationCount == max_vectored_io_size) {
            return false;
        }

        offset += chunkSize;
    }

    return true;
}

bool low_latency_iocp_context::io_operation::start_write(
        span<const std::byte> buffer) noexcept {
    assert(context.is_running_on_io_thread());
    assert(ioState != nullptr);
    assert(ioState->operationCount < max_vectored_io_size);

    bool allowStartingMore = false;

    std::size_t offset = 0;
    while (offset < buffer.size()) {
        auto& op = ioState->operations[ioState->operationCount];
        ++ioState->operationCount;

        op.Internal = 0;
        op.InternalHigh = 0;
        op.Offset = 0;
        op.OffsetHigh = 0;
        op.hEvent = nullptr;

        // Truncate over-large chunks to a number of bytes that will still
        // preserve alignment requirements of the underlying device if this
        // happens to be an unbuffered storage device.
        // In this case we truncate to the largest multiple of 64k less than
        // 2^32 to allow for up to 64k alignment.
        // TODO: Ideally we'd just use the underlying alignment of the file-handle.
        static constexpr std::size_t truncatedChunkSize = 0xFFFF0000u;
        static constexpr std::size_t maxChunkSize = 0xFFFFFFFFu;

        std::size_t chunkSize = buffer.size() - offset;
        if (chunkSize > maxChunkSize) {
            chunkSize = truncatedChunkSize;
        }

        BOOL ok = ::WriteFile(
            fileHandle,
            buffer.data() + offset,
            static_cast<DWORD>(chunkSize),
            nullptr,
            reinterpret_cast<OVERLAPPED*>(&op));
        if (!ok) {
            DWORD errorCode = ::GetLastError();
            if (errorCode == ERROR_IO_PENDING) {
                ++ioState->pendingCompletionNotifications;
            } else {
                // Immediate Failure.
                // Don't launch any more operations.
                // TODO: Should we cancel the other operations?
                return false;
            }
        } else {
            // Succceeded synchronously
            if (!skipNotificationOnSuccess) {
                ++ioState->pendingCompletionNotifications;
            }
        }

        if (ioState->operationCount == max_vectored_io_size) {
            return false;
        }

        offset += chunkSize;
    }

    return true;
}

std::size_t low_latency_iocp_context::io_operation::get_result(std::error_code& ec) noexcept {
    assert(context.is_running_on_io_thread());
    assert(ioState != nullptr);
    assert(ioState->completed);

    ec = std::error_code{};

    std::size_t totalBytesTransferred = 0;
    for (std::size_t i = 0; i < ioState->operationCount; ++i) {
        DWORD bytesTransferred = 0;

        // This (hoepfully) shouldn't involve any kernel calls since it should
        // already be complete and this can be determined by looking at the OVERLAPPED
        // structure.
        BOOL ok = ::GetOverlappedResultEx(
            fileHandle,
            reinterpret_cast<OVERLAPPED*>(&ioState->operations[i]),
            &bytesTransferred,
            0 /* timeout */,
            FALSE /* bAlertable */);
        totalBytesTransferred += bytesTransferred;
        if (!ok) {
            // Stop at the first error we encountered.
            DWORD errorCode = ::GetLastError();
            ec = std::error_code{static_cast<int>(errorCode), std::system_category()};
            break;
        }
    }

    return totalBytesTransferred;
}

std::tuple<low_latency_iocp_context::readable_byte_stream,
           low_latency_iocp_context::writable_byte_stream>
low_latency_iocp_context::scheduler::open_pipe_impl(
    low_latency_iocp_context& context) {
    std::mt19937_64 rand{::GetTickCount64() + ::GetCurrentThreadId()};

    safe_handle serverHandle;

    auto toHex = [](std::uint8_t value) noexcept -> char {
        return value < 10 ? char('0' + value) : char('a' - 10 + value);
    };

    char pipeName[10 + 16] = {'\\', '\\', '.', '\\', 'p', 'i', 'p', 'e', '\\'};

    // Try to create the server side of a new named pipe
    // Use a randomly generated name to ensure uniqueness.

    const DWORD pipeBufferSize = 16 * 1024;
    const int maxAttempts = 100;

    for (int attempt = 1; ; ++attempt) {
        const std::uint64_t randomNumber = rand();
        for (int i = 0; i < 16; ++i) {
            pipeName[9 + i] = toHex((randomNumber >> (4 * i)) & 0xFu);
        }
        pipeName[25] = '\0';

        HANDLE pipe = ::CreateNamedPipeA(
            pipeName,
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS | PIPE_WAIT,
            1, // nMaxInstances
            0, // nOutBufferSize
            pipeBufferSize, // nInBufferSize
            0, // nDefaultTimeout
            nullptr); // lpSecurityAttributes
        if (pipe == INVALID_HANDLE_VALUE) {
            DWORD errorCode = ::GetLastError();
            if (errorCode == ERROR_ALREADY_EXISTS && attempt < maxAttempts) {
                // Try again with a different random name
                continue;
            }

            throw std::system_error{
                static_cast<int>(errorCode), std::system_category(),
                "open_pipe: CreateNamedPipe"};
        }

        serverHandle = safe_handle{pipe};
        break;
    }

    // Open the client-side of this named-pipe
    safe_handle clientHandle{::CreateFileA(
        pipeName,
        GENERIC_WRITE,
        0, // dwShareMode
        nullptr, // lpSecurityAttributes
        OPEN_EXISTING, // dwCreationDisposition
        FILE_FLAG_OVERLAPPED,
        nullptr)};
    if (clientHandle == INVALID_HANDLE_VALUE) {
        DWORD errorCode = ::GetLastError();
        throw std::system_error{
            static_cast<int>(errorCode), std::system_category(),
            "open_pipe: CreateFile"};
    }

    context.associate_file_handle(serverHandle.get());
    context.associate_file_handle(clientHandle.get());

    return {
        readable_byte_stream{context, std::move(serverHandle)},
        writable_byte_stream{context, std::move(clientHandle)}
        };
}

} // namespace unifex::win32
