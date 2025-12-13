#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define DEVICE_PATH "/dev/mydma0"
#define TEST_STRING "Hello DMA Loopback! This is the final test."

// ANSI escape codes for colored output
#define ANSI_COLOR_ORANGE  "\x1b[93m" // For sent data (Bright Yellow)
#define ANSI_COLOR_CYAN    "\x1b[36m" // For received data
#define ANSI_COLOR_GREEN   "\x1b[32m" // For success
#define ANSI_COLOR_RED     "\x1b[31m" // For error/failure
#define ANSI_COLOR_RESET   "\x1b[0m"  // Reset to default color

int main() {
    int fd;
    ssize_t bytes_written, bytes_read;
    const char *write_buf = TEST_STRING;
    size_t write_len = strlen(write_buf) + 1; // +1 to include null terminator
    char *read_buf = malloc(write_len);

    if (!read_buf) {
        perror(ANSI_COLOR_RED "Failed to allocate read buffer" ANSI_COLOR_RESET);
        return EXIT_FAILURE;
    }

    // 1. 打开设备
    printf("Opening device: %s\n", DEVICE_PATH);
    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror(ANSI_COLOR_RED "Failed to open device" ANSI_COLOR_RESET);
        free(read_buf);
        return EXIT_FAILURE;
    }
    printf("Device opened successfully.\n\n");

    // 2. 写入数据以触发DMA
    printf(ANSI_COLOR_ORANGE "Sent:\n  \"%s\"" ANSI_COLOR_RESET " (%zu bytes)\n", write_buf, write_len);
    bytes_written = write(fd, write_buf, write_len);
    if (bytes_written < 0) {
        perror(ANSI_COLOR_RED "Failed to write to device" ANSI_COLOR_RESET);
        close(fd);
        free(read_buf);
        return EXIT_FAILURE;
    }
    printf("Write completed.\n\n");

    // 3. 读取数据以获取DMA结果
    printf("Reading %zu bytes from device...\n", write_len);
    bytes_read = read(fd, read_buf, write_len);
    if (bytes_read < 0) {
        perror(ANSI_COLOR_RED "Failed to read from device" ANSI_COLOR_RESET);
        close(fd);
        free(read_buf);
        return EXIT_FAILURE;
    }
    printf(ANSI_COLOR_CYAN "Received:\n  \"%s\"" ANSI_COLOR_RESET " (%zd bytes)\n\n", read_buf, bytes_read);

    // 4. 比较结果
    printf("Comparing sent and received data...\n");
    if (bytes_read == write_len && strcmp(write_buf, read_buf) == 0) {
        printf(ANSI_COLOR_GREEN "\nSUCCESS: Data loopback test passed!" ANSI_COLOR_RESET "\n");
    } else {
        fprintf(stderr, ANSI_COLOR_RED "\nFAILURE: Data mismatch!" ANSI_COLOR_RESET "\n");
        fprintf(stderr, "  Expected: " ANSI_COLOR_ORANGE "\"%s\"" ANSI_COLOR_RESET "\n", write_buf);
        fprintf(stderr, "  Received: " ANSI_COLOR_CYAN "\"%s\"" ANSI_COLOR_RESET "\n", read_buf);
    }

    // 5. 清理
    free(read_buf);
    close(fd);

    return EXIT_SUCCESS;
}