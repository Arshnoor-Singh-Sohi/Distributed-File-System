/* ======================================================== */
/*          S3.c - Secondary TXT Server                     */
/* ======================================================== */
/* This server is part of my Distributed File System project. */
/* It listens for requests specifically from the S1 server. */
/* Its main responsibility is managing .txt files.          */

// Standard C library includes needed for various functions
#include <stdio.h>      // For input/output: printf, perror, fopen, fread, fwrite, snprintf, etc.
#include <stdlib.h>     // For general utilities: exit, atoi, getenv, system, malloc, free
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
#include <stdint.h>

/* --- Server Identity --- */
// This #define MUST be set correctly for each secondary server.
// 2 for PDF server (S2)
// 3 for TXT server (S3)
// 4 for ZIP server (S4)
#define SERVER_NUM 3 // This is server S3

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
    printf("DEBUG: S%d Server starting up...\n", SERVER_NUM); // Use SERVER_NUM define
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
    // This server needs its own directory (e.g., ~/S3) to store files.
    char server_storage_dir_path[128];                                                       // Buffer for directory path string
    snprintf(server_storage_dir_path, sizeof(server_storage_dir_path), "~/S%d", SERVER_NUM); // Use SERVER_NUM

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
    int main_listener_socket_fd;              // File descriptor for the main socket listening for S1 connections
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
    int processing_status_flag = 1;    // Redundant flag: 1=OK, 0=Fail

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
        processing_status_flag = 0; // Mark failure
        // Fall through to exit (socket will be closed by exit or caller)
    }
    else if (bytes_read_cmd == 0)
    {
        fprintf(stderr, "[S%d_CHILD_WARN] S1 closed connection before sending command.\n", SERVER_NUM);
        processing_status_flag = 0; // Mark failure
        // Fall through to exit
    }
    else // Read successful
    {
        // Null-terminate the received command string
        s1_command_full_buffer[bytes_read_cmd] = '\0';
        printf("DEBUG (PID %d): Received command from S1: \"%s\"\n", getpid(), s1_command_full_buffer);

        // --- Parse and Dispatch Command ---
        // Find the first space to separate command verb from parameters
        char *command_verb_ptr = s1_command_full_buffer;
        char *command_params_ptr = strchr(s1_command_full_buffer, ' ');
        if (command_params_ptr != NULL)
        {
            *command_params_ptr = '\0'; // Null-terminate the verb
            command_params_ptr++;       // Move pointer past the space to the start of params
        }
        else
        {
            // If no space, parameters are empty (might be okay for some commands?)
            command_params_ptr = ""; // Point to an empty string
        }

        printf("DEBUG (PID %d): Parsed Verb: '%s', Params: '%s'\n", getpid(), command_verb_ptr, command_params_ptr);

        // Call the appropriate handler based on the command verb
        if (strcmp(command_verb_ptr, "STORE") == 0)
        {
            handle_store_command(s1_socket_descriptor, command_params_ptr);
        }
        else if (strcmp(command_verb_ptr, "GETFILES") == 0)
        {
            handle_getfiles_command(s1_socket_descriptor, command_params_ptr);
        }
        else if (strcmp(command_verb_ptr, "RETRIEVE") == 0)
        {
            handle_retrieve_command(s1_socket_descriptor, command_params_ptr);
        }
        else if (strcmp(command_verb_ptr, "REMOVE") == 0)
        {
            handle_remove_command(s1_socket_descriptor, command_params_ptr);
        }
        else if (strcmp(command_verb_ptr, "MAKETAR") == 0)
        {
            handle_maketar_command(s1_socket_descriptor, command_params_ptr);
        }
        else
        {
            // Unknown command received
            fprintf(stderr, "[S%d_CHILD_ERROR] Received unknown command from S1: '%s'\n", SERVER_NUM, command_verb_ptr);
            const char *error_unknown_cmd_msg = "ERROR:UNKNOWN_COMMAND";
            // Try to send error back to S1
            ssize_t write_err_unknown = write(s1_socket_descriptor, error_unknown_cmd_msg, strlen(error_unknown_cmd_msg));
            if (write_err_unknown < 0)
            {
                perror("[S%d_CHILD_WARN] Failed sending UNKNOWN_COMMAND error to S1");
            }
            processing_status_flag = 0; // Mark failure
        }
    }

    // This function implicitly returns after handling one command.
    // The child process will then exit in the main loop's child block.
    printf("DEBUG (PID %d): process_s1_connection finished (Status Flag: %d).\n", getpid(), processing_status_flag);

} // End of process_s1_connection

/* ======================================================== */
/*             Command Handling Functions                   */
/* ======================================================== */

/**
 * @brief Handles the 'STORE' command from S1.
 * Receives a file and saves it locally in the appropriate S3 directory.
 * Protocol:
 * 1. S1 sends "STORE <filename> <~S1/dest_path> <size>"
 * 2. S3 creates directory if needed.
 * 3. S3 sends "READY".
 * 4. S1 sends <size> bytes of file content.
 * 5. S3 reads content and saves to file.
 * 6. S3 sends "SUCCESS" or "ERROR:<reason>".
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "STORE" (filename, dest_path, size).
 */
