#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#define SHM_SIZE 4096

void cleanup_resources(int shm_fd, sem_t *sem, void *shm_buf) {
    if (shm_buf != NULL && shm_buf != MAP_FAILED) {
        munmap(shm_buf, SHM_SIZE);
    }
    if (sem != NULL && sem != SEM_FAILED) {
        sem_close(sem);
    }
    if (shm_fd != -1) {
        close(shm_fd);
    }
}

void write_checked(int fd, const char *msg, size_t len) {
    ssize_t result = write(fd, msg, len);
    (void)result;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        const char msg[] = "Usage: child <filename> <shm_name> <sem_name>\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        exit(EXIT_FAILURE);
    }

    const char *filename = argv[1];
    const char *shm_name = argv[2];
    const char *sem_name = argv[3];

    int32_t file = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file == -1) {
        const char msg[] = "error: failed to open file\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        exit(EXIT_FAILURE);
    }

    const char header[] = "==Results of division==\n";
    write_checked(file, header, sizeof(header) - 1);

    {
        char msg[128];
        int len = snprintf(msg, sizeof(msg), "Child process started. File: %s\n", filename);
        write_checked(STDOUT_FILENO, msg, len);
    }

    int shm_fd = shm_open(shm_name, O_RDWR, 0);
    if (shm_fd == -1) {
        const char msg[] = "error: failed to open shared memory\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        close(file);
        exit(EXIT_FAILURE);
    }

    char *shm_buf = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_buf == MAP_FAILED) {
        const char msg[] = "error: failed to map shared memory\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        close(shm_fd);
        close(file);
        exit(EXIT_FAILURE);
    }

    uint32_t *shm_length = (uint32_t *)shm_buf;
    char *shm_data = shm_buf + sizeof(uint32_t);

    sem_t *sem = sem_open(sem_name, O_RDWR);
    if (sem == SEM_FAILED) {
        const char msg[] = "error: failed to open semaphore\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        cleanup_resources(shm_fd, sem, shm_buf);
        close(file);
        exit(EXIT_FAILURE);
    }

    bool running = true;

    while (running) {
        sem_wait(sem);
        
        if (*shm_length == UINT32_MAX) {
            running = false;
            *shm_length = 0;
            sem_post(sem);
            break;
        }
        
        if (*shm_length > 0) {
            char *input = shm_data;
            ssize_t bytes = *shm_length;
            
            int *numbers = NULL;
            int capacity = 10;
            int count = 0;
            
            numbers = malloc(capacity * sizeof(int));
            if (numbers == NULL) {
                const char msg[] = "error: memory allocation failed\n";
                write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
                *shm_length = 0;
                sem_post(sem);
                exit(EXIT_FAILURE);
            }

            int i = 0;
            while (i < bytes) {
                while (i < bytes && (input[i] == ' ' || input[i] == '\t' || input[i] == '\n' || input[i] == '\r')) {
                    i++;
                }
                
                if (i >= bytes) {
                    break;
                }

                char *endptr;
                long num = strtol(&input[i], &endptr, 10);
                
                if (endptr != &input[i]) {
                    if (count >= capacity) {
                        capacity *= 2;
                        int *new_numbers = realloc(numbers, capacity * sizeof(int));
                        if (new_numbers == NULL) {
                            const char msg[] = "error: memory reallocation failed\n";
                            write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
                            free(numbers);
                            *shm_length = 0;
                            sem_post(sem);
                            exit(EXIT_FAILURE);
                        }
                        numbers = new_numbers;
                    }
                    
                    numbers[count++] = (int)num;
                    i = endptr - input;
                } else {
                    i++;
                }
            }

            if (count < 2) {
                const char msg[] = "error: need at least 2 numbers\n";
                sem_post(sem);
                
                sem_wait(sem);
                strcpy(shm_data, msg);
                *shm_length = strlen(msg);
                sem_post(sem);
                
                free(numbers);
                continue;
            }

            int op_capacity = 256;
            char *operation = malloc(op_capacity);
            if (operation == NULL) {
                const char msg[] = "error: memory allocation failed\n";
                write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
                free(numbers);
                *shm_length = 0;
                sem_post(sem);
                exit(EXIT_FAILURE);
            }
            
            int op_len = snprintf(operation, op_capacity, "Operation: %d", numbers[0]);
            double result = (double)numbers[0];

            for (int i = 1; i < count; i++) {
                if (numbers[i] == 0) {
                    int needed = snprintf(NULL, 0, " / %d", numbers[i]);
                    if (op_len + needed >= op_capacity) {
                        op_capacity = op_len + needed + 64;
                        char *new_operation = realloc(operation, op_capacity);
                        if (new_operation == NULL) {
                            const char msg[] = "error: memory reallocation failed\n";
                            write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
                            free(operation);
                            free(numbers);
                            *shm_length = 0;
                            sem_post(sem);
                            exit(EXIT_FAILURE);
                        }
                        operation = new_operation;
                    }
                    op_len += snprintf(operation + op_len, op_capacity - op_len, " / %d", numbers[i]);

                    write_checked(file, operation, op_len);
                    const char error_msg[] = " = error: division by zero\n";
                    write_checked(file, error_msg, sizeof(error_msg) - 1);

                    const char response[] = "error: division by zero. Processes terminating\n";
                    
                    sem_post(sem);
                    
                    sem_wait(sem);
                    strcpy(shm_data, response);
                    *shm_length = strlen(response);
                    sem_post(sem);

                    free(operation);
                    free(numbers);
                    close(file);
                    cleanup_resources(shm_fd, sem, shm_buf);
                    exit(EXIT_FAILURE);
                }
                
                int needed = snprintf(NULL, 0, " / %d", numbers[i]);
                if (op_len + needed >= op_capacity) {
                    op_capacity = op_len + needed + 64;
                    char *new_operation = realloc(operation, op_capacity);
                    if (new_operation == NULL) {
                        const char msg[] = "error: memory reallocation failed\n";
                        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
                        free(operation);
                        free(numbers);
                        *shm_length = 0;
                        sem_post(sem);
                        exit(EXIT_FAILURE);
                    }
                    operation = new_operation;
                }
                op_len += snprintf(operation + op_len, op_capacity - op_len, " / %d", numbers[i]);
                
                result /= numbers[i];
            }

            char result_str[64];
            snprintf(result_str, sizeof(result_str), " = %.6f\n", result);
            
            write_checked(file, operation, op_len);
            write_checked(file, result_str, strlen(result_str));

            char response[128];
            snprintf(response, sizeof(response), "result: %.6f (written to file)\n", result);
            
            *shm_length = 0;
            sem_post(sem);
            
            sem_wait(sem);
            strcpy(shm_data, response); 
            *shm_length = strlen(response);
            sem_post(sem);
            
            fsync(STDOUT_FILENO);

            free(operation);
            free(numbers);
        } else {
            sem_post(sem);
        }
        
        usleep(10000);
    }

    close(file);
    cleanup_resources(shm_fd, sem, shm_buf);
    return 0;
}