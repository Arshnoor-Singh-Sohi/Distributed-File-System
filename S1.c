/* S1.c - This is the main server program for my distributed file system project! */
/* It listens for clients and handles different file operations. */

#include <stdio.h>      // Need this for standard input/output like printf, perror
#include <stdlib.h>     // For stuff like exit(), atoi(), getenv()
#include <string.h>     // Need this for string functions like memset, strncmp, strlen
#include <unistd.h>     // For system calls like fork(), read(), write(), close(), getpid()
#include <signal.h>     // Used to handle signals, specifically SIGCHLD to deal with zombie processes
#include <sys/socket.h> // Core socket functions and structures
#include <sys/types.h>  // Basic system data types
#include <netinet/in.h> // Structures for internet domain addresses (like sockaddr_in)
#include <arpa/inet.h>  // For functions like inet_pton() and htons()
#include <libgen.h>     // Needed for basename() function to extract filename from path
#include <sys/stat.h>   // For file status functions like stat()
#include <ctype.h>      // For character type functions like isdigit()
#include <fcntl.h>      // For file control options, specifically fsync() to ensure data is written
#include <errno.h>      // To check the value of errno after system calls
#include <stdint.h>     // Needed for fixed-width integer types like uint32_t, uint64_t, and UINT32_MAX
#include <sys/wait.h>

// Defining a maximum size for the file list buffer, just in case it gets really long
#define MAX_FILE_LIST_SIZE 65536 // 64KB, seems like enough space

/* === Function Prototypes === */
// These tell the compiler what functions exist before they are fully defined later.
// Makes the code easier to organize

// This one handles the overall client communication loop
void process_client(int client_connection_fd);

// These handle specific commands from the client
void handle_upload_command(int client_connection_fd, char *command_string);
void handle_download_command(int connected_client_fd, char *client_request_string);
void handle_remove_command(int requesting_client_socket, char *full_command_line);
void handle_downltar_command(int client_comm_socket, char *received_command_line);
void handle_dispfnames_command(int client_socket_fd, char *command_line);

// These are for talking to the other servers (S2, S3, S4)
int transfer_to_secondary_server(int target_server_id, const char *local_s1_source_filepath, const char *original_client_dest_path, uint64_t actual_file_size);
int retrieve_from_secondary_server(int secondary_server_number, const char *requested_filepath, char *output_data_buffer, size_t *received_file_size_ptr);
int remove_from_secondary_server(int which_secondary_server, const char *client_specified_filepath);
int get_tar_from_secondary_server(int secondary_host_id, const char *type_of_file, const char *local_save_filepath);
int get_filenames_from_secondary_server(int target_server_id, const char *client_provided_pathname, char *destination_buffer, size_t buffer_capacity);

// Signal handler function to reap terminated child processes
void sigchld_handler(int signal_number)
{
    // Using waitpid() in a loop to reap ALL terminated children non-blockingly
    // WNOHANG ensures the call doesn't block if no children have exited
    // Looping handles the case where multiple children might exit around the same time
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    // We don't need to re-install the handler on most modern systems
}

// The main entry point of our S1 server program
int main(int argc, char *argv[])
{
    // argc is the number of arguments, argv holds the arguments themselves.
    // argv[0] is the program name, argv[1] should be the port number.
    printf("DEBUG: S1 Server starting...\n");
    printf("DEBUG: Received %d command-line arguments.\n", argc);

    /* Checking if exactly one argument (the port number) was provided */
    // It needs exactly 2 args: program name and port.
    if (argc < 2 || argc > 2) // Checking lower and upper bounds instead of just != 2
    {
        // Print an error message showing how to run the program correctly.
        printf("Usage: %s <port_number>\n", argv[0]);
        // Exit the program because we can't run without the port.
        // Using exit code 1 usually means an error happened.
        fprintf(stderr, "ERROR: Incorrect number of arguments provided.\n"); // More specific error msg
        exit(1);
    }

    // Declaring variables needed for the server socket setup
    int listener_socket_fd;                 // File descriptor for the main listening socket
    int connection_socket_fd;               // File descriptor for the socket connected to a specific client
    int server_listen_port;                 // The port number the server will listen on
    struct sockaddr_in server_address_info; // Structure to hold server address details
    struct sockaddr_in client_address_info; // Structure to hold client address details
    socklen_t client_address_length;        // Variable to store the size of the client address structure

    // This is important! It tells the parent process (S1) not to wait for child processes
    // when they finish. This prevents "zombie" processes from building up.
    // Found this suggestion online, seems helpful.
    signal(SIGCHLD, sigchld_handler); // Registering our custom handler
    printf("DEBUG: SIGCHLD handler set to SIG_IGN to prevent zombie processes.\n");

    /* == Create the main server socket == */
    printf("DEBUG: Creating the listener socket...\n");
    // AF_INET means we're using IPv4. SOCK_STREAM means we're using TCP (reliable connection).
    // 0 lets the system choose the appropriate protocol (which is TCP for SOCK_STREAM).
    listener_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    // socket() returns -1 if it fails.
    if (listener_socket_fd < 0)
    {
        // perror prints a system error message based on the 'errno' variable.
        perror("FATAL ERROR: Socket creation failed");
        // If we can't create a socket, the server can't run.
        exit(1);
    }
    printf("DEBUG: Listener socket created successfully (fd = %d).\n", listener_socket_fd);

    /* == Allow the socket address to be reused immediately after closing == */
    // This is super useful during development/testing when restarting the server quickly.
    // Otherwise, the OS might keep the port busy for a bit (TIME_WAIT state).
    int allow_reuse = 1; // Set the option value to true (1)
    printf("DEBUG: Setting SO_REUSEADDR socket option...\n");
    if (setsockopt(listener_socket_fd, SOL_SOCKET, SO_REUSEADDR, &allow_reuse, sizeof(allow_reuse)) < 0)
    {
        // This isn't always fatal, so just print a warning.
        perror("WARNING: setsockopt(SO_REUSEADDR) failed");
        // We could potentially continue without it, but it's good to know if it failed.
    }
    else
    {
        printf("DEBUG: SO_REUSEADDR option set successfully.\n");
    }

    /* == Setting up the server's address information == */
    printf("DEBUG: Configuring server address...\n");
    // Zero out the structure first to make sure there's no garbage data.
    memset(&server_address_info, 0, sizeof(server_address_info));
    // Convert the port number argument (which is a string) to an integer.
    server_listen_port = atoi(argv[1]);
    printf("DEBUG: Server port number from argument: %d\n", server_listen_port);

    // Set the address family to IPv4.
    server_address_info.sin_family = AF_INET;
    // Set the IP address. INADDR_ANY means the server will accept connections on any
    // available network interface on the machine (e.g., ethernet, wifi).
    // htonl converts the address to network byte order (important for compatibility).
    server_address_info.sin_addr.s_addr = htonl(INADDR_ANY);
    // Set the port number. htons converts the port number to network byte order.
    server_address_info.sin_port = htons(server_listen_port);

    /* == Bind the socket to the specified address and port == */
    printf("DEBUG: Binding socket (fd=%d) to port %d...\n", listener_socket_fd, server_listen_port);
    // bind() associates the socket we created with the address/port we configured.
    if (bind(listener_socket_fd, (struct sockaddr *)&server_address_info, sizeof(server_address_info)) < 0)
    {
        perror("FATAL ERROR: Binding failed");
        // If binding fails (e.g., port already in use), we need to close the socket we opened.
        close(listener_socket_fd);
        // And exit, because the server can't listen on the required port.
        exit(1);
    }
    printf("DEBUG: Socket bound successfully.\n");

    /* == Start listening for incoming client connections == */
    // listen() marks the socket as passive, meaning it will be used to accept incoming connections.
    // The '5' is the backlog - the maximum number of pending connections the OS should queue up
    // while our server is busy handling an existing connection. 5 is pretty standard.
    printf("DEBUG: Setting socket to listen mode (backlog = 5)...\n");
    if (listen(listener_socket_fd, 5) < 0)
    {
        perror("FATAL ERROR: Listen failed");
        // If listen fails, close the socket and exit.
        close(listener_socket_fd);
        exit(1);
    }

    // Let the user know the server is up and running!
    printf("Server S1 running on port %d...\n", server_listen_port);

    /* == Prepare the S1 storage directory == */
    // Make sure the directory where S1 stores its files exists.
    // Using system() is maybe easier than using C functions like stat/mkdir for this setup.
    printf("DEBUG: Ensuring ~/S1 directory exists...\n");
    // First, try removing any *file* named S1 in the home directory, just in case.
    // The -f flag prevents errors if it doesn't exist.
    int sys_ret_rm = system("rm -f ~/S1");
    if (sys_ret_rm != 0)
    {
        printf("DEBUG: 'rm -f ~/S1' command returned %d (this might be okay if it didn't exist)\n", sys_ret_rm);
    }
    // Now, create the directory. The -p flag means it creates parent directories if needed
    // and doesn't give an error if the directory already exists. Super handy!
    int sys_ret_mkdir = system("mkdir -p ~/S1");
    if (sys_ret_mkdir != 0)
    {
        fprintf(stderr, "WARNING: 'mkdir -p ~/S1' command failed with return code %d. Check permissions?\n", sys_ret_mkdir);
        // Maybe exit here if the directory is crucial? For now, just warn.
    }
    else
    {
        printf("DEBUG: ~/S1 directory should now exist.\n");
    }

    // signal(SIGCHLD, SIG_IGN); // Removed second call

    int server_running_status = 1;

    /* == Main Server Loop - Wait for and handle client connections == */
    printf("DEBUG: Entering main server loop to accept client connections...\n");
    while (server_running_status)
    {
        // Get the size of the client address structure.
        client_address_length = sizeof(client_address_info);

        // accept() waits here until a client tries to connect.
        // When a connection comes in, it creates a *new* socket (connection_socket_fd)
        // specifically for communicating with that client.
        // It also fills in the client_address_info structure.
        printf("DEBUG: Waiting to accept a new client connection...\n");
        connection_socket_fd = accept(listener_socket_fd, (struct sockaddr *)&client_address_info, &client_address_length);

        // Check if accept() failed.
        if (connection_socket_fd < 0)
        {
            // If accept fails (e.g., interrupted by a signal), print an error
            // and just continue the loop to wait for the next connection attempt.
            if (errno == EINTR)
            { // Check if interrupted by signal
                printf("DEBUG: accept() interrupted, continuing loop.\n");
                continue;
            }
            perror("WARNING: Accept failed");
            // No need to exit the whole server, just skip this failed connection attempt.
            continue; // Go back to the start of the while loop
        }

        // If we got here, accept() was successful!
        printf("Client connected! (Client Socket FD: %d)\n", connection_socket_fd);

        /* == Fork a child process to handle this client == */
        // fork() creates a new process which is a copy of the current (parent) process.
        printf("DEBUG: Forking a child process to handle the client...\n");
        pid_t child_process_id = fork();

        // Check the return value of fork()
        if (child_process_id < 0)
        {
            // Fork failed - this is bad. Log error and close the client socket we just accepted.
            perror("ERROR: Fork failed");
            close(connection_socket_fd);
            // The parent continues running, maybe the next fork will work.
        }
        else if (child_process_id == 0)
        {
            /* == This is the CHILD process == */
            printf("DEBUG: Child process (PID: %d) started. Closing listener socket.\n", getpid());
            // The child process doesn't need the main listening socket, only the parent does.
            close(listener_socket_fd);
            // Call the function that handles all communication with this specific client.
            printf("DEBUG: Child process (PID: %d) calling process_client().\n", getpid());
            process_client(connection_socket_fd); // Pass the connection socket to the handler
            // After process_client finishes (e.g., client disconnects), the child process should exit.
            printf("DEBUG: Child process (PID: %d) exiting after process_client().\n", getpid());
            // Ensure socket is closed in child before exit, even if process_client didn't
            close(connection_socket_fd);
            exit(0); // Use exit(0) for successful completion of the child's task.
        }
        else
        {
            /* == This is the PARENT process == */
            printf("DEBUG: Parent process (PID: %d) created child (PID: %d). Closing connection socket.\n", getpid(), child_process_id);
            // The parent process doesn't need the connection socket for this specific client,
            // the child process is handling it. Closing it here prevents resource leaks.
            close(connection_socket_fd);
            // The parent immediately loops back to accept() to wait for the *next* client.
        }
    } // End of main server loop (while)

    // This part might not be reached if the loop is infinite, but doing as it is a good practice.
    printf("DEBUG: Server loop exited (this shouldn't happen with while(1)). Closing listener socket.\n");
    close(listener_socket_fd); // Close the main listening socket when the server shuts down.
    return 0;                  // Indicates successful execution of the main function.
}

/* ========================================================================= */
/*                      process_client Function                             */
/* ========================================================================= */
/**
 * @brief Function to handle all communication with a single connected client.
 * This runs inside the child process created by fork(). It reads commands from
 * the client in a loop and dispatches them to the appropriate handler function.
 *
 * @param client_connection_fd The socket file descriptor connected to the client.
 */
void process_client(int client_connection_fd) // Renamed client_sd for clarity
{
    // Buffer to store messages received from the client. 1024 bytes should be enough
    char client_message_buffer[1024];
    // Variable to store the number of bytes received from read()
    ssize_t bytes_received;
    // Adding a counter for requests processed, maybe useful later
    int request_counter = 0;
    const int MAX_REQUESTS_BEFORE_SOMETHING = 1000; // Just a placeholder constant

    // This is the main loop for this client connection.
    // It keeps running until the client disconnects or an error occurs.
    // while(1) means "loop forever".
    printf("DEBUG (PID %d): process_client started for client fd %d. Entering command loop.\n", getpid(), client_connection_fd);

    while (1) // Using 1 for true, standard practice for infinite loops
    {
        // Print a message indicating the server is ready for the next command from this client.
        printf("DEBUG (PID %d): Waiting for command from client fd %d (Request #%d)...\n", getpid(), client_connection_fd, request_counter + 1);

        /* Read client's request/command */
        // Try to read data from the client socket into our buffer.
        // read() returns the number of bytes read, 0 if the client disconnected, or -1 on error.
        bytes_received = read(client_connection_fd, client_message_buffer, sizeof(client_message_buffer) - 1); // Leave space for null terminator

        // Check the return value of read()
        if (bytes_received < 0) // An error occurred during read
        {
            // Check if interrupted by signal
            if (errno == EINTR)
            {
                printf("DEBUG (PID %d): read() interrupted, continuing loop.\n", getpid());
                continue;
            }
            perror("ERROR (PID %d): Error reading from client socket");
            // If reading fails, we can't continue with this client. Break out of the loop.
            break;
        }

        if (bytes_received == 0) // Client closed the connection gracefully
        {
            printf("INFO (PID %d): Client fd %d disconnected.\n", getpid(), client_connection_fd);
            // No more data to read, break out of the loop.
            break;
        }

        // If we got here, we received some data (bytes_received > 0).
        // Add a null terminator to make it a valid C string.
        client_message_buffer[bytes_received] = '\0';
        printf("DEBUG (PID %d): Received %zd bytes from client fd %d: \"%s\"\n", getpid(), client_connection_fd, bytes_received, client_message_buffer);

        // Increment the request counter (just for tracking/debug)
        request_counter++;
        if (request_counter == MAX_REQUESTS_BEFORE_SOMETHING)
        {
            printf("DEBUG (PID %d): Processed %d requests for client fd %d.\n", getpid(), request_counter, client_connection_fd);
            // This doesn't really do anything, just an example of a check
        }

        /* === Parse the command and call the appropriate handler function === */
        // We use strncmp to compare the beginning of the received buffer with known command strings.
        // It's safer than strcmp because it limits the number of characters compared.

        // Check for "uploadf" command (length 7 + space)
        if (strncmp(client_message_buffer, "uploadf ", 8) == 0)
        {
            printf("DEBUG (PID %d): Recognized 'uploadf' command.\n", getpid());
            handle_upload_command(client_connection_fd, client_message_buffer);
        }
        // Check for "downlf" command (length 6 + space)
        else if (strncmp(client_message_buffer, "downlf ", 7) == 0)
        {
            printf("DEBUG (PID %d): Recognized 'downlf' command.\n", getpid());
            handle_download_command(client_connection_fd, client_message_buffer);
        }
        // Check for "removef" command (length 7 + space)
        else if (strncmp(client_message_buffer, "removef ", 8) == 0)
        {
            printf("DEBUG (PID %d): Recognized 'removef' command.\n", getpid());
            handle_remove_command(client_connection_fd, client_message_buffer);
        }
        // Check for "downltar" command (length 8 + space)
        else if (strncmp(client_message_buffer, "downltar ", 9) == 0)
        {
            printf("DEBUG (PID %d): Recognized 'downltar' command.\n", getpid());
            handle_downltar_command(client_connection_fd, client_message_buffer);
        }
        // Check for "dispfnames" command (length 10 + space)
        else if (strncmp(client_message_buffer, "dispfnames ", 11) == 0)
        {
            printf("DEBUG (PID %d): Recognized 'dispfnames' command.\n", getpid());
            handle_dispfnames_command(client_connection_fd, client_message_buffer);

            // So, after it returns, we should break the loop here.
            // printf("DEBUG (PID %d): Connection closed by dispfnames handler. Exiting process_client.\n", getpid());
        }
        // If none of the known commands match...
        else
        {
            // Send an error message back to the client.
            const char *error_message_unknown_cmd = "ERROR: UNKNOWN COMMAND";
            printf("WARN (PID %d): Received unknown command: \"%s\". Sending error.\n", getpid(), client_message_buffer);
            ssize_t write_check = write(client_connection_fd, error_message_unknown_cmd, strlen(error_message_unknown_cmd));
            if (write_check < 0)
            {
                perror("ERROR (PID %d): Failed to send UNKNOWN COMMAND error to client");
                // If we can't even send an error, best to close the connection.
                break;
            }
        }
        // Loop continues here to wait for the next command (unless dispfnames was called)...

    } // End of while(1) loop

    // When the loop finishes (due to disconnect, error, or dispfnames),
    // the socket might already be closed by the handler (dispfnames) or by main().
    // No explicit close here needed as main() handles it upon child exit.
    printf("DEBUG (PID %d): Exiting process_client function for fd %d.\n", getpid(), client_connection_fd);
    // The child process will exit after this function returns in main().
}

/* ========================================================================= */
/*               handle_upload_command Function (Modified)                   */
/* ========================================================================= */
/**
 * @brief Handles the 'uploadf' command from the client.
 * It's job is to get a file from the client and save it.
 * If it's a .c file, It will keep it here in ~/S1/....
 * If it's .pdf, .txt, or .zip, It will save it here first, then send it to S2/S3/S4, and then delete the copy.
 * It need to talk back and forth with the client (READY, size, file data, final status).
 *
 * @param client_connection_fd The socket number for talking to the specific client.
 * @param command_string The full command string received (e.g., "uploadf file.txt ~S1/folder/file.txt").
 */
