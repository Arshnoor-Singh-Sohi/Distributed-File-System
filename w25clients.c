/* ======================================================== */
/*       w25clients.c - DFS Client Program                  */
/* ======================================================== */
/* This is the client program for my Distributed File System. */
/* It connects to the S1 server and sends user commands.    */
/* It handles interactions like uploads, downloads, etc.    */

// Standard C library includes
#include <stdio.h>      // For standard I/O: printf, fgets, stdin, stdout, stderr, perror, fopen, etc.
#include <stdlib.h>     // For general utilities: exit, atoi (not used?), malloc/free (optional), EXIT_SUCCESS/FAILURE
#include <string.h>     // For string manipulation: strcmp, strncmp, strcpy, strlen, strcspn, strtok, strrchr, memset, memcpy
#include <unistd.h>     // For POSIX system calls: read, write, close
#include <sys/socket.h> // For socket programming functions and structures: socket, connect, sockaddr_in
#include <netinet/in.h> // For internet address structures (sockaddr_in) and functions (htons, htonl)
#include <arpa/inet.h>  // For IP address conversion: inet_pton
#include <fcntl.h>      // For file control options (not directly used here, but often related)
#include <sys/stat.h>   // For file status functions: stat() (useful for checking local files)
#include <libgen.h>     // For basename() (useful for extracting filenames, though strrchr used here)
#include <ctype.h>      // For character type checking: isdigit() used in downlf
#include <errno.h>      // For checking the errno variable after system calls
#include <sys/wait.h>
#include <stdint.h>

// Server Configuration (Connects ONLY to S1)
// Make sure these match the running S1 server details! Found online examples use defines.
#define S1_SERVER_IP_ADDRESS "127.0.0.1" // Assuming S1 runs on the same machine (localhost) for testing
#define S1_SERVER_PORT 9555              // The port number S1 is listening on (make sure this is correct!)

// Buffer Sizes
#define USER_COMMAND_BUFFER_SIZE 1024   // Max length for user input command
#define FILE_TRANSFER_CHUNK_SIZE 4096   // Size of chunks for file up/download
#define SERVER_RESPONSE_BUFFER_SIZE 256 // Buffer for simple ACK/ERROR messages from server
#define MAX_FILENAME_LIST_SIZE 65536    // Max expected size for dispfnames result (should match S1)

#ifndef FILE_CHUNK_BUFFER_SIZE_CLIENT
#define FILE_CHUNK_BUFFER_SIZE_CLIENT 4096
#endif

#ifndef MAX_REASONABLE_FILE_SIZE
#define MAX_REASONABLE_FILE_SIZE (100 * 1024 * 1024) // 100 MiB
#endif

/* --- Function Prototypes --- */
// Establishes a connection to the S1 server.
int establish_server_connection();
// Validates the syntax of the user's command locally before sending.
int check_user_command_syntax(const char *full_command, char *parsed_command_type, char *first_parameter, char *second_parameter);

// Functions to handle the logic for each specific command.
void execute_uploadf_command(int server_socket_fd, char *local_filename, char *server_destination_path);
void execute_download_command(int server_socket_fd, const char *command_line, const char *s1_filepath);
void execute_removef_command(int server_socket_fd, char *server_filepath);
void execute_downltar_command(int server_socket_fd, char *requested_file_type);
void execute_dispfnames_command(int server_socket_fd, char *server_directory_path);

/* ======================================================== */
/*                 Main Function (Entry Point)              */
/* ======================================================== */
int main()
{
    // Buffers to hold user input and parsed command components
    char user_input_buffer[USER_COMMAND_BUFFER_SIZE];
    char command_action_type[20];    // e.g., "uploadf", "downlf"
    char command_parameter_one[256]; // First argument (filename, path, type)
    char command_parameter_two[256]; // Second argument (dest_path for uploadf)
    int client_running_status = 1;   // Flag for main loop control (redundant)

    // Welcome message and command list
    printf("------------------------------------------------------\n");
    printf("Welcome to w25clients - Distributed File System Client\n");
    printf("------------------------------------------------------\n");
    printf("Connects to S1 Server at: %s:%d\n\n", S1_SERVER_IP_ADDRESS, S1_SERVER_PORT);
    printf("Available Commands:\n");
    printf("  uploadf <local_filename> <~S1/destination_path>\n");
    printf("  downlf <~S1/path/filename>\n");
    printf("  removef <~S1/path/filename>\n");
    printf("  downltar <.c | .pdf | .txt>\n");
    printf("  dispfnames <~S1/path/>\n");
    printf("  exit\n\n");

    // --- Main Client Loop ---
    // Keep running until the user types 'exit'.
    while (client_running_status == 1) // Using flag instead of while(1)
    {
        // Display the command prompt
        printf("w25clients$ ");
        fflush(stdout); // Make sure the prompt is displayed immediately

        // Read the user's command line input
        if (fgets(user_input_buffer, sizeof(user_input_buffer), stdin) == NULL)
        {
            // Error reading input or EOF reached (e.g., Ctrl+D)
            printf("\nINFO: Input stream closed. Exiting.\n");
            break; // Exit the loop
        }
        printf("DEBUG: Raw input read: '%s'\n", user_input_buffer);

        // Remove the trailing newline character added by fgets()
        user_input_buffer[strcspn(user_input_buffer, "\n")] = '\0';

        // Check for empty input
        if (strlen(user_input_buffer) == 0)
        {
            printf("DEBUG: Empty command entered.\n");
            continue; // Ask for input again
        }

        // Check if the user wants to exit
        if (strcmp(user_input_buffer, "exit") == 0)
        {
            printf("INFO: 'exit' command received. Shutting down client...\n");
            client_running_status = 0; // Set flag to exit loop
            break;                     // Exit the loop immediately
        }

        // --- Validate Command Syntax Locally ---
        printf("DEBUG: Validating command syntax locally...\n");
        // Call the validation function. It prints errors if syntax is wrong.
        // Returns 0 on success, non-zero on failure.
        int validation_result = check_user_command_syntax(user_input_buffer,
                                                          command_action_type,
                                                          command_parameter_one,
                                                          command_parameter_two);

        // If validation failed, print message already handled by validator, loop again.
        if (validation_result != 0)
        {
            printf("DEBUG: Command validation failed. Asking for input again.\n");
            continue; // Go to the next iteration of the while loop
        }
        // If validation passed, command_action_type and parameters are filled.
        printf("DEBUG: Command validation successful. Type: '%s', Param1: '%s', Param2: '%s'\n",
               command_action_type, command_parameter_one, command_parameter_two);

        // --- Connect to S1 Server ---
        printf("DEBUG: Attempting to connect to S1 server...\n");
        int server_connection_fd = establish_server_connection();

        // Check if connection failed
        if (server_connection_fd < 0)
        {
            fprintf(stderr, "ERROR: Failed to establish connection with S1 server at %s:%d. Is S1 running?\n",
                    S1_SERVER_IP_ADDRESS, S1_SERVER_PORT);
            // Don't exit the client, just skip processing this command.
            continue; // Go to the next iteration of the while loop
        }
        // Connection successful!
        printf("DEBUG: Connected to S1 server successfully (Socket FD: %d).\n", server_connection_fd);

        // --- Process the Validated Command ---
        // Call the appropriate function based on the parsed command type.
        // We pass the original user input buffer as well, as some handlers might re-parse.
        printf("DEBUG: Processing command '%s'...\n", command_action_type);
        if (strcmp(command_action_type, "uploadf") == 0)
        {
            execute_uploadf_command(server_connection_fd, command_parameter_one, command_parameter_two);
        }
        else if (strcmp(command_action_type, "downlf") == 0)
        {
            execute_download_command(server_connection_fd, user_input_buffer, command_parameter_one);
        }
        else if (strcmp(command_action_type, "removef") == 0)
        {
            execute_removef_command(server_connection_fd, command_parameter_one);
        }
        else if (strcmp(command_action_type, "downltar") == 0)
        {
            execute_downltar_command(server_connection_fd, command_parameter_one);
        }
        else if (strcmp(command_action_type, "dispfnames") == 0)
        {
            execute_dispfnames_command(server_connection_fd, command_parameter_one);
        }
        // No 'else' needed here because validation already checked the command type.

        // --- Close Connection ---
        // After processing one command, close the connection to S1.
        // (Except for dispfnames, where S1 closes it).
        // Check if socket is still valid before closing (dispfnames might have closed it implicitly)
        printf("DEBUG: Command processing finished. Closing connection to S1 (fd=%d).\n", server_connection_fd);
        // Check if close() returns an error
        if (close(server_connection_fd) != 0)
        {
            perror("WARN: Error closing socket connection to S1");
        }
        printf("DEBUG: Connection closed.\n");

    } // End of main client loop (while)

    printf("\nw25clients finished.\n");
    return EXIT_SUCCESS; // Indicate successful termination

} // End of main function

