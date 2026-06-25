# SimpleMail

> **CS39006 — Computer Network Lab, Mini Project 2**
> A complete command-line email system implemented in C using raw TCP sockets, custom application-layer protocols, and a non-blocking, single-threaded server architecture.

---

## Project Overview

SimpleMail is a command-line email system consisting of two programs:

- **`smserver`** — A concurrent mail server that stores per-user mailboxes as flat files on disk and speaks two custom protocols.
- **`smclient`** — An interactive client that connects to the server to send and receive mail via a simple terminal menu.

Communication uses two purpose-built, text-based, TCP protocols:

- **SMTP2** (SimpleMail Transfer Protocol 2) — for composing and delivering mail.
- **SMP** (SimpleMail Pickup Protocol) — for authenticated mailbox access (list, read, delete).

---

## File Structure

```
.
├── smserver.c        # Server implementation
├── smclient.c        # Client implementation
├── global.c          # Shared types, constants, and utility functions
├── Makefile          # Build system
├── userfile.txt      # Sample user registry (username + password per line)
└── mailboxes/        # Auto-created at runtime if not exists already; one sub-directory per user
    └── <username>/
        ├── 1.txt     # Individual mail files
        ├── 2.txt
        └── .metadata # Next available mail ID (hidden file, server-managed)
```

---

## Compilation & Execution

### Build

```bash
make
```

This produces two executables: `smserver` and `smclient`.

To remove build artifacts:

```bash
make clean
```

### Run the Server

```bash
./smserver <port> <userfile>
```

**Example:**

```bash
./smserver 9000 userfile.txt
```

The server reads `userfile.txt`, creates the `mailboxes/` directory tree if it does not already exist, and begins listening. Every significant event is printed to stdout with a `[YYYY-MM-DD HH:MM:SS]` timestamp prefix.

### Run the Client

```bash
./smclient <server_ip> <port>
```

**Example:**

```bash
./smclient 127.0.0.1 9000
```

Upon connecting, the client presents an interactive menu:

```
Connected to SimpleMail server
1. Send a mail
2. Check my mail
3. Quit
>
```

---

## Protocol Overview

Both protocols operate over TCP. Every command and response is a single line terminated by `\r\n`. Multi-line content (mail body, `LIST` output, `READ` output) is terminated by a line containing only `.` (dot-termination).

### SMTP2 — Sending Protocol (MODE SEND)

| Step | Client → Server | Server → Client |
|---|---|---|
| Mode selection | `MODE SEND` | `OK` |
| Sender | `FROM <display_name>` | `OK Sender accepted` |
| Recipients (repeat) | `TO <username>` | `OK Recipient accepted` / `ERR No such user` |
| Subject | `SUB <subject>` | `OK Subject accepted` |
| Body start | `BODY` | `OK Send body, end with CRLF.CRLF` |
| Body lines | `<line>` | *(none)* |
| Body end | `.` | `OK Delivered to N mailboxes` |
| Session end | `QUIT` | `BYE` |

Commands must arrive in strict order: `FROM → TO(s) → SUB → BODY → . → QUIT`. Any out-of-sequence command receives `ERR Bad sequence`.

### SMP — Retrieval Protocol (MODE RECV)

| Step | Client → Server | Server → Client |
|---|---|---|
| Mode selection | `MODE RECV` | `OK` |
| Challenge | *(server-initiated)* | `AUTH REQUIRED <nonce>` |
| Auth response | `AUTH <username> <hash>` | `OK Welcome <username>` / `ERR Authentication failed` |
| List mailbox | `LIST` | `OK <count> messages` + tab-delimited lines + `.` |
| Read a mail | `READ <id>` | `OK` + mail contents + `.` |
| Delete a mail | `DELETE <id>` | `OK Deleted` / `ERR No such message` |
| Count mails | `COUNT` | `OK <count>` |
| Logout | `QUIT` | `BYE` |

Authentication uses a **DJB2 challenge-response** scheme: the server appends an 8-character random alphanumeric nonce to `AUTH REQUIRED`. The client computes `djb2(password + nonce)` and sends the resulting unsigned decimal hash. The raw password is never transmitted over the network.

