/*
 * datagen.c
 *
 * Generates input.txt with L positive integers and H hidden negative keys
 * randomly distributed.
 *
 *   ./datagen 100000 50
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char *argv[]) {
    int L = 12000;
    int H = 150;
    if (argc >= 2) L = atoi(argv[1]);
    if (argc >= 3) H = atoi(argv[2]);
    if (L < 12000)        { printf("L must be at least 12000\n"); return 1; }
    if (H < 1 || H > L)   { printf("H must be between 1 and L\n"); return 1; }

    int *arr = (int *)malloc((size_t)L * sizeof(int));
    if (!arr) { perror("malloc"); return 1; }

    srand((unsigned int)time(NULL));
    for (int i = 0; i < L; i++) arr[i] = (rand() % 1000) + 1;

    int placed = 0;
    while (placed < H) {
        int p = rand() % L;
        if (arr[p] > 0) {
            arr[p] = -((rand() % 100) + 1);
            placed++;
        }
    }

    FILE *fp = fopen("input.txt", "w");
    if (!fp) { perror("fopen"); free(arr); return 1; }
    for (int i = 0; i < L; i++) fprintf(fp, "%d\n", arr[i]);
    fclose(fp);
    free(arr);

    printf("input.txt created with L=%d and H=%d hidden keys\n", L, H);
    return 0;
}
