/* ======================================================== */
/*          S2.c - Secondary PDF Server                     */
/* ======================================================== */
/* This server is part of my Distributed File System project. */
/* It listens for requests specifically from the S1 server. */
/* Its main responsibility is managing .pdf files.          */

// Standard C library includes needed for various functions
#include <stdio.h>      // For input/output: printf, perror, fopen, fread, fwrite, snprintf, etc.
#include <stdlib.h>     // For general utilities: exit, atoi, getenv, system, malloc, free (though malloc not used here yet)
#include <string.h>     // For string manipulation: memset, strncmp, strlen, strcpy, strrchr
#include <unistd.h>     // For POSIX system calls: fork, read, write, close, getpid, unlink (same as remove)
#include <signal.h>     // For signal handling: signal() function and SIGCHLD
#include <sys/socket.h> // For socket programming functions and structures: socket, bind, listen, accept, sockaddr_in
#include <sys/types.h>  // For fundamental system data types like pid_t, size_t, ssize_t
#include <netinet/in.h> // For internet address structures (sockaddr_in) and functions (htons, htonl)
#include <arpa/inet.h>  // For functions like inet_pton (IP address conversion)
#include <libgen.h>     // For basename() function (useful for extracting filenames from paths)
#include <sys/stat.h>   // For file status functions: stat() and checking file types (S_ISDIR)
#include <errno.h>      // For checking the errno variable after system calls fail
#include <ctype.h>      // For character type checking, like isdigit()
#include <sys/wait.h>

/* --- Server Identity --- */
// This #define MUST be set correctly for each secondary server.
// 2 for PDF server (S2)
// 3 for TXT server (S3)
// 4 for ZIP server (S4)
#define SERVER_NUM 2 // This is server S2

// Define a buffer size for reading/writing file chunks
#define FILE_CHUNK_BUFFER_SIZE 4096 // 4KB buffer for file transfers

/* --- Function Prototypes --- */
// This function handles all the logic for a single connection from S1.
void process_s1_connection(int s1_socket_descriptor);

// These functions handle specific commands received from S1.
void handle_store_command(int s1_socket_fd, const char *command_params);
void handle_getfiles_command(int s1_socket_fd, const char *command_params);
void handle_retrieve_command(int s1_socket_fd, const char *command_params);
void handle_remove_command(int s1_socket_fd, const char *command_params);
void handle_maketar_command(int s1_socket_fd, const char *command_params);

// Signal handler function to reap terminated child processes
void sigchld_handler(int signal_number)
{
    // Use waitpid() in a loop to reap ALL terminated children non-blockingly
    // WNOHANG ensures the call doesn't block if no children have exited
    // Looping handles the case where multiple children might exit around the same time
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    // We don't need to re-install the handler on most modern systems
}