void handle_store_command(int s1_socket_fd, const char *command_params)
{
    char received_client_filename[256];                         // Filename S1 got from client
    char original_s1_destination_path[256];                     // Path S1 got from client (~S1/...)
    unsigned long expected_file_size_ulong = 0;                 // Size from S1 command
    int items_parsed_count = 0;                                 // Result of sscanf
    char *server_local_home_path = NULL;                        // Path to this server's home dir
    char final_s3_filepath_absolute[512];                       // Absolute path for saving file locally (e.g., ~/S3/...)
    char final_s3_directory_absolute[512];                      // Absolute path for the directory containing the file
    char system_mkdir_command_buffer[1024];                     // Buffer for mkdir command
    FILE *local_output_file_handle = NULL;                      // File pointer for writing the received file
    const char *response_string_to_s1 = "ERROR:STORE_INTERNAL"; // Default error message
    int store_operation_has_failed = 0;                         // 0=OK, 1=Failed

    printf("DEBUG (PID %d) STORE: Handling STORE command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse Parameters */
    printf("DEBUG (PID %d) STORE: Parsing parameters...\n", getpid());
    // Use sscanf to extract the three parts from the command parameters string
    items_parsed_count = sscanf(command_params, "%255s %255s %lu",
                                received_client_filename, original_s1_destination_path, &expected_file_size_ulong);
    // Check if exactly 3 items were successfully parsed
    if (items_parsed_count != 3)
    {
        fprintf(stderr, "[S%d_STORE_ERROR] Invalid STORE parameters. Expected 3 items, got %d.\n", SERVER_NUM, items_parsed_count);
        response_string_to_s1 = "ERROR:BAD_STORE_PARAMS";
        store_operation_has_failed = 1;
        goto send_store_command_response; // Use goto for unified cleanup/response
    }
    // Convert size to size_t for local use
    size_t expected_file_byte_count = (size_t)expected_file_size_ulong;
    printf("DEBUG (PID %d) STORE: Parsed OK -> File='%s', S1_Dest='%s', Size=%zu\n",
           getpid(), received_client_filename, original_s1_destination_path, expected_file_byte_count);

    /* 2. Get Home Directory */
    printf("DEBUG (PID %d) STORE: Getting HOME directory environment variable...\n", getpid());
    server_local_home_path = getenv("HOME");
    // Check if getenv failed
    if (server_local_home_path == NULL)
    {
        perror("[S%d_STORE_ERROR] getenv(\"HOME\") failed");
        response_string_to_s1 = "ERROR:SERVER_NO_HOME";
        store_operation_has_failed = 1;
        goto send_store_command_response;
    }
    printf("DEBUG (PID %d) STORE: Server HOME directory: '%s'.\n", getpid(), server_local_home_path);

    /* 3. Construct Local S3 Paths */
    printf("DEBUG (PID %d) STORE: Constructing local S%d file paths...\n", getpid(), SERVER_NUM);
    // Verify that the destination path starts with "~S1/" as expected from S1
    if (strncmp(original_s1_destination_path, "~S1/", 4) != 0)
    {
        fprintf(stderr, "[S%d_STORE_ERROR] Received destination path missing ~S1/ prefix: '%s'\n",
                SERVER_NUM, original_s1_destination_path);
        response_string_to_s1 = "ERROR:INVALID_S1_PATH_PREFIX";
        store_operation_has_failed = 1;
        goto send_store_command_response;
    }
    // Get the path part *after* "~S1/"
    char *path_relative_to_s1_root = original_s1_destination_path + 4; // e.g., "folder/file.txt"
    // Find the last '/' to separate directory and filename
    char *last_separator_ptr = strrchr(path_relative_to_s1_root, '/');

    // Determine the directory and full path for the *local S3* filesystem
    if (last_separator_ptr == NULL || last_separator_ptr == path_relative_to_s1_root)
    {
        // Case 1: Path is like "file.txt" or "/file.txt" (relative to ~S1/)
        // The directory to create locally is just ~/S<N> (e.g., ~/S3)
        snprintf(final_s3_directory_absolute, sizeof(final_s3_directory_absolute), "%s/S%d", server_local_home_path, SERVER_NUM);
        // The full path for the file locally is ~/S<N>/file.txt
        snprintf(final_s3_filepath_absolute, sizeof(final_s3_filepath_absolute), "%s/S%d/%s", server_local_home_path, SERVER_NUM, path_relative_to_s1_root);
    }
    else
    {
        // Case 2: Path is like "folder/subfolder/file.txt"
        // The directory to create locally is ~/S<N>/folder/subfolder
        snprintf(final_s3_directory_absolute, sizeof(final_s3_directory_absolute), "%s/S%d/%.*s",
                 server_local_home_path, SERVER_NUM, (int)(last_separator_ptr - path_relative_to_s1_root), path_relative_to_s1_root);
        // The full path for the file locally is ~/S<N>/folder/subfolder/file.txt
        snprintf(final_s3_filepath_absolute, sizeof(final_s3_filepath_absolute), "%s/S%d/%s", server_local_home_path, SERVER_NUM, path_relative_to_s1_root);
    }
    printf("DEBUG (PID %d) STORE: Local S%d Directory Path: '%s'\n", getpid(), SERVER_NUM, final_s3_directory_absolute);
    printf("DEBUG (PID %d) STORE: Local S%d File Path: '%s'\n", getpid(), SERVER_NUM, final_s3_filepath_absolute);

    /* 4. Create Directory Structure */
    // Use `mkdir -p` via system() to create the directory if it doesn't exist.
    snprintf(system_mkdir_command_buffer, sizeof(system_mkdir_command_buffer), "mkdir -p \"%s\"", final_s3_directory_absolute);
    printf("DEBUG (PID %d) STORE: Executing directory creation: %s\n", getpid(), system_mkdir_command_buffer);
    int mkdir_return_code = system(system_mkdir_command_buffer);
    // Check if the command failed (returned non-zero).
    if (mkdir_return_code != 0)
    {
        // Log a warning, but continue. fopen() later will fail if the directory isn't writable.
        fprintf(stderr, "[S%d_STORE_WARN] system('%s') returned status %d.\n",
                SERVER_NUM, system_mkdir_command_buffer, mkdir_return_code);
    }
    else
    {
        printf("DEBUG (PID %d) STORE: Directory creation command completed.\n", getpid());
    }

    /* 5. Send "READY" signal to S1 */
    const char *protocol_ready_signal = "READY";
    printf("DEBUG (PID %d) STORE: Sending '%s' signal to S1...\n", getpid(), protocol_ready_signal);
    ssize_t bytes_written_ready = write(s1_socket_fd, protocol_ready_signal, strlen(protocol_ready_signal));
    // Check if write failed or was incomplete
    if (bytes_written_ready < (ssize_t)strlen(protocol_ready_signal))
    {
        perror("[S%d_STORE_ERROR] Failed sending READY signal to S1");
        store_operation_has_failed = 1;
        response_string_to_s1 = "ERROR:WRITE_READY_FAIL"; // Specific internal code
        // Cannot send final response if write fails now, just cleanup and exit child.
        goto cleanup_store_operation_resources;
    }
    printf("DEBUG (PID %d) STORE: READY signal sent successfully.\n", getpid());

    /* 6. Open Local File for Writing */
    printf("DEBUG (PID %d) STORE: Opening local file '%s' for writing (binary mode)...\n", getpid(), final_s3_filepath_absolute);
    local_output_file_handle = fopen(final_s3_filepath_absolute, "wb"); // "wb" = write binary
    // Check if fopen failed
    if (local_output_file_handle == NULL)
    {
        // *** CRITICAL: Protocol requires us to read data from S1 even if fopen fails! ***
        // Log the error, proceed to read/discard data, and send error response *later*.
        perror("[S%d_STORE_ERROR] fopen failed for local file");
        fprintf(stderr, "[S%d_STORE_ERROR] Failed opening '%s'. Will read and discard data from S1.\n",
                SERVER_NUM, final_s3_filepath_absolute);
        // Do NOT set store_operation_has_failed = 1 yet. It will be set later based on NULL check.
    }
    else
    {
        printf("DEBUG (PID %d) STORE: Local file opened successfully for writing.\n", getpid());
    }

    /* 7. Receive File Data from S1 */
    char data_transfer_buffer[FILE_CHUNK_BUFFER_SIZE]; // Buffer for chunks
    size_t bytes_received_cumulative = 0;              // Counter for received bytes
    int local_file_write_error = 0;                    // Flag: 1 if fwrite fails

    printf("DEBUG (PID %d) STORE: Starting loop to receive %zu bytes from S1...\n", getpid(), expected_file_byte_count);
    // Loop until expected size is received or an error occurs
    // Using a for loop structure for variety
    for (bytes_received_cumulative = 0; bytes_received_cumulative < expected_file_byte_count; /* Updated inside */)
    {
        // Calculate how many bytes to attempt reading in this iteration
        size_t read_attempt_size = FILE_CHUNK_BUFFER_SIZE;
        size_t remaining_bytes = expected_file_byte_count - bytes_received_cumulative;
        if (remaining_bytes < read_attempt_size)
        {
            read_attempt_size = remaining_bytes;
        }

        // Read data chunk sent by S1 from the socket
        ssize_t bytes_read_this_iteration = read(s1_socket_fd, data_transfer_buffer, read_attempt_size);

        // Check the result of read()
        if (bytes_read_this_iteration < 0)
        { // Read error
            perror("[S%d_STORE_ERROR] read() failed while receiving data from S1");
            store_operation_has_failed = 1;
            response_string_to_s1 = "ERROR:NETWORK_READ_FAIL";
            goto cleanup_store_operation_resources; // Jump to cleanup
        }
        if (bytes_read_this_iteration == 0)
        { // S1 disconnected prematurely
            fprintf(stderr, "[S%d_STORE_ERROR] S1 closed connection unexpectedly (received %zu/%zu bytes)\n",
                    SERVER_NUM, bytes_received_cumulative, expected_file_byte_count);
            store_operation_has_failed = 1;
            response_string_to_s1 = "ERROR:S1_UNEXPECTED_CLOSE";
            goto cleanup_store_operation_resources; // Jump to cleanup
        }

        // --- Write received data to local file ---
        // Only attempt write if:
        // 1. File was opened successfully (handle is not NULL).
        // 2. No previous fwrite error occurred (flag is 0).
        if (local_output_file_handle != NULL && local_file_write_error == 0)
        {
            size_t bytes_written_this_iteration = fwrite(data_transfer_buffer, 1, bytes_read_this_iteration, local_output_file_handle);
            // Check if fwrite wrote the expected number of bytes
            if (bytes_written_this_iteration != (size_t)bytes_read_this_iteration)
            {
                perror("[S%d_STORE_ERROR] fwrite() failed while writing to local file");
                fprintf(stderr, "[S%d_STORE_ERROR] Error writing to '%s'. Disk full?\n", SERVER_NUM, final_s3_filepath_absolute);
                local_file_write_error = 1; // Set flag to prevent further writes
                // IMPORTANT: Continue reading data from S1 socket to maintain protocol synchronization!
                // The overall operation will be marked as failed later.
            }
        }

        // Add number of bytes successfully read from socket to total count
        bytes_received_cumulative += bytes_read_this_iteration;

    } // End of for loop receiving data

    printf("DEBUG (PID %d) STORE: Finished receiving loop. Total received: %zu bytes\n", getpid(), bytes_received_cumulative);

    /* 8. Determine Final Status and Prepare Response */
    // Only evaluate file status if no network read error occurred
    if (store_operation_has_failed == 0)
    {
        if (local_output_file_handle == NULL)
        { // Check if fopen() failed earlier
            store_operation_has_failed = 1;
            response_string_to_s1 = "ERROR:LOCAL_FOPEN_FAILED";
            printf("[S%d_STORE_ERROR] Final status check: fopen had failed.\n", SERVER_NUM);
        }
        else if (local_file_write_error == 1)
        { // Check if fwrite() failed during reception
            store_operation_has_failed = 1;
            response_string_to_s1 = "ERROR:LOCAL_FWRITE_FAILED";
            printf("[S%d_STORE_ERROR] Final status check: fwrite had failed.\n", SERVER_NUM);
            // File is likely corrupt, cleanup happens below.
        }
        else if (bytes_received_cumulative != expected_file_byte_count)
        { // Check for size mismatch
            store_operation_has_failed = 1;
            response_string_to_s1 = "ERROR:SIZE_MISMATCH";
            fprintf(stderr, "[S%d_STORE_ERROR] Final status check: Size mismatch (Expected %zu, Got %zu).\n",
                    SERVER_NUM, expected_file_byte_count, bytes_received_cumulative);
            // File is incomplete, cleanup happens below.
        }
        else
        {
            // If we reach here, all checks passed - SUCCESS!
            store_operation_has_failed = 0; // Explicitly mark success
            response_string_to_s1 = "SUCCESS";
            printf("DEBUG (PID %d) STORE: File received and saved successfully to '%s'.\n", getpid(), final_s3_filepath_absolute);
            // Close the successfully written file NOW.
            if (fclose(local_output_file_handle) != 0)
            {
                perror("[S%d_STORE_WARN] fclose failed after successful write");
                // Still report SUCCESS to S1 as data should be on disk.
            }
            local_output_file_handle = NULL; // Mark as closed to prevent double close in cleanup
        }
    }
    // If store_operation_has_failed was already 1 due to network error, response_string_to_s1 is already set.

cleanup_store_operation_resources:
    // Cleanup potentially opened file handle, especially if an error occurred.
    if (local_output_file_handle != NULL)
    {
        fclose(local_output_file_handle);
        local_output_file_handle = NULL; // Avoid double close
        // If the operation failed *after* the file was opened, attempt to remove the partial/corrupt file.
        if (store_operation_has_failed == 1 && final_s3_filepath_absolute[0] != '\0')
        {
            printf("DEBUG (PID %d) STORE: Operation failed. Attempting to remove partial file '%s'\n", getpid(), final_s3_filepath_absolute);
            // remove() is same as unlink()
            if (remove(final_s3_filepath_absolute) != 0 && errno != ENOENT)
            { // Ignore "No such file" error
                perror("[S%d_STORE_WARN] Failed to remove partial file during cleanup");
            }
        }
    }

send_store_command_response:
    // Send the final calculated response string to S1, unless write failed earlier.
    if (response_string_to_s1 != NULL && strcmp(response_string_to_s1, "ERROR:WRITE_READY_FAIL") != 0)
    {
        printf("DEBUG (PID %d) STORE: Sending final response to S1: '%s'\n", getpid(), response_string_to_s1);
        ssize_t bytes_written_final_response = write(s1_socket_fd, response_string_to_s1, strlen(response_string_to_s1));
        if (bytes_written_final_response < 0)
        {
            perror("[S%d_STORE_ERROR] Failed to send final STORE response to S1");
        }
    }
    else if (response_string_to_s1 == NULL)
    {
        // Fallback if something went wrong setting the message
        fprintf(stderr, "[S%d_STORE_ERROR] Internal error: response_string_to_s1 is NULL!\n", SERVER_NUM);
        const char *fallback_err = "ERROR:UNKNOWN";
        write(s1_socket_fd, fallback_err, strlen(fallback_err));
    }

    printf("DEBUG (PID %d) STORE: Exiting handle_store_command (Failed=%d).\n", getpid(), store_operation_has_failed);

} // End handle_store_command

/**
 * @brief Handles the 'GETFILES' command from S1.
 * Finds TXT files in the specified directory, sorts them, and sends the list back.
 * Protocol:
 * 1. S1 sends "GETFILES <~S1/pathname>"
 * 2. S3 finds matching TXT files locally (e.g., in ~/S3/pathname).
 * 3. S3 sends the sorted, newline-separated list of filenames (basenames only).
 * 4. S3 closes the connection to signal the end of the list.
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "GETFILES" (the pathname).
 */
void handle_getfiles_command(int s1_socket_fd, const char *command_params)
{
    char s1_pathname_parameter[256];        // Path argument from S1 command
    char local_s3_directory_to_search[512]; // Absolute path for local find command
    char *home_dir_path_env = NULL;         // Server's home directory
    char system_find_command[1024];         // Buffer for find/sort command
    char temp_file_list_path[256];          // Path for temporary file list
    FILE *temp_list_file_handle = NULL;     // File pointer for temp list
    int getfiles_operation_status = 1;      // 1 = OK, 0 = Fail

    printf("DEBUG (PID %d) GETFILES: Handling GETFILES command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse Path Parameter */
    // Extract the path argument after "GETFILES "
    if (sscanf(command_params, "%255s", s1_pathname_parameter) != 1)
    {
        fprintf(stderr, "[S%d_GETFILES_ERROR] Invalid GETFILES parameters. Expected path argument.\n", SERVER_NUM);
        // Protocol: Close connection on error without sending specific message.
        getfiles_operation_status = 0;
        goto cleanup_getfiles_resources; // Use goto for unified cleanup
    }
    printf("DEBUG (PID %d) GETFILES: Parsed Pathname Argument: '%s'\n", getpid(), s1_pathname_parameter);

    /* 2. Get Home Directory */
    home_dir_path_env = getenv("HOME");
    // Check if getenv failed
    if (home_dir_path_env == NULL)
    {
        perror("[S%d_GETFILES_ERROR] getenv(\"HOME\") failed");
        getfiles_operation_status = 0;
        goto cleanup_getfiles_resources;
    }

    /* 3. Construct Local S3 Directory Path */
    // Validate S1 path prefix
    if (strncmp(s1_pathname_parameter, "~S1/", 4) != 0)
    {
        fprintf(stderr, "[S%d_GETFILES_ERROR] Pathname parameter missing ~S1/ prefix: '%s'\n", SERVER_NUM, s1_pathname_parameter);
        getfiles_operation_status = 0;
        goto cleanup_getfiles_resources;
    }
    // Construct the local S3 path corresponding to the S1 path (e.g., /home/user/S3/folder/)
    snprintf(local_s3_directory_to_search, sizeof(local_s3_directory_to_search), "%s/S%d/%s",
             home_dir_path_env, SERVER_NUM, s1_pathname_parameter + 4); // Skip "~S1/"
    // Ensure path consistency for `find`: remove trailing slash if present (unless it's just "/")
    size_t local_path_length = strlen(local_s3_directory_to_search);
    if (local_path_length > 1 && local_s3_directory_to_search[local_path_length - 1] == '/')
    {
        local_s3_directory_to_search[local_path_length - 1] = '\0';
    }
    printf("DEBUG (PID %d) GETFILES: Local S%d directory path to search: '%s'\n", getpid(), SERVER_NUM, local_s3_directory_to_search);

    /* 4. Check if Local Directory Exists */
    struct stat dir_stat_buffer;
    // Use stat() to check existence and if it's a directory
    if (stat(local_s3_directory_to_search, &dir_stat_buffer) != 0 || !S_ISDIR(dir_stat_buffer.st_mode))
    {
        // Directory doesn't exist or isn't a directory.
        fprintf(stderr, "[S%d_GETFILES_WARN] Directory not found or not a directory: '%s'. Sending empty list indication.\n",
                SERVER_NUM, local_s3_directory_to_search);
        // Protocol: Send empty response by just closing the connection.
        getfiles_operation_status = 1; // Operation "succeeded" in terms of protocol (empty list)
        goto cleanup_getfiles_resources;
    }
    printf("DEBUG (PID %d) GETFILES: Local directory exists.\n", getpid());

    /* 5. Prepare Temp File Path */
    // Using /tmp for temporary storage if possible.
    snprintf(temp_file_list_path, sizeof(temp_file_list_path), "/tmp/s%d_files_%d.txt", SERVER_NUM, getpid());
    printf("DEBUG (PID %d) GETFILES: Using temporary file path: '%s'\n", getpid(), temp_file_list_path);
    // Clean up any leftover temp file from previous runs (ignore errors)
    remove(temp_file_list_path);

    /* 6. Execute `find` Command to Get Sorted File List */
    // Command: find <dir> -maxdepth 1 -type f -name "*.txt" -printf "%f\\n" 2>/dev/null | sort > <temp_file>
    snprintf(system_find_command, sizeof(system_find_command),
             "find \"%s\" -maxdepth 1 -type f -name \"*.txt\" -printf \"%%f\\n\" 2>/dev/null | sort > \"%s\"",
             local_s3_directory_to_search, temp_file_list_path); // Use .txt pattern
    printf("DEBUG (PID %d) GETFILES: Executing find/sort command: %s\n", getpid(), system_find_command);
    int find_command_exit_status = system(system_find_command);
    // Log warning if command fails, but proceed; fopen will check if file exists.
    if (find_command_exit_status != 0)
    {
        fprintf(stderr, "[S%d_GETFILES_WARN] Find/sort command exited with status %d.\n", SERVER_NUM, find_command_exit_status);
    }

    /* 7. Read Temp File and Send Content to S1 */
    printf("DEBUG (PID %d) GETFILES: Attempting to read results from '%s' and send to S1...\n", getpid(), temp_file_list_path);
    temp_list_file_handle = fopen(temp_file_list_path, "r"); // Open for reading
    // Check if file exists/is readable (might not if find failed or found nothing)
    if (temp_list_file_handle == NULL)
    {
        perror("[S%d_GETFILES_INFO] Failed to open temp file list (maybe no .txt files found?)");
        printf("[S%d_GETFILES_INFO] Sending empty list indication (closing connection).\n", SERVER_NUM);
        getfiles_operation_status = 1;   // Still successful protocol-wise
        goto cleanup_getfiles_resources; // Close connection
    }

    // Read chunks from the temporary file and write them directly to the S1 socket
    char list_data_buffer[FILE_CHUNK_BUFFER_SIZE];
    size_t bytes_read_from_temp_file;
    while ((bytes_read_from_temp_file = fread(list_data_buffer, 1, sizeof(list_data_buffer), temp_list_file_handle)) > 0)
    {
        ssize_t bytes_written_to_s1_socket = write(s1_socket_fd, list_data_buffer, bytes_read_from_temp_file);
        // Check for write error or incomplete write
        if (bytes_written_to_s1_socket < 0 || (size_t)bytes_written_to_s1_socket != bytes_read_from_temp_file)
        {
            perror("[S%d_GETFILES_ERROR] Failed writing file list chunk to S1");
            getfiles_operation_status = 0; // Mark failure
            break;                         // Stop sending
        }
    }
    // Check if the loop finished due to an error reading the temp file
    if (ferror(temp_list_file_handle))
    {
        perror("[S%d_GETFILES_ERROR] Error reading from temp file list");
        getfiles_operation_status = 0; // Mark failure
    }

    // Close the temp file stream if it was opened
    if (temp_list_file_handle != NULL)
    {
        fclose(temp_list_file_handle);
        temp_list_file_handle = NULL;
    }
    printf("DEBUG (PID %d) GETFILES: Finished sending list content (or empty list indication) to S1.\n", getpid());

cleanup_getfiles_resources:
    // Clean up the temporary file if its path was generated
    if (temp_file_list_path[0] != '\0')
    {
        printf("DEBUG (PID %d) GETFILES: Removing temp file '%s'.\n", getpid(), temp_file_list_path);
        remove(temp_file_list_path); // Attempt removal, ignore errors
    }

    // Protocol: S1 expects the connection to be closed after the list is sent (or immediately on error/empty).
    printf("DEBUG (PID %d) GETFILES: Closing connection to S1 (Status: %d).\n", getpid(), getfiles_operation_status);
    // Close the socket explicitly here.
    close(s1_socket_fd);

    printf("DEBUG (PID %d) GETFILES: Exiting handle_getfiles_command.\n", getpid());
    // Child process will exit after this function returns.

} // End handle_getfiles_command

/**
 * @brief Handles the 'RETRIEVE' command from S1 for .txt files.
 * Finds the requested text file locally and sends its size (binary uint32_t) then content.
 * Protocol:
 * 1. S1 sends "RETRIEVE <~S1/file_path>"
 * 2. S3 looks for the file locally (e.g., ~/S3/file_path).
 * 3. If found: S3 sends <size_as_binary_uint32_network_order> followed immediately by file content.
 * 4. If not found or error: S3 sends binary size 0 (4 bytes, network order).
 * 5. S3 closes connection after sending content or size 0.
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "RETRIEVE" (the file_path).
 */
void handle_retrieve_command(int s1_socket_fd, const char *command_params)
{
    char s1_txt_filepath_arg[256];          // Path from S1's command
    char local_s3_txt_filepath[512];        // Corresponding local path
    char *server_home_dir_path = NULL;      // Server's home directory path
    FILE *requested_txt_file_handle = NULL; // File pointer
    int retrieve_operation_status = 1;      // 1=OK, 0=Fail

    printf("DEBUG (PID %d) S3 RETRIEVE: Handling RETRIEVE. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse File Path Parameter */
    if (sscanf(command_params, "%255s", s1_txt_filepath_arg) != 1)
    {
        fprintf(stderr, "[S%d_RETRIEVE_ERROR] Invalid RETRIEVE parameters.\n", SERVER_NUM);
        uint32_t zero_size = htonl(0); // Send binary 0 for error
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_operation_status = 0;
        goto cleanup_retrieve_resources; // Use original goto label
    }

    /* 2. Get Home Directory */
    server_home_dir_path = getenv("HOME");
    if (!server_home_dir_path)
    {
        perror("[S%d_RETRIEVE_ERROR] getenv(\"HOME\") failed");
        uint32_t zero_size = htonl(0);
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_operation_status = 0;
        goto cleanup_retrieve_resources;
    }

    /* 3. Construct Local S3 File Path */
    if (strncmp(s1_txt_filepath_arg, "~S1/", 4) != 0)
    {
        fprintf(stderr, "[S%d_RETRIEVE_ERROR] Filepath missing ~S1/ prefix: '%s'\n", SERVER_NUM, s1_txt_filepath_arg);
        uint32_t zero_size = htonl(0);
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_operation_status = 0;
        goto cleanup_retrieve_resources;
    }
    snprintf(local_s3_txt_filepath, sizeof(local_s3_txt_filepath), "%s/S%d/%s",
             server_home_dir_path, SERVER_NUM, s1_txt_filepath_arg + 4);
    printf("DEBUG (PID %d) S3 RETRIEVE: Looking for local file: '%s'\n", getpid(), local_s3_txt_filepath);


    /* 4. Open Local File (with retry for race condition) */
    printf("DEBUG (PID %d) S3 RETRIEVE: Attempting to open file (attempt 1)...\n", getpid());
    requested_txt_file_handle = fopen(local_s3_txt_filepath, "rb"); // Read binary is safer

    if (requested_txt_file_handle == NULL)
    {
        // First attempt failed, log warning, wait briefly, try again
        perror("[S%d_RETRIEVE_WARN] Initial fopen failed (could be race condition or file not found)");
        printf("DEBUG (PID %d) S3 RETRIEVE: Initial fopen failed. Waiting 100ms and retrying...\n", getpid());
        usleep(100000); // Wait 100 milliseconds

        printf("DEBUG (PID %d) S3 RETRIEVE: Attempting to open file (attempt 2)...\n", getpid());
        requested_txt_file_handle = fopen(local_s3_txt_filepath, "rb"); // Try opening again

        if (requested_txt_file_handle == NULL)
        {
            // Second attempt also failed, assume file really doesn't exist or is inaccessible
            perror("[S%d_RETRIEVE_ERROR] Second fopen attempt failed");
            printf("[S%d_RETRIEVE_INFO] File '%s' not found/accessible after retry. Sending size 0.\n", SERVER_NUM, local_s3_txt_filepath);
            uint32_t zero_size = htonl(0); // Send binary 0 for file not found
            write(s1_socket_fd, &zero_size, sizeof(zero_size));
            retrieve_operation_status = 0;
            goto cleanup_retrieve_resources;
        }
         printf("DEBUG (PID %d) S3 RETRIEVE: File opened successfully on second attempt.\n", getpid());
    }
    else
    {
        printf("DEBUG (PID %d) S3 RETRIEVE: File opened successfully on first attempt.\n", getpid());
    }



    /* 5. Get File Size */
    // This part runs only if fopen succeeded (either first or second try)
    long file_size_value = -1;
    if (fseek(requested_txt_file_handle, 0, SEEK_END) == 0)
    {
        file_size_value = ftell(requested_txt_file_handle);
        if (file_size_value < 0 || fseek(requested_txt_file_handle, 0, SEEK_SET) != 0)
        {
            perror("[S%d_RETRIEVE_ERROR] ftell or fseek failed");
            file_size_value = -1; // Mark error
        }
    }
    else
    {
        perror("[S%d_RETRIEVE_ERROR] fseek to end failed");
        file_size_value = -1; // Mark error
    }

    if (file_size_value < 0)
    {
        fprintf(stderr, "[S%d_RETRIEVE_ERROR] Could not determine file size.\n", SERVER_NUM);
        uint32_t zero_size = htonl(0); // Send binary 0 for error
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_operation_status = 0;
        goto cleanup_retrieve_resources;
    }
    printf("DEBUG (PID %d) S3 RETRIEVE: File size: %ld bytes.\n", getpid(), file_size_value);

    /* 6. Send File Size (Binary Network Order) */
    if ((unsigned long)file_size_value > UINT32_MAX)
    {
        fprintf(stderr, "[S%d_RETRIEVE_ERROR] File size %ld exceeds uint32_t limit! Sending 0 size.\n", SERVER_NUM, file_size_value);
        uint32_t zero_size = htonl(0);
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_operation_status = 0;
        goto cleanup_retrieve_resources;
    }
    uint32_t file_size_network_order = htonl((uint32_t)file_size_value);
    printf("DEBUG (PID %d) S3 RETRIEVE: Sending binary size %u (Network: 0x%x).\n", getpid(), (uint32_t)file_size_value, file_size_network_order);

    ssize_t write_size_check = write(s1_socket_fd, &file_size_network_order, sizeof(file_size_network_order));
    if (write_size_check != sizeof(file_size_network_order))
    {
        perror("[S%d_RETRIEVE_ERROR] Failed to send binary file size");
        retrieve_operation_status = 0;
        goto cleanup_retrieve_resources;
    }
    printf("DEBUG (PID %d) S3 RETRIEVE: Binary size sent successfully.\n", getpid());

    /* 7. Send File Content */
    printf("DEBUG (PID %d) S3 RETRIEVE: Sending %ld bytes of content...\n", getpid(), file_size_value);
    char txt_content_chunk_buffer[FILE_CHUNK_BUFFER_SIZE];
    size_t bytes_read_txt_chunk;
    size_t total_bytes_sent_content = 0;

    while ((bytes_read_txt_chunk = fread(txt_content_chunk_buffer, 1, sizeof(txt_content_chunk_buffer), requested_txt_file_handle)) > 0)
    {
        ssize_t bytes_written_txt_chunk = write(s1_socket_fd, txt_content_chunk_buffer, bytes_read_txt_chunk);
        if (bytes_written_txt_chunk < 0 || (size_t)bytes_written_txt_chunk != bytes_read_txt_chunk)
        {
            perror("[S%d_RETRIEVE_ERROR] Failed writing content chunk");
            retrieve_operation_status = 0;
            break;
        }
        total_bytes_sent_content += bytes_written_txt_chunk;
    }
    if (ferror(requested_txt_file_handle))
    {
        perror("[S%d_RETRIEVE_ERROR] Error reading file during send");
        retrieve_operation_status = 0;
    }

    if (retrieve_operation_status == 1)
    {
        printf("DEBUG (PID %d) S3 RETRIEVE: Finished sending content. Total sent: %zu bytes.\n", getpid(), total_bytes_sent_content);
        if (total_bytes_sent_content != (size_t)file_size_value)
        {
            fprintf(stderr, "[S%d_RETRIEVE_WARN] Size mismatch after sending! Expected %ld, Sent %zu.\n", SERVER_NUM, file_size_value, total_bytes_sent_content);
            retrieve_operation_status = 0; // Treat size mismatch as failure
        }
    }

cleanup_retrieve_resources:
    if (requested_txt_file_handle != NULL)
    {
        fclose(requested_txt_file_handle);
    }
    printf("DEBUG (PID %d) S3 RETRIEVE: Closing connection to S1 (Status: %d).\n", getpid(), retrieve_operation_status);
    close(s1_socket_fd); // Close connection as required by protocol
    printf("DEBUG (PID %d) S3 RETRIEVE: Exiting handle_retrieve_command.\n", getpid());
}


/**
 * @brief Handles the 'REMOVE' command from S1.
 * Deletes the specified TXT file locally after checking it exists.
 * Protocol:
 * 1. S1 sends "REMOVE <~S1/file_path>"
 * 2. S3 checks if local file exists (e.g., ~/S3/file_path).
 * 3. If exists, S3 attempts to remove it.
 * 4. S3 sends "SUCCESS" or "ERROR:<reason>".
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "REMOVE" (the file_path).
 */
void handle_remove_command(int s1_socket_fd, const char *command_params)
{
    char s1_filepath_to_remove_arg[256];                          // Path arg from S1
    char local_s3_filepath_absolute[512];                         // Absolute path for local remove
    char *home_dir_pointer = NULL;                                // Server's home directory
    const char *final_response_message = "ERROR:REMOVE_INTERNAL"; // Default error
    char formatted_error_response[300];                           // Buffer for specific error messages
    int remove_operation_failed_flag = 0;                         // 0=OK, 1=Failed

    printf("DEBUG (PID %d) REMOVE: Handling REMOVE command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse File Path */
    // Extract path from parameters
    if (sscanf(command_params, "%255s", s1_filepath_to_remove_arg) != 1)
    {
        fprintf(stderr, "[S%d_REMOVE_ERROR] Invalid REMOVE parameters.\n", SERVER_NUM);
        final_response_message = "ERROR:BAD_REMOVE_PARAMS";
        remove_operation_failed_flag = 1;
        goto send_remove_command_response; // Use goto for unified response sending
    }
    printf("DEBUG (PID %d) REMOVE: Parsed Filepath Argument: '%s'\n", getpid(), s1_filepath_to_remove_arg);

    /* 2. Get Home Directory */
    home_dir_pointer = getenv("HOME");
    // Check if getenv failed
    if (home_dir_pointer == NULL)
    { // Check for NULL
        perror("[S%d_REMOVE_ERROR] getenv(\"HOME\") failed");
        final_response_message = "ERROR:SERVER_NO_HOME";
        remove_operation_failed_flag = 1;
        goto send_remove_command_response;
    }

    /* 3. Construct Local S3 Path */
    // Validate S1 path prefix
    if (strncmp(s1_filepath_to_remove_arg, "~S1/", 4) != 0)
    {
        fprintf(stderr, "[S%d_REMOVE_ERROR] Filepath parameter missing ~S1/ prefix: '%s'\n", SERVER_NUM, s1_filepath_to_remove_arg);
        final_response_message = "ERROR:INVALID_S1_PATH_PREFIX";
        remove_operation_failed_flag = 1;
        goto send_remove_command_response;
    }
    // Construct local S3 path (e.g., /home/user/S3/folder/file.txt)
    snprintf(local_s3_filepath_absolute, sizeof(local_s3_filepath_absolute), "%s/S%d/%s",
             home_dir_pointer, SERVER_NUM, s1_filepath_to_remove_arg + 4); // Skip "~S1/"
    printf("DEBUG (PID %d) REMOVE: Planning to remove local file: '%s'\n", getpid(), local_s3_filepath_absolute);

    /* 4. Check if File Exists (using stat) */
    struct stat file_status_buffer;
    printf("DEBUG (PID %d) REMOVE: Checking file existence using stat()...\n", getpid());
    // Use stat() to check before attempting remove()
    if (stat(local_s3_filepath_absolute, &file_status_buffer) != 0)
    {
        // stat failed. Check errno to see why.
        if (errno == ENOENT)
        { // File Not Found - this is an expected error case
            printf("[S%d_REMOVE_WARN] File not found: '%s'. Reporting error to S1.\n", SERVER_NUM, local_s3_filepath_absolute);
            // Format error message including the path S1 requested
            snprintf(formatted_error_response, sizeof(formatted_error_response), "ERROR:FILE_NOT_FOUND:%s", s1_filepath_to_remove_arg);
            final_response_message = formatted_error_response;
        }
        else
        {                                                                                                               // Other error (e.g., permission denied accessing path components)
            snprintf(formatted_error_response, sizeof(formatted_error_response), "ERROR:STAT_ACCESS_FAILED:%d", errno); // Include errno
            perror("[S%d_REMOVE_ERROR] stat() failed");
            final_response_message = formatted_error_response;
        }
        remove_operation_failed_flag = 1; // Mark failure
        goto send_remove_command_response;
    }
    // Optional: Check if it's a regular file before removing
    if (!S_ISREG(file_status_buffer.st_mode))
    {
        fprintf(stderr, "[S%d_REMOVE_ERROR] Path exists but is not a regular file: '%s'\n", SERVER_NUM, local_s3_filepath_absolute);
        snprintf(formatted_error_response, sizeof(formatted_error_response), "ERROR:PATH_NOT_A_FILE:%s", s1_filepath_to_remove_arg);
        final_response_message = formatted_error_response;
        remove_operation_failed_flag = 1;
        goto send_remove_command_response;
    }
    printf("DEBUG (PID %d) REMOVE: File exists and is accessible. Proceeding with remove().\n", getpid());

    /* 5. Remove File */
    // Attempt to remove the file using remove() (same as unlink())
    if (remove(local_s3_filepath_absolute) != 0)
    {
        // remove() failed (e.g., write permission denied in directory)
        snprintf(formatted_error_response, sizeof(formatted_error_response), "ERROR:LOCAL_REMOVE_FAILED:%d", errno);
        perror("[S%d_REMOVE_ERROR] remove() failed");
        final_response_message = formatted_error_response;
        remove_operation_failed_flag = 1;
        goto send_remove_command_response;
    }

    // If remove() succeeded
    printf("[S%d_REMOVE_INFO] Successfully removed local file: '%s'\n", SERVER_NUM, local_s3_filepath_absolute);
    final_response_message = "SUCCESS"; // Set the success message
    remove_operation_failed_flag = 0;

send_remove_command_response:
    // Send the final response string (SUCCESS or ERROR:...) back to S1
    printf("DEBUG (PID %d) REMOVE: Sending final response to S1: '%s'\n", getpid(), final_response_message);
    ssize_t bytes_written_response = write(s1_socket_fd, final_response_message, strlen(final_response_message));
    // Check if write failed
    if (bytes_written_response < 0)
    {
        perror("[S%d_REMOVE_ERROR] Failed sending final response to S1");
    }

    printf("DEBUG (PID %d) REMOVE: Exiting handle_remove_command (Failed Flag=%d).\n", getpid(), remove_operation_failed_flag);

} // End handle_remove_command

/**
 * @brief Handles the 'MAKETAR' command from S1.
 * Creates a tarball of relevant files (.txt for S3) and sends it back.
 * Protocol:
 * 1. S1 sends "MAKETAR .filetype" (e.g., ".txt")
 * 2. S3 creates tarball (e.g., text.tar) in a temp dir.
 * 3. If files found/tar created: S3 sends "<size_as_string>" followed by tar content.
 * 4. If no files/error: S3 sends "0" (size zero string).
 * 5. S3 cleans up temp dir.
 * 6. S3 closes connection.
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "MAKETAR" (the filetype).
 */
void handle_maketar_command(int s1_socket_fd, const char *command_params)
{
    char filetype_parameter_arg[10];         // File type arg from S1 (".txt")
    char *home_directory_env_value = NULL;   // Server's home directory
    char temporary_working_directory[256];   // Path for temporary tar creation
    char local_tar_archive_filepath[512];    // Full path to the created tar file
    char system_command_buffer[1024];        // Buffer for system() calls
    char system_cleanup_command_buffer[256]; // Buffer for rm cleanup command
    char expected_file_name_pattern[10];     // e.g., "*.txt"
    char expected_tar_archive_name[20];      // e.g., "text.tar"
    const char *size_string_response = "0";  // Default response: size "0" (string)
    FILE *tar_archive_file_stream = NULL;    // File pointer for reading tarball
    int maketar_operation_status = 1;        // 1=OK, 0=Fail
    int temporary_directory_created = 0;     // Flag for cleanup logic

    printf("DEBUG (PID %d) MAKETAR: Handling MAKETAR command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse File Type */
    // Extract file type argument
    if (sscanf(command_params, "%9s", filetype_parameter_arg) != 1)
    {
        fprintf(stderr, "[S%d_MAKETAR_ERROR] Invalid MAKETAR parameters.\n", SERVER_NUM);
        maketar_operation_status = 0;
        goto send_maketar_response; // Send size "0" string on error
    }
    printf("DEBUG (PID %d) MAKETAR: Parsed Filetype Argument: '%s'\n", getpid(), filetype_parameter_arg);

    /* 2. Validate File Type for THIS Server (S3) */
    // Set expected pattern and filename based on SERVER_NUM (should be 3)
    if (SERVER_NUM == 3 && strcasecmp(filetype_parameter_arg, ".txt") == 0)
    {
        strcpy(expected_file_name_pattern, "*.txt");
        strcpy(expected_tar_archive_name, "text.tar");
    }
    else
    {
        // File type doesn't match what S3 handles, or SERVER_NUM is wrong.
        fprintf(stderr, "[S%d_MAKETAR_ERROR] Invalid file type '%s' requested for server S%d.\n",
                SERVER_NUM, filetype_parameter_arg, SERVER_NUM);
        // Protocol: send "0" size string.
        maketar_operation_status = 0;
        goto send_maketar_response;
    }
    printf("DEBUG (PID %d) MAKETAR: Server S%d handling type '%s'. Pattern: '%s', Tar File: '%s'.\n",
           getpid(), SERVER_NUM, filetype_parameter_arg, expected_file_name_pattern, expected_tar_archive_name);

    /* 3. Get Home Directory */
    home_directory_env_value = getenv("HOME");
    // Check getenv result
    if (home_directory_env_value == NULL)
    {
        perror("[S%d_MAKETAR_ERROR] getenv(\"HOME\") failed");
        maketar_operation_status = 0;
        goto send_maketar_response; // Send size "0"
    }

    /* 4. Create Temporary Directory */
    // Construct path like /home/user/s3_temp_tar_12345
    snprintf(temporary_working_directory, sizeof(temporary_working_directory), "%s/s%d_temp_tar_%d", home_directory_env_value, SERVER_NUM, getpid());
    // Construct mkdir command
    snprintf(system_command_buffer, sizeof(system_command_buffer), "mkdir -p \"%s\"", temporary_working_directory);
    printf("DEBUG (PID %d) MAKETAR: Creating temporary directory: %s\n", getpid(), system_command_buffer);
    // Execute mkdir
    int mkdir_exit_code = system(system_command_buffer);
    // Check result
    if (mkdir_exit_code != 0)
    {
        fprintf(stderr, "[S%d_MAKETAR_ERROR] Failed to create temporary directory '%s' (Code: %d).\n",
                SERVER_NUM, temporary_working_directory, mkdir_exit_code);
        maketar_operation_status = 0;
        goto send_maketar_response; // Send size "0"
    }
    temporary_directory_created = 1; // Mark that cleanup is needed

    /* 5. Construct and Execute Tar Command */
    // Construct path for the output tar file (e.g., /home/user/s3_temp_tar_12345/text.tar)
    snprintf(local_tar_archive_filepath, sizeof(local_tar_archive_filepath), "%s/%s", temporary_working_directory, expected_tar_archive_name);
    // Construct the find + tar command pipeline
    // find ~/S<N> -name "*.ext" -type f -print0 | xargs -0 tar -cf /path/to/temp/file.tar 2>/dev/null
    snprintf(system_command_buffer, sizeof(system_command_buffer),
             "find %s/S%d -name \"%s\" -type f -print0 | xargs -0 tar -cf \"%s\" 2>/dev/null",
             home_directory_env_value, SERVER_NUM, expected_file_name_pattern, local_tar_archive_filepath);
    printf("DEBUG (PID %d) MAKETAR: Executing tar creation command: %s\n", getpid(), system_command_buffer);
    // Execute the command
    int tar_command_exit_code = system(system_command_buffer);
    // Check exit code - non-zero might just mean no files found. Stat will verify.
    if (tar_command_exit_code != 0)
    {
        fprintf(stderr, "[S%d_MAKETAR_WARN] Tar command pipeline exited with status %d.\n", SERVER_NUM, tar_command_exit_code);
    }

    /* 6. Check Tar File Existence and Size */
    printf("DEBUG (PID %d) MAKETAR: Checking resulting tar file '%s'...\n", getpid(), local_tar_archive_filepath);
    struct stat tar_file_status_info;
    long tar_file_actual_size = 0; // Use long for size
    // Use stat() to check if file exists and get its status
    if (stat(local_tar_archive_filepath, &tar_file_status_info) == 0)
    {
        // Stat succeeded, file exists. Check size.
        if (tar_file_status_info.st_size > 0)
        {
            tar_file_actual_size = tar_file_status_info.st_size; // Store the size
            printf("DEBUG (PID %d) MAKETAR: Tar file created successfully. Size: %ld bytes.\n", getpid(), tar_file_actual_size);
        }
        else
        {
            // File exists but is empty (0 bytes) - likely no matching files found.
            printf("[S%d_MAKETAR_INFO] Tar file is empty (0 bytes). No .txt files found?\n", SERVER_NUM);
            tar_file_actual_size = 0; // Ensure size is 0
        }
    }
    else
    {
        // Stat failed, file likely doesn't exist (tar command failed or found nothing).
        perror("[S%d_MAKETAR_WARN] stat failed on tar file (assuming no files found)");
        printf("[S%d_MAKETAR_INFO] Tar file '%s' not found or inaccessible.\n", SERVER_NUM, local_tar_archive_filepath);
        tar_file_actual_size = 0; // Treat as size 0
    }

    /* 7. Prepare Size Response String */
    char actual_size_as_string[32];
    // Convert the determined size (0 or >0) to a string
    snprintf(actual_size_as_string, sizeof(actual_size_as_string), "%ld", tar_file_actual_size);
    size_string_response = actual_size_as_string; // Update the response pointer

send_maketar_response:
    /* 8. Send Size String to S1 */
    printf("DEBUG (PID %d) MAKETAR: Sending size string '%s' to S1...\n", getpid(), size_string_response);
    ssize_t bytes_written_size_resp = write(s1_socket_fd, size_string_response, strlen(size_string_response));
    // Check if write failed or was incomplete
    if (bytes_written_size_resp < (ssize_t)strlen(size_string_response))
    {
        perror("[S%d_MAKETAR_ERROR] Failed sending size string to S1");
        maketar_operation_status = 0;   // Mark failure
        goto cleanup_maketar_resources; // Cannot proceed to send content
    }
    printf("DEBUG (PID %d) MAKETAR: Size string sent.\n", getpid());

    /* 9. Send Tar Content (only if size > 0 and operation was successful so far) */
    // Need to re-check size based on the string we sent (or the long variable)
    long size_to_send = atol(size_string_response); // Re-parse size string just to be sure
    if (maketar_operation_status == 1 && size_to_send > 0)
    {
        printf("DEBUG (PID %d) MAKETAR: Opening tar file '%s' to send %ld bytes of content...\n",
               getpid(), local_tar_archive_filepath, size_to_send);
        tar_archive_file_stream = fopen(local_tar_archive_filepath, "rb"); // Read binary
        // Check fopen result
        if (tar_archive_file_stream == NULL)
        {
            perror("[S%d_MAKETAR_ERROR] Failed to open created tar file for reading");
            // We already sent the size, S1 might hang. Close connection abruptly.
            maketar_operation_status = 0;
        }
        else
        {
            // File opened, proceed to send content
            char tar_data_buffer[FILE_CHUNK_BUFFER_SIZE];
            size_t bytes_read_this_chunk;
            size_t total_bytes_sent_tar = 0;
            // Loop reading chunks and writing them to socket
            while ((bytes_read_this_chunk = fread(tar_data_buffer, 1, sizeof(tar_data_buffer), tar_archive_file_stream)) > 0)
            {
                ssize_t bytes_written_this_chunk = write(s1_socket_fd, tar_data_buffer, bytes_read_this_chunk);
                // Check for write error or incomplete write
                if (bytes_written_this_chunk < 0 || (size_t)bytes_written_this_chunk != bytes_read_this_chunk)
                {
                    perror("[S%d_MAKETAR_ERROR] Failed writing tar content chunk to S1");
                    maketar_operation_status = 0; // Mark failure
                    break;                        // Stop sending
                }
                total_bytes_sent_tar += bytes_written_this_chunk;
            }
            // Check for fread error
            if (ferror(tar_archive_file_stream))
            {
                perror("[S%d_MAKETAR_ERROR] Error reading tar file content during send");
                maketar_operation_status = 0; // Mark failure
            }
            // Close the tar file stream
            fclose(tar_archive_file_stream);
            tar_archive_file_stream = NULL; // Mark closed
            printf("DEBUG (PID %d) MAKETAR: Finished sending tar content (%zu bytes sent).\n", getpid(), total_bytes_sent_tar);
        }
    }
    else
    {
        // Size was "0" or an earlier error occurred, no content to send.
        printf("DEBUG (PID %d) MAKETAR: Skipping content sending (Size was 0 or prior error).\n", getpid());
    }

cleanup_maketar_resources:
    // Clean up the temporary directory if it was created
    if (temporary_directory_created == 1)
    {
        // Construct cleanup command: rm -rf /path/to/temp
        snprintf(system_cleanup_command_buffer, sizeof(system_cleanup_command_buffer), "rm -rf \"%s\"", temporary_working_directory);
        printf("DEBUG (PID %d) MAKETAR: Cleaning up temporary directory: %s\n", getpid(), system_cleanup_command_buffer);
        // Execute cleanup, ignore errors for cleanup.
        int cleanup_exit_code = system(system_cleanup_command_buffer);
        if (cleanup_exit_code != 0)
        {
            fprintf(stderr, "[S%d_MAKETAR_WARN] Temporary directory cleanup failed (Code: %d).\n", SERVER_NUM, cleanup_exit_code);
        }
    }
    // Ensure file stream is closed if loop broke early
    if (tar_archive_file_stream != NULL)
    {
        fclose(tar_archive_file_stream);
    }
    // Protocol: Close connection after sending data or size "0"
    printf("DEBUG (PID %d) MAKETAR: Closing connection to S1 (Status: %d).\n", getpid(), maketar_operation_status);
    close(s1_socket_fd);

    printf("DEBUG (PID %d) MAKETAR: Exiting handle_maketar_command.\n", getpid());

} // End handle_maketar_command