/* ======================================================== */
/*          Server Connection Function                      */
/* ======================================================== */
/**
 * @brief Creates a socket and connects to the S1 server.
 * Uses the globally defined S1_SERVER_IP_ADDRESS and S1_SERVER_PORT.
 *
 * @return The socket file descriptor if connection is successful,
 *         or -1 if any step fails.
 */
int establish_server_connection()
{
    int client_socket_fd;                 // File descriptor for the client's socket
    struct sockaddr_in s1_server_address; // Structure to hold S1 server's address details
    int connection_result_code;           // Result of connect() call

    printf("DEBUG: Creating client socket (IPv4, TCP)...\n");
    /* 1. Create Socket */
    client_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    // Check if socket creation failed (returns -1)
    if (client_socket_fd < 0) // Using < 0 check
    {
        perror("ERROR: Client socket creation failed");
        return -1; // Return error code
    }
    printf("DEBUG: Client socket created (fd=%d).\n", client_socket_fd);

    /* 2. Set up S1 Server Address Structure */
    printf("DEBUG: Setting up S1 server address structure (%s:%d)...\n", S1_SERVER_IP_ADDRESS, S1_SERVER_PORT);
    // Clear the structure first
    memset(&s1_server_address, 0, sizeof(s1_server_address));
    // Set address family (IPv4)
    s1_server_address.sin_family = AF_INET;
    // Set port number (convert to network byte order)
    s1_server_address.sin_port = htons(S1_SERVER_PORT);
    // Convert the IP address string to binary network format
    // inet_pton returns 1 on success, 0 if address is invalid, -1 on error.
    int pton_status = inet_pton(AF_INET, S1_SERVER_IP_ADDRESS, &s1_server_address.sin_addr);
    if (pton_status != 1) // Check for non-success (0 or -1)
    {
        if (pton_status == 0)
        {
            fprintf(stderr, "ERROR: Invalid S1 server IP address format '%s'.\n", S1_SERVER_IP_ADDRESS);
        }
        else
        {
            perror("ERROR: inet_pton failed for S1 server address");
        }
        close(client_socket_fd); // Close the created socket
        return -1;               // Return error code
    }
    printf("DEBUG: S1 server address structure configured.\n", getpid());

    /* 3. Connect to S1 Server */
    printf("DEBUG: Attempting to connect to S1 server...\n");
    // connect() attempts to establish a TCP connection to the server.
    // Returns 0 on success, -1 on error.
    connection_result_code = connect(client_socket_fd, (struct sockaddr *)&s1_server_address, sizeof(s1_server_address));
    // Using != 0 check for failure
    if (connection_result_code != 0)
    {
        perror("ERROR: Connection attempt to S1 server failed");
        close(client_socket_fd); // Close the socket
        return -1;               // Return error code
    }

    // If we reach here, connection was successful!
    printf("DEBUG: Connection to S1 established.\n");
    return client_socket_fd; // Return the connected socket file descriptor

} // End of establish_server_connection

/* ======================================================== */
/*          Command Syntax Validation Function              */
/* ======================================================== */
/**
 * @brief Validates the syntax of the command entered by the user.
 * Checks the command type and the number/format of required parameters.
 * Parses the command into type, param1, and param2 if valid.
 * Prints specific error messages for invalid syntax.
 *
 * @param full_command The complete command string entered by the user.
 * @param parsed_command_type Output buffer to store the parsed command type (e.g., "uploadf").
 * @param first_parameter Output buffer to store the first parameter.
 * @param second_parameter Output buffer to store the second parameter (if applicable).
 * @return 0 if the command syntax is valid, -1 otherwise.
 */