---

## Architecture & Design Choices

### 1. Non-Blocking Concurrency via `select()` and Finite State Machines

The server handles all clients inside a **single-threaded event loop** driven by `select()`. This avoids the complexity of multi-threading while achieving true concurrency with no blocking I/O.

Because no client can block the loop, the server tracks each connection's progress through a **two-level state machine**:

**Macro state** (`connection_status` enum in `global.c`):

| State | Meaning |
|---|---|
| `CONN_FREE` | Slot is available |
| `CONN_CONNECTED` | Connected; waiting for `MODE SEND` or `MODE RECV` |
| `CONN_SEND` | Active SMTP2 session |
| `CONN_RECV` | Active SMP session |

**Protocol-level FSM states:**

- SMTP2: `SMTP2_INIT -> SMTP2_FROM -> SMTP2_TO_SUB -> SMTP2_BODY -> SMTP2_FINISH`
- SMP: `SMP_INIT -> SMP_INIT_AUTH -> SMP_AUTH -> SMP_MAILBOX_READY -> SMP_FINISH`

Each incoming message is routed to either `handleSend()` or `handleRetrieve()`, which advance the appropriate FSM.

**Transient states** (`SMTP2_FINISH`, `SMP_FINISH`) are handled without waiting for the next `select()` event. The main loop detects a finished state immediately after the handler returns and calls it once more with an empty string to execute any final teardown logic (e.g., resetting to `CONN_CONNECTED` for SMTP2 re-use, or freeing the slot for SMP). This ensures prompt cleanup without stalling the loop.

**Memory efficiency** is achieved by storing the two protocol contexts inside a `union` in each `client_connection` slot. Since a connection is always in exactly one protocol mode, only one context is ever live at a time, halving the per-slot memory footprint.

### 2. Mail ID Persistence — The `.metadata` Database Approach

A core requirement is that mail IDs must be monotonically increasing and must never be reused, even after deletion and across server restarts.

Tracking a `next_mail_id` counter purely in RAM would risk a "split-brain" desync: if the server crashes mid-delivery, or if two deliveries arrive concurrently, the in-memory counter could diverge from what is actually stored on disk.

The chosen solution treats each user's mailbox directory as a tiny database:

- **Primary source of truth:** A hidden `.metadata` file inside each mailbox directory stores a single integer — the next available mail ID.
- **Transactional delivery:** On each delivery, the server reads `.metadata`, writes `<id>.txt`, then atomically overwrites `.metadata` with `next_id + 1`. The ID is committed to disk before the server acknowledges delivery.
- **Fallback:** If `.metadata` is absent (e.g., first boot on a pre-existing mailbox), `getNextMailId()` scans the directory via `readdir()`, finds the maximum `.txt` ID present, and uses `max + 1`. It then creates `.metadata` so subsequent calls are fast.
- **Filtering:** `LIST` and `COUNT` operations explicitly check for the `.txt` extension (and skip `METADATA_FILE` by name in `COUNT`) so the hidden file is never exposed as a mail entry.

### 3. Client Connection Lifecycle

The assignment mandates one protocol session per TCP connection. The client manages this with a `state` variable (`CLIENT_FREE`, `CLIENT_SEND`, `CLIENT_RECV`) and an `is_connected` flag.
Teardown and reconnect logic is triggered at specific transition points:

- **`sendMail()` closes the session itself at the end of delivery after the user chooses the Quit option by calling `closeSession()`**, which sends `QUIT`, waits for `BYE`, and calls `close()`.
- **Switching to `checkMail()` triggers a teardown-and-reconnect only if `state != CLIENT_FREE`**, meaning a previous session socket is still logically open. In that case the main loop calls `closeSession()` followed by `connectServer()` before entering `checkMail()`.
- Logout inside `checkMail()` calls `closeSession()` and sets `state = CLIENT_FREE`, signalling that the next menu action can reconnect normally.
`is_connected` is only set to 0 on a timeout, when `recvMsg() returns <= 0` at the start of `sendMail()` or `checkMail()`. This causes the main loop's if `(!is_connected)` guard to establish a fresh connection before showing the menu again.

### 4. Dot-Stuffing

