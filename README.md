# IE3090 RemoteOps – IT24102368

## Remote System Monitoring and Management Tool

RemoteOps is a client-server network programming application developed for the IE3090 Network Programming module.

The system consists of:

* **Agent** – server program running on the managed machine.
* **Controller** – client program used by the administrator.

The Agent and Controller communicate using TCP/IP. UDP is used for periodic system monitoring.

---

## Student Information

**Registration Number:** IT24102368

**Module:** IE3090 – Network Programming

**Assignment:** RemoteOps

---

## Personalization Details

| Item                 | Value                    |
| -------------------- | ------------------------ |
| Registration Number  | IT24102368               |
| Agent Port           | 9410                     |
| Session ID           | SID:8632                 |
| Authentication Token | OPS-2368                 |
| Agent Source         | agent_368.c              |
| Controller Source    | controller_368.c         |
| Makefile             | Makefile_368             |
| Log File             | remoteops_IT24102368.log |
| Storage Path         | ./agentfiles/IT24102368/ |

### Personalization Calculation

* First four digits of numeric part: `2410`
* Agent port: `7000 + 2410 = 9410`
* Last four digits: `2368`
* Reversed last four digits: `8632`
* Session ID: `SID:8632`
* Authentication token: `OPS-2368`
* Last three digits: `368`

---

## Main Features

RemoteOps implements the following required features:

1. Multiple simultaneous Controller connections using POSIX threads.
2. Authentication using the personalized token.
3. System information monitoring using `SYSINFO`.
4. Running process listing using `LISTPROC`.
5. Restricted remote command execution using `EXEC`.
6. File upload using `PUT`.
7. File download using `GET`.
8. Periodic UDP monitoring using `MONITOR START` and `MONITOR STOP`.
9. Graceful client disconnection using `QUIT`.
10. Timestamped activity logging.

---

## EXEC Command Whitelist

For security, only the following commands are allowed:

* `DATE`
* `UPTIME`
* `DISKFREE`
* `HOSTNAME`
* `WHOAMI`

Other commands are rejected by the Agent.

---

## Project Files

```text
agent_368.c
controller_368.c
Makefile_368
README.md
remoteops_IT24102368.log
agentfiles/
└── IT24102368/
```

### agent_368.c

The Agent/server program. It accepts Controller connections, authenticates clients, processes commands, transfers files, performs UDP monitoring and records activity in the log.

### controller_368.c

The Controller/client program. It connects to the Agent and allows the administrator to perform RemoteOps operations.

### Makefile_368

Build file used to compile the Agent and Controller programs using GCC.

---

## Compilation

Open a terminal in the project directory and run:

```bash
make -f Makefile_368
```

This compiles both programs.

After successful compilation, the following executables are created:

```text
agent_368
controller_368
```

---

## Running the Agent

Start the Agent on the managed machine:

```bash
./agent_368
```

The Agent listens for TCP connections on:

```text
Port: 9410
```

---

## Running the Controller

In another terminal, start the Controller:

```bash
./controller_368
```

The Controller connects to the Agent and provides the available RemoteOps commands.

---

## Authentication

Use the personalized authentication token:

```text
AUTH OPS-2368
```

After successful authentication, the Agent returns:

```text
OK AUTHENTICATED SID:8632
```

The Session ID `SID:8632` is included in TCP responses and UDP monitoring messages.

---

## Storage

Uploaded files are stored under:

```text
./agentfiles/IT24102368/
```

The storage directory is personalized using the full registration number.

---

## Logging

Agent activity is recorded in:

```text
remoteops_IT24102368.log
```

The log contains timestamps for connections, authentication attempts, commands, file transfers, monitoring operations and disconnections.

---

## Communication Protocol

The RemoteOps TCP protocol supports:

```text
AUTH
SYSINFO
LISTPROC
EXEC
PUT
GET
MONITOR START
MONITOR STOP
QUIT
```

TCP text messages use newline-based framing. PUT and GET transfer the specified number of raw file bytes.

UDP monitoring sends periodic system statistics containing:

```text
SYSINFO <cpu_load> <mem_used_mb> <uptime_sec> SID:8632
```

---

## Concurrency

The Agent uses a **thread-per-client** concurrency model.

Each Controller connection is handled by a separate POSIX thread, allowing multiple Controllers to communicate with the Agent simultaneously.

---

## Testing

The implementation was tested for:

* Authentication
* SYSINFO
* LISTPROC
* EXEC allowed commands
* EXEC rejected commands
* PUT file upload
* GET file download
* UDP monitoring START/STOP
* QUIT
* Activity logging
* Multiple client connections

---

## Author

**Registration Number:** IT24102368
**Module:** IE3090 Network Programming
**Assignment:** RemoteOps
