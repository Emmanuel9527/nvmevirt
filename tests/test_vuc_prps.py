#!/usr/bin/env python3
"""Compile the actual VUC PRP copier with mock physical pages; no kernel load."""
from pathlib import Path
import os
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'io.c').read_text()
start = source.index('static int nvmev_copy_vuc_prps(')
end = source.index('\nstatic inline unsigned int __get_io_worker', start)
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
typedef uint64_t u64;
typedef uint64_t __le64;
typedef uint8_t u8;
#define PAGE_SIZE 4096u
#define PAGE_OFFSET_MASK (PAGE_SIZE - 1)
#define NVMEV_MQSIM_VUC_SLOT_BYTES 65536u
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define le64_to_cpu(x) (x)
struct nvme_common_command { u64 prp1, prp2; };
static _Alignas(4096) u8 physical[32][PAGE_SIZE];
static void *nvmev_map_prp_page(u64 addr, bool *remapped) {
    *remapped = false;
    return addr / PAGE_SIZE < 32 ? physical[addr / PAGE_SIZE] : NULL;
}
static void memunmap(void *p) { (void)p; }
static void kunmap_atomic(void *p) { (void)p; }
'''
tests = r'''
static void roundtrip(size_t bytes, size_t offset, size_t transfer, size_t list_offset) {
    struct nvme_common_command cmd = { PAGE_SIZE + offset, 0 };
    u8 input[65536], output[65536];
    size_t first = min_t(size_t, transfer, PAGE_SIZE - offset);
    size_t remaining = transfer - first;
    memset(physical, 0x5a, sizeof(physical));
    for (size_t i = 0; i < bytes; i++) input[i] = (u8)(i * 13 + 7);
    if (remaining <= PAGE_SIZE && remaining) cmd.prp2 = 2 * PAGE_SIZE;
    else if (remaining) {
        /* A valid, nonzero first-list offset. */
        cmd.prp2 = 20 * PAGE_SIZE + list_offset;
        for (size_t i = 0; i < DIV_ROUND_UP(remaining, PAGE_SIZE); i++) {
            u64 address = (i + 2) * PAGE_SIZE;
            memcpy(physical[20] + list_offset + i * 8, &address, 8);
        }
    }
    u8 list_before[PAGE_SIZE];
    memcpy(list_before, physical[20], PAGE_SIZE);
    assert(nvmev_copy_vuc_prps(&cmd, input, bytes, transfer, true) == 0);
    assert(nvmev_copy_vuc_prps(&cmd, output, bytes, transfer, false) == 0);
    assert(memcmp(input, output, bytes) == 0);
    assert(memcmp(physical[1] + offset, input, bytes) == 0);
    if (offset) assert(physical[1][offset - 1] == 0x5a);
    assert(memcmp(list_before, physical[20], PAGE_SIZE) == 0);
    size_t end = PAGE_SIZE + offset + bytes;
    assert(((u8 *)physical)[end] == 0x5a);
}
int main(void) {
    const size_t lengths[] = {1, 4096, 4097, 12288, 65536};
    const size_t offsets[] = {0, 4, 4092};
    u8 buffer[65536];
    for (size_t i = 0; i < ARRAY_SIZE(lengths); i++)
        for (size_t j = 0; j < ARRAY_SIZE(offsets); j++)
            roundtrip(lengths[i], offsets[j], lengths[i], 8);
    const size_t short_lengths[] = {1, 4096, 4097, 4288, 8192, 12288};
    for (size_t i = 0; i < ARRAY_SIZE(short_lengths); i++)
        for (size_t j = 0; j < ARRAY_SIZE(offsets); j++)
            for (size_t list_offset = 0; list_offset <= 8; list_offset += 8)
                roundtrip(short_lengths[i], offsets[j], 65536, list_offset);
    struct nvme_common_command cmd = {PAGE_SIZE, 2 * PAGE_SIZE + 8};
    assert(nvmev_copy_vuc_prps(&cmd, buffer, 8192, 8192, false) == -EINVAL);
    cmd.prp2 = 20 * PAGE_SIZE + PAGE_SIZE - 8;
    assert(nvmev_copy_vuc_prps(&cmd, buffer, 12288, 12288, false) == -EINVAL);
    cmd.prp2 = 20 * PAGE_SIZE + 1;
    assert(nvmev_copy_vuc_prps(&cmd, buffer, 12288, 12288, false) == -EINVAL);
    cmd.prp2 = 20 * PAGE_SIZE;
    memset(physical[20], 0, PAGE_SIZE);
    assert(nvmev_copy_vuc_prps(&cmd, buffer, 12288, 12288, false) == -EINVAL);
    ((u64 *)physical[20])[0] = 2 * PAGE_SIZE + 4;
    assert(nvmev_copy_vuc_prps(&cmd, buffer, 12288, 12288, false) == -EINVAL);
    cmd.prp1 = 40 * PAGE_SIZE;
    assert(nvmev_copy_vuc_prps(&cmd, buffer, 4096, 4096, false) == -EFAULT);
    cmd.prp1 = 0;
    assert(nvmev_copy_vuc_prps(&cmd, buffer, 4096, 4096, false) == -EINVAL);
    assert(nvmev_copy_vuc_prps(&cmd, buffer, 0, 0, false) == -EINVAL);
    assert(nvmev_copy_vuc_prps(&cmd, buffer, 65537, 65537, false) == -EINVAL);
    cmd.prp1 = PAGE_SIZE;
    assert(nvmev_copy_vuc_prps(&cmd, buffer, 8192, 4096, true) == -EINVAL);
    puts("PASS: 15 full + 36 short transfers, both directions, PRP-list/data guards, 10 invalid cases");
}
'''
with tempfile.TemporaryDirectory(prefix='nvmev-prp-test-') as directory:
    cfile = Path(directory) / 'prp_test.c'
    binary = Path(directory) / 'prp_test'
    cfile.write_text(preamble + source[start:end] + tests)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', str(cfile), '-o', str(binary)], check=True)
    # No heap allocation is under test. Disable LSan, which cannot run under ptrace.
    env = dict(os.environ)
    env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
    subprocess.run([str(binary)], check=True, env=env)
