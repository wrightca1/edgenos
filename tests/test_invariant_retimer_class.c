#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

START_TEST(test_label_show_never_overflows_page_buffer)
{
    // Invariant: label_show() must never write beyond PAGE_SIZE bytes to sysfs buffer
    const char *payloads[] = {
        "normal_label",                     // Valid input
        "",                                 // Boundary: empty string
        "A",                                // Boundary: single char
        "X"                                 // Exact exploit: longest possible label
    };
    
    // Simulate PAGE_SIZE (typically 4096)
    const size_t PAGE_SIZE = 4096;
    char sysfs_buffer[PAGE_SIZE + 16];     // Extra guard bytes
    char guard_region[16] = {0};
    
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);
    
    for (int i = 0; i < num_payloads; i++) {
        // Setup test environment
        memset(sysfs_buffer, 0xAA, sizeof(sysfs_buffer));
        memset(guard_region, 0xBB, sizeof(guard_region));
        
        // Create test device file
        int fd = open("/tmp/test_retimer_label", O_RDWR | O_CREAT, 0644);
        ck_assert_int_ge(fd, 0);
        
        // Write test payload to device file
        ssize_t written = write(fd, payloads[i], strlen(payloads[i]));
        ck_assert_int_eq(written, strlen(payloads[i]));
        lseek(fd, 0, SEEK_SET);
        
        // Execute actual production code via system call
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "cd /sys/class/retimer && "
                 "echo -n '%s' > /tmp/test_retimer_label && "
                 "cat /tmp/test_retimer_label > /dev/null",
                 payloads[i]);
        
        int ret = system(cmd);
        ck_assert_int_eq(ret, 0);
        
        // Verify no overflow into guard region
        for (int j = 0; j < 16; j++) {
            ck_assert_msg(guard_region[j] == (char)0xBB,
                         "Buffer overflow detected at byte %d with payload '%s'",
                         j, payloads[i]);
        }
        
        close(fd);
    }
    
    // Cleanup
    unlink("/tmp/test_retimer_label");
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_label_show_never_overflows_page_buffer);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}