/* ======================================================== */
/*                 Main Function (Entry Point)              */
/* ======================================================== */
int main(int argc, char *argv[])
{
    // argc: number of command-line arguments
    // argv: array of command-line argument strings (argv[0] is program name)
    printf("DEBUG: S%d Server starting up...\n", SERVER_NUM);
    printf("DEBUG: Command line args count: %d\n", argc);

    /* --- Argument Validation --- */
    // We need exactly one argument: the port number.
    // argc should be 2: ./S<N> <port>
    int required_arg_count = 2; // Making it a variable for clarity
    if (argc != required_arg_count)
    {
        // Print an error message to standard error explaining correct usage.
        fprintf(stderr, "[S%d_MAIN_ERROR] Incorrect number of arguments provided.\n", SERVER_NUM);
        fprintf(stderr, "[S%d_MAIN_INFO] Usage: %s <port_number>\n", SERVER_NUM, argv[0]);
        // Exit the program indicating an error.
        exit(EXIT_FAILURE); // Use EXIT_FAILURE for portability
    }
    int server_port_number_arg = atoi(argv[1]); // Convert port argument string to integer
    printf("DEBUG: Port number specified: %d\n", server_port_number_arg);

    /* --- Prepare Storage Directory --- */
    // This server needs its own directory (e.g., ~/S2) to store files.
    char server_storage_dir_path[128]; // Buffer for directory path string
    snprintf(server_storage_dir_path, sizeof(server_storage_dir_path), "~/S%d", SERVER_NUM);

    // Commands to ensure the directory exists and is initially empty (or just exists)
    char sys_cmd_remove_file[256];    // Buffer for system command
    char sys_cmd_make_directory[256]; // Buffer for system command
    // Construct commands safely using snprintf
    snprintf(sys_cmd_remove_file, sizeof(sys_cmd_remove_file), "rm -f %s", server_storage_dir_path);
    snprintf(sys_cmd_make_directory, sizeof(sys_cmd_make_directory), "mkdir -p %s", server_storage_dir_path);

    printf("DEBUG: Ensuring storage directory '%s' exists...\n", server_storage_dir_path);
    // Execute 'rm -f' first to remove any file that might exist with the same name.
    int rm_exit_status = system(sys_cmd_remove_file);
    if (rm_exit_status != 0)
    {
        // This might be okay if the file simply didn't exist. Just log it.
        printf("DEBUG: Command '%s' exited with status %d.\n", sys_cmd_remove_file, rm_exit_status);
    }
    // Execute 'mkdir -p' to create the directory. '-p' ensures parent dirs are created
    // and it doesn't fail if the directory already exists.
    int mkdir_exit_status = system(sys_cmd_make_directory);
    if (mkdir_exit_status != 0)
    {
        // This is potentially more serious. Log a warning. File operations might fail later.
        fprintf(stderr, "[S%d_MAIN_WARN] Command '%s' failed with status %d. Check permissions?\n",
                SERVER_NUM, sys_cmd_make_directory, mkdir_exit_status);
    }
    else
    {
        printf("DEBUG: Storage directory '%s' should now be ready.\n", server_storage_dir_path);
    }

    /* --- Socket Setup Variables --- */
    int main_listener_socket_fd;              // File descriptor for the main listening socket
    int s1_connection_socket_fd;              // File descriptor for a connection accepted from S1
    struct sockaddr_in server_net_address;    // Structure for this server's network address
    struct sockaddr_in s1_client_net_address; // Structure for the connected S1's network address
    socklen_t s1_client_address_length;       // Variable to store the size of the S1 address structure

    /* --- Signal Handling --- */
    // Handle SIGCHLD to prevent zombie processes when child processes exit.
    // SIG_IGN tells the system not to wait for the child, effectively auto-reaping it.
    signal(SIGCHLD, sigchld_handler); // Register our custom handler
    printf("DEBUG: SIGCHLD handler configured to SIG_IGN.\n");

    /* --- Create Listening Socket --- */
    printf("DEBUG: Creating main listening socket (IPv4, TCP)...\n");
    // AF_INET: Address Family - Internet (IPv4)
    // SOCK_STREAM: Socket Type - TCP (reliable, connection-oriented)
    // 0: Protocol - System chooses appropriate protocol (TCP for SOCK_STREAM)
    main_listener_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    // socket() returns -1 on failure.
    if (main_listener_socket_fd < 0)
    {
        // perror() prints a system error message based on the 'errno' variable.
        perror("[S%d_MAIN_FATAL] Socket creation failed");
        // Exit immediately if socket creation fails.
        exit(EXIT_FAILURE);
    }
    printf("DEBUG: Listening socket created successfully (fd=%d).\n", main_listener_socket_fd);

    /* --- Set Socket Options (SO_REUSEADDR) --- */
    // Allows the server to restart quickly and reuse the port address, even if it's in TIME_WAIT state.
    // Very helpful during development and testing.
    int reuse_addr_option = 1; // Option value (1 means enable)
    printf("DEBUG: Setting SO_REUSEADDR socket option...\n");
    if (setsockopt(main_listener_socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr_option, sizeof(reuse_addr_option)) < 0)
    {
        // This isn't usually fatal, so just print a warning.
        perror("[S%d_MAIN_WARN] setsockopt(SO_REUSEADDR) failed");
    }
    else
    {
        printf("DEBUG: SO_REUSEADDR option enabled.\n");
    }

    /* --- Configure Server Network Address --- */
    printf("DEBUG: Configuring server network address...\n");
    // Zero out the structure to avoid garbage data.
    memset(&server_net_address, 0, sizeof(server_net_address));
    // Set address family to IPv4.
    server_net_address.sin_family = AF_INET;
    // Set the IP address. INADDR_ANY means listen on all available network interfaces.
    // htonl converts the value to network byte order (Big Endian).
    server_net_address.sin_addr.s_addr = htonl(INADDR_ANY);
    // Set the port number provided via command line.
    // htons converts the port number to network byte order.
    server_net_address.sin_port = htons(server_port_number_arg);
    printf("DEBUG: Server configured to listen on port %d (Network Byte Order: %d).\n",
           server_port_number_arg, server_net_address.sin_port);

    /* --- Bind Socket to Address --- */
    // Associates the created socket with the configured network address and port.
    printf("DEBUG: Binding socket (fd=%d) to port %d...\n", main_listener_socket_fd, server_port_number_arg);
    if (bind(main_listener_socket_fd, (struct sockaddr *)&server_net_address, sizeof(server_net_address)) < 0)
    {
        // bind() fails if the port is already in use or other permission issues.
        perror("[S%d_MAIN_FATAL] Binding failed");
        close(main_listener_socket_fd); // Close the socket fd before exiting.
        exit(EXIT_FAILURE);
    }
    printf("DEBUG: Socket bound successfully.\n");

    /* --- Start Listening for Connections --- */
    // Marks the socket as passive, ready to accept incoming connection requests.
    // The second argument (5) is the backlog: the maximum queue length for pending connections.
    printf("DEBUG: Setting socket to listen mode (backlog=5)...\n");
    if (listen(main_listener_socket_fd, 5) < 0)
    {
        // listen() can fail due to various system issues.
        perror("[S%d_MAIN_FATAL] Listen failed");
        close(main_listener_socket_fd); // Close the socket fd before exiting.
        exit(EXIT_FAILURE);
    }

    // Server initialization complete and ready to accept connections.
    printf("[S%d_MAIN_INFO] Server S%d operational on port %d.\n", SERVER_NUM, SERVER_NUM, server_port_number_arg);

    /* --- Main Server Accept Loop --- */
    int server_is_running = 1; // Redundant flag for loop control
    printf("DEBUG: Entering main accept loop to wait for S1 connections...\n");
    // This loop runs forever, accepting and handling connections.
    while (server_is_running) // Use flag instead of `while(1)`
    {
        // Prepare for accept() call.
        s1_client_address_length = sizeof(s1_client_net_address);
        printf("DEBUG: Waiting to accept a new connection from S1...\n");

        // accept() blocks (waits) until a connection request arrives from S1.
        // When a connection is made, it creates a *new* socket specifically for
        // communicating with that S1 instance and returns its file descriptor.
        // It also fills in the s1_client_net_address structure.
        s1_connection_socket_fd = accept(main_listener_socket_fd,
                                         (struct sockaddr *)&s1_client_net_address,
                                         &s1_client_address_length);

        // Check if accept() failed.
        if (s1_connection_socket_fd < 0)
        {
            // Accept can fail (e.g., if interrupted by a signal).
            // Log the error but continue the loop to wait for the next connection.
            perror("[S%d_MAIN_WARN] accept() failed");
            continue; // Go to the next iteration of the while loop.
        }

        // Connection accepted successfully!
        printf("[S%d_MAIN_INFO] Connection accepted from S1 (Client Socket FD: %d)\n", SERVER_NUM, s1_connection_socket_fd);

        /* --- Fork Child Process --- */
        // Create a new child process to handle this S1 connection concurrently.
        // The parent process will continue listening for new connections.
        printf("DEBUG: Forking child process to handle S1 request...\n");
        pid_t child_pid = fork(); // fork() returns PID of child to parent, 0 to child, -1 on error.

        // Check the result of fork().
        if (child_pid < 0)
        {
            // Fork failed (e.g., system resource limits reached).
            perror("[S%d_MAIN_ERROR] Fork failed");
            // Close the socket we just accepted, as no child will handle it.
            close(s1_connection_socket_fd);
            // The parent continues running, trying to accept new connections.
        }
        else if (child_pid == 0)
        {
            /* --- Child Process Logic --- */
            // This block is executed ONLY by the child process.
            printf("DEBUG (PID %d): Child process started for S1 connection fd %d.\n", getpid(), s1_connection_socket_fd);

            // The child process does NOT need the main listening socket. Close it.
            printf("DEBUG (PID %d): Child closing listening socket fd %d.\n", getpid(), main_listener_socket_fd);
            close(main_listener_socket_fd);

            // Call the function that handles the entire interaction with this S1 connection.
            process_s1_connection(s1_connection_socket_fd);

            // After the handler function finishes, the child's job is done.
            printf("DEBUG (PID %d): Child process finished request handling. Exiting.\n", getpid());
            // Close the connection socket before exiting (though exit() usually does this).
            close(s1_connection_socket_fd);
            // Exit the child process with a success status.
            exit(EXIT_SUCCESS);
        }
        else // child_pid > 0
        {
            /* --- Parent Process Logic --- */
            // This block is executed ONLY by the parent process after a successful fork.
            printf("DEBUG (PID %d): Parent created child process (PID %d) for S1 fd %d.\n", getpid(), child_pid, s1_connection_socket_fd);

            // The parent process does NOT need the socket connected to this specific S1 instance.
            // The child process is handling it. Close the parent's copy of the descriptor.
            printf("DEBUG (PID %d): Parent closing connected S1 socket fd %d.\n", getpid(), s1_connection_socket_fd);
            close(s1_connection_socket_fd);
            // The parent immediately loops back to the top of the 'while' loop
            // to call accept() again, waiting for the *next* S1 connection.
        }
    } // End of main server accept loop (while)

    // This part of main is typically unreachable in an infinite server loop.
    printf("[S%d_MAIN_INFO] Server loop exited (should not happen normally). Closing listener socket.\n", SERVER_NUM);
    close(main_listener_socket_fd); // Close the main listening socket.
    return 0;                       // Indicate successful termination (if loop somehow ends).

} // End of main function

/* ======================================================== */
/*          S1 Connection Handling Function                 */
/* ======================================================== */
/**
 * @brief Handles the communication with a single connected S1 instance.
 * Runs inside the child process created by fork(). Reads a single command
 * from S1, processes it by calling the appropriate handler, and then exits.
 *
 * @param s1_socket_descriptor The file descriptor for the connected S1 socket.
 */