void handle_upload_command(int client_connection_fd, char *command_string)
{
    printf("DEBUG (PID %d) Upload: --- Entering handle_upload_command ---\n", getpid());
    printf("DEBUG (PID %d) Upload: Received raw command: '%s'\n", getpid(), command_string);

    // Variables to store parts of the command
    char original_client_fname[256];     // The filename the client thinks it's uploading (e.g., "sample.txt")
    char client_provided_dest_path[256]; // The destination path client specified (e.g., "~S1/stuff/sample.txt")
    // Buffer for sending the final success/error message back to the client
    char final_status_msg[512];
    // Placeholder for home directory path on the server
    char *server_home_dir = NULL;
    // To keep track of if the upload process failed at some point
    int upload_has_failed_flag = 0; // 0 = okay so far, 1 = failed
    // A message to send if things go wrong
    const char *error_details_for_client = NULL;
    // Let's add a redundant check flag
    int preliminary_check_passed = 0; // 0 = not checked, 1 = passed
    FILE *local_file_pointer = NULL;  // File handle for local saving

    /* Step 1: Parse the command string */
    // ----------------------------------
    // Use sscanf to extract the client filename and destination path from the command.
    // It should find the command name ("uploadf"), then a string (%s), then another string (%s).
    // It returns the number of items successfully matched. We expect 2 items after "uploadf".
    printf("DEBUG (PID %d) Upload: Parsing command string...\n", getpid());
    int items_parsed = sscanf(command_string, "uploadf %255s %255s", original_client_fname, client_provided_dest_path);

    // Check if we got exactly 2 arguments. Using '< 2' covers cases where it's 0 or 1.
    if (items_parsed < 2)
    {
        // If the format is wrong, set the failure flag and the error message.
        upload_has_failed_flag = 1;
        error_details_for_client = "ERROR: Invalid uploadf command format. Use: uploadf <local_filename> <destination_path>";
        printf("ERROR (PID %d) Upload: Invalid command format. Parsed %d items, expected 2.\n", getpid(), items_parsed);
        // Go directly to sending the error message (at the end).
        goto send_final_response; // Jump to the cleanup/response part
    }
    // If parsing worked:
    printf("DEBUG (PID %d) Upload: Parsed OK -> Client FName: '%s', Dest Path: '%s'\n", getpid(), original_client_fname, client_provided_dest_path);
    preliminary_check_passed = 1; // Mark basic check as passed

    /* Step 2: Validate the Destination Path Prefix */
    // ----------------------------------------------
    // The project requires all destination paths to start with "~S1/"
    printf("DEBUG (PID %d) Upload: Validating destination path prefix...\n", getpid());
    // Use strncmp to compare the first 4 characters.
    if (strncmp(client_provided_dest_path, "~S1/", 4) != 0)
    {
        // If it doesn't start correctly, set failure flag and error message.
        upload_has_failed_flag = 1;
        error_details_for_client = "ERROR: Destination path must start with ~S1/";
        printf("ERROR (PID %d) Upload: Invalid destination path prefix: '%s'.\n", getpid(), client_provided_dest_path);
        goto send_final_response; // Jump to cleanup/response
    }
    printf("DEBUG (PID %d) Upload: Destination path prefix '~S1/' is valid.\n", getpid());

    /* Step 3: Get the Server's Home Directory */
    // -----------------------------------------
    // We need the absolute path to the home directory to construct the full file path.
    printf("DEBUG (PID %d) Upload: Getting server's HOME directory environment variable...\n", getpid());
    server_home_dir = getenv("HOME");
    // Check if getenv failed (returned NULL).
    if (server_home_dir == NULL) // Changed from !home
    {
        // This is a server-side configuration issue.
        upload_has_failed_flag = 1;
        error_details_for_client = "ERROR: Cannot determine home directory on server";
        perror("ERROR (PID %d) Upload: getenv(\"HOME\") failed");
        printf("ERROR (PID %d) Upload: Server configuration issue - HOME directory not found.\n", getpid());
        goto send_final_response; // Jump to cleanup/response
    }
    printf("DEBUG (PID %d) Upload: Server HOME directory: '%s'\n", getpid(), server_home_dir);

    /* Step 4: Construct the Absolute Local S1 Paths */
    // -----------------------------------------------
    // We need the full path to the file *in the S1 directory structure* (e.g., /home/user/S1/stuff/sample.txt)
    // And the path to the directory *containing* the file (e.g., /home/user/S1/stuff) so we can create it.
    char absolute_s1_filepath[512]; // Final path like /home/user/S1/folder/file.ext
    char absolute_s1_dirpath[512];  // Directory path like /home/user/S1/folder
    int dir_path_len = 0;           // Redundant variable to store length

    // Get the part of the client path *after* "~S1/"
    char *path_after_s1prefix = client_provided_dest_path + 4; // Point past "~S1/"
    printf("DEBUG (PID %d) Upload: Relative path part: '%s'\n", getpid(), path_after_s1prefix);

    // Find the last '/' in the relative path part. This separates the directory from the filename.
    char *final_slash_ptr = strrchr(path_after_s1prefix, '/');

    // Check if there's no slash, or if it's the very first character (e.g., "~S1/file.txt" or "~S1//file.txt")
    if (final_slash_ptr == NULL || final_slash_ptr == path_after_s1prefix)
    {
        // The file goes directly into the S1 root directory.
        // The directory to create is just ~/S1
        snprintf(absolute_s1_dirpath, sizeof(absolute_s1_dirpath), "%s/S1", server_home_dir);
        // The full file path is ~/S1/<relative_path_part>
        snprintf(absolute_s1_filepath, sizeof(absolute_s1_filepath), "%s/S1/%s", server_home_dir, path_after_s1prefix);
        printf("DEBUG (PID %d) Upload: Path case: File in S1 root.\n", getpid());
    }
    else
    {
        // There is a subdirectory (e.g., "~S1/folder/file.txt")
        // The directory path is ~/S1/ plus the part before the last slash.
        snprintf(absolute_s1_dirpath, sizeof(absolute_s1_dirpath), "%s/S1/%.*s", server_home_dir, (int)(final_slash_ptr - path_after_s1prefix), path_after_s1prefix);
        // The full file path is ~/S1/ plus the whole relative path part.
        snprintf(absolute_s1_filepath, sizeof(absolute_s1_filepath), "%s/S1/%s", server_home_dir, path_after_s1prefix);
        printf("DEBUG (PID %d) Upload: Path case: File in subdirectory.\n", getpid());
    }

    dir_path_len = strlen(absolute_s1_dirpath); // Store length
    printf("DEBUG (PID %d) Upload: Absolute directory to create: '%s' (Length: %d)\n", getpid(), absolute_s1_dirpath, dir_path_len);
    printf("DEBUG (PID %d) Upload: Absolute final file path in S1: '%s'\n", getpid(), absolute_s1_filepath);

    /* Step 5: Create the Directory Structure if it Doesn't Exist */
    // ----------------------------------------------------------
    // Use `mkdir -p` command via system(). It's easy because it creates parent directories
    // and doesn't complain if the directory already exists. Need quotes around path for safety.
    char mkdir_system_command[1024];
    snprintf(mkdir_system_command, sizeof(mkdir_system_command), "mkdir -p \"%s\"", absolute_s1_dirpath);
    printf("DEBUG (PID %d) Upload: Executing directory creation command: %s\n", getpid(), mkdir_system_command);

    int mkdir_return_status = system(mkdir_system_command);
    // Check if the command failed (returned non-zero).
    // Using !( == 0) just to be slightly different from != 0
    if (!(mkdir_return_status == 0))
    {
        // This *could* be an error (like permissions), but maybe not fatal yet.
        // fopen() later will fail if the directory isn't writable. So just log a warning.
        fprintf(stderr, "WARN (PID %d) Upload: system('%s') returned status %d. Will proceed and let fopen check.\n", getpid(), mkdir_system_command, mkdir_return_status);
        // We don't set upload_has_failed_flag here yet.
    }
    else
    {
        printf("DEBUG (PID %d) Upload: Directory creation command executed successfully (or dir already existed).\n", getpid());
    }

    /* Step 6: Check the File Extension of the Target File */
    // ---------------------------------------------------
    // We need to know the file type to decide if it stays in S1 or gets forwarded.
    // Find the filename part (after the last '/') in the *absolute* S1 path.
    char *target_filename_ptr = strrchr(absolute_s1_filepath, '/');
    if (target_filename_ptr == NULL)
    {                                               // Should only happen if path was just "/" ? Very unlikely.
        target_filename_ptr = absolute_s1_filepath; // Fallback: assume whole path is filename
        printf("WARN (PID %d) Upload: No '/' found in absolute path '%s'? Using whole string as filename.\n", getpid(), absolute_s1_filepath);
    }
    else
    {
        target_filename_ptr++; // Move pointer *past* the slash to the start of the filename.
    }
    int filename_len = strlen(target_filename_ptr); // Redundant calculation
    printf("DEBUG (PID %d) Upload: Extracted filename part: '%s' (Length: %d)\n", getpid(), target_filename_ptr, filename_len);

    // Find the last '.' in the filename part.
    char *file_extension_ptr = strrchr(target_filename_ptr, '.');
    // Check if '.' was not found (NULL) or if it's the very first character (like ".bashrc").
    // Files need a proper extension for this project.
    if (file_extension_ptr == NULL || file_extension_ptr == target_filename_ptr)
    {
        upload_has_failed_flag = 1;
        error_details_for_client = "ERROR: Target filename has no valid extension";
        printf("ERROR (PID %d) Upload: Invalid or missing extension in filename '%s'.\n", getpid(), target_filename_ptr);
        goto send_final_response; // Jump to cleanup/response
    }
    printf("DEBUG (PID %d) Upload: Detected file extension: '%s'\n", getpid(), file_extension_ptr);

    // Redundant check using the flag set earlier
    if (preliminary_check_passed != 1)
    {
        upload_has_failed_flag = 1;
        error_details_for_client = "ERROR: Preliminary check failed (internal logic error)";
        printf("ERROR (PID %d) Upload: Internal state error - preliminary check flag not set.\n", getpid());
        goto send_final_response;
    }

    /* Step 7: Send "READY" Signal to the Client */
    // -------------------------------------------
    // Tell the client we are ready to receive the file size and content.
    const char *go_ahead_signal = "READY";
    printf("DEBUG (PID %d) Upload: Sending '%s' signal to client fd %d\n", getpid(), go_ahead_signal, client_connection_fd);
    ssize_t write_check_ready = write(client_connection_fd, go_ahead_signal, strlen(go_ahead_signal));
    // Check if write failed.
    if (write_check_ready < 0)
    {
        upload_has_failed_flag = 1;
        // Don't set error_details_for_client here, as we can't send it anyway if write failed.
        perror("ERROR (PID %d) Upload: Failed to send READY signal to client");
        printf("ERROR (PID %d) Upload: Cannot proceed with upload, client communication failed.\n", getpid());
        // No point sending final response, just return.
        return; // Exit the handler function immediately.
    }
    printf("DEBUG (PID %d) Upload: READY signal sent successfully.\n", getpid());

    /* Step 8: Receive the File Size (as uint64_t) from the Client */
    // -----------------------------------------------------------
    uint64_t expected_bytes_total = 0; // Using uint64_t for potentially large files
    printf("DEBUG (PID %d) Upload: Waiting to receive file size (uint64_t) from client...\n", getpid());
    ssize_t bytes_read_for_size = read(client_connection_fd, &expected_bytes_total, sizeof(expected_bytes_total));

    // Check if we read the correct number of bytes for a uint64_t.
    // Must check for error (<0) separately from incomplete read.
    if (bytes_read_for_size < 0)
    {
        upload_has_failed_flag = 1;
        perror("ERROR (PID %d) Upload: Failed to read file size from client");
        // Can't proceed, client might be gone. No message needed.
        return; // Exit handler.
    }
    else if (bytes_read_for_size < (ssize_t)sizeof(expected_bytes_total))
    {
        // We didn't get enough bytes. This is bad protocol sync or client disconnect.
        upload_has_failed_flag = 1;
        fprintf(stderr, "ERROR (PID %d) Upload: Incomplete read for file size (got %zd bytes, expected %zu).\n", getpid(), bytes_read_for_size, sizeof(expected_bytes_total));
        // Can't proceed. No message needed.
        return; // Exit handler.
    }
    // If we got here, size was received successfully.
    printf("DEBUG (PID %d) Upload: Received file size: %llu bytes.\n", getpid(), (unsigned long long)expected_bytes_total);

    /* Step 9: Open the Local S1 File for Writing */
    // --------------------------------------------
    // Try to open the file where we intend to save it (in binary write mode "wb").
    printf("DEBUG (PID %d) Upload: Attempting to open local file '%s' for writing...\n", getpid(), absolute_s1_filepath);
    local_file_pointer = fopen(absolute_s1_filepath, "wb");

    // IMPORTANT: Check if fopen failed (returned NULL).
    if (local_file_pointer == NULL)
    {
        // If we can't open the file (e.g., permissions, path is bad despite mkdir check),
        // log the error BUT DO NOT return yet.
        // We *must* still read the file data the client is about to send to keep the
        // communication synchronized. We will set the failure flag and send an error *after* reading.
        perror("ERROR (PID %d) Upload: fopen failed for local file");
        fprintf(stderr, "ERROR (PID %d) Upload: Failed to open local file '%s' for writing. Will still attempt to read from client.\n", getpid(), absolute_s1_filepath);
        // upload_has_failed_flag will be set later based on local_file_pointer being NULL.
    }
    else
    {
        // File opened successfully!
        printf("DEBUG (PID %d) Upload: Successfully opened local file '%s' for writing.\n", getpid(), absolute_s1_filepath);
    }

    /* Step 10: Receive the File Content from the Client */
    // ---------------------------------------------------
    // Read the file data chunk by chunk from the client socket and write to the local file (if open).
    char file_transfer_chunk[4096];      // Buffer for chunks of the file
    size_t bytes_accumulated_so_far = 0; // How many bytes received for this file
    ssize_t chunk_bytes_received;        // Bytes received in the current read() call
    int local_write_failure_flag = 0;    // Flag: 1 if fwrite() fails

    printf("DEBUG (PID %d) Upload: Starting loop to receive %llu bytes of file content from client...\n", getpid(), (unsigned long long)expected_bytes_total);

    // Loop until we have received the expected number of bytes.
    while (bytes_accumulated_so_far < expected_bytes_total)
    {
        // Calculate how many bytes to read in this iteration.
        // Usually the buffer size, unless less than that remains.
        size_t bytes_to_read_this_time = sizeof(file_transfer_chunk);
        if (expected_bytes_total - bytes_accumulated_so_far < bytes_to_read_this_time)
        {
            bytes_to_read_this_time = expected_bytes_total - bytes_accumulated_so_far;
        }

        // Read the next chunk from the client socket.
        chunk_bytes_received = read(client_connection_fd, file_transfer_chunk, bytes_to_read_this_time);

        // Check the result of read()
        if (chunk_bytes_received < 0)
        { // Read error
            if (errno == EINTR)
            {
                printf("DEBUG (PID %d) Upload: read interrupted, continuing.\n", getpid());
                continue;
            } // Handle interruption
            perror("ERROR (PID %d) Upload: read() failed while receiving file content from client");
            upload_has_failed_flag = 1;                                                  // Mark failure
            error_details_for_client = "ERROR: Network read error during file transfer"; // Set error msg
            break;                                                                       // Exit the receiving loop
        }
        if (chunk_bytes_received == 0)
        { // Client closed connection unexpectedly
            fprintf(stderr, "ERROR (PID %d) Upload: Client disconnected before sending entire file (received %zu/%llu bytes)\n", getpid(), bytes_accumulated_so_far, (unsigned long long)expected_bytes_total);
            upload_has_failed_flag = 1;                                                   // Mark failure
            error_details_for_client = "ERROR: Client disconnected during file transfer"; // Set error msg
            break;                                                                        // Exit the receiving loop
        }

        // --- Try writing to the local file ---
        // Only attempt to write if:
        // 1. The file was opened successfully (local_file_pointer is not NULL).
        // 2. No previous write error has occurred (local_write_failure_flag is 0).
        if (local_file_pointer != NULL && local_write_failure_flag == 0)
        {
            // Write the chunk we just received to the local file.
            size_t bytes_written_to_file = fwrite(file_transfer_chunk, 1, chunk_bytes_received, local_file_pointer);
            // Check if fwrite wrote the expected number of bytes.
            if (bytes_written_to_file < (size_t)chunk_bytes_received)
            {
                perror("ERROR (PID %d) Upload: fwrite() failed while writing to local file");
                fprintf(stderr, "ERROR (PID %d) Upload: Failed writing to '%s'. Setting write failure flag.\n", getpid(), absolute_s1_filepath);
                // Set the flag to prevent further writes.
                local_write_failure_flag = 1;
                // We MUST continue reading from the client socket to consume the rest of the file data,
                // otherwise the protocol gets messed up for the final response.
                // The overall upload will be marked as failed later.
            }
        }

        // Add the number of bytes received in this chunk to the total count.
        bytes_accumulated_so_far += chunk_bytes_received;
        // Maybe add a progress indicator for large files?
        // if (bytes_accumulated_so_far % (1024*1024) == 0) {
        //    printf("DEBUG (PID %d) Upload: Progress: Received %zu / %llu bytes\n", getpid(), bytes_accumulated_so_far, (unsigned long long)expected_bytes_total);
        // }

    } // End of while loop for receiving file content

    printf("DEBUG (PID %d) Upload: Finished receiving loop. Total bytes received: %zu\n", getpid(), bytes_accumulated_so_far);

    /* Step 11: Check for Errors After Receiving is Done */
    // -------------------------------------------------
    // Now that we've tried to receive everything, check all the possible failure conditions.

    // Case 1: Did fopen() fail earlier?
    if (local_file_pointer == NULL)
    {
        upload_has_failed_flag = 1;
        // Use a more specific error message than the generic one set during fopen failure.
        error_details_for_client = "ERROR: Failed to create file locally (fopen permission/path error)";
        printf("DEBUG (PID %d) Upload: Failure condition: fopen had failed earlier.\n", getpid());
    }
    // Case 2: Did fwrite() fail at some point during reception?
    else if (local_write_failure_flag == 1)
    {
        upload_has_failed_flag = 1;
        error_details_for_client = "ERROR: Failed to write file content locally (fwrite disk full/error)";
        printf("DEBUG (PID %d) Upload: Failure condition: fwrite had failed during reception.\n", getpid());
        // If write failed, the file is corrupt. Close and try to delete it.
        fclose(local_file_pointer); // Close the corrupted file handle
        printf("DEBUG (PID %d) Upload: Attempting to remove corrupted local file: '%s'\n", getpid(), absolute_s1_filepath);
        remove(absolute_s1_filepath); // Attempt to remove the garbage file
        local_file_pointer = NULL;    // Ensure we don't try to use fp again
    }
    // Case 3: Did the client disconnect or have a read error *during* transfer? (Flag already set in loop)
    else if (upload_has_failed_flag == 1)
    {
        // Error message was already set inside the loop.
        printf("DEBUG (PID %d) Upload: Failure condition: Read error or disconnect during transfer.\n", getpid());
        if (local_file_pointer != NULL)
        {
            fclose(local_file_pointer); // Close the incomplete file
            printf("DEBUG (PID %d) Upload: Attempting to remove incomplete local file: '%s'\n", getpid(), absolute_s1_filepath);
            remove(absolute_s1_filepath); // Remove the partial file
            local_file_pointer = NULL;
        }
    }
    // Case 4: Did we receive fewer bytes than expected, even if no specific error occurred? (Should be caught by case 3 usually)
    else if (bytes_accumulated_so_far < expected_bytes_total)
    {
        upload_has_failed_flag = 1;
        error_details_for_client = "ERROR: Incomplete file transfer (received less data than expected)";
        fprintf(stderr, "ERROR (PID %d) Upload: Received only %zu bytes, but expected %llu.\n", getpid(), bytes_accumulated_so_far, (unsigned long long)expected_bytes_total);
        if (local_file_pointer != NULL)
        {
            fclose(local_file_pointer); // Close the incomplete file
            printf("DEBUG (PID %d) Upload: Attempting to remove incomplete local file: '%s'\n", getpid(), absolute_s1_filepath);
            remove(absolute_s1_filepath); // Remove the partial file
            local_file_pointer = NULL;
        }
    }

    // If any failure occurred, jump to sending the error response.
    if (upload_has_failed_flag == 1)
    {
        printf("DEBUG (PID %d) Upload: Upload process failed. Jumping to send final response.\n", getpid());
        goto send_final_response;
    }

    // --- If we reach here, the file was received successfully into the local S1 path! ---
    printf("DEBUG (PID %d) Upload: File content received successfully into '%s'.\n", getpid(), absolute_s1_filepath);

    /* Step 12: Flush and Sync the Local File to Disk */
    // -----------------------------------------------
    // Make sure the data is actually written from buffers to the physical disk.
    // This is important before we try to forward it or confirm success.
    printf("DEBUG (PID %d) Upload: Flushing and syncing local file: %s\n", getpid(), absolute_s1_filepath);

    // fflush() tries to write out buffered data.
    printf("DEBUG (PID %d) Upload: Calling fflush()...\n", getpid());
    if (!(fflush(local_file_pointer) == 0))
    { // Using !(==0) for variety
        // A flush error is usually not critical if fsync works, so just warn.
        perror("WARN (PID %d) Upload: fflush failed");
    }

    // fsync() ensures data is written to the underlying device. Need file descriptor.
    int file_descriptor_for_sync = fileno(local_file_pointer); // Get the integer fd from FILE*
    printf("DEBUG (PID %d) Upload: Calling fsync() on fd %d...\n", getpid(), file_descriptor_for_sync);
    if (!(fsync(file_descriptor_for_sync) == 0))
    { // Using !(==0) for variety
        // fsync failure is more serious. Data might not be on disk.
        perror("ERROR (PID %d) Upload: fsync failed - data might not be durable on disk!");
        // If we can't guarantee the file is saved, we should report an error.
        upload_has_failed_flag = 1;
        error_details_for_client = "ERROR: Failed to sync received file to disk";
        fclose(local_file_pointer); // Close the file handle
        local_file_pointer = NULL;  // Mark as closed
        printf("DEBUG (PID %d) Upload: fsync failed. Jumping to send final response.\n", getpid());
        goto send_final_response; // Jump to send the error
    }

    // Now close the file handle.
    printf("DEBUG (PID %d) Upload: Calling fclose()...\n", getpid());
    if (!(fclose(local_file_pointer) == 0))
    { // Using !(==0) for variety
        // This is weird, but the data is likely okay due to fsync. Just log it.
        perror("ERROR (PID %d) Upload: fclose failed after successful fsync");
        // Proceed anyway, assuming file is ok.
    }
    local_file_pointer = NULL; // Mark as closed
    printf("DEBUG (PID %d) Upload: Local file flushed, synced, and closed successfully.\n", getpid());

    /* Step 13: Determine Forwarding Action Based on Extension */
    // ---------------------------------------------------------
    // Now decide what to do with the file we just saved.
    printf("DEBUG (PID %d) Upload: Determining action based on extension '%s'...\n", getpid(), file_extension_ptr);

    // Check if it's a .c file (case-insensitive comparison).
    if (strcasecmp(file_extension_ptr, ".c") == 0)
    {
        /* .c files just stay here in S1 */
        snprintf(final_status_msg, sizeof(final_status_msg), "SUCCESS: .c file stored successfully in S1.");
        printf("INFO (PID %d) Upload: File is .c, keeping locally at '%s'.\n", getpid(), absolute_s1_filepath);
        // No transfer needed, no deletion needed. Jump to sending the success message.
        goto send_final_response;
    }
    else // It's not .c, so it needs to be forwarded.
    {
        /* Needs forwarding to S2, S3, or S4 */
        int target_secondary_server_id = 0; // Which server to send to (2, 3, or 4)

        // Determine the target server based on the extension.
        if (strcasecmp(file_extension_ptr, ".pdf") == 0)
        {
            target_secondary_server_id = 2;
        }
        else if (strcasecmp(file_extension_ptr, ".txt") == 0)
        {
            target_secondary_server_id = 3;
        }
        else if (strcasecmp(file_extension_ptr, ".zip") == 0)
        {
            target_secondary_server_id = 4;
        }
        else
        {
            // This *shouldn't* happen because we checked the extension earlier,
            // but check again just in case (defensive programming!).
            upload_has_failed_flag = 1;
            snprintf(final_status_msg, sizeof(final_status_msg), "ERROR: Unsupported file type '%s' detected after save.", file_extension_ptr); // Use status msg buffer for error
            error_details_for_client = final_status_msg;                                                                                        // Point to the message
            printf("ERROR (PID %d) Upload: Unsupported file type '%s' detected late! Should have been caught earlier.\n", getpid(), file_extension_ptr);
            // Remove the unsupported file we saved.
            printf("DEBUG (PID %d) Upload: Removing unsupported local file '%s'.\n", getpid(), absolute_s1_filepath);
            remove(absolute_s1_filepath);
            goto send_final_response; // Jump to send the error
        }

        printf("INFO (PID %d) Upload: File type '%s' needs forwarding to S%d.\n", getpid(), file_extension_ptr, target_secondary_server_id);

        /* === Attempt to transfer the file to the secondary server === */
        printf("DEBUG (PID %d) Upload: Calling transfer_to_secondary_server(S%d, src='%s', dest='%s', size=%llu)\n",
               getpid(), target_secondary_server_id, absolute_s1_filepath, client_provided_dest_path, (unsigned long long)expected_bytes_total);

        // Call the transfer function. It returns 1 for success, 0 for failure.
        int transfer_result = transfer_to_secondary_server(target_secondary_server_id, absolute_s1_filepath, client_provided_dest_path, expected_bytes_total);
        int transfer_attempt_count = 1; // Redundant variable usage

        // Check the result of the transfer.
        if (transfer_result == 1) // Use == 1 for clarity
        {
            // Transfer successful!
            printf("INFO (PID %d) Upload: Transfer to S%d completed successfully (attempt %d).\n", getpid(), target_secondary_server_id, transfer_attempt_count);
            // Set the success message for the client.
            snprintf(final_status_msg, sizeof(final_status_msg), "SUCCESS: File uploaded and transferred to S%d.", target_secondary_server_id);

            // Now, remove the local copy from S1 as required.
            printf("DEBUG (PID %d) Upload: Removing local S1 copy: '%s'\n", getpid(), absolute_s1_filepath);
            if (!(remove(absolute_s1_filepath) == 0))
            { // Use !(==0) for variety
                // If removal fails, it's not ideal, but the transfer worked. Just log a warning.
                perror("WARN (PID %d) Upload: Failed to remove local file after successful transfer");
            }
            else
            {
                printf("DEBUG (PID %d) Upload: Local S1 copy removed successfully.\n", getpid());
            }
            printf("DEBUG (PID %d) Upload: Adding short delay (1s) after successful transfer to S%d...\n", getpid(), target_secondary_server_id);
            sleep(1); // Add a 1-second delay to allow secondary server to finalize write/close
            // Proceed to send the success message.
            goto send_final_response;
        }
        else // Transfer failed (transfer_result was 0).
        {
            // The transfer function failed.
            upload_has_failed_flag = 1; // Mark overall failure
            fprintf(stderr, "ERROR (PID %d) Upload: transfer_to_secondary_server failed for S%d (attempt %d).\n", getpid(), target_secondary_server_id, transfer_attempt_count);
            // Set the error message for the client.
            snprintf(final_status_msg, sizeof(final_status_msg), "ERROR: File received by S1, but failed to transfer to S%d.", target_secondary_server_id);
            error_details_for_client = final_status_msg;

            // Project doesn't specify what to do with the local S1 copy if transfer fails.
            // Let's keep it in S1 for potential debugging or manual retry?
            // If we wanted to remove it on failure, we'd uncomment the next line:
            // remove(absolute_s1_filepath);
            printf("INFO (PID %d) Upload: Keeping local S1 copy '%s' after transfer failure.\n", getpid(), absolute_s1_filepath);

            // Proceed to send the error message.
            goto send_final_response;
        }
    } // End of else block for non-.c files

/* Step 14: Send Final Response to Client */
// ---------------------------------------
// This is the target for all the goto jumps.
send_final_response:
    printf("DEBUG (PID %d) Upload: Reached final response stage.\n", getpid());

    // Check if the operation failed overall.
    if (upload_has_failed_flag == 1)
    {
        // Make sure we have an error message to send.
        if (error_details_for_client == NULL)
        {
            // Fallback error message if something went wrong setting the specific one.
            error_details_for_client = "ERROR: An unspecified error occurred during upload.";
            printf("ERROR (PID %d) Upload: Upload failed but no specific error message was set! Using fallback.\n", getpid());
        }
        // Send the error message.
        printf("DEBUG (PID %d) Upload: Sending final ERROR response to client: '%s'\n", getpid(), error_details_for_client);
        ssize_t write_check_err = write(client_connection_fd, error_details_for_client, strlen(error_details_for_client));
        if (write_check_err < 0)
        {
            perror("ERROR (PID %d) Upload: Failed to send final error response to client");
        }
    }
    else // Operation succeeded!
    {
        // Send the success message (which was prepared earlier in final_status_msg).
        printf("DEBUG (PID %d) Upload: Sending final SUCCESS response to client: '%s'\n", getpid(), final_status_msg);
        ssize_t write_check_ok = write(client_connection_fd, final_status_msg, strlen(final_status_msg));
        if (write_check_ok < 0)
        {
            perror("ERROR (PID %d) Upload: Failed to send final success response to client");
        }
    }

    // Clean up file pointer if jump happened before close
    if (local_file_pointer != NULL)
    {
        fclose(local_file_pointer);
    }

    printf("DEBUG (PID %d) Upload: --- Exiting handle_upload_command ---\n", getpid());
    // Return control back to the process_client loop.

} // End of handle_upload_command

