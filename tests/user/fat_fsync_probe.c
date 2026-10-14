/* Guest-only FAT write/sync/read probe. Never execute on the host. */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    const char *path = "/fsync-qemu-qa";
    const char payload[] = "FAT fsync identity probe";
    char received[sizeof(payload)];
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    int result = 1;
    if (fd < 0) {
        (void)write(1, "FAT FSYNC CREATE FAIL\n", 22);
        return result;
    }
    if (write(fd, payload, sizeof(payload)) == (ssize_t)sizeof(payload) &&
        fsync(fd) == 0 && lseek(fd, 0, SEEK_SET) == 0 &&
        read(fd, received, sizeof(received)) == (ssize_t)sizeof(received) &&
        memcmp(payload, received, sizeof(payload)) == 0) result = 0;
    if (close(fd) != 0) result = 1;
    if (unlink(path) != 0) result = 1;
    const char *message = result ? "FAT FSYNC FAIL\n" : "FAT FSYNC PASS\n";
    (void)write(1, message, strlen(message));
    return result;
}
