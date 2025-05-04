# Distributed File System (DFS)


## Table of Contents

- [Overview](#overview)
- [System Architecture](#system-architecture)
- [Components](#components)
- [Workflow](#workflow)
- [Installation](#installation)
- [Usage Guide](#usage-guide)
- [Commands in Detail](#commands-in-detail)
- [Implementation Details](#implementation-details)
- [Protocol Specifications](#protocol-specifications)
- [Error Handling](#error-handling)
- [Development and Testing](#development-and-testing)
- [Contributing](#contributing)

## Overview

This Distributed File System (DFS) implements a client-server architecture using socket programming to create a transparent file distribution system. The project demonstrates fundamental concepts of distributed systems, socket programming, file operations, and concurrent processing.

The key features of this system include:

- **Transparent Distribution:** Files are stored on different servers based on file type, but clients interact as if everything is on a single server
- **File Type Segregation:** Files are automatically categorized and stored on specialized servers based on extension
- **Concurrent Client Handling:** Multiple clients can connect and perform operations simultaneously
- **Comprehensive File Operations:** Upload, download, remove, list, and create archives
- **Socket-based Communication:** All inter-server and client-server communication uses TCP sockets

The system implements a pragmatic approach to distributed storage where the complexity of managing multiple storage servers is hidden from the client, providing a seamless user experience while enabling efficient file management.

## System Architecture

The Distributed File System consists of five main components:

1. **Client (w25clients):** Interface for users to interact with the system
2. **Main Server (S1):** Primary entry point that handles client requests and distributes files
3. **PDF Server (S2):** Specialized server for storing and managing PDF files (.pdf)
4. **Text Server (S3):** Specialized server for storing and managing text files (.txt)
5. **ZIP Server (S4):** Specialized server for storing and managing archive files (.zip)

### Architecture Diagram

```
                     ┌───────┐
                     │Client │
                     └───┬───┘
                         │
                         │ (All Client Requests)
                         ▼
┌───────────────────────────────────────────┐
│                                           │
│  ┌───────────────────┐                    │
│  │ Main Server (S1)  │                    │
│  │                   │                    │
│  │ - Handles all     │                    │
│  │   client requests │                    │
│  │ - Stores .c files │                    │
│  │ - Forwards other  │                    │
│  │   files to        │                    │
│  │   specialized     │                    │
│  │   servers         │                    │
│  └─────┬──────┬──────┘                    │
│        │      │      │                    │
│        │      │      │                    │
│        ▼      ▼      ▼                    │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐      │
│  │   S2    │ │   S3    │ │   S4    │      │
│  │         │ │         │ │         │      │
│  │ PDF     │ │ Text    │ │ ZIP     │      │
│  │ Server  │ │ Server  │ │ Server  │      │
│  │ (.pdf)  │ │ (.txt)  │ │ (.zip)  │      │
│  └─────────┘ └─────────┘ └─────────┘      │
│                                           │
└───────────────────────────────────────────┘
           Distributed File System
```

### Data Flow Diagram

```
┌─────────┐          ┌────────┐          ┌────────┐
│         │  Upload  │        │  Store   │        │
│ Client  ├─────────►│   S1   ├─────────►│S2/S3/S4│
│         │          │        │          │        │
└────┬────┘          └────┬───┘          └────────┘
     │                    │
     │  Request           │
     └────────────────────┘
                │
                ▼
┌─────────┐          ┌────────┐          ┌────────┐
│         │ Response │        │ Retrieve │        │
│ Client  │◄─────────┤   S1   │◄─────────┤S2/S3/S4│
│         │          │        │          │        │
└─────────┘          └────────┘          └────────┘
```

## Components

### Client (w25clients.c)

The client component is the user interface to the distributed file system. It connects to the main server (S1) and sends commands for various file operations.

Key features:
- Provides a command-line interface for users
- Validates commands locally before sending to S1
- Handles file upload and download operations
- Manages connection with the main server
- Processes and displays server responses

### Main Server (S1.c)

S1 is the central component of the DFS, acting as the primary interface between clients and the storage servers. It creates an appearance of a unified file system while actually distributing files to specialized servers.

Key responsibilities:
- Accepts and handles connections from multiple clients
- Manages its own storage (~/S1) for .c files
- Forwards files to appropriate specialized servers based on extension
- Retrieves files from specialized servers when clients request them
- Coordinates file operations across the distributed system
- Creates child processes for each client connection to handle requests concurrently

### PDF Server (S2.c)

S2 is a specialized server dedicated to storing and managing PDF files (.pdf). 

Key responsibilities:
- Accepts connections from S1 only
- Stores PDF files in its own storage area (~/S2)
- Handles STORE, RETRIEVE, REMOVE, GETFILES, and MAKETAR commands from S1
- Manages the PDF files directory structure mirroring the original path
- Creates archive files (tar) of PDF files when requested

### Text Server (S3.c)

S3 is responsible for storing and managing text files (.txt) within the system.

Key responsibilities:
- Accepts connections from S1 only
- Stores text files in its own storage area (~/S3)
- Handles the same command set as S2 for text files
- Maintains a directory structure that mirrors the client's view
- Creates archive files (tar) of text files when requested

### ZIP Server (S4.c)

S4 manages the storage and operations for ZIP archive files (.zip).

Key responsibilities:
- Accepts connections from S1 only
- Stores ZIP files in its own storage area (~/S4)
- Handles the same command set as S2/S3 for ZIP files
- Maintains mirrored directory structure as presented to clients
- Manages archive operations for ZIP files

## Workflow

### File Upload Process

1. Client issues an `uploadf` command with a local file and destination path
2. Client validates the command format locally
3. Client connects to S1 and sends the command
4. S1 acknowledges with "READY" signal
5. Client sends file size (uint64_t) followed by file content
6. S1 receives and temporarily stores the file
7. S1 examines the file extension:
   - For `.c` files: Keeps them in ~/S1
   - For `.pdf` files: Transfers to S2 using the STORE command
   - For `.txt` files: Transfers to S3 using the STORE command
   - For `.zip` files: Transfers to S4 using the STORE command
8. When transfer to secondary server is complete, S1 deletes the non-.c file from its storage
9. S1 sends success/error message to client
10. Client displays the result and closes connection

### File Download Process

1. Client issues a `downlf` command with a file path
2. Client validates the command locally
3. Client connects to S1 and sends the command
4. S1 determines the file type based on extension
5. Depending on file type, S1 either:
   - Serves `.c` files directly from its local storage
   - Requests `.pdf` files from S2 using RETRIEVE command
   - Requests `.txt` files from S3 using RETRIEVE command
   - Requests `.zip` files from S4 using RETRIEVE command
6. S1 receives the file from the appropriate server (if not local)
7. S1 sends the file size followed by content to the client
8. Client receives and saves the file locally
9. Client displays success/error message and closes connection

### File Removal Process

1. Client issues a `removef` command with a file path
2. Client validates the command locally
3. Client connects to S1 and sends the command
4. S1 determines where the file is stored based on extension
5. S1 either:
   - Removes local `.c` file directly
   - Sends REMOVE command to S2 for `.pdf` files
   - Sends REMOVE command to S3 for `.txt` files
   - Sends REMOVE command to S4 for `.zip` files
6. S1 receives success/error response from specialized server (if applicable)
7. S1 forwards the response to the client
8. Client displays the result and closes connection

### File Listing Process

1. Client issues a `dispfnames` command with a directory path
2. Client validates the command locally
3. Client connects to S1 and sends the command
4. S1 gathers files from multiple sources:
   - Collects `.c` files from its local storage directory
   - Requests `.pdf` file list from S2 using GETFILES command
   - Requests `.txt` file list from S3 using GETFILES command
   - Requests `.zip` file list from S4 using GETFILES command
5. S1 combines all lists and sorts them alphabetically within each file type
6. S1 sends the consolidated list to the client
7. S1 closes the connection (signaling end of list)
8. Client displays the file list and exits

### Archive Creation Process

1. Client issues a `downltar` command with a file type (`.c`, `.pdf`, or `.txt`)
2. Client validates the command locally
3. Client connects to S1 and sends the command
4. S1 processes the request based on file type:
   - For `.c` files: Creates tar archive from its local storage
   - For `.pdf` files: Requests tar archive from S2 using MAKETAR command
   - For `.txt` files: Requests tar archive from S3 using MAKETAR command
5. S1 receives the tar file if requested from secondary server
6. S1 sends the tar file size followed by content to the client
7. Client receives and saves the tar file locally
8. Client displays success/error message and closes connection

## Installation

### Prerequisites

- Unix/Linux operating system
- GCC compiler
- Basic understanding of socket programming
- Network access between server machines (can be on localhost for testing)

### Compilation

1. Clone the repository:
   ```bash
   git clone https://github.com/Arshnoor-Singh-Sohi/Distributed-File-System.git
   cd Distributed-File-System
   ```

2. Compile each component:
   ```bash
   gcc -o S1 S1.c
   gcc -o S2 S2.c
   gcc -o S3 S3.c
   gcc -o S4 S4.c
   gcc -o w25clients w25clients.c
   ```

### Configuration

By default, the system uses the following ports and IP addresses:

- S1: Port 9555 (configurable via command line)
- S2: Port 8002 (configured to 8000 + SERVER_NUM)
- S3: Port 8003 (configured to 8000 + SERVER_NUM)
- S4: Port 8004 (configured to 8000 + SERVER_NUM)

The client is pre-configured to connect to S1 at 127.0.0.1:9555. If you need to change this, modify the following lines in `w25clients.c`:

```c
#define S1_SERVER_IP_ADDRESS "127.0.0.1"  // Change to S1's IP if not on localhost
#define S1_SERVER_PORT 9555              // Change to match S1's port
```

### Storage Directories

The system uses the following directories for storage:

- S1: `~/S1/`
- S2: `~/S2/`
- S3: `~/S3/`
- S4: `~/S4/`

These directories are automatically created when the servers start.

## Usage Guide

### Starting the System

1. Start the servers in order:

   ```bash
   ./S1 9555  # Start S1 on port 9555
   ./S2 8002  # Start S2 on port 8002
   ./S3 8003  # Start S3 on port 8003
   ./S4 8004  # Start S4 on port 8004
   ```

2. Start the client:

   ```bash
   ./w25clients
   ```

### Client Command Syntax

The client supports the following commands:

1. **Upload a file**:
   ```
   uploadf <local_filename> <~S1/destination_path>
   ```
   Example:
   ```
   uploadf document.pdf ~S1/docs/report.pdf
   ```

2. **Download a file**:
   ```
   downlf <~S1/path/filename>
   ```
   Example:
   ```
   downlf ~S1/docs/report.pdf
   ```

3. **Remove a file**:
   ```
   removef <~S1/path/filename>
   ```
   Example:
   ```
   removef ~S1/docs/report.pdf
   ```

4. **Download a tar archive of files**:
   ```
   downltar <.c | .pdf | .txt>
   ```
   Example:
   ```
   downltar .pdf
   ```

5. **List files in a directory**:
   ```
   dispfnames <~S1/path/>
   ```
   Example:
   ```
   dispfnames ~S1/docs/
   ```

6. **Exit the client**:
   ```
   exit
   ```

## Commands in Detail

### `uploadf`: Upload a File

The `uploadf` command allows users to upload a file from their local system to the distributed file system. The file is automatically routed to the appropriate server based on its extension.

**Syntax:**
```
uploadf <local_filename> <~S1/destination_path>
```

**Parameters:**
- `local_filename`: Path to the file on the client's system
- `destination_path`: Path where the file should be stored in the DFS (must start with ~S1/)

**Supported file types:**
- `.c` files: Stored on S1
- `.pdf` files: Stored on S2
- `.txt` files: Stored on S3
- `.zip` files: Stored on S4

**Protocol steps:**
1. Client sends command to S1
2. S1 responds with "READY"
3. Client sends file size (uint64_t)
4. Client sends file content
5. S1 forwards file to appropriate server if needed
6. S1 sends success/error response

**Example:**
```
w25clients$ uploadf program.c ~S1/projects/program.c
```
This uploads `program.c` to the main server (S1) in the path ~/S1/projects/.

```
w25clients$ uploadf document.pdf ~S1/documents/report.pdf
```
This uploads `document.pdf` to S2 (transparent to the user) in the path ~/S2/documents/.

### `downlf`: Download a File

The `downlf` command allows users to download a file from the distributed file system to their local system. The client requests the file from S1, which retrieves it from the appropriate server.

**Syntax:**
```
downlf <~S1/path/filename>
```

**Parameters:**
- `path/filename`: Path of the file to download (must start with ~S1/)

**Protocol steps:**
1. Client sends command to S1
2. S1 determines file location based on extension
3. S1 retrieves the file from appropriate server if needed
4. S1 sends file size (uint32_t)
5. S1 sends file content
6. Client saves the file locally

**Example:**
```
w25clients$ downlf ~S1/projects/program.c
```
This downloads the file from S1 and saves it as `program.c` in the client's current directory.

```
w25clients$ downlf ~S1/documents/report.pdf
```
This retrieves `report.pdf` from S2 (via S1) and saves it locally.

### `removef`: Remove a File

The `removef` command deletes a file from the distributed file system. It removes the file from the appropriate server based on the file extension.

**Syntax:**
```
removef <~S1/path/filename>
```

**Parameters:**
- `path/filename`: Path of the file to remove (must start with ~S1/)

**Protocol steps:**
1. Client sends command to S1
2. S1 determines file location based on extension
3. S1 removes the file locally or forwards command to appropriate server
4. S1 sends success/error response to client

**Example:**
```
w25clients$ removef ~S1/projects/program.c
```
This removes the C file from S1.

```
w25clients$ removef ~S1/documents/report.pdf
```
This removes the PDF file from S2 (via S1 command).

### `downltar`: Download a Tar Archive

The `downltar` command creates and downloads a tar archive containing all files of a specified type. The command consolidates files from the appropriate server based on the file type.

**Syntax:**
```
downltar <.c | .pdf | .txt>
```

**Parameters:**
- `filetype`: The type of files to include in the archive (`.c`, `.pdf`, or `.txt`)

**Protocol steps:**
1. Client sends command to S1
2. S1 creates archive for `.c` files locally or requests archive from appropriate server
3. S1 sends file size
4. S1 sends archive content
5. Client saves the archive locally

**Example:**
```
w25clients$ downltar .c
```
This creates and downloads an archive (`cfiles.tar`) containing all C files from S1.

```
w25clients$ downltar .pdf
```
This creates and downloads an archive (`pdf.tar`) containing all PDF files from S2.

### `dispfnames`: Display Filenames

The `dispfnames` command lists all files in a specified directory across all servers. It consolidates file lists from each server and presents them in an ordered manner.

**Syntax:**
```
dispfnames <~S1/path/>
```

**Parameters:**
- `path`: Directory path to list files from (must start with ~S1/)

**Protocol steps:**
1. Client sends command to S1
2. S1 gathers file lists from all relevant servers
3. S1 consolidates and sorts the lists
4. S1 sends the combined list to client
5. S1 closes the connection to signal end of list

**Example:**
```
w25clients$ dispfnames ~S1/projects/
```
This lists all files in the ~/S1/projects/ directory across all servers, sorted by type and name.

## Implementation Details

### Socket Communication

All communication in the system uses TCP sockets for reliable, connection-oriented data transfer. 

Key implementation aspects:
- Socket creation with `socket(AF_INET, SOCK_STREAM, 0)`
- Connection establishment with `connect()` (client) and `accept()` (servers)
- Data transfer with `read()` and `write()` system calls
- Connection termination with `close()`

### Concurrency Model

The system uses a process-based concurrency model with the `fork()` system call:

1. Servers listen for incoming connections on a main socket
2. When a connection is accepted, the server forks a child process
3. The child process handles the client request exclusively
4. The parent process returns to listening for new connections
5. The `SIGCHLD` signal handler prevents zombie processes

This model allows multiple clients to be served simultaneously without blocking.

### File Type Routing

S1 routes files to the appropriate server based on file extension:

```c
// Simplified pseudocode from S1.c
if (strcasecmp(file_extension_ptr, ".c") == 0) {
    // Keep file in S1
} else if (strcasecmp(file_extension_ptr, ".pdf") == 0) {
    target_secondary_server_id = 2;  // Route to S2
} else if (strcasecmp(file_extension_ptr, ".txt") == 0) {
    target_secondary_server_id = 3;  // Route to S3
} else if (strcasecmp(file_extension_ptr, ".zip") == 0) {
    target_secondary_server_id = 4;  // Route to S4
}
```

### Path Translation

The system maintains transparent path translation between what clients see and where files are actually stored:

1. Clients reference all paths as `~S1/path/to/file.ext`
2. S1 translates these paths based on file type:
   - `.c` files: `~/S1/path/to/file.c`
   - `.pdf` files: `~/S2/path/to/file.pdf`
   - `.txt` files: `~/S3/path/to/file.txt`
   - `.zip` files: `~/S4/path/to/file.zip`

### Protocol Buffers

The code uses fixed-size buffers for various data types:

- Command buffers: 1024 bytes
- File transfer chunks: 4096 bytes
- Server response buffers: 256 bytes
- File list buffer: 65536 bytes (64KB)

These sizes balance performance and resource usage for most typical file operations.

## Protocol Specifications

### Server-Client Protocols

#### `uploadf` Protocol

```
Client → S1: "uploadf <filename> <~S1/path>"
S1 → Client: "READY"
Client → S1: <uint64_t file_size>
Client → S1: <file_content> (multiple chunks)
S1 → Client: "SUCCESS" or "ERROR:reason"
```

#### `downlf` Protocol

```
Client → S1: "downlf <~S1/path/file>"
S1 → Client: <uint32_t file_size> or "ERROR:reason"
S1 → Client: <file_content> (multiple chunks)
```

#### `removef` Protocol

```
Client → S1: "removef <~S1/path/file>"
S1 → Client: "SUCCESS" or "ERROR:reason"
```

#### `downltar` Protocol

```
Client → S1: "downltar <.filetype>"
S1 → Client: <uint32_t tar_size> or "ERROR:reason"
S1 → Client: <tar_content> (multiple chunks)
```

#### `dispfnames` Protocol

```
Client → S1: "dispfnames <~S1/path/>"
S1 → Client: <file_list> (multiple chunks)
S1: *closes connection*
```

### Inter-Server Protocols

#### `STORE` Protocol (S1 → S2/S3/S4)

```
S1 → Sx: "STORE <filename> <~S1/path> <size>"
Sx → S1: "READY"
S1 → Sx: <file_content> (multiple chunks)
Sx → S1: "SUCCESS" or "ERROR:reason"
```

#### `RETRIEVE` Protocol (S1 → S2/S3/S4)

```
S1 → Sx: "RETRIEVE <~S1/path/file>"
Sx → S1: <uint32_t file_size> or binary 0
Sx → S1: <file_content> (multiple chunks)
```

#### `REMOVE` Protocol (S1 → S2/S3/S4)

```
S1 → Sx: "REMOVE <~S1/path/file>"
Sx → S1: "SUCCESS" or "ERROR:reason"
```

#### `GETFILES` Protocol (S1 → S2/S3/S4)

```
S1 → Sx: "GETFILES <~S1/path/>"
Sx → S1: <file_list> (newline-separated)
Sx: *closes connection*
```

#### `MAKETAR` Protocol (S1 → S2/S3/S4)

```
S1 → Sx: "MAKETAR <.filetype>"
Sx → S1: <size_string> (ASCII digits)
Sx → S1: <tar_content> (if size > 0)
```

## Error Handling

The system implements comprehensive error handling at multiple levels:

### Client-Side Error Handling

1. **Command Validation**:
   - Checks command format before sending to server
   - Validates path prefixes, filenames, and parameter counts
   - Provides specific error messages for invalid commands

2. **Socket Communication Errors**:
   - Handles connection failure to S1
   - Detects read/write errors during data transfer
   - Reports socket errors with appropriate messages

3. **File Operation Errors**:
   - Checks for local file access issues during upload/download
   - Handles file permission problems
   - Manages disk space issues

### Server-Side Error Handling

1. **Network Communication Errors**:
   - Manages connection interruptions and premature disconnects
   - Handles incomplete data transfers
   - Deals with protocol violations

2. **File System Errors**:
   - Checks for file existence before operations
   - Manages directory creation/access issues
   - Handles permission problems

3. **Inter-Server Communication Errors**:
   - Detects and reports failures between servers
   - Manages timeouts and disconnections

### Error Response Format

Errors are reported as strings with an "ERROR:" prefix followed by a specific reason:

```
"ERROR:FILE_NOT_FOUND"
"ERROR:PERMISSION_DENIED"
"ERROR:INVALID_PATH"
"ERROR:STORAGE_FULL"
```

## Development and Testing

### Development Environment

The system was developed in a Unix/Linux environment using C for its system programming capabilities, particularly for socket communication and process management.

Key development tools:
- GCC compiler
- Common Unix development libraries
- Socket programming APIs

### Testing Methodology

To test the system, follow these steps:

1. **Component Testing**:
   - Start each server separately
   - Test basic connectivity between components
   - Verify server initialization and directory creation

2. **Command Testing**:
   - Test each command individually
   - Verify proper handling of valid and invalid inputs
   - Check error cases and edge conditions

3. **Integration Testing**:
   - Run all components together
   - Perform sequences of related operations
   - Test concurrent client connections

4. **Performance Testing**:
   - Test with files of varying sizes
   - Measure response times under load
   - Evaluate system behavior with multiple clients

### Debugging Techniques

The codebase includes extensive debug logging with `printf` statements. To aid in troubleshooting:

1. All debug messages are prefixed with "DEBUG:"
2. Error messages use "ERROR:" or "WARN:" prefixes
3. Process IDs are included in child process logs
4. Socket file descriptors are logged for tracking connections
5. Command parameters are echoed for verification

## Contributing

Contributions to this project are welcome! Here's how you can contribute:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/your-feature`)
3. Commit your changes (`git commit -m 'Add some feature'`)
4. Push to the branch (`git push origin feature/your-feature`)
5. Open a Pull Request

### Coding Guidelines

- Follow the existing code style
- Add comments for complex logic
- Include proper error handling
- Write meaningful commit messages
- Update documentation for new features


## Acknowledgments

- This project was developed as part of Course COMP-8567 (Summer 2025)
- Special thanks to contributors and reviewers for their feedback

---

*This README was last updated on May 4, 2025*
