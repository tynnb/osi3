#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <errno.h>
#include <sys/stat.h>

#define SHM_SIZE 4096

static char CHILD_PROCESS_NAME[] = "child";

char SHM_NAME[64];
char SEM_NAME[64];

bool is_valid_input(const char *input, ssize_t len) {
    if (len == 0) return false;
    
    for (ssize_t i = 0; i < len; i++) {
        if (input[i] == '\n') continue;
        if (!isdigit(input[i]) && input[i] != ' ' && input[i] != '\t' && input[i] != '-') {
            return false;
        }
    }
    return true;
}

bool is_empty_input(const char *input, ssize_t len) {
    for (ssize_t i = 0; i < len; i++) {
        if (input[i] != ' ' && input[i] != '\t' && input[i] != '\n' && input[i] != '\r') {
            return false;
        }
    }
    return true;
}

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
    (void)argv;
    
    if (argc != 1) {
        const char msg[] = "Usage: ./parent\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        exit(EXIT_FAILURE);
    }

    pid_t pid = getpid();
    snprintf(SHM_NAME, sizeof(SHM_NAME), "/div_shm_%d", pid);
    snprintf(SEM_NAME, sizeof(SEM_NAME), "/div_sem_%d", pid);

    const char filename_prompt[] = "Введите имя файла: ";
    write_checked(STDOUT_FILENO, filename_prompt, sizeof(filename_prompt) - 1);
    
    char filename[256];
    ssize_t bytes = read(STDIN_FILENO, filename, sizeof(filename) - 1);
    if (bytes <= 0) {
        const char msg[] = "error: failed to read filename\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        exit(EXIT_FAILURE);
    }
    
    if (bytes > 0 && filename[bytes - 1] == '\n') {
        filename[bytes - 1] = '\0';
    } else {
        filename[bytes] = '\0';
    }

    int shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (shm_fd == -1) {
        const char msg[] = "error: failed to create shared memory\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        exit(EXIT_FAILURE);
    }

    if (ftruncate(shm_fd, SHM_SIZE) == -1) {
        const char msg[] = "error: failed to resize shared memory\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        cleanup_resources(shm_fd, NULL, NULL);
        exit(EXIT_FAILURE);
    }

    char *shm_buf = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_buf == MAP_FAILED) {
        const char msg[] = "error: failed to map shared memory\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        cleanup_resources(shm_fd, NULL, NULL);
        exit(EXIT_FAILURE);
    }

    uint32_t *shm_length = (uint32_t *)shm_buf;
    char *shm_data = shm_buf + sizeof(uint32_t);

    *shm_length = 0;
    memset(shm_data, 0, SHM_SIZE - sizeof(uint32_t));

    sem_t *sem = sem_open(SEM_NAME, O_CREAT | O_EXCL, 0600, 1);
    if (sem == SEM_FAILED) {
        const char msg[] = "error: failed to create semaphore\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        cleanup_resources(shm_fd, sem, shm_buf);
        exit(EXIT_FAILURE);
    }

    const pid_t child = fork();

    switch (child) {
    case -1: {
        const char msg[] = "error: failed to spawn new process\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        cleanup_resources(shm_fd, sem, shm_buf);
        shm_unlink(SHM_NAME);
        sem_unlink(SEM_NAME);
        exit(EXIT_FAILURE);
    } break;

    case 0: {
        close(shm_fd);
        sem_close(sem);
        munmap(shm_buf, SHM_SIZE);

        char *const args[] = {CHILD_PROCESS_NAME, filename, SHM_NAME, SEM_NAME, NULL};
        execv(CHILD_PROCESS_NAME, args);

        const char msg[] = "error: failed to exec into child\n";
        write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
        exit(EXIT_FAILURE);
    } break;
    
    default: {
        {
            char msg[128];
            const int32_t length = snprintf(msg, sizeof(msg), "Дочерний процесс создан с PID: %d\n", child);
            write_checked(STDOUT_FILENO, msg, length);
        }

        const char info[] = "Вводите числа через пробел (например: 100 2 5)\n";
        write_checked(STDOUT_FILENO, info, sizeof(info) - 1);

        char buf[4096];
        int child_exited = 0;

        usleep(50000);

        while (!child_exited) {
            sem_wait(sem);
            if (*shm_length > 0) {
                if (*shm_length == UINT32_MAX) {
                    child_exited = 1;
                    *shm_length = 0;
                    sem_post(sem);
                    break;
                }
                
                write_checked(STDOUT_FILENO, shm_data, *shm_length);
                
                if (strstr(shm_data, "Processes terminating") != NULL) {
                    child_exited = 1;
                    *shm_length = 0;
                    sem_post(sem);
                    break;
                }
                
                *shm_length = 0;
            }
            sem_post(sem);

            int status;
            pid_t result = waitpid(child, &status, WNOHANG);
            if (result != 0) {
                child_exited = 1;
                break;
            }

            const char prompt[] = "> ";
            write_checked(STDOUT_FILENO, prompt, sizeof(prompt) - 1);

            bytes = read(STDIN_FILENO, buf, sizeof(buf));
            if (bytes < 0) {
                const char msg[] = "error: failed to read from stdin\n";
                write_checked(STDERR_FILENO, msg, sizeof(msg) - 1);
                break;
            }
            else if (bytes == 0) {
                child_exited = 1;
                break;
            }

            if (is_empty_input(buf, bytes)) {
                const char msg[] = "Введена пустая строка. Завершение работы.\n";
                write_checked(STDOUT_FILENO, msg, sizeof(msg) - 1);
                child_exited = 1;
                break;
            }

            if (!is_valid_input(buf, bytes)) {
                const char msg[] = "Ошибка: ввод содержит недопустимые символы. Завершение работы.\n";
                write_checked(STDOUT_FILENO, msg, sizeof(msg) - 1);
                child_exited = 1;
                break;
            }

            sem_wait(sem);
            *shm_length = bytes;
            memcpy(shm_data, buf, bytes);
            sem_post(sem);

            usleep(10000);
        }

        sem_wait(sem);
        *shm_length = UINT32_MAX;
        sem_post(sem);

        waitpid(child, NULL, 0);

        cleanup_resources(shm_fd, sem, shm_buf);
        shm_unlink(SHM_NAME);
        sem_unlink(SEM_NAME);

        const char msg[] = "Родительский процесс завершен\n";
        write_checked(STDOUT_FILENO, msg, sizeof(msg) - 1);
    } break;
    }

    return 0;
}