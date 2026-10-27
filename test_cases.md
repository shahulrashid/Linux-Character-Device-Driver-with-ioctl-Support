# Test cases

All cases below are automated in `test_app --test` (run `sudo ./tests/run_tests.sh`),
except the "Manual" section, which needs a shell.

| # | Scenario | Action | Expected result |
|---|----------|--------|-----------------|
| 1 | Open / close | `open("/dev/mychardev")` | valid fd |
| 2 | Round trip | write `Hello Linux` (+NUL), `lseek(0)`, read | write returns 12, read returns same 12 bytes |
| 3 | Length query | `ioctl GET_LENGTH` after #2 | 12 |
| 4 | EOF | read again after #2 | returns 0 |
| 5 | Empty buffer | `CLEAR_BUFFER`, read | returns 0 |
| 6 | Zero-length write | `write(fd, "", 0)` | returns 0 |
| 7 | Partial read | 10 bytes stored, read 4+4+4 | 4, 4, 2, then 0 |
| 8 | Capacity | `ioctl GET_CAPACITY` | > 0 (default 1024) |
| 9 | Oversized write | write capacity+100 bytes | returns capacity (partial write) |
| 10 | Full buffer | write again | -1, `ENOSPC` |
| 11 | lseek END | after writing 2 bytes | returns 2 |
| 12 | lseek out of range | offset > capacity, or negative | -1, `EINVAL` |
| 13 | Append | write `AB`, reopen with `O_APPEND`, write `CD` | read gives `ABCD` |
| 14 | Wrong ioctl magic | `_IO('x',0)` | -1, `ENOTTY` |
| 15 | Unknown ioctl number | `_IO('k',99)` | -1, `ENOTTY` |
| 16 | Bad ioctl pointer | `GET_LENGTH` with NULL | -1, `EFAULT` |
| 17 | Bad write pointer | `write(fd, NULL, 10)` | -1, `EFAULT` |
| 18 | Bad read pointer | `read(fd, NULL, 3)` with data present | -1, `EFAULT` |
| 19 | Concurrency | 4 threads x 20 000 `pwrite`/`pread` of 64-byte uniform blocks | every block read back is uniform (no torn data) |

## Manual

| Scenario | Command | Expected |
|----------|---------|----------|
| Shell write | `echo -n "hi" > /dev/mychardev; cat /dev/mychardev` | `hi` |
| Overwrite semantics | `echo -n "hello" > /dev/mychardev; echo -n "yo" > /dev/mychardev; cat /dev/mychardev` | `yollo` (writes start at offset 0 and overwrite; use CLEAR_BUFFER first) |
| Custom size | `sudo insmod mychardev.ko buffer_size=16` | capacity 16; test #9 truncates at 16 |
| Bad size | `sudo insmod mychardev.ko buffer_size=0` | insmod fails with `Invalid argument` |
| Clean unload | `sudo rmmod mychardev` | `/dev/mychardev` disappears, no oops in `dmesg` |
| Race check (optional) | temporarily comment out the mutex calls in `mychardev.c`, rebuild, run test #19 | test fails, proving the test detects races |