int check_user_command_syntax(const char *full_command, char *parsed_command_type, char *first_parameter, char *second_parameter)
{
    // Buffer to safely tokenize the command without modifying the original
    char command_string_copy[USER_COMMAND_BUFFER_SIZE];
    char *current_token_ptr = NULL; // Pointer for strtok results
    int parameter_count = 0;        // Count parsed parameters
    int validation_status = 0;      // 0 = OK, -1 = Error

    printf("DEBUG VALIDATE: Validating '%s'\n", full_command);

    // Initialize output parameters to empty strings
    parsed_command_type[0] = '\0';
    first_parameter[0] = '\0';
    second_parameter[0] = '\0';

    // --- Step 1: Basic Checks and Copy ---
    if (full_command == NULL || strlen(full_command) == 0)
    {
        printf("ERROR: Command is empty.\n");
        return -1;
    }
    // Copy the command string because strtok modifies its input
    strncpy(command_string_copy, full_command, sizeof(command_string_copy) - 1);
    command_string_copy[sizeof(command_string_copy) - 1] = '\0'; // Ensure null termination

    // --- Step 2: Parse Command Type (First Token) ---
    printf("DEBUG VALIDATE: Parsing command type...\n");
    // Get the first token separated by space
    current_token_ptr = strtok(command_string_copy, " ");
    // Check if any token was found
    if (current_token_ptr == NULL)
    {
        printf("ERROR: Command seems empty or contains only spaces.\n");
        return -1; // Invalid
    }
    // Copy the first token (command type) to the output buffer
    strncpy(parsed_command_type, current_token_ptr, 19); // Max 19 chars + null
    parsed_command_type[19] = '\0';
    printf("DEBUG VALIDATE: Parsed type: '%s'\n", parsed_command_type);

    // --- Step 3: Validate Command Type ---
    // Check if the parsed type is one of the known valid commands
    const char *valid_commands[] = {"uploadf", "downlf", "removef", "downltar", "dispfnames", NULL};
    int known_command_found = 0;
    for (int i = 0; valid_commands[i] != NULL; ++i)
    {
        if (strcmp(parsed_command_type, valid_commands[i]) == 0)
        {
            known_command_found = 1;
            break;
        }
    }
    if (known_command_found == 0) // Changed from checking individual strings
    {
        printf("ERROR: Unknown command specified: '%s'\n", parsed_command_type);
        return -1; // Invalid
    }
    printf("DEBUG VALIDATE: Command type '%s' is recognized.\n", parsed_command_type);

    // --- Step 4: Parse Parameters ---
    printf("DEBUG VALIDATE: Parsing parameters...\n");
    // Loop to get subsequent tokens (parameters)
    while ((current_token_ptr = strtok(NULL, " ")) != NULL)
    {
        parameter_count++;
        if (parameter_count == 1)
        {
            strncpy(first_parameter, current_token_ptr, 255);
            first_parameter[255] = '\0';
            printf("DEBUG VALIDATE: Parsed Param1: '%s'\n", first_parameter);
        }
        else if (parameter_count == 2)
        {
            strncpy(second_parameter, current_token_ptr, 255);
            second_parameter[255] = '\0';
            printf("DEBUG VALIDATE: Parsed Param2: '%s'\n", second_parameter);
        }
        else
        {
            // Too many parameters found
            printf("ERROR: Too many arguments provided for command '%s'.\n", parsed_command_type);
            validation_status = -1;
            break; // Stop parsing extra tokens
        }
    }

    // If parsing didn't hit "too many args" error, proceed with specific checks
    if (validation_status == 0)
    {
        // --- Step 5: Specific Validation Based on Command Type ---
        printf("DEBUG VALIDATE: Performing command-specific checks...\n");

        // Case 1: uploadf <local_filename> <~S1/destination_path>
        if (strcmp(parsed_command_type, "uploadf") == 0)
        {
            if (parameter_count != 2)
            {
                printf("ERROR: 'uploadf' requires exactly two parameters: <local_filename> <~S1/destination_path>\n");
                validation_status = -1;
            }
            else if (strncmp(second_parameter, "~S1/", 4) != 0)
            {
                printf("ERROR: Destination path for 'uploadf' must start with ~S1/ (got '%s')\n", second_parameter);
                validation_status = -1;
            }
            // Maybe add check for valid local filename characters? (Optional enhancement)
            // Maybe add check for file extension? (.c, .pdf, .txt, .zip)
            char *ext_up = strrchr(first_parameter, '.');
            if (!ext_up || (strcasecmp(ext_up, ".c") != 0 && strcasecmp(ext_up, ".pdf") != 0 && strcasecmp(ext_up, ".txt") != 0 && strcasecmp(ext_up, ".zip") != 0))
            {
                printf("ERROR: 'uploadf' filename must have a valid extension (.c, .pdf, .txt, .zip). Got: '%s'\n", first_parameter);
                validation_status = -1;
            }
        }
        // Case 2: downlf <~S1/path/filename>
        else if (strcmp(parsed_command_type, "downlf") == 0)
        {
            if (parameter_count != 1)
            {
                printf("ERROR: 'downlf' requires exactly one parameter: <~S1/path/filename>\n");
                validation_status = -1;
            }
            else if (strncmp(first_parameter, "~S1/", 4) != 0)
            {
                printf("ERROR: File path for 'downlf' must start with ~S1/ (got '%s')\n", first_parameter);
                validation_status = -1;
            }
            // Check for valid extension (.c, .pdf, .txt, .zip allowed based on PDF)
            char *ext_dl = strrchr(first_parameter, '.');
            if (!ext_dl || (strcasecmp(ext_dl, ".c") != 0 && strcasecmp(ext_dl, ".pdf") != 0 && strcasecmp(ext_dl, ".txt") != 0 && strcasecmp(ext_dl, ".zip") != 0))
            {
                printf("ERROR: 'downlf' filename must have a valid extension (.c, .pdf, .txt, .zip). Got: '%s'\n", first_parameter);
                validation_status = -1;
            }
        }
        // Case 3: removef <~S1/path/filename>
        else if (strcmp(parsed_command_type, "removef") == 0)
        {
            if (parameter_count != 1)
            {
                printf("ERROR: 'removef' requires exactly one parameter: <~S1/path/filename>\n");
                validation_status = -1;
            }
            else if (strncmp(first_parameter, "~S1/", 4) != 0)
            {
                printf("ERROR: File path for 'removef' must start with ~S1/ (got '%s')\n", first_parameter);
                validation_status = -1;
            }
            // Check for valid extension (.c, .pdf, .txt, .zip - although PDF example missing in removef section of PDF, assume all types allowed based on other commands)
            char *ext_rm = strrchr(first_parameter, '.');
            if (!ext_rm || (strcasecmp(ext_rm, ".c") != 0 && strcasecmp(ext_rm, ".pdf") != 0 && strcasecmp(ext_rm, ".txt") != 0 && strcasecmp(ext_rm, ".zip") != 0))
            {
                printf("ERROR: 'removef' filename must have a valid extension (.c, .pdf, .txt, .zip). Got: '%s'\n", first_parameter);
                validation_status = -1;
            }
        }
        // Case 4: downltar <.c | .pdf | .txt>
        else if (strcmp(parsed_command_type, "downltar") == 0)
        {
            if (parameter_count != 1)
            {
                printf("ERROR: 'downltar' requires exactly one parameter: <.c | .pdf | .txt>\n");
                validation_status = -1;
            }
            else if (strcmp(first_parameter, ".c") != 0 &&
                     strcmp(first_parameter, ".pdf") != 0 &&
                     strcmp(first_parameter, ".txt") != 0)
            {
                // Case sensitive comparison seems intended for the file type argument.
                printf("ERROR: File type for 'downltar' must be exactly '.c', '.pdf', or '.txt' (got '%s')\n", first_parameter);
                validation_status = -1;
            }
        }
        // Case 5: dispfnames <~S1/path/>
        else if (strcmp(parsed_command_type, "dispfnames") == 0)
        {
            if (parameter_count != 1)
            {
                printf("ERROR: 'dispfnames' requires exactly one parameter: <~S1/pathname/>\n");
                validation_status = -1;
            }
            else if (strncmp(first_parameter, "~S1/", 4) != 0)
            {
                printf("ERROR: Pathname for 'dispfnames' must start with ~S1/ (got '%s')\n", first_parameter);
                validation_status = -1;
            }
            // Should path end with '/'? PDF implies yes, but example doesn't show it. Let's allow both for robustness.
        }
    } // End command-specific checks

    printf("DEBUG VALIDATE: Final validation status: %d (0=OK, -1=Error)\n", validation_status);
    return validation_status; // Return 0 if OK, -1 if error

} // End of check_user_command_syntax

/* ======================================================== */
/*             Command Processing Functions                 */
/* ======================================================== */

/**
 * @brief Processes the 'uploadf' command. Connects to S1, sends command,
 * waits for READY, sends size, sends content, gets final response.
 *
 * @param server_socket_fd Socket descriptor connected to S1.
 * @param local_filename Path to the local file to upload.
 * @param server_destination_path Destination path on the server (starts with ~S1/).
 */
