/* ======================================================== */
/*          S4.c - Secondary ZIP Server                     */
/* ======================================================== */
/* This server is part of my Distributed File System project. */
/* It listens for requests specifically from the S1 server. */
/* Its main responsibility is managing .zip files.          */

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

/* --- Server Identity --- */
// This #define MUST be set correctly for each secondary server.
// 2 for PDF server (S2)
// 3 for TXT server (S3)
// 4 for ZIP server (S4)
#define SERVER_NUM 4 // This is server S4

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
    // This server needs its own directory (e.g., ~/S4) to store files.
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
    int server_operational_state = 1; // Redundant flag for loop control
    printf("DEBUG: Entering main accept loop to wait for S1 connections...\n");
    // This loop runs forever, accepting and handling connections.
    while (server_operational_state) // Use flag instead of `while(1)`
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
        pid_t child_proc_id = fork(); // fork() returns PID of child to parent, 0 to child, -1 on error.

        // Check the result of fork().
        if (child_proc_id < 0)
        {
            // Fork failed (e.g., system resource limits reached).
            perror("[S%d_MAIN_ERROR] Fork failed");
            // Close the socket we just accepted, as no child will handle it.
            close(s1_connection_socket_fd);
            // The parent continues running, trying to accept new connections.
        }
        else if (child_proc_id == 0)
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
        else // child_proc_id > 0
        {
            /* --- Parent Process Logic --- */
            // This block is executed ONLY by the parent process after a successful fork.
            printf("DEBUG (PID %d): Parent created child process (PID %d) for S1 fd %d.\n", getpid(), child_proc_id, s1_connection_socket_fd);

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
    char s1_command_received_buffer[1024]; // Buffer to hold the command from S1
    ssize_t bytes_read_from_s1;            // Result of the read() call
    int request_processing_ok = 1;         // Redundant flag: 1=OK, 0=Fail

    printf("DEBUG (PID %d): process_s1_connection started for fd %d.\n", getpid(), s1_socket_descriptor);

    // --- Read the command from S1 ---
    // We expect S1 to send one command and then potentially data.
    printf("DEBUG (PID %d): Waiting to read command from S1...\n", getpid());
    memset(s1_command_received_buffer, 0, sizeof(s1_command_received_buffer));                                           // Clear buffer first
    bytes_read_from_s1 = read(s1_socket_descriptor, s1_command_received_buffer, sizeof(s1_command_received_buffer) - 1); // Leave space for '\0'

    // Check read result
    if (bytes_read_from_s1 < 0)
    {
        perror("[S%d_CHILD_ERROR] Failed to read command from S1");
        request_processing_ok = 0; // Mark failure
        // Fall through to exit (socket will be closed by exit or caller)
    }
    else if (bytes_read_from_s1 == 0)
    {
        fprintf(stderr, "[S%d_CHILD_WARN] S1 closed connection before sending command.\n", SERVER_NUM);
        request_processing_ok = 0; // Mark failure
        // Fall through to exit
    }
    else // Read successful
    {
        // Null-terminate the received command string
        s1_command_received_buffer[bytes_read_from_s1] = '\0';
        printf("DEBUG (PID %d): Received command from S1: \"%s\"\n", getpid(), s1_command_received_buffer);

        // --- Parse and Dispatch Command ---
        // Find the first space to separate command verb from parameters
        char *command_action_verb = s1_command_received_buffer;                    // Start of buffer is verb
        char *command_action_parameters = strchr(s1_command_received_buffer, ' '); // Find first space
        // If space found, split the string
        if (command_action_parameters != NULL)
        {
            *command_action_parameters = '\0'; // Place null terminator after verb
            command_action_parameters++;       // Move parameters pointer past the new null term (was space)
        }
        else
        {
            // No space found, means no parameters
            command_action_parameters = ""; // Point to an empty string for parameters
        }

        printf("DEBUG (PID %d): Parsed Verb: '%s', Params: '%s'\n", getpid(), command_action_verb, command_action_parameters);

        // Call the appropriate handler based on the command verb
        if (strcmp(command_action_verb, "STORE") == 0)
        {
            handle_store_command(s1_socket_descriptor, command_action_parameters);
        }
        else if (strcmp(command_action_verb, "GETFILES") == 0)
        {
            handle_getfiles_command(s1_socket_descriptor, command_action_parameters);
        }
        else if (strcmp(command_action_verb, "RETRIEVE") == 0)
        {
            handle_retrieve_command(s1_socket_descriptor, command_action_parameters);
        }
        else if (strcmp(command_action_verb, "REMOVE") == 0)
        {
            handle_remove_command(s1_socket_descriptor, command_action_parameters);
        }
        else if (strcmp(command_action_verb, "MAKETAR") == 0)
        {
            handle_maketar_command(s1_socket_descriptor, command_action_parameters);
        }
        else
        {
            // Unknown command received
            fprintf(stderr, "[S%d_CHILD_ERROR] Received unknown command from S1: '%s'\n", SERVER_NUM, command_action_verb);
            const char *response_unknown_cmd = "ERROR:UNKNOWN_COMMAND";
            // Try to send error back to S1
            ssize_t write_check_unknown = write(s1_socket_descriptor, response_unknown_cmd, strlen(response_unknown_cmd));
            if (write_check_unknown < 0)
            {
                perror("[S%d_CHILD_WARN] Failed sending UNKNOWN_COMMAND error to S1");
            }
            request_processing_ok = 0; // Mark failure
        }
    }

    // This function implicitly returns after handling one command.
    // The child process will then exit in the main loop's child block.
    printf("DEBUG (PID %d): process_s1_connection finished (Request OK Flag: %d).\n", getpid(), request_processing_ok);

} // End of process_s1_connection

/* ======================================================== */
/*             Command Handling Functions                   */
/* ======================================================== */

/**
 * @brief Handles the 'STORE' command from S1.
 * Receives a ZIP file and saves it locally in the appropriate S4 directory.
 * Protocol:
 * 1. S1 sends "STORE <filename.zip> <~S1/dest_path> <size>"
 * 2. S4 creates directory if needed.
 * 3. S4 sends "READY".
 * 4. S1 sends <size> bytes of file content.
 * 5. S4 reads content and saves to file.
 * 6. S4 sends "SUCCESS" or "ERROR:<reason>".
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "STORE" (filename, dest_path, size).
 */