/* ========================================================================= */
/*             handle_download_command Function                              */
/* ========================================================================= */
/**
 * @brief My function to handle the 'downlf' command.
 * The client wants a file! I need to figure out where it is and send it back.
 * Client gives me a path like ~S1/folder/file.ext
 *
 * Protocol:
 * 1. Check the command is okay ("downlf ~S1/...").
 * 2. Check the file extension.
 * 3. If it's a '.c' file:
 *    a. Find it locally in my ~/S1 directory.
 *    b. Check if it exists and I can open it.
 *    c. Get its size.
 *    d. Send the size (as uint32_t) to the client.
 *    e. Read the file chunk by chunk and send the content to the client.
 * 4. If it's '.pdf', '.txt', or '.zip':
 *    a. Figure out which server has it (S2 for pdf, S3 for txt, S4 for zip).
 *    b. Call my helper function `retrieve_from_secondary_server` to get the file content and size.
 *    c. If retrieval worked:
 *       i. Send the size (as uint32_t) to the client.
 *       ii. Send the file content (which is now in a buffer) to the client.
 *    d. If retrieval failed, send an error message to the client.
 * 5. Handle errors (bad command, path, extension, file not found, cannot open, out of memory, retrieval failed) by sending an "ERROR: ..." message.
 *
 * @param connected_client_fd The socket number for talking to this specific client.
 * @param client_request_string The full command string received (e.g., "downlf ~S1/folder/file.c").
 */
void handle_download_command(int connected_client_fd, char *client_request_string)
{
    printf("DEBUG (PID %d) Download: --- Entering handle_download_command ---\n", getpid());
    printf("DEBUG (PID %d) Download: Received raw command: '%s'\n", getpid(), client_request_string);

    // Variable to hold the path part (~S1/...) from the client's command
    char requested_client_path[256];
    // Buffer for constructing the full path on *this* server for .c files
    char absolute_local_path_s1[512];
    // General purpose buffer for sending file chunks or error messages
    char message_buffer[4096];             // Using a general buffer
    const char *error_message_text = NULL; // Pointer to the error message to send, if any
    // To store info about the file, like its size
    struct stat file_status_info;
    // Pointer for finding the file extension
    char *file_extension_dot = NULL;
    // Pointer to the server's home directory path
    char *server_home_directory = NULL;
    // File handle for reading local .c files
    FILE *local_file_stream = NULL;
    // Buffer for secondary retrieval
    char *secondary_file_content_buffer = NULL;
    // Redundant counter
    int validation_step_counter = 0;

    /* --- Step 1: Parse the Command --- */
    printf("DEBUG (PID %d) Download: Parsing command...\n", getpid());
    // Try to extract the file path part following "downlf ". Expect 1 item.
    if (sscanf(client_request_string, "downlf %255s", requested_client_path) != 1)
    {
        snprintf(message_buffer, sizeof(message_buffer), "ERROR: Invalid downlf command format. Use: downlf <~S1/path/to/file>");
        error_message_text = message_buffer;
        printf("ERROR (PID %d) Download: Command parsing failed. Format incorrect.\n", getpid());
        goto send_error_and_exit; // Jump to error handling
    }
    validation_step_counter++; // 1
    printf("DEBUG (PID %d) Download: Parsed OK -> Requested Path: '%s'\n", getpid(), requested_client_path);

    /* --- Step 2: Validate Path Prefix --- */
    printf("DEBUG (PID %d) Download: Validating path prefix (step %d)...\n", getpid(), validation_step_counter + 1);
    // Check if the path starts with "~S1/"
    if (strncmp(requested_client_path, "~S1/", 4) != 0)
    {
        snprintf(message_buffer, sizeof(message_buffer), "ERROR: Path must start with ~S1/");
        error_message_text = message_buffer;
        printf("ERROR (PID %d) Download: Invalid path prefix: '%s'.\n", getpid(), requested_client_path);
        goto send_error_and_exit; // Jump to error handling
    }
    validation_step_counter++; // 2
    printf("DEBUG (PID %d) Download: Path prefix '~S1/' is valid.\n", getpid());

    /* --- Step 3: Find File Extension --- */
    printf("DEBUG (PID %d) Download: Finding file extension (step %d)...\n", getpid(), validation_step_counter + 1);
    // Find the last '.' in the path.
    file_extension_dot = strrchr(requested_client_path, '.');
    // Check if '.' was found and it's not the *only* character or the first after a slash (basic check)
    if (file_extension_dot == NULL || file_extension_dot == requested_client_path || *(file_extension_dot - 1) == '/')
    {
        snprintf(message_buffer, sizeof(message_buffer), "ERROR: File path has no valid extension");
        error_message_text = message_buffer;
        printf("ERROR (PID %d) Download: No valid extension found in '%s'.\n", getpid(), requested_client_path);
        goto send_error_and_exit; // Jump to error handling
    }
    validation_step_counter++; // 3
    printf("DEBUG (PID %d) Download: Detected extension: '%s'\n", getpid(), file_extension_dot);

    /* --- Step 4: Get Server Home Directory --- */
    printf("DEBUG (PID %d) Download: Getting server HOME directory (step %d)...\n", getpid(), validation_step_counter + 1);
    server_home_directory = getenv("HOME");
    // Check if getenv failed
    if (server_home_directory == NULL) // Changed from !home
    {
        snprintf(message_buffer, sizeof(message_buffer), "ERROR: Server configuration error (cannot find HOME directory)");
        error_message_text = message_buffer;
        perror("ERROR (PID %d) Download: getenv(\"HOME\") failed");
        goto send_error_and_exit; // Jump to error handling
    }
    // Redundant check (just for structure)
    // if (strlen(server_home_directory) == 0)
    // {
    //     snprintf(message_buffer, sizeof(message_buffer), "ERROR: Server configuration error (HOME directory is empty)");
    //     error_message_text = message_buffer;
    //     fprintf(stderr, "ERROR (PID %d) Download: getenv(\"HOME\") returned an empty string.\n", getpid());
    //     goto send_error_and_exit;
    // }
    validation_step_counter++; // 4
    printf("DEBUG (PID %d) Download: Server HOME directory: '%s'\n", getpid(), server_home_directory);

    /* --- Step 5: Handle File Based on Extension --- */
    printf("DEBUG (PID %d) Download: Determining action based on extension '%s'.\n", getpid(), file_extension_dot);

    // --- Case 1: .c file (handle locally) ---
    if (strcasecmp(file_extension_dot, ".c") == 0)
    {
        printf("INFO (PID %d) Download: File is .c, processing locally.\n", getpid());
        // Construct the full path to the file within this server's ~/S1 directory
        // requested_client_path + 4 skips the "~S1/" part
        snprintf(absolute_local_path_s1, sizeof(absolute_local_path_s1), "%s/S1/%s", server_home_directory, requested_client_path + 4);
        printf("DEBUG (PID %d) Download: Constructed local absolute path: '%s'\n", getpid(), absolute_local_path_s1);

        // Check if the file actually exists using stat()
        printf("DEBUG (PID %d) Download: Checking file existence with stat()...\n", getpid());
        if (stat(absolute_local_path_s1, &file_status_info) < 0)
        { // Check for error (< 0)
            // If stat fails, the file likely doesn't exist or there's a permission issue.
            char temp_error_buf[600]; // Need buffer because path is variable
            snprintf(temp_error_buf, sizeof(temp_error_buf), "ERROR: File not found or inaccessible: %s", requested_client_path);
            perror("WARN (PID %d) Download: stat() failed"); // Log system error
            printf("ERROR (PID %d) Download: Local file '%s' not found or cannot access.\n", getpid(), absolute_local_path_s1);

            // Send error message to client
            write(connected_client_fd, temp_error_buf, strlen(temp_error_buf));
            return; // Exit the function early since the file doesn't exist
        }
        printf("DEBUG (PID %d) Download: stat() successful. File exists. Size from stat: %ld bytes.\n", getpid(), (long)file_status_info.st_size);

        /* Open the file for reading (binary mode "rb") */
        printf("DEBUG (PID %d) Download: Opening local file '%s' for reading...\n", getpid(), absolute_local_path_s1);
        local_file_stream = fopen(absolute_local_path_s1, "rb");
        // Check if fopen failed
        if (local_file_stream == NULL) // Check for NULL
        {
            snprintf(message_buffer, sizeof(message_buffer), "ERROR: Failed to open local file: %s", requested_client_path);
            error_message_text = message_buffer;
            perror("ERROR (PID %d) Download: fopen() failed"); // Log system error
            goto send_error_and_exit;                          // Jump to error handling
        }
        printf("DEBUG (PID %d) Download: Local file opened successfully.\n", getpid());

        /* Get file size using fseek/ftell */
        // Using uint32_t as required by the protocol with client
        uint32_t local_file_byte_count = 0;
        // Seek to end, get position, seek back to start
        if (fseek(local_file_stream, 0, SEEK_END) != 0)
        {
            snprintf(message_buffer, sizeof(message_buffer), "ERROR: fseek(SEEK_END) failed for %s", requested_client_path);
            error_message_text = message_buffer;
            perror("ERROR (PID %d) Download: fseek SEEK_END failed");
            goto send_error_and_exit;
        }
        long ftell_size = ftell(local_file_stream);
        if (ftell_size < 0)
        {
            snprintf(message_buffer, sizeof(message_buffer), "ERROR: ftell failed for %s", requested_client_path);
            error_message_text = message_buffer;
            perror("ERROR (PID %d) Download: ftell failed");
            goto send_error_and_exit;
        }
        // Check for potential overflow if size > UINT32_MAX?
        if (ftell_size > UINT32_MAX)
        {
            fprintf(stderr, "WARN (PID %d) Download: File size %ld exceeds uint32_t limit! Sending truncated size.\n", getpid(), ftell_size);
            local_file_byte_count = UINT32_MAX; // Send max possible 32-bit value
        }
        else
        {
            local_file_byte_count = (uint32_t)ftell_size;
        }
        if (fseek(local_file_stream, 0, SEEK_SET) != 0)
        {
            snprintf(message_buffer, sizeof(message_buffer), "ERROR: fseek(SEEK_SET) failed for %s", requested_client_path);
            error_message_text = message_buffer;
            perror("ERROR (PID %d) Download: fseek SEEK_SET failed");
            goto send_error_and_exit;
        }
        printf("DEBUG (PID %d) Download: File size determined via fseek/ftell: %u bytes.\n", getpid(), local_file_byte_count);

        /* Send the file size (uint32_t) to the client */
        if ((unsigned long)file_status_info.st_size > UINT32_MAX)
        {
            fprintf(stderr, "ERROR (PID %d) Download: .c file size %ld exceeds protocol limit!\n", getpid(), (long)file_status_info.st_size);
            // Send ERROR message to client instead? Or handle appropriately.
            // For now, sending 0 size might be interpreted as error/empty by client.
            uint32_t zero_size_net = htonl(0);
            write(connected_client_fd, &zero_size_net, sizeof(zero_size_net));
            fclose(local_file_stream); // Close file
            return;                    // Abort sending this file
        }
        uint32_t file_size_u32 = (uint32_t)file_status_info.st_size; // Cast to uint32_t
        uint32_t size_network_order = htonl(file_size_u32);          // Convert to network byte order
        printf("DEBUG (PID %d) Download: Sending .c file size %u (Network: 0x%x) to client...\n", getpid(), file_size_u32, size_network_order);

        // Send the correctly formatted size header
        if (write(connected_client_fd, &size_network_order, sizeof(size_network_order)) != sizeof(size_network_order))
        {
            perror("ERROR (PID %d) Download: Failed sending .c file size header");
            fclose(local_file_stream); // Close file before returning
            return;                    // Error
        }
        printf("DEBUG (PID %d) Download: File size sent successfully.\n", getpid());

        /* Send the file content */
        printf("DEBUG (PID %d) Download: Starting loop to send file content...\n", getpid());
        size_t bytes_read_this_chunk;
        size_t total_bytes_sent = 0;
        // Reuse message_buffer as file_chunk_data
        char *file_chunk_data = message_buffer;
        size_t chunk_buffer_size = sizeof(message_buffer);

        // Loop while we can read data from the file
        while ((bytes_read_this_chunk = fread(file_chunk_data, 1, chunk_buffer_size, local_file_stream)) > 0)
        {
            // Try to write the chunk we just read to the client socket
            ssize_t write_data_check = write(connected_client_fd, file_chunk_data, bytes_read_this_chunk);
            // Check for write error or incomplete write
            if (write_data_check < 0 || (size_t)write_data_check != bytes_read_this_chunk)
            {
                perror("ERROR (PID %d) Download: Failed to send file content chunk to client");
                // Client might be gone, no message needed
                goto cleanup_and_exit; // Clean up and return
            }
            total_bytes_sent += write_data_check;
            // printf("DEBUG (PID %d) Download: Sent chunk, total %zu bytes...\n", getpid(), total_bytes_sent);
        }

        // Check if fread finished due to error
        if (ferror(local_file_stream))
        {
            perror("ERROR (PID %d) Download: Error reading from local file during sending");
            // Client might have received partial data, but we can't fix it now.
        }

        // Close the local file - Already handled in cleanup
        // fclose(local_file_stream);
        // local_file_stream = NULL; // Mark as closed
        printf("INFO (PID %d) Download: Local file '%s' sent to client (Total: %zu bytes).\n", getpid(), requested_client_path, total_bytes_sent);

    } // End of .c file handling
    else // --- Case 2: Not .c (pdf, txt, zip) -> Request from secondary server ---
    {
        int target_secondary_server_id = 0;
        printf("INFO (PID %d) Download: File is not .c. Determining secondary server...\n", getpid());

        // Determine secondary server based on extension
        if (strcasecmp(file_extension_dot, ".pdf") == 0)
        {
            target_secondary_server_id = 2;
        }
        else if (strcasecmp(file_extension_dot, ".txt") == 0)
        {
            target_secondary_server_id = 3;
        }
        else if (strcasecmp(file_extension_dot, ".zip") == 0)
        {
            target_secondary_server_id = 4;
        }
        else
        {
            snprintf(message_buffer, sizeof(message_buffer), "ERROR: Unsupported file type '%s'", file_extension_dot);
            error_message_text = message_buffer;
            printf("ERROR (PID %d) Download: Unsupported extension '%s'.\n", getpid(), file_extension_dot);
            goto send_error_and_exit;
        }

        printf("DEBUG (PID %d) Download: File type '%s' should be on S%d.\n", getpid(), file_extension_dot, target_secondary_server_id);

        /* Allocate buffer for secondary server response */
        size_t buffer_allocation_size = 10 * 1024 * 1024; // 10MB
        secondary_file_content_buffer = malloc(buffer_allocation_size);
        if (secondary_file_content_buffer == NULL)
        {
            snprintf(message_buffer, sizeof(message_buffer), "ERROR: Server memory allocation failed");
            error_message_text = message_buffer;
            perror("ERROR (PID %d) Download: malloc failed");
            goto send_error_and_exit;
        }

        /* Retrieve file from secondary server */
        size_t retrieved_file_byte_count = 0;
        printf("DEBUG (PID %d) Download: Calling retrieve_from_secondary_server(S%d, '%s')\n",
               getpid(), target_secondary_server_id, requested_client_path);

        int retrieval_status = retrieve_from_secondary_server(
            target_secondary_server_id,
            requested_client_path,
            secondary_file_content_buffer,
            &retrieved_file_byte_count);

        if (!retrieval_status || retrieved_file_byte_count == 0)
        {
            snprintf(message_buffer, sizeof(message_buffer),
                     "ERROR: File '%s' not found on S%d", requested_client_path, target_secondary_server_id);
            error_message_text = message_buffer;
            printf("ERROR (PID %d) Download: Retrieval failed or empty file.\n", getpid());
            goto send_error_and_exit;
        }

        /* Validate retrieved file size */
        if (retrieved_file_byte_count > UINT32_MAX)
        {
            fprintf(stderr, "WARN (PID %d) Download: File size %zu exceeds protocol limit\n",
                    getpid(), retrieved_file_byte_count);
            snprintf(message_buffer, sizeof(message_buffer),
                     "WARNING: File truncated to %u bytes (original size %zu)",
                     UINT32_MAX, retrieved_file_byte_count);
            error_message_text = message_buffer;
        }

        /* Send file size header */
        uint32_t network_byte_order_size = htonl((uint32_t)retrieved_file_byte_count);
        printf("DEBUG (PID %d) Download: Sending size header: %u bytes\n",
               getpid(), (uint32_t)retrieved_file_byte_count);

        if (write(connected_client_fd, &network_byte_order_size, sizeof(network_byte_order_size)) != sizeof(network_byte_order_size))
        {
            perror("ERROR (PID %d) Download: Failed to send size header");
            goto cleanup_and_exit;
        }

        /* Send file content */
        printf("DEBUG (PID %d) Download: Sending %zu bytes to client\n", getpid(), retrieved_file_byte_count);
        ssize_t bytes_sent = write(connected_client_fd, secondary_file_content_buffer, retrieved_file_byte_count);

        if (bytes_sent < 0 || (size_t)bytes_sent != retrieved_file_byte_count)
        {
            perror("ERROR (PID %d) Download: File content send failed");
            snprintf(message_buffer, sizeof(message_buffer),
                     "ERROR: Partial transfer (%zd/%zu bytes sent)", bytes_sent, retrieved_file_byte_count);
            error_message_text = message_buffer;
            goto cleanup_and_exit;
        }

        printf("INFO (PID %d) Download: Successfully sent %zd bytes to client\n", getpid(), bytes_sent);

        // cleanup_and_exit:
        //     free(secondary_file_content_buffer);
        //     secondary_file_content_buffer = NULL;
        //     return;
    }
    // End of non-.c file handling

    // If we reach here, the operation was successful (either local send or secondary retrieval/send)
    printf("DEBUG (PID %d) Download: --- Exiting handle_download_command (Success) ---\n", getpid());
    goto cleanup_and_exit; // Jump to common cleanup

/* --- Error Handling Target --- */
send_error_and_exit:
    printf("DEBUG (PID %d) Download: Sending error message to client: '%s'\n", getpid(), error_message_text);
    // Ensure we have a valid error message pointer
    if (error_message_text != NULL)
    {
        ssize_t write_err_check = write(connected_client_fd, error_message_text, strlen(error_message_text));
        if (write_err_check < 0)
        {
            perror("ERROR (PID %d) Download: Failed to send error message to client");
        }
    }
    else
    {
        // Fallback if error_message_text was somehow NULL
        const char *fallback_err = "ERROR: An unknown server error occurred during download";
        printf("ERROR (PID %d) Download: error_message_text was NULL! Sending fallback.\n", getpid());
        write(connected_client_fd, fallback_err, strlen(fallback_err));
    }

/* --- Common Cleanup Target --- */
cleanup_and_exit:
    // Clean up any resources that might be open
    if (local_file_stream != NULL)
    {
        fclose(local_file_stream);
    }
    if (secondary_file_content_buffer != NULL)
    {
        printf("DEBUG (PID %d) Download: Freeing secondary retrieval buffer at %p.\n", getpid(), secondary_file_content_buffer);
        free(secondary_file_content_buffer);
        secondary_file_content_buffer = NULL;
    }

    printf("DEBUG (PID %d) Download: --- Exiting handle_download_command (Cleanup Complete) ---\n", getpid());
    return;

} // End of handle_download_command