void execute_uploadf_command(int server_socket_fd, char *local_filename, char *server_destination_path)
{
    FILE *local_file_handle = NULL;                           // File pointer for the local file
    char file_chunk_buffer[FILE_TRANSFER_CHUNK_SIZE];         // Buffer for file content
    size_t local_file_byte_size = 0;                          // Size of the local file
    char command_string_to_server[USER_COMMAND_BUFFER_SIZE];  // Command sent to S1
    char server_response_buffer[SERVER_RESPONSE_BUFFER_SIZE]; // Buffer for S1 responses
    ssize_t bytes_received_from_server;
    ssize_t bytes_written_to_server;
    int upload_failed_flag = 0; // 0=OK, 1=Failed

    printf("DEBUG UPLOAD: Starting 'uploadf' process for '%s' -> '%s'\n", local_filename, server_destination_path);

    /* 1. Check Local File Existence and Get Size */
    printf("DEBUG UPLOAD: Checking local file '%s'...\n", local_filename);
    local_file_handle = fopen(local_filename, "rb"); // Open for binary reading
    // Check if fopen failed
    if (local_file_handle == NULL)
    {
        perror("ERROR: Could not open local file");
        fprintf(stderr, "ERROR: Local file '%s' not found or cannot be opened for reading.\n", local_filename);
        return; // Cannot proceed without the local file
    }
    // Get file size using fseek/ftell
    if (fseek(local_file_handle, 0, SEEK_END) != 0 ||
        (local_file_byte_size = ftell(local_file_handle)) == (size_t)-1 || // Check for error (-1L)
        fseek(local_file_handle, 0, SEEK_SET) != 0)
    {
        perror("ERROR: Could not determine size of local file");
        fprintf(stderr, "ERROR: Failed to get size for '%s'.\n", local_filename);
        fclose(local_file_handle);
        return; // Cannot proceed without size
    }
    printf("DEBUG UPLOAD: Local file size: %zu bytes.\n", local_file_byte_size);

    /* 2. Construct and Send Command to S1 */
    // Format: "uploadf <local_filename_basename> <server_destination_path>"
    // Note: S1 uses the *destination* path to figure out extension, the source filename is less critical for S1.
    // Sending full local path might leak info, let's try sending basename.
    // char *local_filename_base = basename(local_filename); // Requires mutable string, make copy
    // For simplicity matching original, send the full local filename as given by user.
    snprintf(command_string_to_server, sizeof(command_string_to_server), "uploadf %s %s",
             local_filename, server_destination_path);
    printf("DEBUG UPLOAD: Sending command to S1: '%s'\n", command_string_to_server);
    bytes_written_to_server = write(server_socket_fd, command_string_to_server, strlen(command_string_to_server));
    // Check write result
    if (bytes_written_to_server < (ssize_t)strlen(command_string_to_server))
    {
        perror("ERROR: Failed sending command string to S1");
        if (bytes_written_to_server < 0)
            fprintf(stderr, "ERROR: Write error occurred.\n");
        else
            fprintf(stderr, "ERROR: Incomplete write (%zd/%zu bytes).\n", bytes_written_to_server, strlen(command_string_to_server));
        fclose(local_file_handle);
        return; // Cannot proceed
    }
    printf("DEBUG UPLOAD: Command sent.\n");

    /* 3. Wait for "READY" Response from S1 */
    printf("DEBUG UPLOAD: Waiting for 'READY' response from S1...\n");
    memset(server_response_buffer, 0, sizeof(server_response_buffer)); // Clear buffer
    bytes_received_from_server = read(server_socket_fd, server_response_buffer, sizeof(server_response_buffer) - 1);
    // Check read result
    if (bytes_received_from_server <= 0)
    {
        if (bytes_received_from_server == 0)
            fprintf(stderr, "ERROR: S1 closed connection before sending READY.\n");
        else
            perror("ERROR: Failed reading READY response from S1");
        fclose(local_file_handle);
        return; // Cannot proceed
    }
    // Null-terminate the response
    server_response_buffer[bytes_received_from_server] = '\0';
    printf("DEBUG UPLOAD: Received from S1: '%s'\n", server_response_buffer);

    // Check if the response is "READY"
    if (strcmp(server_response_buffer, "READY") != 0)
    {
        fprintf(stderr, "ERROR: Server did not respond with READY. Response: '%s'. Aborting upload.\n", server_response_buffer);
        fclose(local_file_handle);
        return; // Server is not ready or sent an error
    }
    printf("DEBUG UPLOAD: Server is READY.\n");

    /* 4. Send File Size (uint64_t) to S1 */
    uint64_t file_size_64bit = (uint64_t)local_file_byte_size; // Use 64-bit for size
    printf("DEBUG UPLOAD: Sending file size (%llu bytes) to S1...\n", (unsigned long long)file_size_64bit);
    bytes_written_to_server = write(server_socket_fd, &file_size_64bit, sizeof(file_size_64bit));
    // Check write result
    if (bytes_written_to_server < (ssize_t)sizeof(file_size_64bit))
    {
        perror("ERROR: Failed sending file size to S1");
        if (bytes_written_to_server < 0)
            fprintf(stderr, "ERROR: Write error occurred.\n");
        else
            fprintf(stderr, "ERROR: Incomplete write (%zd/%zu bytes).\n", bytes_written_to_server, sizeof(file_size_64bit));
        fclose(local_file_handle);
        return; // Cannot proceed
    }
    printf("DEBUG UPLOAD: File size sent.\n");

    /* 5. Send File Content */
    printf("DEBUG UPLOAD: Starting file content transfer...\n");
    size_t total_bytes_actually_sent = 0; // Counter for sent bytes
    size_t bytes_read_from_local_file;    // Bytes read by fread
    int send_loop_iteration = 0;          // Redundant counter

    // Loop reading chunks from local file and writing them to socket
    while (total_bytes_actually_sent < local_file_byte_size &&
           (bytes_read_from_local_file = fread(file_chunk_buffer, 1, sizeof(file_chunk_buffer), local_file_handle)) > 0)
    {
        send_loop_iteration++;
        // Write the read chunk to the server socket
        bytes_written_to_server = write(server_socket_fd, file_chunk_buffer, bytes_read_from_local_file);
        // Check write result
        if (bytes_written_to_server < 0 || (size_t)bytes_written_to_server != bytes_read_from_local_file)
        {
            perror("ERROR: Failed writing file content chunk to S1");
            if (bytes_written_to_server >= 0)
                fprintf(stderr, "ERROR: Incomplete write (%zd/%zu bytes).\n", bytes_written_to_server, bytes_read_from_local_file);
            upload_failed_flag = 1; // Mark failure
            break;                  // Stop sending loop
        }
        // Update total bytes sent
        total_bytes_actually_sent += bytes_written_to_server;
        printf("DEBUG UPLOAD: Sent chunk %d, total %zu / %zu bytes.\n", send_loop_iteration, total_bytes_actually_sent, local_file_byte_size);

    } // End of file content sending loop

    // Check if loop exited due to fread error
    if (ferror(local_file_handle))
    {
        perror("ERROR: Error reading from local file during upload");
        upload_failed_flag = 1; // Mark failure
    }
    // Check if loop finished but not all bytes were sent (should only happen if write failed)
    else if (upload_failed_flag == 0 && total_bytes_actually_sent != local_file_byte_size)
    {
        fprintf(stderr, "ERROR: Upload incomplete! Sent %zu but expected %zu bytes.\n", total_bytes_actually_sent, local_file_byte_size);
        upload_failed_flag = 1; // Mark failure
    }

    // Close the local file handle
    fclose(local_file_handle);
    printf("DEBUG UPLOAD: Finished sending content (or stopped due to error).\n");

    /* 6. Get Final Response from S1 */
    // Only expect final response if the sending part didn't fail catastrophically
    if (upload_failed_flag == 0)
    {
        printf("DEBUG UPLOAD: Waiting for final SUCCESS/ERROR response from S1...\n");
        memset(server_response_buffer, 0, sizeof(server_response_buffer)); // Clear buffer
        bytes_received_from_server = read(server_socket_fd, server_response_buffer, sizeof(server_response_buffer) - 1);
        // Check read result
        if (bytes_received_from_server <= 0)
        {
            if (bytes_received_from_server == 0)
                fprintf(stderr, "ERROR: S1 closed connection before sending final status.\n");
            else
                perror("ERROR: Failed reading final response from S1");
            // Cannot determine final status
        }
        else
        {
            // Null-terminate and print the final response
            server_response_buffer[bytes_received_from_server] = '\0';
            printf("Upload Result: %s\n", server_response_buffer); // Display final status to user
        }
    }
    else
    {
        printf("Upload Status: Failed due to client-side error during transfer.\n");
    }

    printf("DEBUG UPLOAD: Exiting 'uploadf' processing.\n");

} // End of execute_uploadf_command