The dot-termination convention (`\r\n.\r\n`) requires that any body line starting with `.` be escaped by prepending an extra period (byte-stuffing on the client side) and stripped on receipt (de-stuffing on the server side). This is fully implemented in both directions:

- **Client (`smclient.c`):** Before sending each body line, if `protocol_buf[0] == '.'`, the line is sent as `.%s` (doubling the leading dot).
- **Server (`smserver.c`, `SMTP2_BODY` state):** When a body line starts with `.` and is not the lone dot-terminator, the leading `.` is stripped before appending to the body buffer.

---

## Edge Case Handling

### 30-Second Inactivity Timeout

The `select()` call uses a **1-second `timeval` poll** rather than blocking indefinitely. After each `select()` returns, the server iterates over all connections in `CONN_CONNECTED` state and computes `difftime(current_time, connect_time)`. Any connection that has waited more than 30 seconds for a `MODE` declaration is forcibly closed and its slot is freed.

### Server Timeout

When the server times out a connection, the next `recvMsg()` call in the client returns `<= 0`. Both `sendMail()` and `checkMail()` check this return value immediately after sending the `MODE` command. If the connection has dropped, the client prints a friendly message (`[Error] The server closed the connection (30-second timeout).`), resets `is_connected = 0`, and returns to the main menu so a fresh connection is made on the next action.

### 64 KB Body Overflow

If the incoming body exceeds the 65536-byte limit, a naïve early exit from the receive loop would leave unread bytes in the TCP buffer, causing a protocol desync on the next message.

Instead, the server sets an `is_overflow` flag when the limit is reached. It continues reading and discarding body lines until the dot-terminator arrives, fully draining the network buffer. Only then does it send `ERR Body too large`. This ensures the connection remains in a known, clean state.

### Empty Subject

A `SUB` command with no trailing text results in the subject being stored as `(no subject)` rather than an empty string, per the specification.

### No Valid Recipients

If a `BODY` command is received but no `TO` command has been accepted yet (all recipients were unknown), the server responds with `ERR No valid recipients` and does not enter body-receive mode.

### Out-of-Sequence Commands

Both FSMs respond with `ERR Bad sequence` for any command that arrives out of the expected order, e.g., `BODY` before `SUB`, or `SUB` before any `FROM`.

### Authentication Brute-Force Limit

The SMP authentication loop allows at most **3 attempts**. On the third failure the server sends `ERR Too many failures` and closes the connection. The attempt counter is stored per-connection in `smp_context.auth_attempts`.

---

## Assumptions

1. **Static user registry.** The users loaded from `userfile.txt` at startup are the only valid users for the lifetime of that server process. Runtime registration of new users is not supported. To add users, edit `userfile.txt` and restart the server.

2. **`.metadata` is the authoritative ID source.** Each user's mailbox directory is expected to contain a `.metadata` file that stores a single integer representing the next mail ID to be assigned. If this file is absent (e.g., on first use of a pre-existing mailbox), the server falls back to scanning for the highest-numbered `.txt` file and reconstructs the metadata file. Manually editing or deleting `.metadata` may cause ID gaps but will not cause ID reuse.

3. **Mail files use the `.txt` extension exclusively.** The server identifies mail files strictly by the `.txt` extension. Files with any other extension (including the `.metadata` file) inside a mailbox directory are ignored by `LIST`, `COUNT`, and `READ`. Placing non-`.txt` files in a mailbox directory by hand will not affect correctness.

4. **Single-host deployment assumed for address binding.** The server binds to `INADDR_ANY`, accepting connections on all available network interfaces. The client takes the server IP as a command-line argument.

5. **No TLS/encryption.** All communication is plaintext over TCP. The DJB2 challenge-response scheme prevents raw password transmission but does not provide confidentiality of message contents.

---

## Sample `userfile.txt`

```
alice secretpass1
bob hunter2
charlie x9Kp2mW
dave p@ssw0rd
eve sunshine99
```

Each line contains a username (lowercase alphabetic, max 20 characters) and a password (case-sensitive alphanumeric, max 30 characters) separated by a single space.

---

*Author: 23CS30029 — Kshetrimayum Abo*