/* ========================================================================= */
/*               handle_remove_command Function                              */
/* ========================================================================= */
/**
 * @brief My function to handle the 'removef' command.
 * The client wants to delete a file specified by a path like ~S1/folder/file.ext.
 *
 * Logic:
 * 1. Check the command format ("removef ~S1/...").
 * 2. Find the file extension.
 * 3. If it's a '.c' file:
 *    a. Construct the full local path in ~/S1.
 *    b. Check if the file exists using stat(). (We cannot remove non-existent file).
 *    c. Try to remove the local file using remove().
 *    d. Prepare a "SUCCESS" or "ERROR" message.
 * 4. If it's '.pdf', '.txt', or '.zip':
 *    a. Determine the correct secondary server (S2, S3, S4).
 *    b. Call my helper function `remove_from_secondary_server`.
 *    c. Prepare a "SUCCESS" or "ERROR" message based on the helper function's result.
 * 5. Handle errors (bad command, path, extension, file not found, permission error, secondary server failure).
 * 6. Send the final "SUCCESS: ..." or "ERROR: ..." message to the client.
 *
 * @param requesting_client_socket The socket number for talking to the client.
 * @param full_command_line The command string received (e.g., "removef ~S1/stuff/myfile.c").
 */
void handle_remove_command(int requesting_client_socket, char *full_command_line)
{
    printf("DEBUG (PID %d) Remove: --- Entering handle_remove_command ---\n", getpid());
    printf("DEBUG (PID %d) Remove: Received raw command: '%s'\n", getpid(), full_command_line);

    // Variable to hold the path part (~S1/...) from the command
    char path_to_remove_client_fmt[256];
    // Pointer to the file extension within the path
    char *file_extension_str = NULL;
    // Pointer to the server's home directory path
    char *server_home_dir_path = NULL;
    // Buffer for the final message (SUCCESS or ERROR) sent back to the client
    char final_status_message_buffer[512]; // Increased size slightly for more descriptive messages
    // Buffer for constructing the full path for local .c files
    char absolute_path_on_s1[512];
    // Just an indicator
    int operation_type = 1; // Redundant variable
    // Flag to track if operation failed
    int operation_failed_flag = 0; // 0=OK, 1=Failed

    // Initialize response buffer
    final_status_message_buffer[0] = '\0';

    /* --- Step 1: Parse Command --- */
    printf("DEBUG (PID %d) Remove: Parsing command...\n", getpid());
    // Extract the path after "removef ". Expect 1 item.
    if (sscanf(full_command_line, "removef %255s", path_to_remove_client_fmt) != 1)
    {
        snprintf(final_status_message_buffer, sizeof(final_status_message_buffer), "ERROR: Invalid removef command format. Use: removef <~S1/path/to/file>");
        printf("ERROR (PID %d) Remove: Command parsing failed.\n", getpid());
        operation_failed_flag = 1;
        goto send_final_message; // Jump to the end to send the message
    }
    printf("DEBUG (PID %d) Remove: Parsed OK -> Path to remove: '%s'\n", getpid(), path_to_remove_client_fmt);

    /* --- Step 2: Validate Path Prefix --- */
    printf("DEBUG (PID %d) Remove: Validating path prefix...\n", getpid());
    // Path must start with "~S1/"
    if (strncmp(path_to_remove_client_fmt, "~S1/", 4) != 0)
    {
        snprintf(final_status_message_buffer, sizeof(final_status_message_buffer), "ERROR: Path must start with ~S1/");
        printf("ERROR (PID %d) Remove: Invalid path prefix: '%s'.\n", getpid(), path_to_remove_client_fmt);
        operation_failed_flag = 1;
        goto send_final_message; // Jump to the end
    }
    printf("DEBUG (PID %d) Remove: Path prefix OK.\n", getpid());

    /* --- Step 3: Find File Extension --- */
    printf("DEBUG (PID %d) Remove: Finding file extension...\n", getpid());
    // Find the last '.'
    file_extension_str = strrchr(path_to_remove_client_fmt, '.');
    // Check if found and valid
    if (file_extension_str == NULL || file_extension_str == path_to_remove_client_fmt || *(file_extension_str - 1) == '/') // Check for NULL or invalid position
    {
        snprintf(final_status_message_buffer, sizeof(final_status_message_buffer), "ERROR: File path has no valid extension");
        printf("ERROR (PID %d) Remove: No valid extension found in '%s'.\n", getpid(), path_to_remove_client_fmt);
        operation_failed_flag = 1;
        goto send_final_message; // Jump to the end
    }
    printf("DEBUG (PID %d) Remove: Detected extension: '%s'\n", getpid(), file_extension_str);

    /* --- Step 4: Get Server Home Directory --- */
    printf("DEBUG (PID %d) Remove: Getting server HOME directory...\n", getpid());
    server_home_dir_path = getenv("HOME");
    // Check if getenv failed
    if (server_home_dir_path == NULL) // Check for NULL
    {
        snprintf(final_status_message_buffer, sizeof(final_status_message_buffer), "ERROR: Server configuration error (cannot find HOME directory)");
        perror("ERROR (PID %d) Remove: getenv(\"HOME\") failed");
        operation_failed_flag = 1;
        goto send_final_message; // Jump to the end
    }
    // Redundant check using operation_type
    if (operation_type == 1 && server_home_dir_path == NULL)
    {
        snprintf(final_status_message_buffer, sizeof(final_status_message_buffer), "ERROR: Server configuration error (redundant check failed)");
        printf("ERROR (PID %d) Remove: Redundant HOME check failed (logic error?).\n", getpid());
        operation_failed_flag = 1;
        goto send_final_message;
    }
    printf("DEBUG (PID %d) Remove: Server HOME directory: '%s'\n", getpid(), server_home_dir_path);

    /* --- Step 5: Handle Based on Extension --- */
    printf("DEBUG (PID %d) Remove: Determining action for extension '%s'...\n", getpid(), file_extension_str);

    // --- Case 1: .c file (remove locally) ---
    if (strcasecmp(file_extension_str, ".c") == 0)
    {
        printf("INFO (PID %d) Remove: File is .c, attempting local removal.\n", getpid());
        // Construct the full absolute path in ~/S1/
        snprintf(absolute_path_on_s1, sizeof(absolute_path_on_s1), "%s/S1/%s", server_home_dir_path, path_to_remove_client_fmt + 4);
        printf("DEBUG (PID %d) Remove: Absolute path for local removal: '%s'\n", getpid(), absolute_path_on_s1);

        /* IMPORTANT: Check if file exists *before* trying to remove */
        struct stat file_stat_check; // Variable to hold stat info (we don't actually use the info)
        printf("DEBUG (PID %d) Remove: Checking existence with stat()...\n", getpid());
        if (stat(absolute_path_on_s1, &file_stat_check) != 0) // Check if stat failed
        {
            // If stat fails, file doesn't exist or permissions issue. Use errno to be more specific?
            if (errno == ENOENT)
            { // ENOENT means "No such file or directory"
                printf("ERROR (PID %d) Remove: File '%s' does not exist.\n", getpid(), absolute_path_on_s1);
                snprintf(final_status_message_buffer, sizeof(final_status_message_buffer), "ERROR: File not found: %s", path_to_remove_client_fmt);
            }
            else
            {
                // Other error (e.g., permission denied EACCES)
                snprintf(final_status_message_buffer, sizeof(final_status_message_buffer), "ERROR: Cannot access file path: %s (errno: %d)", path_to_remove_client_fmt, errno);
                perror("WARN (PID %d) Remove: stat() failed");
                printf("ERROR (PID %d) Remove: stat() failed for '%s' (errno %d).\n", getpid(), absolute_path_on_s1, errno);
            }
            operation_failed_flag = 1;
            goto send_final_message; // Jump to send message
        }
        // If stat succeeded, the file exists. Now try to remove it.
        printf("DEBUG (PID %d) Remove: File exists. Attempting remove()...\n", getpid());
        if (remove(absolute_path_on_s1) != 0) // Check if remove() failed
        {
            // remove() failed (e.g., permissions error EACCES, or it's a directory EISDIR)
            snprintf(final_status_message_buffer, sizeof(final_status_message_buffer), "ERROR: Failed to remove file: %s (errno: %d)", path_to_remove_client_fmt, errno);
            perror("ERROR (PID %d) Remove: remove() failed"); // Log system error
            printf("ERROR (PID %d) Remove: remove() failed for '%s' (errno %d).\n", getpid(), absolute_path_on_s1, errno);
            operation_failed_flag = 1;
            goto send_final_message; // Jump to send message
        }

        // If remove succeeded!
        printf("INFO (PID %d) Remove: Successfully removed local file '%s'.\n", getpid(), absolute_path_on_s1);
        snprintf(final_status_message_buffer, sizeof(final_status_message_buffer), "SUCCESS: File removed: %s", path_to_remove_client_fmt);
        // Success message is set, now jump to send it.
        goto send_final_message;

    } // End of .c file handling
    else // --- Case 2: Not .c (pdf, txt, zip) -> Request removal from secondary server ---
    {
        int secondary_server_target = 0; // Which server: 2, 3, or 4?
        printf("INFO (PID %d) Remove: File is not .c. Determining secondary server for removal request.\n", getpid());

        // Determine target server
        if (strcasecmp(file_extension_str, ".pdf") == 0)
        {
            secondary_server_target = 2;
        }
        else if (strcasecmp(file_extension_str, ".txt") == 0)
        {
            secondary_server_target = 3;
        }
        else if (strcasecmp(file_extension_str, ".zip") == 0)
        {
            secondary_server_target = 4;
        }
        else
        {
            // Invalid extension (shouldn't happen if upload validation worked)
            snprintf(final_status_message_buffer, sizeof(final_status_message_buffer), "ERROR: Unsupported file type for removal: %s", file_extension_str);
            printf("ERROR (PID %d) Remove: Unsupported extension '%s'.\n", getpid(), file_extension_str);
            operation_failed_flag = 1;
            goto send_final_message; // Jump to send message
        }

        printf("DEBUG (PID %d) Remove: Requesting removal of '%s' from S%d.\n", getpid(), path_to_remove_client_fmt, secondary_server_target);

        /* Call the function to request removal from secondary server */
        // Assume remove_from_secondary_server(num, path) returns 1 for success, 0 for failure.
        printf("DEBUG (PID %d) Remove: Calling remove_from_secondary_server(S%d, Path='%s')...\n",
               getpid(), secondary_server_target, path_to_remove_client_fmt);

        if (remove_from_secondary_server(secondary_server_target, path_to_remove_client_fmt))
        {
            // Secondary server reported success
            printf("INFO (PID %d) Remove: S%d reported successful removal of '%s'.\n", getpid(), secondary_server_target, path_to_remove_client_fmt);
            // Prepare success message, including which server it was on
            snprintf(final_status_message_buffer, sizeof(final_status_message_buffer),
                     "SUCCESS: File removed (Path: %s, Server: S%d)", path_to_remove_client_fmt, secondary_server_target);
            // Jump to send message
            goto send_final_message;
        }
        else // remove_from_secondary_server returned failure
        {
            // Secondary server reported failure (e.g., file not found there, or other error)
            printf("ERROR (PID %d) Remove: S%d reported failure to remove '%s'.\n", getpid(), secondary_server_target, path_to_remove_client_fmt);
            // Prepare error message
            snprintf(final_status_message_buffer, sizeof(final_status_message_buffer),
                     "ERROR: Failed to remove file from S%d (maybe not found?): %s", secondary_server_target, path_to_remove_client_fmt);
            operation_failed_flag = 1; // Mark failure
            // Jump to send message
            goto send_final_message;
        }
    } // End of non-.c file handling

/* --- Send Final Message --- */
// This is the target for all the goto jumps.
send_final_message:
    // Check if a message was prepared (could be SUCCESS or ERROR)
    if (final_status_message_buffer[0] == '\0')
    {
        // Fallback if no message was explicitly set
        if (operation_failed_flag == 1)
        {
            snprintf(final_status_message_buffer, sizeof(final_status_message_buffer), "ERROR: Remove failed with unspecified error.");
            printf("ERROR (PID %d) Remove: Operation failed but no specific message set!\n", getpid());
        }
        else
        {
            snprintf(final_status_message_buffer, sizeof(final_status_message_buffer), "SUCCESS: Remove operation completed (no specific message).");
            printf("WARN (PID %d) Remove: Operation succeeded but no specific message set!\n", getpid());
        }
    }

    // Send the final message
    printf("DEBUG (PID %d) Remove: Sending final response: '%s'\n", getpid(), final_status_message_buffer);
    ssize_t write_check = write(requesting_client_socket, final_status_message_buffer, strlen(final_status_message_buffer));
    if (write_check < 0)
    {
        perror("ERROR (PID %d) Remove: Failed to send final remove response to client");
    }

    printf("DEBUG (PID %d) Remove: --- Exiting handle_remove_command ---\n", getpid());
    return; // Return to process_client loop

} // End of handle_remove_command

/* ========================================================================= */
/*             handle_downltar_command Function                              */
/* ========================================================================= */
/**
 * @brief My job is to handle the 'downltar' command.
 * The client wants a tarball (.tar file) containing all files of a specific type (.c, .pdf, or .txt).
 *
 * Logic:
 * 1. Parse the command ("downltar .filetype").
 * 2. Validate the filetype (only .c, .pdf, .txt allowed).
 * 3. Get the server's home directory path.
 * 4. Create a unique temporary directory (e.g., ~/temp_tar_PID) to work in.
 * 5. If filetype is '.c':
 *    a. Use `find` and `tar` system commands to create 'cfiles.tar' directly in the temp dir, containing all .c files under ~/S1.
 * 6. If filetype is '.pdf' or '.txt':
 *    a. Determine the target secondary server (S2 for pdf, S3 for txt).
 *    b. Call `get_tar_from_secondary_server` to request the tarball (e.g., pdf.tar or text.tar) and save it in our temp dir.
 *    c. If retrieval fails, set error message, and go to cleanup.
 * 7. Check if the tar file was created successfully and is not empty (using stat()). If not, set error message, go to cleanup.
 * 8. If tar file is okay:
 *    a. Open the tar file. If fails, set error message, go to cleanup.
 *    b. Get its size. Check for uint32_t overflow.
 *    c. Send the size (uint32_t) to the client. If fails, close file, go to cleanup_only.
 *    d. Read the tar file chunk by chunk and send content to the client. If fails, close file, go to cleanup_only.
 *    e. Close the tar file.
 * 9. Send error message to client if one was set during the process.
 * 10. Cleanup: Remove the temporary directory and its contents.
 *
 * @param client_comm_socket The socket descriptor for communication with the client.
 * @param received_command_line The full command string (e.g., "downltar .pdf").
 */