/* ======================================================== */
/*          Execute Download Command Function               */
/* ======================================================== */
/**
 * @brief Handles the logic for the 'downlf' command.
 * Sends the command to S1, receives the initial response (checks for ERROR string first),
 * then potentially receives the file size (binary uint32_t), receives the file content,
 * and saves it locally.
 *
 * @param server_socket_fd The connected socket to the S1 server.
 * @param command_line The full command string (e.g., "downlf ~S1/path/file.ext").
 * @param s1_filepath The path of the file requested from S1 (e.g., "~S1/path/file.ext").
 */
void execute_download_command(int server_socket_fd, const char *command_line, const char *s1_filepath)
{
    printf("DEBUG DOWNLOAD: Starting 'downlf' process for '%s'\n", s1_filepath);

    // Variables needed
    char *local_filename = NULL;     // Name to save the file locally
    char initial_response_check[16]; // Small buffer to read initial bytes & check for ERROR
    char full_error_message[1024];   // Larger buffer if initial response IS an error
    ssize_t initial_bytes_read;      // Bytes read for initial check
    uint32_t received_file_size = 0; // File size reported by server (host byte order)
    FILE *local_file_handle = NULL;  // File pointer for writing locally
    int download_success = 0;        // Flag: 0=Fail, 1=Success

    /* 1. Extract local filename from the S1 path */
    // Use basename() to get the filename part (e.g., "file.ext" from "~S1/path/file.ext")
    // Note: basename might modify the input string, so use a copy if needed,
    // but here s1_filepath is const, so it should be safe or return internal pointer.
    // basename() is declared in <libgen.h>
    local_filename = basename((char *)s1_filepath); // Cast needed as basename expects char*
    if (local_filename == NULL || strlen(local_filename) == 0)
    {
        fprintf(stderr, "ERROR DOWNLOAD: Could not extract local filename from '%s'.\n", s1_filepath);
        // Cannot proceed without a filename to save as.
        // Don't communicate with server, just return.
        return;
    }
    printf("DEBUG DOWNLOAD: Will save file locally as: '%s'\n", local_filename);

    /* 2. Send the command to the S1 server */
    printf("DEBUG DOWNLOAD: Sending command to S1: '%s'\n", command_line);
    ssize_t bytes_sent_cmd = write(server_socket_fd, command_line, strlen(command_line));
    if (bytes_sent_cmd < (ssize_t)strlen(command_line))
    {
        perror("ERROR DOWNLOAD: Failed to send command to S1");
        return; // Cannot proceed if command sending fails
    }
    printf("DEBUG DOWNLOAD: Command sent.\n");

    /* 3. Receive the initial response (Size or ERROR) */
    printf("DEBUG DOWNLOAD: Waiting for initial response from S1 (Error or Binary Size...).\n");

    memset(initial_response_check, 0, sizeof(initial_response_check)); // Clear buffer
    // Try to read at least 5 bytes to check for "ERROR", but up to buffer size
    initial_bytes_read = read(server_socket_fd, initial_response_check, sizeof(initial_response_check) - 1);

    // Check read result
    if (initial_bytes_read < 0)
    {
        perror("ERROR DOWNLOAD: Failed to read initial response from S1");
        return; // Cannot proceed
    }
    if (initial_bytes_read == 0)
    {
        fprintf(stderr, "ERROR DOWNLOAD: S1 closed connection before sending response.\n");
        return; // Cannot proceed
    }
    // Null-terminate what was read
    initial_response_check[initial_bytes_read] = '\0';
    printf("DEBUG DOWNLOAD: Received initial %zd bytes from S1.\n", initial_bytes_read);

    // *** Check if the response starts with "ERROR" ***
    if (strncmp(initial_response_check, "ERROR", 5) == 0)
    {
        printf("DEBUG DOWNLOAD: Initial response starts with ERROR.\n");
        // Copy the initial part to the larger error buffer
        strncpy(full_error_message, initial_response_check, sizeof(full_error_message) - 1);
        full_error_message[sizeof(full_error_message) - 1] = '\0';
        size_t current_error_len = initial_bytes_read;

        // Optional: Try to read more of the error message if needed.
        // For simplicity, we'll just print what we initially got, which should include the core error.
        // A more robust solution might use non-blocking reads or select() here if errors are very long.
        fprintf(stderr, "Download failed: %s\n", full_error_message); // Print the error message received
        download_success = 0;
        goto download_cleanup; // Go to cleanup
    }
    // --- If it wasn't "ERROR", THEN check if it's a valid size header ---
    else if (initial_bytes_read == sizeof(uint32_t))
    {
        // Exactly 4 bytes received, and it wasn't "ERROR", assume binary size
        printf("DEBUG DOWNLOAD: Initial 4 bytes not ERROR, assuming size.\n");
        uint32_t network_size_from_s1;
        // Copy the 4 bytes read from the initial buffer into the uint32_t variable
        memcpy(&network_size_from_s1, initial_response_check, sizeof(uint32_t));
        // Convert network byte order to host byte order
        // ntohl() is declared in <arpa/inet.h>
        received_file_size = ntohl(network_size_from_s1);
        printf("DEBUG DOWNLOAD: Parsed binary file size: %u bytes.\n", received_file_size);

        // Sanity check the received size *after* converting to host order
        if (received_file_size > MAX_REASONABLE_FILE_SIZE)
        {
            // This block should ONLY execute if received_file_size is truly larger than MAX
            fprintf(stderr, "ERROR: Server reported unreasonably large file size: %u bytes. Aborting.\n", received_file_size);

            // Drain socket buffer before returning
            printf("DEBUG DOWNLOAD: Draining socket due to large size error...\n");
            char drain_buffer[1024];
            int flags = fcntl(server_socket_fd, F_GETFL, 0);
            if (flags != -1) fcntl(server_socket_fd, F_SETFL, flags | O_NONBLOCK);
            while (read(server_socket_fd, drain_buffer, sizeof(drain_buffer)) > 0); // Drain loop
            if (flags != -1) fcntl(server_socket_fd, F_SETFL, flags);
            printf("DEBUG DOWNLOAD: Socket drained.\n");

            return; // Stop processing this command
        }

        // Check if size is 0 (server indicates file not found or empty)
        if (received_file_size == 0)
        {
            printf("INFO: Server reported file size is 0 bytes. File not found on server or empty.\n");
            download_success = 0; // No content downloaded
            goto download_cleanup;
        }
        printf("INFO: Expecting to receive file of size %u bytes.\n", received_file_size);
        // Ready to receive content
    }
    else
    {
        // Received something, wasn't "ERROR", wasn't 4 bytes. Protocol violation.
        fprintf(stderr, "ERROR DOWNLOAD: Received unexpected initial response from S1 (Got %zd bytes, expected 4 for size or 'ERROR' string): '%.*s'\n",
                initial_bytes_read, (int)initial_bytes_read, initial_response_check);
        download_success = 0;
        goto download_cleanup;
    }

    /* 4. Open Local File for Writing */
    // This section only runs if received_file_size > 0
    printf("DEBUG DOWNLOAD: Opening local file '%s' for writing (binary)...\n", local_filename);
    local_file_handle = fopen(local_filename, "wb"); // Write binary mode
    if (local_file_handle == NULL)
    {
        perror("ERROR DOWNLOAD: Failed to open local file for writing");
        fprintf(stderr, "ERROR: Could not open '%s'. Check permissions.\n", local_filename);
        // Need to inform server? Or just close? Let's just close.
        // The caller loop will handle closing the socket.
        return; // Exit command processing
    }
    printf("DEBUG DOWNLOAD: Local file opened successfully.\n");

    /* 5. Receive File Content */
    char file_content_buffer[FILE_CHUNK_BUFFER_SIZE_CLIENT]; // Use client-defined chunk size
    size_t total_bytes_written = 0;
    ssize_t bytes_read_from_socket = 0;

    printf("DEBUG DOWNLOAD: Starting loop to receive %u bytes of content...\n", received_file_size);

    // Loop until the expected number of bytes are received
    while (total_bytes_written < received_file_size)
    {
        // Calculate how much more data we need
        size_t bytes_remaining = received_file_size - total_bytes_written;
        // Determine how much to read in this iteration (up to buffer size)
        size_t bytes_to_read_now = (bytes_remaining < sizeof(file_content_buffer)) ? bytes_remaining : sizeof(file_content_buffer);

        // Read data from the server socket
        bytes_read_from_socket = read(server_socket_fd, file_content_buffer, bytes_to_read_now);

        // Check read result
        if (bytes_read_from_socket < 0)
        {
            perror("ERROR DOWNLOAD: Error reading file content from S1");
            download_success = 0;  // Mark failure
            goto download_cleanup; // Go to cleanup
        }
        if (bytes_read_from_socket == 0)
        {
            fprintf(stderr, "ERROR DOWNLOAD: S1 closed connection unexpectedly during file transfer.\n");
            fprintf(stderr, "ERROR: Received %zu / %u bytes.\n", total_bytes_written, received_file_size);
            download_success = 0;  // Mark failure
            goto download_cleanup; // Go to cleanup
        }

        // Write the received chunk to the local file
        size_t bytes_written_this_chunk = fwrite(file_content_buffer, 1, bytes_read_from_socket, local_file_handle);

        // Check fwrite result
        if (bytes_written_this_chunk != (size_t)bytes_read_from_socket)
        {
            perror("ERROR DOWNLOAD: Error writing received data to local file");
            fprintf(stderr, "ERROR: Failed writing to '%s'. Disk full?\n", local_filename);
            download_success = 0;  // Mark failure
            goto download_cleanup; // Go to cleanup
        }

        // Update total bytes written
        total_bytes_written += bytes_written_this_chunk;

        // Optional: Print progress?
        // printf("DEBUG DOWNLOAD: Received %zu bytes (Total: %zu / %u)\n", bytes_written_this_chunk, total_bytes_written, received_file_size);
    }

    // If loop finished and we got here, check if expected size was received
    if (total_bytes_written == received_file_size)
    {
        printf("Download successful: '%s' (%u bytes received).\n", local_filename, received_file_size);
        download_success = 1; // Mark success
    }
    else
    {
        // This case should be caught by read errors/EOF earlier, but as a fallback
        fprintf(stderr, "ERROR DOWNLOAD: Size mismatch after loop! Expected %u, Got %zu.\n", received_file_size, total_bytes_written);
        download_success = 0;
    }

download_cleanup:
    /* 6. Cleanup */
    printf("DEBUG DOWNLOAD: Cleaning up download resources...\n");
    // Close the local file if it was opened
    if (local_file_handle != NULL)
    {
        if (fclose(local_file_handle) != 0)
        {
            perror("WARN DOWNLOAD: fclose failed for local file");
        }
        local_file_handle = NULL; // Avoid double close

        // If download failed *after* file was opened, remove the partial file
        if (!download_success)
        {
            printf("DEBUG DOWNLOAD: Download failed. Removing potentially partial file '%s'.\n", local_filename);
            // remove() is declared in <stdio.h> or <stdlib.h> depending on system
            // errno is declared in <errno.h>
            if (remove(local_filename) != 0 && errno != ENOENT)
            { // Ignore if already gone
                perror("WARN DOWNLOAD: Failed to remove partial download file");
            }
        }
    }
    // If download_success is 0 AND local_file_handle is NULL (meaning we jumped from the size 0 check or ERROR check)
    else if (!download_success && received_file_size == 0)
    {
        // Print a more specific message for the file-not-found case (where size was 0)
        // Avoid printing this if an ERROR string was received (which would have already printed a message)
        if (strncmp(initial_response_check, "ERROR", 5) != 0)
        { // Check we didn't already print an ERROR
            printf("File not found on server or is empty.\n");
        }
    }

    // The server connection (server_socket_fd) will be closed by the caller (main loop)
    printf("DEBUG DOWNLOAD: Exiting 'downlf' processing.\n");

} // End execute_download_command

