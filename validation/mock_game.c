// mock_game.c — a synthetic target process with a "client.dll" mapping and a known memory layout.
//
// It exists to test the model-written reader against a live process rather than against a fixture:
// the reader resolves the pid by name, resolves the module base from /proc/<pid>/maps, follows the
// pointer chain (view-matrix pointer → matrix, entity-list pointer → entity pointers), decodes each
// entity and prints JSON. This program writes the same values it publishes to `expected.json`, so the
// comparison is between what the target *actually holds in memory* and what the reader *decoded*.
//
// Layout (base of the client.dll mapping):
//   base + 0x1000 : u64 → pointer to the 4×4 view matrix
//   base + 0x1100 : 16 × f32 view matrix
//   base + 0x2000 : u64 → pointer to an array of 64 entity pointers
//   base + 0x3000 : 64 × u64 entity pointers (entities 8..63 are null → the reader must survive)
//   base + 0x4000 + i*0x100 : entity record
//       +0x00 x, +0x04 y, +0x08 z, +0x0C health, +0x10 team, +0x14 dormant(u8),
//       +0x18 yaw, +0x1C pitch, +0x20 name[32]
//
// Build:  cc -O2 -o game.exe validation/mock_game.c
// Run:    ./game.exe          (writes expected.json, then sleeps; SIGTERM to stop)
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#define MAP_LEN   0x8000
#define OFF_VM_PTR  0x1000
#define OFF_VM      0x1100
#define OFF_EL_PTR  0x2000
#define OFF_EL      0x3000
#define OFF_ENT     0x4000
#define ENT_STRIDE  0x100
#define N_ENTITIES  64
#define N_LIVE      8

struct Entity {
    float x, y, z;
    int32_t health, team;
    uint8_t dormant;
    float yaw, pitch;
    char name[32];
};

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);  // allow the sibling reader to read us

    const char* mod = "client.dll";
    int fd = open(mod, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, MAP_LEN) != 0) { perror("client.dll"); return 1; }

    uint8_t* base = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    memset(base, 0, MAP_LEN);

    // column-major perspective-ish matrix, values chosen so a transposed/misindexed read is visible
    float* vm = (float*)(base + OFF_VM);
    const float vm_src[16] = {1.5f, 0.0f, 0.0f, 0.0f,
                              0.0f, 1.9f, 0.0f, 0.0f,
                              0.0f, 0.0f, 1.01f, 1.0f,
                              0.25f, -0.75f, -0.5f, 0.0f};
    memcpy(vm, vm_src, sizeof vm_src);

    uint64_t* el = (uint64_t*)(base + OFF_EL);
    for (int i = 0; i < N_ENTITIES; ++i) {
        if (i >= N_LIVE) { el[i] = 0; continue; }
        struct Entity* e = (struct Entity*)(base + OFF_ENT + i * ENT_STRIDE);
        e->x = 100.0f * (float)(i + 1);
        e->y = -25.0f * (float)i;
        e->z = 64.0f;
        e->health = 100 - i;
        e->team = (i % 3) + 1;
        e->dormant = (i % 7 == 0) ? 1 : 0;
        e->yaw = 1.5f * (float)i;
        e->pitch = -0.5f;
        snprintf(e->name, sizeof e->name, "bot%02d", i);
        el[i] = (uint64_t)(uintptr_t)e;
    }

    *(uint64_t*)(base + OFF_VM_PTR) = (uint64_t)(uintptr_t)vm;
    *(uint64_t*)(base + OFF_EL_PTR) = (uint64_t)(uintptr_t)el;
    msync(base, MAP_LEN, MS_SYNC);

    FILE* f = fopen("expected.json", "w");
    fprintf(f, "{\"module\":\"client.dll\",\"base\":%llu,\"view_matrix\":[",
            (unsigned long long)(uintptr_t)base);
    for (int i = 0; i < 16; ++i) fprintf(f, "%s%.6g", i ? "," : "", (double)vm_src[i]);
    fprintf(f, "],\"entities\":[");
    for (int i = 0; i < N_LIVE; ++i) {
        const struct Entity* e = (const struct Entity*)(base + OFF_ENT + i * ENT_STRIDE);
        fprintf(f, "%s{\"index\":%d,\"health\":%d,\"team\":%d,\"dormant\":%s,"
                   "\"origin\":{\"x\":%.6g,\"y\":%.6g,\"z\":%.6g},\"yaw\":%.6g,\"pitch\":%.6g,"
                   "\"name\":\"%s\"}",
                i ? "," : "", i, e->health, e->team, e->dormant ? "true" : "false",
                (double)e->x, (double)e->y, (double)e->z, (double)e->yaw, (double)e->pitch, e->name);
    }
    fprintf(f, "]}\n");
    fclose(f);

    printf("pid=%d base=0x%llx ready\n", (int)getpid(), (unsigned long long)(uintptr_t)base);
    fflush(stdout);
    for (;;) pause();
}