void handle_downltar_command(int client_comm_socket, char *received_command_line)
{
    printf("DEBUG (PID %d) TarDownload: --- Entering handle_downltar_command ---\n", getpid());
    printf("DEBUG (PID %d) TarDownload: Received raw command: '%s'\n", getpid(), received_command_line);

    // Variable declarations
    char requested_file_type[10];             // To store ".c", ".pdf", or ".txt"
    const char *client_error_response = NULL; // Pointer to error message for client, if any
    char server_home_dir_path[512];           // Server's HOME path (buffer)
    char temporary_work_directory[256];       // Path to the temp dir (e.g., ~/temp_tar_12345)
    char full_tar_filepath[512];              // Full path to the tar file inside temp dir
    char system_command_buffer[1024];         // Buffer for constructing system() commands (mkdir, tar, rm)
    char error_message_storage[200];          // Buffer to store generated error messages for client
    int operation_status_ok = 1;              // Redundant flag: 1 = ok, 0 = failed
    int needs_cleanup_routine = 0;            // Flag to indicate if temp dir needs removal at the end
    FILE *tar_file_stream_ptr = NULL;         // File pointer for reading the tar file

    // Initialize paths/buffers
    requested_file_type[0] = '\0';
    server_home_dir_path[0] = '\0';
    temporary_work_directory[0] = '\0';
    full_tar_filepath[0] = '\0';
    error_message_storage[0] = '\0';

    /* --- Step 1: Parse Command --- */
    printf("DEBUG (PID %d) TarDownload: Parsing command...\n", getpid());
    // Extract the filetype (e.g., ".c") after "downltar ". Expect 1 item.
    if (sscanf(received_command_line, "downltar %9s", requested_file_type) != 1)
    {
        snprintf(error_message_storage, sizeof(error_message_storage), "ERROR: Invalid downltar command format. Use: downltar <.c|.pdf|.txt>");
        client_error_response = error_message_storage;
        printf("ERROR (PID %d) TarDownload: Command parsing failed.\n", getpid());
        operation_status_ok = 0;        // Mark failure
        goto send_response_and_cleanup; // Jump to cleanup/response
    }
    printf("DEBUG (PID %d) TarDownload: Parsed OK -> Requested type: '%s'\n", getpid(), requested_file_type);

    /* --- Step 2: Validate File Type --- */
    printf("DEBUG (PID %d) TarDownload: Validating file type...\n", getpid());
    // Check if it's one of the allowed types: .c, .pdf, .txt
    int is_valid_type_for_tar = (strcasecmp(requested_file_type, ".c") == 0 ||
                                 strcasecmp(requested_file_type, ".pdf") == 0 ||
                                 strcasecmp(requested_file_type, ".txt") == 0);

    if (is_valid_type_for_tar == 0) // Check boolean result
    {
        snprintf(error_message_storage, sizeof(error_message_storage), "ERROR: Unsupported file type for downltar. Must be .c, .pdf, or .txt");
        client_error_response = error_message_storage;
        printf("ERROR (PID %d) TarDownload: Invalid file type '%s'.\n", getpid(), requested_file_type);
        operation_status_ok = 0;        // Mark failure
        goto send_response_and_cleanup; // Jump to cleanup/response
    }
    printf("DEBUG (PID %d) TarDownload: File type '%s' is valid for downltar.\n", getpid(), requested_file_type);

    /* --- Step 3: Get Home Directory --- */
    printf("DEBUG (PID %d) TarDownload: Getting server HOME directory...\n", getpid());
    char *home_env_ptr = getenv("HOME");
    // Check if getenv failed
    if (home_env_ptr == NULL) // Check for NULL
    {
        snprintf(error_message_storage, sizeof(error_message_storage), "ERROR: Server configuration error (cannot find HOME directory)");
        client_error_response = error_message_storage;
        perror("ERROR (PID %d) TarDownload: getenv(\"HOME\") failed");
        operation_status_ok = 0;        // Mark failure
        goto send_response_and_cleanup; // Jump to cleanup/response
    }
    // Copy to our buffer for safety
    strncpy(server_home_dir_path, home_env_ptr, sizeof(server_home_dir_path) - 1);
    server_home_dir_path[sizeof(server_home_dir_path) - 1] = '\0';
    printf("DEBUG (PID %d) TarDownload: Server HOME: '%s'\n", getpid(), server_home_dir_path);

    /* --- Step 4: Create Temporary Directory --- */
    // Create a unique directory name using the process ID (PID)
    snprintf(temporary_work_directory, sizeof(temporary_work_directory), "%s/s1_temp_tar_%d", server_home_dir_path, getpid());
    // Construct the mkdir command
    /* --- Step 4: Create Temporary Directory using mkdir() C function --- */
    // Create a unique directory name using the process ID (PID)
    snprintf(temporary_work_directory, sizeof(temporary_work_directory), "%s/s1_temp_tar_%d", server_home_dir_path, getpid());
    printf("DEBUG (PID %d) TarDownload: Attempting to create directory using mkdir(): '%s'\n", getpid(), temporary_work_directory);

    // Use the mkdir() system call directly (mode 0777 gives standard permissions)
    if (mkdir(temporary_work_directory, 0777) != 0)
    {
        // Check if the error was because the directory already exists (which is okay)
        if (errno != EEXIST)
        {
            // It's some other error (e.g., permissions)
            perror("ERROR (PID %d) TarDownload: mkdir() function call failed");
            snprintf(error_message_storage, sizeof(error_message_storage), "ERROR: Server failed creating temp dir (mkdir errno %d)", errno);
            client_error_response = error_message_storage;
            operation_status_ok = 0;
            // No cleanup needed as dir wasn't successfully created by us now
            needs_cleanup_routine = 0;
            goto send_response_and_cleanup;
        }
        else
        {
            // Directory already existed, which is fine for our purposes.
            printf("DEBUG (PID %d) TarDownload: mkdir() reported directory '%s' already exists.\n", getpid(), temporary_work_directory);
            needs_cleanup_routine = 1; // We still need to clean it up later
        }
    }
    else
    {
        // mkdir() succeeded!
        needs_cleanup_routine = 1;
        printf("DEBUG (PID %d) TarDownload: Temporary directory '%s' created successfully using mkdir().\n", getpid(), temporary_work_directory);
    }

    /* --- Step 5: Handle Tar Creation/Retrieval Based on Type --- */
    printf("DEBUG (PID %d) TarDownload: Processing based on file type '%s'...\n", getpid(), requested_file_type);
    // --- Case 5a: .c files (local tar creation) ---
    if (strcasecmp(requested_file_type, ".c") == 0)
    {
        // Set the target tar filename
        snprintf(full_tar_filepath, sizeof(full_tar_filepath), "%s/cfiles.tar", temporary_work_directory);
        printf("INFO (PID %d) TarDownload: Creating local tar for .c files: '%s'\n", getpid(), full_tar_filepath);

        // Construct the find + tar command:
        // find ~/S1 -name "*.c" -type f -print0 | xargs -0 tar -cf /path/to/temp/cfiles.tar 2>/dev/null
        snprintf(system_command_buffer, sizeof(system_command_buffer),
                 "find \"%s/S1\" -name \"*%s\" -type f -print0 | xargs -0 tar -cf \"%s\" 2>/dev/null",
                 server_home_dir_path, requested_file_type, full_tar_filepath);
        printf("DEBUG (PID %d) TarDownload: Executing tar command: %s\n", getpid(), system_command_buffer);

        // Execute the tar command
        int tar_command_status = system(system_command_buffer);
        // Check status - just log a warning if non-zero, stat() will verify the result later
        if (tar_command_status != 0)
        {
            fprintf(stderr, "WARN (PID %d) TarDownload: tar command pipeline returned status %d. Will check resulting file.\n", getpid(), tar_command_status);
        }
        else
        {
            printf("DEBUG (PID %d) TarDownload: Tar command pipeline executed (returned 0).\n", getpid());
        }
    }
    // --- Case 5b: .pdf or .txt files (retrieve from secondary) ---
    else
    {
        int target_server_for_tar = 0;
        char expected_tar_filename[20]; // "pdf.tar" or "text.tar"

        // Determine server number and tar filename
        if (strcasecmp(requested_file_type, ".pdf") == 0)
        {
            target_server_for_tar = 2;
            strcpy(expected_tar_filename, "pdf.tar");
        }
        else
        { // Must be .txt based on earlier validation
            target_server_for_tar = 3;
            strcpy(expected_tar_filename, "text.tar");
        }

        // Set the full path where we want to save the received tar file
        snprintf(full_tar_filepath, sizeof(full_tar_filepath), "%s/%s", temporary_work_directory, expected_tar_filename);
        printf("INFO (PID %d) TarDownload: Requesting '%s' from S%d, saving to '%s'.\n", getpid(), expected_tar_filename, target_server_for_tar, full_tar_filepath);

        // Call the function to get the tar file from the secondary server
        // It returns 1 on success, 0 on failure.
        // Using simple if condition
        if (get_tar_from_secondary_server(target_server_for_tar, requested_file_type, full_tar_filepath) == 0)
        {
            // Retrieval failed.
            snprintf(error_message_storage, sizeof(error_message_storage), "ERROR: Failed to retrieve %s tarball from S%d", requested_file_type, target_server_for_tar);
            client_error_response = error_message_storage;
            printf("ERROR (PID %d) TarDownload: get_tar_from_secondary_server failed.\n", getpid());
            operation_status_ok = 0;        // Mark failure
            goto send_response_and_cleanup; // Jump to cleanup/response
        }
        printf("DEBUG (PID %d) TarDownload: Tar file retrieval from S%d successful.\n", getpid(), target_server_for_tar);
    } // End of handling non-.c types

    /* --- Step 6: Verify Tar File and Get Size --- */
    // Only proceed if the previous steps were okay
    if (operation_status_ok == 1)
    {
        printf("DEBUG (PID %d) TarDownload: Verifying resulting tar file: '%s'\n", getpid(), full_tar_filepath);
        struct stat tar_file_details;
        // Use stat() to check if the file exists and get its size
        if (stat(full_tar_filepath, &tar_file_details) != 0) // Using != 0 check
        {
            // stat failed.
            if (errno == ENOENT)
            {
                snprintf(error_message_storage, sizeof(error_message_storage), "ERROR: No files found or failed to create tarball (file not found)");
                printf("ERROR (PID %d) TarDownload: Resulting tar file '%s' not found.\n", getpid(), full_tar_filepath);
            }
            else
            {
                snprintf(error_message_storage, sizeof(error_message_storage), "ERROR: Cannot access resulting tarball (errno: %d)", errno);
                perror("ERROR (PID %d) TarDownload: stat() failed on tar file");
                printf("ERROR (PID %d) TarDownload: stat failed for '%s', errno %d.\n", getpid(), full_tar_filepath, errno);
            }
            client_error_response = error_message_storage;
            operation_status_ok = 0;        // Mark failure
            goto send_response_and_cleanup; // Jump to cleanup/response
        }

        // Check if the file size is zero
        if (tar_file_details.st_size == 0)
        {
            snprintf(error_message_storage, sizeof(error_message_storage), "ERROR: No files of the specified type found to include in tarball.");
            client_error_response = error_message_storage;
            printf("WARN (PID %d) TarDownload: Resulting tar file '%s' is empty (0 bytes).\n", getpid(), full_tar_filepath);
            operation_status_ok = 0;        // Mark failure
            goto send_response_and_cleanup; // Jump to cleanup/response
        }
        printf("DEBUG (PID %d) TarDownload: Tar file verified. Size: %ld bytes.\n", getpid(), (long)tar_file_details.st_size);

        /* --- Step 7: Send Tar File to Client --- */
        printf("DEBUG (PID %d) TarDownload: Opening tar file '%s' for sending...\n", getpid(), full_tar_filepath);
        tar_file_stream_ptr = fopen(full_tar_filepath, "rb");
        if (tar_file_stream_ptr == NULL) // Check for NULL
        {
            snprintf(error_message_storage, sizeof(error_message_storage), "ERROR: Failed to open created tar file for sending");
            client_error_response = error_message_storage;
            perror("ERROR (PID %d) TarDownload: fopen failed for tar file");
            printf("ERROR (PID %d) TarDownload: Failed to open '%s' for reading.\n", getpid(), full_tar_filepath);
            operation_status_ok = 0;        // Mark failure
            goto send_response_and_cleanup; // Jump to cleanup/response
        }
        printf("DEBUG (PID %d) TarDownload: Tar file opened successfully.\n", getpid());

        /* Send file size (uint32_t) */
        uint32_t size_to_send_client = 0;
        if (tar_file_details.st_size > UINT32_MAX)
        {
            fprintf(stderr, "WARN (PID %d) TarDownload: Tar file size %ld exceeds uint32_t! Sending truncated size.\n", getpid(), (long)tar_file_details.st_size);
            size_to_send_client = UINT32_MAX;
        }
        else
        {
            size_to_send_client = (uint32_t)tar_file_details.st_size;
        }
        printf("DEBUG (PID %d) TarDownload: Sending tar file size (%u) to client fd %d...\n", getpid(), size_to_send_client, client_comm_socket);
        ssize_t write_check_size = write(client_comm_socket, &size_to_send_client, sizeof(size_to_send_client));
        if (write_check_size < (ssize_t)sizeof(size_to_send_client))
        {
            perror("ERROR (PID %d) TarDownload: Failed to send tar file size to client");
            // Can't send error if write failed, just cleanup and return
            operation_status_ok = 0; // Mark failure (though can't report it)
            goto cleanup_only;
        }
        printf("DEBUG (PID %d) TarDownload: Tar file size sent successfully.\n", getpid());

        /* Send file content */
        printf("DEBUG (PID %d) TarDownload: Starting loop to send tar content...\n", getpid());
        // Reuse system_command_buffer for file chunks
        char *file_data_buffer = system_command_buffer;
        size_t file_data_buffer_capacity = sizeof(system_command_buffer);
        size_t bytes_read_from_file;
        size_t total_sent_bytes = 0;

        // Loop while reading chunks from the tar file
        while ((bytes_read_from_file = fread(file_data_buffer, 1, file_data_buffer_capacity, tar_file_stream_ptr)) > 0)
        {
            // Send the chunk to the client
            ssize_t bytes_written_to_socket = write(client_comm_socket, file_data_buffer, bytes_read_from_file);
            // Check for write error or incomplete write
            if (bytes_written_to_socket < 0 || (size_t)bytes_written_to_socket != bytes_read_from_file)
            {
                perror("ERROR (PID %d) TarDownload: Failed to send tar content chunk to client");
                operation_status_ok = 0; // Mark failure
                goto cleanup_only;       // Exit, client might be gone
            }
            total_sent_bytes += bytes_written_to_socket;
        }

        // Check for fread error after loop
        if (ferror(tar_file_stream_ptr))
        {
            perror("ERROR (PID %d) TarDownload: Error reading from tar file during sending");
            // Client might have partial data. Let cleanup happen.
            operation_status_ok = 0; // Mark failure, though likely too late to tell client.
        }

        // Close file stream - handled in cleanup
        // fclose(tar_file_stream_ptr);
        // tar_file_stream_ptr = NULL; // Mark as closed
        printf("INFO (PID %d) TarDownload: Tar file sent to client. Total %zu bytes.\n", getpid(), total_sent_bytes);
    }

/* --- Step 8: Send Response / Cleanup --- */
send_response_and_cleanup:
    // If an error occurred at any point before sending started
    if (client_error_response != NULL)
    {
        printf("DEBUG (PID %d) TarDownload: Sending final ERROR response: '%s'\n", getpid(), client_error_response);
        ssize_t write_error_final = write(client_comm_socket, client_error_response, strlen(client_error_response));
        if (write_error_final < 0)
        {
            perror("ERROR (PID %d) TarDownload: Failed to send final error message to client");
        }
    }
    else
    {
        // If we got here without client_error_response being set, it means success.
        // No explicit success message needed for client in this command? Original code didn't send one.
        printf("DEBUG (PID %d) TarDownload: Operation successful. No explicit success message sent to client.\n", getpid());
    }

cleanup_only: // Target for cases where we can't send response (e.g., write failed mid-transfer)
    /* --- Step 9: Cleanup Temporary Directory --- */
    if (needs_cleanup_routine == 1) // Only remove if it was created successfully
    {
        // Construct the remove command: rm -rf /path/to/temp_dir
        snprintf(system_command_buffer, sizeof(system_command_buffer), "rm -rf \"%s\"", temporary_work_directory);
        printf("DEBUG (PID %d) TarDownload: Cleaning up temporary directory: %s\n", getpid(), system_command_buffer);
        // Execute cleanup command
        int remove_status_code = system(system_command_buffer);
        if (remove_status_code != 0)
        {
            // Log warning if cleanup failed, but don't report to client
            fprintf(stderr, "WARN (PID %d) TarDownload: system('%s') failed with status %d during cleanup.\n", getpid(), system_command_buffer, remove_status_code);
        }
        else
        {
            printf("DEBUG (PID %d) TarDownload: Temporary directory removed successfully.\n", getpid());
        }
    }
    else
    {
        printf("DEBUG (PID %d) TarDownload: No temporary directory cleanup needed.\n", getpid());
    }
    // Just to be safe, if file stream was opened but not closed due to mid-send error
    if (tar_file_stream_ptr != NULL)
    {
        fclose(tar_file_stream_ptr);
        tar_file_stream_ptr = NULL;
    }

    printf("DEBUG (PID %d) TarDownload: --- Exiting handle_downltar_command ---\n", getpid());
    return; // Return to process_client loop

} // End of handle_downltar_command

/* ========================================================================= */
/*             handle_dispfnames_command Function                            */
/* ========================================================================= */
/**
 * @brief Handles the 'dispfnames' command from the client.
 * My goal is to get a list of filenames (.c, .pdf, .txt, .zip) under a specific
 * directory path (~S1/...), combine them, sort them by type then alphabetically,
 * and send the final list back to the client before closing the connection.
 *
 * Steps:
 * 1. Parse command ("dispfnames ~S1/...").
 * 2. Validate path starts with ~S1/.
 * 3. Get server's home directory.
 * 4. Create a temporary file path string (use /tmp if possible).
 * 5. Get local .c filenames using `find` and append sorted results to the temp file.
 * 6. Loop through S2, S3, S4:
 *    a. Call `get_filenames_from_secondary_server` for the path.
 *    b. Append the received (already sorted) list of .pdf/.txt/.zip to the temp file.
 * 7. Read the combined temp file.
 * 8. Build the final output string in the required order (.c, then .pdf, then .txt, then .zip).
 * 9. Send the final formatted string to the client.
 * 10. **CLOSE THE CLIENT CONNECTION**.
 * 11. Remove the temporary file.
 *
 * @param client_socket_fd The socket descriptor for the connected client.
 * @param command_line The raw command string (e.g., "dispfnames ~S1/").
 */