void process_s1_connection(int s1_socket_descriptor)
{
    char s1_command_full_buffer[1024]; // Buffer to hold the command from S1
    ssize_t bytes_read_cmd;            // Result of the read() call
    int processing_status = 1;         // Redundant flag

    printf("DEBUG (PID %d): process_s1_connection started for fd %d.\n", getpid(), s1_socket_descriptor);

    // --- Read the command from S1 ---
    // We expect S1 to send one command and then potentially data.
    printf("DEBUG (PID %d): Waiting to read command from S1...\n", getpid());
    memset(s1_command_full_buffer, 0, sizeof(s1_command_full_buffer));                                       // Clear buffer first
    bytes_read_cmd = read(s1_socket_descriptor, s1_command_full_buffer, sizeof(s1_command_full_buffer) - 1); // Leave space for '\0'

    // Check read result
    if (bytes_read_cmd < 0)
    {
        perror("[S%d_CHILD_ERROR] Failed to read command from S1");
        processing_status = 0; // Mark failure
        // Fall through to exit (socket will be closed by exit or caller)
    }
    else if (bytes_read_cmd == 0)
    {
        fprintf(stderr, "[S%d_CHILD_WARN] S1 closed connection before sending command.\n", SERVER_NUM);
        processing_status = 0; // Mark failure
        // Fall through to exit
    }
    else // Read successful
    {
        // Null-terminate the received command string
        s1_command_full_buffer[bytes_read_cmd] = '\0';
        printf("DEBUG (PID %d): Received command from S1: \"%s\"\n", getpid(), s1_command_full_buffer);

        // --- Parse and Dispatch Command ---
        // Find the first space to separate command verb from parameters
        char *command_verb = s1_command_full_buffer;
        char *command_parameters = strchr(s1_command_full_buffer, ' ');
        if (command_parameters != NULL)
        {
            *command_parameters = '\0'; // Null-terminate the verb
            command_parameters++;       // Move pointer past the space to the start of params
        }
        else
        {
            // If no space, parameters are empty (might be okay for some commands?)
            command_parameters = ""; // Point to an empty string
        }

        printf("DEBUG (PID %d): Parsed Verb: '%s', Params: '%s'\n", getpid(), command_verb, command_parameters);

        // Call the appropriate handler based on the command verb
        if (strcmp(command_verb, "STORE") == 0)
        {
            handle_store_command(s1_socket_descriptor, command_parameters);
        }
        else if (strcmp(command_verb, "GETFILES") == 0)
        {
            handle_getfiles_command(s1_socket_descriptor, command_parameters);
        }
        else if (strcmp(command_verb, "RETRIEVE") == 0)
        {
            handle_retrieve_command(s1_socket_descriptor, command_parameters);
        }
        else if (strcmp(command_verb, "REMOVE") == 0)
        {
            handle_remove_command(s1_socket_descriptor, command_parameters);
        }
        else if (strcmp(command_verb, "MAKETAR") == 0)
        {
            handle_maketar_command(s1_socket_descriptor, command_parameters);
        }
        else
        {
            // Unknown command received
            fprintf(stderr, "[S%d_CHILD_ERROR] Received unknown command from S1: '%s'\n", SERVER_NUM, command_verb);
            const char *unknown_cmd_err = "ERROR:UNKNOWN_COMMAND";
            // Try to send error back to S1
            ssize_t write_err_check = write(s1_socket_descriptor, unknown_cmd_err, strlen(unknown_cmd_err));
            if (write_err_check < 0)
            {
                perror("[S%d_CHILD_WARN] Failed sending UNKNOWN_COMMAND error to S1");
            }
            processing_status = 0; // Mark failure
        }
    }

    // This function implicitly returns after handling one command.
    // The child process will then exit in the main loop's child block.
    printf("DEBUG (PID %d): process_s1_connection finished (Status: %d).\n", getpid(), processing_status);

} // End of process_s1_connection

/* ======================================================== */
/*             Command Handling Functions                   */
/* ======================================================== */

/**
 * @brief Handles the 'STORE' command from S1.
 * Receives a file and saves it locally in the appropriate S2 directory.
 * Protocol:
 * 1. S1 sends "STORE <filename> <~S1/dest_path> <size>"
 * 2. S2 creates directory if needed.
 * 3. S2 sends "READY".
 * 4. S1 sends <size> bytes of file content.
 * 5. S2 reads content and saves to file.
 * 6. S2 sends "SUCCESS" or "ERROR:<reason>".
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "STORE" (filename, dest_path, size).
 */