/**
 * @brief Processes the 'removef' command. Sends command, reads simple response.
 *
 * @param server_socket_fd Socket descriptor connected to S1.
 * @param server_filepath Path of the file to remove on the server (starts with ~S1/).
 */
void execute_removef_command(int server_socket_fd, char *server_filepath)
{
    char command_to_server_buffer[USER_COMMAND_BUFFER_SIZE];
    char server_status_response[SERVER_RESPONSE_BUFFER_SIZE];
    ssize_t bytes_written_cmd;
    ssize_t bytes_read_resp;

    printf("DEBUG REMOVE: Starting 'removef' process for '%s'\n", server_filepath);

    /* 1. Construct and Send Command */
    snprintf(command_to_server_buffer, sizeof(command_to_server_buffer), "removef %s", server_filepath);
    printf("DEBUG REMOVE: Sending command to S1: '%s'\n", command_to_server_buffer);
    bytes_written_cmd = write(server_socket_fd, command_to_server_buffer, strlen(command_to_server_buffer));
    // Check write result
    if (bytes_written_cmd < (ssize_t)strlen(command_to_server_buffer))
    {
        perror("ERROR: Failed sending command to S1");
        return; // Cannot proceed
    }
    printf("DEBUG REMOVE: Command sent.\n");

    /* 2. Read Server Response */
    printf("DEBUG REMOVE: Waiting for response from S1...\n");
    memset(server_status_response, 0, sizeof(server_status_response)); // Clear buffer
    bytes_read_resp = read(server_socket_fd, server_status_response, sizeof(server_status_response) - 1);
    // Check read result
    if (bytes_read_resp <= 0)
    {
        if (bytes_read_resp == 0)
            fprintf(stderr, "ERROR: S1 closed connection before sending remove status.\n");
        else
            perror("ERROR: Failed reading response from S1");
        return; // Cannot determine status
    }
    // Null-terminate and print the response
    server_status_response[bytes_read_resp] = '\0';
    printf("Remove Result: %s\n", server_status_response); // Display result to user

    printf("DEBUG REMOVE: Exiting 'removef' processing.\n");

} // End of execute_removef_command