void handle_dispfnames_command(int client_socket_fd, char *command_line)
{
    char client_pathname_arg[256];                 // Path arg from client (~S1/...)
    char server_home_directory[512];               // Server's HOME path
    char temporary_storage_filepath[256];          // Path for temp file (e.g., /tmp/s1_files_...)
    char absolute_search_path_s1[512];             // Absolute path for find command (~/S1/...)
    char sys_cmd_buffer[1024];                     // Buffer for system() commands
    char secondary_server_response_area[16384];    // Buffer for S2/S3/S4 file lists (16KB)
    char final_list_to_client[MAX_FILE_LIST_SIZE]; // Final combined & ordered list (Matches client buffer size)
    const char *error_to_client_ptr = NULL;        // Pointer to error message if failure
    char error_msg_storage[200];                   // Buffer for generated error messages
    int temp_file_created_flag = 0;                // Track if temp file needs cleanup
    FILE *temp_file_pointer = NULL;                // File stream for temp file
    size_t output_buffer_current_length = 0;       // Track length of final_list_to_client

    printf("DEBUG (PID %d) ListFiles: --- Entering handle_dispfnames_command ---\n", getpid());
    printf("DEBUG (PID %d) ListFiles: Received raw command: '%s'\n", getpid(), command_line);

    // Ensure final buffer is initially empty
    memset(final_list_to_client, 0, sizeof(final_list_to_client));
    temporary_storage_filepath[0] = '\0'; // Initialize temp path

    /* 1. Parse command */
    printf("DEBUG (PID %d) ListFiles: Parsing command...\n", getpid());
    if (sscanf(command_line, "dispfnames %255s", client_pathname_arg) != 1)
    {
        snprintf(error_msg_storage, sizeof(error_msg_storage), "ERROR: Invalid dispfnames command format. Use: dispfnames <~S1/path>");
        error_to_client_ptr = error_msg_storage;
        printf("ERROR (PID %d) ListFiles: Command parsing failed.\n", getpid());
        goto send_result_and_close; // Error path
    }
    printf("DEBUG (PID %d) ListFiles: Parsed pathname: '%s'\n", getpid(), client_pathname_arg);

    /* 2. Validate path */
    printf("DEBUG (PID %d) ListFiles: Validating path prefix...\n", getpid());
    if (strncmp(client_pathname_arg, "~S1/", 4) != 0)
    {
        snprintf(error_msg_storage, sizeof(error_msg_storage), "ERROR: Path must start with ~S1/");
        error_to_client_ptr = error_msg_storage;
        printf("ERROR (PID %d) ListFiles: Invalid path prefix '%s'.\n", getpid(), client_pathname_arg);
        goto send_result_and_close; // Error path
    }
    printf("DEBUG (PID %d) ListFiles: Path prefix OK.\n", getpid());

    /* 3. Get home directory */
    printf("DEBUG (PID %d) ListFiles: Getting server HOME directory...\n", getpid());
    char *home_env = getenv("HOME");
    if (home_env == NULL) // Check NULL
    {
        snprintf(error_msg_storage, sizeof(error_msg_storage), "ERROR: Server configuration error (cannot find HOME)");
        error_to_client_ptr = error_msg_storage;
        perror("ERROR (PID %d) ListFiles: getenv(\"HOME\") failed");
        goto send_result_and_close; // Error path
    }
    // Copy to our buffer
    strncpy(server_home_directory, home_env, sizeof(server_home_directory) - 1);
    server_home_directory[sizeof(server_home_directory) - 1] = '\0';
    printf("DEBUG (PID %d) ListFiles: Server HOME: '%s'\n", getpid(), server_home_directory);

    /* 4. Temporary file path */
    // Using /tmp is generally better if available and permissions allow.
    snprintf(temporary_storage_filepath, sizeof(temporary_storage_filepath), "/tmp/s1_files_%d.txt", getpid());
    printf("DEBUG (PID %d) ListFiles: Using temp file: '%s'\n", getpid(), temporary_storage_filepath);

    // Ensure temp file is clean before starting (remove() returns 0 on success, -1 on error)
    if (remove(temporary_storage_filepath) == 0)
    {
        printf("DEBUG (PID %d) ListFiles: Removed existing temp file.\n", getpid());
    }
    else
    {
        if (errno != ENOENT)
        { // ENOENT (No such file) is expected, ignore that error.
            perror("WARN (PID %d) ListFiles: Pre-cleanup remove() failed for temp file");
        }
    }
    temp_file_created_flag = 1; // Assume we will create it, so cleanup is needed

    /* 5. Get .c files from S1 */
    // Construct the absolute path for find command (~/S1/...)
    snprintf(absolute_search_path_s1, sizeof(absolute_search_path_s1), "%s/S1/%s", server_home_directory, client_pathname_arg + 4); // Skip ~S1/
                                                                                                                                    // Remove trailing slash if present for 'find' consistency
    size_t s1_path_len = strlen(absolute_search_path_s1);
    if (s1_path_len > 1 && absolute_search_path_s1[s1_path_len - 1] == '/')
    {
        absolute_search_path_s1[s1_path_len - 1] = '\0';
    }
    printf("DEBUG (PID %d) ListFiles: Looking for .c files in '%s'\n", getpid(), absolute_search_path_s1);

    // Use find to get sorted list and append to temp_file.
    // Only want filenames, use -printf "%f\\n". Sort pipe handles ordering.
    // Use '>>' to append. 2>/dev/null suppresses find errors (like path not found).
    snprintf(sys_cmd_buffer, sizeof(sys_cmd_buffer),
             "find \"%s\" -maxdepth 1 -type f -name \"*.c\" -printf \"%%f\\n\" 2>/dev/null | sort >> \"%s\"",
             absolute_search_path_s1, temporary_storage_filepath);
    printf("DEBUG (PID %d) ListFiles: Executing C file find/sort: %s\n", getpid(), sys_cmd_buffer);
    int find_c_status = system(sys_cmd_buffer);
    // system() status isn't super reliable for pipelines, check file later.
    if (find_c_status != 0)
    {
        fprintf(stderr, "WARN (PID %d) ListFiles: Find/sort command for .c files returned status %d.\n", getpid(), find_c_status);
    }
    else
    {
        printf("DEBUG (PID %d) ListFiles: Find/sort for .c files executed (status 0).\n", getpid());
    }

    /* 6. Get files from secondary servers (PDF, TXT, ZIP) */
    // Loop from server 2 to 4
    int server_index; // Use for loop
    for (server_index = 2; server_index <= 4; ++server_index)
    {
        char file_extension_type[10] = ""; // Determine expected extension
        switch (server_index)
        {
        case 2:
            strcpy(file_extension_type, ".pdf");
            break;
        case 3:
            strcpy(file_extension_type, ".txt");
            break;
        case 4:
            strcpy(file_extension_type, ".zip");
            break;
        default:
            continue; // Should not happen
        }

        printf("DEBUG (PID %d) ListFiles: Querying S%d for %s files in '%s'\n", getpid(), server_index, file_extension_type, client_pathname_arg);
        memset(secondary_server_response_area, 0, sizeof(secondary_server_response_area)); // Clear buffer

        // Call helper function. Returns 1 on success, 0 on error/no files.
        if (get_filenames_from_secondary_server(server_index, client_pathname_arg, secondary_server_response_area, sizeof(secondary_server_response_area)))
        {
            // Check if any data was actually received (buffer not empty)
            if (strlen(secondary_server_response_area) > 0)
            {
                printf("DEBUG (PID %d) ListFiles: Received from S%d:\n---\n%s---\n", getpid(), server_index, secondary_server_response_area);
                // Append the secondary server files (assumed sorted, newline-separated filenames)
                temp_file_pointer = fopen(temporary_storage_filepath, "a"); // Open in append mode ("a")
                if (temp_file_pointer != NULL)                              // Check if fopen succeeded
                {
                    // Write the received list
                    size_t written_count = fwrite(secondary_server_response_area, 1, strlen(secondary_server_response_area), temp_file_pointer);
                    if (written_count < strlen(secondary_server_response_area))
                    {
                        perror("WARN (PID %d) ListFiles: Incomplete write to temp file");
                        fprintf(stderr, "WARN (PID %d) ListFiles: Failed writing S%d list to temp file.\n", getpid(), server_index);
                    }
                    // Ensure there's a newline after the last file from secondary server IF data was written
                    if (written_count > 0 && secondary_server_response_area[strlen(secondary_server_response_area) - 1] != '\n')
                    {
                        fputc('\n', temp_file_pointer);
                    }
                    fclose(temp_file_pointer);
                    temp_file_pointer = NULL; // Mark as closed
                }
                else
                {
                    perror("ERROR (PID %d) ListFiles: Failed to open temp file for appending");
                    // Continue trying other servers? Yes, let's try to get as much as possible.
                }
            }
            else
            {
                printf("DEBUG (PID %d) ListFiles: Received EMPTY list from S%d.\n", getpid(), server_index);
            }
        }
        else
        {
            printf("DEBUG (PID %d) ListFiles: Error or connection issue querying S%d.\n", getpid(), server_index);
            // Maybe set a flag indicating partial results? For now, just continue.
        }
    } // End loop through secondary servers

    /* 7. Read the combined temp file and create the final ordered list */
    // Open the temp file for reading
    printf("DEBUG (PID %d) ListFiles: Reading combined list from '%s' to format output.\n", getpid(), temporary_storage_filepath);
    temp_file_pointer = fopen(temporary_storage_filepath, "r");

    // Check if temp file exists/is readable (it might be empty if no files found anywhere)
    if (temp_file_pointer == NULL) // Use == NULL
    {
        // This happens if find failed AND all secondary requests failed/returned empty.
        printf("DEBUG (PID %d) ListFiles: Temp file '%s' not found or unreadable. Assuming no files found.\n", getpid(), temporary_storage_filepath);
        // Send empty list (empty string) - client should handle this.
        // final_list_to_client is already zeroed out.
        output_buffer_current_length = 0;
        // Proceed to send result and close
        goto send_result_and_close;
    }

    // Read temp file line by line and append to final buffer in correct order
    char line_buffer[512]; // Buffer for reading lines

    // --- Append files in order: .c, .pdf, .txt, .zip ---
    const char *extensions_order[] = {".c", ".pdf", ".txt", ".zip", NULL}; // Order to process
    for (int ext_idx = 0; extensions_order[ext_idx] != NULL; ++ext_idx)
    {
        const char *current_extension = extensions_order[ext_idx];
        size_t current_ext_len = strlen(current_extension);
        printf("DEBUG (PID %d) ListFiles: Adding '%s' files to final buffer...\n", getpid(), current_extension);

        rewind(temp_file_pointer); // Go back to start of temp file for each extension type
        while (fgets(line_buffer, sizeof(line_buffer), temp_file_pointer))
        {
            size_t line_len = strlen(line_buffer);
            // Remove trailing newline if present for check
            if (line_len > 0 && line_buffer[line_len - 1] == '\n')
            {
                line_buffer[line_len - 1] = '\0';
                line_len--; // Adjust length
            }
            // Check if line is long enough and ends with the current extension
            if (line_len >= current_ext_len && strcmp(line_buffer + line_len - current_ext_len, current_extension) == 0)
            {
                // Add back the newline for the final output format
                line_buffer[line_len] = '\n';
                line_buffer[line_len + 1] = '\0';
                line_len++; // Adjust length back

                // Check buffer space before appending
                if (output_buffer_current_length + line_len < sizeof(final_list_to_client))
                {
                    strcat(final_list_to_client, line_buffer); // Append the line
                    output_buffer_current_length += line_len;
                }
                else
                {
                    fprintf(stderr, "WARN (PID %d) ListFiles: Output buffer full while adding %s files.\n", getpid(), current_extension);
                    goto buffer_full_actions; // Jump out if buffer full
                }
            }
        } // End while loop for current extension
    } // End for loop iterating through extensions

buffer_full_actions: // Label for jump if buffer got full during processing
    // Ensure buffer is null-terminated (already handled by memset and strcat).
    final_list_to_client[sizeof(final_list_to_client) - 1] = '\0';

    // Close the temp file
    fclose(temp_file_pointer);
    temp_file_pointer = NULL; // Mark as closed

    printf("DEBUG (PID %d) ListFiles: Final formatted list prepared (length %zu).\n", getpid(), output_buffer_current_length);

send_result_and_close:
    /* 8. Send the final list or error to the client */
    if (error_to_client_ptr != NULL)
    {
        // Send error message
        printf("DEBUG (PID %d) ListFiles: Sending error message to client: '%s'\n", getpid(), error_to_client_ptr);
        ssize_t write_err_chk = write(client_socket_fd, error_to_client_ptr, strlen(error_to_client_ptr));
        if (write_err_chk < 0)
        {
            perror("ERROR (PID %d) ListFiles: Failed to write error message to client");
        }
        printf("DEBUG (PID %d) ListFiles: Error message sent.\n", getpid());
    }
    else
    {
        // Send success list (might be empty if no files found)
        printf("DEBUG (PID %d) ListFiles: Sending final list (len %zu) to client...\n", getpid(), output_buffer_current_length);
        ssize_t bytes_sent_list = write(client_socket_fd, final_list_to_client, output_buffer_current_length);
        if (bytes_sent_list < 0)
        {
            perror("ERROR (PID %d) ListFiles: Failed to write final list to client");
        }
        else if ((size_t)bytes_sent_list < output_buffer_current_length)
        {
            fprintf(stderr, "ERROR (PID %d) ListFiles: Incomplete write of final list (%zd/%zu bytes)\n", getpid(), bytes_sent_list, output_buffer_current_length);
        }
        else
        {
            printf("DEBUG (PID %d) ListFiles: Successfully sent %zd bytes to client.\n", getpid(), bytes_sent_list);
        }
    }

    /* 9. *** CLOSE THE CONNECTION *** */
    // Required by the project description for dispfnames.
    printf("DEBUG (PID %d) ListFiles: Closing connection to client (fd=%d).\n", getpid(), client_socket_fd);
    close(client_socket_fd); // Close connection

    /* 10. Cleanup */
    // Ensure temp file pointer is closed if jump occurred before fclose
    if (temp_file_pointer != NULL)
    {
        fclose(temp_file_pointer);
        temp_file_pointer = NULL;
    }
    // Remove the temporary file if its path was generated
    if (temp_file_created_flag == 1 && temporary_storage_filepath[0] != '\0')
    {
        printf("DEBUG (PID %d) ListFiles: Cleaning up temp file '%s'.\n", getpid(), temporary_storage_filepath);
        if (remove(temporary_storage_filepath) != 0)
        {
            if (errno != ENOENT)
            { // Ignore "No such file" error
                perror("WARN (PID %d) ListFiles: Failed to remove temp file during cleanup");
            }
        }
    }

    printf("DEBUG (PID %d) ListFiles: --- Exiting handle_dispfnames_command ---\n", getpid());
    // Function ends, returning to process_client loop (which will likely find socket closed on next read)

} // End of handle_dispfnames_command

/* ============================================================================ */
/*             get_filenames_from_secondary_server Function                     */
/* ============================================================================ */
/**
 * @brief Connects to a secondary server (S2, S3, or S4) and requests a list of filenames.
 * Assumes the secondary server will send back a newline-separated list of BASENAMES
 * for the given path and then close the connection.
 * Reads the entire response into the provided buffer.
 *
 * Protocol:
 * 1. Connect to secondary server.
 * 2. Send "GETFILES ~S1/path/..." command.
 * 3. Read data from the socket into output_buffer until the server closes the connection (read returns 0).
 * 4. Close socket.
 * 5. Return 1 on success (even if list is empty), 0 on connection or read error.
 *
 * @param target_server_id The server number (2, 3, or 4).
 * @param client_provided_pathname The path argument from the client's command (e.g., ~S1/folder/).
 * @param destination_buffer Buffer to store the received list.
 * @param buffer_capacity Size of the destination_buffer.
 * @return 1 if the list was retrieved successfully (no socket/read errors), 0 otherwise.
 */
int get_filenames_from_secondary_server(int target_server_id, const char *client_provided_pathname, char *destination_buffer, size_t buffer_capacity)
{
    int comm_socket_fd = -1;                  // Socket fd
    struct sockaddr_in target_server_address; // Server address info
    ssize_t total_bytes_accumulated = 0;      // Track total bytes read
    int operation_successful_flag = 0;        // 0 = fail, 1 = success
    int connection_attempted_flag = 0;        // Redundant flag

    printf("DEBUG (PID %d) GetFNames: Connecting to S%d for path '%s'\n", getpid(), target_server_id, client_provided_pathname);

    // Ensure output buffer is initially clear and has capacity
    if (buffer_capacity > 0)
    {
        memset(destination_buffer, 0, buffer_capacity);
    }
    else
    {
        fprintf(stderr, "ERROR (PID %d) GetFNames: Zero buffer capacity provided!\n", getpid());
        return 0; // Cannot proceed
    }

    /* Create socket */
    printf("DEBUG (PID %d) GetFNames: Creating socket...\n", getpid());
    comm_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (comm_socket_fd < 0)
    {
        perror("ERROR (PID %d) GetFNames: Socket creation failed");
        goto cleanup_getfnames;
    }
    connection_attempted_flag = 1; // Mark that we have a socket

    /* Set up server address */
    printf("DEBUG (PID %d) GetFNames: Setting up address for S%d...\n", getpid(), target_server_id);
    memset(&target_server_address, 0, sizeof(target_server_address));
    target_server_address.sin_family = AF_INET;                      // IPv4
    target_server_address.sin_port = htons(8000 + target_server_id); // Port 8002/3/4
    // Use localhost
    if (inet_pton(AF_INET, "127.0.0.1", &target_server_address.sin_addr) <= 0) // Check <= 0
    {
        perror("ERROR (PID %d) GetFNames: inet_pton failed");
        goto cleanup_getfnames;
    }

    /* Connect to server */
    printf("DEBUG (PID %d) GetFNames: Connecting to S%d...\n", getpid(), target_server_id);
    if (connect(comm_socket_fd, (struct sockaddr *)&target_server_address, sizeof(target_server_address)) < 0) // Check < 0
    {
        char error_buffer_connect[100];
        snprintf(error_buffer_connect, sizeof(error_buffer_connect), "ERROR (PID %d) GetFNames: connect() to S%d failed", getpid(), target_server_id);
        perror(error_buffer_connect);
        goto cleanup_getfnames;
    }
    printf("DEBUG (PID %d) GetFNames: Connected to S%d.\n", getpid(), target_server_id);

    /* Send GETFILES command */
    char getfiles_command_str[1024];
    snprintf(getfiles_command_str, sizeof(getfiles_command_str), "GETFILES %s", client_provided_pathname);
    printf("DEBUG (PID %d) GetFNames: Sending command to S%d: '%s'\n", getpid(), target_server_id, getfiles_command_str);
    ssize_t write_check_getf = write(comm_socket_fd, getfiles_command_str, strlen(getfiles_command_str));
    // Check write result
    if (write_check_getf < 0 || (size_t)write_check_getf != strlen(getfiles_command_str))
    {
        perror("ERROR (PID %d) GetFNames: Failed to send GETFILES command");
        goto cleanup_getfnames;
    }
    printf("DEBUG (PID %d) GetFNames: Command sent.\n", getpid());

    /* Read response in a loop until connection is closed by secondary server */
    printf("DEBUG (PID %d) GetFNames: Waiting for response from S%d (read until EOF)...\n", getpid(), target_server_id);

    // Loop to read data chunks
    // Using 'while' loop structure here
    total_bytes_accumulated = 0;
    while (total_bytes_accumulated < (ssize_t)(buffer_capacity - 1))
    {
        // Calculate space remaining in buffer
        size_t space_left_in_buffer = buffer_capacity - 1 - total_bytes_accumulated;
        // Read next chunk
        ssize_t bytes_read_in_this_chunk = read(comm_socket_fd,
                                                destination_buffer + total_bytes_accumulated,
                                                space_left_in_buffer);

        if (bytes_read_in_this_chunk < 0)
        { // Read Error
            if (errno == EINTR)
            {
                printf("DEBUG (PID %d) GetFNames: read interrupted, continuing.\n", getpid());
                continue;
            } // Handle interruption
            perror("ERROR (PID %d) GetFNames: read() failed from secondary server");
            total_bytes_accumulated = -1; // Mark error state
            break;                        // Exit read loop
        }
        if (bytes_read_in_this_chunk == 0)
        { // Connection closed by server (End Of File)
            printf("DEBUG (PID %d) GetFNames: S%d closed connection (EOF received).\n", getpid(), target_server_id);
            break; // Exit read loop, successfully read all data
        }

        // If read succeeded, add to total
        total_bytes_accumulated += bytes_read_in_this_chunk;
        printf("DEBUG (PID %d) GetFNames: Read %zd bytes from S%d (total %zd)\n", getpid(), bytes_read_in_this_chunk, target_server_id, total_bytes_accumulated);

        // Check if buffer is now full (safety check)
        if (total_bytes_accumulated >= (ssize_t)(buffer_capacity - 1))
        {
            fprintf(stderr, "WARN (PID %d) GetFNames: Response buffer from S%d is full (%zu bytes). Truncating data.\n", getpid(), target_server_id, buffer_capacity);
            break; // Exit loop, data will be truncated
        }
    } // End of read loop

    // Check if loop exited due to read error
    if (total_bytes_accumulated < 0)
    {
        operation_successful_flag = 0; // Failed due to read error
    }
    else
    {
        // If no read error, operation is considered successful (even if list is empty)
        operation_successful_flag = 1;
        // Ensure null termination of the received buffer (even if buffer full, overwrite last char)
        destination_buffer[total_bytes_accumulated < (ssize_t)(buffer_capacity - 1) ? total_bytes_accumulated : buffer_capacity - 1] = '\0';
        printf("DEBUG (PID %d) GetFNames: Total received from S%d: %zd bytes.\n", getpid(), target_server_id, total_bytes_accumulated < 0 ? 0 : total_bytes_accumulated);
    }

cleanup_getfnames:
    // Close socket if it was opened
    if (comm_socket_fd >= 0)
    {
        printf("DEBUG (PID %d) GetFNames: Closing socket fd %d.\n", getpid(), comm_socket_fd);
        close(comm_socket_fd);
        comm_socket_fd = -1; // Mark as closed
    }

    printf("DEBUG (PID %d) GetFNames: --- Exiting get_filenames_from_secondary_server (Success Flag: %d) ---\n", getpid(), operation_successful_flag);
    return operation_successful_flag; // Return 1 if success, 0 if error

} // End of get_filenames_from_secondary_server

/* ============================================================================ */
/*             transfer_to_secondary_server Function                            */
/* ============================================================================ */
/**
 * @brief My job here is to connect to the other servers (S2, S3, or S4) and send a file to them.
 * This happens when the client uploads a file that S1 doesn't keep (like pdf, txt, zip).
 *
 * Protocol:
 * 1. Connect to the right secondary server (S2 for pdf, S3 for txt, S4 for zip).
 * 2. Send a command like: "STORE actual_filename.ext ~S1/original/path/given/by/client.ext file_size_in_bytes"
 * 3. Wait for the secondary server to send back "READY".
 * 4. If READY received, open the file locally (the one S1 just received) and send its contents.
 * 5. Wait for the secondary server to send back "SUCCESS" or "ERROR".
 * 6. Return 1 if it was SUCCESS, 0 otherwise.
 *
 * @param target_server_id The secondary server number (2, 3, or 4).
 * @param local_s1_source_filepath The absolute path of the file on *this* S1 server that needs to be sent. (e.g., /home/user/S1/folder/file.pdf)
 * @param original_client_dest_path The original destination path string the client sent (e.g., ~S1/folder/file.pdf). The secondary server needs this!
 * @param actual_file_size The exact size of the file being transferred (uint64_t).
 * @return 1 if the transfer was successful (secondary server sent SUCCESS), 0 otherwise.
 */