void handle_store_command(int s1_socket_fd, const char *command_params)
{
    char received_filename[256];
    char s1_dest_path[256];
    unsigned long expected_file_size_ul = 0;
    int parse_count = 0;
    char *server_home_path_ptr = NULL;
    char local_s2_full_filepath[512];
    char local_s2_directory_path[512];
    char mkdir_system_command[1024];
    FILE *output_file_stream = NULL;
    const char *final_status_response = "ERROR:INTERNAL"; // Default error
    int operation_failed = 0;                             // 0=OK, 1=Failed

    printf("DEBUG (PID %d) STORE: Handling STORE command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse Parameters */
    printf("DEBUG (PID %d) STORE: Parsing parameters...\n", getpid());
    parse_count = sscanf(command_params, "%255s %255s %lu", received_filename, s1_dest_path, &expected_file_size_ul);
    if (parse_count != 3)
    {
        fprintf(stderr, "[S%d_STORE_ERROR] Invalid STORE parameters. Expected 3, got %d.\n", SERVER_NUM, parse_count);
        final_status_response = "ERROR:BAD_PARAMS";
        operation_failed = 1;
        goto send_store_response; // Use goto for cleanup/response
    }
    size_t expected_file_size = (size_t)expected_file_size_ul; // Cast to size_t
    printf("DEBUG (PID %d) STORE: Parsed OK: File='%s', Dest='%s', Size=%zu\n",
           getpid(), received_filename, s1_dest_path, expected_file_size);

    /* 2. Get Home Directory */
    printf("DEBUG (PID %d) STORE: Getting HOME directory...\n", getpid());
    server_home_path_ptr = getenv("HOME");
    if (server_home_path_ptr == NULL)
    {
        perror("[S%d_STORE_ERROR] getenv(\"HOME\") failed");
        final_status_response = "ERROR:NO_HOME_DIR";
        operation_failed = 1;
        goto send_store_response;
    }

    /* 3. Construct Local S2 Paths */
    printf("DEBUG (PID %d) STORE: Constructing local S%d paths...\n", getpid(), SERVER_NUM);
    // Expect path like "~S1/folder/file.pdf" from S1
    if (strncmp(s1_dest_path, "~S1/", 4) != 0)
    {
        fprintf(stderr, "[S%d_STORE_ERROR] Received destination path missing ~S1/ prefix: %s\n", SERVER_NUM, s1_dest_path);
        final_status_response = "ERROR:INVALID_DEST_PATH_PREFIX";
        operation_failed = 1;
        goto send_store_response;
    }
    char *relative_path = s1_dest_path + 4; // Point past "~S1/"
    char *last_slash_in_relative = strrchr(relative_path, '/');

    // Determine directory and full path for the *local S2* filesystem
    if (last_slash_in_relative == NULL || last_slash_in_relative == relative_path)
    {
        // Path is like "file.pdf" or "/file.pdf", relative to ~S1/
        // Directory to create is ~/S<N>
        snprintf(local_s2_directory_path, sizeof(local_s2_directory_path), "%s/S%d", server_home_path_ptr, SERVER_NUM);
        // Full path is ~/S<N>/file.pdf
        snprintf(local_s2_full_filepath, sizeof(local_s2_full_filepath), "%s/S%d/%s", server_home_path_ptr, SERVER_NUM, relative_path);
    }
    else
    {
        // Path is like "folder/file.pdf"
        // Directory is ~/S<N>/folder
        snprintf(local_s2_directory_path, sizeof(local_s2_directory_path), "%s/S%d/%.*s",
                 server_home_path_ptr, SERVER_NUM, (int)(last_slash_in_relative - relative_path), relative_path);
        // Full path is ~/S<N>/folder/file.pdf
        snprintf(local_s2_full_filepath, sizeof(local_s2_full_filepath), "%s/S%d/%s", server_home_path_ptr, SERVER_NUM, relative_path);
    }
    printf("DEBUG (PID %d) STORE: Local Dir Path: '%s'\n", getpid(), local_s2_directory_path);
    printf("DEBUG (PID %d) STORE: Local File Path: '%s'\n", getpid(), local_s2_full_filepath);

    /* 4. Create Directory Structure */
    snprintf(mkdir_system_command, sizeof(mkdir_system_command), "mkdir -p \"%s\"", local_s2_directory_path);
    printf("DEBUG (PID %d) STORE: Executing: %s\n", getpid(), mkdir_system_command);
    int mkdir_exit_code = system(mkdir_system_command);
    if (mkdir_exit_code != 0)
    {
        // Log warning, but proceed. fopen will catch the error if dir isn't writable.
        fprintf(stderr, "[S%d_STORE_WARN] system('%s') returned status %d.\n", SERVER_NUM, mkdir_system_command, mkdir_exit_code);
    }
    else
    {
        printf("DEBUG (PID %d) STORE: Directory creation command finished.\n", getpid());
    }

    /* 5. Send "READY" signal to S1 */
    const char *ready_signal = "READY";
    printf("DEBUG (PID %d) STORE: Sending '%s' signal to S1...\n", getpid(), ready_signal);
    ssize_t write_ready_check = write(s1_socket_fd, ready_signal, strlen(ready_signal));
    if (write_ready_check < (ssize_t)strlen(ready_signal))
    {
        perror("[S%d_STORE_ERROR] Failed to send READY signal to S1");
        operation_failed = 1;
        final_status_response = "ERROR:WRITE_READY_FAILED"; // Set specific error
        // Cannot send final response if write fails, just cleanup and exit child
        goto cleanup_store_resources;
    }
    printf("DEBUG (PID %d) STORE: READY signal sent.\n", getpid());

    /* 6. Open Local File for Writing */
    printf("DEBUG (PID %d) STORE: Opening local file '%s' for writing...\n", getpid(), local_s2_full_filepath);
    output_file_stream = fopen(local_s2_full_filepath, "wb"); // Binary write mode
    if (output_file_stream == NULL)
    {
        // *** CRITICAL: If fopen fails, DO NOT send error yet! ***
        // We *must* still read the file data S1 is about to send to keep the protocol synchronized.
        // We will read and discard the data, then send an appropriate error response at the end.
        perror("[S%d_STORE_ERROR] fopen failed for local file");
        fprintf(stderr, "[S%d_STORE_ERROR] Failed opening '%s'. Will read and discard data from S1.\n", SERVER_NUM, local_s2_full_filepath);
        // Let operation_failed remain 0 for now, will be set based on NULL check later.
    }
    else
    {
        printf("DEBUG (PID %d) STORE: Local file opened successfully for writing.\n", getpid());
    }

    /* 7. Receive File Data from S1 */
    char file_data_chunk_buffer[FILE_CHUNK_BUFFER_SIZE];
    size_t total_bytes_received = 0;
    int local_write_error_occurred = 0; // Flag: 1 if fwrite fails

    printf("DEBUG (PID %d) STORE: Starting loop to receive %zu bytes from S1...\n", getpid(), expected_file_size);
    // Loop until expected size is received or error occurs
    while (total_bytes_received < expected_file_size)
    {
        // Calculate how many bytes to read in this chunk
        size_t bytes_to_read_now = FILE_CHUNK_BUFFER_SIZE;
        if (expected_file_size - total_bytes_received < bytes_to_read_now)
        {
            bytes_to_read_now = expected_file_size - total_bytes_received;
        }

        // Read data chunk sent by S1
        ssize_t bytes_read_this_chunk = read(s1_socket_fd, file_data_chunk_buffer, bytes_to_read_now);

        // Check read result
        if (bytes_read_this_chunk < 0)
        { // Read error
            perror("[S%d_STORE_ERROR] read() failed while receiving data from S1");
            operation_failed = 1;
            final_status_response = "ERROR:READ_FAILED";
            goto cleanup_store_resources; // Jump to cleanup
        }
        if (bytes_read_this_chunk == 0)
        { // S1 disconnected prematurely
            fprintf(stderr, "[S%d_STORE_ERROR] S1 closed connection unexpectedly (received %zu/%zu bytes)\n",
                    SERVER_NUM, total_bytes_received, expected_file_size);
            operation_failed = 1;
            final_status_response = "ERROR:S1_DISCONNECT";
            goto cleanup_store_resources; // Jump to cleanup
        }

        // --- Write data to local file (if opened successfully and no prior write error) ---
        if (output_file_stream != NULL && local_write_error_occurred == 0)
        {
            size_t bytes_written_to_file = fwrite(file_data_chunk_buffer, 1, bytes_read_this_chunk, output_file_stream);
            // Check if fwrite wrote the correct number of bytes
            if (bytes_written_to_file != (size_t)bytes_read_this_chunk)
            {
                perror("[S%d_STORE_ERROR] fwrite() failed while writing to local file");
                local_write_error_occurred = 1; // Set flag
                // IMPORTANT: Continue reading data from S1 to maintain sync!
                // The overall operation will be marked as failed later.
            }
        }

        // Add received bytes to total count
        total_bytes_received += bytes_read_this_chunk;

    } // End of while loop receiving data

    printf("DEBUG (PID %d) STORE: Finished receiving loop. Total received: %zu bytes\n", getpid(), total_bytes_received);

    /* 8. Determine Final Status and Prepare Response */
    if (operation_failed == 0)
    { // Only check details if no read/connect error occurred yet
        if (output_file_stream == NULL)
        { // fopen had failed earlier
            operation_failed = 1;
            final_status_response = "ERROR:FOPEN_FAILED";
            printf("[S%d_STORE_ERROR] Final status: fopen had failed.\n", SERVER_NUM);
        }
        else if (local_write_error_occurred == 1)
        { // fwrite had failed
            operation_failed = 1;
            final_status_response = "ERROR:FWRITE_FAILED";
            printf("[S%d_STORE_ERROR] Final status: fwrite had failed.\n", SERVER_NUM);
            // Cleanup partial file happens in cleanup section
        }
        else if (total_bytes_received != expected_file_size)
        { // Received size mismatch
            operation_failed = 1;
            final_status_response = "ERROR:INCOMPLETE_TRANSFER";
            fprintf(stderr, "[S%d_STORE_ERROR] Final status: Size mismatch (Expected %zu, Got %zu).\n",
                    SERVER_NUM, expected_file_size, total_bytes_received);
            // Cleanup partial file happens in cleanup section
        }
        else
        {
            // All checks passed - SUCCESS!
            operation_failed = 0; // Explicitly set success state
            final_status_response = "SUCCESS";
            printf("DEBUG (PID %d) STORE: File received and saved successfully.\n", getpid());
            // Close the successfully written file now
            if (fclose(output_file_stream) != 0)
            {
                perror("[S%d_STORE_WARN] fclose failed after successful write");
                // Still report SUCCESS to S1
            }
            output_file_stream = NULL; // Mark as closed to prevent double close
        }
    }
    // If operation_failed was already 1 due to read error, final_status_response is already set.

cleanup_store_resources:
    // Cleanup potentially opened file if an error occurred before successful close
    if (output_file_stream != NULL)
    {
        fclose(output_file_stream);
        output_file_stream = NULL; // Avoid double close
        // If the operation failed, attempt to remove the partial/corrupt file
        if (operation_failed == 1 && local_s2_full_filepath[0] != '\0')
        {
            printf("DEBUG (PID %d) STORE: Operation failed. Removing partial file '%s'\n", getpid(), local_s2_full_filepath);
            if (remove(local_s2_full_filepath) != 0 && errno != ENOENT)
            {
                perror("[S%d_STORE_WARN] Failed to remove partial file during cleanup");
            }
        }
    }

send_store_response:
    // Send the final calculated response string to S1 (unless write failed earlier)
    if (final_status_response != NULL && strcmp(final_status_response, "ERROR:WRITE_READY_FAILED") != 0)
    {
        printf("DEBUG (PID %d) STORE: Sending final response to S1: '%s'\n", getpid(), final_status_response);
        ssize_t write_final_check = write(s1_socket_fd, final_status_response, strlen(final_status_response));
        if (write_final_check < 0)
        {
            perror("[S%d_STORE_ERROR] Failed to send final response to S1");
        }
    }
    else if (final_status_response == NULL)
    {
        fprintf(stderr, "[S%d_STORE_ERROR] Internal error: final_status_response is NULL!\n", SERVER_NUM);
    }

    printf("DEBUG (PID %d) STORE: Exiting handle_store_command.\n", getpid());

} // End handle_store_command

/**
 * @brief Handles the 'GETFILES' command from S1.
 * Finds PDF files in the specified directory, sorts them, and sends the list back.
 * Protocol:
 * 1. S1 sends "GETFILES <~S1/pathname>"
 * 2. S2 finds matching PDF files locally (e.g., in ~/S2/pathname).
 * 3. S2 sends the sorted, newline-separated list of filenames (basenames only).
 * 4. S2 closes the connection to signal the end of the list.
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "GETFILES" (the pathname).
 */
void handle_getfiles_command(int s1_socket_fd, const char *command_params)
{
    char s1_pathname_arg[256];
    char local_s2_dir_path[512];
    char *server_home_dir_env = NULL;
    char find_system_command[1024];
    char temp_list_filepath[256]; // Path to store find results temporarily
    FILE *temp_list_file_stream = NULL;
    int getfiles_op_status = 1; // 1 = OK, 0 = Fail

    printf("DEBUG (PID %d) GETFILES: Handling GETFILES command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse Path Parameter */
    if (sscanf(command_params, "%255s", s1_pathname_arg) != 1)
    {
        fprintf(stderr, "[S%d_GETFILES_ERROR] Invalid GETFILES parameters.\n", SERVER_NUM);
        // S1 expects connection close on error here, no specific error message sent back.
        getfiles_op_status = 0;
        goto cleanup_getfiles; // Use goto for cleanup
    }
    printf("DEBUG (PID %d) GETFILES: Parsed Pathname: '%s'\n", getpid(), s1_pathname_arg);

    /* 2. Get Home Directory */
    server_home_dir_env = getenv("HOME");
    if (server_home_dir_env == NULL)
    {
        perror("[S%d_GETFILES_ERROR] getenv(\"HOME\") failed");
        getfiles_op_status = 0;
        goto cleanup_getfiles;
    }

    /* 3. Construct Local S2 Directory Path */
    // Expect path like "~S1/folder/" from S1
    if (strncmp(s1_pathname_arg, "~S1/", 4) != 0)
    {
        fprintf(stderr, "[S%d_GETFILES_ERROR] Pathname missing ~S1/ prefix: %s\n", SERVER_NUM, s1_pathname_arg);
        getfiles_op_status = 0;
        goto cleanup_getfiles;
    }
    // Construct path like /home/user/S2/folder/
    snprintf(local_s2_dir_path, sizeof(local_s2_dir_path), "%s/S%d/%s", server_home_dir_env, SERVER_NUM, s1_pathname_arg + 4);
    // Remove trailing slash if present for 'find' consistency
    size_t path_len = strlen(local_s2_dir_path);
    if (path_len > 1 && local_s2_dir_path[path_len - 1] == '/')
    {
        local_s2_dir_path[path_len - 1] = '\0';
    }
    printf("DEBUG (PID %d) GETFILES: Local directory path to search: '%s'\n", getpid(), local_s2_dir_path);

    /* 4. Check if Local Directory Exists */
    struct stat directory_status_info;
    if (stat(local_s2_dir_path, &directory_status_info) != 0 || !S_ISDIR(directory_status_info.st_mode))
    {
        fprintf(stderr, "[S%d_GETFILES_WARN] Directory not found or not a directory: '%s'. Sending empty list.\n", SERVER_NUM, local_s2_dir_path);
        // If dir doesn't exist, treat as empty list. S1 expects connection close.
        getfiles_op_status = 1; // Operation considered "successful" (returned empty list)
        goto cleanup_getfiles;
    }

    /* 5. Prepare Temp File Path */
    // Use /tmp if possible for temporary storage
    snprintf(temp_list_filepath, sizeof(temp_list_filepath), "/tmp/s%d_files_%d.txt", SERVER_NUM, getpid());
    printf("DEBUG (PID %d) GETFILES: Using temp file: '%s'\n", getpid(), temp_list_filepath);
    // Clean up any leftover temp file from previous runs
    remove(temp_list_filepath);

    /* 6. Execute `find` Command */
    // Use find to get only basenames (-printf "%f\\n"), sorted, output to temp file.
    // Redirect stderr (2>/dev/null) to suppress find errors (e.g., permission denied on subdirs).
    snprintf(find_system_command, sizeof(find_system_command),
             "find \"%s\" -maxdepth 1 -type f -name \"*.pdf\" -printf \"%%f\\n\" 2>/dev/null | sort > \"%s\"",
             local_s2_dir_path, temp_list_filepath);
    printf("DEBUG (PID %d) GETFILES: Executing: %s\n", getpid(), find_system_command);
    int find_exit_code = system(find_system_command);
    if (find_exit_code != 0)
    {
        fprintf(stderr, "[S%d_GETFILES_WARN] Find/sort command returned status %d.\n", SERVER_NUM, find_exit_code);
        // Proceed anyway, if file exists but is empty, that's fine.
    }

    /* 7. Read Temp File and Send Content to S1 */
    printf("DEBUG (PID %d) GETFILES: Reading results from '%s' and sending to S1...\n", getpid(), temp_list_filepath);
    temp_list_file_stream = fopen(temp_list_filepath, "r");
    if (temp_list_file_stream == NULL)
    {
        // If temp file doesn't exist (e.g., find failed or found nothing), send empty list.
        perror("[S%d_GETFILES_INFO] Failed to open temp file list (maybe no files found?)");
        printf("[S%d_GETFILES_INFO] Sending empty list indication (closing connection).\n", SERVER_NUM);
        getfiles_op_status = 1; // Still successful in terms of protocol
        goto cleanup_getfiles;  // Close connection
    }

    // Read chunks from temp file and write to S1 socket
    char file_list_chunk[FILE_CHUNK_BUFFER_SIZE];
    size_t bytes_read_from_temp;
    while ((bytes_read_from_temp = fread(file_list_chunk, 1, sizeof(file_list_chunk), temp_list_file_stream)) > 0)
    {
        ssize_t bytes_written_to_s1 = write(s1_socket_fd, file_list_chunk, bytes_read_from_temp);
        // Check for write error or incomplete write
        if (bytes_written_to_s1 < 0 || (size_t)bytes_written_to_s1 != bytes_read_from_temp)
        {
            perror("[S%d_GETFILES_ERROR] Failed writing file list chunk to S1");
            getfiles_op_status = 0; // Mark failure
            break;                  // Stop sending
        }
    }
    // Check if loop finished due to fread error
    if (ferror(temp_list_file_stream))
    {
        perror("[S%d_GETFILES_ERROR] Error reading from temp file list");
        getfiles_op_status = 0; // Mark failure
    }

    // Close the temp file stream if it was opened
    if (temp_list_file_stream != NULL)
    {
        fclose(temp_list_file_stream);
        temp_list_file_stream = NULL;
    }
    printf("DEBUG (PID %d) GETFILES: Finished sending list content to S1.\n", getpid());

cleanup_getfiles:
    // Clean up the temporary file if it exists
    if (temp_list_filepath[0] != '\0')
    {
        printf("DEBUG (PID %d) GETFILES: Removing temp file '%s'.\n", getpid(), temp_list_filepath);
        remove(temp_list_filepath);
    }

    // S1 expects the connection to be closed after the list is sent (or immediately on error/empty).
    printf("DEBUG (PID %d) GETFILES: Closing connection to S1 (Status: %d).\n", getpid(), getfiles_op_status);
    // Socket will be closed by child process exit handler in main(). No need to close here?
    // Let's close it explicitly here for safety.
    close(s1_socket_fd);

    printf("DEBUG (PID %d) GETFILES: Exiting handle_getfiles_command.\n", getpid());
    // Child process will exit after this function returns.

} // End handle_getfiles_command

/**
 * @brief Handles the 'RETRIEVE' command from S1.
 * Finds the requested file locally and sends its size (string) then content.
 * Protocol:
 * 1. S1 sends "RETRIEVE <~S1/file_path>"
 * 2. S2 looks for the file locally (e.g., ~/S2/file_path).
 * 3. If found: S2 sends "<size_as_string>" followed immediately by file content.
 * 4. If not found: S2 sends "0" (size zero as string).
 * 5. S2 closes connection after sending content or size "0".
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "RETRIEVE" (the file_path).
 */
void handle_retrieve_command(int s1_socket_fd, const char *command_params)
{
    char s1_filepath_arg[256];
    char local_s2_filepath[512];
    char *server_home_dir_ptr = NULL;
    FILE *requested_file_stream = NULL;
    int retrieve_op_status = 1; // 1=OK, 0=Fail

    printf("DEBUG (PID %d) RETRIEVE: Handling RETRIEVE command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse File Path Parameter */
    if (sscanf(command_params, "%255s", s1_filepath_arg) != 1)
    {
        fprintf(stderr, "[S%d_RETRIEVE_ERROR] Invalid RETRIEVE parameters.\n", SERVER_NUM);
        uint32_t zero_size = htonl(0); // Send binary zero for error
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_op_status = 0;
        goto cleanup_retrieve;
    }
    printf("DEBUG (PID %d) RETRIEVE: Parsed Filepath: '%s'\n", getpid(), s1_filepath_arg);

    /* 2. Get Home Directory */
    server_home_dir_ptr = getenv("HOME");
    if (server_home_dir_ptr == NULL)
    {
        perror("[S%d_RETRIEVE_ERROR] getenv(\"HOME\") failed");
        uint32_t zero_size = htonl(0); // Send binary zero for error
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_op_status = 0;
        goto cleanup_retrieve;
    }

    /* 3. Construct Local S2 File Path */
    if (strncmp(s1_filepath_arg, "~S1/", 4) != 0)
    {
        fprintf(stderr, "[S%d_RETRIEVE_ERROR] Filepath missing ~S1/ prefix: %s\n", SERVER_NUM, s1_filepath_arg);
        uint32_t zero_size = htonl(0); // Send binary zero for error
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_op_status = 0;
        goto cleanup_retrieve;
    }
    snprintf(local_s2_filepath, sizeof(local_s2_filepath), "%s/S%d/%s", server_home_dir_ptr, SERVER_NUM, s1_filepath_arg + 4);
    printf("DEBUG (PID %d) RETRIEVE: Looking for local file at: '%s'\n", getpid(), local_s2_filepath);

    /* 4. Open Local File */
    requested_file_stream = fopen(local_s2_filepath, "rb"); // Read binary
    if (requested_file_stream == NULL)
    {
        perror("[S%d_RETRIEVE_WARN] fopen failed (likely file not found)");
        uint32_t zero_size = htonl(0); // Send binary zero for error
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_op_status = 0;
        goto cleanup_retrieve;
    }
    printf("DEBUG (PID %d) RETRIEVE: File opened successfully.\n", getpid());

    /* 5. Get File Size */
    long file_byte_size = -1;
    if (fseek(requested_file_stream, 0, SEEK_END) == 0)
    {
        file_byte_size = ftell(requested_file_stream);
        if (fseek(requested_file_stream, 0, SEEK_SET) != 0)
            file_byte_size = -1;
    }
    if (file_byte_size < 0)
    {
        perror("[S%d_RETRIEVE_ERROR] fseek/ftell failed to get file size");
        uint32_t zero_size = htonl(0); // Send binary zero for error
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_op_status = 0;
        goto cleanup_retrieve;
    }
    printf("DEBUG (PID %d) RETRIEVE: File size is %ld bytes.\n", getpid(), file_byte_size);

    /* 6. Send File Size (as binary uint32_t in network byte order) */
    uint32_t file_size_network_order = htonl((uint32_t)file_byte_size);
    printf("DEBUG (PID %d) RETRIEVE: Sending size %u bytes as binary.\n", getpid(), (uint32_t)file_byte_size);

    ssize_t write_size_check = write(s1_socket_fd, &file_size_network_order, sizeof(file_size_network_order));
    if (write_size_check != sizeof(file_size_network_order))
    {
        perror("[S%d_RETRIEVE_ERROR] Failed to send binary file size");
        retrieve_op_status = 0;
        goto cleanup_retrieve;
    }

    /* 7. Send File Content */
    printf("DEBUG (PID %d) RETRIEVE: Starting loop to send file content...\n", getpid());
    char file_content_chunk_buffer[FILE_CHUNK_BUFFER_SIZE];
    size_t bytes_read_from_file_chunk;

    while ((bytes_read_from_file_chunk = fread(file_content_chunk_buffer, 1, sizeof(file_content_chunk_buffer), requested_file_stream)) > 0)
    {
        ssize_t bytes_written_content_chunk = write(s1_socket_fd, file_content_chunk_buffer, bytes_read_from_file_chunk);

        if (bytes_written_content_chunk < 0 || (size_t)bytes_written_content_chunk != bytes_read_from_file_chunk)
        {
            perror("[S%d_RETRIEVE_ERROR] Failed writing file content chunk to S1");
            retrieve_op_status = 0;
            break;
        }
    }

cleanup_retrieve:
    if (requested_file_stream != NULL)
        fclose(requested_file_stream);
    close(s1_socket_fd);

    printf("DEBUG (PID %d) RETRIEVE: Exiting handle_retrieve_command.\n", getpid());
}

/**
 * @brief Handles the 'REMOVE' command from S1.
 * Deletes the specified file locally after checking it exists.
 * Protocol:
 * 1. S1 sends "REMOVE <~S1/file_path>"
 * 2. S2 checks if local file exists (e.g., ~/S2/file_path).
 * 3. If exists, S2 attempts to remove it.
 * 4. S2 sends "SUCCESS" or "ERROR:<reason>".
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "REMOVE" (the file_path).
 */
void handle_remove_command(int s1_socket_fd, const char *command_params)
{
    char s1_filepath_param[256];
    char local_s2_filepath_to_remove[512];
    char *server_home_dir_env_ptr = NULL;
    const char *response_message_to_s1 = "ERROR:INTERNAL"; // Default error
    char error_response_buffer[300];                       // Buffer for formatted error messages
    int remove_op_failed = 0;                              // 0=OK, 1=Failed

    printf("DEBUG (PID %d) REMOVE: Handling REMOVE command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse File Path */
    if (sscanf(command_params, "%255s", s1_filepath_param) != 1)
    {
        fprintf(stderr, "[S%d_REMOVE_ERROR] Invalid REMOVE parameters.\n", SERVER_NUM);
        response_message_to_s1 = "ERROR:BAD_PARAMS";
        remove_op_failed = 1;
        goto send_remove_response;
    }
    printf("DEBUG (PID %d) REMOVE: Parsed Filepath: '%s'\n", getpid(), s1_filepath_param);

    /* 2. Get Home Directory */
    server_home_dir_env_ptr = getenv("HOME");
    if (!server_home_dir_env_ptr)
    { // Using !ptr check here
        perror("[S%d_REMOVE_ERROR] getenv(\"HOME\") failed");
        response_message_to_s1 = "ERROR:NO_HOME_DIR";
        remove_op_failed = 1;
        goto send_remove_response;
    }

    /* 3. Construct Local S2 Path */
    if (strncmp(s1_filepath_param, "~S1/", 4) != 0)
    {
        fprintf(stderr, "[S%d_REMOVE_ERROR] Filepath missing ~S1/ prefix: %s\n", SERVER_NUM, s1_filepath_param);
        response_message_to_s1 = "ERROR:INVALID_PATH_PREFIX";
        remove_op_failed = 1;
        goto send_remove_response;
    }
    snprintf(local_s2_filepath_to_remove, sizeof(local_s2_filepath_to_remove), "%s/S%d/%s", server_home_dir_env_ptr, SERVER_NUM, s1_filepath_param + 4);
    printf("DEBUG (PID %d) REMOVE: Attempting to remove local file: '%s'\n", getpid(), local_s2_filepath_to_remove);

    /* 4. Check if File Exists */
    struct stat file_existence_check;
    printf("DEBUG (PID %d) REMOVE: Checking file existence with stat()...\n", getpid());
    if (stat(local_s2_filepath_to_remove, &file_existence_check) != 0)
    {
        // stat failed, likely file not found or permissions issue
        if (errno == ENOENT)
        { // File Not Found
            printf("[S%d_REMOVE_WARN] File not found: '%s'.\n", SERVER_NUM, local_s2_filepath_to_remove);
            snprintf(error_response_buffer, sizeof(error_response_buffer), "ERROR:FILE_NOT_FOUND:%s", s1_filepath_param); // Include original path in error
            response_message_to_s1 = error_response_buffer;
        }
        else
        {                                                                                                  // Other error (e.g., permission denied)
            snprintf(error_response_buffer, sizeof(error_response_buffer), "ERROR:STAT_FAILED:%d", errno); // Include errno
            perror("[S%d_REMOVE_ERROR] stat() failed");
            response_message_to_s1 = error_response_buffer;
        }
        remove_op_failed = 1;
        goto send_remove_response;
    }
    // If stat succeeded, file exists. Check if it's a regular file (optional but good practice)
    if (!S_ISREG(file_existence_check.st_mode))
    {
        fprintf(stderr, "[S%d_REMOVE_ERROR] Path exists but is not a regular file: '%s'\n", SERVER_NUM, local_s2_filepath_to_remove);
        snprintf(error_response_buffer, sizeof(error_response_buffer), "ERROR:NOT_A_FILE:%s", s1_filepath_param);
        response_message_to_s1 = error_response_buffer;
        remove_op_failed = 1;
        goto send_remove_response;
    }
    printf("DEBUG (PID %d) REMOVE: File exists. Proceeding with remove().\n", getpid());

    /* 5. Remove File */
    if (remove(local_s2_filepath_to_remove) != 0)
    {
        // remove() failed (e.g., permission denied)
        snprintf(error_response_buffer, sizeof(error_response_buffer), "ERROR:REMOVE_FAILED:%d", errno);
        perror("[S%d_REMOVE_ERROR] remove() failed");
        response_message_to_s1 = error_response_buffer;
        remove_op_failed = 1;
        goto send_remove_response;
    }

    // If remove succeeded
    printf("[S%d_REMOVE_INFO] Successfully removed file: '%s'\n", SERVER_NUM, local_s2_filepath_to_remove);
    response_message_to_s1 = "SUCCESS"; // Set success message
    remove_op_failed = 0;

send_remove_response:
    // Send the final response string to S1
    printf("DEBUG (PID %d) REMOVE: Sending response to S1: '%s'\n", getpid(), response_message_to_s1);
    ssize_t write_resp_check = write(s1_socket_fd, response_message_to_s1, strlen(response_message_to_s1));
    if (write_resp_check < 0)
    {
        perror("[S%d_REMOVE_ERROR] Failed sending response to S1");
    }

    printf("DEBUG (PID %d) REMOVE: Exiting handle_remove_command (Failed=%d).\n", getpid(), remove_op_failed);

} // End handle_remove_command

/**
 * @brief Handles the 'MAKETAR' command from S1.
 * Creates a tarball of relevant files (.pdf for S2) and sends it back.
 * Protocol:
 * 1. S1 sends "MAKETAR .filetype" (e.g., ".pdf")
 * 2. S2 creates tarball (e.g., pdf.tar) in a temp dir.
 * 3. If files found/tar created: S2 sends "<size_as_string>" followed by tar content.
 * 4. If no files/error: S2 sends "0" (size zero string).
 * 5. S2 cleans up temp dir.
 * 6. S2 closes connection.
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "MAKETAR" (the filetype).
 */
void handle_maketar_command(int s1_socket_fd, const char *command_params)
{
    char requested_filetype_arg[10];
    char *server_home_directory_ptr = NULL;
    char temp_directory_path[256];
    char local_tar_file_path[512];
    char tar_creation_command[1024];
    char cleanup_command[256];
    char expected_file_pattern[10];      // e.g., "*.pdf"
    char expected_tar_filename[20];      // e.g., "pdf.tar"
    const char *size_response_str = "0"; // Default to "0" (no files/error)
    FILE *tar_file_stream_ptr = NULL;
    int maketar_op_status = 1; // 1=OK, 0=Fail
    int temp_dir_created = 0;  // Flag for cleanup

    printf("DEBUG (PID %d) MAKETAR: Handling MAKETAR command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse File Type */
    if (sscanf(command_params, "%9s", requested_filetype_arg) != 1)
    {
        fprintf(stderr, "[S%d_MAKETAR_ERROR] Invalid MAKETAR parameters.\n", SERVER_NUM);
        maketar_op_status = 0;
        goto send_maketar_size_and_content; // Send size "0"
    }
    printf("DEBUG (PID %d) MAKETAR: Parsed Filetype: '%s'\n", getpid(), requested_filetype_arg);

    /* 2. Validate File Type for THIS Server */
    // Set expected pattern and filename based on SERVER_NUM
    // S2 only handles .pdf
    if (SERVER_NUM == 2 && strcasecmp(requested_filetype_arg, ".pdf") == 0)
    {
        strcpy(expected_file_pattern, "*.pdf");
        strcpy(expected_tar_filename, "pdf.tar");
    }
    else if (SERVER_NUM == 3 && strcasecmp(requested_filetype_arg, ".txt") == 0)
    {
        // Placeholder for S3 logic (should not be reached in S2 binary)
        strcpy(expected_file_pattern, "*.txt");
        strcpy(expected_tar_filename, "text.tar");
        fprintf(stderr, "[S%d_MAKETAR_WARN] MAKETAR called with .txt on PDF server? (Logic check)\n", SERVER_NUM);
    }
    else if (SERVER_NUM == 4 && strcasecmp(requested_filetype_arg, ".zip") == 0)
    {
        // Placeholder for S4 logic (should not be reached in S2 binary)
        strcpy(expected_file_pattern, "*.zip");
        strcpy(expected_tar_filename, "zip.tar");
        fprintf(stderr, "[S%d_MAKETAR_WARN] MAKETAR called with .zip on PDF server? (Logic check)\n", SERVER_NUM);
    }
    else
    {
        // File type doesn't match what this server handles
        fprintf(stderr, "[S%d_MAKETAR_ERROR] Invalid file type '%s' requested for server S%d.\n",
                SERVER_NUM, requested_filetype_arg, SERVER_NUM);
        // Protocol requires sending "0" size string in this case too.
        maketar_op_status = 0;
        goto send_maketar_size_and_content;
    }
    printf("DEBUG (PID %d) MAKETAR: Server S%d handling type '%s'. Pattern: '%s', Tar File: '%s'.\n",
           getpid(), SERVER_NUM, requested_filetype_arg, expected_file_pattern, expected_tar_filename);

    /* 3. Get Home Directory */
    server_home_directory_ptr = getenv("HOME");
    if (server_home_directory_ptr == NULL)
    {
        perror("[S%d_MAKETAR_ERROR] getenv(\"HOME\") failed");
        maketar_op_status = 0;
        goto send_maketar_size_and_content; // Send size "0"
    }

    /* 4. Create Temporary Directory */
    snprintf(temp_directory_path, sizeof(temp_directory_path), "%s/s%d_temp_tar_%d", server_home_directory_ptr, SERVER_NUM, getpid());
    snprintf(tar_creation_command, sizeof(tar_creation_command), "mkdir -p \"%s\"", temp_directory_path); // Reuse buffer
    printf("DEBUG (PID %d) MAKETAR: Creating temp dir: %s\n", getpid(), tar_creation_command);
    int mkdir_code = system(tar_creation_command);
    if (mkdir_code != 0)
    {
        fprintf(stderr, "[S%d_MAKETAR_ERROR] Failed to create temp directory '%s' (Code: %d).\n", SERVER_NUM, temp_directory_path, mkdir_code);
        maketar_op_status = 0;
        goto send_maketar_size_and_content; // Send size "0"
    }
    temp_dir_created = 1; // Mark for cleanup

    /* 5. Construct and Execute Tar Command */
    snprintf(local_tar_file_path, sizeof(local_tar_file_path), "%s/%s", temp_directory_path, expected_tar_filename);
    // Command: find ~/S<N> -name "*.ext" -type f -print0 | xargs -0 tar -cf /path/to/temp/file.tar 2>/dev/null
    snprintf(tar_creation_command, sizeof(tar_creation_command),
             "find %s/S%d -name \"%s\" -type f -print0 | xargs -0 tar -cf \"%s\" 2>/dev/null",
             server_home_directory_ptr, SERVER_NUM, expected_file_pattern, local_tar_file_path);
    printf("DEBUG (PID %d) MAKETAR: Executing tar command: %s\n", getpid(), tar_creation_command);
    int tar_exit_code = system(tar_creation_command);
    if (tar_exit_code != 0)
    {
        fprintf(stderr, "[S%d_MAKETAR_WARN] Tar command pipeline exited with status %d.\n", SERVER_NUM, tar_exit_code);
        // Continue to check if file exists, maybe find just found nothing.
    }

    /* 6. Check Tar File Existence and Size */
    printf("DEBUG (PID %d) MAKETAR: Checking result file '%s'...\n", getpid(), local_tar_file_path);
    struct stat tar_file_info;
    long tar_file_byte_size = 0; // Use long for size
    // Check if file exists and get status
    if (stat(local_tar_file_path, &tar_file_info) == 0)
    {
        // File exists, check if size is > 0
        if (tar_file_info.st_size > 0)
        {
            tar_file_byte_size = tar_file_info.st_size; // Get the size
            printf("DEBUG (PID %d) MAKETAR: Tar file created successfully. Size: %ld bytes.\n", getpid(), tar_file_byte_size);
        }
        else
        {
            // File exists but is empty (no matching files found by find)
            printf("[S%d_MAKETAR_INFO] Tar file is empty (0 bytes). No files found?\n", SERVER_NUM);
            tar_file_byte_size = 0; // Ensure size is 0
        }
    }
    else
    {
        // Stat failed, file likely doesn't exist (tar command failed or found nothing)
        perror("[S%d_MAKETAR_WARN] stat failed on tar file (likely no files found)");
        printf("[S%d_MAKETAR_INFO] Tar file '%s' not found or inaccessible.\n", SERVER_NUM, local_tar_file_path);
        tar_file_byte_size = 0; // Treat as size 0
    }

    /* 7. Prepare Size Response String */
    char size_string_buffer[32];
    snprintf(size_string_buffer, sizeof(size_string_buffer), "%ld", tar_file_byte_size);
    size_response_str = size_string_buffer; // Point to the generated string

send_maketar_size_and_content:
    /* 8. Send Size String to S1 */
    printf("DEBUG (PID %d) MAKETAR: Sending size string '%s' to S1...\n", getpid(), size_response_str);
    ssize_t write_size_response_check = write(s1_socket_fd, size_response_str, strlen(size_response_str));
    if (write_size_response_check < (ssize_t)strlen(size_response_str))
    {
        perror("[S%d_MAKETAR_ERROR] Failed writing size string to S1");
        maketar_op_status = 0; // Mark failure
        goto cleanup_maketar;  // Cannot proceed
    }

    /* 9. Send Tar Content (only if size > 0 and op status is OK) */
    if (maketar_op_status == 1 && tar_file_byte_size > 0)
    {
        printf("DEBUG (PID %d) MAKETAR: Opening tar file '%s' to send content...\n", getpid(), local_tar_file_path);
        tar_file_stream_ptr = fopen(local_tar_file_path, "rb");
        if (tar_file_stream_ptr == NULL)
        {
            perror("[S%d_MAKETAR_ERROR] Failed to open created tar file for reading");
            // We already sent the size, S1 might be stuck waiting. Best effort is to close connection.
            maketar_op_status = 0;
        }
        else
        {
            printf("DEBUG (PID %d) MAKETAR: Sending %ld bytes of tar content...\n", getpid(), tar_file_byte_size);
            char tar_content_chunk[FILE_CHUNK_BUFFER_SIZE];
            size_t bytes_read_tar_chunk;
            size_t total_tar_bytes_sent = 0;
            while ((bytes_read_tar_chunk = fread(tar_content_chunk, 1, sizeof(tar_content_chunk), tar_file_stream_ptr)) > 0)
            {
                ssize_t bytes_written_tar_chunk = write(s1_socket_fd, tar_content_chunk, bytes_read_tar_chunk);
                if (bytes_written_tar_chunk < 0 || (size_t)bytes_written_tar_chunk != bytes_read_tar_chunk)
                {
                    perror("[S%d_MAKETAR_ERROR] Failed writing tar content chunk to S1");
                    maketar_op_status = 0; // Mark failure
                    break;                 // Stop sending
                }
                total_tar_bytes_sent += bytes_written_tar_chunk;
            }
            if (ferror(tar_file_stream_ptr))
            {
                perror("[S%d_MAKETAR_ERROR] Error reading tar file content");
                maketar_op_status = 0; // Mark failure
            }
            fclose(tar_file_stream_ptr);
            tar_file_stream_ptr = NULL; // Mark closed
            printf("DEBUG (PID %d) MAKETAR: Finished sending tar content (%zu bytes sent).\n", getpid(), total_tar_bytes_sent);
        }
    }
    else
    {
        printf("DEBUG (PID %d) MAKETAR: Skipping content sending (Size was 0 or error occurred).\n", getpid());
    }

cleanup_maketar:
    // Clean up the temporary directory if it was created
    if (temp_dir_created == 1)
    {
        snprintf(cleanup_command, sizeof(cleanup_command), "rm -rf \"%s\"", temp_directory_path);
        printf("DEBUG (PID %d) MAKETAR: Cleaning up temp directory: %s\n", getpid(), cleanup_command);
        int rm_code = system(cleanup_command);
        if (rm_code != 0)
        {
            fprintf(stderr, "[S%d_MAKETAR_WARN] Temp directory cleanup failed (Code: %d).\n", SERVER_NUM, rm_code);
        }
    }
    // Close file stream just in case loop broke before fclose
    if (tar_file_stream_ptr != NULL)
    {
        fclose(tar_file_stream_ptr);
    }
    // Protocol requires closing connection after sending data or size "0"
    printf("DEBUG (PID %d) MAKETAR: Closing connection to S1 (Status: %d).\n", getpid(), maketar_op_status);
    close(s1_socket_fd);

    printf("DEBUG (PID %d) MAKETAR: Exiting handle_maketar_command.\n", getpid());

} // End handle_maketar_command