void handle_store_command(int s1_socket_fd, const char *command_params)
{
    char received_zip_filename[256];                     // Filename S1 sends (e.g., "archive.zip")
    char original_s1_destination_path[256];              // Path S1 provides (~S1/...)
    unsigned long expected_zip_file_size = 0;            // Size from S1 command
    int params_parsed_count = 0;                         // Result of sscanf
    char *server_local_homedir = NULL;                   // Path to S4's home dir
    char local_s4_filepath_final[512];                   // Absolute path for saving file locally (e.g., ~/S4/...)
    char local_s4_directory_final[512];                  // Absolute path for the directory containing the file
    char sys_mkdir_command_buffer[1024];                 // Buffer for mkdir command
    FILE *local_zip_file_output_stream = NULL;           // File pointer for writing the received file
    const char *response_msg_to_s1 = "ERROR:STORE_INIT"; // Default error message
    int store_op_has_failed = 0;                         // 0=OK, 1=Failed

    printf("DEBUG (PID %d) STORE: Handling STORE command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse Parameters */
    printf("DEBUG (PID %d) STORE: Parsing parameters...\n", getpid());
    // Use sscanf to extract the three parts: filename, path, size
    params_parsed_count = sscanf(command_params, "%255s %255s %lu",
                                 received_zip_filename, original_s1_destination_path, &expected_zip_file_size);
    // Check if exactly 3 items were parsed
    if (params_parsed_count != 3)
    {
        fprintf(stderr, "[S%d_STORE_ERROR] Invalid STORE parameters. Expected 3 items, got %d.\n", SERVER_NUM, params_parsed_count);
        response_msg_to_s1 = "ERROR:INVALID_STORE_PARAMS";
        store_op_has_failed = 1;
        goto send_store_response_to_s1; // Use goto for unified cleanup/response
    }
    // Cast size to size_t
    size_t expected_zip_byte_count = (size_t)expected_zip_file_size;
    printf("DEBUG (PID %d) STORE: Parsed OK -> File='%s', S1_Dest='%s', Size=%zu\n",
           getpid(), received_zip_filename, original_s1_destination_path, expected_zip_byte_count);

    /* 2. Get Home Directory */
    printf("DEBUG (PID %d) STORE: Getting HOME directory...\n", getpid());
    server_local_homedir = getenv("HOME");
    // Check if getenv failed
    if (server_local_homedir == NULL)
    {
        perror("[S%d_STORE_ERROR] getenv(\"HOME\") failed");
        response_msg_to_s1 = "ERROR:SERVER_CANNOT_GET_HOME";
        store_op_has_failed = 1;
        goto send_store_response_to_s1;
    }
    printf("DEBUG (PID %d) STORE: Server HOME directory: '%s'.\n", getpid(), server_local_homedir);

    /* 3. Construct Local S4 Paths */
    printf("DEBUG (PID %d) STORE: Constructing local S%d file paths...\n", getpid(), SERVER_NUM);
    // Validate the path prefix from S1
    if (strncmp(original_s1_destination_path, "~S1/", 4) != 0)
    {
        fprintf(stderr, "[S%d_STORE_ERROR] Received destination path missing ~S1/ prefix: '%s'\n",
                SERVER_NUM, original_s1_destination_path);
        response_msg_to_s1 = "ERROR:BAD_S1_PATH_PREFIX";
        store_op_has_failed = 1;
        goto send_store_response_to_s1;
    }
    // Get the part after "~S1/" (e.g., "data/archive.zip")
    char *relative_path_from_s1 = original_s1_destination_path + 4;
    // Find the last slash to separate directory and filename parts
    char *last_slash_location = strrchr(relative_path_from_s1, '/');

    // Determine the local S4 directory and full file path
    // Using if/else structure
    if (last_slash_location == NULL || last_slash_location == relative_path_from_s1)
    {
        // Case: No directory specified, or starts with '/' (e.g., "archive.zip", "/archive.zip")
        // Directory is ~/S<N> (e.g., ~/S4)
        snprintf(local_s4_directory_final, sizeof(local_s4_directory_final), "%s/S%d", server_local_homedir, SERVER_NUM);
        // Full path is ~/S<N>/archive.zip
        snprintf(local_s4_filepath_final, sizeof(local_s4_filepath_final), "%s/S%d/%s", server_local_homedir, SERVER_NUM, relative_path_from_s1);
    }
    else
    {
        // Case: Directory specified (e.g., "data/archive.zip")
        // Directory is ~/S<N>/data
        snprintf(local_s4_directory_final, sizeof(local_s4_directory_final), "%s/S%d/%.*s",
                 server_local_homedir, SERVER_NUM, (int)(last_slash_location - relative_path_from_s1), relative_path_from_s1);
        // Full path is ~/S<N>/data/archive.zip
        snprintf(local_s4_filepath_final, sizeof(local_s4_filepath_final), "%s/S%d/%s", server_local_homedir, SERVER_NUM, relative_path_from_s1);
    }
    printf("DEBUG (PID %d) STORE: Local S%d Directory Path: '%s'\n", getpid(), SERVER_NUM, local_s4_directory_final);
    printf("DEBUG (PID %d) STORE: Local S%d File Path: '%s'\n", getpid(), SERVER_NUM, local_s4_filepath_final);

    /* 4. Create Directory Structure if needed */
    // Use `mkdir -p` via system() call
    snprintf(sys_mkdir_command_buffer, sizeof(sys_mkdir_command_buffer), "mkdir -p \"%s\"", local_s4_directory_final);
    printf("DEBUG (PID %d) STORE: Executing directory creation: %s\n", getpid(), sys_mkdir_command_buffer);
    int mkdir_status_code = system(sys_mkdir_command_buffer);
    // Check the return code
    if (mkdir_status_code != 0)
    {
        // Log warning, but continue; fopen will fail if dir creation really failed badly.
        fprintf(stderr, "[S%d_STORE_WARN] system('%s') returned status %d.\n",
                SERVER_NUM, sys_mkdir_command_buffer, mkdir_status_code);
    }
    else
    {
        printf("DEBUG (PID %d) STORE: Directory creation command completed.\n", getpid());
    }

    /* 5. Send "READY" signal to S1 */
    const char *ready_message = "READY";
    printf("DEBUG (PID %d) STORE: Sending '%s' signal to S1...\n", getpid(), ready_message);
    ssize_t written_ready_bytes = write(s1_socket_fd, ready_message, strlen(ready_message));
    // Check if write failed or was incomplete
    if (written_ready_bytes < (ssize_t)strlen(ready_message))
    {
        perror("[S%d_STORE_ERROR] Failed sending READY signal to S1");
        store_op_has_failed = 1;
        response_msg_to_s1 = "ERROR:CANNOT_SEND_READY"; // Set specific error code
        // Cannot send final response if this write fails, just cleanup and exit child.
        goto cleanup_store_file_resources;
    }
    printf("DEBUG (PID %d) STORE: READY signal sent okay.\n", getpid());

    /* 6. Open Local File for Writing */
    printf("DEBUG (PID %d) STORE: Opening local file '%s' for writing (binary)...\n", getpid(), local_s4_filepath_final);
    local_zip_file_output_stream = fopen(local_s4_filepath_final, "wb"); // Write binary mode
    // Check fopen result
    if (local_zip_file_output_stream == NULL)
    {
        // *** CRITICAL: Protocol requires reading data even if fopen fails! ***
        // Log the error, proceed to read/discard data, set error response later.
        perror("[S%d_STORE_ERROR] fopen failed for local file");
        fprintf(stderr, "[S%d_STORE_ERROR] Cannot open/create '%s'. Will read/discard S1 data.\n",
                SERVER_NUM, local_s4_filepath_final);
        // Do NOT set store_op_has_failed = 1 yet.
    }
    else
    {
        printf("DEBUG (PID %d) STORE: Local file opened successfully.\n", getpid());
    }

    /* 7. Receive File Data from S1 */
    char file_receive_buffer[FILE_CHUNK_BUFFER_SIZE]; // Chunk buffer
    size_t received_bytes_total = 0;                  // Counter
    int local_write_failed_flag = 0;                  // Flag if fwrite fails

    printf("DEBUG (PID %d) STORE: Starting loop to receive %zu bytes from S1...\n", getpid(), expected_zip_byte_count);
    // Loop until expected size is received or error
    while (received_bytes_total < expected_zip_byte_count)
    {
        // Calculate bytes to read in this iteration
        size_t bytes_to_read_this_time = FILE_CHUNK_BUFFER_SIZE;
        size_t bytes_remaining_total = expected_zip_byte_count - received_bytes_total;
        if (bytes_remaining_total < bytes_to_read_this_time)
        {
            bytes_to_read_this_time = bytes_remaining_total;
        }

        // Read data chunk from S1 socket
        ssize_t bytes_actually_read = read(s1_socket_fd, file_receive_buffer, bytes_to_read_this_time);

        // Check read result
        if (bytes_actually_read < 0)
        { // Read error
            perror("[S%d_STORE_ERROR] read() failed during data reception from S1");
            store_op_has_failed = 1;
            response_msg_to_s1 = "ERROR:S1_DATA_READ_FAIL";
            goto cleanup_store_file_resources; // Jump to cleanup
        }
        if (bytes_actually_read == 0)
        { // S1 disconnected prematurely
            fprintf(stderr, "[S%d_STORE_ERROR] S1 closed connection during data transfer (got %zu/%zu bytes)\n",
                    SERVER_NUM, received_bytes_total, expected_zip_byte_count);
            store_op_has_failed = 1;
            response_msg_to_s1 = "ERROR:S1_EARLY_CLOSE";
            goto cleanup_store_file_resources; // Jump to cleanup
        }

        // --- Write received data to local file (if possible) ---
        if (local_zip_file_output_stream != NULL && local_write_failed_flag == 0)
        {
            size_t bytes_actually_written = fwrite(file_receive_buffer, 1, bytes_actually_read, local_zip_file_output_stream);
            // Check fwrite result
            if (bytes_actually_written != (size_t)bytes_actually_read)
            {
                perror("[S%d_STORE_ERROR] fwrite() failed writing to local file");
                fprintf(stderr, "[S%d_STORE_ERROR] Failed writing chunk to '%s'.\n", SERVER_NUM, local_s4_filepath_final);
                local_write_failed_flag = 1; // Set flag
                // IMPORTANT: Keep reading from S1 socket!
            }
        }

        // Add bytes read from socket to total count
        received_bytes_total += bytes_actually_read;

    } // End of while loop receiving data

    printf("DEBUG (PID %d) STORE: Finished receiving loop. Total received: %zu bytes\n", getpid(), received_bytes_total);

    /* 8. Determine Final Status and Prepare Response */
    // Check status only if no network error occurred yet
    if (store_op_has_failed == 0)
    {
        if (local_zip_file_output_stream == NULL)
        { // fopen failed earlier
            store_op_has_failed = 1;
            response_msg_to_s1 = "ERROR:FILE_OPEN_FAILED";
            printf("[S%d_STORE_ERROR] Final check: fopen had failed.\n", SERVER_NUM);
        }
        else if (local_write_failed_flag == 1)
        { // fwrite failed earlier
            store_op_has_failed = 1;
            response_msg_to_s1 = "ERROR:FILE_WRITE_FAILED";
            printf("[S%d_STORE_ERROR] Final check: fwrite had failed.\n", SERVER_NUM);
            // File is corrupt, cleanup happens below.
        }
        else if (received_bytes_total != expected_zip_byte_count)
        { // Size mismatch
            store_op_has_failed = 1;
            response_msg_to_s1 = "ERROR:TRANSFER_SIZE_MISMATCH";
            fprintf(stderr, "[S%d_STORE_ERROR] Final check: Size mismatch (Expected %zu, Got %zu).\n",
                    SERVER_NUM, expected_zip_byte_count, received_bytes_total);
            // File is incomplete, cleanup happens below.
        }
        else
        {
            // All good - SUCCESS!
            store_op_has_failed = 0; // Mark success
            response_msg_to_s1 = "SUCCESS";
            printf("DEBUG (PID %d) STORE: File stored successfully: '%s'.\n", getpid(), local_s4_filepath_final);
            // Close the successfully written file now.
            if (fclose(local_zip_file_output_stream) != 0)
            {
                perror("[S%d_STORE_WARN] fclose failed after successful write, but reporting SUCCESS");
            }
            local_zip_file_output_stream = NULL; // Mark closed to prevent double close
        }
    }
    // If store_op_has_failed was already 1, response_msg_to_s1 is already set.

cleanup_store_file_resources:
    // Cleanup file handle and potentially remove partial file
    if (local_zip_file_output_stream != NULL)
    {
        fclose(local_zip_file_output_stream);
        local_zip_file_output_stream = NULL; // Avoid double close
        // If operation failed AFTER file was opened, remove partial/corrupt file.
        if (store_op_has_failed == 1 && local_s4_filepath_final[0] != '\0')
        {
            printf("DEBUG (PID %d) STORE: Operation failed. Removing potentially bad file '%s'\n", getpid(), local_s4_filepath_final);
            if (remove(local_s4_filepath_final) != 0 && errno != ENOENT)
            {
                perror("[S%d_STORE_WARN] Failed removing partial file in cleanup");
            }
        }
    }

send_store_response_to_s1:
    // Send final response (SUCCESS or ERROR:...) to S1, unless write failed before content transfer.
    if (response_msg_to_s1 != NULL && strcmp(response_msg_to_s1, "ERROR:CANNOT_SEND_READY") != 0)
    {
        printf("DEBUG (PID %d) STORE: Sending final response to S1: '%s'\n", getpid(), response_msg_to_s1);
        ssize_t final_write_check = write(s1_socket_fd, response_msg_to_s1, strlen(response_msg_to_s1));
        if (final_write_check < 0)
        {
            perror("[S%d_STORE_ERROR] Failed sending final STORE response to S1");
        }
    }
    else if (response_msg_to_s1 == NULL)
    {
        // Fallback if message is somehow NULL
        fprintf(stderr, "[S%d_STORE_ERROR] Internal Error: response_msg_to_s1 is NULL!\n", SERVER_NUM);
        write(s1_socket_fd, "ERROR:NULL_RESP", 15);
    }

    printf("DEBUG (PID %d) STORE: Exiting handle_store_command (Failed=%d).\n", getpid(), store_op_has_failed);

} // End handle_store_command

/**
 * @brief Handles the 'GETFILES' command from S1.
 * Finds ZIP files in the specified directory, sorts them, and sends the list back.
 * Protocol:
 * 1. S1 sends "GETFILES <~S1/pathname>"
 * 2. S4 finds matching ZIP files locally (e.g., in ~/S4/pathname).
 * 3. S4 sends the sorted, newline-separated list of filenames (basenames only).
 * 4. S4 closes the connection to signal the end of the list.
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "GETFILES" (the pathname).
 */
void handle_getfiles_command(int s1_socket_fd, const char *command_params)
{
    char s1_pathname_argument[256];          // Path argument from S1
    char local_s4_directory_absolute[512];   // Absolute path for local find
    char *home_directory_path = NULL;        // Server's home directory
    char system_find_sort_command[1024];     // Buffer for find/sort command
    char temporary_file_list_path[256];      // Path for temporary file list
    FILE *temporary_list_file_stream = NULL; // File pointer for temp list
    int getfiles_success_flag = 1;           // 1 = OK, 0 = Fail

    printf("DEBUG (PID %d) GETFILES: Handling GETFILES command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse Path Parameter */
    if (sscanf(command_params, "%255s", s1_pathname_argument) != 1)
    {
        fprintf(stderr, "[S%d_GETFILES_ERROR] Invalid GETFILES parameters. Expected path.\n", SERVER_NUM);
        // Protocol: Close connection on error.
        getfiles_success_flag = 0;
        goto cleanup_getfiles_op; // Use goto for cleanup
    }
    printf("DEBUG (PID %d) GETFILES: Parsed Pathname: '%s'\n", getpid(), s1_pathname_argument);

    /* 2. Get Home Directory */
    home_directory_path = getenv("HOME");
    if (home_directory_path == NULL)
    {
        perror("[S%d_GETFILES_ERROR] getenv(\"HOME\") failed");
        getfiles_success_flag = 0;
        goto cleanup_getfiles_op;
    }

    /* 3. Construct Local S4 Directory Path */
    if (strncmp(s1_pathname_argument, "~S1/", 4) != 0)
    {
        fprintf(stderr, "[S%d_GETFILES_ERROR] Pathname argument missing ~S1/ prefix: '%s'\n", SERVER_NUM, s1_pathname_argument);
        getfiles_success_flag = 0;
        goto cleanup_getfiles_op;
    }
    // Construct local S4 path (e.g., /home/user/S4/folder/)
    snprintf(local_s4_directory_absolute, sizeof(local_s4_directory_absolute), "%s/S%d/%s",
             home_directory_path, SERVER_NUM, s1_pathname_argument + 4); // Skip "~S1/"
    // Remove trailing slash for `find` consistency
    size_t path_current_len = strlen(local_s4_directory_absolute);
    if (path_current_len > 1 && local_s4_directory_absolute[path_current_len - 1] == '/')
    {
        local_s4_directory_absolute[path_current_len - 1] = '\0';
    }
    printf("DEBUG (PID %d) GETFILES: Local S%d directory path for search: '%s'\n", getpid(), SERVER_NUM, local_s4_directory_absolute);

    /* 4. Check if Local Directory Exists */
    struct stat directory_status_check;
    if (stat(local_s4_directory_absolute, &directory_status_check) != 0 || !S_ISDIR(directory_status_check.st_mode))
    {
        fprintf(stderr, "[S%d_GETFILES_WARN] Local directory not found or not a directory: '%s'. Sending empty list.\n",
                SERVER_NUM, local_s4_directory_absolute);
        // Protocol: Send empty response by closing connection.
        getfiles_success_flag = 1; // Indicate successful empty response
        goto cleanup_getfiles_op;
    }
    printf("DEBUG (PID %d) GETFILES: Local directory found.\n", getpid());

    /* 5. Prepare Temp File Path */
    // Use /tmp for temp file
    snprintf(temporary_file_list_path, sizeof(temporary_file_list_path), "/tmp/s%d_files_%d.txt", SERVER_NUM, getpid());
    printf("DEBUG (PID %d) GETFILES: Using temporary file: '%s'\n", getpid(), temporary_file_list_path);
    // Ensure temp file is clean before use
    remove(temporary_file_list_path); // Ignore error

    /* 6. Execute `find` Command to Get Sorted File List */
    // Command: find <dir> -maxdepth 1 -type f -name "*.zip" -printf "%f\\n" 2>/dev/null | sort > <temp_file>
    snprintf(system_find_sort_command, sizeof(system_find_sort_command),
             "find \"%s\" -maxdepth 1 -type f -name \"*.zip\" -printf \"%%f\\n\" 2>/dev/null | sort > \"%s\"",
             local_s4_directory_absolute, temporary_file_list_path); // Use .zip pattern
    printf("DEBUG (PID %d) GETFILES: Executing find/sort: %s\n", getpid(), system_find_sort_command);
    int find_command_status = system(system_find_sort_command);
    // Check status, but proceed even if non-zero (fopen will handle missing file)
    if (find_command_status != 0)
    {
        fprintf(stderr, "[S%d_GETFILES_WARN] Find/sort command exited with status %d.\n", SERVER_NUM, find_command_status);
    }

    /* 7. Read Temp File and Send Content to S1 */
    printf("DEBUG (PID %d) GETFILES: Reading temp file '%s' and sending results to S1...\n", getpid(), temporary_file_list_path);
    temporary_list_file_stream = fopen(temporary_file_list_path, "r"); // Read mode
    // Check if file exists/readable
    if (temporary_list_file_stream == NULL)
    {
        perror("[S%d_GETFILES_INFO] Cannot open temp file list (no .zip files found?)");
        printf("[S%d_GETFILES_INFO] Sending empty list indication (closing connection).\n", SERVER_NUM);
        getfiles_success_flag = 1; // Successful empty response
        goto cleanup_getfiles_op;  // Close connection
    }

    // Read from temp file, write to socket
    char file_list_read_buffer[FILE_CHUNK_BUFFER_SIZE];
    size_t bytes_read_list_chunk;
    while ((bytes_read_list_chunk = fread(file_list_read_buffer, 1, sizeof(file_list_read_buffer), temporary_list_file_stream)) > 0)
    {
        ssize_t bytes_written_list_chunk = write(s1_socket_fd, file_list_read_buffer, bytes_read_list_chunk);
        // Check for write error or incomplete write
        if (bytes_written_list_chunk < 0 || (size_t)bytes_written_list_chunk != bytes_read_list_chunk)
        {
            perror("[S%d_GETFILES_ERROR] Failed writing file list chunk to S1");
            getfiles_success_flag = 0; // Mark failure
            break;                     // Stop sending
        }
    }
    // Check for read error from temp file
    if (ferror(temporary_list_file_stream))
    {
        perror("[S%d_GETFILES_ERROR] Error reading from temporary file list");
        getfiles_success_flag = 0; // Mark failure
    }

    // Close the temp file stream if opened
    if (temporary_list_file_stream != NULL)
    {
        fclose(temporary_list_file_stream);
        temporary_list_file_stream = NULL;
    }
    printf("DEBUG (PID %d) GETFILES: Finished sending list content to S1.\n", getpid());

cleanup_getfiles_op:
    // Clean up the temporary file
    if (temporary_file_list_path[0] != '\0')
    {
        printf("DEBUG (PID %d) GETFILES: Cleaning up temporary file '%s'.\n", getpid(), temporary_file_list_path);
        remove(temporary_file_list_path); // Ignore errors on remove
    }

    // Protocol: S1 expects connection close after list (or immediately on error/empty).
    printf("DEBUG (PID %d) GETFILES: Closing connection to S1 (Success Flag: %d).\n", getpid(), getfiles_success_flag);
    close(s1_socket_fd); // Close the socket

    printf("DEBUG (PID %d) GETFILES: Exiting handle_getfiles_command.\n", getpid());
    // Child process will exit.

} // End handle_getfiles_command

/**
 * @brief Handles the 'RETRIEVE' command from S1.
 * Finds the requested ZIP file locally and sends its size (string) then content.
 * Protocol:
 * 1. S1 sends "RETRIEVE <~S1/file_path.zip>"
 * 2. S4 looks for the file locally (e.g., ~/S4/file_path.zip).
 * 3. If found: S4 sends "<size_as_string>" followed immediately by file content.
 * 4. If not found: S4 sends "0" (size zero as string).
 * 5. S4 closes connection after sending content or size "0".
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "RETRIEVE" (the file_path).
 */
void handle_retrieve_command(int s1_socket_fd, const char *command_params)
{
    char s1_zip_filepath_arg[256];          
    char local_s4_zip_filepath[512];        
    char *server_home_dir = NULL;           
    FILE *requested_zip_file_handle = NULL; 
    int retrieve_success_indicator = 1;     

    printf("DEBUG (PID %d) RETRIEVE: Handling RETRIEVE command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse File Path Parameter */
    if (sscanf(command_params, "%255s", s1_zip_filepath_arg) != 1)
    {
        fprintf(stderr, "[S%d_RETRIEVE_ERROR] Invalid RETRIEVE parameters.\n", SERVER_NUM);
        // Send binary 0 (4 bytes) for error
        uint32_t zero_size = 0;
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_success_indicator = 0;
        goto cleanup_retrieve_operation;
    }

    /* 2. Get Home Directory */
    server_home_dir = getenv("HOME");
    if (!server_home_dir)
    {
        perror("[S%d_RETRIEVE_ERROR] getenv(\"HOME\") failed");
        uint32_t zero_size = 0;
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_success_indicator = 0;
        goto cleanup_retrieve_operation;
    }

    /* 3. Construct Local S4 File Path */
    if (strncmp(s1_zip_filepath_arg, "~S1/", 4) != 0)
    {
        fprintf(stderr, "[S%d_RETRIEVE_ERROR] Invalid path prefix\n", SERVER_NUM);
        uint32_t zero_size = 0;
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_success_indicator = 0;
        goto cleanup_retrieve_operation;
    }
    snprintf(local_s4_zip_filepath, sizeof(local_s4_zip_filepath), "%s/S%d/%s",
             server_home_dir, SERVER_NUM, s1_zip_filepath_arg + 4);

    /* 4. Open Local File */
    requested_zip_file_handle = fopen(local_s4_zip_filepath, "rb");
    if (requested_zip_file_handle == NULL)
    {
        perror("[S%d_RETRIEVE_WARN] fopen failed");
        uint32_t zero_size = 0;
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_success_indicator = 0;
        goto cleanup_retrieve_operation;
    }

    /* 5. Get File Size */
    long zip_file_size_bytes = -1;
    if (fseek(requested_zip_file_handle, 0, SEEK_END) != 0 || 
        (zip_file_size_bytes = ftell(requested_zip_file_handle)) < 0 ||
        fseek(requested_zip_file_handle, 0, SEEK_SET) != 0)
    {
        perror("[S%d_RETRIEVE_ERROR] File size check failed");
        uint32_t zero_size = 0;
        write(s1_socket_fd, &zero_size, sizeof(zero_size));
        retrieve_success_indicator = 0;
        goto cleanup_retrieve_operation;
    }

    /* 6. Send File Size (binary network order) */
    uint32_t net_size = htonl((uint32_t)zip_file_size_bytes);
    if (write(s1_socket_fd, &net_size, sizeof(net_size)) != sizeof(net_size))
    {
        perror("[S%d_RETRIEVE_ERROR] Size header send failed");
        retrieve_success_indicator = 0;
        goto cleanup_retrieve_operation;
    }

    /* 7. Send File Content */
    char buffer[FILE_CHUNK_BUFFER_SIZE];
    size_t total_sent = 0;
    while (total_sent < (size_t)zip_file_size_bytes)
    {
        size_t read_size = fread(buffer, 1, sizeof(buffer), requested_zip_file_handle);
        if (read_size <= 0) break;

        ssize_t sent = write(s1_socket_fd, buffer, read_size);
        if (sent <= 0) {
            perror("[S%d_RETRIEVE_ERROR] Content send failed");
            retrieve_success_indicator = 0;
            break;
        }
        total_sent += sent;
    }

cleanup_retrieve_operation:
    if (requested_zip_file_handle) fclose(requested_zip_file_handle);
    close(s1_socket_fd); // Protocol requires connection close after transfer
    printf("DEBUG (PID %d) RETRIEVE: Exiting. Success: %d\n", getpid(), retrieve_success_indicator);
}
 // End handle_retrieve_command

/**
 * @brief Handles the 'REMOVE' command from S1.
 * Deletes the specified ZIP file locally after checking it exists.
 * Protocol:
 * 1. S1 sends "REMOVE <~S1/file_path.zip>"
 * 2. S4 checks if local file exists (e.g., ~/S4/file_path.zip).
 * 3. If exists, S4 attempts to remove it.
 * 4. S4 sends "SUCCESS" or "ERROR:<reason>".
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "REMOVE" (the file_path).
 */
void handle_remove_command(int s1_socket_fd, const char *command_params)
{
    char s1_filepath_parameter[256];                       // Path arg from S1
    char local_s4_filepath_absolute[512];                  // Absolute path for local remove
    char *server_home_directory = NULL;                    // Server's home directory
    const char *response_to_send_s1 = "ERROR:REMOVE_INIT"; // Default error
    char error_message_buffer[300];                        // Buffer for specific errors
    int remove_op_failed = 0;                              // 0=OK, 1=Failed

    printf("DEBUG (PID %d) REMOVE: Handling REMOVE command. Params: '%s'\n", getpid(), command_params);

    /* 1. Parse File Path */
    if (sscanf(command_params, "%255s", s1_filepath_parameter) != 1)
    {
        fprintf(stderr, "[S%d_REMOVE_ERROR] Invalid REMOVE parameters.\n", SERVER_NUM);
        response_to_send_s1 = "ERROR:MISSING_REMOVE_PARAM";
        remove_op_failed = 1;
        goto send_remove_result_to_s1; // Use goto for response sending
    }
    printf("DEBUG (PID %d) REMOVE: Parsed Filepath: '%s'\n", getpid(), s1_filepath_parameter);

    /* 2. Get Home Directory */
    server_home_directory = getenv("HOME");
    if (server_home_directory == NULL)
    { // Check NULL
        perror("[S%d_REMOVE_ERROR] getenv(\"HOME\") failed");
        response_to_send_s1 = "ERROR:SERVER_NO_HOME_DIR";
        remove_op_failed = 1;
        goto send_remove_result_to_s1;
    }

    /* 3. Construct Local S4 Path */
    if (strncmp(s1_filepath_parameter, "~S1/", 4) != 0)
    {
        fprintf(stderr, "[S%d_REMOVE_ERROR] Filepath missing ~S1/ prefix: '%s'\n", SERVER_NUM, s1_filepath_parameter);
        response_to_send_s1 = "ERROR:BAD_S1_PATH_PREFIX";
        remove_op_failed = 1;
        goto send_remove_result_to_s1;
    }
    // Construct local S4 path (e.g., /home/user/S4/folder/file.zip)
    snprintf(local_s4_filepath_absolute, sizeof(local_s4_filepath_absolute), "%s/S%d/%s",
             server_home_directory, SERVER_NUM, s1_filepath_parameter + 4); // Skip "~S1/"
    printf("DEBUG (PID %d) REMOVE: Local path for removal: '%s'\n", getpid(), local_s4_filepath_absolute);

    /* 4. Check if File Exists */
    struct stat file_check_status;
    printf("DEBUG (PID %d) REMOVE: Checking file existence via stat()...\n", getpid());
    if (stat(local_s4_filepath_absolute, &file_check_status) != 0)
    {
        // Stat failed. Check errno.
        if (errno == ENOENT)
        { // File Not Found
            printf("[S%d_REMOVE_WARN] File not found: '%s'.\n", SERVER_NUM, local_s4_filepath_absolute);
            snprintf(error_message_buffer, sizeof(error_message_buffer), "ERROR:ZIP_FILE_NOT_FOUND:%s", s1_filepath_parameter);
            response_to_send_s1 = error_message_buffer;
        }
        else
        { // Other stat error (permissions?)
            snprintf(error_message_buffer, sizeof(error_message_buffer), "ERROR:STAT_CHECK_FAILED:%d", errno);
            perror("[S%d_REMOVE_ERROR] stat() failed");
            response_to_send_s1 = error_message_buffer;
        }
        remove_op_failed = 1; // Mark failure
        goto send_remove_result_to_s1;
    }
    // Optional: Check if it's a regular file
    if (!S_ISREG(file_check_status.st_mode))
    {
        fprintf(stderr, "[S%d_REMOVE_ERROR] Path is not a regular file: '%s'\n", SERVER_NUM, local_s4_filepath_absolute);
        snprintf(error_message_buffer, sizeof(error_message_buffer), "ERROR:PATH_NOT_REGULAR_FILE:%s", s1_filepath_parameter);
        response_to_send_s1 = error_message_buffer;
        remove_op_failed = 1;
        goto send_remove_result_to_s1;
    }
    printf("DEBUG (PID %d) REMOVE: File exists. Attempting remove().\n", getpid());

    /* 5. Remove File */
    // Use remove() (or unlink())
    if (remove(local_s4_filepath_absolute) != 0)
    {
        // Remove failed (e.g., permissions)
        snprintf(error_message_buffer, sizeof(error_message_buffer), "ERROR:OS_REMOVE_FAILED:%d", errno);
        perror("[S%d_REMOVE_ERROR] remove() failed");
        response_to_send_s1 = error_message_buffer;
        remove_op_failed = 1;
        goto send_remove_result_to_s1;
    }

    // Remove succeeded!
    printf("[S%d_REMOVE_INFO] Successfully removed local file: '%s'\n", SERVER_NUM, local_s4_filepath_absolute);
    response_to_send_s1 = "SUCCESS"; // Set success message
    remove_op_failed = 0;

send_remove_result_to_s1:
    // Send the final response string to S1
    printf("DEBUG (PID %d) REMOVE: Sending response to S1: '%s'\n", getpid(), response_to_send_s1);
    ssize_t response_write_check = write(s1_socket_fd, response_to_send_s1, strlen(response_to_send_s1));
    if (response_write_check < 0)
    {
        perror("[S%d_REMOVE_ERROR] Failed writing final response to S1");
    }

    printf("DEBUG (PID %d) REMOVE: Exiting handle_remove_command (Failed=%d).\n", getpid(), remove_op_failed);

} // End handle_remove_command

/**
 * @brief Handles the 'MAKETAR' command from S1.
 * Creates a tarball of relevant files (.zip for S4) and sends it back.
 * NOTE: Project PDF indicates `downltar` (and thus MAKETAR on secondary) is NOT for .zip files.
 * This implementation keeps the original code's behavior of handling .zip for MAKETAR.
 *
 * Protocol:
 * 1. S1 sends "MAKETAR .zip"
 * 2. S4 creates tarball (e.g., zip.tar) in a temp dir.
 * 3. If files found/tar created: S4 sends "<size_as_string>" followed by tar content.
 * 4. If no files/error: S4 sends "0" (size zero string).
 * 5. S4 cleans up temp dir.
 * 6. S4 closes connection.
 *
 * @param s1_socket_fd Socket connected to S1.
 * @param command_params Parameters following "MAKETAR" (the filetype, should be ".zip").
 */
void handle_maketar_command(int s1_socket_fd, const char *command_params)
{
    char filetype_arg_received[10];         // File type arg from S1 (".zip")
    char *server_local_home_path = NULL;    // Server's home directory
    char temporary_tar_directory[256];      // Path for temp dir
    char local_tar_output_filepath[512];    // Full path to the zip.tar file
    char system_command_line_buffer[1024];  // Buffer for system() commands
    char cleanup_rm_command_buffer[256];    // Buffer for rm cleanup
    char target_file_pattern[10];           // "*.zip"
    char target_tar_output_name[20];        // "zip.tar"
    const char *size_response_string = "0"; // Default response string is "0"
    FILE *tar_output_file_stream = NULL;    // File pointer for reading tarball
    int maketar_success_flag = 1;           // 1=OK, 0=Fail
    int temp_dir_was_created = 0;           // Flag for cleanup logic

    printf("DEBUG (PID %d) MAKETAR: Handling MAKETAR command. Params: '%s'\n", getpid(), command_params);
    printf("INFO (PID %d) MAKETAR: Note - Project PDF does not specify MAKETAR for .zip, but handling as per original code.\n", getpid());

    /* 1. Parse File Type */
    if (sscanf(command_params, "%9s", filetype_arg_received) != 1)
    {
        fprintf(stderr, "[S%d_MAKETAR_ERROR] Invalid MAKETAR parameters.\n", SERVER_NUM);
        maketar_success_flag = 0;
        goto send_maketar_response_and_cleanup; // Send size "0"
    }
    printf("DEBUG (PID %d) MAKETAR: Parsed Filetype: '%s'\n", getpid(), filetype_arg_received);

    /* 2. Validate File Type for THIS Server (S4) */
    // S4 only handles .zip for this command (based on original code)
    if (SERVER_NUM == 4 && strcasecmp(filetype_arg_received, ".zip") == 0)
    {
        strcpy(target_file_pattern, "*.zip");
        strcpy(target_tar_output_name, "zip.tar");
    }
    else
    {
        // File type doesn't match .zip or SERVER_NUM is wrong.
        fprintf(stderr, "[S%d_MAKETAR_ERROR] Invalid file type '%s' requested for server S%d.\n",
                SERVER_NUM, filetype_arg_received, SERVER_NUM);
        // Protocol: send "0" size string.
        maketar_success_flag = 0;
        goto send_maketar_response_and_cleanup;
    }
    printf("DEBUG (PID %d) MAKETAR: Server S%d handling type '%s'. Pattern: '%s', Tar File: '%s'.\n",
           getpid(), SERVER_NUM, filetype_arg_received, target_file_pattern, target_tar_output_name);

    /* 3. Get Home Directory */
    server_local_home_path = getenv("HOME");
    if (server_local_home_path == NULL)
    {
        perror("[S%d_MAKETAR_ERROR] getenv(\"HOME\") failed");
        maketar_success_flag = 0;
        goto send_maketar_response_and_cleanup; // Send size "0"
    }

    /* 4. Create Temporary Directory */
    // Construct path like /home/user/s4_temp_tar_12345
    snprintf(temporary_tar_directory, sizeof(temporary_tar_directory), "%s/s%d_temp_tar_%d", server_local_home_path, SERVER_NUM, getpid());
    // Construct mkdir command
    snprintf(system_command_line_buffer, sizeof(system_command_line_buffer), "mkdir -p \"%s\"", temporary_tar_directory);
    printf("DEBUG (PID %d) MAKETAR: Creating temp dir: %s\n", getpid(), system_command_line_buffer);
    // Execute mkdir
    int mkdir_return_val = system(system_command_line_buffer);
    if (mkdir_return_val != 0)
    {
        fprintf(stderr, "[S%d_MAKETAR_ERROR] Failed to create temporary directory '%s' (Return: %d).\n",
                SERVER_NUM, temporary_tar_directory, mkdir_return_val);
        maketar_success_flag = 0;
        goto send_maketar_response_and_cleanup; // Send size "0"
    }
    temp_dir_was_created = 1; // Mark that cleanup is needed

    /* 5. Construct and Execute Tar Command */
    // Construct path for the output tar file (e.g., /home/user/s4_temp_tar_12345/zip.tar)
    snprintf(local_tar_output_filepath, sizeof(local_tar_output_filepath), "%s/%s", temporary_tar_directory, target_tar_output_name);
    // Construct the find + tar command pipeline
    // find ~/S<N> -name "*.zip" -type f -print0 | xargs -0 tar -cf /path/to/temp/zip.tar 2>/dev/null
    snprintf(system_command_line_buffer, sizeof(system_command_line_buffer),
             "find %s/S%d -name \"%s\" -type f -print0 | xargs -0 tar -cf \"%s\" 2>/dev/null",
             server_local_home_path, SERVER_NUM, target_file_pattern, local_tar_output_filepath);
    printf("DEBUG (PID %d) MAKETAR: Executing tar command: %s\n", getpid(), system_command_line_buffer);
    // Execute the command
    int tar_pipeline_return_code = system(system_command_line_buffer);
    // Check return code - non-zero might just mean find found nothing. Stat will verify.
    if (tar_pipeline_return_code != 0)
    {
        fprintf(stderr, "[S%d_MAKETAR_WARN] Tar command pipeline exited with status %d.\n", SERVER_NUM, tar_pipeline_return_code);
    }

    /* 6. Check Tar File Existence and Size */
    printf("DEBUG (PID %d) MAKETAR: Checking resulting tar file '%s'...\n", getpid(), local_tar_output_filepath);
    struct stat tar_file_status;
    long actual_tar_file_size = 0; // Use long for size
    // Use stat() to check if file exists and get its size
    if (stat(local_tar_output_filepath, &tar_file_status) == 0)
    {
        // Stat succeeded. Check size.
        if (tar_file_status.st_size > 0)
        {
            actual_tar_file_size = tar_file_status.st_size; // Store the size
            printf("DEBUG (PID %d) MAKETAR: Tar file created. Size: %ld bytes.\n", getpid(), actual_tar_file_size);
        }
        else
        {
            // File exists but is empty (0 bytes). No matching zip files found.
            printf("[S%d_MAKETAR_INFO] Tar file is empty (0 bytes). No .zip files found.\n", SERVER_NUM);
            actual_tar_file_size = 0; // Ensure size is 0
        }
    }
    else
    {
        // Stat failed. Tar command likely failed or found nothing.
        perror("[S%d_MAKETAR_WARN] stat failed on tar file (likely no files found)");
        printf("[S%d_MAKETAR_INFO] Tar file '%s' not found or inaccessible.\n", SERVER_NUM, local_tar_output_filepath);
        actual_tar_file_size = 0; // Treat as size 0
    }

    /* 7. Prepare Size Response String */
    char size_string_for_s1[32];
    // Convert the determined size (0 or >0) to a string for sending
    snprintf(size_string_for_s1, sizeof(size_string_for_s1), "%ld", actual_tar_file_size);
    size_response_string = size_string_for_s1; // Update pointer to the actual size string

send_maketar_response_and_cleanup:
    /* 8. Send Size String to S1 */
    printf("DEBUG (PID %d) MAKETAR: Sending size string '%s' to S1...\n", getpid(), size_response_string);
    ssize_t written_size_bytes = write(s1_socket_fd, size_response_string, strlen(size_response_string));
    // Check write result
    if (written_size_bytes < (ssize_t)strlen(size_response_string))
    {
        perror("[S%d_MAKETAR_ERROR] Failed writing size string to S1");
        maketar_success_flag = 0;     // Mark failure
        goto cleanup_maketar_process; // Cannot send content if size failed
    }
    printf("DEBUG (PID %d) MAKETAR: Size string sent.\n", getpid());

    /* 9. Send Tar Content (only if size > 0 and operation OK) */
    // Re-parse the size string to get the long value for comparison
    long size_value_sent = atol(size_response_string);
    if (maketar_success_flag == 1 && size_value_sent > 0)
    {
        printf("DEBUG (PID %d) MAKETAR: Opening tar file '%s' to send %ld bytes...\n",
               getpid(), local_tar_output_filepath, size_value_sent);
        tar_output_file_stream = fopen(local_tar_output_filepath, "rb"); // Read binary
        // Check fopen
        if (tar_output_file_stream == NULL)
        {
            perror("[S%d_MAKETAR_ERROR] Failed to open created tar file for reading");
            // Already sent size, S1 expects content. Close connection.
            maketar_success_flag = 0;
        }
        else
        {
            // File opened, send content
            printf("DEBUG (PID %d) MAKETAR: Sending tar content...\n", getpid());
            char tar_data_chunk_buffer[FILE_CHUNK_BUFFER_SIZE];
            size_t bytes_read_tar_data;
            size_t cumulative_bytes_sent_tar = 0;
            // Loop reading chunks and writing to socket
            while ((bytes_read_tar_data = fread(tar_data_chunk_buffer, 1, sizeof(tar_data_chunk_buffer), tar_output_file_stream)) > 0)
            {
                ssize_t bytes_written_tar_data = write(s1_socket_fd, tar_data_chunk_buffer, bytes_read_tar_data);
                // Check write result
                if (bytes_written_tar_data < 0 || (size_t)bytes_written_tar_data != bytes_read_tar_data)
                {
                    perror("[S%d_MAKETAR_ERROR] Failed writing tar content chunk to S1");
                    maketar_success_flag = 0; // Mark failure
                    break;                    // Stop sending
                }
                cumulative_bytes_sent_tar += bytes_written_tar_data;
            }
            // Check for fread error
            if (ferror(tar_output_file_stream))
            {
                perror("[S%d_MAKETAR_ERROR] Error reading tar file content during send");
                maketar_success_flag = 0; // Mark failure
            }
            // Close tar file stream
            fclose(tar_output_file_stream);
            tar_output_file_stream = NULL; // Mark closed
            printf("DEBUG (PID %d) MAKETAR: Finished sending tar content (%zu bytes sent).\n", getpid(), cumulative_bytes_sent_tar);
        }
    }
    else
    {
        // Size was 0 or error occurred before content sending.
        printf("DEBUG (PID %d) MAKETAR: Skipping content sending (Size 0 or prior error).\n", getpid());
    }

cleanup_maketar_process:
    // Clean up the temporary directory if it was created
    if (temp_dir_was_created == 1)
    {
        // Construct cleanup command: rm -rf /path/to/temp
        snprintf(cleanup_rm_command_buffer, sizeof(cleanup_rm_command_buffer), "rm -rf \"%s\"", temporary_tar_directory);
        printf("DEBUG (PID %d) MAKETAR: Cleaning up temporary directory: %s\n", getpid(), cleanup_rm_command_buffer);
        // Execute cleanup, ignore errors.
        int cleanup_status_code = system(cleanup_rm_command_buffer);
        if (cleanup_status_code != 0)
        {
            fprintf(stderr, "[S%d_MAKETAR_WARN] Temporary directory cleanup failed (Code: %d).\n", SERVER_NUM, cleanup_status_code);
        }
    }
    // Ensure file stream is closed if loop broke early
    if (tar_output_file_stream != NULL)
    {
        fclose(tar_output_file_stream);
    }
    // Protocol: Close connection after sending data or size "0"
    printf("DEBUG (PID %d) MAKETAR: Closing connection to S1 (Status Flag: %d).\n", getpid(), maketar_success_flag);
    close(s1_socket_fd);

    printf("DEBUG (PID %d) MAKETAR: Exiting handle_maketar_command.\n", getpid());

} // End handle_maketar_command
