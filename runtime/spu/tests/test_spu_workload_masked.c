#include "../spu_workload.c"
#include <assert.h>

static void prog_a(spu_context* c) { (void)c; }
static void prog_b(spu_context* c) { (void)c; }

int main(void)
{
    enum { SIZE = 0x57D0, VAR = 0x5780, VLEN = 0x50 };
    static uint8_t img[SIZE], loaded[SIZE];
    for (unsigned i = 0; i < VAR; i++) img[i] = (uint8_t)(i * 7 + 3);
    const uint64_t fp = spu_workload_fingerprint(img, SIZE);

    spu_workload_register_img(0x1111ULL, prog_a, 2, "a");
    spu_workload_register_img_masked(fp, prog_b, 3, "b", SIZE, VAR, VLEN);

    for (int run = 0; run < 2; run++) {
        memcpy(loaded, img, SIZE);
        for (unsigned i = 0; i < VLEN; i++) loaded[VAR + i] = (uint8_t)(0x47 + run * 13 + i);
        uint64_t plain = spu_workload_fingerprint(loaded, SIZE), got = 0;
        int id = -1;
        assert(plain != fp);
        assert(spu_workload_find_img(plain, &id) == NULL);
        assert(spu_workload_find_img_masked(loaded, SIZE, &got, &id) == prog_b);
        assert(got == fp && id == 3);
    }

    memcpy(loaded, img, SIZE);
    loaded[0x100] ^= 1;
    int id = -1;
    assert(spu_workload_find_img_masked(loaded, SIZE, NULL, &id) == NULL && id == 0);
    assert(spu_workload_find_img_masked(img, SIZE - 0x10, NULL, &id) == NULL);
    assert(spu_workload_find_img(fp, &id) == prog_b && id == 3);
    spu_workload_register_img_masked(0x2222ULL, prog_a, 4, "bad", 0x100, 0xF0, 0x20);
    assert(spu_workload_find(0x2222ULL) == NULL);

    printf("test_spu_workload_masked: ok\n");
    return 0;
}