int transfer_to_secondary_server(int target_server_id, const char *local_s1_source_filepath, const char *original_client_dest_path, uint64_t actual_file_size)
{
    printf("DEBUG (PID %d) Transfer: --- Starting transfer_to_secondary_server ---\n", getpid());
    printf("DEBUG (PID %d) Transfer: Target=S%d, LocalSrc='%s', ClientDest='%s', Size=%llu\n",
           getpid(), target_server_id, local_s1_source_filepath, original_client_dest_path, (unsigned long long)actual_file_size);

    int socket_to_secondary = -1;    // Socket file descriptor for talking to S2/S3/S4
    FILE *source_file_handle = NULL; // File pointer to read the local S1 file
    int transfer_outcome = 0;        // 0 means failure (default), 1 means success
    int connection_stage = 0;        // Redundant variable to track progress maybe?
    char store_command_buffer[1024]; // Buffer for the command string
    char server_reply_buffer[256];   // Buffer for the server's response

    /* Stage 1: Create a socket to connect out */
    // ------------------------------------------
    connection_stage = 1;
    printf("DEBUG (PID %d) Transfer [Stage %d]: Creating socket to connect to S%d...\n", getpid(), connection_stage, target_server_id);
    socket_to_secondary = socket(AF_INET, SOCK_STREAM, 0);
    // Check if socket creation failed (returns value less than 0)
    if (0 > socket_to_secondary)
    {
        perror("FATAL ERROR (PID %d) Transfer: Socket creation failed");
        goto cleanup_transfer; // Use goto for cleanup
    }
    printf("DEBUG (PID %d) Transfer [Stage %d]: Socket created OK (fd=%d).\n", getpid(), connection_stage, socket_to_secondary);

    /* Stage 2: Figure out the secondary server's address */
    // ----------------------------------------------------
    connection_stage = 2;
    struct sockaddr_in secondary_server_details; // Structure to hold address info
    printf("DEBUG (PID %d) Transfer [Stage %d]: Setting up address for S%d...\n", getpid(), connection_stage, target_server_id);
    // Clear the structure first
    memset(&secondary_server_details, 0, sizeof(secondary_server_details));
    secondary_server_details.sin_family = AF_INET; // IPv4
    // Port convention: S2=8002, S3=8003, S4=8004
    int secondary_port = 8000 + target_server_id;
    secondary_server_details.sin_port = htons(secondary_port); // Convert port to network byte order

    // Assuming secondary servers are running on the same machine (localhost)
    const char *secondary_ip = "127.0.0.1";
    printf("DEBUG (PID %d) Transfer [Stage %d]: Target IP: %s, Port: %d\n", getpid(), connection_stage, secondary_ip, secondary_port);
    // Convert the IP address string to binary format
    int pton_result = inet_pton(AF_INET, secondary_ip, &secondary_server_details.sin_addr);
    // Check if inet_pton failed (returns 0 or -1 on error)
    if (1 != pton_result) // Different check: 1 != result
    {
        perror("FATAL ERROR (PID %d) Transfer: inet_pton failed for localhost IP");
        goto cleanup_transfer; // Use goto for cleanup
    }
    printf("DEBUG (PID %d) Transfer [Stage %d]: Address setup OK.\n", getpid(), connection_stage);

    /* Stage 3: Connect to the secondary server */
    // ------------------------------------------
    connection_stage = 3;
    printf("DEBUG (PID %d) Transfer [Stage %d]: Attempting connection to S%d (%s:%d)...\n", getpid(), connection_stage, target_server_id, secondary_ip, secondary_port);
    // Try to establish the TCP connection
    if (connect(socket_to_secondary, (struct sockaddr *)&secondary_server_details, sizeof(secondary_server_details)) < 0)
    {
        // If connect fails, print a specific error message
        char error_message_buffer[128];
        snprintf(error_message_buffer, sizeof(error_message_buffer), "ERROR (PID %d) Transfer: connect() call to S%d failed", getpid(), target_server_id);
        perror(error_message_buffer); // Print system error details
        goto cleanup_transfer;        // Use goto for cleanup
    }
    printf("DEBUG (PID %d) Transfer [Stage %d]: Connected successfully to S%d.\n", getpid(), connection_stage, target_server_id);

    /* Stage 4: Send the STORE command */
    // ---------------------------------
    connection_stage = 4;
    // We need the *filename* part of the local S1 path. basename() is good for this.
    // Need a mutable copy for basename potentially, or just cast carefully.
    char *local_filename_base;
    char local_path_copy[512]; // Create a mutable copy
    strncpy(local_path_copy, local_s1_source_filepath, sizeof(local_path_copy) - 1);
    local_path_copy[sizeof(local_path_copy) - 1] = '\0';
    local_filename_base = basename(local_path_copy);

    // Construct the command string: "STORE <filename> <original_client_dest_path> <size>"
    // Use %llu for uint64_t size
    snprintf(store_command_buffer, sizeof(store_command_buffer), "STORE %s %s %llu",
             local_filename_base, original_client_dest_path, (unsigned long long)actual_file_size);

    printf("DEBUG (PID %d) Transfer [Stage %d]: Sending command to S%d: '%s'\n", getpid(), connection_stage, target_server_id, store_command_buffer);
    // Send the command string over the socket
    ssize_t write_check_cmd = write(socket_to_secondary, store_command_buffer, strlen(store_command_buffer));
    // Check if write failed
    if (write_check_cmd < (ssize_t)strlen(store_command_buffer))
    {
        perror("ERROR (PID %d) Transfer: Failed to write STORE command to secondary server");
        goto cleanup_transfer; // Use goto for cleanup
    }
    printf("DEBUG (PID %d) Transfer [Stage %d]: Command sent successfully (%zd bytes).\n", getpid(), connection_stage, write_check_cmd);

    /* Stage 5: Wait for the "READY" signal */
    // --------------------------------------
    connection_stage = 5;
    printf("DEBUG (PID %d) Transfer [Stage %d]: Waiting for 'READY' response from S%d...\n", getpid(), connection_stage, target_server_id);
    // Clear the buffer before reading
    memset(server_reply_buffer, 0, sizeof(server_reply_buffer));
    // Try to read the response from the secondary server
    ssize_t bytes_read_reply1 = read(socket_to_secondary, server_reply_buffer, sizeof(server_reply_buffer) - 1); // Leave space for null terminator

    // Check read result
    if (bytes_read_reply1 < 0)
    { // Read error
        perror("ERROR (PID %d) Transfer: Failed to read READY response from secondary server");
        goto cleanup_transfer;
    }
    if (bytes_read_reply1 == 0)
    { // Server disconnected
        fprintf(stderr, "ERROR (PID %d) Transfer: Secondary server S%d closed connection unexpectedly after command sent.\n", getpid(), target_server_id);
        goto cleanup_transfer;
    }

    // Null-terminate the received string
    server_reply_buffer[bytes_read_reply1] = '\0';
    printf("DEBUG (PID %d) Transfer [Stage %d]: Received response from S%d: '%s'\n", getpid(), connection_stage, target_server_id, server_reply_buffer);

    // Check if the response is exactly "READY"
    if (strcmp(server_reply_buffer, "READY") != 0) // Using strcmp here
    {
        // If it's not READY, something is wrong with the secondary server or protocol.
        fprintf(stderr, "ERROR (PID %d) Transfer: Secondary server S%d did not send READY (sent: '%s'). Aborting transfer.\n", getpid(), target_server_id, server_reply_buffer);
        goto cleanup_transfer;
    }
    // If we get here, the server is ready!
    printf("DEBUG (PID %d) Transfer [Stage %d]: Received READY. Proceeding to send file content.\n", getpid(), connection_stage);

    /* Stage 6: Send the actual file content */
    // ---------------------------------------
    connection_stage = 6;
    // Open the local S1 source file in binary read mode ("rb")
    printf("DEBUG (PID %d) Transfer [Stage %d]: Opening local source file '%s' for reading...\n", getpid(), connection_stage, local_s1_source_filepath);
    source_file_handle = fopen(local_s1_source_filepath, "rb");
    // Check if fopen failed
    if (source_file_handle == NULL) // Using == NULL
    {
        // If we can't open the source file S1 has, we can't send it.
        char error_message_buffer[600];
        snprintf(error_message_buffer, sizeof(error_message_buffer), "ERROR (PID %d) Transfer: Failed to open source file '%s' for reading", getpid(), local_s1_source_filepath);
        perror(error_message_buffer);
        goto cleanup_transfer;
    }
    printf("DEBUG (PID %d) Transfer [Stage %d]: Source file opened successfully.\n", getpid(), connection_stage);

    // Prepare buffer for reading/writing file chunks
    char file_chunk_buffer[4096];          // 4KB chunk size
    size_t bytes_read_from_local_file;     // How many bytes fread() got
    size_t total_bytes_sent_to_remote = 0; // Counter for sent bytes
    int loop_iteration_counter = 0;        // Redundant counter

    printf("DEBUG (PID %d) Transfer [Stage %d]: Starting loop to send %llu bytes to S%d...\n", getpid(), connection_stage, (unsigned long long)actual_file_size, target_server_id);

    // Loop while fread() successfully reads data from the file
    while ((bytes_read_from_local_file = fread(file_chunk_buffer, 1, sizeof(file_chunk_buffer), source_file_handle)) > 0)
    {
        loop_iteration_counter++; // Increment redundant counter

        // Try to write the chunk we just read to the network socket
        ssize_t bytes_written_to_network = write(socket_to_secondary, file_chunk_buffer, bytes_read_from_local_file);

        // Check for write errors
        if (bytes_written_to_network < 0 || (size_t)bytes_written_to_network != bytes_read_from_local_file)
        {
            perror("ERROR (PID %d) Transfer: write() failed while sending file content");
            goto cleanup_transfer;
        }
        // Add to the total count of bytes sent
        total_bytes_sent_to_remote += bytes_written_to_network;
    } // End of while loop (reading from file)

    // After the loop, check if fread() failed due to an error (not just EOF)
    if (ferror(source_file_handle))
    {
        perror("ERROR (PID %d) Transfer: fread() failed while reading source file");
        goto cleanup_transfer;
    }

    // We are done reading the local file, close it. - Handled in cleanup
    // printf("DEBUG (PID %d) Transfer [Stage %d]: Finished reading local file. Closing handle.\n", getpid(), connection_stage);
    // fclose(source_file_handle);
    // source_file_handle = NULL; // Mark as closed
    printf("DEBUG (PID %d) Transfer [Stage %d]: Finished sending file content. Total sent: %zu bytes (in %d chunks).\n", getpid(), connection_stage, total_bytes_sent_to_remote, loop_iteration_counter);

    // Optional check: Did we send the expected number of bytes?
    if (total_bytes_sent_to_remote != actual_file_size)
    {
        fprintf(stderr, "WARNING (PID %d) Transfer: Sent %zu bytes, but expected %llu! Proceeding anyway.\n", getpid(), total_bytes_sent_to_remote, (unsigned long long)actual_file_size);
    }

    /* Stage 7: Wait for the final response (SUCCESS/ERROR) */
    // ------------------------------------------------------
    connection_stage = 7;
    printf("DEBUG (PID %d) Transfer [Stage %d]: Waiting for final SUCCESS/ERROR response from S%d...\n", getpid(), connection_stage, target_server_id);
    // Reuse the reply buffer, clear it first
    memset(server_reply_buffer, 0, sizeof(server_reply_buffer));
    // Read the final status message
    ssize_t bytes_read_reply2 = read(socket_to_secondary, server_reply_buffer, sizeof(server_reply_buffer) - 1);

    // Check read result
    if (bytes_read_reply2 < 0)
    { // Read error
        perror("ERROR (PID %d) Transfer: Failed to read final SUCCESS/ERROR response from secondary server");
        goto cleanup_transfer;
    }
    if (bytes_read_reply2 == 0)
    { // Server disconnected
        fprintf(stderr, "ERROR (PID %d) Transfer: Secondary server S%d closed connection before sending final status.\n", getpid(), target_server_id);
        goto cleanup_transfer;
    }

    // Null-terminate the response
    server_reply_buffer[bytes_read_reply2] = '\0';
    printf("DEBUG (PID %d) Transfer [Stage %d]: Final response received from S%d: '%s'\n", getpid(), connection_stage, target_server_id, server_reply_buffer);

    /* Stage 8: Determine return value based on response */
    // ---------------------------------------------
    connection_stage = 8;
    // Check if the final response starts with "SUCCESS" (use strncmp for safety)
    if (strncmp(server_reply_buffer, "SUCCESS", 7) == 0)
    {
        printf("DEBUG (PID %d) Transfer [Stage %d]: Transfer deemed SUCCESSFUL.\n", getpid(), connection_stage);
        transfer_outcome = 1; // Set outcome to success
    }
    else
    {
        printf("DEBUG (PID %d) Transfer [Stage %d]: Transfer deemed FAILED (final response was not SUCCESS).\n", getpid(), connection_stage);
        transfer_outcome = 0; // Ensure outcome is failure
    }

cleanup_transfer:
    // Cleanup resources: close file handle and socket descriptor
    printf("DEBUG (PID %d) Transfer: Cleaning up resources...\n", getpid());
    if (source_file_handle != NULL)
    {
        fclose(source_file_handle);
        source_file_handle = NULL;
    }
    if (socket_to_secondary >= 0)
    {
        close(socket_to_secondary);
        socket_to_secondary = -1;
    }

    printf("DEBUG (PID %d) Transfer: --- Exiting transfer_to_secondary_server (returning %d) ---\n", getpid(), transfer_outcome);
    return transfer_outcome; // Return 1 for success, 0 for failure

} // End of transfer_to_secondary_server

/* ============================================================================ */
/*             retrieve_from_secondary_server Function                          */
/* ============================================================================ */
/**
 * @brief My job is to talk to a secondary server (S2, S3, or S4) and get a file from it.
 * This is called by handle_download_command when the user requests a non-.c file.
 *
 * Protocol (Updated for Binary Size):
 * 1. Connect to the specified secondary server.
 * 2. Send the command "RETRIEVE ~S1/path/to/file.ext".
 * 3. Wait for the secondary server to reply.
 * 4. Read the first 4 bytes as a binary uint32_t (network byte order) representing the file size.
 * 5. If size > 0, read exactly that many bytes of file content.
 * 6. If size == 0, it means the file was not found on the secondary server.
 * 7. Return 1 on success (filling output buffer and size pointer), 0 on any failure.
 *
 * @param secondary_server_number The server ID (2, 3, or 4).
 * @param requested_filepath The file path S1 needs (e.g., ~S1/folder/file.pdf).
 * @param output_data_buffer A buffer (allocated by caller) to store the received file content.
 *                           NOTE: Caller must ensure this buffer is large enough!
 * @param received_file_size_ptr Pointer to a size_t variable where the actual received file size will be stored.
 * @return 1 if the file was successfully retrieved, 0 on any error (connection, read, file not found).
 */
int retrieve_from_secondary_server(int secondary_server_number, const char *requested_filepath, char *output_data_buffer, size_t *received_file_size_ptr)
{
    printf("DEBUG (PID %d) SecondaryRetrieve: --- Starting retrieve_from_secondary_server ---\n", getpid());
    printf("DEBUG (PID %d) SecondaryRetrieve: Target=S%d, Filepath='%s'\n", getpid(), secondary_server_number, requested_filepath);

    // Variable declarations
    int socket_to_secondary = -1;         // Socket file descriptor
    struct sockaddr_in secondary_addr;    // Struct for server address info
    char command_to_send[1024];           // Buffer for "RETRIEVE ..." command
    uint32_t file_size_network_order = 0; // To receive the binary size header
    uint32_t file_size_host_order = 0;    // Size converted to host byte order
    int operation_result_status = 0;      // 0 = failure (default), 1 = success
    size_t total_bytes_accumulated = 0;   // Counter for received file content bytes
    // Assume caller provided a sufficiently large output_data_buffer
    // We don't know the capacity here, which is a bit risky.

    /* Step 1: Create the socket */
    printf("DEBUG (PID %d) SecondaryRetrieve: Creating socket...\n", getpid());
    socket_to_secondary = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_to_secondary < 0) // Check using < 0
    {
        perror("ERROR (PID %d) SecondaryRetrieve: Socket creation failed");
        goto cleanup_secondary_retrieval; // Use goto for cleanup
    }
    printf("DEBUG (PID %d) SecondaryRetrieve: Socket created OK (fd=%d).\n", getpid(), socket_to_secondary);

    /* Step 2: Prepare the secondary server's address */
    printf("DEBUG (PID %d) SecondaryRetrieve: Setting up address for S%d...\n", getpid(), secondary_server_number);
    memset(&secondary_addr, 0, sizeof(secondary_addr)); // Zero out the struct
    secondary_addr.sin_family = AF_INET;                // IPv4
    // Port mapping: S2=9002, S3=9003, S4=9004
    secondary_addr.sin_port = htons(8000 + secondary_server_number);
    const char *secondary_server_ip = "127.0.0.1"; // Assuming localhost for simplicity

    // Convert IP string to network binary format
    if (inet_pton(AF_INET, secondary_server_ip, &secondary_addr.sin_addr) <= 0) // Check using <= 0
    {
        perror("ERROR (PID %d) SecondaryRetrieve: inet_pton failed for secondary server IP");
        goto cleanup_secondary_retrieval;
    }
    printf("DEBUG (PID %d) SecondaryRetrieve: Address setup OK (IP=%s, Port=%d).\n", getpid(), secondary_server_ip, 8000 + secondary_server_number);

    /* Step 3: Connect to the secondary server */
    printf("DEBUG (PID %d) SecondaryRetrieve: Connecting to S%d...\n", getpid(), secondary_server_number);
    if (connect(socket_to_secondary, (struct sockaddr *)&secondary_addr, sizeof(secondary_addr)) < 0) // Check using < 0
    {
        char error_msg_buffer[150]; // Buffer for formatted error message
        snprintf(error_msg_buffer, sizeof(error_msg_buffer), "ERROR (PID %d) SecondaryRetrieve: Connection to secondary server S%d failed", getpid(), secondary_server_number);
        perror(error_msg_buffer); // Print system error related to connect failure
        goto cleanup_secondary_retrieval;
    }
    printf("DEBUG (PID %d) SecondaryRetrieve: Connected successfully to S%d.\n", getpid(), secondary_server_number);

    /* Step 4: Send the RETRIEVE command */
    // Construct command: "RETRIEVE <filepath>"
    snprintf(command_to_send, sizeof(command_to_send), "RETRIEVE %s", requested_filepath);
    printf("DEBUG (PID %d) SecondaryRetrieve: Sending command: '%s'\n", getpid(), command_to_send);

    // Send the command string over the socket
    ssize_t bytes_sent_cmd = write(socket_to_secondary, command_to_send, strlen(command_to_send));
    // Check if write failed or was incomplete
    if (bytes_sent_cmd < (ssize_t)strlen(command_to_send))
    {
        perror("ERROR (PID %d) SecondaryRetrieve: Failed to send RETRIEVE command");
        goto cleanup_secondary_retrieval;
    }
    printf("DEBUG (PID %d) SecondaryRetrieve: Command sent OK (%zd bytes).\n", getpid(), bytes_sent_cmd);

    /* Step 5: Receive the File Size Header (Binary uint32_t) */
    printf("DEBUG (PID %d) SecondaryRetrieve: Waiting to receive binary file size header (4 bytes)...\n", getpid());

    // Read exactly 4 bytes for the size header
    ssize_t size_header_bytes_read = read(socket_to_secondary, &file_size_network_order, sizeof(file_size_network_order));

    // Check if reading the size header failed or was incomplete
    if (size_header_bytes_read < 0)
    {
        perror("ERROR (PID %d) SecondaryRetrieve: Failed to read size header from secondary server");
        goto cleanup_secondary_retrieval;
    }
    if (size_header_bytes_read == 0)
    {
        fprintf(stderr, "ERROR (PID %d) SecondaryRetrieve: S%d closed connection before sending size header.\n", getpid(), secondary_server_number);
        goto cleanup_secondary_retrieval;
    }
    if (size_header_bytes_read != sizeof(file_size_network_order))
    {
        fprintf(stderr, "ERROR (PID %d) SecondaryRetrieve: Incomplete size header received (%zd/%zu bytes).\n", getpid(), size_header_bytes_read, sizeof(file_size_network_order));
        goto cleanup_secondary_retrieval;
    }

    // Convert the received network byte order size to host byte order
    file_size_host_order = ntohl(file_size_network_order);
    printf("DEBUG (PID %d) SecondaryRetrieve: Received binary size: %u bytes.\n", getpid(), file_size_host_order);

    /* Step 6: Check File Size and Proceed */
    // Check if the secondary server reported size 0 (file not found)
    if (file_size_host_order == 0)
    {
        printf("INFO (PID %d) SecondaryRetrieve: Secondary server reported size 0 - file likely not found.\n", getpid());
        operation_result_status = 0; // Indicate failure (file not found)
        // No content to read, jump straight to cleanup
        goto cleanup_secondary_retrieval;
    }

    // --- If size > 0, receive the file content ---
    printf("DEBUG (PID %d) SecondaryRetrieve: Receiving %u bytes of file content...\n", getpid(), file_size_host_order);
    // Make sure the size isn't impossibly large? (Optional check, depends on buffer size assumption)
    // size_t some_reasonable_limit = 100 * 1024 * 1024; // e.g., 100 MiB limit
    // if (file_size_host_order > some_reasonable_limit) {
    //    fprintf(stderr, "ERROR (PID %d) SecondaryRetrieve: Reported size %u exceeds limit %zu. Aborting.\n", getpid(), file_size_host_order, some_reasonable_limit);
    //    goto cleanup_secondary_retrieval;
    // }

    // Loop to read exactly file_size_host_order bytes
    size_t remaining_bytes_to_read = file_size_host_order;
    while (remaining_bytes_to_read > 0)
    {

        // Read directly into the output buffer provided by the caller
        ssize_t bytes_read_this_chunk = read(socket_to_secondary,
                                             output_data_buffer + total_bytes_accumulated, // Write at correct offset
                                             remaining_bytes_to_read);                     // Try to read remaining bytes

        // Check read result
        if (bytes_read_this_chunk < 0)
        {
            perror("ERROR (PID %d) SecondaryRetrieve: Error reading file content chunk");
            operation_result_status = 0; // Mark failure
            goto cleanup_secondary_retrieval;
        }
        if (bytes_read_this_chunk == 0)
        {
            fprintf(stderr, "ERROR (PID %d) SecondaryRetrieve: S%d closed connection unexpectedly during content transfer.\n", getpid(), secondary_server_number);
            fprintf(stderr, "ERROR (PID %d) SecondaryRetrieve: Expected %zu more bytes, received %zu total.\n", getpid(), remaining_bytes_to_read, total_bytes_accumulated);
            operation_result_status = 0; // Mark failure
            goto cleanup_secondary_retrieval;
        }

        // Update counters
        total_bytes_accumulated += bytes_read_this_chunk;
        remaining_bytes_to_read -= bytes_read_this_chunk;

        // Optional: Add a small progress indicator for large files
        // printf("DEBUG (PID %d) SecondaryRetrieve: Received chunk %zd bytes, Total %zu / %u\n", getpid(), bytes_read_this_chunk, total_bytes_accumulated, file_size_host_order);
    }

    // Check if we received exactly the expected number of bytes
    if (total_bytes_accumulated == file_size_host_order)
    {
        printf("DEBUG (PID %d) SecondaryRetrieve: Successfully received %zu bytes of file content.\n", getpid(), total_bytes_accumulated);
        *received_file_size_ptr = total_bytes_accumulated; // Store the actual size received
        operation_result_status = 1;                       // Mark success!
    }
    else
    {
        // This case should ideally not be reached if the loops above are correct, but good as a fallback check.
        fprintf(stderr, "ERROR (PID %d) SecondaryRetrieve: Size mismatch after receiving content! Expected %u, Got %zu.\n",
                getpid(), file_size_host_order, total_bytes_accumulated);
        operation_result_status = 0; // Mark failure
    }

cleanup_secondary_retrieval:
    /* Step 7: Cleanup */
    printf("DEBUG (PID %d) SecondaryRetrieve: Cleaning up connection.\n", getpid());
    // Close the socket if it was opened
    if (socket_to_secondary >= 0)
    {
        close(socket_to_secondary);
        socket_to_secondary = -1; // Mark as closed
    }

    // Make sure the size pointer reflects failure if necessary
    if (operation_result_status == 0)
    {
        *received_file_size_ptr = 0; // Ensure size is 0 on failure
    }

    printf("DEBUG (PID %d) SecondaryRetrieve: --- Exiting retrieve_from_secondary_server (Result: %d) ---\n", getpid(), operation_result_status);
    return operation_result_status; // Return 1 if success, 0 otherwise

} // End of retrieve_from_secondary_server