/**
 * @brief Processes the 'downltar' command. Connects to S1, sends command,
 * reads initial response (checking for ERROR string first), then reads size
 * (binary uint32_t), reads content, and saves tar file locally.
 *
 * @param server_socket_fd Socket descriptor connected to S1.
 * @param requested_file_type The type of files for the tarball (".c", ".pdf", or ".txt").
 */
void execute_downltar_command(int server_socket_fd, char *requested_file_type)
{
    char command_string_for_s1[USER_COMMAND_BUFFER_SIZE];
    char local_tar_output_filename[20]; // e.g., "cfiles.tar", "pdf.tar", "text.tar"
    char receive_data_buffer[FILE_TRANSFER_CHUNK_SIZE];
    ssize_t bytes_received;
    ssize_t bytes_sent_cmd;
    FILE *local_tar_file_output = NULL;
    uint32_t expected_tar_size = 0; // S1 sends uint32_t size for tar
    size_t content_start_offset = 0;
    int download_tar_failed = 0;        // 0=OK, 1=Failed
    size_t total_bytes_written_tar = 0; // Initialize counter

    printf("DEBUG TAR: Starting 'downltar' process for type '%s'\n", requested_file_type);

    /* 1. Determine Local Output Filename */
    if (strcmp(requested_file_type, ".c") == 0)
    {
        strcpy(local_tar_output_filename, "cfiles.tar");
    }
    else if (strcmp(requested_file_type, ".pdf") == 0)
    {
        strcpy(local_tar_output_filename, "pdf.tar");
    }
    else if (strcmp(requested_file_type, ".txt") == 0)
    {
        strcpy(local_tar_output_filename, "text.tar");
    }
    else
    {
        fprintf(stderr, "ERROR: Invalid file type '%s' passed to downltar handler.\n", requested_file_type);
        strcpy(local_tar_output_filename, "unknown.tar");
    }
    printf("DEBUG TAR: Local output filename set to '%s'.\n", local_tar_output_filename);

    /* 2. Construct and Send Command */
    snprintf(command_string_for_s1, sizeof(command_string_for_s1), "downltar %s", requested_file_type);
    printf("DEBUG TAR: Sending command to S1: '%s'\n", command_string_for_s1);
    bytes_sent_cmd = write(server_socket_fd, command_string_for_s1, strlen(command_string_for_s1));
    if (bytes_sent_cmd < (ssize_t)strlen(command_string_for_s1))
    {
        perror("ERROR: Failed sending command to S1");
        return;
    }
    printf("DEBUG TAR: Command sent.\n");

    /* 3. Read Initial Response (Error or Size + Content) */
    printf("DEBUG TAR: Waiting for initial response from S1...\n");
    memset(receive_data_buffer, 0, sizeof(receive_data_buffer));
    bytes_received = read(server_socket_fd, receive_data_buffer, sizeof(receive_data_buffer) - 1);
    if (bytes_received <= 0)
    {
        if (bytes_received == 0)
            fprintf(stderr, "ERROR: S1 closed connection before sending response.\n");
        else
            perror("ERROR: Failed reading initial response from S1");
        return;
    }
    receive_data_buffer[bytes_received] = '\0'; // Null-terminate
    printf("DEBUG TAR: Received initial %zd bytes from S1.\n", bytes_received);

    /* --- FIX: Check for ERROR Message String FIRST --- */
    // Check if the response *starts with* "ERROR"
    if (strncmp(receive_data_buffer, "ERROR", 5) == 0)
    {
        // Server sent an error message string. Print it and exit.
        fprintf(stderr, "Download failed. Server response: %s\n", receive_data_buffer);
        // Optional: attempt to read any remaining data on the socket?
        // while(read(server_socket_fd, receive_data_buffer, sizeof(receive_data_buffer)-1) > 0);
        return; // Exit function, server reported error
    }
    /* --- END FIX --- */

    /* 5. Extract File Size (uint32_t) - ONLY if not an ERROR */
    printf("DEBUG TAR: Initial response not ERROR, parsing size...\n"); // Added clarification
    if (bytes_received >= (ssize_t)sizeof(uint32_t))
    {
        memcpy(&expected_tar_size, receive_data_buffer, sizeof(expected_tar_size));
        content_start_offset = sizeof(expected_tar_size);
        printf("DEBUG TAR: Parsed binary tar file size: %u bytes.\n", expected_tar_size);
    }
    else
    {
        // Received data, wasn't "ERROR", but not enough for size. Protocol error.
        fprintf(stderr, "ERROR: Received incomplete initial response from S1 (Got %zd bytes, expected %zu for size).\n", bytes_received, sizeof(uint32_t));
        return; // Cannot proceed
    }

    // Reasonableness check (e.g., max 100MB)
    size_t MAX_TAR_SIZE = 100 * 1024 * 1024; // 100 MiB
    if (expected_tar_size == 0)
    {
        printf("INFO: Server reported tar file size is 0 bytes. No files found?\n");
        // Create empty file locally
    }
    else if (expected_tar_size > MAX_TAR_SIZE)
    {
        fprintf(stderr, "ERROR: Server reported unreasonably large tar size: %u bytes. Aborting.\n", expected_tar_size);
        // Consider reading/discarding data S1 might send, or just close. Closing is simpler for client.
        return;
    }
    printf("INFO: Expecting tar file of size %u bytes.\n", expected_tar_size);

    /* 6. Open Local Output File */
    printf("DEBUG TAR: Opening local file '%s' for writing (binary)...\n", local_tar_output_filename);
    local_tar_file_output = fopen(local_tar_output_filename, "wb"); // Write binary
    if (local_tar_file_output == NULL)
    {
        perror("ERROR: Failed creating local output tar file");
        fprintf(stderr, "ERROR: Cannot open '%s' for writing.\n", local_tar_output_filename);
        // Consider reading/discarding data S1 might send.
        return; // Cannot proceed
    }
    printf("DEBUG TAR: Local file opened okay.\n");

    /* 7. Write Initial Content (if any) */
    size_t initial_tar_content_bytes = 0;
    if (bytes_received > (ssize_t)content_start_offset)
    {
        initial_tar_content_bytes = bytes_received - content_start_offset;
        // Check for overflow/truncation
        if (initial_tar_content_bytes > expected_tar_size && expected_tar_size > 0)
        {
            fprintf(stderr, "WARN: Initial content (%zu) exceeds expected size (%u)! Truncating write.\n",
                    initial_tar_content_bytes, expected_tar_size);
            initial_tar_content_bytes = expected_tar_size;
        }
        printf("DEBUG TAR: Writing initial %zu tar content bytes...\n", initial_tar_content_bytes);
        size_t written_initial = fwrite(receive_data_buffer + content_start_offset, 1, initial_tar_content_bytes, local_tar_file_output);
        if (written_initial < initial_tar_content_bytes)
        {
            perror("ERROR: Failed writing initial tar content to local file");
            download_tar_failed = 1; // Mark failure
        }
        total_bytes_written_tar = written_initial; // Initialize total written
    }
    else
    {
        printf("DEBUG TAR: No tar content in initial response.\n");
        total_bytes_written_tar = 0; // Initialize total written
    }

    /* 8. Receive Remaining Tar Content */
    printf("DEBUG TAR: Starting loop to receive remaining tar content...\n");

    // Loop only if initial write OK and more data expected
    while (download_tar_failed == 0 && total_bytes_written_tar < expected_tar_size)
    {
        // Calculate remaining
        size_t remaining_tar_bytes = expected_tar_size - total_bytes_written_tar;
        size_t read_tar_amount = (remaining_tar_bytes < sizeof(receive_data_buffer)) ? remaining_tar_bytes : sizeof(receive_data_buffer);

        bytes_received = read(server_socket_fd, receive_data_buffer, read_tar_amount); // Reuse buffer

        // Check read result
        if (bytes_received < 0)
        { // Error
            if (errno == EINTR)
            {
                printf("DEBUG TAR: read() interrupted, continue loop.\n");
                continue;
            }
            perror("ERROR: Read error while receiving tar content");
            download_tar_failed = 1;
            break; // Exit loop
        }
        if (bytes_received == 0)
        { // Disconnect
            fprintf(stderr, "ERROR: Server closed connection before sending entire tar file (Received %zu/%u bytes).\n",
                    total_bytes_written_tar, expected_tar_size);
            download_tar_failed = 1;
            break; // Exit loop
        }

        // Write received chunk
        size_t bytes_written_this_tar_loop = fwrite(receive_data_buffer, 1, bytes_received, local_tar_file_output);
        // Check fwrite result
        if (bytes_written_this_tar_loop < (size_t)bytes_received)
        {
            perror("ERROR: Failed writing tar content chunk to local file");
            download_tar_failed = 1;
            break; // Exit loop
        }
        // Update total
        total_bytes_written_tar += bytes_written_this_tar_loop;
        printf("DEBUG TAR: Received %zu / %u bytes\n", total_bytes_written_tar, expected_tar_size);

    } // End of tar content receiving loop

    /* 9. Finalize and Cleanup */
    printf("DEBUG TAR: Closing local tar file handle.\n");
    if (local_tar_file_output != NULL)
    {
        if (fclose(local_tar_file_output) != 0)
        {
            perror("WARN: Error closing local output tar file");
        }
        local_tar_file_output = NULL; // Mark closed
    }

    // Check final status and print message
    if (download_tar_failed == 0 && total_bytes_written_tar == expected_tar_size)
    {
        printf("Tar file download successful: '%s' (%zu bytes).\n", local_tar_output_filename, total_bytes_written_tar);
    }
    else if (download_tar_failed == 0 && total_bytes_written_tar < expected_tar_size)
    {
        // This case should ideally not be reached if size=0 handled or loop exit worked
        fprintf(stderr, "Tar download failed: Incomplete file received (%zu/%u bytes).\n", total_bytes_written_tar, expected_tar_size);
    }
    else
    {
        // download_tar_failed was set
        fprintf(stderr, "Tar download failed due to transfer error. Partial file '%s' may exist.\n", local_tar_output_filename);
        // Optional: remove(local_tar_output_filename); // Remove partial file on error
    }

    printf("DEBUG TAR: Exiting 'downltar' processing.\n");

} // End of execute_downltar_command (Corrected)