/* ============================================================================ */
/*             remove_from_secondary_server Function                */
/* ============================================================================ */
/**
 * @brief My job is to talk to a secondary server (S2, S3, or S4) and tell it to delete a file.
 * This is called by handle_remove_command when the user asks to remove a non-.c file.
 *
 * Protocol:
 * 1. Connect to the specified secondary server.
 * 2. Send the command "REMOVE ~S1/path/to/file.ext".
 * 3. Wait for the secondary server to reply ("SUCCESS" or "ERROR").
 * 4. Return 1 if the reply was "SUCCESS", 0 otherwise.
 *
 * @param which_secondary_server The server number (2, 3, or 4).
 * @param client_specified_filepath The full path of the file to remove, as given by the client (e.g., ~S1/folder/file.pdf).
 * @return 1 if the secondary server confirmed removal, 0 on any failure or if the server reported an error.
 */
int remove_from_secondary_server(int which_secondary_server, const char *client_specified_filepath)
{
    printf("DEBUG (PID %d) SecondaryRemove: --- Starting remove_from_secondary_server ---\n", getpid());
    printf("DEBUG (PID %d) SecondaryRemove: Target=S%d, Filepath='%s'\n", getpid(), which_secondary_server, client_specified_filepath);

    // Variable declarations
    int socket_handle_to_secondary = -1;       // Socket descriptor for the connection
    struct sockaddr_in secondary_address_info; // Struct for server address
    char command_string_buffer[1024];          // Buffer for the "REMOVE ..." command
    char response_buffer_from_secondary[256];  // Buffer for the server's reply
    int final_outcome_code = 0;                // 0 = failure, 1 = success (default to failure)
    int connection_status_flag = 0;            // Redundant flag: 1 if connected

    /* Step 1: Create the socket */
    printf("DEBUG (PID %d) SecondaryRemove: Creating socket...\n", getpid());
    socket_handle_to_secondary = socket(AF_INET, SOCK_STREAM, 0);
    // Check if socket() failed (using == -1)
    if (socket_handle_to_secondary == -1)
    {
        perror("ERROR (PID %d) SecondaryRemove: Socket creation failed");
        goto cleanup_remove_secondary; // Use goto for cleanup
    }
    printf("DEBUG (PID %d) SecondaryRemove: Socket created OK (fd=%d).\n", getpid(), socket_handle_to_secondary);

    /* Step 2: Prepare the secondary server's address */
    printf("DEBUG (PID %d) SecondaryRemove: Setting up address for S%d...\n", getpid(), which_secondary_server);
    // Clear the struct
    memset(&secondary_address_info, 0, sizeof(secondary_address_info));
    secondary_address_info.sin_family = AF_INET; // IPv4
    // Port number based on server ID (8002, 8003, 8004)
    secondary_address_info.sin_port = htons(8000 + which_secondary_server);
    const char *secondary_ip_addr = "127.0.0.1"; // Assuming localhost

    // Convert IP address string to network format
    // Check if inet_pton succeeded (should return 1)
    if (inet_pton(AF_INET, secondary_ip_addr, &secondary_address_info.sin_addr) != 1) // Using != 1 check
    {
        perror("ERROR (PID %d) SecondaryRemove: inet_pton failed for address");
        goto cleanup_remove_secondary; // Use goto for cleanup
    }
    printf("DEBUG (PID %d) SecondaryRemove: Address setup OK (IP=%s, Port=%d).\n", getpid(), secondary_ip_addr, 8000 + which_secondary_server);

    /* Step 3: Connect to the secondary server */
    printf("DEBUG (PID %d) SecondaryRemove: Connecting to S%d...\n", getpid(), which_secondary_server);
    // Try connecting. Check if connect() failed (returns non-zero on error)
    if (connect(socket_handle_to_secondary, (struct sockaddr *)&secondary_address_info, sizeof(secondary_address_info)) != 0)
    {
        char error_log_msg[150];
        snprintf(error_log_msg, sizeof(error_log_msg), "ERROR (PID %d) SecondaryRemove: Connection to secondary server S%d failed", getpid(), which_secondary_server);
        perror(error_log_msg);
        goto cleanup_remove_secondary; // Use goto for cleanup
    }
    connection_status_flag = 1; // Set redundant flag
    printf("DEBUG (PID %d) SecondaryRemove: Connected successfully to S%d.\n", getpid(), which_secondary_server);

    /* Step 4: Send the REMOVE command */
    // Construct the command: "REMOVE <filepath>"
    snprintf(command_string_buffer, sizeof(command_string_buffer), "REMOVE %s", client_specified_filepath);
    printf("DEBUG (PID %d) SecondaryRemove: Sending command: '%s'\n", getpid(), command_string_buffer);

    // Send the command string
    ssize_t bytes_written_cmd = write(socket_handle_to_secondary, command_string_buffer, strlen(command_string_buffer));
    // Check if write failed or didn't write everything
    if (bytes_written_cmd < (ssize_t)strlen(command_string_buffer))
    {
        perror("ERROR (PID %d) SecondaryRemove: Failed to send REMOVE command to secondary server");
        goto cleanup_remove_secondary; // Use goto for cleanup
    }
    printf("DEBUG (PID %d) SecondaryRemove: Command sent OK (%zd bytes).\n", getpid(), bytes_written_cmd);

    /* Step 5: Receive the response */
    printf("DEBUG (PID %d) SecondaryRemove: Waiting for response from S%d...\n", getpid(), which_secondary_server);
    // Clear the response buffer
    memset(response_buffer_from_secondary, 0, sizeof(response_buffer_from_secondary));
    // Read the reply
    ssize_t received_byte_count = read(socket_handle_to_secondary, response_buffer_from_secondary, sizeof(response_buffer_from_secondary) - 1);

    // Check the result of read()
    if (received_byte_count < 0)
    { // Error during read
        perror("ERROR (PID %d) SecondaryRemove: Failed to read remove response from secondary server");
        goto cleanup_remove_secondary;
    }
    if (received_byte_count == 0 && connection_status_flag == 1)
    { // Server closed connection unexpectedly
        fprintf(stderr, "ERROR (PID %d) SecondaryRemove: S%d closed connection before sending remove response.\n", getpid(), which_secondary_server);
        goto cleanup_remove_secondary;
    }

    // Add null terminator to make it a string
    response_buffer_from_secondary[received_byte_count] = '\0';
    printf("DEBUG (PID %d) SecondaryRemove: Response received from S%d: '%s'\n", getpid(), which_secondary_server, response_buffer_from_secondary);

    /* Step 6: Check response and set outcome */
    // Check if the response starts with "SUCCESS" (using strncmp for safety, comparing 7 chars)
    if (strncmp(response_buffer_from_secondary, "SUCCESS", 7) == 0)
    {
        printf("DEBUG (PID %d) SecondaryRemove: Secondary server reported SUCCESS.\n", getpid());
        final_outcome_code = 1; // Set outcome to success
    }
    else
    {
        printf("DEBUG (PID %d) SecondaryRemove: Secondary server did NOT report SUCCESS (Response: '%s').\n", getpid(), response_buffer_from_secondary);
        final_outcome_code = 0; // Ensure outcome is failure
    }

cleanup_remove_secondary:
    /* Step 7: Cleanup */
    printf("DEBUG (PID %d) SecondaryRemove: Cleaning up connection.\n", getpid());
    // Close the network connection if it was opened
    if (socket_handle_to_secondary >= 0)
    {
        close(socket_handle_to_secondary);
        socket_handle_to_secondary = -1; // Mark as closed
    }

    printf("DEBUG (PID %d) SecondaryRemove: --- Exiting remove_from_secondary_server (Result: %d) ---\n", getpid(), final_outcome_code);
    return final_outcome_code; // Return 1 if success, 0 otherwise

} // End of remove_from_secondary_server

/* ============================================================================ */
/*             get_tar_from_secondary_server Function                           */
/* ============================================================================ */
/**
 * @brief My function to connect to a secondary server (S2 or S3) and request a tarball.
 * S2/S3 will create the tarball (e.g., pdf.tar, text.tar) and send it back.
 * I need to receive it and save it to a local file path provided by the caller.
 *
 * Protocol:
 * 1. Connect to the specified secondary server (S2 or S3).
 * 2. Send the command "MAKETAR .filetype" (e.g., "MAKETAR .pdf").
 * 3. Read the response. It should be either:
 *    - "ERROR: reason"
 *    - OR the size of the tar file as a string (e.g., "123456").
 * 4. If size received:
 *    a. Open the local output file path for writing.
 *    b. Read the specified number of bytes from the socket (the tar content).
 *    c. Write the received bytes to the local output file.
 *    d. Close the output file.
 * 5. Return 1 if the tar file was successfully received and saved, 0 otherwise.
 *
 * @param secondary_host_id The server number (2 or 3).
 * @param type_of_file The file extension string (".pdf" or ".txt").
 * @param local_save_filepath The full path where the received tarball should be saved locally (e.g., /home/user/temp_tar_12345/pdf.tar).
 * @return 1 if tarball was retrieved successfully, 0 on any failure.
 */
int get_tar_from_secondary_server(int secondary_host_id, const char *type_of_file, const char *local_save_filepath)
{
    printf("DEBUG (PID %d) GetTar: --- Starting get_tar_from_secondary_server ---\n", getpid());
    printf("DEBUG (PID %d) GetTar: Target=S%d, Type='%s', SavePath='%s'\n", getpid(), secondary_host_id, type_of_file, local_save_filepath);

    // Variable declarations
    int remote_server_socket_fd = -1;           // Socket file descriptor
    struct sockaddr_in remote_server_addr_info; // Address struct
    char command_to_send_buffer[256];           // Buffer for "MAKETAR ..."
    char size_response_buffer[32];              // Buffer for initial size/error response
    long expected_tar_size_bytes = -1;          // Parsed size from response, init to invalid
    FILE *output_tar_file_stream = NULL;        // File pointer for saving the tarball
    int overall_success_status = 0;             // 0 = fail, 1 = success
    int protocol_step_counter = 0;              // Redundant counter
    int socket_is_connected = 0;                // Flag if connect succeeded

    /* Protocol Step 1: Create Socket */
    protocol_step_counter++; // 1
    printf("DEBUG (PID %d) GetTar [Step %d]: Creating socket...\n", getpid(), protocol_step_counter);
    remote_server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (remote_server_socket_fd < 0) // Check < 0
    {
        perror("ERROR (PID %d) GetTar: Socket creation failed");
        goto cleanup_gettar; // Use goto for cleanup
    }
    printf("DEBUG (PID %d) GetTar [Step %d]: Socket created OK (fd=%d).\n", getpid(), protocol_step_counter, remote_server_socket_fd);

    /* Protocol Step 2: Setup Server Address */
    protocol_step_counter++; // 2
    printf("DEBUG (PID %d) GetTar [Step %d]: Setting up address for S%d...\n", getpid(), protocol_step_counter, secondary_host_id);
    memset(&remote_server_addr_info, 0, sizeof(remote_server_addr_info));
    remote_server_addr_info.sin_family = AF_INET;                       // IPv4
    remote_server_addr_info.sin_port = htons(8000 + secondary_host_id); // Port 8002 or 8003
    // Use 127.0.0.1 (localhost)
    if (inet_pton(AF_INET, "127.0.0.1", &remote_server_addr_info.sin_addr) != 1) // Check != 1
    {
        perror("ERROR (PID %d) GetTar: inet_pton failed");
        goto cleanup_gettar;
    }
    printf("DEBUG (PID %d) GetTar [Step %d]: Address OK.\n", getpid(), protocol_step_counter);

    /* Protocol Step 3: Connect to Server */
    protocol_step_counter++; // 3
    printf("DEBUG (PID %d) GetTar [Step %d]: Connecting to S%d...\n", getpid(), protocol_step_counter, secondary_host_id);
    if (connect(remote_server_socket_fd, (struct sockaddr *)&remote_server_addr_info, sizeof(remote_server_addr_info)) != 0) // Check != 0
    {
        char err_buf[100];
        snprintf(err_buf, sizeof(err_buf), "ERROR (PID %d) GetTar: Connect to S%d failed", getpid(), secondary_host_id);
        perror(err_buf);
        goto cleanup_gettar;
    }
    socket_is_connected = 1; // Mark connected
    printf("DEBUG (PID %d) GetTar [Step %d]: Connected.\n", getpid(), protocol_step_counter);

    /* Protocol Step 4: Send MAKETAR Command */
    protocol_step_counter++; // 4
    // Command format: "MAKETAR .filetype"
    snprintf(command_to_send_buffer, sizeof(command_to_send_buffer), "MAKETAR %s", type_of_file);
    printf("DEBUG (PID %d) GetTar [Step %d]: Sending command: '%s'\n", getpid(), protocol_step_counter, command_to_send_buffer);
    ssize_t write_cmd_check = write(remote_server_socket_fd, command_to_send_buffer, strlen(command_to_send_buffer));
    // Check write result
    if (write_cmd_check < (ssize_t)strlen(command_to_send_buffer))
    {
        perror("ERROR (PID %d) GetTar: Failed to send MAKETAR command");
        goto cleanup_gettar;
    }
    printf("DEBUG (PID %d) GetTar [Step %d]: Command sent.\n", getpid(), protocol_step_counter);

    /* Protocol Step 5: Read Initial Response (Size or Error) */
    protocol_step_counter++; // 5
    printf("DEBUG (PID %d) GetTar [Step %d]: Waiting for initial response (size/error)...\n", getpid(), protocol_step_counter);
    memset(size_response_buffer, 0, sizeof(size_response_buffer)); // Clear buffer
    ssize_t response_bytes_read = read(remote_server_socket_fd, size_response_buffer, sizeof(size_response_buffer) - 1);

    // Check read result
    if (response_bytes_read < 0)
    { // Read error
        perror("ERROR (PID %d) GetTar: Failed to read response from secondary server");
        goto cleanup_gettar;
    }
    if (response_bytes_read == 0)
    { // Disconnected
        fprintf(stderr, "ERROR (PID %d) GetTar: Secondary server S%d disconnected before sending response.\n", getpid(), secondary_host_id);
        goto cleanup_gettar;
    }

    // Null-terminate the response
    size_response_buffer[response_bytes_read] = '\0';
    printf("DEBUG (PID %d) GetTar [Step %d]: Received initial response: '%s'\n", getpid(), protocol_step_counter, size_response_buffer);

    /* Protocol Step 6: Check for Error / Parse Size */
    protocol_step_counter++; // 6
    // Check if response starts with "ERROR"
    if (strncmp(size_response_buffer, "ERROR", 5) == 0)
    {
        // Secondary server reported an error
        fprintf(stderr, "ERROR (PID %d) GetTar: S%d responded with error: %s\n", getpid(), secondary_host_id, size_response_buffer);
        goto cleanup_gettar;
    }

    // If not ERROR, try to parse it as a number (the size) using strtol for better error checking
    char *parse_end_ptr;
    expected_tar_size_bytes = strtol(size_response_buffer, &parse_end_ptr, 10); // Base 10

    // Check if parsing failed (no digits, leftover chars, negative result)
    if (parse_end_ptr == size_response_buffer || *parse_end_ptr != '\0' || expected_tar_size_bytes < 0)
    {
        fprintf(stderr, "ERROR (PID %d) GetTar: Invalid size response from S%d: '%s'\n", getpid(), secondary_host_id, size_response_buffer);
        goto cleanup_gettar;
    }

    printf("DEBUG (PID %d) GetTar [Step %d]: Parsed expected tar size: %ld bytes.\n", getpid(), protocol_step_counter, expected_tar_size_bytes);

    // Handle case where server reports size 0 (no files found)
    if (expected_tar_size_bytes == 0)
    {
        printf("INFO (PID %d) GetTar: S%d reported 0 files found (size 0). Treating as failure to get tar.\n", getpid(), secondary_host_id);
        goto cleanup_gettar; // Return failure (as per original logic interpretation)
    }

    /* Protocol Step 7: Receive Tar Content and Save to File */
    protocol_step_counter++; // 7
    printf("DEBUG (PID %d) GetTar [Step %d]: Opening local output file '%s' for writing...\n", getpid(), protocol_step_counter, local_save_filepath);
    // Open the local file specified by the caller for binary writing
    output_tar_file_stream = fopen(local_save_filepath, "wb");
    // Check if fopen failed
    if (output_tar_file_stream == NULL) // Use == NULL
    {
        perror("ERROR (PID %d) GetTar: Failed to open local output file for writing");
        fprintf(stderr, "ERROR (PID %d) GetTar: Cannot create/write to '%s'.\n", getpid(), local_save_filepath);
        goto cleanup_gettar; // Return failure
    }
    printf("DEBUG (PID %d) GetTar [Step %d]: Output file opened successfully.\n", getpid(), protocol_step_counter);

    printf("DEBUG (PID %d) GetTar [Step %d]: Starting loop to receive %ld bytes of tar content...\n", getpid(), protocol_step_counter, expected_tar_size_bytes);
    // Buffer for receiving chunks
    char reception_buffer[4096];
    size_t bytes_received_so_far = 0;
    ssize_t bytes_read_this_loop;
    int read_loop_count = 0; // Redundant counter

    // Loop until we have received the total expected size
    while (bytes_received_so_far < (size_t)expected_tar_size_bytes)
    {
        read_loop_count++;
        // Calculate remaining bytes to avoid reading past expected size
        size_t bytes_remaining = (size_t)expected_tar_size_bytes - bytes_received_so_far;
        size_t bytes_to_read_now = (bytes_remaining < sizeof(reception_buffer)) ? bytes_remaining : sizeof(reception_buffer);

        bytes_read_this_loop = read(remote_server_socket_fd, reception_buffer, bytes_to_read_now);

        // Check read result
        if (bytes_read_this_loop < 0)
        { // Read error
            perror("ERROR (PID %d) GetTar: Failed while reading tar content from socket");
            goto cleanup_gettar; // Use goto for cleanup (will also close file/remove partial)
        }
        if (bytes_read_this_loop == 0)
        { // Server disconnected prematurely
            fprintf(stderr, "ERROR (PID %d) GetTar: Secondary server S%d disconnected before sending entire tar file (got %zu/%lu bytes).\n",
                    getpid(), secondary_host_id, bytes_received_so_far, (unsigned long)expected_tar_size_bytes);
            goto cleanup_gettar; // Use goto for cleanup
        }

        // Write the received chunk to the local file
        size_t bytes_written_to_file = fwrite(reception_buffer, 1, bytes_read_this_loop, output_tar_file_stream);
        // Check if fwrite failed
        if (bytes_written_to_file < (size_t)bytes_read_this_loop)
        {
            perror("ERROR (PID %d) GetTar: Failed writing received chunk to local file");
            fprintf(stderr, "ERROR (PID %d) GetTar: fwrite error on '%s'. Disk full?\n", getpid(), local_save_filepath);
            goto cleanup_gettar; // Use goto for cleanup
        }

        // Update total bytes received
        bytes_received_so_far += bytes_written_to_file; // Use bytes written for accuracy

    } // End of receiving loop

    printf("DEBUG (PID %d) GetTar [Step %d]: Finished receiving loop in %d reads. Total bytes received: %zu\n", getpid(), protocol_step_counter, read_loop_count, bytes_received_so_far);

    /* Protocol Step 8: Final Check */
    protocol_step_counter++; // 8
    // Check: Did we receive exactly the expected number of bytes?
    if (bytes_received_so_far == (size_t)expected_tar_size_bytes)
    {
        printf("INFO (PID %d) GetTar: Successfully received expected %zu bytes for '%s'.\n", getpid(), bytes_received_so_far, local_save_filepath);
        overall_success_status = 1; // Set success
    }
    else
    {
        // This might happen if server sent more data than the size indicated? Or less but didn't disconnect?
        fprintf(stderr, "WARN (PID %d) GetTar: Mismatch! Expected %lu bytes, but received %zu bytes. File might be corrupt.\n",
                getpid(), (unsigned long)expected_tar_size_bytes, bytes_received_so_far);
        // Treat mismatch as failure.
        overall_success_status = 0;
        // Cleanup will handle removing the file.
    }

/* --- Cleanup Section --- */
cleanup_gettar:
    protocol_step_counter++; // Final step number
    printf("DEBUG (PID %d) GetTar [Step %d]: Cleaning up resources...\n", getpid(), protocol_step_counter);

    // Close the output file stream if it was opened
    if (output_tar_file_stream != NULL)
    {
        if (fclose(output_tar_file_stream) != 0)
        {
            perror("WARN (PID %d) GetTar: fclose failed for output file");
        }
        output_tar_file_stream = NULL; // Mark as closed
        // If the operation ultimately failed, remove the potentially incomplete/corrupt file
        if (overall_success_status == 0 && local_save_filepath != NULL)
        {
            printf("DEBUG (PID %d) GetTar: Operation failed, removing potentially incomplete file: '%s'\n", getpid(), local_save_filepath);
            remove(local_save_filepath); // Ignore error
        }
    }
    else
    {
        // If fopen failed, ensure no partial file is left if path exists
        if (overall_success_status == 0 && local_save_filepath != NULL)
        {
            // Remove might fail if the path was bad, that's okay.
            remove(local_save_filepath); // Ignore error
        }
    }

    // Close the network socket if it was opened
    if (remote_server_socket_fd >= 0)
    {
        printf("DEBUG (PID %d) GetTar: Closing network socket fd %d.\n", getpid(), remote_server_socket_fd);
        close(remote_server_socket_fd);
        remote_server_socket_fd = -1; // Mark as closed
    }

    printf("DEBUG (PID %d) GetTar: --- Exiting get_tar_from_secondary_server (Result: %d) ---\n", getpid(), overall_success_status);
    return overall_success_status; // Return 1 for success, 0 for failure

} // End of get_tar_from_secondary_server