/**
 * @brief Processes the 'dispfnames' command. Sends command, reads response until
 * server closes connection, then prints the received list.
 *
 * @param server_socket_fd Socket descriptor connected to S1.
 * @param server_directory_path Pathname on the server for which to list files (starts with ~S1/).
 */
void execute_dispfnames_command(int server_socket_fd, char *server_directory_path)
{
    char command_for_server[USER_COMMAND_BUFFER_SIZE];
    // Allocate buffer dynamically or use a large static buffer. Static is simpler for now.
    char server_file_list_response[MAX_FILENAME_LIST_SIZE]; // Matches define used in original code
    ssize_t bytes_written_cmd;
    ssize_t bytes_read_resp_chunk;
    size_t total_response_bytes_received = 0;
    int disp_op_failed = 0; // 0=OK, 1=Fail

    printf("DEBUG DISP: Starting 'dispfnames' process for path '%s'\n", server_directory_path);

    /* 1. Construct and Send Command */
    snprintf(command_for_server, sizeof(command_for_server), "dispfnames %s", server_directory_path);
    printf("DEBUG DISP: Sending command to S1: '%s'\n", command_for_server);
    bytes_written_cmd = write(server_socket_fd, command_for_server, strlen(command_for_server));
    // Check write result
    if (bytes_written_cmd < (ssize_t)strlen(command_for_server))
    {
        perror("ERROR: Failed writing command to S1 server");
        return; // Cannot proceed
    }
    printf("DEBUG DISP: Command sent.\n");

    /* 2. Read Server Response Loop (until connection close) */
    printf("DEBUG DISP: Waiting for server response (read until connection close)...\n");
    // Clear the response buffer before reading
    memset(server_file_list_response, 0, sizeof(server_file_list_response));

    // Loop to read chunks from server until error or connection close (read returns 0)
    while (total_response_bytes_received < sizeof(server_file_list_response) - 1) // Check buffer space
    {
        // Read into the buffer at the current offset
        bytes_read_resp_chunk = read(server_socket_fd,
                                     server_file_list_response + total_response_bytes_received,
                                     sizeof(server_file_list_response) - 1 - total_response_bytes_received);

        // Check read result
        if (bytes_read_resp_chunk < 0)
        { // Read Error
            perror("ERROR: Failed reading response from S1 server");
            disp_op_failed = 1;
            break; // Exit loop
        }
        if (bytes_read_resp_chunk == 0)
        { // Connection Closed by S1 (End Of File / End Of List)
            printf("DEBUG DISP: S1 server closed connection. End of file list received.\n");
            break; // Exit loop, successfully received all data
        }

        // Successfully read a chunk, update total bytes
        total_response_bytes_received += bytes_read_resp_chunk;
        printf("DEBUG DISP: Received %zd bytes chunk (total %zu bytes so far).\n", bytes_read_resp_chunk, total_response_bytes_received);
    }

    // Ensure null termination (redundant due to initial memset, but safe)
    server_file_list_response[total_response_bytes_received] = '\0';

    /* 3. Check for Buffer Overflow */
    // If loop finished because buffer was full, print a warning.
    if (total_response_bytes_received >= sizeof(server_file_list_response) - 1 && bytes_read_resp_chunk > 0)
    {
        fprintf(stderr, "WARNING: File list response buffer is full (%zu bytes). List might be truncated.\n", sizeof(server_file_list_response));
        // Consider increasing MAX_FILENAME_LIST_SIZE if this is a common issue.
    }

    /* 4. Process and Print Response */
    // Check if any error occurred during read
    if (disp_op_failed == 1)
    {
        fprintf(stderr, "Could not completely retrieve file list due to read error.\n");
    }
    // Check if response is empty
    else if (total_response_bytes_received == 0)
    {
        // S1 likely closed connection immediately (e.g., path not found error) or path was empty.
        printf("Files found in %s:\n(No files found or path does not exist on server)\n", server_directory_path);
    }
    // Check if response starts with "ERROR" (S1 might send this for invalid path etc.)
    else if (strncmp(server_file_list_response, "ERROR", 5) == 0)
    {
        printf("Server Error: %s\n", server_file_list_response);
    }
    else
    {
        // Successfully received list, print it
        printf("Files found in %s:\n%s", server_directory_path, server_file_list_response); // Response likely has newlines
        // Ensure a final newline for cleaner terminal output if needed
        if (server_file_list_response[total_response_bytes_received - 1] != '\n')
        {
            printf("\n");
        }
    }

    // Note: S1 closes the connection in this command, so client doesn't need to close it here.
    printf("DEBUG DISP: Exiting 'dispfnames' processing (connection closed by S1).\n");

} // End of execute_dispfnames_command
